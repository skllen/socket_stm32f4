#include <string.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

/* 根据你的工程实际头文件调整 */
#include "esp8266.h"

#define ESP8266_TEST_SERVER_PORT        777
#define ESP8266_RECV_BUF_SIZE          512

#define ESP8266_SERVER_TASK_STACK       768
#define ESP8266_CLIENT_TASK_STACK       768

#define ESP8266_SERVER_TASK_PRIO        3
#define ESP8266_CLIENT_TASK_PRIO        3

extern  uint32_t esp8266_get_local_ip(struct AT_Device *pdev);
typedef struct
{
    int client_fd;
} esp8266_client_arg_t;


/*
 * 每个客户端一个任务：
 * 负责 recv 数据，然后 send 回显。
 */
//static void esp8266_client_echo_task(void *argument)
//{
//    esp8266_client_arg_t *arg;
//		int err;
//    int client_socket_fd;
//    uint8_t recv_buf[ESP8266_RECV_BUF_SIZE];
//    int len;
//    struct UART_Device *puart = Get_UART_Device("stm32_f4_uart1");
//    
//    arg = (esp8266_client_arg_t *)argument;
//    client_socket_fd = arg->client_fd;

//    vPortFree(arg);

//    while (1)
//    {
//        len = esp8266_recv(client_socket_fd,
//                           recv_buf,
//                           sizeof(recv_buf),
//                           0);

//        if (len > 0)
//        {

//            esp8266_send(client_socket_fd,
//                         recv_buf,
//                         len,
//                         0);
//        }
//        else
//        {
//            /*
//             * len == 0：客户端关闭
//             * len < 0 ：接收出错
//             */
//            //break;
//        }

//        vTaskDelay(pdMS_TO_TICKS(10));
//    }

//    err = esp8266_close(client_socket_fd);

//    vTaskDelete(NULL);
//}


/*
 * TCP Server 任务：
 * 只负责 accept 新客户端。
 * accept 成功后，创建 client_echo_task。
 */
static void esp8266_tcp_server_task(void *argument)
{
//    struct AT_Device *pdev;
//    int server_socket_fd;
//    int client_socket_fd;
//    int err;

//    struct sockaddr_in server_addr;

//    pdev = (struct AT_Device *)argument;

//    server_socket_fd = esp8266_socket(AF_INET, SOCK_STREAM, 0);
//    if (server_socket_fd < 0)
//    {
//        vTaskDelete(NULL);
//    }

//    memset(&server_addr, 0, sizeof(server_addr));

//    server_addr.sin_family = AF_INET;
//    server_addr.sin_port   = htons(ESP8266_TEST_SERVER_PORT);

//    /*
//     * 如果这个 demo 放在 esp8266.c 里，可以用 esp8266_get_local_ip(pdev)。
//     * 如果放在其他 .c 文件，esp8266_get_local_ip 是 static 调不到，
//     * 那就直接改成：
//     *
//     * server_addr.sin_addr.s_addr = 0;
//     */
//    server_addr.sin_addr.s_addr = esp8266_get_local_ip(pdev);

//    /*
//     * 如果获取 IP 失败，就绑定 0.0.0.0。
//     * 对 ESP8266 TCP Server 来说，最终 listen 主要用端口。
//     */
//    if (server_addr.sin_addr.s_addr == 0)
//    {
//        server_addr.sin_addr.s_addr = 0;
//    }

//    err = esp8266_bind(server_socket_fd,
//                       (struct sockaddr *)&server_addr,
//                       sizeof(server_addr));
//    if (err != 0)
//    {
//        esp8266_close(server_socket_fd);
//        vTaskDelete(NULL);
//    }

//    err = esp8266_listen(server_socket_fd, 4);
//    if (err != 0)
//    {
//        esp8266_close(server_socket_fd);
//        vTaskDelete(NULL);
//    }

//    while (1)
//    {
//        struct sockaddr_in client_addr;
//        socklen_t client_addrlen;
//        esp8266_client_arg_t *client_arg;
//        BaseType_t ret;

//        memset(&client_addr, 0, sizeof(client_addr));
//        client_addrlen = sizeof(client_addr);

//        /*
//         * 这里会不断重新进入 accept。
//         * 不要在这个任务里 recv。
//         */
//        client_socket_fd = esp8266_accept(server_socket_fd,
//                                          (struct sockaddr *)&client_addr,
//                                          &client_addrlen);

//        if (client_socket_fd < 0)
//        {
//            vTaskDelay(pdMS_TO_TICKS(100));
//            continue;
//        }

//        client_arg = pvPortMalloc(sizeof(esp8266_client_arg_t));
//        if (client_arg == NULL)
//        {
//            esp8266_close(client_socket_fd);
//            continue;
//        }

//        client_arg->client_fd = client_socket_fd;

//        ret = xTaskCreate(esp8266_client_echo_task,
//                          "esp_cli_echo",
//                          ESP8266_CLIENT_TASK_STACK,
//                          client_arg,
//                          ESP8266_CLIENT_TASK_PRIO,
//                          NULL);

//        if (ret != pdPASS)
//        {
//            vPortFree(client_arg);
//            esp8266_close(client_socket_fd);
//        }

//        /*
//         * 到这里 server task 立刻继续 while，
//         * 重新进入 esp8266_accept() 等下一个客户端。
//         */
//    }
}


/*
 * 外部调用这个函数启动 TCP Server 测试。
 *
 * 调用位置：ESP8266 初始化完成、连上 WiFi 后调用。
 */
int esp8266_tcp_server_test_start(struct AT_Device *pdev)
{
    BaseType_t ret;

    ret = xTaskCreate(esp8266_tcp_server_task,
                      "esp_tcp_srv",
                      ESP8266_SERVER_TASK_STACK,
                      pdev,
                      ESP8266_SERVER_TASK_PRIO,
                      NULL);

    if (ret != pdPASS)
    {
        return -1;
    }

    return 0;
}
