#include "device_socket.h"
#include <stdio.h>    
#include <string.h>

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
