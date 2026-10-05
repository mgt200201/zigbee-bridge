/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file matter_ota.cpp
 *
 * @brief 网关自身 Matter OTA：A/B 分区、扇区续传、启动确认后才标有效
 */

#include "matter_ota.h"
#include "rcp_coprocessor.h"

#include <esp_app_format.h>
#include <esp_flash_encrypt.h>
#include <esp_image_format.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <inttypes.h>
#include <nvs.h>
#include <string.h>

#include <app/clusters/ota-requestor/OTADownloader.h>
#include <app/clusters/ota-requestor/OTARequestorInterface.h>
#include <esp_matter_ota.h>
#include <lib/core/OTAImageHeader.h>
#include <lib/support/CHIPMem.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ConfigurationManager.h>
#include <platform/DeviceInstanceInfoProvider.h>

#include <system/SystemLayer.h>

static const char *TAG = "matter_ota";

static constexpr uint32_t kCheckpointMagic   = 0x4D4F5441; /* 'MOTA' */
static constexpr uint8_t kCheckpointVersion  = 3;
static constexpr size_t kDigestMax           = 64;
static constexpr uint32_t kFlashSector       = 4096;
static constexpr uint32_t kCheckpointStride  = 64 * 1024;
static constexpr const char *kNvsNamespace   = "mot_ota";
static constexpr const char *kNvsKey         = "ckpt";
static constexpr const char *kVerNs          = "gw_ver";
static constexpr const char *kVerCurKey      = "sw_num";
static constexpr const char *kVerPrevKey     = "sw_prev";
static constexpr const char *kVerStrKey      = "sw_str";

#ifndef CONFIG_OTA_AUTO_REBOOT_DELAY_MS
#define CONFIG_OTA_AUTO_REBOOT_DELAY_MS 5000
#endif

struct OtaCheckpoint {
    uint32_t magic;
    uint8_t version;
    uint8_t digest_len;
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t software_version;
    uint32_t payload_size;
    uint32_t flash_bytes;
    uint32_t partition_addr;
    uint32_t host_offset;
    uint32_t host_size;
    uint8_t digest[kDigestMax];
};

static void checkpoint_erase(void)
{
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_erase_key(handle, kNvsKey);
    nvs_commit(handle);
    nvs_close(handle);
}

static bool checkpoint_load(OtaCheckpoint *out)
{
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(*out);
    esp_err_t err = nvs_get_blob(handle, kNvsKey, out, &len);
    nvs_close(handle);
    if (err != ESP_OK || len != sizeof(*out) || out->magic != kCheckpointMagic ||
        out->version != kCheckpointVersion) {
        return false;
    }
    if (out->digest_len > kDigestMax) {
        return false;
    }
    if (out->flash_bytes != out->payload_size) {
        out->flash_bytes = (out->flash_bytes / kFlashSector) * kFlashSector;
    }
    return out->flash_bytes > 0 && out->flash_bytes <= out->payload_size;
}

static void checkpoint_save(const OtaCheckpoint &ckpt)
{
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed, resume point not saved");
        return;
    }
    esp_err_t err = nvs_set_blob(handle, kNvsKey, &ckpt, sizeof(ckpt));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Checkpoint save failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t erase_for_write(const esp_partition_t *part, uint32_t offset, size_t size)
{
    const uint32_t erase = part->erase_size ? part->erase_size : kFlashSector;
    const uint32_t first = offset / erase;
    const uint32_t last  = (offset + static_cast<uint32_t>(size) - 1) / erase;

    if ((offset % erase) == 0) {
        return esp_partition_erase_range(part, offset, (last - first + 1) * erase);
    }
    if (first != last) {
        return esp_partition_erase_range(part, (first + 1) * erase, (last - first) * erase);
    }
    return ESP_OK;
}

namespace {

class GatewayOtaImageProcessor : public chip::OTAImageProcessorInterface
{
public:
    void SetDownloader(chip::OTADownloader *downloader) { mDownloader = downloader; }

    CHIP_ERROR PrepareDownload() override;
    CHIP_ERROR Finalize() override;
    CHIP_ERROR Apply() override;
    CHIP_ERROR Abort() override;
    CHIP_ERROR ProcessBlock(chip::ByteSpan &block) override;
    bool IsFirstImageRun() override;
    CHIP_ERROR ConfirmCurrentImage() override;

private:
    static void HandlePrepareDownload(intptr_t context);
    static void HandleFinalize(intptr_t context);
    static void HandleApply(intptr_t context);
    static void HandleAbort(intptr_t context);
    static void HandleProcessBlock(intptr_t context);
    static void HandleRestart(chip::System::Layer *layer, void *appState);

    CHIP_ERROR SetBlock(chip::ByteSpan &block);
    void ReleaseBlock();
    CHIP_ERROR ValidateAndAcceptHeader(const chip::OTAImageHeader &header);
    esp_err_t WritePayload(const uint8_t *data, size_t size);
    void PersistProgress(uint32_t flashBytes);
    void RememberIdentity(const chip::OTAImageHeader &header, uint32_t partitionAddr);

    chip::OTADownloader *mDownloader = nullptr;
    const esp_partition_t *mPartition = nullptr;
    chip::OTAImageHeaderParser mHeaderParser;
    chip::MutableByteSpan mBlock;

    bool mOpen            = false;
    bool mHeaderDone      = false;
    bool mResumeChecked   = false;
    bool mSkipArmed       = false;
    bool mDiscardOnAbort  = false;
    bool mImageReady      = false;
    bool mCombined        = false;
    bool mHostPassthrough = false;

    uint32_t mPayloadSize      = 0;
    uint32_t mWritten          = 0;
    uint32_t mHostWritten      = 0;
    uint32_t mHostExpect       = 0;
    uint32_t mHostImageOffset  = 0;
    uint32_t mResumeOffset     = 0;
    uint32_t mStoredFlashBytes = 0;

    uint16_t mVendorId  = 0;
    uint16_t mProductId = 0;
    uint32_t mSoftwareVersion = 0;
    uint8_t mDigestLen = 0;
    uint8_t mDigest[kDigestMax] = {};
};

GatewayOtaImageProcessor gProcessor;

void GatewayOtaImageProcessor::RememberIdentity(const chip::OTAImageHeader &header, uint32_t partitionAddr)
{
    mVendorId         = header.mVendorId;
    mProductId        = header.mProductId;
    mSoftwareVersion  = header.mSoftwareVersion;
    mDigestLen        = 0;
    memset(mDigest, 0, sizeof(mDigest));
    if (header.mImageDigest.size() <= kDigestMax) {
        mDigestLen = static_cast<uint8_t>(header.mImageDigest.size());
        memcpy(mDigest, header.mImageDigest.data(), mDigestLen);
    }
    (void)partitionAddr;
}

void GatewayOtaImageProcessor::PersistProgress(uint32_t flashBytes)
{
    if (flashBytes == 0 || mPartition == nullptr || mPayloadSize == 0) {
        return;
    }
    if (flashBytes != mPayloadSize) {
        flashBytes = (flashBytes / kFlashSector) * kFlashSector;
    }
    if (flashBytes == 0 || flashBytes <= mStoredFlashBytes) {
        return;
    }

    OtaCheckpoint ckpt = {};
    ckpt.magic            = kCheckpointMagic;
    ckpt.version          = kCheckpointVersion;
    ckpt.digest_len       = mDigestLen;
    ckpt.vendor_id        = mVendorId;
    ckpt.product_id       = mProductId;
    ckpt.software_version = mSoftwareVersion;
    ckpt.payload_size     = mPayloadSize;
    ckpt.flash_bytes      = flashBytes;
    ckpt.partition_addr   = mPartition->address;
    ckpt.host_offset      = mHostImageOffset;
    ckpt.host_size        = mHostExpect;
    memcpy(ckpt.digest, mDigest, mDigestLen);
    checkpoint_save(ckpt);
    mStoredFlashBytes = flashBytes;
    ESP_LOGI(TAG, "Resume point saved at %" PRIu32 " / %" PRIu32, flashBytes, mPayloadSize);
}

CHIP_ERROR GatewayOtaImageProcessor::ValidateAndAcceptHeader(const chip::OTAImageHeader &header)
{
    uint16_t vendor = 0;
    uint16_t product = 0;
    uint32_t current = 0;
    CHIP_ERROR err = chip::DeviceLayer::GetDeviceInstanceInfoProvider()->GetVendorId(vendor);
    if (err != CHIP_NO_ERROR) {
        return err;
    }
    err = chip::DeviceLayer::GetDeviceInstanceInfoProvider()->GetProductId(product);
    if (err != CHIP_NO_ERROR) {
        return err;
    }
    err = chip::DeviceLayer::ConfigurationMgr().GetSoftwareVersion(current);
    if (err != CHIP_NO_ERROR) {
        return err;
    }

    if (header.mVendorId != vendor || header.mProductId != product) {
        ESP_LOGE(TAG, "Image identity %u/%u does not match device %u/%u",
                 header.mVendorId, header.mProductId, vendor, product);
        return CHIP_ERROR_INVALID_FILE_IDENTIFIER;
    }
    if (header.mSoftwareVersion <= current) {
        ESP_LOGE(TAG, "Image version %" PRIu32 " is not newer than %" PRIu32,
                 header.mSoftwareVersion, current);
        return CHIP_ERROR_INVALID_FILE_IDENTIFIER;
    }
    if (header.mMinApplicableVersion.HasValue() && current < header.mMinApplicableVersion.Value()) {
        return CHIP_ERROR_INVALID_FILE_IDENTIFIER;
    }
    if (header.mMaxApplicableVersion.HasValue() && current > header.mMaxApplicableVersion.Value()) {
        return CHIP_ERROR_INVALID_FILE_IDENTIFIER;
    }
    if (header.mPayloadSize == 0) {
        return CHIP_ERROR_INVALID_FILE_IDENTIFIER;
    }
    if (esp_flash_encryption_enabled()) {
        ESP_LOGE(TAG, "Flash encryption is on; this OTA writer does not encrypt partial writes");
        return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
    }
    return CHIP_NO_ERROR;
}

esp_err_t GatewayOtaImageProcessor::WritePayload(const uint8_t *data, size_t size)
{
    if (size == 0) {
        return ESP_OK;
    }
    if (mPartition == nullptr || mHostWritten > mPartition->size ||
        size > mPartition->size - mHostWritten) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (mHostWritten == 0 && data[0] != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "ESP image magic mismatch (expected 0xE9, saw 0x%02x)", data[0]);
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }

    esp_err_t err = erase_for_write(mPartition, mHostWritten, size);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_partition_write(mPartition, mHostWritten, data, size);
    if (err != ESP_OK) {
        return err;
    }
    mHostWritten += static_cast<uint32_t>(size);
    mSkipArmed = false;

    uint32_t persist_at = mCombined ? (mHostImageOffset + mHostWritten) : mHostWritten;
    const uint32_t host_total = mHostExpect ? mHostExpect
                                            : (mCombined ? rcp_coprocessor_host_size() : mPayloadSize);
    if ((host_total > 0 && mHostWritten == host_total) ||
        (persist_at / kCheckpointStride) > (mStoredFlashBytes / kCheckpointStride)) {
        PersistProgress(persist_at);
    }
    return ESP_OK;
}

CHIP_ERROR GatewayOtaImageProcessor::PrepareDownload()
{
    if (mDownloader == nullptr) {
        ESP_LOGE(TAG, "BDX downloader is not bound");
        return CHIP_ERROR_INCORRECT_STATE;
    }
    chip::DeviceLayer::PlatformMgr().ScheduleWork(HandlePrepareDownload, reinterpret_cast<intptr_t>(this));
    return CHIP_NO_ERROR;
}

CHIP_ERROR GatewayOtaImageProcessor::Finalize()
{
    chip::DeviceLayer::PlatformMgr().ScheduleWork(HandleFinalize, reinterpret_cast<intptr_t>(this));
    return CHIP_NO_ERROR;
}

CHIP_ERROR GatewayOtaImageProcessor::Apply()
{
    chip::DeviceLayer::PlatformMgr().ScheduleWork(HandleApply, reinterpret_cast<intptr_t>(this));
    return CHIP_NO_ERROR;
}

CHIP_ERROR GatewayOtaImageProcessor::Abort()
{
    chip::DeviceLayer::PlatformMgr().ScheduleWork(HandleAbort, reinterpret_cast<intptr_t>(this));
    return CHIP_NO_ERROR;
}

CHIP_ERROR GatewayOtaImageProcessor::ProcessBlock(chip::ByteSpan &block)
{
    CHIP_ERROR err = SetBlock(block);
    if (err != CHIP_NO_ERROR) {
        return err;
    }
    chip::DeviceLayer::PlatformMgr().ScheduleWork(HandleProcessBlock, reinterpret_cast<intptr_t>(this));
    return CHIP_NO_ERROR;
}

bool GatewayOtaImageProcessor::IsFirstImageRun()
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    bool pending = running != nullptr &&
                   esp_ota_get_state_partition(running, &state) == ESP_OK &&
                   state == ESP_OTA_IMG_PENDING_VERIFY;

    chip::OTARequestorInterface *requestor = chip::GetRequestorInstance();
    bool applying = requestor != nullptr &&
                    requestor->GetCurrentUpdateState() == chip::OTARequestorInterface::OTAUpdateStateEnum::kApplying;
    return pending || applying;
}

CHIP_ERROR GatewayOtaImageProcessor::ConfirmCurrentImage()
{
    chip::OTARequestorInterface *requestor = chip::GetRequestorInstance();
    uint32_t current = 0;
    if (chip::DeviceLayer::ConfigurationMgr().GetSoftwareVersion(current) != CHIP_NO_ERROR) {
        return CHIP_ERROR_INTERNAL;
    }

    if (requestor != nullptr &&
        requestor->GetCurrentUpdateState() == chip::OTARequestorInterface::OTAUpdateStateEnum::kApplying &&
        current != requestor->GetTargetVersion()) {
        ESP_LOGE(TAG, "Running version %" PRIu32 " != target %" PRIu32 ", rolling back",
                 current, requestor->GetTargetVersion());
        esp_ota_mark_app_invalid_rollback_and_reboot();
        return CHIP_ERROR_INCORRECT_STATE;
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mark image valid: %s", esp_err_to_name(err));
        return CHIP_ERROR_INTERNAL;
    }
    ESP_LOGI(TAG, "New image confirmed (version %" PRIu32 ")", current);
    return CHIP_NO_ERROR;
}

void GatewayOtaImageProcessor::HandlePrepareDownload(intptr_t context)
{
    auto *self = reinterpret_cast<GatewayOtaImageProcessor *>(context);
    self->mOpen           = false;
    self->mHeaderDone     = false;
    self->mResumeChecked  = false;
    self->mSkipArmed      = false;
    self->mDiscardOnAbort = false;
    self->mImageReady     = false;
    self->mCombined        = false;
    self->mHostPassthrough = false;
    self->mPayloadSize    = 0;
    self->mWritten        = 0;
    self->mHostWritten    = 0;
    self->mHostExpect     = 0;
    self->mHostImageOffset = 0;
    self->mResumeOffset   = 0;
    self->mStoredFlashBytes = 0;
    self->mParams.downloadedBytes = 0;
    self->mParams.totalFileBytes  = 0;
    self->ReleaseBlock();
    self->mHeaderParser.Init();
    rcp_coprocessor_reset_session();

    self->mPartition = esp_ota_get_next_update_partition(nullptr);
    if (self->mPartition == nullptr || self->mDownloader == nullptr) {
        ESP_LOGE(TAG, "No inactive OTA slot");
        if (self->mDownloader != nullptr) {
            self->mDownloader->OnPreparedForDownload(CHIP_ERROR_INTERNAL);
        }
        return;
    }

    ESP_LOGI(TAG, "OTA slot %s @ 0x%" PRIx32 " (%" PRIu32 " bytes)",
             self->mPartition->label, self->mPartition->address, self->mPartition->size);
    self->mOpen = true;
    self->mDownloader->OnPreparedForDownload(CHIP_NO_ERROR);
}

void GatewayOtaImageProcessor::HandleFinalize(intptr_t context)
{
    auto *self = reinterpret_cast<GatewayOtaImageProcessor *>(context);
    self->ReleaseBlock();
    if (!self->mOpen) {
        return;
    }
    if (self->mPartition == nullptr) {
        return;
    }
    uint32_t expect = self->mHostExpect;
    if (expect == 0) {
        expect = self->mCombined ? rcp_coprocessor_host_size() : self->mPayloadSize;
    }
    if (expect == 0 || self->mHostWritten != expect) {
        ESP_LOGE(TAG, "Incomplete host image (%" PRIu32 " / %" PRIu32 ")", self->mHostWritten, expect);
        checkpoint_erase();
        self->mStoredFlashBytes = 0;
        self->mImageReady = false;
        return;
    }

    esp_image_metadata_t metadata;
    const esp_partition_pos_t pos = {
        .offset = self->mPartition->address,
        .size   = self->mPartition->size,
    };
    esp_err_t err = esp_image_verify(ESP_IMAGE_VERIFY, &pos, &metadata);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Image verify failed: %s", esp_err_to_name(err));
        self->mDiscardOnAbort = true;
        checkpoint_erase();
        self->mStoredFlashBytes = 0;
        self->mImageReady = false;
        return;
    }

    self->PersistProgress(self->mCombined ? (self->mHostImageOffset + self->mHostWritten)
                                          : self->mHostWritten);
    self->mImageReady = true;
    ESP_LOGI(TAG, "Image verified, %" PRIu32 " bytes in %s%s", self->mHostWritten, self->mPartition->label,
             self->mCombined ? " (S3; H2 RCP already flashed)" : "");
}

void GatewayOtaImageProcessor::HandleApply(intptr_t context)
{
    auto *self = reinterpret_cast<GatewayOtaImageProcessor *>(context);
    if (!self->mImageReady || self->mPartition == nullptr) {
        ESP_LOGE(TAG, "Apply requested before a verified image was stored");
        return;
    }

    esp_err_t err = esp_ota_set_boot_partition(self->mPartition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return;
    }
    checkpoint_erase();
    self->mStoredFlashBytes = 0;
    ESP_LOGI(TAG, "Boot slot set to %s, restarting in %d ms",
             self->mPartition->label, CONFIG_OTA_AUTO_REBOOT_DELAY_MS);
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(CONFIG_OTA_AUTO_REBOOT_DELAY_MS), HandleRestart, nullptr);
}

void GatewayOtaImageProcessor::HandleAbort(intptr_t context)
{
    auto *self = reinterpret_cast<GatewayOtaImageProcessor *>(context);
    const bool discard = self->mDiscardOnAbort || self->mSkipArmed;
    const bool reboot_after = rcp_coprocessor_needs_reboot_after_abort();
    const uint32_t persist_at = self->mCombined ? (self->mHostImageOffset + self->mHostWritten)
                                                : self->mHostWritten;
    self->mOpen = false;
    self->mHeaderParser.Clear();
    self->ReleaseBlock();
    rcp_coprocessor_abort();

    if (discard) {
        if (self->mSkipArmed) {
            ESP_LOGW(TAG, "Provider rejected resume, next attempt downloads from the start");
        }
        checkpoint_erase();
        self->mStoredFlashBytes = 0;
    } else {
        self->PersistProgress(persist_at);
    }
    self->mSkipArmed = false;
    self->mDiscardOnAbort = false;
    ESP_LOGW(TAG, "OTA download aborted");
    if (reboot_after) {
        ESP_LOGW(TAG, "Zigbee UART was taken for H2 flash; rebooting to restore RCP");
        chip::DeviceLayer::SystemLayer().StartTimer(
            chip::System::Clock::Milliseconds32(CONFIG_OTA_AUTO_REBOOT_DELAY_MS), HandleRestart, nullptr);
    }
}

void GatewayOtaImageProcessor::HandleProcessBlock(intptr_t context)
{
    auto *self = reinterpret_cast<GatewayOtaImageProcessor *>(context);
    if (!self->mOpen || self->mDownloader == nullptr) {
        return;
    }

    chip::ByteSpan block(self->mBlock.data(), self->mBlock.size());
    if (!self->mHeaderDone) {
        chip::OTAImageHeader header;
        CHIP_ERROR err = self->mHeaderParser.AccumulateAndDecode(block, header);
        if (err == CHIP_ERROR_BUFFER_TOO_SMALL) {
            self->mDownloader->FetchNextData();
            return;
        }
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "OTA header rejected: %" CHIP_ERROR_FORMAT, err.Format());
            self->mDiscardOnAbort = true;
            self->mDownloader->EndDownload(err);
            return;
        }

        err = self->ValidateAndAcceptHeader(header);
        if (err != CHIP_NO_ERROR) {
            self->mDiscardOnAbort = true;
            self->mDownloader->EndDownload(err);
            return;
        }

        self->mPayloadSize = static_cast<uint32_t>(header.mPayloadSize);
        self->mHostExpect = self->mPayloadSize;
        self->mParams.totalFileBytes = header.mPayloadSize;
        self->RememberIdentity(header, self->mPartition->address);
        self->mHeaderParser.Clear();
        self->mHeaderDone = true;

        OtaCheckpoint saved;
        if (checkpoint_load(&saved) && saved.partition_addr == self->mPartition->address &&
            saved.vendor_id == header.mVendorId && saved.product_id == header.mProductId &&
            saved.software_version == header.mSoftwareVersion &&
            saved.payload_size == self->mPayloadSize && saved.digest_len == self->mDigestLen &&
            memcmp(saved.digest, self->mDigest, self->mDigestLen) == 0) {
            uint8_t magic = 0;
            if (saved.host_offset > 0 && saved.flash_bytes < saved.host_offset) {
                ESP_LOGW(TAG, "RCP flash did not finish, ignoring resume at %" PRIu32, saved.flash_bytes);
                checkpoint_erase();
            } else if (esp_partition_read(self->mPartition, 0, &magic, 1) == ESP_OK &&
                       magic == ESP_IMAGE_HEADER_MAGIC) {
                self->mResumeOffset = saved.flash_bytes;
                self->mStoredFlashBytes = saved.flash_bytes;
                if (saved.host_offset > 0 && saved.flash_bytes >= saved.host_offset) {
                    self->mCombined = true;
                    self->mHostPassthrough = true;
                    self->mHostImageOffset = saved.host_offset;
                    self->mHostExpect = saved.host_size ? saved.host_size
                                                        : (saved.payload_size - saved.host_offset);
                    self->mHostWritten = saved.flash_bytes - saved.host_offset;
                }
                ESP_LOGI(TAG, "Resuming at %" PRIu32 " / %" PRIu32 " bytes", self->mResumeOffset, self->mPayloadSize);
            } else {
                ESP_LOGW(TAG, "Checkpoint flash is not an ESP image, starting over");
                checkpoint_erase();
            }
        } else {
            checkpoint_erase();
        }
        self->mResumeChecked = true;
        ESP_LOGI(TAG, "Image version %" PRIu32 ", payload %" PRIu32 " bytes", self->mSoftwareVersion, self->mPayloadSize);
    }

    if (!self->mResumeChecked) {
        self->mDownloader->EndDownload(CHIP_ERROR_INCORRECT_STATE);
        return;
    }

    /* 续传点在 payload 空间。本块里落在续传点之前的字节已经在 flash 上，
     * 用 BDX SkipData 让 Provider 从续传点之后继续发，而不是逐块重下。 */
    const uint32_t payloadHere = static_cast<uint32_t>(block.size());
    if (self->mResumeOffset > self->mWritten) {
        const uint32_t have = self->mWritten;
        if (have + payloadHere <= self->mResumeOffset) {
            const uint32_t skip = self->mResumeOffset - (have + payloadHere);
            self->mWritten = self->mResumeOffset;
            self->mParams.downloadedBytes = self->mWritten;
            self->mResumeOffset = self->mWritten;
            if (skip > 0) {
                self->mSkipArmed = true;
                CHIP_ERROR skipErr = self->mDownloader->SkipData(skip);
                if (skipErr != CHIP_NO_ERROR) {
                    ESP_LOGW(TAG, "SkipData failed, next attempt starts from byte 0");
                    self->mDiscardOnAbort = true;
                    self->mDownloader->EndDownload(skipErr);
                }
                return;
            }
            self->mDownloader->FetchNextData();
            return;
        }

        const uint32_t drop = self->mResumeOffset - have;
        block = block.SubSpan(drop);
        self->mWritten = self->mResumeOffset;
        self->mParams.downloadedBytes = self->mWritten;
        self->mResumeOffset = self->mWritten;
    }

    const uint32_t consume = static_cast<uint32_t>(block.size());
    esp_err_t werr = ESP_OK;
    if (self->mHostPassthrough) {
        werr = self->WritePayload(block.data(), block.size());
    } else {
        if (!self->mCombined && consume >= 4 &&
            rcp_coprocessor_payload_is_combined(block.data(), block.size())) {
            self->mCombined = true;
            ESP_LOGI(TAG, "Matter payload is combined S3 + H2 RCP image");
        }
        if (self->mCombined) {
            const uint8_t *host = nullptr;
            size_t host_len = 0;
            werr = rcp_coprocessor_feed(block.data(), block.size(), &host, &host_len);
            if (werr == ESP_OK) {
                if (rcp_coprocessor_host_offset() != 0) {
                    self->mHostImageOffset = rcp_coprocessor_host_offset();
                }
                if (rcp_coprocessor_host_size() != 0) {
                    self->mHostExpect = rcp_coprocessor_host_size();
                }
            }
            if (werr == ESP_OK && host_len > 0) {
                if (rcp_coprocessor_host_size() > self->mPartition->size) {
                    ESP_LOGE(TAG, "S3 image %" PRIu32 " does not fit OTA slot", rcp_coprocessor_host_size());
                    werr = ESP_ERR_INVALID_SIZE;
                } else {
                    werr = self->WritePayload(host, host_len);
                }
            }
        } else {
            if (self->mPayloadSize > self->mPartition->size) {
                ESP_LOGE(TAG, "Host-only payload does not fit OTA slot");
                werr = ESP_ERR_INVALID_SIZE;
            } else {
                werr = self->WritePayload(block.data(), block.size());
            }
        }
    }
    if (werr != ESP_OK) {
        ESP_LOGE(TAG, "Flash write failed: %s", esp_err_to_name(werr));
        /* Combined RCP flash is not resumable; a mid-RCP checkpoint skips the H2 image. */
        self->mDiscardOnAbort = self->mCombined || (werr == ESP_ERR_OTA_VALIDATE_FAILED);
        self->mDownloader->EndDownload(CHIP_ERROR_WRITE_FAILED);
        return;
    }

    self->mWritten += consume;
    self->mParams.downloadedBytes = self->mWritten;

    if ((self->mWritten % kCheckpointStride) < 1024 || self->mWritten == self->mPayloadSize) {
        ESP_LOGI(TAG, "Downloaded %" PRIu32 " / %" PRIu32, self->mWritten, self->mPayloadSize);
    }

    if (self->mWritten < self->mPayloadSize) {
        CHIP_ERROR ferr = self->mDownloader->FetchNextData();
        if (ferr != CHIP_NO_ERROR) {
            self->mDownloader->EndDownload(ferr);
        }
    }
}

void GatewayOtaImageProcessor::HandleRestart(chip::System::Layer *layer, void *appState)
{
    (void)layer;
    (void)appState;
    esp_restart();
}

CHIP_ERROR GatewayOtaImageProcessor::SetBlock(chip::ByteSpan &block)
{
    if (block.empty()) {
        ReleaseBlock();
        return CHIP_NO_ERROR;
    }
    if (mBlock.size() < block.size()) {
        ReleaseBlock();
        uint8_t *ptr = static_cast<uint8_t *>(chip::Platform::MemoryAlloc(block.size()));
        if (ptr == nullptr) {
            return CHIP_ERROR_NO_MEMORY;
        }
        mBlock = chip::MutableByteSpan(ptr, block.size());
    }
    return chip::CopySpanToMutableSpan(block, mBlock);
}

void GatewayOtaImageProcessor::ReleaseBlock()
{
    if (mBlock.data() != nullptr) {
        chip::Platform::MemoryFree(mBlock.data());
    }
    mBlock = chip::MutableByteSpan();
}

void BindDownloader(chip::OTADownloader *downloader)
{
    gProcessor.SetDownloader(downloader);
}

esp_matter_ota_requestor_impl_t gImpl;

} // namespace

extern "C" esp_err_t matter_ota_init(void)
{
    gImpl.driver          = nullptr;
    gImpl.user_consent    = nullptr;
    gImpl.image_processor = &gProcessor;

    esp_matter_ota_set_downloader_bind_cb(BindDownloader);

    esp_matter_ota_config_t config = {};
    config.periodic_query_timeout = 24 * 60 * 60;
    config.watchdog_timeout       = 6 * 60 * 60;
    config.impl                   = &gImpl;

    esp_err_t err = esp_matter_ota_requestor_set_config(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA requestor config failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Matter OTA requestor ready (A/B slots, resume + rollback)");
    return ESP_OK;
}

extern "C" void matter_ota_persist_boot_version(void)
{
    uint32_t current = 0;
    if (chip::DeviceLayer::ConfigurationMgr().GetSoftwareVersion(current) != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Cannot read running software version");
        return;
    }

    char ver_str[64] = {0};
    if (chip::DeviceLayer::ConfigurationMgr().GetSoftwareVersionString(ver_str, sizeof(ver_str)) != CHIP_NO_ERROR) {
        snprintf(ver_str, sizeof(ver_str), "%" PRIu32, current);
    }

    nvs_handle_t handle;
    if (nvs_open(kVerNs, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "NVS %s open failed, version not persisted", kVerNs);
        return;
    }

    uint32_t last = 0;
    const bool had_last = nvs_get_u32(handle, kVerCurKey, &last) == ESP_OK;

    if (!had_last) {
        ESP_LOGI(TAG, "Software version %" PRIu32 " (%s), first NVS record", current, ver_str);
    } else if (current > last) {
        ESP_LOGI(TAG, "Software version %" PRIu32 " (%s), upgraded from %" PRIu32, current, ver_str, last);
    } else if (current < last) {
        ESP_LOGW(TAG, "Software version %" PRIu32 " (%s), rolled back from %" PRIu32, current, ver_str, last);
    } else {
        ESP_LOGI(TAG, "Software version %" PRIu32 " (%s), unchanged since last boot", current, ver_str);
    }

    nvs_set_u32(handle, kVerPrevKey, had_last ? last : current);
    nvs_set_u32(handle, kVerCurKey, current);
    nvs_set_str(handle, kVerStrKey, ver_str);
    nvs_commit(handle);
    nvs_close(handle);
}
