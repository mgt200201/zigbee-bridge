/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file wifi_manager.cpp
 *
 * @brief WiFi 连接管理层
 *
 * 负责监测 WiFi 连接状态，处理断线重连。
 * 通过 Chip DeviceEvent 机制接收事件通知。
 *
 * 另：WiFi 拿到 IP 后启动一次 SNTP 校时，成功后把时间回写外部 RTC(RX8025T)，
 *     这样网关掉电重启后即使暂时没联网，也能对外提供正确的时间。
 *
 * 依赖：Matter 协议栈已启动（由 app_main 保证），RTC 已在 app_main 初始化
 */

#include "wifi_manager.h"
#include "zb_device_ota.h"
#include "rtc_rx8025t.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <board_config.h>

/* 直接用底层 esp_sntp（而非 esp_netif_sntp 封装）：需要把解析好的 IP 显式喂给
 * SNTP，绕开封装内部 sntp_setservername() 触发的"按域名解析且优先 IPv4"行为 */
#include <esp_sntp.h>

/* 校时诊断/服务器解析用：解析 NTP 主机名、打印网卡 IP/DNS */
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <lwip/ip_addr.h>
#include <lwip/ip6_addr.h>
#include <string.h>

#include <esp_matter.h>

#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "wifi_manager";

/* ---- 校时参数 ----
 * CONFIG_LWIP_SNTP_MAX_SERVERS=1，只能配置单个 NTP 服务器，故按轮次轮换候选主机。
 * 国内常见 NTP（ntp.aliyun.com / ntp.tencent.com）只有 A 记录、没有 AAAA，
 * 纯 IPv6 网络里解析不出来，所以候选中必须包含带 AAAA 的记录。 */
static const char *const RTC_NTP_HOSTS[] = {
    "ntp.aliyun.com",   /* 国内 IPv4 首选（无 AAAA） */
    "time.apple.com",   /* 含 AAAA：纯 IPv6 可用 */
    "ntp.neu.edu.cn",   /* 含 AAAA(2001:da8:9000::130)：国内 CERNET2 */
    "ntp.ubuntu.com",   /* 含 AAAA：最后兜底 */
};
#define RTC_NTP_HOST_COUNT       (sizeof(RTC_NTP_HOSTS) / sizeof(RTC_NTP_HOSTS[0]))
#define RTC_ADDR_WAIT_MS         10000    /* 等"有可用地址(IPv4 或全局 IPv6)"的上限(ms) */
#define RTC_SNTP_WAIT_MS         6000     /* 单次等待 SNTP 同步的上限(ms) */
#define RTC_SNTP_RETRY_MS        6000     /* 单次未同步成功后的重试间隔(ms) */
#define RTC_SNTP_MAX_RETRY       18       /* 最大重试次数（约覆盖 3~4 分钟的网络收敛窗口） */
#define RTC_SNTP_TASK_STACK      4096
#define RTC_SNTP_TASK_PRIO       5

/* 当前时区（POSIX TZ 格式），运行时可改；RTC 里存的就是该时区的本地时间 */
static char s_tz[40] = WIFI_MANAGER_TZ_DEFAULT;

/* 防止并发重复启动校时任务；仅在"同步成功"或"彻底失败"后复位，
 * 这样网关刚联网 DNS/NTP 未就绪导致首次超时时，后续 IP 事件仍会重试 */
static bool s_rtc_sync_started = false;

/* ---- 无 DNS 时的兜底 ----
 * 纯 IPv6 热点（只做 SLAAC、不做 DHCPv4）可能既不给 IPv4，也不通过 RDNSS 下发 DNS，
 * 此时域名解析必然失败，SNTP 连请求都发不出去。这里在缺 DNS 时补公共 DNS：
 * 有 IPv4 用 IPv4 DNS，纯 IPv6 网络必须用 IPv6 DNS。 */
#define RTC_DNS_FALLBACK_V4   "223.5.5.5"      /* AliDNS */
#define RTC_DNS_FALLBACK_V6   "2400:3200::1"   /* AliDNS IPv6 */

/* 选一张真正有可用地址的网卡：优先有 IPv4 的，其次有全局 IPv6 的。
 * 不用 esp_netif_get_default_netif()：设备是 STA+AP 双模，默认网卡不一定是 STA。 */
static esp_netif_t *rtc_pick_netif(void)
{
    esp_netif_t *fallback = NULL;
    esp_netif_t *n = NULL;
    while ((n = esp_netif_next(n)) != NULL) {
        if (!esp_netif_is_netif_up(n)) {
            continue;
        }
        esp_netif_ip_info_t ip4 = {};
        esp_netif_get_ip_info(n, &ip4);
        if (ip4.ip.addr != 0) {
            return n;
        }
        if (fallback == NULL) {
            esp_ip6_addr_t v6[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
            int cnt = esp_netif_get_all_ip6(n, v6);
            for (int i = 0; i < cnt; ++i) {
                if (esp_netif_ip6_get_addr_type(&v6[i]) == ESP_IP6_ADDR_IS_GLOBAL) {
                    fallback = n;
                    break;
                }
            }
        }
    }
    return fallback;
}

/* 判断网卡上的主 DNS 是否真的可用。
 * RDNSS 常下发路由器链路本地地址(fe80::)，lwip 拿它去解析会直接返回
 * ERR_VAL(rc=202)，这类 DNS 等同于没有。 */
static bool rtc_dns_usable(esp_netif_t *netif)
{
    esp_netif_dns_info_t dns = {};
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) {
        return false;
    }
    if (ESP_IP_IS_ANY(dns.ip)) {
        return false;
    }
    if (dns.ip.type == ESP_IPADDR_TYPE_V6
        && ip6_addr_islinklocal((const ip6_addr_t *)&dns.ip.u_addr.ip6)) {
        return false;
    }
    return true;
}

/* 任一在用网卡上是否有可用 DNS */
static bool rtc_any_dns_usable(void)
{
    esp_netif_t *netif = rtc_pick_netif();
    return netif != NULL && rtc_dns_usable(netif);
}

static void rtc_apply_dns_fallback(void)
{
    esp_netif_t *netif = rtc_pick_netif();
    if (!netif) {
        ESP_LOGW(TAG, "[dns] no usable netif, skip fallback");
        return;
    }

    if (rtc_dns_usable(netif)) {
        return;   /* 网络已下发可用 DNS，不动它 */
    }
    ESP_LOGW(TAG, "[dns] DNS is link-local(fe80::) or absent, unusable -> override");

    esp_netif_ip_info_t ip4 = {};
    esp_netif_get_ip_info(netif, &ip4);

    esp_netif_dns_info_t dns = {};
    const char *addr;
    if (ip4.ip.addr != 0) {
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        ip4addr_aton(RTC_DNS_FALLBACK_V4, (ip4_addr_t *)&dns.ip.u_addr.ip4);
        addr = RTC_DNS_FALLBACK_V4;
    } else {
        dns.ip.type = ESP_IPADDR_TYPE_V6;
        if (!inet6_aton(RTC_DNS_FALLBACK_V6, (ip6_addr_t *)&dns.ip.u_addr.ip6)) {
            ESP_LOGW(TAG, "[dns] bad fallback address");
            return;
        }
        addr = RTC_DNS_FALLBACK_V6;
    }

    esp_err_t err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    ESP_LOGW(TAG, "[dns] network gave no DNS, fallback %s -> %s",
             addr, esp_err_to_name(err));
}

/* ---- 校时失败诊断 ----
 * 打印各网卡的 IPv4/IPv6/DNS，并尝试解析 NTP 主机名。
 * 用于区分三类根因：没有 IPv4 / 没有 DNS / 域名解析不到地址。
 * 定位完成后可整块删除（连同任务里的调用）。 */
static void rtc_log_network_diag(void)
{
    esp_netif_t *n = NULL;
    while ((n = esp_netif_next(n)) != NULL) {
        const char *key = esp_netif_get_ifkey(n);
        int up = esp_netif_is_netif_up(n) ? 1 : 0;
        esp_netif_ip_info_t ip4 = {};
        esp_netif_get_ip_info(n, &ip4);
        if (ip4.ip.addr) {
            ESP_LOGI(TAG, "[diag] %s up=%d IPv4 " IPSTR, key, up, IP2STR(&ip4.ip));
        } else {
            ESP_LOGW(TAG, "[diag] %s up=%d NO IPv4", key, up);
        }
        esp_ip6_addr_t v6[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
        int cnt = esp_netif_get_all_ip6(n, v6);
        for (int i = 0; i < cnt; ++i) {
            ESP_LOGI(TAG, "[diag] %s IPv6[%d] " IPV6STR, key, i, IPV62STR(v6[i]));
        }
        esp_netif_dns_info_t dns = {};
        if (esp_netif_get_dns_info(n, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK
            && !ESP_IP_IS_ANY(dns.ip)) {
            if (dns.ip.type == ESP_IPADDR_TYPE_V6) {
                ESP_LOGI(TAG, "[diag] %s DNS main: " IPV6STR, key, IPV62STR(dns.ip.u_addr.ip6));
            } else {
                ESP_LOGI(TAG, "[diag] %s DNS main: " IPSTR, key, IP2STR(&dns.ip.u_addr.ip4));
            }
        } else {
            ESP_LOGW(TAG, "[diag] %s DNS main: none", key);
        }
    }

    /* DNS 不可用时逐个解析只会白等 lwip 的 DNS 超时（每个主机约 5~7s），
     * 这里直接跳过，等 DNS 就绪后的下一次诊断再打印。 */
    if (!rtc_any_dns_usable()) {
        ESP_LOGW(TAG, "[diag] DNS unusable, skip hostname resolution");
        return;
    }

    char buf[64];
    for (size_t h = 0; h < sizeof(RTC_NTP_HOSTS) / sizeof(RTC_NTP_HOSTS[0]); ++h) {
        struct addrinfo hints = {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        struct addrinfo *res = NULL;
        int rc = getaddrinfo(RTC_NTP_HOSTS[h], "123", &hints, &res);
        if (rc != 0 || res == NULL) {
            ESP_LOGW(TAG, "[diag] resolve '%s' FAILED (rc=%d)", RTC_NTP_HOSTS[h], rc);
            continue;
        }
        for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
            if (ai->ai_family == AF_INET) {
                inet_ntoa_r(((struct sockaddr_in *)ai->ai_addr)->sin_addr, buf, sizeof(buf));
                ESP_LOGI(TAG, "[diag] '%s' -> A %s", RTC_NTP_HOSTS[h], buf);
            } else if (ai->ai_family == AF_INET6) {
                inet6_ntoa_r(((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr, buf, sizeof(buf));
                ESP_LOGI(TAG, "[diag] '%s' -> AAAA %s", RTC_NTP_HOSTS[h], buf);
            }
        }
        freeaddrinfo(res);
    }
}

/* 解析单个 NTP 主机名，返回设备真正可达的服务器地址。
 *
 * 必须自己做解析（不让 SNTP 内部 sntp_setservername 代劳）的两个原因：
 *  1) 地址族：lwip 的 dns_gethostbyname() 默认 LWIP_DNS_ADDRTYPE_IPV4_IPV6
 *     （优先 IPv4），纯 IPv6 网络里会拿到一条设备根本路由不到的 A 记录；
 *  2) 主机名：ntp.aliyun.com 只有 A 记录、没有 AAAA，纯 IPv6 网络里按 AAAA
 *     解析必然失败，需要轮换到带 AAAA 的主机名（见 RTC_NTP_HOSTS）。 */
static bool rtc_resolve_host(const char *host, ip_addr_t *out)
{
    esp_netif_t *netif = rtc_pick_netif();
    bool have_v4 = false;
    if (netif) {
        esp_netif_ip_info_t ip4 = {};
        esp_netif_get_ip_info(netif, &ip4);
        have_v4 = (ip4.ip.addr != 0);
    }

    struct addrinfo hints = {};
    hints.ai_family = have_v4 ? AF_INET : AF_INET6;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, "123", &hints, &res) != 0 || res == NULL) {
        return false;
    }

    bool ok = false;
    for (struct addrinfo *ai = res; ai != NULL && !ok; ai = ai->ai_next) {
        if (have_v4 && ai->ai_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)ai->ai_addr;
            ip_addr_set_ip4_u32(out, sin->sin_addr.s_addr);
            ok = true;
        } else if (!have_v4 && ai->ai_family == AF_INET6) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ai->ai_addr;
            IP_SET_TYPE_VAL(*out, IPADDR_TYPE_V6);
            memcpy(ip_2_ip6(out)->addr, sin6->sin6_addr.un.u8_addr,
                   sizeof(ip_2_ip6(out)->addr));
            ip6_addr_clear_zone(ip_2_ip6(out));
            ok = true;
        }
    }
    freeaddrinfo(res);
    return ok;
}

/* 后台任务：轮换候选 NTP 主机 → 解析成显式 IP → SNTP 同步 → 回写 RTC
 *
 * 注意调用顺序：esp_sntp_init() 是异步 tcpip 回调，且 sntp_init() 里会立即用
 * "当前已设置的服务器"发一次请求。若先 init 再 setserver，首个请求会带着空地址
 * 发出去，然后要等 lwip 内部 15s 重试才轮到真实地址。所以必须 setserver 在前。 */
static void rtc_time_sync_task(void *arg)
{
    (void)arg;

    /* 首个 IP 事件到来时可能只有链路本地 IPv6，全局地址/DNS 尚未收敛。
     * 先等到"有 IPv4 或全局 IPv6"，再补 DNS、做诊断，避免在 DNS 都没就绪时
     * 逐个解析主机名而白等超时。 */
    for (int waited = 0; waited < RTC_ADDR_WAIT_MS && rtc_pick_netif() == NULL;
         waited += 500) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    rtc_apply_dns_fallback();
    rtc_log_network_diag();

    bool sntp_started = false;
    bool synced = false;

    for (int attempt = 0; attempt < RTC_SNTP_MAX_RETRY && !synced; ++attempt) {
        /* RDNSS 可能把 DNS 重新写回链路本地地址，每轮都确认一次 */
        rtc_apply_dns_fallback();

        /* 一轮内逐个试候选主机：解析不到（例如纯 IPv6 下该域名没有 AAAA）
         * 就立刻换下一个，不必白等一整个重试周期。 */
        ip_addr_t srv = {};
        const char *host = NULL;
        for (size_t k = 0; k < RTC_NTP_HOST_COUNT; ++k) {
            const char *cand = RTC_NTP_HOSTS[(attempt + k) % RTC_NTP_HOST_COUNT];
            if (rtc_resolve_host(cand, &srv)) {
                host = cand;
                break;
            }
        }

        if (host == NULL) {
            ESP_LOGW(TAG, "attempt %d/%d: no NTP hostname resolvable yet",
                     attempt + 1, RTC_SNTP_MAX_RETRY);
            vTaskDelay(pdMS_TO_TICKS(1000));   /* DNS 可能刚就绪，快速重试 */
            continue;
        }

        if (!sntp_started) {
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);   /* 立即步进系统时钟 */
            esp_sntp_setserver(0, &srv);   /* 必须在 init 之前（init 是异步回调） */
            esp_sntp_init();
            sntp_started = true;
        } else {
            esp_sntp_setserver(0, &srv);
            esp_sntp_restart();            /* 立即用新服务器重发请求 */
        }
        ESP_LOGI(TAG, "attempt %d/%d: NTP server '%s' -> %s",
                 attempt + 1, RTC_SNTP_MAX_RETRY, host,
                 IP_IS_V6(&srv) ? "IPv6" : "IPv4");

        /* 轮询等待本次同步完成（底层 SNTP 内部会自行重试发请求） */
        for (int waited = 0; waited < RTC_SNTP_WAIT_MS; waited += 200) {
            if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
                synced = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        if (!synced) {
            ESP_LOGW(TAG, "SNTP not ready (attempt %d/%d), next in %d ms",
                     attempt + 1, RTC_SNTP_MAX_RETRY, RTC_SNTP_RETRY_MS);
            vTaskDelay(pdMS_TO_TICKS(RTC_SNTP_RETRY_MS));
        }
    }

    if (sntp_started) {
        esp_sntp_stop();
    }

    if (synced) {
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);   /* 存东八区本地时间 */
        if (rtc_rx8025t_set_time(&local) == ESP_OK) {
            ESP_LOGI(TAG, "RTC calibrated: %04d-%02d-%02d %02d:%02d:%02d",
                     local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                     local.tm_hour, local.tm_min, local.tm_sec);
        } else {
            ESP_LOGW(TAG, "RTC write failed (external RTC absent?)");
        }
    } else {
        ESP_LOGE(TAG, "SNTP sync failed after %d attempts", RTC_SNTP_MAX_RETRY);
        s_rtc_sync_started = false;   /* 允许下次 IP 事件重试 */
    }

    vTaskDelete(NULL);
}

/* ==================== 生命周期 ==================== */

void wifi_manager_set_timezone(const char *tz)
{
    if (tz == NULL || tz[0] == '\0') {
        return;
    }
    bool changed = (strcmp(s_tz, tz) != 0);

    snprintf(s_tz, sizeof(s_tz), "%s", tz);
    setenv("TZ", s_tz, 1);
    tzset();
    ESP_LOGI(TAG, "timezone = '%s'", s_tz);

    if (!changed) {
        return;
    }

    /* RTC 存的是本地时间，时区变化后按新时区重写一次。
     * 系统时间尚未校准（年份还是 1970）时跳过，避免把垃圾值写进 RTC。 */
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    if (local.tm_year + 1900 >= 2020 && rtc_rx8025t_set_time(&local) == ESP_OK) {
        ESP_LOGI(TAG, "RTC rewritten for timezone: %04d-%02d-%02d %02d:%02d:%02d",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                 local.tm_hour, local.tm_min, local.tm_sec);
    }
}

const char *wifi_manager_get_timezone(void)
{
    return s_tz;
}

void wifi_manager_init(void)
{
    wifi_manager_set_timezone(WIFI_MANAGER_TZ_DEFAULT);

    ESP_LOGI(TAG, "WiFi manager initialized");
}

/* ==================== 事件处理 ==================== */

void wifi_manager_handle_chip_event(const void *event, intptr_t arg)
{
    (void)arg;
    const ChipDeviceEvent *chip_event = static_cast<const ChipDeviceEvent *>(event);

    switch (chip_event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address Changed");
        /* HA 经局域网 HTTP 上传子设备 OTA 包 */
        if (zb_device_ota_http_start() != ESP_OK) {
            ESP_LOGW(TAG, "zb_device_ota HTTP start deferred/failed");
        }
        /* 拿到 IP 后启动一次 SNTP 校时 → 回写外部 RTC */
        if (!s_rtc_sync_started) {
            s_rtc_sync_started = true;
            if (xTaskCreate(rtc_time_sync_task, "rtc_time_sync",
                            RTC_SNTP_TASK_STACK, NULL, RTC_SNTP_TASK_PRIO, NULL) != pdPASS) {
                ESP_LOGW(TAG, "failed to start rtc_time_sync task");
                s_rtc_sync_started = false;
            }
        }
        break;

    case chip::DeviceLayer::DeviceEventType::kWiFiConnectivityChange:
        if (chip_event->WiFiConnectivityChange.Result
            == chip::DeviceLayer::kConnectivity_Established) {
            ESP_LOGI(TAG, "WiFi connected");
        } else if (chip_event->WiFiConnectivityChange.Result
                   == chip::DeviceLayer::kConnectivity_Lost) {
            ESP_LOGI(TAG, "WiFi disconnected, reconnecting...");
            esp_wifi_connect();
        }
        break;

    default:
        break;
    }
}
