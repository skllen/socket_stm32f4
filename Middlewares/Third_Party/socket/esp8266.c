#include "esp8266.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stdlib.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "at_device.h"
#include "cmsis_os2.h"

#define ESP8266_OK          0
#define ESP8266_ERROR       1
#define ESP8266_TIMEOUT     2
#define ESP8266_BUSY        3

#define ESP8266_RX_BUF_SIZE         1024 /*recv_task 缓冲数组大小*/           
#define ESP8266_SOCKET_QUEUE_SIZE   1024 /*每条socket 队列的大小,单位字节*/
#define RECV_NOTIFILE_COUNTING      10  /*每条socket 接收数据完成信号量通知数量*/

#ifndef AF_INET
#define AF_INET 2
#endif

#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif

#define AT_TIMEOUT 5000

#define ESP8266_SOCKET_NUM  (5)   /* socket 0 ~ 4，4个客户端socket_t/ 一个作为服务器socket_t*/
#define ESP8266_UART_NAME              "stm32_f4_uart2"

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

// +IPD,<link_id>,<len>,<remote_ip>,<remote_port>:<data>
typedef struct
{
    uint8_t  link_id;       // ESP8266 硬件连接号
    uint16_t data_len;      // 数据长度

    uint8_t  has_remote;    // 是否解析到了远端 IP 和端口
    uint32_t remote_ip;     // 远端 IP，建议网络字节序
    uint16_t remote_port;   // 远端端口，主机字节序
} ESP8266_IPD_Info;

typedef struct
{
    uint8_t  link_id;
    uint8_t  flags;
    uint16_t data_len;

    uint32_t remote_ip;      /* 网络字节序 */
    uint16_t remote_port;    /* 主机字节序 */
} ESP8266_RecvHeader;

static struct AT_Device g_esp8266ATDevice;
//static struct socket_t g_esp8266_sockets[ESP8266_SOCKET_NUM];

static TaskHandle_t  g_start_recv_handle;
void Start_RECV_Task(void *argument);

/**********************************************************************
 * 函数名称： get_netdev
 * 功能描述： 获得网卡设备
 * 输入参数： 无
 * 输出参数： 无
 * 返 回 值： AT_Device结构体指针
 * 修改日期：	版本号	  修改人 	  修改内容
 * -----------------------------------------------
 * 2024/09/01		 V1.0	  韦东山 	  创建
 ***********************************************************************/
struct AT_Device * get_netdev(void)
{
	return &g_esp8266ATDevice;
}

/**********************************************************************
 * 函数名称： w800_setsockopt
 * 功能描述： 设置socket参数, 比如设置recv、send函数的超时时间
 * 输入参数： socket/level/optname/optval/optlen - 网络参数
 * 输出参数： 无
 * 返 回 值： (0)-成功
 * 修改日期：	版本号	  修改人 	  修改内容
 * -----------------------------------------------
 * 2024/11/15		 V1.0	  韦东山 	  创建
 ***********************************************************************/
int esp8266_setsockopt(int socket, int level, int optname, const void *optval, socklen_t optlen)
{
	uint32_t timeout = *((uint32_t *)optval);
	struct AT_Device * ptDev = get_netdev();

	if (optname == SO_RCVTIMEO)
		ptDev->sockets[socket].recv_timeout = timeout;
	if (optname == SO_SNDTIMEO)
		ptDev->sockets[socket].send_timeout = timeout;

	return 0;
}
/**********************************************************************
 * 函数名称： esp8266_gethostbyname
 * 功能描述： 解析域名得到IP
 * 输入参数： addr - URL域名
 * 输出参数： 无
 * 返 回 值： 32bit ip地址
 *            以IP 192.168.1.49为例, 返回的32bit IP里最高字节是192
 * 修改日期：	版本号	  修改人 	  修改内容
 * -----------------------------------------------
 * 2024/11/15		 V1.0	  韦东山 	  创建
 ***********************************************************************/
uint32_t esp8266_gethostbyname(char *addr)
{
	 uint8_t buf[100];
	uint8_t resp[64];
	int err;
	struct AT_Device * ptDev = get_netdev();
	uint32_t resp_len;
	int a, b, c, d;
	uint32_t ipaddr;
    if((100 - 13) < strlen(addr))
    {
        return 0;
    }
		
	/* 构造AT命令(查询ip) */
//	sprintf((char *)buf, "AT+CIPDOMAIN=%s\r\n", addr);
		snprintf((char *)buf, sizeof(buf),"AT+CIPDOMAIN=\"%s\"\r\n", addr);

	/* 执行AT命令 */
	err = at_send_cmd(ptDev, (char *)buf, (uint8_t *)resp, &resp_len,  sizeof(resp), AT_TIMEOUT);

	if (err)
	{
		return 0;
	}

	/* 解析得到IP */
	sscanf((const char *)resp, "+CIPDOMAIN:%d.%d.%d.%d", &a, &b, &c, &d);

	ipaddr = ((uint32_t)a<<24) | ((uint32_t)b<<16) | ((uint32_t)c<<8) | ((uint32_t)d);
	return ipaddr;
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
    struct AT_Device *ptDev = get_netdev();

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

static struct socket_t *get_esp8266_unuse_fd_socket(void)
{
    int i;
    struct AT_Device *pDev = get_netdev();
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
    struct AT_Device *ptDev = get_netdev();
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
int esp8266_flush(int socket)
{
	int i = 0;
	uint8_t dummy;
    struct socket_t *pSocket = fd_socket_get(socket);
    if(pSocket == NULL)
    {
        return -1;
    }

	/* 先读取数据 */
	while (1)
	{
		if (pdPASS != xQueueReceive(pSocket->recv_queue, &dummy, 0))
			break;
		i++;
	}

	return i;
}


int esp8266_init(char *uart_dev)
{
    /*初始化g_esp8266 sockets 状态*/
    /*初始化g_esp8266 sockets的互斥量队列*/
	/*初始化g_esp8266 发送,resp互斥锁，以及内部数值*/
    struct AT_Device *pdev = get_netdev();
    struct socket_t  *psocket;
    /*全部清零*/
    memset(pdev, 0, sizeof(struct AT_Device));
	  //pdev->sockets = g_esp8266_sockets;
    pdev->dev_lock = xSemaphoreCreateMutex();
    pdev->send_lock   = xSemaphoreCreateMutex();
    pdev->at_resp_sem = xSemaphoreCreateBinary();
    pdev->in_cmd = 0;
    if (pdev->dev_lock ==NULL || pdev->send_lock ==NULL || pdev->at_resp_sem ==NULL)
        return -1;
    printf("esp8266_init\r\n");
    for(int i =0 ;i < ESP8266_SOCKET_NUM; i++)
    {
        psocket = &pdev->sockets[i];
        psocket->sockfd     = -1;
		psocket->hw_socket  = 0xFFFFFFFFU;
        psocket->status = SOCKET_FREE;
        psocket->recv_lock = xSemaphoreCreateCounting(RECV_NOTIFILE_COUNTING,0);
        psocket->send_lock = xSemaphoreCreateMutex();
        psocket->recv_queue = xQueueCreate(ESP8266_SOCKET_QUEUE_SIZE,sizeof(uint8_t));
        psocket->mode      = SOCKET_UNKNOWN;
        psocket->recv_timeout = 1000;
        psocket->open_flag = 0;
				psocket->send_timeout = 1000;
        if(psocket->recv_lock == NULL || psocket->recv_queue == NULL || psocket->send_lock == NULL)
            return -1;
    }
    pdev->puart       = Get_UART_Device(uart_dev);//获取串口
    pdev->puart->UART_Init( pdev->puart, 115200, 8, 1, 0); //初始化串口

    xTaskCreate(
        Start_RECV_Task,       // 函数指针, 任务函数
        "Start_recv_task",     // 任务的名字
        AT_PARSER_TASK_STACK_SIZE,                   // 栈大小,单位为word,10表示40字节
        pdev,                  // 调用任务函数时传入的参数
        osPriorityNormal+1,                     // 优先级
        &g_start_recv_handle); // 任务句柄, 以后使用它来操作这个任务
                               //   err = at_send_cmd(pdev, "AT+RST\r\n", NULL, NULL, 0, 15000);
                               //   while (err);
    at_send_cmd(pdev, "AT+RST\r\n", NULL, NULL, 0, AT_TIMEOUT);
    vTaskDelay(2000);
    return 0;     
}

void test_socket_tcp_server_task(void)
{
    struct UART_Device *puart1 = Get_UART_Device("stm32_f4_uart1");
    puart1->UART_Init(puart1, 115200, 8, 1, 0);
    int socket_server_fd = esp8266_socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr;
    socklen_t server_addr_len;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(80);
    inet_pton(AF_INET, "192.168.0.152", &server_addr.sin_addr);
    server_addr_len = sizeof(server_addr);
    esp8266_bind(socket_server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    esp8266_listen(socket_server_fd, 10);
    uint8_t buf[256];
    int data_len;
    int client_fd ;
		
		while(1)
		{
						client_fd = esp8266_accept(socket_server_fd, (struct sockaddr *)&server_addr, &server_addr_len);
            if(client_fd < 0)
						{
							printf("client_fd %d/r/n",client_fd);
							vTaskDelay(1000);
							continue;
						}
						while(1)
						{
							printf("client_fd %d/r/n",client_fd);
							data_len = esp8266_recv(client_fd, buf, 256, 0);
							if(data_len <=0)
							{
								continue;
							}
								puart1->UART_Send(puart1, buf, data_len, 100);
							printf("\r\n");
								esp8266_send(client_fd,buf,data_len,0);
						
//								err = esp8266_close(client_fd);
//							if(err !=0)
//								printf("err %d \r\n",err);
							break;
						}
		}
}

#define HTTP_PORT 80
/*----内嵌页面 ---- */
static const char HTML_PAGE[] ="HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>LED</title>"
    "<style>body{font-family:sans-serif;text-align:center;padding:40px}"
    "button{padding:12px 28px;font-size:1em;margin:8px;border:none;border-radius:8px;cursor:pointer}"
    "#status{font-size:1.2em;margin:16px}</style></head>"
    "<body><h2>LED Control</h2>"
    "<div id='status'>-</div>"
    "<button style='background:#2ecc71' onclick='ctrl(\"on\")'>ON</button>"
    "<button style='background:#e74c3c' onclick='ctrl(\"off\")'>OFF</button>"
    "<script>"
    "function ctrl(m){"
    "  fetch('/led',{method:'POST',"
    "    headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "    body:'mode='+m})"
    "  .then(r=>{if(r.ok)document.getElementById('status').textContent='LED: '+m.toUpperCase();});"
    "}"
    "</script></body></html>";

static const char RESP_OK[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";

static const char RESP_400[] =
    "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nBad Request";

/* ---- 接收完整HTTP请求头 ---- */
static int recv_request(int fd, char *buf, int buf_size)
{
    int total = 0;
    uint32_t timeout_ms = 3000;
    setsockopt(fd, 0, SO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));

    while (total < buf_size - 1) {
        int n = recv(fd, buf + total, buf_size - 1 - total, 0);
        if (n > 0) {
            total += n;
            buf[total] = '\0';
            if (strstr(buf, "\r\n\r\n"))
                return total;   // 头部接收完整
        } else {
            break;              // 超时或断开
        }
    }
    return total;
}

/* ---- 处理一次连接 ---- */
static void handle_client(int fd)
{
    char buf[512];
    int len = recv_request(fd, buf, sizeof(buf));

    if (len <= 0) {
        send(fd, RESP_400, strlen(RESP_400), 0);
        return;
    }

    if (strncmp(buf, "POST /led", 9) == 0) {
        char *body = strstr(buf, "\r\n\r\n");
        if (body) {
            body += 4;
            if (strstr(body, "mode=on")) {
                HAL_GPIO_WritePin(GPIOF, GPIO_PIN_9, GPIO_PIN_SET);
                printf("LED ON\r\n");
            } else if (strstr(body, "mode=off")) {
                HAL_GPIO_WritePin(GPIOF, GPIO_PIN_9, GPIO_PIN_RESET);
                printf("LED OFF\r\n");
            }
        }
        send(fd, RESP_OK, strlen(RESP_OK), 0);
    } else {
        send(fd, HTML_PAGE, strlen(HTML_PAGE), 0);
    }
}

/* ---- FreeRTOS 任务 ---- */
void http_server_task(void *arg)
{
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(HTTP_PORT),
        .sin_addr.s_addr = 0
    };

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, 5);
    printf("HTTP server on port %d\r\n", HTTP_PORT);

    for (;;) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

        handle_client(cli);
        closesocket(cli);
    }
}



void Client_TCP_Task(void * parm)
{
	int len;
	    char buf[256] = {0};
	   int sockfd = esp8266_socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(1883);
    inet_pton(AF_INET, "192.168.0.152", &server_addr.sin_addr);
    esp8266_connect(sockfd,(const struct sockaddr *)&server_addr,sizeof(server_addr));
    while(1)
    {
        if(0 >= (len =esp8266_recv(sockfd,buf,sizeof(buf),0)))
        {
					 vTaskDelay(100);
					continue;

        }
        printf("recv:%s\r\n",buf);
        esp8266_send(sockfd, buf, len, 0);
    }
}

int esp8266_wifi_connect(char *ssid, char *password)
{
    struct AT_Device *pdev = get_netdev();

    esp8266_init("stm32_f4_uart2");
	esp8266_connect_ap(ssid,password);
		    xTaskCreate(
        http_server_task,       // 函数指针, 任务函数
        "Client_TCP_Task",     // 任务的名字
        AT_PARSER_TASK_STACK_SIZE,                   // 栈大小,单位为word,10表示40字节
        pdev,                  // 调用任务函数时传入的参数
        osPriorityNormal,                     // 优先级
        NULL); // 任务句柄, 以后使用它来操作这个任务
                               //   err = at_send_cmd(pdev, "AT+RST\r\n", NULL, NULL, 0, 15000);
                               //   while (err);

	// ip = esp8266_gethostbyname("www.baidu.com");
		
    // extern int esp8266_tcp_server_test_start(struct AT_Device *pdev);
    // err = esp8266_tcp_server_test_start(pdev);
    //     if (err != 0)
    // {
    //     return err;
    // }
	return 0;
}

/**
 * @brief  查询 WiFi 连接状态，并可选获取 SSID
 * @param  ssid_buf  输出缓冲区，填NULL 则不获取 SSID
 * @param  ssid_len  缓冲区大小
 * @return 1 = 已连接，0 = 未连接，-1 = AT 通信失败
 */
int esp8266_get_wifi_status(char *ssid_buf, ssize_t ssid_len)
{
    struct AT_Device *pdev = get_netdev();
    if (pdev == NULL) return -1;

    uint8_t resp[128] = {0};
    uint32_t resp_len = 0;

    int err = at_send_cmd(pdev, "AT+CWJAP?\r\n",
                          resp, &resp_len, sizeof(resp), AT_TIMEOUT);
    if (err != 0) return -1;

    const char *p = strstr((const char *)resp, "+CWJAP:");
    if (p == NULL){
        /* 未连接 */
        return (strstr((const char *)resp, "No AP") != NULL) ? 0 : -1;
    }

    /* 解析 SSID：+CWJAP:"ssid",... */
    if (ssid_buf != NULL && ssid_len > 0)
    {
        memset(ssid_buf, 0, ssid_len);
        sscanf(p, "+CWJAP:\"%63[^\"]\"", ssid_buf); /* 最多63字符 */
    }

    return 1;
}

/**********************************************************************
 * 函数名称： esp8266_connect_ap
 * 功能描述： 连接WIFI AP
 * 输入参数： ssid   - AP名称
 *            passwd - 密码
 * 输出参数： 无
 * 返 回 值： 0-成功, (-1)-失败
 * 修改日期：	版本号	  修改人 	  修改内容
 ***********************************************************************/
int esp8266_connect_ap(char *ssid, char *passwd)
{
    char cmd[128];
	uint32_t local_ip;
    int err = -1;
    struct AT_Device *pdev = get_netdev();
    err = at_send_cmd(pdev, "ATE0\r\n", NULL, NULL, 0, AT_TIMEOUT);
    if(err) 
    {
        return err;
    }
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, passwd);
    err = at_send_cmd(pdev, cmd, NULL, NULL, 0, AT_TIMEOUT);
    if(err) 
    {
        return err;
    }
    local_ip = esp8266_get_local_ip(pdev);
    if(!local_ip)
    {
        return -1;
    }
    err = at_send_cmd(pdev, "AT+CIPMUX=1\r\n", NULL, NULL, 0, AT_TIMEOUT);
    if(err) 
    {
        return err;
    }
    err = at_send_cmd(pdev, "AT+CIPDINFO=1\r\n", NULL, NULL, 0, AT_TIMEOUT);
    if(err) 
    {
        return err;
    }

    err = esp8266_get_wifi_status(cmd, sizeof(cmd));
    if(!err) 
    {
        return err;
    }
    err = strcmp(cmd, ssid);
    if(err)
    {
        return -1;
    }
	return 0;
}

static int parse_dec_u32(const char **pp, uint32_t *out)
{
    const char *p = *pp;
    uint32_t val = 0;
    uint8_t has_digit = 0;

    while (*p >= '0' && *p <= '9')
    {
        has_digit = 1;
        val = val * 10 + (*p - '0');
        p++;
    }

    if (!has_digit)
        return -1;

    *out = val;
    *pp = p;

    return 0;
}

static void esp8266_uart_discard_bytes(struct UART_Device *puart, uint16_t len)
{
    uint8_t dummy;
    while (len--)
        while (puart->UART_Recv(puart, &dummy, portMAX_DELAY) != 0);
}

/* ------------------------------------------------------------------ */
/*  解析 +IPD 头部（不变，仅格式整理）                                   */
/* ------------------------------------------------------------------ */
static int esp8266_parse_ipd_header(const char *ipd, ESP8266_IPD_Info *info)
{
    const char *p;
    uint32_t    val;
    char        ip_str[46];
    const char *ip_start;
    ssize_t     ip_len;

    if (ipd == NULL || info == NULL)
        return -1;

    memset(info, 0, sizeof(ESP8266_IPD_Info));

    if (strncmp(ipd, "+IPD,", 5) != 0)
        return -2;

    p = ipd + 5;

    /* link_id */
    if (parse_dec_u32(&p, &val) != 0|| val > 4)
        return -3;
    info->link_id = (uint8_t)val;

    if (*p++ != ',')
        return -5;

    /* data_len */
    if (parse_dec_u32(&p, &val) != 0 || val > 65535)
        return -6;
    info->data_len = (uint16_t)val;

    /* 模式 1: +IPD,<link_id>,<len>:<data> */
    if (*p == ':')
    {
        info->has_remote = 0;
        return 0;
    }

    /* 模式 2: +IPD,<link_id>,<len>,<remote_ip>,<remote_port>:<data> */
    if (*p != ',')
        return -8;
    p++;

    info->has_remote = 1;

    /* remote_ip，兼容带引号和不带引号 */
    if (*p == '"')
    {
        p++;
        ip_start = p;
        while (*p && *p != '"') p++;
        if (*p != '"') return -9;
        ip_len = (ssize_t)(p - ip_start);
        p++;if (*p != ',') return -10;
        p++;}
    else
    {
        ip_start = p;
        while (*p && *p != ',') p++;
        if (*p != ',') return -11;
        ip_len = (ssize_t)(p - ip_start);
        p++;
    }

    if (ip_len <= 0 || ip_len >= (ssize_t)sizeof(ip_str))
        return -12;

    memcpy(ip_str, ip_start, (size_t)ip_len);
    ip_str[ip_len] = '\0';

    if (inet_pton(AF_INET, ip_str, &info->remote_ip) != 1)
        return -13;

    /* remote_port */
    if (parse_dec_u32(&p, &val) != 0 || val > 65535)
        return -14;
    info->remote_port = (uint16_t)val;

    if (*p != ':')
        return -15;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  处理 +IPD 数据包：查找/创建 socket，将数据入队                */
/* ------------------------------------------------------------------ */
static void handle_ipd_packet(struct UART_Device *puart,
                              struct AT_Device   *pdev,
                              const char         *ipd_start)
{
    ESP8266_IPD_Info    ipd_info;
    struct socket_t    *psocket;
    struct sockaddr_in *sin;
    int                sockfd;
    uint8_t             data;

    if (esp8266_parse_ipd_header(ipd_start, &ipd_info) != 0)
        return;
		printf("ipd_len %d\r\n", ipd_info.data_len);
    psocket = get_esp8266_socket_for_hw_socket(ipd_info.link_id);
    if (psocket == NULL){ /*先默认按TCP*/
        sockfd = esp8266_socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0)
        {
            esp8266_uart_discard_bytes(puart, ipd_info.data_len);
            return;
        }
        psocket = fd_socket_get(sockfd);

        vTaskSuspendAll();
        {
            sin                = (struct sockaddr_in *)&psocket->remote_addr;
            sin->sin_family      = AF_INET;
            sin->sin_addr.s_addr = ipd_info.remote_ip;
            sin->sin_port        = htons(ipd_info.remote_port);

            psocket->hw_socket   = ipd_info.link_id;
            psocket->open_flag   = 1;
					  psocket->status			 = SOCKET_USED;
            psocket->mode        = SOCKET_CLIENT;
        }
        xTaskResumeAll();
    }

    for (uint16_t i = 0; i < ipd_info.data_len; i++)
    {
        if (puart->UART_Recv(puart, &data, portMAX_DELAY) == 0)
            xQueueSend(psocket->recv_queue, &data, 0);
    }
    xSemaphoreGive(psocket->recv_lock);
}

/* ------------------------------------------------------------------ */
/*  处理 x,CLOSED 被动关闭事件                                          */
/* ------------------------------------------------------------------ */
static void handle_closed_event(struct AT_Device *pdev,const char *line)
{
    if(1 == pdev->in_cmd)
        return;
    const char      *comma   = strchr(line, ',');
    struct socket_t *psocket;
    uint8_t          link_id;
    /* 格式必须是 "x,CLOSED"，x 为0-4 */
    if (comma == NULL || comma != line + 1 ||
        line[0] < '0' || line[0] > '4')
        return;

    link_id = (uint8_t)(line[0] - '0');
    psocket = get_esp8266_socket_for_hw_socket(link_id);
    if (psocket == NULL)
        return;
    if(0 == psocket->open_flag)
        return ;
    vTaskSuspendAll();
	    printf("handle_closed_event\r\n");
	    fd_socket_free(psocket->sockfd);
    psocket->open_flag = 0;
    psocket->status      = SOCKET_FREE;
    psocket->sockfd = -1;
    xTaskResumeAll();
}

/* ------------------------------------------------------------------ */
/*  处理普通 AT 响应行                */
/* ------------------------------------------------------------------ */
static void handle_at_response(struct AT_Device *pdev,
                               const char       *line,
                               uint32_t          content_len)
{
    /* 优先匹配更长的字符串，避免 "SEND OK" 被"OK" 先命中 */
    if (strstr(line, "SEND OK"))
    {
        pdev->resp_status = ESP8266_OK;
        pdev->in_cmd = 0;
        xSemaphoreGive(pdev->at_resp_sem);
    }
    else if (strstr(line, "SEND FAIL"))
    {
        pdev->resp_status = ESP8266_ERROR;
        pdev->in_cmd = 0;
        xSemaphoreGive(pdev->at_resp_sem);
    }
    else if (strstr(line, "OK"))
    {
        pdev->resp_status = ESP8266_OK;
        pdev->in_cmd = 0;
        xSemaphoreGive(pdev->at_resp_sem);
    }
    else if (strstr(line, "ERROR"))
    {
        pdev->resp_status = ESP8266_ERROR;
        pdev->in_cmd = 0;
        xSemaphoreGive(pdev->at_resp_sem);
    }
    else if (pdev->resp_line_counts < ESP8266_RESP_LINE_MAX)
    {
        pdev->resp_line_len[pdev->resp_line_counts] = content_len + 1;
        memcpy(pdev->resp[pdev->resp_line_counts++], line, content_len + 1);
    }
}

/* ------------------------------------------------------------------ */
/*  接收主任务                                                           */
/* ------------------------------------------------------------------ */
void Start_RECV_Task(void *argument)
{
    struct AT_Device   *pdev  = get_netdev();
    struct UART_Device *puart = Get_UART_Device("stm32_f4_uart2");
    char     recv_buf[ESP8266_RX_BUF_SIZE];
    uint32_t line_len = 0;
    uint8_t  ch;
    uint8_t     do_reset;
    char*ipd_start;

    while (1)
    {
        puart->UART_Recv(puart, &ch, portMAX_DELAY);
        /* 缓冲区溢出保护 */
        if (line_len >= ESP8266_RX_BUF_SIZE - 1)
        {
            line_len    = 0;
            recv_buf[0] = '\0';
        }

        recv_buf[line_len]     = (char)ch;
        recv_buf[line_len + 1] = '\0';

        do_reset  = 0;
        ipd_start = (line_len >= 4) ? strstr(recv_buf, "+IPD,") : NULL;

        if (ipd_start && strchr(ipd_start, ':'))
        {
            /*---- 完整 +IPD 头部，处理数据包 ---- */
            handle_ipd_packet(puart, pdev, ipd_start);
            do_reset = 1;
        }
        else if (line_len > 0 &&
                 recv_buf[line_len - 1] == '\r' &&
                 recv_buf[line_len]     == '\n')
        {
            /* ---- 行结束：去掉 \r\n 后分发 ---- */
            uint32_t content_len       = line_len - 1;
            recv_buf[content_len]      = '\0';

            if (strstr(recv_buf, ",CLOSED"))
                handle_closed_event(pdev,recv_buf);
            else
                handle_at_response(pdev, recv_buf, content_len);

            do_reset = 1;
        }

        /* 根据标志决定：重置行or 继续累积 */
        if (do_reset)
        {
            line_len    = 0;
            recv_buf[0] = '\0';
        }
        else
        {
            line_len++;
        }
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

    pdev = get_netdev();
    if (pdev == NULL)
        return -1;
    vTaskSuspendAll();
    psocket = get_esp8266_unuse_fd_socket();
    if (psocket == NULL)
    {
        xTaskResumeAll();
        return -1;
    }
    sockfd = fd_socket_alloc(psocket);
    psocket->open_flag = 1;
    psocket->sockfd = sockfd;
	printf("esp8266_socket psocket->open_flag %d,sockfd %d\r\n",psocket->open_flag,sockfd);
    if (sockfd < 0)
    {
        psocket->open_flag = 0;
        psocket->sockfd = -1;
				printf("esp8266_socket\r\n");
        fd_socket_free(sockfd);
		xTaskResumeAll();
        return -1;
    }
    /* 初始化槽位 */
    psocket->status      = SOCKET_FREE;
    psocket->mode        = SOCKET_UNKNOWN;
    psocket->socket_type = (uint8_t)type;
    psocket->hw_socket   = 0xFFFFFFFFU;   /*尚未分配硬件 link_id */
    memset(&psocket->local_addr,  0, sizeof(psocket->local_addr));
    memset(&psocket->remote_addr, 0, sizeof(psocket->remote_addr));
    xTaskResumeAll();
    return sockfd;
}

struct socket_t * esp8266_get_socket(int sockfd)
{
    struct AT_Device *ptDev = get_netdev();
    for(int i = 0; i < ESP8266_SOCKET_NUM; i++)
    {
        if(ptDev->sockets[i].sockfd == sockfd)
        {
            return &(ptDev->sockets[i]);
        }
    }
    return NULL;
}
/**
 * @brief关闭套接字，释放本地和硬件资源
 * @note   无论 AT 命令是否成功都会释放本地资源，防止 fd 泄漏；
 *         关闭服务端 socket 会同步清理所有属于该服务的客户端 socket
 * @param  sockfd套接字描述符
 * @return 成功返回 0，失败返回 -1
 */
int esp8266_close(int sockfd)
{
    struct AT_Device   *pdev         = get_netdev();
    struct socket_t    *psocket      = fd_socket_get(sockfd);
    char                cmd[48]      = {0};
	uint8_t discard;
    if (psocket == NULL)
        return -1;

    if (psocket->open_flag != 1) return -1;
    xSemaphoreTake(psocket->send_lock, portMAX_DELAY);
    /* ---- 发送 AT 命令关闭硬件连接 ---- */
    if (psocket->hw_socket != 0xFFFFFFFFU)
    {
        memset(cmd, 0, sizeof(cmd));
        /* 客户端：按link_id 关闭单条连接 */
        snprintf(cmd, sizeof(cmd), "AT+CIPCLOSE=%u\r\n", psocket->hw_socket);
        at_send_cmd(pdev, cmd, NULL, NULL, 0, AT_TIMEOUT);
    }
    xSemaphoreGive(psocket->send_lock);
        /* ---- 清空本 socket 接收队列 ---- */
    while (xQueueReceive(psocket->recv_queue, &discard, 0) == pdTRUE);
    /* 释放接收队列信号量*/
    xSemaphoreGive(psocket->recv_lock);

    vTaskSuspendAll();
    /* ---- 重置本 socket 状态字段（信号量和队列句柄不动）---- */
    psocket->open_flag   = 0;
    psocket->hw_socket   = 0xFFFFFFFFU;
    psocket->mode        = SOCKET_UNKNOWN;
    psocket->socket_type = SOCK_UNDEF;
    memset(&psocket->local_addr,  0, sizeof(psocket->local_addr));
    memset(&psocket->remote_addr, 0, sizeof(psocket->remote_addr));
    psocket->status = SOCKET_FREE;
    psocket->sockfd = -1;
	printf("esp8266_close\r\n");
    fd_socket_free(sockfd);
    xTaskResumeAll();
    return 0;
}

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

    pdev = get_netdev();
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

    struct AT_Device *pdev = get_netdev();
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

    /* 只有 TCP socket 才能 listen */
    if (pSocket->socket_type != SOCK_STREAM)
    {
        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /* 必须先 bind 才能 listen，端口为 0 说明还未 bind */
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
    pSocket->status    = SOCKET_USED;  

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
    if (sockfd < 0 || sockfd >= FD_SOCKET_TABLE_SIZE)     return -1;
    if (addr == NULL)                                      return -1;
    if (addrlen < (socklen_t)sizeof(struct sockaddr_in))   return -1;
    if (addr->sa_family != AF_INET)                        return -1;
    struct AT_Device *pdev = get_netdev();
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
    /* 防止重复 connect */
    if (pSocket->mode == SOCKET_CLIENT && pSocket->status == SOCKET_USED)
    {
        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /*---- 解析目标地址 ----*/
    const struct sockaddr_in *dest = (const struct sockaddr_in *)addr;
    uint16_t dest_port = ntohs(dest->sin_port);

    /*不用 inet_ntoa（非线程安全），手动格式化 IP */
    uint32_t ip_val = dest->sin_addr.s_addr;
		printf("remote ip %d \r\n",ip_val);
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

    /* 先占位，防止 AT 命令发送期间被其他任务抢同一 link_id */
    pSocket->hw_socket = (uint32_t)link_id;
    pSocket->status    = SOCKET_USED;

    xSemaphoreGive(pdev->dev_lock);

    /*---- 组装并发送 AT+CIPSTART ----*/
    char cmd[96] = {0};
    int  ret     = -1;
	printf("pSocket->socket_type %d\r\n",pSocket->socket_type);
    if (pSocket->socket_type == SOCK_STREAM)
    {
        /* TCP 连接 */
        snprintf(cmd, sizeof(cmd),
                 "AT+CIPSTART=%d,\"TCP\",\"%s\",%u\r\n",
                 link_id, remote_ip, dest_port);
				printf("%s",cmd);
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

    /*AT 命令失败时回滚已占位的 hw_socket ----*/
    if (ret != 0)
    {
			printf("AT+CIPSTART err\r\n");
//        xSemaphoreTake(pdev->dev_lock, portMAX_DELAY);
        pSocket->hw_socket = 0xFFFFFFFFU;
        pSocket->status    = SOCKET_FREE;
//        xSemaphoreGive(pdev->dev_lock);

        xSemaphoreGive(pSocket->send_lock);
        return -1;
    }

    /*---- 修复：AT 命令成功后，补全 socket 状态 ----*/
    pSocket->mode        = SOCKET_CLIENT;    
    pSocket->remote_addr = *dest;
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
int esp8266_get_cipstatus(struct AT_Device *ptDev, cipstatus_t *conns,
                          int *stat)
{
    int conn_count = 0;

    if (!ptDev || !conns || !stat)
        return -1;

    *stat = -1;

    /* 1. 发送命令 */
    if (at_send_cmd(ptDev, "AT+CIPSTATUS\r\n", NULL, NULL, 0, AT_TIMEOUT) != 0)
        goto cleanup;

    /* 2. 解析第 0 行：STATUS:<state> */
    if (ptDev->resp_line_counts == 0 ||
        sscanf((const char *)ptDev->resp[0], "STATUS:%d", stat) != 1)
        goto cleanup;

    /* 3. 逐行解析 +CIPSTATUS 条目 */
    for (int i = 1;
         i < (int)ptDev->resp_line_counts && i < RESP_ROW_LEN && conn_count < ESP8266_SOCKET_NUM;
         i++)
    {
        const char *line = (const char *)ptDev->resp[i];
						//printf("%s\r\n",line);
        if (strncmp(line, "+CIPSTATUS:", 11) != 0)
            continue;

        char type_str[10], ip_str[32];
        unsigned short remote_port, local_port;
        int hw_socket, tetype;

        if (sscanf(line,
                   "+CIPSTATUS:%d,\"%9[^\"]\",\"%31[^\"]\",%hu,%hu,%d",
                   &hw_socket, type_str, ip_str,
                   &remote_port, &local_port, &tetype) != 6)
            continue;

        uint32_t ip_bin = 0;
        if (inet_pton(AF_INET, ip_str, &ip_bin) != 1)
            continue;

        cipstatus_t *p = &conns[conn_count++];
        p->link_id = hw_socket;
        p->remote_ip = ip_bin;
        p->remote_port = remote_port;
        p->local_port = local_port;
        p->tetype = tetype;
        p->type = (strcmp(type_str, "UDP") == 0) ? SOCK_DGRAM : SOCK_STREAM;
    }

cleanup:
    ptDev->resp_line_counts = 0;
    ptDev->resp_status = 0;
    memset(ptDev->resp_line_len, 0, sizeof(ptDev->resp_line_len));

    /* stat 未成功解析时整体失败 */
    return (*stat >= 0) ? conn_count : -1;
}

/**
* @brief  判断一个 socket 是否与 CIPSTATUS 条目匹配
* @param  sock  已跟踪的 socket
* @param  cip   CIPSTATUS 解析出的条目
* @return 1=匹配，0=不匹配
*/
static int socket_match_cipstatus(const struct socket_t *sock,
                                  const cipstatus_t     *cip)
{
    if (!sock || !cip)
        return 0;

    return (sock->hw_socket                == (uint32_t)cip->link_id)&& (sock->remote_addr.sin_addr.s_addr  == cip->remote_ip)
        && (sock->remote_addr.sin_port         == htons(cip->remote_port));
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
int esp8266_accept(int sockfd,
                   struct sockaddr *addr, socklen_t *addrlen)
{
    struct AT_Device *ptDev = get_netdev();
    cipstatus_t cur[5];
    int cur_count;
    int stat;
    int  j;

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
     * 找第一个新连接并返回
     * ============================================================ */
    for (j = 0; j < cur_count; j++)
    {
        /* 只处理 client 的连接 */
        if (cur[j].tetype == SOCKET_SERVER)
            continue;

        /* 本地端口必须匹配当前 server port */
        if (cur[j].local_port != ntohs(server->local_addr.sin_port))
            continue;

        /* 检查是否已被跟踪 */
        struct socket_t *existing = get_esp8266_socket_for_hw_socket(cur[j].link_id);
				if (existing != NULL)
        {
            if (socket_match_cipstatus(existing, &cur[j]))
            {
                /*
                 * handle_ipd_packet 可能已抢先创建了这个 socket，
                 * 但 local_addr没赋值（local_port=0）。
                 * ✅ Bug2修复：复用该 socket，补全 local_addr 后返回给应用层
                 */
                if (existing->local_addr.sin_port == 0)
                {
                    vTaskSuspendAll();
                    existing->local_addr = server->local_addr;
                    existing->mode= SOCKET_CLIENT;
                    xTaskResumeAll();

                    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in))
                    {
                        struct sockaddr_in *sin = (struct sockaddr_in *)addr;
                        *sin= existing->remote_addr;
                        *addrlen = sizeof(struct sockaddr_in);
                    }
                    return existing->sockfd;
                }

                /* 已被accept 过，跳过 */
                continue;
            }

            /* link_id 被占用但地址不匹配（旧连接残留），跳过 */
            if (existing->open_flag == 1 && existing->status == SOCKET_USED)
                continue;
        }
        /* 新连接：在临界区内分配 socket */
        vTaskSuspendAll();
        /* 分配空闲 socket */
        struct socket_t *client = get_esp8266_unuse_fd_socket();
        if (!client)
        {
            xTaskResumeAll();
            return -1;
        }
        int new_fd = fd_socket_alloc(client);
        if (new_fd < 0)
        {
            xTaskResumeAll();
            return -1;
        }
        client->open_flag = 1;
        client->sockfd = new_fd;
        /* 初始化 client socket */
        client->status = SOCKET_USED;
        client->mode = SOCKET_CLIENT;
        client->socket_type = cur[j].type;
        client->hw_socket = (uint32_t)cur[j].link_id;
        client->remote_addr.sin_family = AF_INET;
        client->remote_addr.sin_addr.s_addr = cur[j].remote_ip;
        client->remote_addr.sin_port = htons(cur[j].remote_port);
        client->local_addr = server->local_addr;
        xTaskResumeAll();
        /* 填充调用者的地址结构 */
        if (addr && addrlen)
        {
            if (*addrlen >= sizeof(struct sockaddr_in))
            {
                struct sockaddr_in *sin = (struct sockaddr_in *)addr;
                memset(sin, 0, sizeof(*sin));
                sin->sin_family = AF_INET;
                sin->sin_addr.s_addr = cur[j].remote_ip;
                sin->sin_port = htons(cur[j].remote_port);
                *addrlen = sizeof(struct sockaddr_in);
            }
        }
        /* 找到一个就返回 */
        return new_fd;
    }
    /* 没有新连接 */
    return -1;
}

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

ssize_t esp8266_send(int sockfd, const void *buf, ssize_t len, int flags)
{
    (void)flags;

    if (sockfd < 0 || sockfd >= FD_SOCKET_TABLE_SIZE) return -1;
    if (buf == NULL || len == 0) return (buf == NULL) ? -1 : 0;

    struct AT_Device *pdev = get_netdev();
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
 // printf("[SEND] len=%d\r\n", (int)len); 
    const uint8_t *ptr      = (const uint8_t *)buf;
    ssize_t         remaining = len;
    ssize_t         sent      = 0;
    char           cmd[48]   = {0};

    while (remaining > 0)
    {
        ssize_t chunk = (remaining > ESP8266_SEND_MAX_LEN)? ESP8266_SEND_MAX_LEN : remaining;

        /* 第一步：发 AT+CIPSEND 指令，不管返回 */
        snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u,%u\r\n",
                 (unsigned)psocket->hw_socket,
                 (unsigned)chunk);
        at_send_cmd(pdev, cmd, NULL, NULL, 0, AT_TIMEOUT);

        /* 第二步：按字节长度发原始数据，等 "SEND OK" */
        at_send_data(pdev, ptr + sent, chunk, AT_TIMEOUT);

        sent      += chunk;
        remaining -= chunk;
    }

    xSemaphoreGive(psocket->send_lock);
    return sent;
}


/**
 * @brief 接收数据（TCP） 
 * @param sockfd 套接字描述符
 * @param buf    接收缓冲区指针
 * @param len    缓冲区长度（字节）
 * @param flags  标志位，通常为 0
 * @return 成功返回实际接收字节数，连接关闭返回 0，失败返回 -1
 */
ssize_t esp8266_recv(int sockfd, void *buf, ssize_t len, int flags)
{
//     uint8_t *cmd = (uint8_t *)buf;
//     ssize_t recv_len = 0;
//     struct socket_t *pSocket = fd_socket_get(sockfd);
//     struct UART_Device* puart1 =Get_UART_Device("stm32_f4_uart1");
//     if (pSocket == NULL)
//     {
//         return -1;
//     }
//     xSemaphoreTake(pSocket->recv_lock, portMAX_DELAY);
//     uint8_t byte;
//     while (recv_len < (ssize_t)len &&
//            pdTRUE == xQueueReceive(pSocket->recv_queue, &byte, 0))
//     {
//         cmd[recv_len++] = byte;
//     }
//    puart1->UART_Send(puart1, cmd, recv_len, 1000);
//     return recv_len;
    return esp8266_recvfrom(sockfd, buf, len, flags, NULL, NULL);
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
ssize_t esp8266_sendto(int sockfd, const void *buf, ssize_t len, int flags,
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

    struct AT_Device *pdev = get_netdev();
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
    ssize_t         remaining = len;
    ssize_t         sent      = 0;
    char           cmd[64]   = {0};

    while (remaining > 0)
    {
        ssize_t chunk = (remaining > ESP8266_SEND_MAX_LEN)? ESP8266_SEND_MAX_LEN : remaining;

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
ssize_t esp8266_recvfrom(int sockfd, void *buf,
                         ssize_t len,
                         int flags,
                         struct sockaddr *src_addr,
                         socklen_t *addrlen)
{
    struct socket_t *psocket;
    uint8_t *pbuf = (uint8_t *)buf;
    ssize_t i;
    int timeout;

    if (buf == NULL || len == 0)
        return -1;

    psocket = fd_socket_get(sockfd);

    if (psocket == NULL || psocket->recv_queue == NULL)
        return -1;

    /* 先非阻塞尝试读取，队列里有残留数据直接拿走 */
    for (i = 0; i < len; i++)
    {
        if (pdPASS != xQueueReceive(psocket->recv_queue, &pbuf[i], 0))
            break;
    }

    /* 没有数据才等待信号量 */
    if (i == 0)
    {
        timeout = psocket->recv_timeout;

        while (pdPASS != xSemaphoreTake(psocket->recv_lock, pdMS_TO_TICKS(10)))
        {
            if (timeout > 10)
                timeout -= 10;
            else
                break;
        }
    }
            /* 等到信号量或超时后再读一次 */
    for (; i < len; i++)
    {
        if (pdPASS != xQueueReceive(psocket->recv_queue, &pbuf[i], 0))
            break;
    }
    /* 读到数据才填充来源地址 */
    if (i > 0)
    {
        if (src_addr != NULL && addrlen != NULL)
        {
            struct sockaddr_in tmp;
            tmp = psocket->remote_addr;
            memcpy(src_addr, &tmp, sizeof(struct sockaddr_in));
            *addrlen = sizeof(struct sockaddr_in);
        }
    }
    
    return i;
}

//ssize_t esp8266_recvfrom(int sockfd, void *buf, ssize_t len, int flags,
//                         struct sockaddr *src_addr, socklen_t *addrlen)
//{
//    if(sockfd <0 || sockfd >= FD_SOCKET_TABLE_SIZE) return -1;

//    // TODO: 实现UDP接收功能，获取来源地址信息
////    if (psocket == NULL) return -1;
////    if (psocket->socket_type != SOCK_DGRAM) return -1;
////    if (psocket->open_flag != 1) return -1;
////    if (psocket->hw_socket == 0xFFFFFFFFU) return -1;
////    struct socket_t *psocket = fd_socket_get(sockfd);
////    uint8_t *pbuf = (uint8_t *)buf;
////    ssize_t len = 0;
////    while(pdTRUE == xQueueReceive(psocket->recv_queue, pbuf[len++], 0));
////    src_addr = psocket->remote_addr;
////    return len;
//	return -1;
//}
// ssize_t esp8266_recvfrom(int sockfd,
//                      void *buf,
//                      ssize_t len,
//                      int flags,
//                      struct sockaddr *src_addr,
//                      socklen_t *addrlen)
// {
//     struct socket_t *psocket;
//     ESP8266_RecvHeader hdr;

//     ssize_t copy_len;
//     ssize_t remain_len;
//     ssize_t i;
//     uint8_t dummy;

//     uint8_t *pbuf = (uint8_t *)buf;

//     if (buf == NULL || len == 0)
//         return -1;

//     if (src_addr != NULL)
//     {
//         if (addrlen == NULL)
//             return -1;

//         if (*addrlen < sizeof(struct sockaddr_in))
//             return -1;
//     }

//     psocket = fd_socket_get(sockfd);

//     if (psocket == NULL)
//         return -1;

//     if (psocket->recv_queue == NULL)
//         return -1;

//     if (xSemaphoreTake(psocket->recv_lock, portMAX_DELAY) != pdTRUE)
//         return -1;
//     /*
//      * 先读固定包头。
//      */
//     if (esp8266_queue_recv_header(psocket->recv_queue,
//                                   &hdr,
//                                   portMAX_DELAY) != 0)
//     {
//         return -1;
//     }

//     copy_len = len < hdr.data_len ? len : hdr.data_len;

//     /*
//      * 读取用户需要的数据。
//      */
//     for (i = 0; i < copy_len; i++)
//     {
//         if (xQueueReceive(psocket->recv_queue, &pbuf[i], portMAX_DELAY) != pdTRUE)
//             return -1;
//     }

//     /*
//      * 如果用户 buf 比一个 IPD 包小，剩余数据丢弃。
//      * 这是 UDP recvfrom 常见处理方式。
//      */
//     remain_len = hdr.data_len - copy_len;

//     while (remain_len--)
//     {
//         if (xQueueReceive(psocket->recv_queue, &dummy, portMAX_DELAY) != pdTRUE)
//             return -1;
//     }

//     /*
//      * 填充发送方地址。
//      * 只有 AT+CIPDINFO=1 模式下才有 remote_ip / remote_port。
//      */
//     if (src_addr != NULL)
//     {
//         if (hdr.flags & ESP8266_RECV_FLAG_HAS_REMOTE)
//         {
//             struct sockaddr_in *sin = (struct sockaddr_in *)src_addr;

//             memset(sin, 0, sizeof(struct sockaddr_in));

//             sin->sin_family = AF_INET;

//             /*
//              * hdr.remote_ip 已经是网络字节序，不能 htonl。
//              */
//             sin->sin_addr.s_addr = hdr.remote_ip;

//             /*
//              * hdr.remote_port 是主机字节序，填 sockaddr 要 htons。
//              */
//             sin->sin_port = htons(hdr.remote_port);

//             *addrlen = sizeof(struct sockaddr_in);
//         }
//         else
//         {
//             /*
//              * AT+CIPDINFO=0 模式，没有远端 IP/端口。
//              */
//             *addrlen = 0;
//         }
//     }
//     return copy_len;
// }


