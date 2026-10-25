/*
   SPDX-License-Identifier: Apache-2.0

   串口命令行接口。
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动串口命令监听任务
 *
 * 支持的命令：factoryreset / getIp / entest / bootest / dltest / iostest /
 * synctest / help。
 */
void serial_cmd_start(void);

#ifdef __cplusplus
}
#endif
