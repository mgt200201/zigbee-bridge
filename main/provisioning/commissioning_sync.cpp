/*
   SPDX-License-Identifier: Apache-2.0

   配网凭据同步实现。
*/

#include "commissioning_sync.h"

#include <inttypes.h>
#include <string.h>

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_random.h>

#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/Base64.h>
#include <platform/ESP32/ESP32Config.h>

#include "commissioning_mgr.h"

static const char *TAG = "commissioning";

void commissioning_sync_to_stack(void)
{
    uint32_t pin = commissioning_mgr_get_pin();
    uint16_t disc = commissioning_mgr_get_discriminator();
    CHIP_ERROR err;

    err = chip::DeviceLayer::Internal::ESP32Config::WriteConfigValue(
        chip::DeviceLayer::Internal::ESP32Config::kConfigKey_SetupPinCode, pin);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to write PIN: %" CHIP_ERROR_FORMAT, err.Format());
    }
    err = chip::DeviceLayer::Internal::ESP32Config::WriteConfigValue(
        chip::DeviceLayer::Internal::ESP32Config::kConfigKey_SetupDiscriminator, static_cast<uint32_t>(disc));
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to write discriminator: %" CHIP_ERROR_FORMAT, err.Format());
    }

    /* ---- 读取迭代计数（不存在则用默认值） ---- */
    uint32_t iterCount = CHIP_DEVICE_CONFIG_USE_TEST_SPAKE2P_ITERATION_COUNT;
    chip::DeviceLayer::Internal::ESP32Config::ReadConfigValue(
        chip::DeviceLayer::Internal::ESP32Config::kConfigKey_Spake2pIterationCount, iterCount);
    chip::DeviceLayer::Internal::ESP32Config::WriteConfigValue(
        chip::DeviceLayer::Internal::ESP32Config::kConfigKey_Spake2pIterationCount, iterCount);

    /* ---- 读取 salt（不存在则生成随机 salt） ---- */
    constexpr size_t kMaxSaltLen = chip::Crypto::kSpake2p_Max_PBKDF_Salt_Length;
    constexpr size_t kSaltB64Len = BASE64_ENCODED_LEN(kMaxSaltLen) + 1;
    static uint8_t salt[kMaxSaltLen];
    static char saltB64[kSaltB64Len];
    size_t saltLen = kMaxSaltLen;
    size_t saltB64ReadLen = 0;

    err = chip::DeviceLayer::Internal::ESP32Config::ReadConfigValueStr(
        chip::DeviceLayer::Internal::ESP32Config::kConfigKey_Spake2pSalt, saltB64, sizeof(saltB64), saltB64ReadLen);
    if (err == CHIP_NO_ERROR && saltB64ReadLen > 0) {
        saltLen = chip::Base64Decode32(saltB64, static_cast<uint32_t>(saltB64ReadLen), salt);
        ESP_LOGI(TAG, "Using existing spake2p salt (%u bytes)", (unsigned)saltLen);
    } else {
        esp_fill_random(salt, kMaxSaltLen);
        saltLen = kMaxSaltLen;
        chip::Base64Encode32(salt, static_cast<uint32_t>(saltLen), saltB64);
        chip::DeviceLayer::Internal::ESP32Config::WriteConfigValueStr(
            chip::DeviceLayer::Internal::ESP32Config::kConfigKey_Spake2pSalt, saltB64, strlen(saltB64));
        ESP_LOGI(TAG, "Generated new spake2p salt (%u bytes)", (unsigned)saltLen);
    }

    /* ---- 用新 PIN 重新生成 spake2p verifier ---- */
    static chip::Crypto::Spake2pVerifier verifier;
    err = verifier.Generate(iterCount, chip::ByteSpan(salt, saltLen), pin);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to generate verifier: %" CHIP_ERROR_FORMAT, err.Format());
    } else {
        static chip::Crypto::Spake2pVerifierSerialized serializedVerifier;
        chip::MutableByteSpan verifierSpan(serializedVerifier);
        err = verifier.Serialize(verifierSpan);
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "Failed to serialize verifier: %" CHIP_ERROR_FORMAT, err.Format());
        } else {
            static char verifierB64[BASE64_ENCODED_LEN(sizeof(serializedVerifier)) + 1];
            chip::Base64Encode32(serializedVerifier, static_cast<uint32_t>(verifierSpan.size()), verifierB64);
            chip::DeviceLayer::Internal::ESP32Config::WriteConfigValueStr(
                chip::DeviceLayer::Internal::ESP32Config::kConfigKey_Spake2pVerifier, verifierB64, strlen(verifierB64));
            ESP_LOGI(TAG, "Regenerated spake2p verifier for new PIN");
        }
    }

    ESP_LOGI(TAG, "Synced commissioning data to Matter stack: PIN=%08" PRIu32 ", disc=%u",
             pin, disc);
}
