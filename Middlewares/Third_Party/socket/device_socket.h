#ifndef __DEVICE_SOCKET_H__
#define __DEVICE_SOCKET_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include "main.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "queue.h"

#define FD_SOCKET_TABLE_SIZE 10

#define AF_INET 2
#define AF_INET6 10
#define PF_INET		AF_INET
#define IPPROTO_TCP 6	  /* Transmission Control Protocol	  */

#define SOCK_STREAM 1 // TCP
#define SOCK_DGRAM 2  // UDP
#define SOCK_UNDEF  0 //未定义  


// #define SO_RECVTIMEO 0x1006
// #define SO_SENDTIMEO 0x1005

#define SO_RCVTIMEO 0
#define SO_SNDTIMEO 1

typedef uint32_t socklen_t;
typedef int ssize_t;

typedef enum {
    SOCKET_FREE,
    SOCKET_USED,
} SocketStatus;

typedef enum {
    SOCKET_UNKNOWN,
    SOCKET_CLIENT,
    SOCKET_SERVER,
} SocketMode;

/*通用地址结构（占位对齐用） */
struct sockaddr {
    uint16_t sa_family;   /* 地址族 AF_INET / AF_INET6 */
    uint8_t  sa_data[14]; /* 地址数据 */
};

struct in_addr {
    uint32_t s_addr;
};

/* IPv4 地址结构 */
struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    uint8_t  sin_zero[8];
};


/* socket 控制块 */
struct socket_t{
    int           sockfd;      /*全局fd描述符*/      
    uint32_t           hw_socket;   /*硬件socket描述符*/
    uint8_t            open_flag;   /*逻辑socket是否打开 0-关闭 1-打开*/
    SocketStatus       status;      /*硬件socket 是否使用 SOCKET_FREE / SOCKET_USED*/
    SocketMode         mode;        /* CLIENT / SERVER */
    uint8_t            socket_type; /* SOCK_STREAM / SOCK_DGRAM */
    struct sockaddr_in local_addr;  /* 本地地址*/
    struct sockaddr_in remote_addr; /* 远程地址 */
    xQueueHandle       recv_queue;  /* 接收队列*/
    xSemaphoreHandle   recv_lock;   /* 接收锁*/
    xSemaphoreHandle   send_lock;   /* 保护每个socket发送时候锁*/
    int recv_timeout;
    int send_timeout;
};

int fd_socket_find(struct socket_t *pdev);

int fd_socket_alloc(struct socket_t *pdev);

int fd_socket_free(int fd);

struct socket_t *fd_socket_get(int fd);
    /* IP地址转换为字符串 */
const char *inet_ntop(int af, const void *src, char *dst, unsigned int size);
    /*  字符串转换为IP地址*/
int inet_pton(int af, const char *src, void *dst);
    /* 网络字节序转主机字节序 */
uint16_t ntohs(uint16_t netshort);
uint16_t htons(uint16_t hostshort); 

int at_socket(int domain, int type, int protocol);
int at_closesocket(int socket);
int at_shutdown(int socket, int how);
int at_bind(int socket, const struct sockaddr *name, socklen_t namelen);
int at_connect(int socket, const struct sockaddr *name, socklen_t namelen);
int at_sendto(int socket, const void *data, size_t size, int flags, const struct sockaddr *to, socklen_t tolen);
int at_send(int socket, const void *data, size_t size, int flags);
int at_recvfrom(int socket, void *mem, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen);
int at_recv(int socket, void *mem, size_t len, int flags);
int at_flush(int socket);
int at_listen(int socket, int backlog);
int at_accept(int socket, struct sockaddr *name, socklen_t *namelen);


#define socket(domain, type, protocol)                      at_socket(domain, type, protocol)
#define closesocket(socket)                                 at_closesocket(socket)
#define shutdown(socket, how)                               at_shutdown(socket, how)
#define bind(socket, name, namelen)                         at_bind(socket, name, namelen)
#define connect(socket, name, namelen)                      at_connect(socket, name, namelen)
#define sendto(socket, data, size, flags, to, tolen)        at_sendto(socket, data, size, flags, to, tolen)
#define send(socket, data, size, flags)                     at_send(socket, data, size, flags)
#define recvfrom(socket, mem, len, flags, from, fromlen)    at_recvfrom(socket, mem, len, flags, from, fromlen)
#define recv(socket, mem, len, flags)                       at_recv(socket, mem, len, flags)
#define net_flush(socket)                                   at_flush(socket)
#define listen(socket, backlog)                             at_listen(socket, backlog)
#define accept(socket, name, namelen)                       at_accept(socket, name, namelen)
#define gethostbyname(addr)                                 at_gethostbyname(addr)

/**********************************************************************
 * 函数名称： at_gethostbyname
 * 功能描述： 解析域名得到IP,把字符串形式的IP转换为整数形式的IP
 * 输入参数： addr - URL域名
 * 输出参数： 无
 * 返 回 值： 32bit ip地址
 *            以IP 192.168.1.49为例, 返回的32bit IP里最高字节是192
 ***********************************************************************/
uint32_t at_gethostbyname(char *addr);

/**********************************************************************
 * 函数名称： setsockopt
 * 功能描述： 设置socket参数, 比如设置recv、send函数的超时时间
 * 输入参数： socket/level/optname/optval/optlen - 网络参数
 * 输出参数： 无
 * 返 回 值： (0)-成功
 ***********************************************************************/
int setsockopt(int socket, int level, int optname, const void *optval, socklen_t optlen);

/**********************************************************************
 * 函数名称： at_init
 * 功能描述： 初始化W800相关结构体
 * 输入参数： uart_dev - UART名称
 * 输出参数： 无
 * 返 回 值： 0-成功, (-1)-失败
 ***********************************************************************/
int at_init(char *uart_dev);

/**********************************************************************
 * 函数名称： at_connect_ap
 * 功能描述： 连接WIFI AP
 * 输入参数： ssid   - AP名称
 *            passwd - 密码
 * 输出参数： 无
 * 返 回 值： 0-成功, (-1)-失败
 ***********************************************************************/
int at_connect_ap(char *ssid, char *passwd);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_SOCKET_H__  */
