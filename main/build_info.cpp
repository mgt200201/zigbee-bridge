/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file build_info.cpp
 *
 * @brief 构建元信息实现
 */

#include "build_info.h"
#include <stdio.h>

static char s_build_str[128];

const char *build_info_get_string(void)
{
    snprintf(s_build_str, sizeof(s_build_str),
             "v%s (%s on %s, %s)",
             BUILD_VERSION,
             BUILD_GIT_SHA,
             BUILD_GIT_BRANCH,
             BUILD_TIMESTAMP);
    return s_build_str;
}