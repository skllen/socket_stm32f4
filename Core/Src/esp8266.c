#include "esp8266.h"
#include "driver_timer.h"
#include "stdio.h"
#include "string.h"
#include "uart_device.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stdlib.h"

#define ESP8266_OK          0
#define ESP8266_ERROR       1
#define ESP8266_TIMEOUT     2
#define ESP8266_BUSY        3

#define ESP8266_RX_BUF_SIZE 512
#define ESP8266_RX_MAX_LINE_SIZE 8

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifndef AF_INET
#define AF_INET 2
#endif

#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif

struct AT_Device g_esp8266;
#define RECV_QUEUE_SIZE  512
#define AT_TIMEOUT 5000

/* CIPSTATUS 解析结果结构 */
// <link	ID>,<type>,<remote	IP>,<remote	port>,<local port>,<tetype>
typedef struct {
    int      link_id;        // 连接 ID
    int      type;           // SOCK_STREAM 或 SOCK_DGRAM
    uint32_t remote_ip;      // 远端 IP，网络字节序
    uint16_t remote_port;    // 远端端口，网络字节序 / 主机序根据你的需求
    uint16_t local_port;     // 本端端口
    int      tetype;         // 0=CLIENT, 1=SERVER
} cipstatus_t;

static TaskHandle_t  g_start_recv_handle;
void Start_RECV_Task(void *argument);

struct AT_Device *Get_AT_Device(char *name)
{
    return &g_esp8266;
}

/**
 * @brief 获取 ESP8266 本机 IP，优先用缓存
 * @return 网络字节序 IP，失败返回 0
 */
static uint32_t esp8266_get_local_ip(struct AT_Device *pdev)
{
    char     ip_str[32] = {0};
    uint32_t ip        = 0;
    int      i= 0;

    /* 有缓存直接返回 */
    if (pdev->local_ip != 0)
        return pdev->local_ip;
    /* 发AT+CIFSR */
    if (at_send_cmd(pdev, "AT+CIFSR\r\n", NULL, NULL, 0, AT_TIMEOUT) != 0)
        return 0;

    /* 解析响应，找+CIFSR:STAIP,"x.x.x.x" */
    for (i = 0; i < (int)pdev->resp_line_counts && i < RESP_ROW_LEN; i++)
    {
        if (sscanf((const char *)pdev->resp[i],"+CIFSR:STAIP,\"%31[^\"]\"", ip_str) == 1)
        {
            inet_pton(AF_INET, ip_str, &ip);
            pdev->local_ip = ip;   /* 写入缓存 */
            break;
        }
    }

    pdev->resp_line_counts = 0;
    pdev->resp_status= 0;
    memset(pdev->resp_line_len, 0, sizeof(pdev->resp_line_len));
    return ip;
}


/*获取esp8266 硬件未使用的  linkid*/
// get_unused_hw_socket：找硬件上未连接的 link_id
static int get_esp8266_unused_hw_socket(void)
{
    struct AT_Device *ptDev = Get_AT_Device("esp8266");

    uint8_t used[ESP8266_SOCKET_NUM] = {0};
    for (int i = 0; i < ESP8266_SOCKET_NUM; i++)
    {
        // 只有 status == SOCKET_USED 真正在用
        if (ptDev->sockets[i].status == SOCKET_USED)
        {
            int linkid = ptDev->sockets[i].hw_socket;
            if (linkid >= 0 && linkid < ESP8266_SOCKET_NUM)
                used[linkid] = 1;
        }
    }

    for (int i = 0; i < ESP8266_SOCKET_NUM; i++)
    {
        if (!used[i]) return i;
    }
    return -1;
}

struct socket_t *get_esp8266_unuse_fd_socket(void)
{
    int i;
    struct AT_Device *pDev = Get_AT_Device("esp8266");
    struct socket_t *ptSockets =pDev->sockets;
    struct socket_t *pSocket = NULL;
    for(i = 0; i < ESP8266_SOCKET_NUM; i++)
    {
        pSocket = &ptSockets[i];
        if((pSocket->status == SOCKET_FREE) && (pSocket->open_flag== 0))
        {
            return pSocket;
        }
    }
    return NULL;
}

static struct socket_t *get_esp8266_socket_for_hw_socket(int hw_socket)
{
    struct AT_Device *ptDev = Get_AT_Device("esp8266");
    for (int i = 0; i < ESP8266_SOCKET_NUM; i++)
    {
        if (ptDev->sockets[i].hw_socket == hw_socket)
        {
            if ((ptDev->sockets[i].status == SOCKET_USED) || (ptDev->sockets[i].open_flag == 1))
            {
                return &ptDev->sockets[i];
            }
        }
    }
    return NULL;
}

void esp8266_init(void)
{
    /*初始化g_esp8266 sockets 状态*/
    /*初始化g_esp8266 sockets的互斥量队列*/
	/*初始化g_esp8266 发送,resp互斥锁，以及内部数值*/
    struct AT_Device *pdev = &g_esp8266;
    struct socket_t  *psocket;

    memset(pdev, 0, sizeof(struct AT_Device));
    pdev->dev_lock = xSemaphoreCreateMutex();
    pdev->send_lock   = xSemaphoreCreateMutex();
    pdev->at_resp_sem = xSemaphoreCreateBinary();
    pdev->prompt_sem  = xSemaphoreCreateBinary();
    for(int i =0 ;i<5 ;i++)
    {
        psocket = &pdev->sockets[i];
        psocket->sockfd     = i;
        psocket->status = SOCKET_FREE;
        psocket->recv_lock = xSemaphoreCreateCounting(10,0);
        psocket->send_lock = xSemaphoreCreateMutex();
        psocket->recv_queue = xQueueCreate(100,sizeof(uint8_t));
        psocket->mode      = SOCKET_UNKNOWN;
    }
    pdev->puart       = Get_UART_Device("stm32_f4_uart2");
}

/**
 * @brief  发送 AT 指令并等待 OK/ERROR
 * @param  cmdAT 指令字符串（不含 \r\n）
 * @param  timeout_ms 超时时间（毫秒）
 * @return ESP8266_OK / ESP8266_ERROR / ESP8266_TIMEOUT
 */
int at_send_cmd(struct AT_Device *ptDev , char *cmd, uint8_t *resp, uint32_t *resp_len, uint32_t max_len,uint32_t timeout_ms)
{
    struct UART_Device *puart = ptDev->puart;
    uint32_t cur_len;
	uint32_t total_len;
	uint32_t i;
    int ret = -1;
    xSemaphoreTake(ptDev->send_lock, portMAX_DELAY);

    ptDev->resp_line_counts = 0;
    ptDev->resp_status = 0;
    memset(ptDev->resp_line_len, 0, sizeof(ptDev->resp_line_len));
    //xSemaphoreTake(ptDev->at_resp_sem, 0);
    puart->UART_Send(puart, (uint8_t *)cmd, strlen(cmd), timeout_ms);
    // puart->UART_Send(puart, (const uint8_t *)"\r\n", 2);

    if (pdTRUE == xSemaphoreTake(ptDev->at_resp_sem, pdMS_TO_TICKS(timeout_ms)))
    {
        if(resp)
        {
            cur_len=0;
            total_len =0;
            //
            for(i=0;i<ptDev->resp_line_counts;i++)
            {
                cur_len = (ptDev->resp_line_len[i] > max_len)? max_len : ptDev->resp_line_len[i] ;
                //
                if((total_len+cur_len) <= max_len)
                {
                    memcpy(resp+total_len, ptDev->resp[i], cur_len);
                    //
                    total_len += cur_len;
                }
            }
            //
            *resp_len= total_len;
            //
        }
        //
        ret = ptDev->resp_status;
    }

    xSemaphoreGive(ptDev->send_lock);
    return ret;
}

int at_send_data(struct AT_Device *pdev,const uint8_t *data, uint32_t len, uint32_t timeout)
{
    int ret = 0;
    xSemaphoreTake(pdev->send_lock, portMAX_DELAY);
    ret = pdev->puart->UART_Send(pdev->puart, (uint8_t *)data, len, timeout);
    // xSemaphoreTake(pdev->at_resp_sem, pdMS_TO_TICKS(timeout));
    xSemaphoreGive(pdev->send_lock);
    return ret;
}

int esp8266_send_data(struct AT_Device *pdev, uint8_t link_id, uint8_t *data, uint32_t len, uint32_t timeout)
{
    char cmd[32];
    int ret = -1;
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d,%d\r\n", link_id, len);

    at_send_cmd(pdev, cmd, NULL, NULL, 0, 1000);

    ret = at_send_data(pdev, data,  len, timeout);

    return ret;
}

int esp8266_wifi_connect(char *ssid, char *password)
{
    char cmd[128];
    uint32_t resp_len;
    uint8_t resp[128];
    volatile int err;
    struct AT_Device *pdev = Get_AT_Device("esp8266");

    esp8266_init();
    struct UART_Device *puart = pdev->puart;
    puart->UART_Init(puart, 115200, 8, 1, 0);
    struct UART_Device *puart1 = Get_UART_Device("stm32_f4_uart1");
    puart1->UART_Init(puart1, 115200, 8, 1, 0);
    xTaskCreate(
        Start_RECV_Task,       // 函数指针, 任务函数
        "Start_recv_task",     // 任务的名字
        512,                   // 栈大小,单位为word,10表示40字节
        NULL,                  // 调用任务函数时传入的参数
        5,                     // 优先级
        &g_start_recv_handle); // 任务句柄, 以后使用它来操作这个任务
                               //   err = at_send_cmd(pdev, "AT+RST\r\n", NULL, NULL, 0, 15000);
                               //   while (err);
    err = at_send_cmd(pdev, "ATE0\r\n", resp, &resp_len, sizeof(resp), 15000);
    while (err)
        ;
    vTaskDelay(1000);
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, password);
    err = at_send_cmd(pdev, cmd, resp, &resp_len, sizeof(resp), 15000);
    while (err)
        ;
    esp8266_get_local_ip(pdev);
    err = at_send_cmd(pdev, "AT+CIPMUX=1\r\n", NULL, NULL, 0, 15000);

    int socket_server_fd = esp8266_socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr;
    socklen_t server_addr_len;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(777);
    inet_pton(AF_INET, "192.168.0.152", &server_addr.sin_addr);
    server_addr_len = sizeof(server_addr);
    esp8266_bind(socket_server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    esp8266_listen(socket_server_fd, 10);
    uint8_t buf[100];
    char c;
    int client_fd ;
		while(1)
		{
			client_fd = esp8266_accept(socket_server_fd, (struct sockaddr *)&server_addr, &server_addr_len);
            c = client_fd + '0';
            if(client_fd < 0)
            continue;
			puart1->UART_Send(puart1,"\r\nclient_fd:",strlen("\r\nclient_fd:"),100);
			puart1->UART_Send(puart1,(uint8_t*)&c, 1,100);
             puart1->UART_Send(puart1,"\r\n",strlen("\r\n"),100);
             esp8266_recv(client_fd, buf, 100, 1);
             esp8266_close(client_fd);
		}
		
    // extern int esp8266_tcp_server_test_start(struct AT_Device *pdev);
    // err = esp8266_tcp_server_test_start(pdev);
    //     if (err != 0)
    // {
    //     return err;
    // }
    return 0;
}

int esp8266_hw_init(void)
{
    int i;
    struct AT_Device *pdev = Get_AT_Device("esp8266");
    struct socket_t *pSockets = pdev->sockets;
    struct socket_t *pSocket;
    struct UART_Device *puart = pdev->puart;
	puart->UART_Init(puart, 115200, 8, 1, 0);

    for(i =0 ;i<ESP8266_SOCKET_NUM; i++)
    {
        pSocket = &pSockets[i];
        // if(i == ESP8266_SOCKET_SERVER_INDEX)
        // {
        //     pSocket->hw_socket = ESP8266_SOCKET_SERVER_ID;
        // }
        // else
        // {
        //     pSocket->hw_socket = i;
        // }
        
        pSocket->status = SOCKET_FREE;
        pSocket->recv_lock = xSemaphoreCreateCounting(ESP8266_SOCKET_RX_SEMAPHORE_COUNT,0);
        pSocket->send_lock = xSemaphoreCreateMutex();
        pSocket->recv_queue = xQueueCreate(ESP8266_SOCKET_RX_QUEUE_SIZE,sizeof(uint8_t));
        pSocket->mode      = SOCKET_UNKNOWN;
        pSocket->socket_type = SOCK_UNDEF;
    }
    pdev->dev_lock = xSemaphoreCreateMutex();
    pdev->send_lock   = xSemaphoreCreateMutex();
    pdev->at_resp_sem = xSemaphoreCreateBinary();
    pdev->prompt_sem  = xSemaphoreCreateBinary();
    xTaskCreate(
        Start_RECV_Task,      // 函数指针, 任务函数
        "Start_recv_task",    // 任务的名字
        512,                  // 栈大小,单位为word,10表示40字节
        NULL,                 // 调用任务函数时传入的参数
        2,                    // 优先级
        &g_start_recv_handle); // 任务句柄, 以后使用它来操作这个任务
    return 0;
}

void Start_RECV_Task(void *argument)
{
    struct AT_Device *pdev = Get_AT_Device("esp8266");
    char recv_buf[ESP8266_RX_BUF_SIZE];
    struct UART_Device *puart = Get_UART_Device("stm32_f4_uart2");
    char *comma;
    volatile uint32_t line_len = 0;
    uint8_t ch;
    uint8_t link_id;
    struct socket_t *psocket;

    char *ipd;
    char *colon;
    uint16_t data_len;
    char *p;
    uint8_t data;
    while (1)
    {
        puart->UART_Recv(puart, &ch, portMAX_DELAY);
        if (line_len < ESP8266_RX_BUF_SIZE - 1)
        {
            recv_buf[line_len] = ch;
            recv_buf[line_len + 1] = '\0';
        }
        else
        {
            /* 缓冲区已满，取出一半移到前面，继续接收   */
            memcpy(recv_buf, recv_buf + (ESP8266_RX_BUF_SIZE) / 2,
                   ESP8266_RX_BUF_SIZE - (ESP8266_RX_BUF_SIZE) / 2);
            line_len = ESP8266_RX_BUF_SIZE - (ESP8266_RX_BUF_SIZE) / 2;

            /* 修复：补存当前字符 ch，并手动递增 line_len */
            recv_buf[line_len] = ch; // ← 补存
            recv_buf[line_len + 1] = '\0';
            line_len++;
            continue;
        }
        //+IPD,0,41,192.168.0.152,62385:cmd:led,offcmd:led,oncmd:oled,hello
        // 解析 +IPD,<link_id>,<len>:<data>,比如aa+IPD,0,200:1234567890 这样的数据也要解析防止干扰
        ipd = (line_len >= 4) ? strstr(recv_buf, "+IPD,") : NULL;
        if (ipd)
        {
            colon = strchr(ipd, ':');
            if (colon)
            {
                link_id = (uint8_t)(ipd[5] - '0');
                if (link_id > 4)
                {
                    line_len = 0;
                    recv_buf[0] = '\0';
                    continue;
                }
                char *comma2 = strchr(ipd + 5, ',');
                if (comma2 == NULL)
                {
                    line_len = 0;
                    recv_buf[0] = '\0';
                    continue;
                }
                p = comma2 + 1;
                data_len = 0;
                while (*p >= '0' && *p <= '9')
                {
                    data_len = data_len * 10 + (*p - '0');
                    p++;
                }
                psocket = get_esp8266_socket_for_hw_socket(link_id);
                if(psocket == NULL)
                {
                    // line_len = 0;
                    // recv_buf[0] = '\0';
                    continue;
                }
                for (uint16_t i = 0; i < data_len; i++)
                {
                    if (0 == puart->UART_Recv(puart, &data, portMAX_DELAY))
                    {
                        xQueueSend(psocket->recv_queue, &data, 0);
                    }
                }
                xSemaphoreGive(psocket->recv_lock);
                line_len = 0;
                recv_buf[0] = '\0';
                continue;
            }
            /* 头部未完整，继续接收 */
        }
        if (line_len > 0 && recv_buf[line_len - 1] == '\r' && recv_buf[line_len] == '\n')
        {
            line_len = line_len - 1;
            recv_buf[line_len] = '\0';
            if (strstr(recv_buf, ",CONNECT"))
            {
                link_id = (uint8_t)(recv_buf[0] - '0');
            }
            // if (strstr(recv_buf, ",CONNECT"))
            // {
            //     comma = strchr(recv_buf, ',');
            //     if (comma && comma == recv_buf + 1 && recv_buf[0] >= '0' && recv_buf[0] <= '4')
            //     {
            //         link_id = recv_buf[0] - '0';
            //         // if(0 == esp8266_socket_inuse(link_id, SOCKET_STATUS_INUSE))
            //     }
            // }
            else if (strstr(recv_buf, ",CLOSED"))
            {
                comma = strchr(recv_buf, ',');
                if (comma && comma == recv_buf + 1 && recv_buf[0] >= '0' && recv_buf[0] <= '4')
                {
                    link_id = recv_buf[0] - '0';
                }
            }
            // else if (strstr(recv_buf, "SEND OK"))//先匹配这个
            // {
            //     pdev->resp_status = ESP8266_OK;
            //     xSemaphoreGive(pdev->at_resp_sem);
            // }
            // else if (strstr(recv_buf, "SEND FAIL"))
            // {
            //     pdev->resp_status = ESP8266_ERROR;
            //     xSemaphoreGive(pdev->at_resp_sem);
            // }
            else if (strstr(recv_buf, "OK"))
            {
                /* 发送成功*/
                pdev->resp_status = ESP8266_OK;
                xSemaphoreGive(pdev->at_resp_sem);
            }
            else if (strstr(recv_buf, "ERROR"))
            {
                /* 发送失败*/
                pdev->resp_status = ESP8266_ERROR;
                xSemaphoreGive(pdev->at_resp_sem);
            }
            else if (pdev->resp_line_counts < ESP8266_RESP_LINE_MAX) /* ← 越界保护 */
            {
                pdev->resp_line_len[pdev->resp_line_counts] = line_len + 1;
                memcpy(pdev->resp[pdev->resp_line_counts++], recv_buf, line_len + 1); // 把一行数据拷贝到缓冲区包括'\0',resp_line_counts++ 表示下一行数据的存放位置
            }

            line_len = 0;
            recv_buf[line_len] = '\0';
            continue;
        }
        line_len++;
    }
}

/**
 * @brief  创建套接字
 * @param  domain   地址族，当前仅支持 AF_INET
 * @param  type     SOCK_STREAM（TCP）或 SOCK_DGRAM（UDP）
 * @param  protocol 通常为 0，忽略
 * @return 成功返回 fd（>= 0），失败返回 -1
 */
int esp8266_socket(int domain, int type, int protocol)
{
    struct AT_Device *pdev    = NULL;
    struct socket_t  *psocket = NULL;
    int               sockfd  = -1;

    /* 参数校验 */
    if (domain != AF_INET)
        return -1;
    if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return -1;

    pdev = Get_AT_Device("esp8266");
    if (pdev == NULL)
        return -1;

    psocket = get_esp8266_unuse_fd_socket();
    if (psocket == NULL)
    {
        return -1;
    }

    xSemaphoreTake(pdev->dev_lock, portMAX_DELAY);
    sockfd = fd_socket_alloc(psocket);
    psocket->open_flag = 1;
    if (sockfd < 0)
    {
        fd_socket_free(sockfd);
        xSemaphoreGive(pdev->dev_lock);
        return -1;
    }
    xSemaphoreGive(pdev->dev_lock);
    xSemaphoreTake(psocket->send_lock, portMAX_DELAY);
    /* 初始化槽位 */
    psocket->open_flag   = 1;
    psocket->status      = SOCKET_FREE;
    psocket->mode        = SOCKET_UNKNOWN;
    psocket->socket_type = (uint8_t)type;
    psocket->hw_socket   = 0xFFFFFFFFU;   /*尚未分配硬件 link_id */
    memset(&psocket->local_addr,  0, sizeof(psocket->local_addr));
    memset(&psocket->remote_addr, 0, sizeof(psocket->remote_addr));
    xSemaphoreGive(psocket->send_lock);
    return sockfd;
}
// /**
//  * @brief  创建套接字
//  * @param  domain   地址族，当前仅支持 AF_INET
//  * @param  type     SOCK_STREAM（TCP）或 SOCK_DGRAM（UDP）
//  * @param  protocol 通常为 0，忽略
//  * @return 成功返回 fd（>= 0），失败返回 -1
//  */
// int esp8266_socket(int domain, int type, int protocol)
// {
//     struct AT_Device *pdev    = NULL;
//     struct socket_t  *psocket = NULL;
//     int               sockfd  = -1;

//     /* 参数校验 */
//     if (domain != AF_INET)
//         return -1;
//     if (type != SOCK_STREAM && type != SOCK_DGRAM)
//         return -1;

//     pdev = Get_AT_Device("esp8266");
//     if (pdev == NULL)
//         return -1;

//     xSemaphoreTake(pdev->dev_lock, portMAX_DELAY);

//     psocket = get_esp8266_unuse_fd_socket();
//     if (psocket == NULL)
//     {
//         xSemaphoreGive(pdev->dev_lock);
//         return -1;
//     }

//     /* 初始化槽位，清理旧数据 */
//     psocket->open_flag   = 0;
//     psocket->status      = SOCKET_FREE;
//     psocket->mode        = SOCKET_UNKNOWN;
//     psocket->socket_type = (uint8_t)type;
//     psocket->hw_socket   = 0xFFFFFFFFU;   /*尚未分配硬件 link_id */
//     memset(&psocket->local_addr,  0, sizeof(psocket->local_addr));
//     memset(&psocket->remote_addr, 0, sizeof(psocket->remote_addr));

//     sockfd = fd_socket_alloc(psocket);
//     if (sockfd < 0)
//     {
//         /* fd表已满，完整回滚槽位 */
//         psocket->open_flag   = 0;
//         psocket->status      = SOCKET_FREE;
//         psocket->mode        = SOCKET_UNKNOWN;
//         psocket->socket_type = SOCK_UNDEF;
//         psocket->hw_socket   = 0xFFFFFFFFU;
//         xSemaphoreGive(pdev->dev_lock);
//         return -1;
//     }

//     xSemaphoreGive(pdev->dev_lock);
//     return sockfd;
// }


/**
 * @brief关闭套接字，释放本地和硬件资源
 * @note   无论 AT 命令是否成功都会释放本地资源，防止 fd 泄漏；
 *         关闭服务端 socket 会同步清理所有属于该服务的客户端 socket
 * @param  sockfd套接字描述符
 * @return 成功返回 0，失败返回 -1
 */
int esp8266_close(int sockfd)
{
    struct AT_Device   *pdev         = Get_AT_Device("esp8266");
    struct socket_t    *psocket      = fd_socket_get(sockfd);
    struct socket_t    *pclient      = NULL;
    struct sockaddr_in *pAddrTemp    = NULL;
    char                cmd[48]      = {0};
    uint8_t             discard      = 0;
    uint16_t            server_port  = 0;
    int                 i            = 0;
    int                 client_count = 0;
    int                 client_fds[ESP8266_SOCKET_NUM];

    /* 初始化客户端 fd 数组 */
    for (i = 0; i < ESP8266_SOCKET_NUM; i++)
        client_fds[i] = -1;

    /* ---- 参数校验 ---- */
    if (sockfd < 0 || sockfd >= FD_SOCKET_TABLE_SIZE)
        return -1;
    if (pdev == NULL)
        return -1;
    if (psocket == NULL)
        return -1;

    xSemaphoreTake(psocket->send_lock, portMAX_DELAY);

    /* 未打开直接返回成功（与 POSIX 语义一致）*/
    if (psocket->open_flag != 1)
    {
        xSemaphoreGive(psocket->send_lock);
        return 0;
    }

    /* ---- 发送 AT 命令关闭硬件连接 ---- */
    if (psocket->hw_socket != 0xFFFFFFFFU)
    {
        memset(cmd, 0, sizeof(cmd));

        if (psocket->mode == SOCKET_SERVER)
        {
            /* 服务端：停止整个 TCP Server */
            snprintf(cmd, sizeof(cmd), "AT+CIPSERVER=0\r\n");
        }
        else
        {
            /* 客户端：按link_id 关闭单条连接 */
            snprintf(cmd, sizeof(cmd), "AT+CIPCLOSE=%u\r\n", psocket->hw_socket);
        }

        /*
         * 忽略返回值：
         * 对端已断开时固件返回 ERROR，本地资源必须无条件释放，
         * 否则 fd 将永久泄漏
         */
        at_send_cmd(pdev, cmd, NULL, NULL, 0, AT_TIMEOUT);
    }

    /* ---- 服务端关闭：同步清理属于本服务的所有客户端 socket ---- */
    if (psocket->mode == SOCKET_SERVER)
    {
        pAddrTemp   = (struct sockaddr_in *)&psocket->local_addr;
        server_port = ntohs(pAddrTemp->sin_port);
        client_count = 0;

        for (i = 0; i < ESP8266_SOCKET_NUM; i++)
        {
            pclient = &pdev->sockets[i];
            if (pclient == psocket)
                continue;

            pAddrTemp = (struct sockaddr_in *)&pclient->local_addr;

            if (pclient->open_flag == 1             &&
                pclient->mode == SOCKET_CLIENT       &&
                ntohs(pAddrTemp->sin_port) == server_port)
            {
                /* 先记录 fd，再重置状态，顺序不能颠倒 */
                client_fds[client_count] = fd_socket_find(pclient);

                /*
                 * 锁序约束：server send_lock → client send_lock
                 * 系统中其他任务不得以相反顺序持锁，否则 ABBA 死锁
                 */
                xSemaphoreTake(pclient->send_lock, portMAX_DELAY);

                /* 清空接收队列（若队列存指针而非字节，需改为释放内存）*/
                while (xQueueReceive(pclient->recv_queue, &discard, 0) == pdTRUE);

                /* 重置状态字段，信号量和队列句柄不动 */
                pclient->open_flag   = 0;
                pclient->hw_socket   = 0xFFFFFFFFU;
                pclient->mode        = SOCKET_UNKNOWN;
                pclient->socket_type = SOCK_UNDEF;
                memset(&pclient->local_addr,  0, sizeof(pclient->local_addr));
                memset(&pclient->remote_addr, 0, sizeof(pclient->remote_addr));

                xSemaphoreGive(pclient->send_lock);

                client_count++;
            }
        }
    }

    /* ---- 清空本 socket 接收队列 ---- */
    while (xQueueReceive(psocket->recv_queue, &discard, 0) == pdTRUE);

    /* ---- 重置本 socket 状态字段（信号量和队列句柄不动）---- */
    psocket->open_flag   = 0;
    psocket->hw_socket   = 0xFFFFFFFFU;
    psocket->mode        = SOCKET_UNKNOWN;
    psocket->socket_type = SOCK_UNDEF;
    memset(&psocket->local_addr,  0, sizeof(psocket->local_addr));
    memset(&psocket->remote_addr, 0, sizeof(psocket->remote_addr));

    xSemaphoreGive(psocket->send_lock);

    /*
     * ---- dev_lock 内：原子标记槽位空闲并注销 fd ----
     *
     * status 必须在 dev_lock 内置为 SOCKET_FREE，而非在 send_lock 内。
     * 若提前标记，esp8266_socket() 可能在 fd_socket_free() 执行前
     * 拿走同一槽位，导致新 fd 与已注销 fd 指向同一 socket_t（竞态）。
     */
    xSemaphoreTake(pdev->dev_lock, portMAX_DELAY);

    psocket->status = SOCKET_FREE;
    fd_socket_free(sockfd);

    /* 同步释放客户端 fd 和槽位（仅服务端关闭时）*/
    for (i = 0; i < client_count; i++)
    {
        if (client_fds[i] >= 0)
        {
            pclient = fd_socket_get(client_fds[i]);
            if (pclient != NULL)
                pclient->status = SOCKET_FREE;
            fd_socket_free(client_fds[i]);
        }
    }

    xSemaphoreGive(pdev->dev_lock);

    return 0;
}

///**
// * @brief关闭套接字，释放本地和硬件资源
// * @note   无论 AT 命令是否成功都会释放本地资源，防止 fd 泄漏；
// *         关闭服务端 socket 会同步清理所有属于该服务的客户端 socket
// * @param  sockfd套接字描述符
// * @return 成功返回 0，失败返回 -1
// */
//int esp8266_close(int sockfd)
//{
//    struct AT_Device   *pdev         = NULL;
//    struct socket_t    *psocket      = NULL;
//    struct socket_t    *pclient      = NULL;
//    struct sockaddr_in *pAddrTemp    = NULL;
//    char                cmd[48]      = {0};
//    uint8_t             discard      = 0;
//    uint16_t            server_port  = 0;
//    int                 i            = 0;
//    int                 client_count = 0;
//    int                 client_fds[ESP8266_SOCKET_NUM];

//    /* 初始化客户端 fd 数组 */
//    for (i = 0; i < ESP8266_SOCKET_NUM; i++)
//        client_fds[i] = -1;

//    /* ---- 参数校验 ---- */
//    if (sockfd < 0 || sockfd >= FD_SOCKET_TABLE_SIZE)
//        return -1;

//    pdev = Get_AT_Device("esp8266");
//    if (pdev == NULL)
//        return -1;

//    psocket = fd_socket_get(sockfd);
//    if (psocket == NULL)
//        return -1;

//    xSemaphoreTake(psocket->send_lock, portMAX_DELAY);

//    /* 未打开直接返回成功（与 POSIX 语义一致）*/
//    if (psocket->open_flag != 1)
//    {
//        xSemaphoreGive(psocket->send_lock);
//        return 0;
//    }

//    /* ---- 发送 AT 命令关闭硬件连接 ---- */
//    if (psocket->hw_socket != 0xFFFFFFFFU)
//    {
//        memset(cmd, 0, sizeof(cmd));

//        if (psocket->mode == SOCKET_SERVER)
//        {
//            /* 服务端：停止整个 TCP Server */
//            snprintf(cmd, sizeof(cmd), "AT+CIPSERVER=0\r\n");
//        }
//        else
//        {
//            /* 客户端：按link_id 关闭单条连接 */
//            snprintf(cmd, sizeof(cmd), "AT+CIPCLOSE=%u\r\n", psocket->hw_socket);
//        }

//        /*
//         * 忽略返回值：
//         * 对端已断开时固件返回 ERROR，本地资源必须无条件释放，
//         * 否则 fd 将永久泄漏
//         */
//        at_send_cmd(pdev, cmd, NULL, NULL, 0, AT_TIMEOUT);
//    }

//    /* ---- 服务端关闭：同步清理属于本服务的所有客户端 socket ---- */
//    if (psocket->mode == SOCKET_SERVER)
//    {
//        pAddrTemp   = (struct sockaddr_in *)&psocket->local_addr;
//        server_port = ntohs(pAddrTemp->sin_port);
//        client_count = 0;

//        for (i = 0; i < ESP8266_SOCKET_NUM; i++)
//        {
//            pclient = &pdev->sockets[i];
//            if (pclient == psocket)
//                continue;

//            pAddrTemp = (struct sockaddr_in *)&pclient->local_addr;

//            if (pclient->open_flag == 1             &&
//                pclient->mode == SOCKET_CLIENT       &&
//                ntohs(pAddrTemp->sin_port) == server_port)
//            {
//                /* 先记录 fd，再重置状态，顺序不能颠倒 */
//                client_fds[client_count] = fd_socket_find(pclient);

//                /*
//                 * 锁序约束：server send_lock → client send_lock
//                 * 系统中其他任务不得以相反顺序持锁，否则 ABBA 死锁
//                 */
//                xSemaphoreTake(pclient->send_lock, portMAX_DELAY);

//                /* 清空接收队列（若队列存指针而非字节，需改为释放内存）*/
//                while (xQueueReceive(pclient->recv_queue, &discard, 0) == pdTRUE);

//                /* 重置状态字段，信号量和队列句柄不动 */
//                pclient->open_flag   = 0;
//                pclient->hw_socket   = 0xFFFFFFFFU;
//                pclient->mode        = SOCKET_UNKNOWN;
//                pclient->socket_type = SOCK_UNDEF;
//                memset(&pclient->local_addr,  0, sizeof(pclient->local_addr));
//                memset(&pclient->remote_addr, 0, sizeof(pclient->remote_addr));

//                xSemaphoreGive(pclient->send_lock);

//                client_count++;
//            }
//        }
//    }

//    /* ---- 清空本 socket 接收队列 ---- */
//    while (xQueueReceive(psocket->recv_queue, &discard, 0) == pdTRUE);

//    /* ---- 重置本 socket 状态字段（信号量和队列句柄不动）---- */
//    psocket->open_flag   = 0;
//    psocket->hw_socket   = 0xFFFFFFFFU;
//    psocket->mode        = SOCKET_UNKNOWN;
//    psocket->socket_type = SOCK_UNDEF;
//    memset(&psocket->local_addr,  0, sizeof(psocket->local_addr));
//    memset(&psocket->remote_addr, 0, sizeof(psocket->remote_addr));

//    xSemaphoreGive(psocket->send_lock);

//    /*
//     * ---- dev_lock 内：原子标记槽位空闲并注销 fd ----
//     *
//     * status 必须在 dev_lock 内置为 SOCKET_FREE，而非在 send_lock 内。
//     * 若提前标记，esp8266_socket() 可能在 fd_socket_free() 执行前
//     * 拿走同一槽位，导致新 fd 与已注销 fd 指向同一 socket_t（竞态）。
//     */
//    xSemaphoreTake(pdev->dev_lock, portMAX_DELAY);

//    psocket->status = SOCKET_FREE;
//    fd_socket_free(sockfd);

//    /* 同步释放客户端 fd 和槽位（仅服务端关闭时）*/
//    for (i = 0; i < client_count; i++)
//    {
//        if (client_fds[i] >= 0)
//        {
//            pclient = fd_socket_get(client_fds[i]);
//            if (pclient != NULL)
//                pclient->status = SOCKET_FREE;
//            fd_socket_free(client_fds[i]);
//        }
//    }

//    xSemaphoreGive(pdev->dev_lock);

//    return 0;
//}

/**
 * @brief 绑定本地地址和端口
 * @param sockfd  套接字描述符
 * @param addr    本地地址结构体指针，必须是 sockaddr_in
 * @param addrlen 地址结构体长度
 * @return 成功返回 0，失败返回 -1
 */
int esp8266_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    struct AT_Device       *pdev    = NULL;
    struct socket_t        *pSocket = NULL;
    const struct sockaddr_in *inaddr = NULL;
    uint16_t                port    = 0;
    int                     i       = 0;
    int                     duplicate = 0;

    /* ---- 参数校验 ---- */
    if (sockfd < 0 || sockfd >= FD_SOCKET_TABLE_SIZE)
        return -1;

    if (addr == NULL)
        return -1;

    if (addrlen < (socklen_t)sizeof(struct sockaddr_in))
        return -1;

    if (addr->sa_family != AF_INET)
        return -1;

    pdev = Get_AT_Device("esp8266");
    if (pdev == NULL)
        return -1;

    pSocket = fd_socket_get(sockfd);
    if (pSocket == NULL)
        return -1;

    inaddr = (const struct sockaddr_in *)addr;
    port = ntohs(inaddr->sin_port);

    /*
     * ESP8266 AT 模式下 TCP server / UDP local port 都需要明确端口。
     * 不支持 POSIX 那种 bind 端口 0 后由系统自动分配端口的语义。
     */
    if (port == 0)
        return -1;

    xSemaphoreTake(pSocket->send_lock, portMAX_DELAY);

    /* ---- socket 状态检查 ---- */

    /* 必须是 esp8266_socket() 创建出来的 socket */
    if (pSocket->open_flag != 1)
    {
        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /* 只支持 TCP / UDP */
    if (pSocket->socket_type != SOCK_STREAM &&
        pSocket->socket_type != SOCK_DGRAM)
    {
        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /* 已经 connect/listen/sendto 占用硬件连接后，不允许再 bind */
    if (pSocket->mode != SOCKET_UNKNOWN ||
        pSocket->status == SOCKET_USED ||
        pSocket->hw_socket != 0xFFFFFFFFU)
    {
        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /* 防止重复 bind */
    if (pSocket->local_addr.sin_port != 0)
    {
        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /*
     * 当前驱动里 close(server) 会根据 local_port 清理 accept 出来的 client。
     * 因此这里不允许多个 socket 绑定同一个本地端口，避免状态混乱。
     */
    xSemaphoreTake(pdev->dev_lock, portMAX_DELAY);

    for (i = 0; i < ESP8266_SOCKET_NUM; i++)
    {
        struct socket_t *pOther = &pdev->sockets[i];

        if (pOther == pSocket)
            continue;

        if (pOther->open_flag == 1 &&
            pOther->local_addr.sin_family == AF_INET &&
            pOther->local_addr.sin_port == inaddr->sin_port)
        {
            duplicate = 1;
            break;
        }
    }

    xSemaphoreGive(pdev->dev_lock);

    if (duplicate)
    {
        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /*
     * bind 只记录本地地址和端口，不发送 AT 指令。
     *
     * TCP server:
     *   esp8266_listen() 里发送 AT+CIPSERVER=1,<port>
     *
     * UDP:
     *   esp8266_connect() / esp8266_sendto() 里根据 local_addr.sin_port
     *   作为本地端口启动 UDP。
     */
    memset(&pSocket->local_addr, 0, sizeof(pSocket->local_addr));
    memcpy(&pSocket->local_addr, inaddr, sizeof(struct sockaddr_in));
    pSocket->local_addr.sin_family = AF_INET;

    xSemaphoreGive(pSocket->send_lock);

    return 0;
}


/**
 * @brief  开始监听连接请求
 * @note   调用前必须先 esp8266_bind()绑定端口；
 *         ESP8266 服务器模式最多支持 4 个并发客户端
 * @param  sockfd  套接字描述符
 * @param  backlog 最大等待连接数，超出4 自动截断
 * @return 成功返回 0，失败返回 -1
 */
int esp8266_listen(int sockfd, int backlog)
{
    /*---- 参数校验 ---- */
    if (sockfd < 0 || sockfd >= FD_SOCKET_TABLE_SIZE)
    {
        return -1;
    }

    /* ESP8266 服务器模式最多 4 个客户端 */
    if (backlog <= 0) backlog = 1;
    if (backlog > 4)backlog = 4;

    struct AT_Device *pdev = Get_AT_Device("esp8266");
    if (pdev == NULL) return -1;

    struct socket_t *pSocket = fd_socket_get(sockfd);
    if (pSocket == NULL) return -1;

    xSemaphoreTake(pSocket->send_lock, portMAX_DELAY);

    /* ---- socket 状态检查 ---- */

    /* 必须是已打开的 socket */
    if (pSocket->open_flag != 1)
    {
        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /* 修复：只有 TCP socket 才能 listen */
    if (pSocket->socket_type != SOCK_STREAM)
    {
        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /* 修复：必须先 bind 才能 listen，端口为 0 说明还未 bind */
    if (pSocket->local_addr.sin_port == 0)
    {
        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /* 修复：防止重复 listen */
    if (pSocket->mode == SOCKET_SERVER)
    {
        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /*---- 依次发送 AT 指令 ---- */
    char cmdbuf[64];

    /* 1. AT+CIPMUX=1  使能多连接模式 */
    if (at_send_cmd(pdev, "AT+CIPMUX=1\r\n", NULL, NULL, 0, AT_TIMEOUT) != 0)
    {
        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /* 2. AT+CIPSERVERMAXCONN=<backlog>  修复：使用调用者传入的 backlog */
    snprintf(cmdbuf, sizeof(cmdbuf), "AT+CIPSERVERMAXCONN=%d\r\n", backlog);
    /* 部分旧固件不支持此命令，忽略返回值 */
    at_send_cmd(pdev, cmdbuf, NULL, NULL, 0, AT_TIMEOUT);

    /* 3. AT+CIPSERVER=1,<port>  启动 TCP 服务器 */
    uint16_t port = ntohs(pSocket->local_addr.sin_port);
    snprintf(cmdbuf, sizeof(cmdbuf), "AT+CIPSERVER=1,%u\r\n", port);

    if (at_send_cmd(pdev, cmdbuf, NULL, NULL, 0, AT_TIMEOUT) != 0)
    {
        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /* ---- 修复：AT 命令全部成功后，再更新状态 ---- */
    pSocket->hw_socket = ESP8266_SOCKET_SERVER_ID;
    pSocket->mode      = SOCKET_SERVER;
    pSocket->status    = SOCKET_USED;   /* 修复：原代码缺少这一行 */

    xSemaphoreGive(pSocket->send_lock);
    return 0;
}


/**
 * @brief发起 TCP/UDP 连接
 * @param  sockfd  套接字描述符
 * @param  addr    目标地址（必须为 sockaddr_in）
 * @param  addrlen 地址结构体长度
 * @return 成功返回 0，失败返回 -1
 */
int esp8266_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    /*---- 参数校验 ----*/
    if (sockfd < 0 || sockfd >= FD_SOCKET_TABLE_SIZE)return -1;
    if (addr == NULL)                                      return -1;
    if (addrlen < (socklen_t)sizeof(struct sockaddr_in))   return -1;
    if (addr->sa_family != AF_INET)                        return -1;

    struct AT_Device *pdev = Get_AT_Device("esp8266");
    if (pdev == NULL) return -1;

    struct socket_t *pSocket = fd_socket_get(sockfd);
    if (pSocket == NULL) return -1;

    xSemaphoreTake(pSocket->send_lock, portMAX_DELAY);

    /*---- socket 状态检查 ----*/
    if (pSocket->open_flag != 1)
    {
        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /* 修复：防止重复 connect */
    if (pSocket->mode == SOCKET_CLIENT && pSocket->status == SOCKET_USED)
    {
        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /*---- 解析目标地址 ----*/
    const struct sockaddr_in *dest = (const struct sockaddr_in *)addr;
    uint16_t dest_port = ntohs(dest->sin_port);

    /* 修复：不用 inet_ntoa（非线程安全），手动格式化 IP */
    uint32_t ip_val = dest->sin_addr.s_addr;
    char remote_ip[16] = {0};
    snprintf(remote_ip, sizeof(remote_ip), "%u.%u.%u.%u",
             (ip_val >>  0) & 0xFFU,
             (ip_val >>  8) & 0xFFU,
             (ip_val >> 16) & 0xFFU,
             (ip_val >> 24) & 0xFFU);

    /*---- 修复：加dev_lock 后再抢占 link_id，防止并发竞态 ----*/
    xSemaphoreTake(pdev->dev_lock, portMAX_DELAY);

    int link_id = get_esp8266_unused_hw_socket();
    if (link_id < 0 || link_id >= ESP8266_SOCKET_NUM)
    {
        xSemaphoreGive(pdev->dev_lock);
        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /* 修复：先占位，防止 AT 命令发送期间被其他任务抢同一 link_id */
    pSocket->hw_socket = (uint32_t)link_id;
    pSocket->status    = SOCKET_USED;

    xSemaphoreGive(pdev->dev_lock);

    /*---- 组装并发送 AT+CIPSTART ----*/
    char cmd[96] = {0};
    int  ret     = -1;

    if (pSocket->socket_type == SOCK_STREAM)
    {
        /* TCP 连接 */
        snprintf(cmd, sizeof(cmd),
                 "AT+CIPSTART=%d,\"TCP\",\"%s\",%u\r\n",
                 link_id, remote_ip, dest_port);
        ret = at_send_cmd(pdev, cmd, NULL, NULL, 0, AT_TIMEOUT);
    }
    else if (pSocket->socket_type == SOCK_DGRAM)
    {
        /* UDP 连接：需要本地端口，若未bind 则默认使用 1234 */
        uint16_t src_port = ntohs(pSocket->local_addr.sin_port);
        if (src_port == 0) src_port = 1234U;

        /* 第5参数 0：目标地址固定，不允许随意更换对端 */
        snprintf(cmd, sizeof(cmd),
                 "AT+CIPSTART=%d,\"UDP\",\"%s\",%u,%u,0\r\n",
                 link_id, remote_ip, dest_port, src_port);
        ret = at_send_cmd(pdev, cmd, NULL, NULL, 0, AT_TIMEOUT);
    }
    else
    {
        /* socket_type 非法，防御性处理（正常流程不应到这里）*/
        ret = -1;
    }

    /*---- 修复：AT 命令失败时回滚已占位的 hw_socket ----*/
    if (ret != 0)
    {
        xSemaphoreTake(pdev->dev_lock, portMAX_DELAY);
        pSocket->hw_socket = 0xFFFFFFFFU;
        pSocket->status    = SOCKET_FREE;
        xSemaphoreGive(pdev->dev_lock);

        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /*---- 修复：AT 命令成功后，补全 socket 状态 ----*/
    pSocket->mode        = SOCKET_CLIENT;    /* 修复：原代码缺少 */
    pSocket->remote_addr = *dest;/* 修复：原代码缺少 */
    /* status/hw_socket 已在占位时写好，无需再写 */

    xSemaphoreGive(pSocket->send_lock);
    return 0;
}

/**
 * @brief  获取 ESP8266 的 CIPSTATUS 信息（多条连接）
 * @param  conns  输出数组，存放解析出的每条连接信息
 * @param  stat   输出全局 Wi‑Fi/连接状态码（0~5）
 * @return 实际存入 conns 的连接条数（>=0）
 *         -1 表示命令失败或无法解析状态行
 */
int esp8266_get_cipstatus(struct AT_Device *ptDev,cipstatus_t *conns, int *stat)
{
    int i;
    int conn_count = 0;
    int global_stat = -1;
    int ret = -1;

    /* 解析时可能用到的所有变量提前声明 */
    const char *line;
    int hw_socket;
    unsigned short remote_port, local_port;
    int tetype;
    char type_str[10];
    char ip_str[32];
    int parsed;
    unsigned long ip_bin;       /* inet_pton 需要 uint32_t 或 unsigned long */
    cipstatus_t *p;

    /* 参数校验 */
    if (!conns || !stat)
        return -1;

    *stat = -1;

    do {
        /* 1. 发送 AT+CIPSTATUS 命令 */
        if (at_send_cmd(ptDev, "AT+CIPSTATUS\r\n", NULL, NULL, 0, AT_TIMEOUT) != 0) {
            ptDev->resp_line_counts = 0;
            ptDev->resp_status = 0;
            memset(ptDev->resp_line_len, 0, sizeof(ptDev->resp_line_len));
            break;
        }

        /* 2. 解析第 0 行：STATUS:<state> */
        if (ptDev->resp_line_counts > 0) {
            if (sscanf((const char *)ptDev->resp[0], "STATUS:%d", &global_stat) != 1) {
                break;
            }
            *stat = global_stat;
        } else {
            break;
        }

        /* 3. 逐行解析 +CIPSTATUS: 连接条目，最多 MAX_CONNECTIONS 个 */
        for (i = 1;
             i < (int)ptDev->resp_line_counts && i < RESP_ROW_LEN
             && conn_count < ESP8266_SOCKET_NUM;
             i++)
        {
            line = (const char *)ptDev->resp[i];

            /* 跳过非连接行 */
            if (strncmp(line, "+CIPSTATUS:", 11) != 0)
                continue;

            /* 初始化临时变量 */
            hw_socket = -1;
            remote_port = 0;
            local_port = 0;
            tetype = 0;
            memset(type_str, 0, sizeof(type_str));
            memset(ip_str, 0, sizeof(ip_str));

            /* 标准格式：+CIPSTATUS:0,"TCP","192.168.0.10",51403,8080,1 */
            parsed = sscanf(line,
                            "+CIPSTATUS:%d,\"%9[^\"]\",\"%31[^\"]\",%hu,%hu,%d",
                            &hw_socket, type_str, ip_str,
                            &remote_port, &local_port, &tetype);
            if (parsed != 6)
                continue;

            /* 将点分十进制 IP 转为网络字节序 uint32_t */
            ip_bin = 0;
            if (inet_pton(AF_INET, ip_str, &ip_bin) != 1)
                continue;

            /* 填充数组当前元素 */
            p = &conns[conn_count];
            p->link_id     = hw_socket;
            p->remote_ip   = (uint32_t)ip_bin;
            p->remote_port = (uint16_t)remote_port;
            p->local_port  = (uint16_t)local_port;
            p->tetype      = tetype;
            p->type        = (strcmp(type_str, "UDP") == 0) ? SOCK_DGRAM : SOCK_STREAM;

            conn_count++;
        }

        ret = conn_count;

    } while (0);

    /* 4. 统一清理 AT 接收缓冲区 */
    ptDev->resp_line_counts = 0;
    ptDev->resp_status = 0;
    memset(ptDev->resp_line_len, 0, sizeof(ptDev->resp_line_len));

    return ret;
}

/**
* @brief  判断一个 socket 是否与 CIPSTATUS 条目匹配
* @param  sock  已跟踪的 socket
* @param  cip   CIPSTATUS 解析出的条目
* @return 1=匹配，0=不匹配
*/
static int socket_match_cipstatus(struct socket_t *sock, cipstatus_t *cip)
{
   if (!sock || !cip)
       return 0;

   /* link_id 必须一致 */
   if (sock->hw_socket != (uint32_t)cip->link_id)
       return 0;

   /* 远端 IP 必须一致（都是网络字节序） */
   if (sock->remote_addr.sin_addr.s_addr != cip->remote_ip)
       return 0;

   /* 远端端口：socket 中已经是网络字节序，cip 中是主机字节序 */
   if (sock->remote_addr.sin_port != htons(cip->remote_port))
       return 0;

   /* 本地端口 */
   if (sock->local_addr.sin_port != htons(cip->local_port))
       return 0;

   return 1;
}

static void socket_cleanup(struct AT_Device *ptDev, struct socket_t *sock)
{
    if (!sock || sock->status == SOCKET_FREE)
        return;

    if (sock->open_flag)
        fd_socket_free((int)sock->sockfd);

    if (sock->recv_queue)
        xQueueReset(sock->recv_queue);

    sock->open_flag   = 0;
    sock->status      = SOCKET_FREE;
    sock->mode        = SOCKET_UNKNOWN;
    sock->hw_socket   = 0;
    sock->socket_type = SOCK_UNDEF;
    memset(&sock->remote_addr, 0, sizeof(sock->remote_addr));
    memset(&sock->local_addr, 0, sizeof(sock->local_addr));
}

/**
 * @brief  接受一个客户端连接
 * @note   每次调用查询一次 CIPSTATUS，清理断开的，找到新连接就返回
 *         找不到返回 -1，调用者自行决定重试策略
 * @param  ptDev   AT 设备句柄
 * @param  sockfd  服务端套接字描述符
 * @param  addr    输出参数，客户端地址（可为 NULL）
 * @param  addrlen 输入/输出参数（可为 NULL）
 * @return 成功返回新连接的 fd（>= 0），无新连接返回 -1
 */
int esp8266_accept( int sockfd,
                   struct sockaddr *addr, socklen_t *addrlen)
{
	struct AT_Device *ptDev = Get_AT_Device("esp8266");
    volatile cipstatus_t cur[5];
    volatile int cur_count;
    int stat;
    int i, j;
    int matched;

    /* 1. 校验 server socket */
    struct socket_t *server = fd_socket_get(sockfd);
    if (!server)
        return -1;
    if (server->mode != SOCKET_SERVER)
        return -1;
    if (!server->open_flag)
        return -1;

    /* 2. 查询当前连接状态 */
    cur_count = esp8266_get_cipstatus(ptDev, cur, &stat);
    if (cur_count < 0)
        return -1;

    /* ============================================================
     * 3. 清理断开的连接
     * ============================================================ */
    for (i = 0; i < ESP8266_SOCKET_NUM; i++) {
        if (ptDev->sockets[i].status != SOCKET_USED)
            continue;
        if (ptDev->sockets[i].mode != SOCKET_CLIENT)
            continue;

        matched = 0;
        for (j = 0; j < cur_count; j++) {
            if (socket_match_cipstatus(&ptDev->sockets[i], &cur[j])) {
                matched = 1;
                break;
            }
        }

        if (!matched) {
            /* 推 EOF 通知 recv */
            if (ptDev->sockets[i].recv_queue) {
                uint8_t eof = 0;
                xQueueSend(ptDev->sockets[i].recv_queue, &eof, 0);
            }
            socket_cleanup(ptDev, &ptDev->sockets[i]);
        }
    }

    /* ============================================================
     * 4. 找第一个新连接并返回
     * ============================================================ */
    for (j = 0; j < cur_count; j++) {
        /* 只处理 server 接入的连接 */
        if (cur[j].tetype != 1)
            continue;

        /* 本地端口必须匹配当前 server */
        if (cur[j].local_port != ntohs(server->local_addr.sin_port))
            continue;

        /* 检查是否已被跟踪 */
        struct socket_t *existing = get_esp8266_socket_for_hw_socket(cur[j].link_id);
        if (existing != NULL) {
            /* link_id 存在且字段匹配，说明是旧连接，跳过 */
            if (socket_match_cipstatus(existing, &cur[j]))
                continue;
            /* 字段不匹配，旧的在第3步已清理，继续当新连接处理 */
        }

        /* 分配空闲 socket */
        struct socket_t *client = get_esp8266_unuse_fd_socket();
        if (!client)
            return -1;

        /* 初始化 client socket */
        client->status      = SOCKET_USED;
        client->open_flag   = 1;
        client->mode        = SOCKET_CLIENT;
        client->socket_type = cur[j].type;
        client->hw_socket   = (uint32_t)cur[j].link_id;

        client->remote_addr.sin_family      = AF_INET;
        client->remote_addr.sin_addr.s_addr = cur[j].remote_ip;
        client->remote_addr.sin_port        = htons(cur[j].remote_port);

        client->local_addr = server->local_addr;

        /* 创建接收队列和锁 */
        if (!client->recv_queue) {
            client->recv_queue = xQueueCreate(ESP8266_SOCKET_RX_QUEUE_SIZE,
                                              sizeof(uint8_t));
        }
        if (!client->recv_lock) {
            client->recv_lock = xSemaphoreCreateMutex();
        }
        if (!client->send_lock) {
            client->send_lock = xSemaphoreCreateMutex();
        }

        /* 分配 fd */
        int new_fd = fd_socket_alloc(client);
        if (new_fd < 0) {
            socket_cleanup(ptDev, client);
            return -1;
        }
        client->sockfd = (uint32_t)new_fd;

        /* 填充调用者的地址结构 */
        if (addr && addrlen) {
            if (*addrlen >= sizeof(struct sockaddr_in)) {
                struct sockaddr_in *sin = (struct sockaddr_in *)addr;
                memset(sin, 0, sizeof(*sin));
                sin->sin_family      = AF_INET;
                sin->sin_addr.s_addr = cur[j].remote_ip;
                sin->sin_port        = htons(cur[j].remote_port);
                *addrlen = sizeof(struct sockaddr_in);
            }
        }

        /* 找到一个就返回 */
        return new_fd;
    }

    /* 没有新连接 */
    return -1;
}


// /**
//  * @brief接受一个客户端连接
//  * @note   调用前 sockfd 必须已经 listen；
//  *         本函数会阻塞，直到有新客户端连入为止
//  * @param  sockfd   服务端套接字描述符
//  * @param  addr     输出参数，接收客户端地址（可为 NULL）
//  * @param  addrlen  输入/输出参数（可为 NULL）
//  * @return 成功返回新连接的 fd（>= 0），失败返回 -1
//  */
// int esp8266_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
// {
//         /*---- 轮询 AT+CIPSTATUS ----*/
//     /*然后取出第一行+CIPSTATUS连接信息解析*/
//     /*把所有连接信息都解析出来存起来*/
//     /*然后判断这一次解析的连接和上一次连接对比，是不是有哪一个link_id没了，如果没有了，则代表对应的socket客户端关闭了连接，释放关闭对应socket*/
//     /*遍历查看每条link_id的客户端socket信息如下*/
//     /*先看是tcp还是udp连接，如果udp，则直接跳过*/
//     /*linkid读取出来,也就是hw_socket,然后去根据这个linkid去查找是否这个连接是新的连接，也就是查找linkid是否有对应socket_t，然后看这个socket
//     是否被使用被打开*/
//     /*如果是旧的连接，则检查对应ip,port这些信息是否和对应socket里面的匹配，如果不匹配，代表换了另一个客户端连接，也是新连接，若是匹配则是旧的连接*/
//     /*如果是旧的连接，则跳过这一行，去处理下一行*/
//     /*如果是新的连接，看一下这个端口是否和源端口不相同，相同则代表不符合，否则代表符合规则，则创建一个客户端socket，然后把对应ip，端口，hw_socket信息，目的端口存入这个socket_t,然后返回 这个socket_t的fd*/
// // AT+CIPSTATUS
// // STATUS:3
// // +CIPSTATUS:0,"TCP","192.168.0.152",51403,8080,1
// // +CIPSTATUS:1,"UDP","0.0.0.0",10482,4444,0
// // +CIPSTATUS:2,"TCP","192.168.0.152",51374,8080,1
// // +CIPSTATUS:3,"TCP","192.168.0.152",51473,8080,1
// // +CIPSTATUS:4,"TCP","192.168.0.152",51498,8080,1
//     // 解析返回数据
//     // STATUS:<stat>
//     // +CIPSTATUS:<link	ID>,<type>,<remote	IP>,<remote	port>,<local port>,<tetype>
//     /*---- 轮询 AT+CIPSTATUS ----*/
//     struct AT_Device   *ptDev         = NULL;
//     struct socket_t    *pServerSocket = NULL;
//     struct socket_t    *pExist        = NULL;
//     struct socket_t    *ptSocket      = NULL;
//     struct sockaddr_in *ptServerAddr  = NULL;
//     struct sockaddr_in *pExistRemote  = NULL;
//     struct sockaddr_in *pLocalAddr    = NULL;
//     struct sockaddr_in  client_addr;
//     const char         *line          = NULL;
//     char                type[10]      = {0};
//     char                remote_ip[32] = {0};
//     char                exist_ip[32]  = {0};
//     uint16_t            server_port   = 0;
//     uint16_t            remote_port   = 0;
//     uint16_t            local_port    = 0;
//     int                 i             = 0;
//     int                 hw_socket     = -1;
//     int                 parsed        = 0;
//     int                 sw_socket     = -1;
//     int                 tetype        = 0;
//     //struct UART_Device *puart = Get_UART_Device("stm32_f4_uart1");
//     //puart->UART_Init(puart, 0,0,0,0);
//     memset(&client_addr, 0, sizeof(client_addr));
//     /* ---- 参数校验 ---- */
//     if (sockfd < 0 || sockfd >= FD_SOCKET_TABLE_SIZE)
//         return -1;

//     ptDev = Get_AT_Device("esp8266");
//     if (ptDev == NULL)
//         return -1;

//     pServerSocket = fd_socket_get(sockfd);
//     if (pServerSocket == NULL)
//         return -1;
//     if(pServerSocket->hw_socket != ESP8266_SOCKET_SERVER_ID)
//     {
//         //puart->UART_Send(puart,(uint8_t *)&(pServerSocket->hw_socket),4,1000);
//         return -1;
//     }
//     if (pServerSocket->open_flag != 1 || pServerSocket->mode != SOCKET_SERVER)
//         return -1;

//     ptServerAddr = (struct sockaddr_in *)&pServerSocket->local_addr;
//     server_port  = ntohs(ptServerAddr->sin_port);
//     xSemaphoreTake(pServerSocket->send_lock, portMAX_DELAY);// server socket 自己的锁
//     /* ---- 轮询 AT+CIPSTATUS，阻塞直到有新客户端接入 ---- */
//     while (1)
//     {
//         /* 服务端已被关闭则退出 */
//         if (pServerSocket->open_flag != 1)
//         {
//             xSemaphoreGive(pServerSocket->send_lock);
//             return -1;
//         }

//         if (at_send_cmd(ptDev, "AT+CIPSTATUS\r\n", NULL, NULL, 0, AT_TIMEOUT) != 0)
//         {
//             ptDev->resp_line_counts = 0;
//             ptDev->resp_status      = 0;
//             memset(ptDev->resp_line_len, 0, sizeof(ptDev->resp_line_len));
//             vTaskDelay(pdMS_TO_TICKS(200));
//             continue;
//         }
//         memset(g_link_id, 0, sizeof(g_link_id));
//         /* 逐行解析，第 0 行是 STATUS:<stat>，从第 1 行开始 */
//         for (i = 1; i < (int)ptDev->resp_line_counts && i < RESP_ROW_LEN; i++)
//         {
//             //puart->UART_Send(puart,(uint8_t *)(ptDev->resp[i]),ptDev->resp_line_len[i],1000);
//             line = (const char *)ptDev->resp[i];

//             if (strncmp(line, "+CIPSTATUS:", 11) != 0)
//                 continue;

//             /* 重置本行相关变量 */
//             hw_socket   = -1;
//             remote_port = 0;
//             local_port  = 0;
//             tetype      = 0;
//             memset(type,      0, sizeof(type));
//             memset(remote_ip, 0, sizeof(remote_ip));

//             /* 格式：+CIPSTATUS:0,"TCP","192.168.0.10",51403,8080,1 */
//             parsed = sscanf(line,
//                 "+CIPSTATUS:%d,\"%9[^\"]\",\"%31[^\"]\",%hu,%hu,%d",
//                 &hw_socket, type, remote_ip, &remote_port, &local_port, &tetype);

//             if (parsed != 6)
//             {
//                 continue;
//             }

//             /* 跳过 UDP */
//             if (strcmp(type, "UDP") == 0)
//                 continue;

//             /*
//              * tetype == 1：外部客户端主动连入（服务端视角）
//              * tetype == 0：ESP8266 自己作为客户端连出，不处理
//              */
//             if (tetype != 1)
//             {
//                 continue;
//             }

//             /* 只处理本服务端口上的连接 */
//             if (local_port != server_port)
//             {
//                 continue;
//             }

//             /* hw_socket 范围校验，ESP8266 最多 0~4*/
//             if (hw_socket < 0 || hw_socket >= ESP8266_SOCKET_NUM)
//             {
//                 continue;
//             }
//             g_link_id[hw_socket] = 1;
// //            puart->UART_Send(puart,"test1",5,1000);
// //            puart->UART_Send(puart,(uint8_t *)&hw_socket,4,1000);
//             /*
//              * 判断新旧连接：
//              *   对应 socket_t 不存在或open_flag==0   → 新连接
//              *   存在且打开，但 IP/port 不匹配          → link_id 被复用，新连接
//              *   存在且打开，IP/port 完全匹配           → 旧连接，跳过
//              */
//             pExist = get_esp8266_socket_for_hw_socket(hw_socket);
//             if (pExist != NULL && pExist->open_flag == 1)
//             {
//                 pExistRemote = (struct sockaddr_in *)&pExist->remote_addr;
//                 memset(exist_ip, 0, sizeof(exist_ip));
//                 inet_ntop(AF_INET, &pExistRemote->sin_addr, exist_ip, sizeof(exist_ip));

//                 if (ntohs(pExistRemote->sin_port) == remote_port &&
//                     strcmp(exist_ip, remote_ip) == 0)
//                 {
//                     continue;/* 旧连接，跳过 */
//                 }
//             }
//            puart->UART_Send(puart,(uint8_t *)&hw_socket,4,1000);
//            puart->UART_Send(puart,remote_ip,12,1000);
//            puart->UART_Send(puart,"\r\n",2,1000);
//             /* ---------- 找到新连接 ---------- */
//             xSemaphoreGive(pServerSocket->send_lock);
//             /* 先保存连接信息到栈变量（释放锁后仍可用） */
//             client_addr.sin_family = AF_INET;
//             client_addr.sin_port   = htons(remote_port);
//             inet_pton(AF_INET, remote_ip, &client_addr.sin_addr);

//             /* 清理响应缓冲区 */
//             ptDev->resp_line_counts = 0;
//             ptDev->resp_status      = 0;
//             memset(ptDev->resp_line_len, 0, sizeof(ptDev->resp_line_len));

//             /*
//              * 先释放 send_lock，再调用 esp8266_socket()。
//              */

//             sw_socket = esp8266_socket(AF_INET, SOCK_STREAM, 0);
//             if (sw_socket < 0 || sw_socket >= FD_SOCKET_TABLE_SIZE)
//             {
//                 return -1;
//             }

//             ptSocket = fd_socket_get(sw_socket);
//             if (ptSocket == NULL)
//             {
//                 /* esp8266_socket() 已分配了槽位，必须释放，否则泄漏 */
//                 esp8266_close(sw_socket);
//                 return -1;
//             }
//             /* 获取客户端 socket 的 send_lock*/
//             xSemaphoreTake(ptSocket->send_lock, portMAX_DELAY);
//             /* 填写远端地址 */
//             memcpy(&ptSocket->remote_addr, &client_addr, sizeof(struct sockaddr_in));

//             /* 填写本端地址 */
//             pLocalAddr             = (struct sockaddr_in *)&ptSocket->local_addr;
//             pLocalAddr->sin_family = AF_INET;
//             pLocalAddr->sin_port   = htons(local_port);

//             /* 绑定硬件 link_id，标记为已打开的客户端 socket */
//             ptSocket->hw_socket = (uint32_t)hw_socket;
//             ptSocket->open_flag = 1;
//             ptSocket->status    = SOCKET_USED;
//             ptSocket->mode      = SOCKET_CLIENT;

//             /* 回填调用方输出参数 */
//             if (addr != NULL)
//                 memcpy(addr, &client_addr, sizeof(struct sockaddr_in));
//             if (addrlen != NULL)
//                 *addrlen = (socklen_t)sizeof(struct sockaddr_in);
//             xSemaphoreGive(ptSocket->send_lock);
//             return sw_socket;
//         }

//         /* 本轮未找到新连接，清缓冲区后等待重试 */
//         ptDev->resp_line_counts = 0;
//         ptDev->resp_status      = 0;
//         memset(ptDev->resp_line_len, 0, sizeof(ptDev->resp_line_len));
//         vTaskDelay(pdMS_TO_TICKS(200));
//     }
// }


/**
 * @brief  发送数据
 * @note   ESP8266 单次 AT+CIPSEND 最大 2048 字节，本函数自动分包；
 *         返回值为实际发送字节数，与POSIX send() 语义一致
 * @param  sockfd  套接字描述符
 * @param  buf     发送缓冲区
 * @param  len     要发送的字节数
 * @param  flags   保留，暂未使用
 * @return 成功返回实际发送字节数（>= 0），失败返回 -1
 */
#define ESP8266_SEND_MAX_LEN  2048U

int esp8266_send(int sockfd, const void *buf, size_t len, int flags)
{
    (void)flags;

    if (sockfd < 0 || sockfd >= FD_SOCKET_TABLE_SIZE) return -1;
    if (buf == NULL || len == 0) return (buf == NULL) ? -1 : 0;

    struct AT_Device *pdev = Get_AT_Device("esp8266");
    if (pdev == NULL) return -1;

    struct socket_t *psocket = fd_socket_get(sockfd);
    if (psocket == NULL) return -1;

    xSemaphoreTake(psocket->send_lock, portMAX_DELAY);

    if (psocket->open_flag != 1 ||
        psocket->status    != SOCKET_USED ||
        psocket->hw_socket == 0xFFFFFFFFU)
    {
        xSemaphoreGive(psocket->send_lock);
        return -1;
    }

    const uint8_t *ptr      = (const uint8_t *)buf;
    size_t         remaining = len;
    size_t         sent      = 0;
    char           cmd[48]   = {0};

    while (remaining > 0)
    {
        size_t chunk = (remaining > ESP8266_SEND_MAX_LEN)? ESP8266_SEND_MAX_LEN : remaining;

        /* 第一步：发 AT+CIPSEND 指令，不管返回 */
        snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u,%u\r\n",
                 (unsigned)psocket->hw_socket,
                 (unsigned)chunk);
        at_send_cmd(pdev, cmd, NULL, NULL, 0, AT_TIMEOUT);

        /* 第二步：按字节长度发原始数据，等 "SEND OK" */
        int ret = at_send_data(pdev, ptr + sent, chunk, AT_TIMEOUT);
        if (ret != 0)
        {
            xSemaphoreGive(psocket->send_lock);
            return -1;
        }

        sent      += chunk;
        remaining -= chunk;
    }

    xSemaphoreGive(psocket->send_lock);
    return (int)sent;
}


/**
 * @brief 接收数据（TCP） 
 * @param sockfd 套接字描述符
 * @param buf    接收缓冲区指针
 * @param len    缓冲区长度（字节）
 * @param flags  标志位，通常为 0
 * @return 成功返回实际接收字节数，连接关闭返回 0，失败返回 -1
 */
ssize_t esp8266_recv(int sockfd, void *buf, size_t len, int flags)
{
    uint8_t *cmd = (uint8_t *)buf;
    ssize_t recv_len = 0;
    struct socket_t *pSocket = fd_socket_get(sockfd);
    struct UART_Device* puart1 =Get_UART_Device("stm32_f4_uart1");
    if (pSocket == NULL)
    {
        return -1;
    }
    xSemaphoreTake(pSocket->recv_lock, portMAX_DELAY);
    uint8_t byte;
    while (recv_len < (ssize_t)len &&
           pdTRUE == xQueueReceive(pSocket->recv_queue, &byte, 0))
    {
        cmd[recv_len++] = byte;
    }
   puart1->UART_Send(puart1, cmd, recv_len, 1000);
    return recv_len;
}

/**
 * @brief发送数据到指定地址（UDP）
 * @note若socket尚未绑定硬件 link_id，自动以 mode=2 启动 UDP 连接
 *        （mode=2 允许每次AT+CIPSEND 指定不同的目标地址，适合 sendto 语义）；
 *        单次最大 2048 字节，超过自动分包。
 * @param sockfd    套接字描述符
 * @param buf       发送缓冲区指针
 * @param len       发送数据长度（字节）
 * @param flags     保留，暂未使用
 * @param dest_addr 目标地址结构体指针（必须为 sockaddr_in）
 * @param addrlen   目标地址结构体长度
 * @return 成功返回实际发送字节数，失败返回 -1
 */
ssize_t esp8266_sendto(int sockfd, const void *buf, size_t len, int flags,
                       const struct sockaddr *dest_addr, socklen_t addrlen)
{
    (void)flags;

    /*---- 参数校验 ----*/
    if (sockfd < 0 || sockfd >= FD_SOCKET_TABLE_SIZE)return -1;
    if (buf == NULL)return -1;
    if (len == 0)                                         return 0;
    if (dest_addr == NULL)                                return -1;
    if (addrlen < (socklen_t)sizeof(struct sockaddr_in)) return -1;
    if (dest_addr->sa_family != AF_INET)                  return -1;

    struct AT_Device *pdev = Get_AT_Device("esp8266");
    if (pdev == NULL) return -1;

    struct socket_t *psocket = fd_socket_get(sockfd);
    if (psocket == NULL) return -1;

    xSemaphoreTake(psocket->send_lock, portMAX_DELAY);

    /*---- socket 状态检查 ----*/
    if (psocket->open_flag != 1){
        xSemaphoreGive(psocket->send_lock);
        return -1;
    }

    /* sendto 仅用于 UDP */
    if (psocket->socket_type != SOCK_DGRAM)
    {
        xSemaphoreGive(psocket->send_lock);
        return -1;
    }

    /*---- 解析目标地址 ----*/
    const struct sockaddr_in *dest = (const struct sockaddr_in *)dest_addr;
    uint16_t dest_port = ntohs(dest->sin_port);
    uint32_t ip_val    = dest->sin_addr.s_addr;
    char remote_ip[16] = {0};
    snprintf(remote_ip, sizeof(remote_ip), "%u.%u.%u.%u",
             (ip_val >>  0) & 0xFFU,
             (ip_val >>  8) & 0xFFU,
             (ip_val >> 16) & 0xFFU,
             (ip_val >> 24) & 0xFFU);

    /*---- 若尚未分配硬件 link_id，先执行 AT+CIPSTART ----*/
    /* mode=2：允许每次 AT+CIPSEND 指定不同目标地址，符合 sendto 无连接语义 */
    if (psocket->hw_socket == 0xFFFFFFFFU)
    {
        xSemaphoreTake(pdev->dev_lock, portMAX_DELAY);

        int link_id = get_esp8266_unused_hw_socket();
        if (link_id < 0)
        {
            xSemaphoreGive(pdev->dev_lock);
            xSemaphoreGive(psocket->send_lock);
            return -1;
        }

        /* 先占位，防止并发任务抢同一 link_id */
        psocket->hw_socket = (uint32_t)link_id;
        psocket->status    = SOCKET_USED;
        xSemaphoreGive(pdev->dev_lock);

        /* 本地源端口：若已bind 则使用绑定端口，否则默认 1234 */
        uint16_t src_port = ntohs(psocket->local_addr.sin_port);
        if (src_port == 0) src_port = 1234U;

        char cipstart_cmd[96] = {0};
        snprintf(cipstart_cmd, sizeof(cipstart_cmd),
                 "AT+CIPSTART=%d,\"UDP\",\"%s\",%u,%u,2\r\n",
                 link_id, remote_ip, dest_port, src_port);

        if (at_send_cmd(pdev, cipstart_cmd, NULL, NULL, 0, AT_TIMEOUT) != 0)
        {
            /* AT命令失败，回滚占位*/
            xSemaphoreTake(pdev->dev_lock, portMAX_DELAY);
            psocket->hw_socket = 0xFFFFFFFFU;
            psocket->status    = SOCKET_FREE;
            xSemaphoreGive(pdev->dev_lock);

            xSemaphoreGive(psocket->send_lock);
            return -1;
        }

        psocket->mode = SOCKET_CLIENT;
    }

    /*---- 分包发送 ----*/
    /* AT+CIPSEND=<link_id>,<len>,"<remote_ip>",<remote_port>* mode=2 下每包均可指定目标地址，支持 sendto 每次发往不同地址 */
    const uint8_t *ptr      = (const uint8_t *)buf;
    size_t         remaining = len;
    size_t         sent      = 0;
    char           cmd[64]   = {0};

    while (remaining > 0)
    {
        size_t chunk = (remaining > ESP8266_SEND_MAX_LEN)? ESP8266_SEND_MAX_LEN : remaining;

        snprintf(cmd, sizeof(cmd),
                 "AT+CIPSEND=%u,%u,\"%s\",%u\r\n",
                 (unsigned)psocket->hw_socket,
                 (unsigned)chunk,
                 remote_ip,
                 (unsigned)dest_port);

        /* 发送 AT 指令，等待 '>' 提示符（与 esp8266_send 保持一致） */
        at_send_cmd(pdev, cmd, NULL, NULL, 0, AT_TIMEOUT);

        /* 发送实际数据 */
        if (at_send_data(pdev, ptr + sent, chunk, AT_TIMEOUT) != 0)
        {
            xSemaphoreGive(psocket->send_lock);
            /* 已发出部分数据则返回已发字节数，否则返回 -1 */
            return (sent > 0) ? (ssize_t)sent : -1;
        }

        sent      += chunk;
        remaining -= chunk;
    }

    xSemaphoreGive(psocket->send_lock);
    return (ssize_t)sent;
}


/**
 * @brief 接收数据并获取来源地址（UDP）
 * @param sockfd   套接字描述符
 * @param buf      接收缓冲区指针
 * @param len      缓冲区长度（字节）
 * @param flags    标志位，通常为 0
 * @param src_addr [out] 来源地址结构体指针，可为 NULL
 * @param addrlen  [out] 来源地址长度，可为 NULL
 * @return 成功返回实际接收字节数，失败返回 -1
 */
ssize_t esp8266_recvfrom(int sockfd, void *buf, size_t len, int flags,
                         struct sockaddr *src_addr, socklen_t *addrlen)
{
    // TODO: 实现UDP接收功能，获取来源地址信息
//    if (psocket == NULL) return -1;
//    if (psocket->socket_type != SOCK_DGRAM) return -1;
//    if (psocket->open_flag != 1) return -1;
//    if (psocket->hw_socket == 0xFFFFFFFFU) return -1;
//    struct socket_t *psocket = fd_socket_get(sockfd);
//    uint8_t *pbuf = (uint8_t *)buf;
//    ssize_t len = 0;
//    while(pdTRUE == xQueueReceive(psocket->recv_queue, pbuf[len++], 0));
//    src_addr = psocket->remote_addr;
//    return len;
	return -1;
}





