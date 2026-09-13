/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file handler_temperature.h
 *
 * @brief Temperature Measurement cluster handler (0x0402)
 */

#pragma once

#include "zcl_handler_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const zcl_device_handler_t temp_handler;

#ifdef __cplusplus
}
#endif