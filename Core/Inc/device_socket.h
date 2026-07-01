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

#define SOCK_STREAM 1 // TCP
#define SOCK_DGRAM 2  // UDP
#define SOCK_UNDEF  0 //未定义  

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
    uint32_t           sockfd;      /*全局fd描述符*/      
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

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_SOCKET_H__  */
