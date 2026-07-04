#ifndef __ESP8266_H__
#define __ESP8266_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "at_device.h"
/* ==================== ESP8266-01S Socket 管理 ==================== */
#define ESP8266_SOCKET_SERVER_ID 0xa55aa55a
#define ESP8266_SOCKET_RX_QUEUE_SIZE 512
#define ESP8266_SOCKET_RX_SEMAPHORE_COUNT 10
#define ESP8266_SOCKET_ACCEPT_QUEUE_SIZE 20

#define ESP8266_RESP_LINE_MAX   RESP_ROW_LEN   /* 最多缓存行数 */

struct AT_Device * get_netdev(void);
int esp8266_init(char *uart_dev);
int esp8266_connect_ap(char *ssid, char *passwd);
int esp8266_setsockopt(int socket, int level, int optname, const void *optval, socklen_t optlen);
uint32_t esp8266_gethostbyname(char *addr);
int esp8266_flush(int socket);
/**
* @brief 创建套接字
* @param domain   地址族，AF_INET（IPv4）、AF_INET6（IPv6）
* @param type     套接字类型，SOCK_STREAM（TCP） 1、SOCK_DGRAM（UDP） 2
* @param protocol 协议，通常为 0
* @return 成功返回套接字描述符（>=0），失败返回 -1
*/
int esp8266_socket(int domain, int type, int protocol);

/**
* @brief 关闭套接字
* @param sockfd 套接字描述符
* @return 成功返回 0，失败返回 -1
*/
int esp8266_close(int sockfd);

/**
* @brief 绑定本地地址和端口
* @param sockfd  套接字描述符
* @param addr    本地地址结构体指针
* @param addrlen 地址结构体长度
* @return 成功返回 0，失败返回 -1
*/
int esp8266_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

/**
* @brief 开始监听连接请求
* @param sockfd  套接字描述符
* @param backlog 等待连接队列最大长度
* @return 成功返回 0，失败返回 -1
*/
int esp8266_listen(int sockfd, int backlog);

/**
* @brief 发起连接
* @param sockfd  套接字描述符
* @param addr    目标地址结构体指针
* @param addrlen 地址结构体长度
* @return 成功返回 0，失败返回 -1
*/
int esp8266_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

/**
* @brief 接受一个连接请求
* @param sockfd  监听套接字描述符
* @param addr    [out] 客户端地址，可为 NULL
* @param addrlen [out] 地址长度，可为 NULL
* @return 成功返回新套接字描述符，失败返回 -1
*/
int esp8266_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

/**
* @brief 发送数据（TCP）
* @param sockfd 套接字描述符
* @param buf    发送缓冲区指针
* @param len    发送数据长度（字节）
* @param flags  标志位，通常为 0
* @return 成功返回实际发送字节数，失败返回 -1
*/
ssize_t esp8266_send(int sockfd, const void *buf, ssize_t len, int flags);
/**
* @brief 接收数据（TCP）
* @param sockfd 套接字描述符
* @param buf    接收缓冲区指针
* @param len    缓冲区长度（字节）
* @param flags  标志位，通常为 0
* @return 成功返回实际接收字节数，连接关闭返回 0，失败返回 -1
*/
ssize_t esp8266_recv(int sockfd, void *buf, ssize_t len, int flags);

/**
* @brief 发送数据到指定地址（UDP）
* @param sockfd    套接字描述符
* @param buf       发送缓冲区指针
* @param len       发送数据长度（字节）
* @param flags     标志位，通常为 0
* @param dest_addr 目标地址结构体指针
* @param addrlen   目标地址结构体长度
* @return 成功返回实际发送字节数，失败返回 -1
*/
ssize_t esp8266_sendto(int sockfd, const void *buf, ssize_t len, int flags,
                      const struct sockaddr *dest_addr, socklen_t addrlen);

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
ssize_t esp8266_recvfrom(int sockfd, void *buf, ssize_t len, int flags,
                        struct sockaddr *src_addr, socklen_t *addrlen);

/* TODO: 暂未实现
 * @brief 设置套接字选项
 * @param sockfd套接字描述符
 * @param level   协议层，如 SOL_SOCKET、IPPROTO_TCP
 * @param optname 选项名，如 SO_REUSEADDR、SO_KEEPALIVE
 * @param optval  选项值指针
 * @param optlen  选项值长度
 * @return 成功返回 0，失败返回 -1
 *
 * int esp8266_setsockopt(int sockfd, int level, int optname,
 *                        const void *optval, socklen_t optlen);
 */

/* TODO: 暂未实现
 * @brief 监控多个描述符的可读/可写/异常事件
 * @param nfds      最大描述符值加1
 * @param readfds   [in/out] 监控可读集合，可为 NULL
 * @param writefds  [in/out] 监控可写集合，可为 NULL
 * @param exceptfds [in/out] 监控异常集合，可为 NULL
 * @param timeout   超时时间，NULL 永久阻塞，0 立即返回
 * @return 就绪描述符数量，超时返回 0，失败返回 -1
 *
 * int esp8266_select(int nfds, fd_set *readfds, fd_set *writefds,
 *                    fd_set *exceptfds, struct timeval *timeout);
 */




#ifdef __cplusplus
}
#endif

#endif /* __ESP8266_H__ */
