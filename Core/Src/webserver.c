#include "device_socket.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include <string.h>
#include <stdio.h>
#include "stm32f4xx_hal_gpio.h"
#include <stdlib.h>
#include "ee_config.h"

#define PORT     80
#define BUF_SIZE 1024

/* ─────────────────────────────────────────────
 * LED硬件操作（最底层，不对外暴露）
 * ───────────────────────────────────────────── */
static void led_hw_on(void){ HAL_GPIO_WritePin(GPIOF, GPIO_PIN_10, GPIO_PIN_RESET); }
static void led_hw_off(void)    { HAL_GPIO_WritePin(GPIOF, GPIO_PIN_10, GPIO_PIN_SET);}
static void led_hw_toggle(void) { HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_10);                }

/* ─────────────────────────────────────────────
 * LED 业务接口（读写 Flash + g_config）
 * ───────────────────────────────────────────── */
int led_get(void) { return g_config.led_state; }

void led_set(int on)
{
    uint16_t val = on ? 1 : 0;
    if (g_config.led_state == val) return;
    EE_WriteVar(EE_ID_LED_STATE, val);
    printf("[LED] state -> %d\r\n", g_config.led_state);
}

void led_freq_set(int freq_ms)
{
    if (freq_ms < 100)freq_ms = 100;
    if (freq_ms > 5000) freq_ms = 5000;
    uint16_t val = (uint16_t)freq_ms;
    if (g_config.led_freq == val) return;
    EE_WriteVar(EE_ID_LED_FREQ, val);
    printf("[LED] freq -> %d ms\r\n", g_config.led_freq);
}

/* ─────────────────────────────────────────────
 * LED Blink 任务
 * ───────────────────────────────────────────── */
static void led_blink_task(void *arg)
{
    (void)arg;
    uint32_t elapsed   = 0;
    uint16_t last_mode = 0xFF;

    while (1) {
        uint16_t mode = g_config.led_state;
        uint16_t freq = g_config.led_freq;
        if (freq< 100) freq = 100;

        if (mode != last_mode) {
            last_mode = mode;
            elapsed   = 0;
            mode ? led_hw_on() : led_hw_off();
        }

        if (mode == 1) {
            elapsed += 20;
            if (elapsed >= freq) {
                elapsed = 0;
                led_hw_toggle();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ─────────────────────────────────────────────
 * HTTP 数据类型
 * ───────────────────────────────────────────── */
typedef struct {
    char        method[8];
    char        path[64];
    char        params_buf[128];
    const char *params;          /* NULL =无参数 */
} http_req_t;

/* ─────────────────────────────────────────────
 * 层1：I/O — recv_request
 * 职责：从 socket 读完整 HTTP 报文（头+ body），不做任何解析
 * ───────────────────────────────────────────── */
static int recv_request(int fd, char *buf, int buf_size)
{
    int total = 0;

    /* 读头部，直到遇到空行 \r\n\r\n */
    while (total < buf_size - 1) {
        int n = recv(fd, buf + total, buf_size - 1 - total, 0);
        if (n > 0) {
            total += n;
            buf[total] = '\0';if (strstr(buf, "\r\n\r\n")) break;
        } else {
            break;
        }
    }

    /* 若是POST，按 Content-Length 继续读 body */
    char *cl_ptr = strstr(buf, "Content-Length:");
    if (cl_ptr) {
        int need = atoi(cl_ptr + 15);
        char *body_start = strstr(buf, "\r\n\r\n");
        if (body_start && need > 0) {
            body_start += 4;
            int have= (int)(buf + total - body_start);
            int remain = need - have;
            while (remain > 0 && total < buf_size - 1) {
                int n = recv(fd, buf + total, remain, 0);
                if (n > 0) {
                    total  += n;
                    buf[total] = '\0';
                    remain -= n;
                } else {
                    break;
                }
            }
        }
    }

    return total;
}

/* ─────────────────────────────────────────────
 * 层2：解析 — http_parse / param_get
 * 职责：把原始字节变成结构化的 http_req_t，不涉及业务
 * ───────────────────────────────────────────── */
int param_get(const char *params, const char *key, char *out, int out_len)
{
    if (!params || !key || !out || out_len <= 0) return -1;

    int key_len = (int)strlen(key);
    const char *p = params;

    while (*p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *val = p + key_len + 1;
            int i = 0;
            while (val[i] && val[i] != '&' && i < out_len - 1) {
                out[i] = val[i];
                i++;
            }
            out[i] = '\0';
            return 0;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return -1;
}

int http_parse(const char *raw, int raw_len, http_req_t *req)
{
    const char *p = raw;
    int i;

    /* method */
    i = 0;
    while (*p && *p != ' ' && i < (int)sizeof(req->method) - 1)
        req->method[i++] = *p++;
    req->method[i] = '\0';
    if (*p != ' ') return -1;
    p++;

    /* path（停在 '?' 或 ' '） */
    const char *path_start = p;
    while (*p && *p != ' ' && *p != '?') p++;
    int path_len = (int)(p - path_start);
    if (path_len >= (int)sizeof(req->path))
        path_len = (int)sizeof(req->path) - 1;
    memcpy(req->path, path_start, path_len);
    req->path[path_len] = '\0';

    /* 参数：GET在 URL，POST 在 body */
    const char *src= NULL;
    int         src_len = 0;

    if (strcmp(req->method, "GET") == 0 && *p == '?') {
        src = p + 1;
        const char *end = strchr(src, ' ');
        src_len = end ? (int)(end - src) : (int)(raw_len - (int)(src - raw));
    } else if (strcmp(req->method, "POST") == 0) {
        const char *body = strstr(raw, "\r\n\r\n");
        if (body) {
            src     = body + 4;
            src_len = raw_len - (int)(src - raw);
        }
    }

    if (src && src_len > 0) {
        if (src_len >= (int)sizeof(req->params_buf))
            src_len = (int)sizeof(req->params_buf) - 1;
        memcpy(req->params_buf, src, src_len);
        req->params_buf[src_len] = '\0';
        req->params = req->params_buf;
    } else {
        req->params_buf[0] = '\0';
        req->params = NULL;
    }

    return 0;
}

/* ─────────────────────────────────────────────
 * 层3：响应工具
 * ───────────────────────────────────────────── */
static void send_response(int sock, const char *content_type, const char *body)
{
    char header[256];
    int body_len = (int)strlen(body);
    snprintf(header, sizeof(header),
             "HTTP/1.0 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %d\r\n"
             "Cache-Control: no-store\r\n"
             "Connection: close\r\n"
             "\r\n",
             content_type, body_len);
    send(sock, header, (int)strlen(header), 0);
    send(sock, body,body_len,0);
}

static void send_json_ok(int client, int led, int freq)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"led\":%d,\"freq\":%d}", led, freq);
    send_response(client, "application/json; charset=utf-8", json);
}

static void send_json_err(int client, const char *msg)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"%s\"}", msg);
    send_response(client, "application/json; charset=utf-8", json);
}

/* ─────────────────────────────────────────────
 * 层4：路由处理函数
 * 职责：只关心业务逻辑，完全不知道 socket 细节
 * ───────────────────────────────────────────── */
static const char HTML_CONTROL[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>LED</title></head><body>"
    "<h3>LED 控制</h3>"
    "<p>当前LED: <span id='show_led'>--</span></p>"
    "<p>当前频率: <span id='show_freq'>--</span> ms</p>"
    "LED: <select id='led'><option value='1'>ON</option><option value='0'>OFF</option></select> "
    "频率: <input id='freq' type='number' value='500' min='100' max='5000' step='100'> ms "
    "<button onclick='sendCtrl()'>设置</button>"
    "<script>"
    "function show(d){"
    "  document.getElementById('show_led').innerHTML=d.led?'ON':'OFF';"
    "  document.getElementById('show_freq').innerHTML=d.freq;"
    "  document.getElementById('led').value=d.led;"
    "  document.getElementById('freq').value=d.freq;"
    "}"
    "function loadStatus(){fetch('/status').then(function(r){return r.json();}).then(show);}"
    "function sendCtrl(){"
    "  var body='led='+document.getElementById('led').value"
    "          +'&freq='+document.getElementById('freq').value;"
    "  fetch('/control',{method:'POST',"
    "    headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
    "  .then(function(r){return r.json();})"
    "  .then(function(d){show(d);});"   /* 直接刷新显示，不再alert */
    "}"
    "loadStatus(); setInterval(loadStatus,10000);"
    "</script></body></html>";

static void handle_root(int client, const http_req_t *req)
{
    (void)req;
    send_response(client, "text/html; charset=utf-8", HTML_CONTROL);
}

static void handle_status(int client, const http_req_t *req)
{
    (void)req;
    send_json_ok(client, g_config.led_state, g_config.led_freq);
}

static void handle_control(int client, const http_req_t *req)
{
    char led_str[8]  = "0";
    char freq_str[16] = "500";

    param_get(req->params, "led",led_str,  sizeof(led_str));
    param_get(req->params, "freq", freq_str, sizeof(freq_str));

    int led  = atoi(led_str);
    int freq = atoi(freq_str);

    led_freq_set(freq);   /* 写Flash，限幅在函数内 */
    led_set(led);         /* 写 Flash */

    printf("[CTRL] LED=%d FREQ=%d ms\r\n", led, g_config.led_freq);send_json_ok(client, g_config.led_state, g_config.led_freq);
}

/* ─────────────────────────────────────────────
 * 路由表：增加路由只改这里
 * ───────────────────────────────────────────── */
typedef void (*http_handler_t)(int client, const http_req_t *req);

typedef struct {
    const char     *method;
    const char     *path;
    http_handler_t  handler;
} route_t;

static const route_t s_routes[] = {
    { "GET",  "/",        handle_root    },
    { "GET",  "/status",  handle_status  },
    { "POST", "/control", handle_control },
};
#define ROUTE_COUNT  (sizeof(s_routes) / sizeof(s_routes[0]))

static void http_dispatch(int client, const http_req_t *req)
{
    for (int i = 0; i < (int)ROUTE_COUNT; i++) {
        if (strcmp(req->method, s_routes[i].method) == 0 &&
            strcmp(req->path,   s_routes[i].path)   == 0) {
            s_routes[i].handler(client, req);
            return;
        }
    }
    send_json_err(client, "not found");
}

/* ─────────────────────────────────────────────
 * 处理单次连接：recv → parse → dispatch
 * 这是四层之间的"胶水"，本身不含业务
 * ───────────────────────────────────────────── */
static uint8_t s_recv_buf[BUF_SIZE];  /* 静态，避免占用任务栈 */

static void http_serve_one(int client)
{
    int len = recv_request(client, (char *)s_recv_buf, sizeof(s_recv_buf) - 1);
    if (len <= 0) return;
    s_recv_buf[len] = '\0';

    printf("----- HTTP REQ -----\r\n%s\r\n--------------------\r\n", s_recv_buf);

    http_req_t req;
    memset(&req, 0, sizeof(req));
    if (http_parse((const char *)s_recv_buf, len, &req) != 0) {
        send_json_err(client, "bad request");
        return;
    }

    printf("[REQ] %s %s params=%s\r\n",
           req.method, req.path, req.params ? req.params : "NULL");

    http_dispatch(client, &req);
}

/* ─────────────────────────────────────────────
 * webserver_task：只负责 accept 循环
 * ───────────────────────────────────────────── */
static int setup_server_socket(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { printf("[HTTP] socket failed\r\n"); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
//    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("[HTTP] bind failed\r\n");
        closesocket(sock);
        return -1;
    }
    if (listen(sock, 4) != 0) {
        printf("[HTTP] listen failed\r\n");
        closesocket(sock);
        return -1;
    }
    printf("[HTTP] listening on port %d\r\n", port);
    return sock;
}

void webserver_task(void *arg)
{
    (void)arg;
    at_init("stm32_f4_uart2");
    at_connect_ap("Tenda", "wxw123456");

    int server = setup_server_socket(PORT);
    if (server < 0) { vTaskDelete(NULL); return; }

    uint32_t tmo = 3000;

    while (1) {
        int client = accept(server, NULL, NULL);
        if (client < 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

        setsockopt(client, 0, SO_RCVTIMEO, &tmo, sizeof(tmo));
        http_serve_one(client);
        closesocket(client);
    }
}

/* ─────────────────────────────────────────────
 * 入口
 * ───────────────────────────────────────────── */
void start_webserver(void)
{
    EE_Init();
    g_config.led_state ? led_hw_on() : led_hw_off();

    xTaskCreate(led_blink_task, "led_blink", 256, NULL, osPriorityNormal, NULL);
    xTaskCreate(webserver_task, "http_srv",512, NULL, osPriorityNormal, NULL);
}
