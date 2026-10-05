/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "rcp_coprocessor.h"

#include "board_config.h"
#include "watchdog.h"
#include "zboss_manager.h"

#include <esp_check.h>
#include <esp_log.h>
#include <esp_rcp_firmware.h>
#include <inttypes.h>
#include <string.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp32_port.h"
#include "esp_loader.h"
#include "esp_loader_io.h"

#include <esp_private/esp_gpio_reserve.h>

static const char *TAG = "rcp_ota";

static constexpr uint32_t kFlashBlock = 1024;
static constexpr size_t kMaxSubfiles  = MAX_SUBFILE_INFO;

struct FlashArg {
    uint32_t tag;
    uint32_t offset;
} __attribute__((packed));

enum class Phase : uint8_t {
    Header = 0,
    Subfile,
    Host,
};

static struct {
    Phase phase;
    bool connected;
    bool rcp_done;
    bool zigbee_paused;
    uint32_t header_need;
    uint32_t header_got;
    uint8_t header[sizeof(esp_rcp_subfile_info_t) * kMaxSubfiles];
    esp_rcp_subfile_info_t files[kMaxSubfiles];
    int file_count;
    int file_index;
    uint32_t sub_got;
    uint32_t host_offset;
    uint32_t host_size;
    uint32_t flash_addr;
    bool flashing;
    uint32_t flash_remain;
    uint8_t block[kFlashBlock];
    uint32_t block_fill;
    FlashArg flash_args[8];
    int flash_arg_count;
} s;

static uint32_t flash_addr_for_tag(uint32_t tag)
{
    for (int i = 0; i < s.flash_arg_count; i++) {
        if (s.flash_args[i].tag == tag) {
            return s.flash_args[i].offset;
        }
    }
    return UINT32_MAX;
}

static bool subfile_is_flashable(uint32_t tag)
{
    return tag == FILETAG_RCP_BOOTLOADER || tag == FILETAG_RCP_PARTITION_TABLE ||
           tag == FILETAG_RCP_FIRMWARE;
}

static bool s_skip_bootloader_enter;
static bool s_h2_reset_to_app;

/* Hold times: EN capacitor + H2 strap sample after CHIP_EN rise.
 * Stock serial-flasher raises BOOT after BOOT_HOLD; that is wrong if EN
 * is still ramping. Keep GPIO9/BOOT latched low until ROM is in download. */
static constexpr int kH2BootSetupMs = 100;
static constexpr int kH2ResetLowMs = 300;
static constexpr int kH2BootAfterResetMs = 400;

static void h2_drive_boot(int level)
{
    gpio_hold_dis(RCP_BOOT_GPIO);
    gpio_set_level(RCP_BOOT_GPIO, level);
    gpio_config_t io = {};
    io.intr_type = GPIO_INTR_DISABLE;
    io.mode = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << RCP_BOOT_GPIO);
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&io));
    gpio_set_drive_capability(RCP_BOOT_GPIO, GPIO_DRIVE_CAP_3);
    gpio_set_level(RCP_BOOT_GPIO, level);
}

/* GPIO46 on ESP32-S3 is a strapping pin and often cannot source 3.3V.
 * Sink to reset H2; release by Hi-Z + pullup so the H2 EN resistor rises. */
static void h2_en_low(void)
{
    gpio_hold_dis(RCP_EN_GPIO);
    gpio_set_level(RCP_EN_GPIO, 0);
    gpio_config_t io = {};
    io.intr_type = GPIO_INTR_DISABLE;
    io.mode = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << RCP_EN_GPIO);
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&io));
    gpio_set_drive_capability(RCP_EN_GPIO, GPIO_DRIVE_CAP_3);
    gpio_set_level(RCP_EN_GPIO, 0);
}

static void h2_en_release(void)
{
    gpio_hold_dis(RCP_EN_GPIO);
    gpio_set_level(RCP_EN_GPIO, 1);
    gpio_config_t io = {};
    io.intr_type = GPIO_INTR_DISABLE;
    io.mode = GPIO_MODE_INPUT;
    io.pin_bit_mask = (1ULL << RCP_EN_GPIO);
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&io));
}

static void h2_enter_download(void)
{
    h2_drive_boot(0);
    gpio_hold_en(RCP_BOOT_GPIO);
    vTaskDelay(pdMS_TO_TICKS(kH2BootSetupMs));

    h2_en_low();
    vTaskDelay(pdMS_TO_TICKS(kH2ResetLowMs));
    h2_en_release();
    vTaskDelay(pdMS_TO_TICKS(kH2BootAfterResetMs));

    ESP_LOGI(TAG, "H2 strap: BOOT(GPIO%d)=%d EN(GPIO%d)=%d (BOOT want 0; EN pad may stay 0 on GPIO46)",
             (int)RCP_BOOT_GPIO, gpio_get_level(RCP_BOOT_GPIO),
             (int)RCP_EN_GPIO, gpio_get_level(RCP_EN_GPIO));
}

extern "C" void __wrap_loader_port_reset_target(void)
{
    if (s_skip_bootloader_enter) {
        return;
    }
    gpio_hold_dis(RCP_BOOT_GPIO);
    h2_drive_boot(s_h2_reset_to_app ? 1 : 0);
    if (!s_h2_reset_to_app) {
        gpio_hold_en(RCP_BOOT_GPIO);
    }
    h2_en_low();
    loader_port_delay_ms(kH2ResetLowMs);
    h2_en_release();
    loader_port_delay_ms(kH2BootAfterResetMs);
}

extern "C" void __wrap_loader_port_enter_bootloader(void)
{
    if (s_skip_bootloader_enter) {
        return;
    }
    s_h2_reset_to_app = false;
    h2_enter_download();
}

static void drain_h2_uart(void)
{
    uint8_t buf[128];
    int total = 0;
    bool usb_boot = false;
    bool spi_boot = false;
    bool waiting_dl = false;
    for (int i = 0; i < 16; i++) {
        int n = uart_read_bytes(UART_NUM_1, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n <= 0) {
            break;
        }
        total += n;
        buf[n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1] = 0;
        if (n >= 8) {
            for (int k = 0; k <= n - 8; k++) {
                if (memcmp(buf + k, "USB_BOOT", 8) == 0) {
                    usb_boot = true;
                }
                if (k + 18 <= n && memcmp(buf + k, "SPI_FAST_FLASH_BOOT", 18) == 0) {
                    spi_boot = true;
                }
                if (k + 21 <= n && memcmp(buf + k, "waiting for download", 21) == 0) {
                    waiting_dl = true;
                }
            }
        }
        const uint8_t *p = buf;
        int m = n;
        while (m > 0 && *p == 0) {
            p++;
            m--;
        }
        ESP_LOGW(TAG, "H2 ROM [%d]: %.*s", n, m, (const char *)p);
        ESP_LOG_BUFFER_HEXDUMP(TAG, buf, n, ESP_LOG_WARN);
    }
    uart_flush_input(UART_NUM_1);
    if (waiting_dl) {
        ESP_LOGI(TAG, "H2 ROM: waiting for download");
    } else if (total == 0) {
        ESP_LOGW(TAG, "H2 ROM: UART RX empty after EN pulse");
    } else if (usb_boot) {
        ESP_LOGW(TAG, "H2 ROM USB_BOOT (boot:0x0). Need GPIO8=1 and GPIO9=0");
    } else if (spi_boot) {
        ESP_LOGW(TAG, "H2 ROM SPI_FAST_FLASH_BOOT (GPIO9 was 1 at EN rise)");
    }
}

static void release_flasher_uart(int rx, int tx)
{
    loader_port_esp32_deinit();
    gpio_reset_pin((gpio_num_t)rx);
    gpio_reset_pin((gpio_num_t)tx);
    esp_gpio_revoke((1ULL << rx) | (1ULL << tx));
    vTaskDelay(pdMS_TO_TICKS(20));
}

static esp_loader_error_t try_h2_sync(int rx, int tx)
{
    loader_esp32_config_t cfg = {};
    cfg.baud_rate         = 115200;
    cfg.uart_port         = UART_NUM_1;
    cfg.uart_rx_pin       = rx;
    cfg.uart_tx_pin       = tx;
    cfg.reset_trigger_pin = CONFIG_PIN_TO_RCP_RESET;
    cfg.gpio0_trigger_pin = CONFIG_PIN_TO_RCP_BOOT;
    cfg.rx_buffer_size    = 2048;
    cfg.tx_buffer_size    = 2048;

    if (loader_port_esp32_init(&cfg) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "serial-flasher UART init failed RX=%d TX=%d", rx, tx);
        return ESP_LOADER_ERROR_FAIL;
    }
    /* init gpio_reset_pin() pulls BOOT/EN up. Enter download immediately. */
    s_skip_bootloader_enter = false;
    s_h2_reset_to_app = false;
    h2_enter_download();
    ESP_LOGI(TAG, "H2 SYNC S3 RX=GPIO%d TX=GPIO%d (BOOT held low)", rx, tx);
    drain_h2_uart();

    s_skip_bootloader_enter = true;
    esp_loader_connect_args_t conn = {
        .sync_timeout = 500,
        .trials = 15,
    };
    esp_loader_error_t err = esp_loader_connect(&conn);
    s_skip_bootloader_enter = false;
    return err;
}

static const char *loader_err_str(esp_loader_error_t err)
{
    switch (err) {
    case ESP_LOADER_SUCCESS: return "ok";
    case ESP_LOADER_ERROR_FAIL: return "fail";
    case ESP_LOADER_ERROR_TIMEOUT: return "timeout";
    case ESP_LOADER_ERROR_INVALID_RESPONSE: return "invalid_response";
    case ESP_LOADER_ERROR_UNSUPPORTED_CHIP: return "unsupported_chip";
    default: return "other";
    }
}

static esp_err_t connect_h2(void)
{
    if (s.connected) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(zboss_manager_pause_for_rcp_flash(), TAG, "pause zigbee");
    s.zigbee_paused = true;

    if (!GPIO_IS_VALID_OUTPUT_GPIO(RCP_EN_GPIO)) {
        ESP_LOGE(TAG, "EN pin GPIO%d is not a valid output, H2 cannot be reset", (int)RCP_EN_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    const int rx = CONFIG_PIN_TO_RCP_TX;
    const int tx = CONFIG_PIN_TO_RCP_RX;

    /* serial-flasher sync blocks without feeding. Pause TWDT for this task only,
     * then restore. Flash writes still call watchdog_feed(). */
    const bool wdt_paused = watchdog_pause_self();
    ESP_LOGI(TAG, "H2 connect (do not type)");
    esp_loader_error_t lerr = try_h2_sync(rx, tx);
    if (lerr != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "H2 bootloader connect %s (EN=GPIO%d BOOT=GPIO%d RX=%d TX=%d). "
                 "Need ROM 'waiting for download'. USB_BOOT means GPIO8 was 0 at reset "
                 "(Joint Download is GPIO8=1, GPIO9=0).",
                 loader_err_str(lerr), (int)RCP_EN_GPIO, (int)RCP_BOOT_GPIO, rx, tx);
        watchdog_resume_self(wdt_paused);
        release_flasher_uart(rx, tx);
        rcp_coprocessor_hold_running();
        return ESP_FAIL;
    }
    if (esp_loader_get_target() != ESP32H2_CHIP) {
        ESP_LOGE(TAG, "Expected ESP32-H2, got chip %d", (int)esp_loader_get_target());
        watchdog_resume_self(wdt_paused);
        loader_port_esp32_deinit();
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Stay at 115200. 460800 during Matter BDX overruns UART and fails MD5. */

    watchdog_resume_self(wdt_paused);
    s.connected = true;
    ESP_LOGI(TAG, "H2 bootloader connected, flashing official RCP");
    return ESP_OK;
}

static esp_err_t flash_flush(bool last)
{
    (void)last;
    if (s.block_fill == 0) {
        return ESP_OK;
    }
    watchdog_feed();
    /* Pass the real length. esp_loader_flash_write() pads the SLIP frame
     * to kFlashBlock in-place; MD5 only covers (size+3)&~3 of payload. */
    if (esp_loader_flash_write(s.block, s.block_fill) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "H2 flash write failed");
        return ESP_FAIL;
    }
    s.block_fill = 0;
    return ESP_OK;
}

static esp_err_t flash_bytes(const uint8_t *data, size_t len)
{
    while (len > 0 && s.flash_remain > 0) {
        size_t room = kFlashBlock - s.block_fill;
        size_t take = len < room ? len : room;
        if (take > s.flash_remain) {
            take = s.flash_remain;
        }
        memcpy(s.block + s.block_fill, data, take);
        s.block_fill += static_cast<uint32_t>(take);
        s.flash_remain -= static_cast<uint32_t>(take);
        data += take;
        len -= take;
        if (s.block_fill == kFlashBlock) {
            ESP_RETURN_ON_ERROR(flash_flush(false), TAG, "flush");
        }
    }
    if (s.flash_remain == 0) {
        ESP_RETURN_ON_ERROR(flash_flush(true), TAG, "last flush");
        if (esp_loader_flash_verify() != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "H2 MD5 verify failed");
            return ESP_FAIL;
        }
        s.flashing = false;
        ESP_LOGI(TAG, "H2 region @ 0x%" PRIx32 " verified", s.flash_addr);
    }
    return ESP_OK;
}

static esp_err_t begin_flash_subfile(uint32_t tag, uint32_t size)
{
    s.flashing = false;
    s.block_fill = 0;
#if !CONFIG_GATEWAY_RCP_OTA
    (void)tag;
    (void)size;
    return ESP_OK;
#else
    if (!subfile_is_flashable(tag)) {
        return ESP_OK;
    }
    uint32_t addr = flash_addr_for_tag(tag);
    if (addr == UINT32_MAX) {
        ESP_LOGE(TAG, "No flash offset for RCP tag %" PRIu32, tag);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(connect_h2(), TAG, "connect");
    uint32_t aligned = (size + 3u) & ~3u;
    if (esp_loader_flash_start(addr, aligned < 4 ? 4 : aligned, kFlashBlock) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "esp_loader_flash_start failed tag=%" PRIu32 " addr=0x%" PRIx32, tag, addr);
        return ESP_FAIL;
    }
    s.flash_addr   = addr;
    s.flash_remain = size;
    s.flashing     = true;
    ESP_LOGI(TAG, "Flashing H2 tag=%" PRIu32 " %" PRIu32 " bytes @ 0x%" PRIx32, tag, size, addr);
    return ESP_OK;
#endif
}

static void parse_header(void)
{
    s.file_count = static_cast<int>(s.header_need / sizeof(esp_rcp_subfile_info_t));
    if (s.file_count > static_cast<int>(kMaxSubfiles)) {
        s.file_count = kMaxSubfiles;
    }
    memcpy(s.files, s.header, s.file_count * sizeof(esp_rcp_subfile_info_t));
    s.host_offset = 0;
    s.host_size   = 0;
    for (int i = 0; i < s.file_count; i++) {
        if (s.files[i].tag == FILETAG_HOST_FIRMWARE) {
            s.host_offset = s.files[i].offset;
            s.host_size   = s.files[i].size;
        }
    }
}

static esp_err_t ingest_flash_args(const uint8_t *data, size_t len)
{
    size_t have = s.flash_arg_count * sizeof(FlashArg);
    size_t cap  = sizeof(s.flash_args);
    if (have >= cap) {
        return ESP_OK;
    }
    size_t room = cap - have;
    size_t take = len < room ? len : room;
    memcpy(reinterpret_cast<uint8_t *>(s.flash_args) + have, data, take);
    s.flash_arg_count = static_cast<int>((have + take) / sizeof(FlashArg));
    return ESP_OK;
}

static esp_err_t finish_rcp(void)
{
    if (s.connected) {
        s_skip_bootloader_enter = false;
        s_h2_reset_to_app = true;
        gpio_hold_dis(RCP_BOOT_GPIO);
        esp_loader_reset_target();
        loader_port_esp32_deinit();
        s.connected = false;
        ESP_LOGI(TAG, "H2 reset into new RCP firmware");
    }
    /* Leave EN/BOOT driven for normal run (deinit does not release GPIOs) */
    rcp_coprocessor_hold_running();
    s.rcp_done = true;
    s.phase    = Phase::Host;
    return ESP_OK;
}

void rcp_coprocessor_hold_running(void)
{
    gpio_hold_dis(RCP_BOOT_GPIO);
    gpio_hold_dis(RCP_EN_GPIO);
    s_h2_reset_to_app = true;
    s_skip_bootloader_enter = false;
    h2_drive_boot(1);
    vTaskDelay(pdMS_TO_TICKS(kH2BootSetupMs));
    h2_en_low();
    vTaskDelay(pdMS_TO_TICKS(kH2ResetLowMs));
    h2_en_release();
    ESP_LOGI(TAG, "H2 held running (EN=GPIO%d released, BOOT=GPIO%d high)",
             (int)RCP_EN_GPIO, (int)RCP_BOOT_GPIO);
}

esp_err_t rcp_coprocessor_sync_test(void)
{
    s.connected = false;
    esp_err_t err = connect_h2();
    if (s.connected) {
        ESP_LOGI(TAG, "synctest: H2 ROM connected, chip=%d", (int)esp_loader_get_target());
        loader_port_esp32_deinit();
        s.connected = false;
    }
    rcp_coprocessor_hold_running();
    return err;
}

void rcp_coprocessor_reset_session(void)
{
    if (s.connected) {
        loader_port_esp32_deinit();
        s.connected = false;
        rcp_coprocessor_hold_running();
    }
    memset(&s, 0, sizeof(s));
    s.phase = Phase::Header;
}

void rcp_coprocessor_abort(void)
{
    rcp_coprocessor_reset_session();
}

bool rcp_coprocessor_payload_is_combined(const uint8_t *data, size_t len)
{
    if (len < 4) {
        return false;
    }
    uint32_t tag = 0;
    memcpy(&tag, data, 4);
    return tag == FILETAG_IMAGE_HEADER;
}

bool rcp_coprocessor_rcp_done(void)
{
    return s.rcp_done;
}

uint32_t rcp_coprocessor_host_offset(void)
{
    return s.host_offset;
}

uint32_t rcp_coprocessor_host_size(void)
{
    return s.host_size;
}

bool rcp_coprocessor_needs_reboot_after_abort(void)
{
    return s.zigbee_paused;
}

esp_err_t rcp_coprocessor_feed(const uint8_t *data, size_t len, const uint8_t **host_data, size_t *host_len)
{
    if (host_data) {
        *host_data = nullptr;
    }
    if (host_len) {
        *host_len = 0;
    }
    if (data == nullptr || len == 0) {
        return ESP_OK;
    }

    if (s.phase == Phase::Host || s.rcp_done) {
        if (host_data) {
            *host_data = data;
        }
        if (host_len) {
            *host_len = len;
        }
        return ESP_OK;
    }

    while (len > 0) {
        if (s.phase == Phase::Header) {
            if (s.header_need == 0) {
                if (s.header_got < sizeof(esp_rcp_subfile_info_t)) {
                    size_t take = sizeof(esp_rcp_subfile_info_t) - s.header_got;
                    if (take > len) {
                        take = len;
                    }
                    memcpy(s.header + s.header_got, data, take);
                    s.header_got += static_cast<uint32_t>(take);
                    data += take;
                    len -= take;
                }
                if (s.header_got >= sizeof(esp_rcp_subfile_info_t)) {
                    auto *info = reinterpret_cast<esp_rcp_subfile_info_t *>(s.header);
                    if (info->tag != FILETAG_IMAGE_HEADER ||
                        info->size % sizeof(esp_rcp_subfile_info_t) != 0 ||
                        info->size > sizeof(s.header)) {
                        ESP_LOGE(TAG, "Not a combined RCP+host image");
                        return ESP_ERR_INVALID_ARG;
                    }
                    s.header_need = info->size;
                }
            }
            if (s.header_need > 0 && s.header_got < s.header_need) {
                size_t take = s.header_need - s.header_got;
                if (take > len) {
                    take = len;
                }
                memcpy(s.header + s.header_got, data, take);
                s.header_got += static_cast<uint32_t>(take);
                data += take;
                len -= take;
            }
            if (s.header_need > 0 && s.header_got >= s.header_need) {
                parse_header();
                s.phase      = Phase::Subfile;
                s.file_index = 1;
                s.sub_got    = 0;
                ESP_LOGI(TAG, "Combined image: RCP then S3 host (%" PRIu32 " + %" PRIu32 ")",
                         s.host_offset, s.host_size);
                if (s.file_index < s.file_count) {
                    ESP_RETURN_ON_ERROR(begin_flash_subfile(s.files[s.file_index].tag, s.files[s.file_index].size),
                                        TAG, "begin first subfile");
                }
            }
            continue;
        }

        if (s.phase == Phase::Subfile) {
            if (s.file_index >= s.file_count) {
                ESP_RETURN_ON_ERROR(finish_rcp(), TAG, "finish");
                if (host_data) {
                    *host_data = data;
                }
                if (host_len) {
                    *host_len = len;
                }
                return ESP_OK;
            }
            esp_rcp_subfile_info_t *cur = &s.files[s.file_index];
            if (cur->tag == FILETAG_HOST_FIRMWARE) {
                ESP_RETURN_ON_ERROR(finish_rcp(), TAG, "finish before host");
                if (host_data) {
                    *host_data = data;
                }
                if (host_len) {
                    *host_len = len;
                }
                return ESP_OK;
            }

            uint32_t left = cur->size - s.sub_got;
            size_t take   = len < left ? len : left;
            if (cur->tag == FILETAG_RCP_FLASH_ARGS) {
                ingest_flash_args(data, take);
            } else if (s.flashing) {
                ESP_RETURN_ON_ERROR(flash_bytes(data, take), TAG, "flash");
            }
            s.sub_got += static_cast<uint32_t>(take);
            data += take;
            len -= take;
            if (s.sub_got >= cur->size) {
                s.file_index++;
                s.sub_got = 0;
                if (s.file_index < s.file_count && s.files[s.file_index].tag != FILETAG_HOST_FIRMWARE) {
                    ESP_RETURN_ON_ERROR(
                        begin_flash_subfile(s.files[s.file_index].tag, s.files[s.file_index].size), TAG,
                        "begin next");
                }
            }
        }
    }
    return ESP_OK;
}
