/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file handler_fc11.h
 *
 * @brief 私有/厂商自定义集群 0xFC11 handler
 */

#pragma once

#include "zcl_handler_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FC11_ATTR_REGISTER_STATUS     0x1FFF   /**< uint16，寄存器状态 */

extern const zcl_device_handler_t fc11_handler;

#ifdef __cplusplus
}
#endif