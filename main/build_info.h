/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file build_info.h
 *
 * @brief 构建元信息
 *
 * 通过 CMake 注入 git SHA、分支、构建时间。
 * 启动时打印到日志，确保线上可追溯固件版本。
 *
 * 如果 git 不可用（如 CI 环境无 .git 目录），自动回退到默认值。
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BUILD_GIT_SHA
#define BUILD_GIT_SHA    "unknown"
#endif

#ifndef BUILD_GIT_BRANCH
#define BUILD_GIT_BRANCH "unknown"
#endif

#ifndef BUILD_TIMESTAMP
#define BUILD_TIMESTAMP  __DATE__ " " __TIME__
#endif

#ifndef BUILD_VERSION
#define BUILD_VERSION    "1.3.3"
#endif

/**
 * @brief 获取构建信息的格式化字符串（启动时打印用）
 *
 * 返回指向静态缓冲区的指针，格式：
 *   "v1.0.0 (abc1234 on main, 2026-09-13 10:00:00)"
 */
const char *build_info_get_string(void);

#ifdef __cplusplus
}
#endif