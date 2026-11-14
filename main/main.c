// 实验 6：网页遥控灯（WebSocket 版），ESP-IDF 移植版
// 原 Arduino 工程：D:\Arduino_ESP32_S3\07_web_led
//
// 板子连上 WiFi 后运行：
//   - 80 端口：HTTP 网页服务器，提供控制页面，以及 GET /state、POST /set、GET /sys（资源占用）接口，
//     以及网页测速用的 GET /speed/down、POST /speed/up
//   - 81 端口：WebSocket 服务器，网页通过它实时收发指令，所有打开的页面即时同步
//   - 5001/5002 端口：纯 TCP 测速，电脑端用 tcp_speed.py
// 访问地址：http://esp32-led.local 或串口里打印的 IP 地址
// 如果 15 秒内连不上 WiFi，板子会自己开一个叫 ESP32-LED 的热点，连上后访问 http://192.168.4.1
//
// WebSocket 协议（ws://板子IP:81/，浏览器只允许从板子自己的网页连接，见 is_allowed_origin）：
//   发送：hex=ff6600&bright=128&on=1   （参数都可选，可任意组合）
//   发送：get                          （只查询状态）
//   接收：{"hex":"#ff6600","bright":128,"on":true,"clients":2}   （任何人改动都会广播给所有客户端）
//   接收：{"error":"..."}              （指令有误时，只发给发送者）
//
// 板子：ESP32-S3 N16R8（16MB 闪存 + 8MB OPI PSRAM），板载 WS2812 灯在 GPIO48

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/temperature_sensor.h"
#include "esp_app_format.h"
#include "esp_chip_info.h"
#include "esp_clk_tree.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "led_strip.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "nvs_flash.h"

#include "secrets.h"

#define RGB_LED_GPIO   48   // 和 Arduino 的 RGB_BUILTIN 一致
#define RGB_BRIGHTNESS 64   // 和 Arduino 的 RGB_BRIGHTNESS 一致
#define HOSTNAME       "esp32-led"
#define AP_SSID        "ESP32-LED"
#define WS_MAX_CLIENTS 5    // 和 Arduino WebSockets 库默认的最大客户端数一致

extern const char page_html_start[] asm("_binary_page_html_start");
extern const char page_html_end[] asm("_binary_page_html_end");

static httpd_handle_t http_server;  // 80 端口
static httpd_handle_t ws_server;    // 81 端口
static esp_netif_t *sta_netif, *ap_netif;
static bool ap_mode = false;
static led_strip_handle_t led;
static temperature_sensor_handle_t temp_sensor;
static uint32_t sketch_size;  // 程序镜像大小，开机算一次

// ---------- 灯的状态 ----------
// HTTP 和 WebSocket 是两个独立的服务器任务，都会改状态，所以要加锁

static SemaphoreHandle_t state_lock;
static uint8_t cur_r = 0, cur_g = 120, cur_b = 255;
static uint8_t brightness = RGB_BRIGHTNESS;
static bool led_on = true;

static void led_write(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(led, 0, r, g, b);
    led_strip_refresh(led);
}

static void apply_led(void)  // 调用前要持有 state_lock
{
    if (led_on) {
        led_write(cur_r * brightness / 255, cur_g * brightness / 255, cur_b * brightness / 255);
    } else {
        led_write(0, 0, 0);
    }
}

static int ws_client_count(void);

static void state_json(char *buf, size_t size)
{
    int clients = ws_client_count();
    xSemaphoreTake(state_lock, portMAX_DELAY);
    snprintf(buf, size, "{\"hex\":\"#%02x%02x%02x\",\"bright\":%d,\"on\":%s,\"clients\":%d}",
             cur_r, cur_g, cur_b, brightness, led_on ? "true" : "false", clients);
    xSemaphoreGive(state_lock);
}

// 校验并解析 6 位十六进制颜色，失败返回 false
static bool parse_hex(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (strlen(s) != 6) return false;
    for (const char *p = s; *p; p++) if (!isxdigit((unsigned char)*p)) return false;
    long v = strtol(s, NULL, 16);
    *r = v >> 16; *g = (v >> 8) & 0xFF; *b = v & 0xFF;
    return true;
}

// 检查并应用一组参数（空字符串表示没有这个参数）。
// 先全部检查，都合法才一起生效，避免改了一半。成功返回 NULL，失败返回原因。
static const char *apply_params(const char *hex, const char *bright, const char *on)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    uint8_t r = cur_r, g = cur_g, b = cur_b;
    int n = brightness;
    const char *err = NULL;

    if (hex[0] && !parse_hex(hex, &r, &g, &b)) { err = "hex 格式应为 RRGGBB"; goto out; }
    if (bright[0]) {
        for (const char *p = bright; *p; p++) {
            if (!isdigit((unsigned char)*p)) { err = "bright 应为 0~255 的整数"; goto out; }
        }
        n = strlen(bright) > 3 ? 256 : atoi(bright);
        if (n > 255) { err = "bright 范围 0~255"; goto out; }
    }
    if (on[0] && strcmp(on, "0") != 0 && strcmp(on, "1") != 0) { err = "on 应为 0 或 1"; goto out; }

    cur_r = r; cur_g = g; cur_b = b;
    brightness = n;
    if (on[0]) led_on = (strcmp(on, "1") == 0);
    apply_led();
out:
    xSemaphoreGive(state_lock);
    return err;
}

// ---------- 允许的来源（HTTP 和 WebSocket 共用） ----------
// 握手时检查 Origin，防止别的网站借你的浏览器偷偷连上来（跨站 WebSocket 劫持）。
// 允许的来源每次现场计算，所以 IP 变了也不用改代码：
//   http://板子当前的局域网IP、http://192.168.4.1（热点模式）、http://esp32-led.local

static bool origin_matches_netif(const char *origin, esp_netif_t *netif)
{
    esp_netif_ip_info_t info;
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK) return false;
    if (info.ip.addr == 0) return false;  // 没启用的网卡 IP 是 0.0.0.0，不能当成合法来源
    char want[32];
    snprintf(want, sizeof(want), "http://" IPSTR, IP2STR(&info.ip));
    return strcmp(origin, want) == 0;
}

static bool is_allowed_origin(const char *raw)
{
    // 去掉首尾空白
    while (isspace((unsigned char)*raw)) raw++;
    char origin[96];
    strlcpy(origin, raw, sizeof(origin));
    for (int i = strlen(origin) - 1; i >= 0 && isspace((unsigned char)origin[i]); i--) origin[i] = 0;

    if (strcasecmp(origin, "http://" HOSTNAME ".local") == 0) return true;
    return origin_matches_netif(origin, sta_netif) || origin_matches_netif(origin, ap_netif);
}

// 读取请求的 Origin 头。没有返回 false。
static bool get_origin(httpd_req_t *req, char *buf, size_t size)
{
    size_t len = httpd_req_get_hdr_value_len(req, "Origin");
    if (len == 0) return false;
    if (len >= size) { buf[0] = 0; return true; }  // 太长的肯定不合法
    httpd_req_get_hdr_value_str(req, "Origin", buf, size);
    return true;
}

// ---------- WebSocket（81 端口） ----------

// 心跳：每 15 秒 ping 一次，连续 2 次没回应就断开，清理掉线的客户端
static struct { int fd; uint8_t missed; } ws_peers[WS_MAX_CLIENTS + 2];

static int ws_client_count(void)
{
    if (!ws_server) return 0;
    size_t n = WS_MAX_CLIENTS;
    int fds[WS_MAX_CLIENTS];
    if (httpd_get_client_list(ws_server, &n, fds) != ESP_OK) return 0;
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        if (httpd_ws_get_fd_info(ws_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) count++;
    }
    return count;
}

static void ws_send_text(int fd, const char *text)
{
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len = strlen(text),
    };
    httpd_ws_send_frame_async(ws_server, fd, &frame);
}

// 在 WebSocket 服务器自己的任务里执行，避免两个任务同时往同一个 socket 写
static void broadcast_work(void *arg)
{
    char s[128];
    state_json(s, sizeof(s));
    size_t n = WS_MAX_CLIENTS;
    int fds[WS_MAX_CLIENTS];
    if (httpd_get_client_list(ws_server, &n, fds) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            if (httpd_ws_get_fd_info(ws_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                ws_send_text(fds[i], s);
            }
        }
    }
    printf("状态 -> %s\n", s);
}

// 状态变化后推送给所有 WebSocket 客户端
static void broadcast_state(void)
{
    if (ws_server) httpd_queue_work(ws_server, broadcast_work, NULL);
}

static void heartbeat_work(void *arg)
{
    for (int i = 0; i < sizeof(ws_peers) / sizeof(ws_peers[0]); i++) {
        int fd = ws_peers[i].fd;
        if (fd < 0 || httpd_ws_get_fd_info(ws_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
        if (ws_peers[i].missed >= 2) {
            printf("WebSocket 客户端 #%d 心跳超时，断开\n", fd);
            httpd_sess_trigger_close(ws_server, fd);
            continue;
        }
        httpd_ws_frame_t ping = { .type = HTTPD_WS_TYPE_PING };
        httpd_ws_send_frame_async(ws_server, fd, &ping);
        ws_peers[i].missed++;
    }
}

static void heartbeat_timer_cb(void *arg)
{
    httpd_queue_work(ws_server, heartbeat_work, NULL);
}

// 握手前检查 Origin。没有 Origin 的连接（脚本、命令行工具，不是浏览器）放行，因为这项检查只针对浏览器。
static esp_err_t ws_pre_handshake(httpd_req_t *req)
{
    char origin[96];
    if (get_origin(req, origin, sizeof(origin)) && !is_allowed_origin(origin)) {
        printf("拒绝 WebSocket 连接：Origin %s 不在允许列表\n", origin);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Origin not allowed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ws_post_handshake(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    for (int i = 0; i < sizeof(ws_peers) / sizeof(ws_peers[0]); i++) {
        if (ws_peers[i].fd < 0) { ws_peers[i].fd = fd; ws_peers[i].missed = 0; break; }
    }
    struct sockaddr_in6 addr;
    socklen_t len = sizeof(addr);
    char ip[48] = "?";
    if (getpeername(fd, (struct sockaddr *)&addr, &len) == 0) {
        if (addr.sin6_family == AF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&addr)->sin_addr, ip, sizeof(ip));
        } else {
            inet6_ntoa_r(addr.sin6_addr, ip, sizeof(ip));
        }
    }
    printf("WebSocket 客户端 #%d 已连接（%s）\n", fd, ip);
    broadcast_state();  // 在线页面数变了，通知大家
    return ESP_OK;
}

// 服务器关闭任何连接时调用（包括 WebSocket 断开）
static void ws_close_fn(httpd_handle_t hd, int fd)
{
    for (int i = 0; i < sizeof(ws_peers) / sizeof(ws_peers[0]); i++) {
        if (ws_peers[i].fd == fd) {
            ws_peers[i].fd = -1;
            printf("WebSocket 客户端 #%d 已断开\n", fd);
            broadcast_state();  // 排队执行，到时这个连接已经从列表里删掉了
        }
    }
    close(fd);
}

// 从 "hex=ff6600&bright=128" 这样的字符串里取出某个参数的值，没有就返回空
static void get_param(const char *msg, const char *key, char *out, size_t size)
{
    size_t klen = strlen(key);
    out[0] = 0;
    const char *p = msg;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t vlen = end - p - klen - 1;
            if (vlen >= size) vlen = size - 1;
            memcpy(out, p + klen + 1, vlen);
            out[vlen] = 0;
            return;
        }
        if (!*end) break;
        p = end + 1;
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    httpd_ws_frame_t frame = { 0 };
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);  // 先只读帧头，拿到长度
    if (ret != ESP_OK) return ret;
    if (frame.len > 1024) return ESP_FAIL;  // 太大的帧直接断开

    uint8_t buf[1025];
    frame.payload = buf;
    if (frame.len) {
        ret = httpd_ws_recv_frame(req, &frame, sizeof(buf) - 1);
        if (ret != ESP_OK) return ret;
    }
    buf[frame.len] = 0;

    if (frame.type == HTTPD_WS_TYPE_PONG) {
        for (int i = 0; i < sizeof(ws_peers) / sizeof(ws_peers[0]); i++) {
            if (ws_peers[i].fd == fd) ws_peers[i].missed = 0;
        }
        return ESP_OK;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;

    if (frame.len > 100) { ws_send_text(fd, "{\"error\":\"消息太长\"}"); return ESP_OK; }

    // 去掉首尾空白
    char *msg = (char *)buf;
    while (isspace((unsigned char)*msg)) msg++;
    for (int i = strlen(msg) - 1; i >= 0 && isspace((unsigned char)msg[i]); i--) msg[i] = 0;

    if (strcmp(msg, "get") == 0) {
        char s[128];
        state_json(s, sizeof(s));
        ws_send_text(fd, s);
        return ESP_OK;
    }

    char hex[16], bright[16], on[8];
    get_param(msg, "hex", hex, sizeof(hex));
    get_param(msg, "bright", bright, sizeof(bright));
    get_param(msg, "on", on, sizeof(on));
    if (!hex[0] && !bright[0] && !on[0]) {
        ws_send_text(fd, "{\"error\":\"未知指令\"}");
        return ESP_OK;
    }
    const char *err = apply_params(hex, bright, on);
    if (err) {
        char e[96];
        snprintf(e, sizeof(e), "{\"error\":\"%s\"}", err);
        ws_send_text(fd, e);
        return ESP_OK;
    }
    broadcast_state();
    return ESP_OK;
}

static void start_ws_server(void)
{
    for (int i = 0; i < sizeof(ws_peers) / sizeof(ws_peers[0]); i++) ws_peers[i].fd = -1;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 81;
    cfg.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;
    cfg.max_open_sockets = WS_MAX_CLIENTS;
    cfg.close_fn = ws_close_fn;
    cfg.stack_size = 6144;
    ESP_ERROR_CHECK(httpd_start(&ws_server, &cfg));

    httpd_uri_t ws = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
        .ws_pre_handshake_cb = ws_pre_handshake,
        .ws_post_handshake_cb = ws_post_handshake,
    };
    httpd_register_uri_handler(ws_server, &ws);

    const esp_timer_create_args_t targs = { .callback = heartbeat_timer_cb, .name = "ws_heartbeat" };
    esp_timer_handle_t t;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &t));
    ESP_ERROR_CHECK(esp_timer_start_periodic(t, 15 * 1000000));
}

// ---------- HTTP（80 端口） ----------

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page_html_start, page_html_end - page_html_start);
}

static esp_err_t handle_state(httpd_req_t *req)
{
    char s[128];
    state_json(s, sizeof(s));
    return send_json(req, "200 OK", s);
}

// 往 JSON 缓冲区里追加内容
typedef struct { char *buf; size_t size, len; } jbuf_t;

static void jcat(jbuf_t *j, const char *fmt, ...)
{
    if (j->len >= j->size) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->len, j->size - j->len, fmt, ap);
    va_end(ap);
    if (n > 0) j->len += n;
}

// 把字符串转成 JSON 字符串（处理引号、反斜杠和控制字符），WiFi 名称里可能有这些字符
static void jstr(jbuf_t *j, const char *s)
{
    jcat(j, "\"");
    for (; *s; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') jcat(j, "\\%c", c);
        else if (c < 0x20) jcat(j, "\\u%04x", c);
        else jcat(j, "%c", c);
    }
    jcat(j, "\"");
}

static const char *reset_reason_text(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "上电";
    case ESP_RST_SW:        return "软件重启";
    case ESP_RST_PANIC:     return "程序崩溃";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "看门狗超时";
    case ESP_RST_BROWNOUT:  return "电压不足";
    case ESP_RST_USB:       return "USB 复位";
    case ESP_RST_DEEPSLEEP: return "深度睡眠唤醒";
    case ESP_RST_EXT:       return "外部复位";
    default:                return "其他";
    }
}

static void ip_of(esp_netif_t *netif, char *out, size_t size)
{
    esp_netif_ip_info_t info = { 0 };
    if (netif) esp_netif_get_ip_info(netif, &info);
    snprintf(out, size, IPSTR, IP2STR(&info.ip));
}

// GET /sys：查看板子运行时的资源占用情况
static esp_err_t handle_sys(httpd_req_t *req)
{
    static char buf[1536];
    jbuf_t j = { buf, sizeof(buf), 0 };

    jcat(&j, "{\"uptime_s\":%lld", esp_timer_get_time() / 1000000);
    jcat(&j, ",\"reset_reason\":"); jstr(&j, reset_reason_text());

    // 芯片
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t cpu_hz = 0;
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &cpu_hz);
    float temp = 0;
    if (temp_sensor) temperature_sensor_get_celsius(temp_sensor, &temp);
    jcat(&j, ",\"chip\":{\"model\":"); jstr(&j, chip.model == CHIP_ESP32S3 ? "ESP32-S3" : CONFIG_IDF_TARGET);
    jcat(&j, ",\"revision\":%d,\"cores\":%d,\"cpu_mhz\":%lu,\"temp_c\":%.1f",
         chip.revision, chip.cores, (unsigned long)(cpu_hz / 1000000), temp);
    jcat(&j, ",\"sdk\":"); jstr(&j, esp_get_idf_version()); jcat(&j, "}");

    // 内部 RAM（堆）：free=现在剩余，min_free=开机以来最少的时候，max_block=能一次申请到的最大连续块
    jcat(&j, ",\"heap\":{\"total\":%u,\"free\":%u,\"min_free\":%u,\"max_block\":%u}",
         heap_caps_get_total_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    // 外扩 PSRAM
    jcat(&j, ",\"psram\":{\"total\":%u,\"free\":%u,\"min_free\":%u}",
         heap_caps_get_total_size(MALLOC_CAP_SPIRAM), heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
         heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));

    // 闪存
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    jcat(&j, ",\"flash\":{\"chip_size\":%lu,\"sketch_size\":%lu,\"app_partition\":%lu,\"speed_mhz\":%d}",
         (unsigned long)flash_size, (unsigned long)sketch_size,
         (unsigned long)esp_ota_get_running_partition()->size,  // 当前程序所在分区的大小
         atoi(CONFIG_ESPTOOLPY_FLASHFREQ));

    // 网络
    char ip[16];
    jcat(&j, ",\"wifi\":{\"mode\":"); jstr(&j, ap_mode ? "AP" : "STA");
    if (!ap_mode) {
        wifi_ap_record_t ap = { 0 };
        esp_wifi_sta_get_ap_info(&ap);
        ip_of(sta_netif, ip, sizeof(ip));
        jcat(&j, ",\"ssid\":"); jstr(&j, (const char *)ap.ssid);
        jcat(&j, ",\"rssi\":%d,\"channel\":%d", ap.rssi, ap.primary);
        jcat(&j, ",\"ip\":"); jstr(&j, ip);
    } else {
        wifi_sta_list_t list = { 0 };
        esp_wifi_ap_get_sta_list(&list);
        ip_of(ap_netif, ip, sizeof(ip));
        jcat(&j, ",\"ip\":"); jstr(&j, ip);
        jcat(&j, ",\"stations\":%d", list.num);
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macs[18];
    snprintf(macs, sizeof(macs), "%02X:%02X:%02X:%02X:%02X:%02X", MAC2STR(mac));
    jcat(&j, ",\"mac\":"); jstr(&j, macs); jcat(&j, "}");

    jcat(&j, ",\"ws_clients\":%d}", ws_client_count());
    return send_json(req, "200 OK", buf);
}

// 从 "a=1&b=2" 格式的字符串里取参数，没有就是空字符串
static void form_arg(const char *query, const char *body, const char *key, char *out, size_t size)
{
    out[0] = 0;
    if (query && httpd_query_key_value(query, key, out, size) == ESP_OK && out[0]) return;
    out[0] = 0;
    if (body && httpd_query_key_value(body, key, out, size) == ESP_OK) return;
    out[0] = 0;
}

// POST /set，参数 hex=RRGGBB&bright=0~255&on=0|1 都可选，放在请求体（表单格式）或网址里都行。
// 只接受 POST：GET 请求可以被别的网页用 <img> 之类的标签悄悄触发（CSRF）。
// 浏览器发 POST 时一定会带 Origin，不是板子自己的网页发来的就拒绝。
static esp_err_t handle_set(httpd_req_t *req)
{
    char origin[96];
    if (get_origin(req, origin, sizeof(origin)) && !is_allowed_origin(origin)) {
        printf("拒绝 /set：Origin %s 不在允许列表\n", origin);
        return send_json(req, "403 Forbidden", "{\"error\":\"来源不被允许\"}");
    }

    char body[257] = "";
    if (req->content_len >= sizeof(body)) return send_json(req, "400 Bad Request", "{\"error\":\"请求体太长\"}");
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) return ESP_FAIL;
        got += n;
    }
    body[got] = 0;

    char query[160] = "";
    bool has_query = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK;

    char hex[16], bright[16], on[8];
    form_arg(has_query ? query : NULL, body, "hex", hex, sizeof(hex));
    form_arg(has_query ? query : NULL, body, "bright", bright, sizeof(bright));
    form_arg(has_query ? query : NULL, body, "on", on, sizeof(on));

    const char *err = apply_params(hex, bright, on);
    if (err) {
        char e[96];
        snprintf(e, sizeof(e), "{\"error\":\"%s\"}", err);
        return send_json(req, "400 Bad Request", e);
    }
    broadcast_state();  // HTTP 改的也通知网页
    return handle_state(req);
}

static esp_err_t handle_set_get(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Allow", "POST");
    return send_json(req, "405 Method Not Allowed", "{\"error\":\"请用 POST\"}");
}

// ---------- 网页测速 ----------
// HTTP 服务器一次只处理一个请求，测速期间板子不响应别的网页请求，灯控会卡几秒
// （WebSocket 在 81 端口的独立任务里，不受影响）。

static uint8_t speed_buf[8192];  // 内容无所谓，全是 0。只有 80 端口的服务器任务用

// GET /speed/down?mb=1~32：发送 mb 兆字节的无用数据，浏览器统计接收速度（下行：板子→浏览器）
static esp_err_t handle_speed_down(httpd_req_t *req)
{
    char query[32], v[8];
    long mb = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "mb", v, sizeof(v)) == ESP_OK) {
        mb = atol(v);
    }
    if (mb < 1 || mb > 32) return send_json(req, "400 Bad Request", "{\"error\":\"mb 范围 1~32\"}");

    size_t total = (size_t)mb * 1048576, sent = 0;
    // 自己写响应头，这样能带 Content-Length，不用分块传输
    char hdr[160];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                        "Content-Length: %u\r\nCache-Control: no-store\r\n\r\n", (unsigned)total);
    if (httpd_send(req, hdr, hlen) != hlen) return ESP_FAIL;

    // 关闭 Nagle 算法：否则发送缓冲区末尾不满一个包的数据要等对方 ACK 才发，
    // 而 Windows 会延迟 ACK，两者叠加后速度会很慢
    int one = 1;
    setsockopt(httpd_req_to_sockfd(req), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    int64_t t0 = esp_timer_get_time();
    while (sent < total) {
        size_t chunk = total - sent < sizeof(speed_buf) ? total - sent : sizeof(speed_buf);
        int n = httpd_send(req, (const char *)speed_buf, chunk);
        if (n <= 0) break;  // 发送超时或连接断开
        sent += n;
    }
    printf("测速下行：发送 %u 字节，用时 %lld ms\n", (unsigned)sent, (esp_timer_get_time() - t0) / 1000);
    return sent == total ? ESP_OK : ESP_FAIL;  // 没发完就断开连接
}

// POST /speed/up：请求体是任意数据，边收边丢弃，只统计字节数和用时（上行：浏览器→板子）
static esp_err_t handle_speed_up(httpd_req_t *req)
{
    size_t total = 0;
    int64_t t0 = esp_timer_get_time();
    while (total < req->content_len) {
        size_t want = req->content_len - total;
        int n = httpd_req_recv(req, (char *)speed_buf, want < sizeof(speed_buf) ? want : sizeof(speed_buf));
        if (n <= 0) return ESP_FAIL;  // 超时或断开
        total += n;
    }
    int64_t us = esp_timer_get_time() - t0;
    printf("测速上行：收到 %u 字节，用时 %lld ms\n", (unsigned)total, us / 1000);
    char s[64];
    snprintf(s, sizeof(s), "{\"bytes\":%u,\"us\":%lld}", (unsigned)total, us);
    return send_json(req, "200 OK", s);
}

static esp_err_t handle_404(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Not found");
    return ESP_OK;
}

static void start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 10;
    cfg.lru_purge_enable = true;  // 连接数满了就关掉最久没用的，浏览器常常一直占着空闲连接
    cfg.stack_size = 8192;
    ESP_ERROR_CHECK(httpd_start(&http_server, &cfg));

    const httpd_uri_t uris[] = {
        { .uri = "/",           .method = HTTP_ANY,  .handler = handle_root },
        { .uri = "/state",      .method = HTTP_ANY,  .handler = handle_state },
        { .uri = "/sys",        .method = HTTP_GET,  .handler = handle_sys },
        { .uri = "/speed/down", .method = HTTP_GET,  .handler = handle_speed_down },
        { .uri = "/speed/up",   .method = HTTP_POST, .handler = handle_speed_up },
        { .uri = "/set",        .method = HTTP_POST, .handler = handle_set },
        { .uri = "/set",        .method = HTTP_GET,  .handler = handle_set_get },
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(http_server, &uris[i]);
    httpd_register_err_handler(http_server, HTTPD_404_NOT_FOUND, handle_404);
}

// ---------- 纯 TCP 测速（不经过 HTTP 服务器，用来和网页测速对比） ----------
// 直接用 lwIP 的 socket 接口，每个端口一个独立任务，测速时网页和灯控照常响应。
// 电脑端用工程目录下的 tcp_speed.py 测，也可以用 iperf2：iperf -c 板子IP -p 5001
//   5001 端口（上行，电脑→板子）：一直收、丢弃，对方关闭发送后回一行 "字节数 微秒数\n"
//   5002 端口（下行，板子→电脑）：连上就一直发，对方断开或满 30 秒停止
// 同一个端口一次只服务一个连接。

static int tcp_listen(uint16_t port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(s, 1) < 0) { close(s); return -1; }
    return s;
}

static void tcp_sink_task(void *arg)
{
    int ls = tcp_listen(5001);
    if (ls < 0) { printf("TCP 测速：5001 端口监听失败\n"); vTaskDelete(NULL); }
    static uint8_t buf[8192];
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        struct timeval tv = { 10, 0 };  // 10 秒没数据就当对方掉线
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        size_t total = 0;
        int64_t t0 = 0, t1 = 0;
        for (;;) {
            int n = recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;  // 0 = 对方关闭了发送，<0 = 出错或超时
            int64_t now = esp_timer_get_time();
            if (!t0) t0 = now;  // 从第一块数据开始计时
            else total += n;    // 第一块不计入，和计时起点对齐
            t1 = now;
        }
        char reply[48];
        int len = snprintf(reply, sizeof(reply), "%u %lld\n", (unsigned)total, (long long)(t1 - t0));
        send(c, reply, len, 0);
        close(c);
        printf("TCP 上行：%u 字节，%lld ms，%.1f Mbps\n", (unsigned)total, (t1 - t0) / 1000,
               t1 > t0 ? total * 8.0 / (t1 - t0) : 0.0);
    }
}

static void tcp_source_task(void *arg)
{
    int ls = tcp_listen(5002);
    if (ls < 0) { printf("TCP 测速：5002 端口监听失败\n"); vTaskDelete(NULL); }
    static uint8_t buf[8192];  // 内容无所谓，全是 0
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        struct timeval tv = { 5, 0 };
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        size_t total = 0;
        int64_t t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < 30000000) {
            int n = send(c, buf, sizeof(buf), 0);
            if (n <= 0) break;  // 对方断开
            total += n;
        }
        int64_t us = esp_timer_get_time() - t0;
        close(c);
        printf("TCP 下行：发出 %u 字节，%lld ms（实际速度以电脑端统计为准）\n", (unsigned)total, us / 1000);
    }
}

// ---------- WiFi ----------

#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t wifi_events;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        if (!ap_mode) esp_wifi_connect();  // 掉线自动重连
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(sta_netif, HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
}

static bool connect_wifi(void)
{
    if (strncmp(WIFI_SSID, "在这里", strlen("在这里")) == 0) {
        printf("还没在 secrets.h 里填 WiFi 名称和密码\n");
        return false;
    }
    printf("正在连接 WiFi：%s ", WIFI_SSID);
    fflush(stdout);

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  // 关闭 WiFi 省电休眠，响应更快（代价是多耗一点电）

    // 等待连接时灯慢闪黄色
    int64_t start = esp_timer_get_time();
    while (!(xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) &&
           esp_timer_get_time() - start < 15000000) {
        bool blink = (esp_timer_get_time() / 300000) % 2;
        led_write(blink ? 40 : 0, blink ? 30 : 0, 0);
        printf(".");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    printf("\n");
    return xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT;
}

static void start_ap(void)
{
    ap_mode = true;  // 先置位，避免断开事件触发重连
    esp_wifi_stop();
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.ap.ssid, AP_SSID, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = strlen(AP_SSID);
    wc.ap.channel = 1;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static int sta_rssi(void)
{
    wifi_ap_record_t ap = { 0 };
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

// ---------- 启动 ----------

static void hw_init(void)
{
    state_lock = xSemaphoreCreateMutex();

    led_strip_config_t lc = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000 };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&lc, &rc, &led));
    led_strip_clear(led);

    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tc, &temp_sensor) == ESP_OK) {
        temperature_sensor_enable(temp_sensor);
    } else {
        temp_sensor = NULL;
    }

    // 算出程序镜像的实际大小（对应 Arduino 的 ESP.getSketchSize）
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_partition_pos_t pos = { .offset = running->address, .size = running->size };
    esp_image_metadata_t meta;
    if (esp_image_get_metadata(&pos, &meta) == ESP_OK) sketch_size = meta.image_len;
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1500));  // 给串口监视器一点时间连上
    printf("实验 6：网页遥控灯（WebSocket 版）\n");

    hw_init();
    wifi_init();

    if (connect_wifi()) {
        char ip[16];
        ip_of(sta_netif, ip, sizeof(ip));
        printf("WiFi 已连接，信号 %d dBm\n", sta_rssi());
        printf("用浏览器打开：http://%s\n", ip);
        if (mdns_init() == ESP_OK && mdns_hostname_set(HOSTNAME) == ESP_OK) {
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
            mdns_service_add(NULL, "_ws", "_tcp", 81, NULL, 0);
            printf("或者打开：http://" HOSTNAME ".local\n");
        }
    } else {
        printf("连不上 WiFi，改为开热点模式\n");
        start_ap();
        printf("手机连接 WiFi 热点 " AP_SSID "，然后打开：http://192.168.4.1\n");
    }

    start_ws_server();
    start_http_server();

    // 纯 TCP 测速任务，放在核心 1（WiFi 协议栈默认在核心 0），优先级比主任务稍高
    xTaskCreatePinnedToCore(tcp_sink_task, "tcp_sink", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(tcp_source_task, "tcp_source", 4096, NULL, 2, NULL, 1);

    xSemaphoreTake(state_lock, portMAX_DELAY);
    apply_led();
    xSemaphoreGive(state_lock);

    // 每 5 秒打印一次地址，方便打开串口监视器时随时看到
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!ap_mode) {
            char ip[16];
            ip_of(sta_netif, ip, sizeof(ip));
            printf("[在线] http://%s  ws://%s:81  信号 %d dBm  WS 客户端 %d\n",
                   ip, ip, sta_rssi(), ws_client_count());
        } else {
            printf("[热点模式] 连接 " AP_SSID " 后打开 http://192.168.4.1\n");
        }
    }
}
