#pragma once

#include <esp_err.h>
#include <esp_zigbee_core.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void zb_tx_init(void);
void zb_tx_pause(void);

void zb_tx_on_off(uint16_t short_addr, uint8_t endpoint, bool on);
void zb_tx_move_to_level(uint16_t short_addr, uint8_t endpoint, uint8_t level);
void zb_tx_move_to_color(uint16_t short_addr, uint8_t endpoint, uint16_t x, uint16_t y);
void zb_tx_move_to_hue_sat(uint16_t short_addr, uint8_t endpoint, uint8_t hue, uint8_t sat);
void zb_tx_move_to_color_temp(uint16_t short_addr, uint8_t endpoint, uint16_t mireds);
void zb_tx_write_u16(uint16_t short_addr, uint8_t endpoint, uint16_t cluster, uint16_t attr, uint16_t value);
void zb_tx_config_report(uint16_t short_addr, uint8_t endpoint, uint16_t cluster, uint16_t attr,
                         uint8_t attr_type, uint16_t change, uint16_t min_interval, uint16_t max_interval);
void zb_tx_bind(uint16_t short_addr, uint8_t endpoint, uint16_t cluster, const esp_zb_ieee_addr_t src_ieee);
void zb_tx_read_basic(uint16_t short_addr, uint8_t endpoint);
void zb_tx_simple_desc(uint16_t short_addr, uint8_t endpoint);

#ifdef __cplusplus
}
#endif
