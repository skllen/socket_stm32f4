#include "device_socket.h"
#include <stdio.h>    
#include <string.h>
#include "esp8266.h"

static volatile struct socket_t *g_fd_table[FD_SOCKET_TABLE_SIZE] = {NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL};

// 初始化时调用一次

int fd_socket_find(struct socket_t *pdev)
{
    for (int i = 0; i < FD_SOCKET_TABLE_SIZE; i++) {
        if (g_fd_table[i] == pdev) {
            return i;
        }
    }
    return -1;
}

int fd_socket_alloc(struct socket_t *pdev)
{
    int ret = -1;
    
    if (pdev == NULL)
    {
        return -1;
    }
    for (int i = 0; i < FD_SOCKET_TABLE_SIZE; i++)
    {
        if (g_fd_table[i] == NULL)
        {
            g_fd_table[i] = pdev;
            g_fd_table[i]->open_flag = 1;
            g_fd_table[i]->sockfd = i;
            ret = i;
            break;
        }
    }

    return ret;
}

int fd_socket_free(int fd)
{
    if(fd < 0 || fd >= FD_SOCKET_TABLE_SIZE)
    {
        return -1;
    }
	printf("fd_socket_free fd %d\r\n",fd);
    g_fd_table[fd]->open_flag = 0;
    g_fd_table[fd]->sockfd = -1;
    g_fd_table[fd] = NULL;
    return 0;
}

struct socket_t *fd_socket_get(int fd)
{
    if(fd < 0 || fd >= FD_SOCKET_TABLE_SIZE)
    {
        return NULL;
    }
    return g_fd_table[fd];
}

/* 二进制 → "192.168.1.1" */
const char *inet_ntop(int af, const void *src, char *dst, unsigned int size) {
    if (af != AF_INET) return NULL;

    const uint8_t *b = (const uint8_t *)src;
    int n = snprintf(dst, size, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
    return (n > 0 && (unsigned)n < size) ? dst : NULL;
}

/* "192.168.1.1" → 二进制 */
int inet_pton(int af, const char *src, void *dst) {
    if (af != AF_INET) return -1;

    uint8_t *b = (uint8_t *)dst;
    int vals[4], n;
    n = sscanf(src, "%d.%d.%d.%d", &vals[0], &vals[1], &vals[2], &vals[3]);
    if (n != 4) return 0;

    for (int i = 0; i < 4; i++) {
        if (vals[i] < 0 || vals[i] > 255) return 0;b[i] = (uint8_t)vals[i];
    }
    return 1;
}
uint16_t ntohs(uint16_t netshort)
{
    return (uint16_t)((netshort << 8) | (netshort >> 8));
}

uint16_t htons(uint16_t hostshort)
{
    return (uint16_t)((hostshort << 8) | (hostshort >> 8));
}

/**********************************************************************
 * 函数名称： at_init
 * 功能描述： 初始化W800相关结构体
 * 输入参数： uart_dev - UART名称
 * 输出参数： 无
 * 返 回 值： 0-成功, (-1)-失败
 * 修改日期：	版本号	  修改人 	  修改内容
 * -----------------------------------------------
 * 2024/09/01		 V1.0	  韦东山 	  创建
 ***********************************************************************/
int at_init(char *uart_dev)
{
	return esp8266_init(uart_dev);
}

/**********************************************************************
 * 函数名称： at_connect_ap
 * 功能描述： 连接WIFI AP
 * 输入参数： ssid   - AP名称
 *            passwd - 密码
 * 输出参数： 无
 * 返 回 值： 0-成功, (-1)-失败
 * 修改日期：	版本号	  修改人 	  修改内容
 * -----------------------------------------------
 * 2024/09/04		 V1.0	  韦东山 	  创建
 ***********************************************************************/
int at_connect_ap(char *ssid, char *passwd)
{
	return esp8266_connect_ap(ssid, passwd);
}

int at_socket(int domain, int type, int protocol)
{
	return esp8266_socket(domain, type, protocol);
}

int at_closesocket(int socket)
{
	return esp8266_close(socket);
}

int at_shutdown(int socket, int how)
{
	return at_closesocket(socket);
}

int at_bind(int socket, const struct sockaddr *name, socklen_t namelen)
{
	return esp8266_bind(socket, name, namelen);
}

int at_connect(int socket, const struct sockaddr *name, socklen_t namelen)
{
	return esp8266_connect(socket, name, namelen);
}

int at_sendto(int socket, const void *data, size_t size, int flags, const struct sockaddr *to, socklen_t tolen)
{
	return esp8266_sendto(socket, data, size, flags, to, tolen);
}

int at_send(int socket, const void *data, size_t size, int flags)
{
	return esp8266_send(socket, data, size, flags);
}

int at_recvfrom(int socket, void *mem, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
	return esp8266_recvfrom(socket, mem, len, flags, from, fromlen);
}

int at_recv(int socket, void *mem, size_t len, int flags)
{
    return at_recvfrom(socket, mem, len, flags, NULL, NULL);
}

int at_flush(int socket)
{
    return esp8266_flush(socket);
}

int at_listen(int socket, int backlog)
{
	return esp8266_listen(socket, backlog);
}

int at_accept(int socket, struct sockaddr *name, socklen_t *namelen)
{
	return esp8266_accept(socket, name, namelen);
}

/**********************************************************************
 * 函数名称： at_gethostbyname
 * 功能描述： 解析域名得到IP,把字符串形式的IP转换为整数形式的IP
 * 输入参数： addr - URL域名
 * 输出参数： 无
 * 返 回 值： 32bit ip地址,
 *            以IP 192.168.1.49为例, 返回的32bit IP里最高字节是192
 * 修改日期：	版本号	  修改人 	  修改内容
 * -----------------------------------------------
 * 2024/11/15		 V1.0	  韦东山 	  创建
 ***********************************************************************/
uint32_t at_gethostbyname(char *addr)
{
	uint32_t ipaddr;
	int i = 0;
	int isIpStr = 1;

	/*
	 * addr: "iot.100ask.net" ==> 32BIT IP
	 * "192.168.1.49"         ==> 32BIT IP, buf[0] = 49, buf[1]=1, buf[2]=168, buf[3]=192
	 */
	
	/* 如果是10进制的点分字符串 */
	for (i = 0; addr[i]; i++)
	{
		if (addr[i] == '.')
			continue;
		if (addr[i] >= '0' && addr[i] <= '9')
			continue;
		isIpStr = 0;
		break;
	}

	if (isIpStr)
	{
		inet_pton(AF_INET, addr, &ipaddr);
		return ipaddr;
	}
	
	return esp8266_gethostbyname(addr);
}

/**********************************************************************
 * 函数名称： setsockopt
 * 功能描述： 设置socket参数, 比如设置recv、send函数的超时时间
 * 输入参数： socket/level/optname/optval/optlen - 网络参数
 * 输出参数： 无
 * 返 回 值： (0)-成功
 * 修改日期：	版本号	  修改人 	  修改内容
 * -----------------------------------------------
 * 2024/11/15		 V1.0	  韦东山 	  创建
 ***********************************************************************/
int setsockopt(int socket, int level, int optname, const void *optval, socklen_t optlen)
{
	return esp8266_setsockopt(socket, level, optname, optval, optlen);
}

