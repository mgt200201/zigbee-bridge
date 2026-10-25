/*
   SPDX-License-Identifier: Apache-2.0

   配网凭据同步：把自定义 PIN / Discriminator / spake2p 参数写入 Matter 协议栈。
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 将 commissioning_mgr 中的 PIN/Discriminator 同步到 CHIP 协议栈
 *
 * 必须在 esp_matter::start() 之前调用：
 *   - 写入 SetupPinCode / SetupDiscriminator
 *   - 读取（或生成）spake2p salt / iteration count
 *   - 用当前 PIN 重新生成 spake2p verifier
 *
 * 否则 PIN 变了但 verifier 没变，PASE 握手会失败
 * （CHIP_ERROR_INVALID_PASE_PARAMETER）。
 */
void commissioning_sync_to_stack(void);

#ifdef __cplusplus
}
#endif
