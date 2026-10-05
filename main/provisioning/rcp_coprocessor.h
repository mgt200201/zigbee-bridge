/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file rcp_coprocessor.h
 *
 * Stream the official ot_rcp combined image onto ESP32-H2 via UART download
 * mode (RESET + BOOT pins), then hand remaining bytes (ESP32-S3 firmware)
 * back to the Matter OTA writer.
 */

#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Drive H2 EN high and BOOT high so the co-processor runs normally.
 * Call once at boot (before Zigbee starts). Also called after RCP flash.
 */
void rcp_coprocessor_hold_running(void);

void rcp_coprocessor_reset_session(void);
void rcp_coprocessor_abort(void);

/** Combined packed image (tag 0xff) vs raw ESP app (magic 0xE9). */
bool rcp_coprocessor_payload_is_combined(const uint8_t *data, size_t len);

/**
 * Consume packed-image bytes. Host firmware leftover is returned in
 * *host_data / *host_len (may be NULL/0). Call until host_size() is known
 * and all RCP bytes have been flashed.
 */
esp_err_t rcp_coprocessor_feed(const uint8_t *data, size_t len,
                               const uint8_t **host_data, size_t *host_len);

bool rcp_coprocessor_rcp_done(void);
uint32_t rcp_coprocessor_host_offset(void);
uint32_t rcp_coprocessor_host_size(void);
/** Zigbee UART was released to serial-flasher; caller should reboot to restore it. */
bool rcp_coprocessor_needs_reboot_after_abort(void);

/** Pause Zigbee UART, try H2 ROM sync, restore EN/BOOT. Caller should reboot. */
esp_err_t rcp_coprocessor_sync_test(void);

#ifdef __cplusplus
}
#endif
