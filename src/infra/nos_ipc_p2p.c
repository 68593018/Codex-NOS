#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include "nos_scheduler.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_api.h"
#include "nos_node_priv.h"

#define TX_QUEUE_SIZE 1024
#define MAX_REMOTE_CONNS 16

/**
 * @brief IPC 连接上下文 (支持异步发送)
 */
typedef struct {
    char uds_path[108];
    int fd;
    nos_buffer_t *tx_queue[TX_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
    pthread_mutex_t lock;
    int is_connected;
    nos_thread_t *owner_thread; // 绑定的 IO 线程
} nos_ipc_conn_t;

static nos_ipc_conn_t g_remote_conns[MAX_REMOTE_CONNS];
static uint32_t g_remote_conn_count = 0;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief 尝试从队列中发送数据
 */
static void nos_ipc_process_tx(nos_ipc_conn_t *conn) {
    pthread_mutex_lock(&conn->lock);
    while (conn->head != conn->tail) {
        nos_buffer_t *buf = conn->tx_queue[conn->head];
        nos_service_msg_t *header = (nos_service_msg_t *)buf->data;
        size_t full_len = sizeof(nos_service_msg_t) + header->payload_len;

        /* SEQPACKET 保证原子写入整个报文 */
        ssize_t sent = send(conn->fd, buf->data, full_len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent == (ssize_t)full_len) {
            atomic_fetch_add(&g_node_ctx.stats.tx_packets, 1);
            atomic_fetch_add(&g_node_ctx.stats.tx_bytes, full_len);
            nos_buffer_release(buf);
            conn->head = (conn->head + 1) % TX_QUEUE_SIZE;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* 缓冲区满，停止发送，等待下次 EPOLLOUT */
            pthread_mutex_unlock(&conn->lock);
            return;
        } else {
            /* 链路故障 */
            nos_sys_log_error("IPC remote send failed to %s: %s", conn->uds_path, strerror(errno));
            atomic_fetch_add(&g_node_ctx.stats.tx_errors, 1);
            close(conn->fd);
            conn->fd = -1;
            conn->is_connected = 0;
            pthread_mutex_unlock(&conn->lock);
            return;
        }
    }
    pthread_mutex_unlock(&conn->lock);
}

/**
 * @brief 接收报文并处理 (支持主动/被动连接)
 */
static void nos_ipc_recv_handler(int fd, nos_ipc_conn_t *conn, nos_thread_t *thread) {
    nos_service_msg_t header_tmp;
    ssize_t peek_len = recv(fd, &header_tmp, sizeof(header_tmp), MSG_PEEK | MSG_DONTWAIT);
    if (peek_len <= 0) {
        if (peek_len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        /* 连接断开 */
        if (conn) {
            nos_sys_log_info("Remote closed connection: %s", conn->uds_path);
            nos_scheduler_remove_fd(conn->owner_thread, fd);
            close(fd); conn->fd = -1; conn->is_connected = 0;
        } else {
            nos_sys_log_info("Passive connection closed (FD:%d)", fd);
            nos_scheduler_remove_fd(thread, fd);
            close(fd);
        }
        return;
    }

    if (header_tmp.magic != NOS_IPC_MAGIC) {
        nos_sys_log_error("Protocol error: Invalid magic. Dropping link.");
        if (conn) {
            nos_scheduler_remove_fd(conn->owner_thread, fd);
            close(fd); conn->fd = -1; conn->is_connected = 0;
        } else {
            nos_scheduler_remove_fd(thread, fd);
            close(fd);
        }
        return;
    }

    size_t full_msg_len = sizeof(nos_service_msg_t) + header_tmp.payload_len;
    nos_buffer_t *buf = nos_buffer_alloc(full_msg_len, 0);
    if (!buf) {
        atomic_fetch_add(&g_node_ctx.stats.buffer_alloc_fails, 1);
        recv(fd, NULL, 0, MSG_TRUNC | MSG_DONTWAIT); // 丢弃该包
        return;
    }

    ssize_t ret = recv(fd, buf->data, full_msg_len, MSG_DONTWAIT);
    if (ret == (ssize_t)full_msg_len) {
        atomic_fetch_add(&g_node_ctx.stats.rx_packets, 1);
        atomic_fetch_add(&g_node_ctx.stats.rx_bytes, ret);
        nos_service_msg_send(buf);
    } else {
        nos_sys_log_error("Partial recv or error on SEQPACKET.");
        atomic_fetch_add(&g_node_ctx.stats.rx_errors, 1);
    }
    nos_buffer_release(buf);
}

/**
 * @brief IPC 事件统一回调 (处理读写)
 */
static void nos_ipc_event_handler(int fd, void *arg) {
    nos_ipc_conn_t *conn = (nos_ipc_conn_t *)arg;
    if (fd < 0) return;

    /* 1. 处理写事件 (优先排干发送队列) */
    nos_ipc_process_tx(conn);

    /* 2. 处理读事件 */
    nos_ipc_recv_handler(fd, conn, conn->owner_thread);
}

/**
 * @brief 内部函数：获取或建立跨进程连接
 */
static nos_ipc_conn_t* get_or_create_conn(const char *uds_path) {
    pthread_mutex_lock(&g_pool_lock);
    for (uint32_t i = 0; i < g_remote_conn_count; i++) {
        if (strcmp(g_remote_conns[i].uds_path, uds_path) == 0) {
            pthread_mutex_unlock(&g_pool_lock);
            return &g_remote_conns[i];
        }
    }

    if (g_remote_conn_count >= MAX_REMOTE_CONNS) {
        pthread_mutex_unlock(&g_pool_lock); return NULL;
    }

    nos_ipc_conn_t *conn = &g_remote_conns[g_remote_conn_count++];
    strncpy(conn->uds_path, uds_path, 107);
    conn->fd = -1;
    conn->head = conn->tail = 0;
    conn->is_connected = 0;
    pthread_mutex_init(&conn->lock, NULL);
    conn->owner_thread = g_node_ctx.mgmt_thread; // 统一由管理线程处理 IO
    
    pthread_mutex_unlock(&g_pool_lock);
    return conn;
}

/**
 * @brief 异步发送入队接口 (组件线程调用)
 */
nos_status_t nos_ipc_send_enqueue(const char *uds_path, nos_buffer_t *buf) {
    nos_ipc_conn_t *conn = get_or_create_conn(uds_path);
    if (!conn) return NOS_ERR;

    pthread_mutex_lock(&conn->lock);
    
    /* 1. 自动重连逻辑 */
    if (conn->fd < 0) {
        int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (fd >= 0) {
            struct sockaddr_un addr = {.sun_family = AF_UNIX};
            strncpy(addr.sun_path, uds_path, sizeof(addr.sun_path) - 1);
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                conn->fd = fd;
                conn->is_connected = 1;
                /* 使用调度器标准接口注册，监听 IN 和 OUT */
                nos_scheduler_add_fd_ex(conn->owner_thread, fd, EPOLLIN | EPOLLOUT | EPOLLET, nos_ipc_event_handler, conn);
                nos_sys_log_info("IPC connected to %s (FD:%d)", uds_path, fd);
            } else {
                nos_sys_log_error("IPC connect to %s failed: %s", uds_path, strerror(errno));
                close(fd);
            }
        }
    }

    /* 2. 入队 */
    uint32_t next_tail = (conn->tail + 1) % TX_QUEUE_SIZE;
    if (next_tail == conn->head) {
        pthread_mutex_unlock(&conn->lock);
        atomic_fetch_add(&g_node_ctx.stats.dropped_full, 1);
        return NOS_ERR_BUSY;
    }

    nos_buffer_retain(buf);
    conn->tx_queue[conn->tail] = buf;
    conn->tail = next_tail;
    pthread_mutex_unlock(&conn->lock);

    /* 3. 如果已连接，主动触发一次尝试发送 */
    if (conn->is_connected) {
        nos_ipc_process_tx(conn);
    }
    
    return NOS_OK;
}

/**
 * @brief 处理被动接入的连接
 */
static void nos_ipc_passive_handler(int fd, void *arg) {
    nos_thread_t *thread = (nos_thread_t *)arg;
    nos_ipc_recv_handler(fd, NULL, thread);
}

/**
 * @brief 监听 Socket 的新连接回调
 */
static void nos_ipc_accept_handler(int listen_fd, void *arg) {
    nos_thread_t *thread = (nos_thread_t *)arg;
    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) return;

    nos_sys_log_info("IPC Accepted new connection: FD %d", client_fd);
    fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL, 0) | O_NONBLOCK);

    /* 直接将 thread 作为 arg 传递给被动处理器 */
    nos_scheduler_add_fd(thread, client_fd, nos_ipc_passive_handler, thread);
}

/**
 * @brief 初始化本地 IPC 监听
 */
nos_status_t nos_ipc_init(nos_thread_t *thread, const char *uds_path) {
    int listen_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (listen_fd < 0) return NOS_ERR;

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, uds_path, sizeof(addr.sun_path) - 1);
    unlink(uds_path);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        nos_sys_log_error("IPC Bind failed: %s", strerror(errno));
        close(listen_fd); return NOS_ERR;
    }

    if (listen(listen_fd, 5) < 0) { close(listen_fd); return NOS_ERR; }

    return nos_scheduler_add_fd(thread, listen_fd, nos_ipc_accept_handler, thread);
}
