#include "device_socket.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include <string.h>
#include <stdio.h>
#include "stm32f4xx_hal_gpio.h"
#include <stdlib.h>
#include "ee_config.h"

#define PORT 80
#define BUF_SIZE 512

static void led_hw_on(void) { HAL_GPIO_WritePin(GPIOF, GPIO_PIN_10, GPIO_PIN_RESET); }
static void led_hw_off(void) { HAL_GPIO_WritePin(GPIOF, GPIO_PIN_10, GPIO_PIN_SET); }
static void led_hw_toggle(void) { HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_10); }


/**
 * @file webserver.c
 * @brief 基于 TCP Socket 的简易 HTTP 服务器模块。
 *
 * 本模块实现了一个简单的 HTTP Web 服务器，用于通过网页控制 LED 状态
 * 以及设置 LED 闪烁频率。服务器支持 GET 和 POST 请求解析，
 * 并支持 application/x-www-form-urlencoded 格式参数提取。
 *
 * @note 本模块适用于嵌入式环境中的轻量级 HTTP 控制场景。
 */

/*── LED 状态用软件变量维护，不依赖 GPIO 回读 ── */
static volatile int s_led_state = 0; /* 0=灭，1=亮；与 PF10 初始 SET(灭) 一致 */

int led_get(void)
{
    return g_config.led_state; /*直接用 g_config，不另开变量 */
}

void led_set(int on)
{
    uint16_t val = on ? 1 : 0;
    if (g_config.led_state == val)
        return;                        /* 值没变就不写 Flash */
    EE_WriteVar(EE_ID_LED_STATE, val); /* 同步到 Flash + g_config */
    printf("led set %d\r\n", g_config.led_state);
}

/* ── LED Blink 任务 ────────────────────────────────────────
 * led_state=0：常灭
 * led_state=1：按 led_freq（ms）为半周期闪烁
 * 每20ms 检查一次，模式切换后立刻响应
 */
static void led_blink_task(void *arg)
{
    (void)arg;
    uint32_t elapsed = 0;
    uint16_t last_mode = 0xFF; /* 强制首次更新 */

    while (1)
    {
        uint16_t mode = g_config.led_state;
        uint16_t freq = g_config.led_freq;
        if (freq < 100)
            freq = 100;

        /* 模式切换时立即刷新 */
        if (mode != last_mode)
        {
            last_mode = mode;
            elapsed = 0;
            mode ? led_hw_on() : led_hw_off();
        }

        if (mode == 1)
        {
            elapsed += 20;
            if (elapsed >= freq)
            {
                elapsed = 0;
                led_hw_toggle();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void send_response(int sock, const char *content_type, const char *body)
{
    char header[256];
    int body_len = (int)strlen(body);
    snprintf(header, sizeof(header), "HTTP/1.0 200 OK\r\n"
                                     "Content-Type: %s\r\n"
                                     "Content-Length: %d\r\n"
                                     "Cache-Control: no-store\r\n"
                                     "Connection: close\r\n"
                                     "\r\n",
             content_type, body_len);
    send(sock, header, (int)strlen(header), 0);
    send(sock, body, body_len, 0);
}

/* ──读取完整 HTTP 请求── */
// static int recv_request(int fd, char *buf, int buf_size)
//{
//     int total= 0;
//     uint32_t timeout_ms = 3000;
//     setsockopt(fd, 0, SO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
//     while (total < buf_size - 1) {
//         int n = recv(fd, buf + total, buf_size - 1 - total, 0);
//         if (n > 0) {
//             total     += n;
//             buf[total] = '\0';
//             if (strstr(buf, "\r\n\r\n")) return total;
//         } else {
//             break;
//         }
//     }
//     return total;
// }

static int recv_request(int fd, char *buf, int buf_size)
{
    int total = 0;
    uint32_t timeout_ms = 3000;
    setsockopt(fd, 0, SO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));

    // 第一步：读到空行（头部结束）
    while (total < buf_size - 1)
    {
        int n = recv(fd, buf + total, buf_size - 1 - total, 0);
        if (n > 0)
        {
            total += n;
            buf[total] = '\0';
            if (strstr(buf, "\r\n\r\n"))
                break; // ← 恢复这行！
        }
        else
        {
            break;
        }
    }

    // 第二步：POST 读body
    char *cl = strstr(buf, "Content-Length:");
    if (cl)
    {
        int need = atoi(cl + 15);
        char *body = strstr(buf, "\r\n\r\n");
        if (body && need > 0)
        {
            body += 4;
            int have = (int)(buf + total - body);
            int remain = need - have;
            while (remain > 0 && total < buf_size - 1)
            {
                int n = recv(fd, buf + total, remain, 0);
                if (n > 0)
                {
                    total += n;
                    buf[total] = '\0';
                    remain -= n;
                }
                else
                {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    break;
                }
            }
        }
    }
    return total;
}


static const char *HTML_CONTROL =
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

    "function loadStatus(){"
    "  fetch('/status')"
    "  .then(function(r){return r.json();})"
    "  .then(function(d){show(d);});"
    "}"
		
    "function sendCtrl(){"
    "  var body='led='+document.getElementById('led').value+'&freq='+document.getElementById('freq').value;"
    "  fetch('/control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
    "  .then(function(r){return r.json();})"
    "  .then(function(d){alert('ok');});"
    "}"

    "loadStatus();"
    "setInterval(loadStatus,10000);"
		
    "</script>"
    "</body></html>";

static uint8_t buf[1024];
    /*POST /PATH HTTP/1.1  ... /r/n/r/nparm1=value1&parm2=value2*/
    /*GET /PATH?parm1=value1&parm2=value2 HTTP/1.1 ...*/
/**
 * @brief HTTP 请求解析结果结构体。
 *
 * 用于保存 HTTP 请求解析后的方法、路径以及参数信息。
 */
typedef struct {
    char        method[8];        /**< 请求方法，例如 "GET" 或 "POST"。 */
    char        path[64];         /**< 请求路径，例如 "/" 或 "/control"。 */
    char        params_buf[128];  /**< 参数缓存区，用于保存解析出的参数字符串。 */
    const char *params;           /**< 参数字符串指针；无参数时为 NULL。 */
} http_req_t;

/**
 * @brief 从 URL 参数字符串中获取指定 key 对应的 value。
 *
 * 该函数用于解析形如 "key1=value1&key2=value2" 的参数字符串，
 * 查找指定 key，并将其对应的 value 拷贝到输出缓冲区中。
 *
 * @param params   参数字符串，例如 "led=1&freq=500"。
 * @param key      需要查找的参数名，例如 "led"。
 * @param out      用于保存参数值的输出缓冲区。
 * @param out_len  输出缓冲区长度。
 *
 * @return int
 * @retval 0   获取参数成功。
 * @retval -1  参数无效或未找到指定 key。
 *
 * @note 该函数不会进行 URL 解码，例如不会将 "%20" 转换为空格。
 * @note 输出字符串会自动以 '\0' 结尾。
 */
int param_get(const char *params, const char *key, char *out, int out_len)
{
    const char *p;
    const char *val;
    int key_len;
    int i;

    if (!params || !key || !out || out_len <= 0)
        return -1;

    key_len = (int)strlen(key);
    p= params;

    while (*p) {
        /* 比较 key */
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            val = p + key_len + 1;
            i= 0;
            while (val[i] && val[i] != '&' && i < out_len - 1){
                out[i] = val[i];
								i++;
						}
            out[i] = '\0';
            return 0;
        }
        /*跳到下一个 & */
        while (*p && *p != '&')
            p++;
        if ('&' == *p)
            p++;
    }

    return -1;
}

/**
 * @brief 解析原始 HTTP 请求数据。
 *
 * 该函数用于解析 HTTP 请求报文，提取请求方法、请求路径以及请求参数。
 * 支持解析 GET 请求 URL 中的查询参数，也支持解析 POST 请求 body 中的表单参数。
 *
 * GET 请求示例：
 * @code
 * GET /control?led=1&freq=500 HTTP/1.1
 * @endcode
 *
 * POST 请求示例：
 * @code
 * POST /control HTTP/1.1
 * Content-Type: application/x-www-form-urlencoded
 *
 * led=1&freq=500
 * @endcode
 *
 * @param raw      原始 HTTP 请求数据缓冲区。
 * @param raw_len  原始 HTTP 请求数据长度。
 * @param req      HTTP 请求解析结果结构体指针。
 *
 * @return int
 * @retval 0   解析成功。
 * @retval -1  解析失败，请求格式错误。
 *
 * @note 解析出的参数会被深拷贝到 req->params_buf 中。
 * @note req->params 指向 req->params_buf；如果没有参数，则为 NULL。
 * @note 当前函数只做简单 HTTP 解析，不支持复杂 Header 解析和分块传输。
 */
int http_parse(const char *raw, int raw_len,http_req_t *req)
{
    const char *p;
    const char *path_start;
    const char *src;
    const char *end;
    const char *body;
    int i;
    int path_len;
    int src_len;

    p= raw;
    src     = NULL;
    src_len = 0;

    /* 1. method */
    i = 0;
    while (*p && *p != ' ' && i < (int)sizeof(req->method) - 1)
        req->method[i++] = *p++;
    req->method[i] = '\0';
    if (*p != ' ') return -1;
    p++;

    /* 2. path，停在 '?' 或 ' ' */
    path_start = p;
    while (*p && *p != ' ' && *p != '?')
        p++;

    path_len = (int)(p - path_start);
    if (path_len >= (int)sizeof(req->path))
        path_len = (int)sizeof(req->path) - 1;
    memcpy(req->path, path_start, path_len);
    req->path[path_len] = '\0';

    /* 3. 深拷贝参数 */
    if (strcmp(req->method, "GET") == 0 && *p == '?') {
        src = p + 1;
        end = strchr(src, ' ');
        src_len = end ? (int)(end - src) : (int)(raw_len - (int)(src - raw));
    } else if (strcmp(req->method, "POST") == 0) {
        body = strstr(raw, "\r\n\r\n");
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
static void send_status_json(int client)
{
    char json[128];

    snprintf(json, sizeof(json),
             "{\"ok\":true,\"led\":%d,\"freq\":%d}",
             g_config.led_state,
             g_config.led_freq);

    send_response(client, "application/json; charset=utf-8", json);
}

void webserver_task(void *arg)
{
    at_init("stm32_f4_uart2");
    at_connect_ap("Tenda", "wxw123456");
    http_req_t req;
    uint32_t timeout_ms = 3000;
    int recv_len = 0;
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0)
    {
        printf("[HTTP] socket failed\r\n");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);

    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        printf("[HTTP] bind failed\r\n");
        vTaskDelete(NULL);
        return;
    }
    listen(server_sock, 4);
    printf("[HTTP] listening on port %d\r\n", PORT);

    while (1)
    {
        int client = accept(server_sock, NULL, NULL);
        if (client < 0)
        {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        setsockopt(client, 0, SO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));

        //        recv_len = recv(client, buf, sizeof(buf), 0);
        //        buf[recv_len] = '\0';
        //        if (recv_len > 0) {
        //            printf(">>> first line: %s", buf);
        //        }
        recv_len = recv_request(client, (char *)buf, sizeof(buf)-1);
				if (recv_len <= 0)
        {
            closesocket(client);
            continue;
        }

        buf[recv_len] = '\0';

        printf("----- HTTP REQ -----\r\n%s\r\n", buf);
				memset(&req, 0, sizeof(req));
				if (0 != http_parse((const char *)buf, recv_len, &req))
        {
            send_response(client, "application/json; charset=utf-8",
                          "{\"ok\":false,\"msg\":\"bad request\"}");
            closesocket(client);
            continue;
        }
				printf("method=%s, path=%s, params=%s\r\n",
               req.method,
               req.path,
               req.params ? req.params : "NULL");
				
   if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/") == 0)
        {
            send_response(client, "text/html; charset=utf-8", HTML_CONTROL);
        }
				else if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/status") == 0)
				{
						send_status_json(client);
				}
        else if (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/control") == 0)
        {
            char led_str[8];
            char freq_str[16];

            int led = 0;
            int freq = 500;

            if (param_get(req.params, "led", led_str, sizeof(led_str)) == 0)
            {
                led = atoi(led_str);
            }

            if (param_get(req.params, "freq", freq_str, sizeof(freq_str)) == 0)
            {
                freq = atoi(freq_str);
            }

            if (freq < 100)
                freq = 100;
            if (freq > 5000)
                freq = 5000;

            printf("LED=%d, FREQ=%d ms\r\n", led, freq);
						g_config.led_freq = freq;
						g_config.led_state =led;
            /*
             * 在这里控制 LED
             * 例如：
             * led_set_enable(led);
             * led_set_period(freq);
             */

            char json[128];
            snprintf(json, sizeof(json),
                     "{\"ok\":true,\"led\":%d,\"freq\":%d}",
                     led, freq);

            send_response(client, "application/json; charset=utf-8", json);
        }
        else
        {
            send_response(client, "application/json; charset=utf-8",
                          "{\"ok\":false,\"msg\":\"not found\"}");
        }

        closesocket(client);
    }
}

/* ── 入口 ── */
void start_webserver(void)
{
    EE_Init(); /* 从 Flash 加载 g_config */

    /* 按 Flash 里的状态恢复 LED 初始输出 */
    g_config.led_state ? led_hw_on() : led_hw_off();

    xTaskCreate(led_blink_task, "led_blink", 256, NULL, osPriorityNormal, NULL);
    xTaskCreate(webserver_task, "http_srv", 512, NULL, osPriorityNormal, NULL);
}
