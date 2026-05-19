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
#include <stdint.h>
#include <time.h>
#include "nos_scheduler.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_api.h"
#include "nos_node_priv.h"
#include "nos_stats.h"
#include "nos_shm_ipc.h"

#define TX_QUEUE_SIZE 1024
#define MAX_REMOTE_CONNS 16
#define NOS_IPC_MAX_PAYLOAD_LEN 4096
#define IPC_RETRY_INITIAL_MS 100
#define IPC_RETRY_MAX_MS 5000
#define NOS_IPC_CTRL_SHM_SETUP 0x53484D31u
#define NOS_IPC_CTRL_SHM_NOTIFY 0x53484D32u

typedef enum {
    NOS_IPC_DISCONNECTED = 0,
    NOS_IPC_CONNECTING,
    NOS_IPC_CONNECTED,
    NOS_IPC_BACKOFF
} nos_ipc_conn_state_t;

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
    nos_ipc_conn_state_t state;
    uint64_t next_retry_ms;
    uint32_t retry_delay_ms;
    uint32_t connect_fail_count;
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t tx_errors;
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t rx_errors;
    uint64_t dropped_full;
    uint32_t generation;
    int is_passive;
    char remote_node[32];
    char shm_name[NOS_SHM_NAME_MAX];
    nos_shm_channel_t *shm;
    nos_thread_t *owner_thread; // 绑定的 IO 线程
} nos_ipc_conn_t;

typedef struct {
    char shm_name[NOS_SHM_NAME_MAX];
    char peer_node[32];
} nos_ipc_shm_setup_t;

static nos_ipc_conn_t g_remote_conns[MAX_REMOTE_CONNS];
static uint32_t g_remote_conn_count = 0;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;

static void nos_ipc_event_handler(int fd, void *arg);

void nos_ipc_register_stats(void) {
    nos_stats_register_counter("ipc", "tx_packets", (const volatile uint64_t *)&g_node_ctx.stats.tx_packets);
    nos_stats_register_counter("ipc", "rx_packets", (const volatile uint64_t *)&g_node_ctx.stats.rx_packets);
    nos_stats_register_counter("ipc", "tx_errors", (const volatile uint64_t *)&g_node_ctx.stats.tx_errors);
    nos_stats_register_counter("ipc", "rx_errors", (const volatile uint64_t *)&g_node_ctx.stats.rx_errors);
    nos_stats_register_counter("ipc", "dropped_full", (const volatile uint64_t *)&g_node_ctx.stats.dropped_full);
    nos_stats_register_counter("ipc", "buffer_alloc_fails", (const volatile uint64_t *)&g_node_ctx.stats.buffer_alloc_fails);
    nos_shm_register_stats();
}

static int ipc_is_ctrl_msg(const nos_service_msg_t *msg) {
    return msg && (msg->flags & NOS_MSG_F_IPC_CTRL);
}

static void ipc_make_shm_name(char *out, size_t out_size, const char *local, const char *remote) {
    snprintf(out, out_size, "/nos_shm_%s_%s", local ? local : "local", remote ? remote : "remote");
}

static void ipc_send_ctrl(int fd, uint32_t code, const void *payload, uint32_t payload_len) {
    uint8_t data[sizeof(nos_service_msg_t) + sizeof(nos_ipc_shm_setup_t)];
    if (payload_len > sizeof(nos_ipc_shm_setup_t)) return;
    memset(data, 0, sizeof(data));
    nos_service_msg_t *msg = (nos_service_msg_t *)data;
    msg->magic = NOS_IPC_MAGIC;
    msg->version = NOS_IPC_VERSION;
    msg->dst_service = 0;
    msg->msg_code = code;
    msg->flags = NOS_MSG_F_IPC_CTRL;
    msg->payload_len = payload_len;
    if (payload && payload_len > 0) memcpy(msg + 1, payload, payload_len);
    (void)send(fd, data, sizeof(nos_service_msg_t) + payload_len, MSG_NOSIGNAL | MSG_DONTWAIT);
}

static uint64_t ipc_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static const char* ipc_state_name(nos_ipc_conn_state_t state) {
    switch (state) {
        case NOS_IPC_DISCONNECTED: return "DISCONNECTED";
        case NOS_IPC_CONNECTING: return "CONNECTING";
        case NOS_IPC_CONNECTED: return "CONNECTED";
        case NOS_IPC_BACKOFF: return "BACKOFF";
        default: return "UNKNOWN";
    }
}

static uint32_t ipc_queue_len(const nos_ipc_conn_t *conn) {
    if (conn->tail >= conn->head) return conn->tail - conn->head;
    return TX_QUEUE_SIZE - conn->head + conn->tail;
}

static void nos_ipc_mark_disconnected(nos_ipc_conn_t *conn) {
    if (!conn) return;
    if (conn->fd >= 0) {
        nos_scheduler_remove_fd(conn->owner_thread, conn->fd);
        close(conn->fd);
    }
    if (conn->shm) {
        nos_shm_channel_close(conn->shm);
        conn->shm = NULL;
    }
    conn->fd = -1;
    conn->is_connected = 0;
    conn->state = NOS_IPC_DISCONNECTED;
    conn->generation++;
}

static void nos_ipc_mark_connect_failed(nos_ipc_conn_t *conn, const char *reason) {
    uint32_t delay = conn->retry_delay_ms ? conn->retry_delay_ms : IPC_RETRY_INITIAL_MS;
    conn->connect_fail_count++;
    conn->retry_delay_ms = (delay >= IPC_RETRY_MAX_MS / 2) ? IPC_RETRY_MAX_MS : delay * 2;
    conn->next_retry_ms = ipc_monotonic_ms() + delay;
    conn->state = NOS_IPC_BACKOFF;
    conn->is_connected = 0;

    if (conn->connect_fail_count == 1 || delay >= IPC_RETRY_MAX_MS) {
        nos_sys_log_error("IPC connect to %s failed: %s; retry in %u ms", conn->uds_path, reason, delay);
    }
}

void nos_ipc_dump_stats(void) {
    uint64_t now = ipc_monotonic_ms();

    printf("\n--- IPC Connections ---\n");
    printf("%-28s %-13s %-4s %-6s %-10s %-8s\n", "Path", "State", "FD", "Queue", "Retry(ms)", "Failures");
    printf("----------------------------------------------------------------------------\n");

    pthread_mutex_lock(&g_pool_lock);
    if (g_remote_conn_count == 0) {
        printf("(no remote connections)\n");
        pthread_mutex_unlock(&g_pool_lock);
        return;
    }

    for (uint32_t i = 0; i < g_remote_conn_count; i++) {
        nos_ipc_conn_t *conn = &g_remote_conns[i];
        pthread_mutex_lock(&conn->lock);
        uint64_t retry_left = 0;
        if (conn->state == NOS_IPC_BACKOFF && conn->next_retry_ms > now) {
            retry_left = conn->next_retry_ms - now;
        }
        printf("%-28s %-13s %-4d %-6u %-10llu %-8u\n",
               conn->uds_path,
               ipc_state_name(conn->state),
               conn->fd,
               ipc_queue_len(conn),
               (unsigned long long)retry_left,
               conn->connect_fail_count);
        pthread_mutex_unlock(&conn->lock);
    }
    pthread_mutex_unlock(&g_pool_lock);

    printf("\n--- IPC Traffic ---\n");
    printf("%-28s %-8s %-10s %-7s %-8s %-10s %-7s %-6s\n",
           "Path", "TX-Pkts", "TX-Bytes", "TX-Err", "RX-Pkts", "RX-Bytes", "RX-Err", "Drop");
    printf("------------------------------------------------------------------------------------------\n");

    pthread_mutex_lock(&g_pool_lock);
    for (uint32_t i = 0; i < g_remote_conn_count; i++) {
        nos_ipc_conn_t *conn = &g_remote_conns[i];
        pthread_mutex_lock(&conn->lock);
        printf("%-28s %-8llu %-10llu %-7llu %-8llu %-10llu %-7llu %-6llu\n",
               conn->uds_path,
               (unsigned long long)conn->tx_packets,
               (unsigned long long)conn->tx_bytes,
               (unsigned long long)conn->tx_errors,
               (unsigned long long)conn->rx_packets,
               (unsigned long long)conn->rx_bytes,
               (unsigned long long)conn->rx_errors,
               (unsigned long long)conn->dropped_full);
        pthread_mutex_unlock(&conn->lock);
    }
    pthread_mutex_unlock(&g_pool_lock);
}

static int nos_ipc_try_connect_locked(nos_ipc_conn_t *conn) {
    uint64_t now = ipc_monotonic_ms();
    if (conn->state == NOS_IPC_BACKOFF && now < conn->next_retry_ms) return 0;

    conn->state = NOS_IPC_CONNECTING;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        nos_ipc_mark_connect_failed(conn, strerror(errno));
        return 0;
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, conn->uds_path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        char reason[128];
        snprintf(reason, sizeof(reason), "%s", strerror(errno));
        close(fd);
        nos_ipc_mark_connect_failed(conn, reason);
        return 0;
    }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    conn->fd = fd;
    conn->is_connected = 1;
    conn->state = NOS_IPC_CONNECTED;
    conn->generation++;
    conn->retry_delay_ms = IPC_RETRY_INITIAL_MS;
    conn->next_retry_ms = 0;
    conn->connect_fail_count = 0;
    nos_scheduler_add_fd_ex(conn->owner_thread, fd, EPOLLIN | EPOLLOUT | EPOLLET, nos_ipc_event_handler, conn);
    if (!conn->shm && conn->remote_node[0] != '\0' && g_node_ctx.node_def) {
        nos_ipc_shm_setup_t setup;
        memset(&setup, 0, sizeof(setup));
        ipc_make_shm_name(conn->shm_name, sizeof(conn->shm_name), g_node_ctx.node_def->name, conn->remote_node);
        strncpy(setup.shm_name, conn->shm_name, sizeof(setup.shm_name) - 1);
        strncpy(setup.peer_node, g_node_ctx.node_def->name, sizeof(setup.peer_node) - 1);
        conn->shm = nos_shm_channel_create(conn->shm_name, 1);
        if (conn->shm) ipc_send_ctrl(conn->fd, NOS_IPC_CTRL_SHM_SETUP, &setup, sizeof(setup));
    }
    nos_sys_log_info("IPC connected to %s (FD:%d)", conn->uds_path, fd);
    return 1;
}

/**
 * @brief 尝试从队列中发送数据
 */
static void nos_ipc_process_tx(nos_ipc_conn_t *conn) {
    pthread_mutex_lock(&conn->lock);
    if (conn->state != NOS_IPC_CONNECTED || conn->fd < 0) {
        pthread_mutex_unlock(&conn->lock);
        return;
    }
    while (conn->head != conn->tail) {
        nos_buffer_t *buf = conn->tx_queue[conn->head];
        nos_service_msg_t *header = (nos_service_msg_t *)buf->data;
        size_t full_len = sizeof(nos_service_msg_t) + header->payload_len;

        if (!ipc_is_ctrl_msg(header) && conn->shm &&
            full_len <= NOS_SHM_SLOT_SIZE &&
            nos_shm_channel_write(conn->shm, buf->data, (uint32_t)full_len) == NOS_OK) {
            ipc_send_ctrl(conn->fd, NOS_IPC_CTRL_SHM_NOTIFY, NULL, 0);
            conn->tx_packets++;
            conn->tx_bytes += full_len;
            nos_buffer_release(buf);
            conn->head = (conn->head + 1) % TX_QUEUE_SIZE;
            continue;
        }

        /* SEQPACKET 保证原子写入整个报文 */
        ssize_t sent = send(conn->fd, buf->data, full_len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent == (ssize_t)full_len) {
            atomic_fetch_add(&g_node_ctx.stats.tx_packets, 1);
            atomic_fetch_add(&g_node_ctx.stats.tx_bytes, full_len);
            conn->tx_packets++;
            conn->tx_bytes += full_len;
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
            conn->tx_errors++;
            nos_ipc_mark_disconnected(conn);
            pthread_mutex_unlock(&conn->lock);
            return;
        }
    }
    pthread_mutex_unlock(&conn->lock);
}

nos_status_t nos_ipc_send_reply(void *reply_conn, uint32_t generation, nos_buffer_t *buf) {
    nos_ipc_conn_t *conn = (nos_ipc_conn_t *)reply_conn;
    if (!conn || !buf) return NOS_ERR;

    pthread_mutex_lock(&conn->lock);
    if (conn->state != NOS_IPC_CONNECTED || conn->fd < 0 || conn->generation != generation) {
        pthread_mutex_unlock(&conn->lock);
        return NOS_ERR;
    }

    uint32_t next_tail = (conn->tail + 1) % TX_QUEUE_SIZE;
    if (next_tail == conn->head) {
        conn->dropped_full++;
        pthread_mutex_unlock(&conn->lock);
        atomic_fetch_add(&g_node_ctx.stats.dropped_full, 1);
        return NOS_ERR_BUSY;
    }

    nos_buffer_retain(buf);
    conn->tx_queue[conn->tail] = buf;
    conn->tail = next_tail;
    pthread_mutex_unlock(&conn->lock);

    nos_ipc_process_tx(conn);
    return NOS_OK;
}

static void ipc_deliver_rx_data(const void *data, uint32_t len, nos_ipc_conn_t *conn) {
    if (!data || len < sizeof(nos_service_msg_t)) return;
    const nos_service_msg_t *header = (const nos_service_msg_t *)data;
    if (header->payload_len > NOS_IPC_MAX_PAYLOAD_LEN) {
        nos_sys_log_error("IPC payload too large: %u bytes", header->payload_len);
        atomic_fetch_add(&g_node_ctx.stats.rx_errors, 1);
        if (conn) conn->rx_errors++;
        return;
    }

    size_t full_msg_len = sizeof(nos_service_msg_t) + header->payload_len;
    if (len != full_msg_len) {
        nos_sys_log_error("IPC message length mismatch: got %u expected %zu", len, full_msg_len);
        atomic_fetch_add(&g_node_ctx.stats.rx_errors, 1);
        if (conn) conn->rx_errors++;
        return;
    }

    nos_buffer_t *buf = nos_buffer_alloc((uint32_t)full_msg_len, 0);
    if (!buf) {
        atomic_fetch_add(&g_node_ctx.stats.buffer_alloc_fails, 1);
        if (conn) conn->rx_errors++;
        return;
    }

    memcpy(buf->data, data, full_msg_len);
    if (conn) {
        extern nos_reply_ctx_t* nos_reply_ctx_create(void *ipc_conn, uint32_t ipc_generation, uint32_t corr_id, uint32_t requester_component);
        buf->reply_ctx = nos_reply_ctx_create(conn, conn->generation, header->seq, header->src_component);
        conn->rx_packets++;
        conn->rx_bytes += (uint64_t)full_msg_len;
    }
    atomic_fetch_add(&g_node_ctx.stats.rx_packets, 1);
    atomic_fetch_add(&g_node_ctx.stats.rx_bytes, full_msg_len);
    nos_service_msg_send(buf);
    nos_buffer_release(buf);
}

static void ipc_shm_drain_cb(const void *data, uint32_t len, void *arg) {
    ipc_deliver_rx_data(data, len, (nos_ipc_conn_t *)arg);
}

static void ipc_handle_ctrl_msg(nos_ipc_conn_t *conn, const nos_service_msg_t *msg) {
    if (!conn || !msg) return;
    if (msg->msg_code == NOS_IPC_CTRL_SHM_SETUP && msg->payload_len >= sizeof(nos_ipc_shm_setup_t)) {
        const nos_ipc_shm_setup_t *setup = (const nos_ipc_shm_setup_t *)(msg + 1);
        strncpy(conn->remote_node, setup->peer_node, sizeof(conn->remote_node) - 1);
        strncpy(conn->shm_name, setup->shm_name, sizeof(conn->shm_name) - 1);
        if (!conn->shm) {
            conn->shm = nos_shm_channel_open(conn->shm_name, 0);
            if (conn->shm) {
                nos_sys_log_info("IPC SHM channel attached: %s peer=%s", conn->shm_name, conn->remote_node);
            }
        }
    } else if (msg->msg_code == NOS_IPC_CTRL_SHM_NOTIFY) {
        if (conn->shm) {
            nos_shm_channel_drain(conn->shm, ipc_shm_drain_cb, conn);
        }
    }
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
            nos_ipc_mark_disconnected(conn);
        } else {
            nos_sys_log_info("Passive connection closed (FD:%d)", fd);
            nos_scheduler_remove_fd(thread, fd);
            close(fd);
        }
        return;
    }

    if (header_tmp.magic != NOS_IPC_MAGIC) {
        nos_sys_log_error("Protocol error: Invalid magic. Dropping link.");
        if (conn) conn->rx_errors++;
        if (conn) {
            nos_ipc_mark_disconnected(conn);
        } else {
            nos_scheduler_remove_fd(thread, fd);
            close(fd);
        }
        return;
    }

    size_t full_msg_len = sizeof(nos_service_msg_t) + header_tmp.payload_len;
    if (header_tmp.payload_len > NOS_IPC_MAX_PAYLOAD_LEN) {
        nos_sys_log_error("IPC payload too large: %u bytes", header_tmp.payload_len);
        atomic_fetch_add(&g_node_ctx.stats.rx_errors, 1);
        if (conn) conn->rx_errors++;
        recv(fd, NULL, 0, MSG_TRUNC | MSG_DONTWAIT);
        return;
    }

    nos_buffer_t *buf = nos_buffer_alloc(full_msg_len, 0);
    if (!buf) {
        atomic_fetch_add(&g_node_ctx.stats.buffer_alloc_fails, 1);
        if (conn) conn->rx_errors++;
        recv(fd, NULL, 0, MSG_TRUNC | MSG_DONTWAIT); // 丢弃该包
        return;
    }

    ssize_t ret = recv(fd, buf->data, full_msg_len, MSG_DONTWAIT);
    if (ret == (ssize_t)full_msg_len) {
        nos_service_msg_t *rx_msg = (nos_service_msg_t *)buf->data;
        if (ipc_is_ctrl_msg(rx_msg)) {
            ipc_handle_ctrl_msg(conn, rx_msg);
        } else {
            ipc_deliver_rx_data(buf->data, (uint32_t)ret, conn);
        }
    } else {
        nos_sys_log_error("Partial recv or error on SEQPACKET.");
        atomic_fetch_add(&g_node_ctx.stats.rx_errors, 1);
        if (conn) conn->rx_errors++;
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
    if (conn->state != NOS_IPC_CONNECTED || conn->fd < 0) return;

    /* 2. 处理读事件 */
    nos_ipc_recv_handler(fd, conn, conn->owner_thread);
}

/**
 * @brief 内部函数：获取或建立跨进程连接
 */
static nos_ipc_conn_t* get_or_create_conn(const char *node_name, const char *uds_path) {
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
    if (node_name) strncpy(conn->remote_node, node_name, sizeof(conn->remote_node) - 1);
    conn->fd = -1;
    conn->head = conn->tail = 0;
    conn->is_connected = 0;
    conn->state = NOS_IPC_DISCONNECTED;
    conn->next_retry_ms = 0;
    conn->retry_delay_ms = IPC_RETRY_INITIAL_MS;
    conn->connect_fail_count = 0;
    conn->tx_packets = 0;
    conn->tx_bytes = 0;
    conn->tx_errors = 0;
    conn->rx_packets = 0;
    conn->rx_bytes = 0;
    conn->rx_errors = 0;
    conn->dropped_full = 0;
    conn->generation = 1;
    conn->is_passive = 0;
    pthread_mutex_init(&conn->lock, NULL);
    conn->owner_thread = g_node_ctx.mgmt_thread; // 统一由管理线程处理 IO
    
    pthread_mutex_unlock(&g_pool_lock);
    return conn;
}

static nos_ipc_conn_t* create_passive_conn(int fd, nos_thread_t *thread) {
    pthread_mutex_lock(&g_pool_lock);
    if (g_remote_conn_count >= MAX_REMOTE_CONNS) {
        pthread_mutex_unlock(&g_pool_lock);
        return NULL;
    }

    nos_ipc_conn_t *conn = &g_remote_conns[g_remote_conn_count++];
    snprintf(conn->uds_path, sizeof(conn->uds_path), "passive:%d", fd);
    conn->fd = fd;
    conn->head = conn->tail = 0;
    conn->is_connected = 1;
    conn->state = NOS_IPC_CONNECTED;
    conn->next_retry_ms = 0;
    conn->retry_delay_ms = IPC_RETRY_INITIAL_MS;
    conn->connect_fail_count = 0;
    conn->tx_packets = 0;
    conn->tx_bytes = 0;
    conn->tx_errors = 0;
    conn->rx_packets = 0;
    conn->rx_bytes = 0;
    conn->rx_errors = 0;
    conn->dropped_full = 0;
    conn->generation = 1;
    conn->is_passive = 1;
    pthread_mutex_init(&conn->lock, NULL);
    conn->owner_thread = thread;

    pthread_mutex_unlock(&g_pool_lock);
    return conn;
}

/**
 * @brief 异步发送入队接口 (组件线程调用)
 */
nos_status_t nos_ipc_send_enqueue(const char *node_name, const char *uds_path, nos_buffer_t *buf) {
    nos_ipc_conn_t *conn = get_or_create_conn(node_name, uds_path);
    if (!conn) return NOS_ERR;

    pthread_mutex_lock(&conn->lock);
    
    /* 1. 入队 */
    uint32_t next_tail = (conn->tail + 1) % TX_QUEUE_SIZE;
    if (next_tail == conn->head) {
        conn->dropped_full++;
        pthread_mutex_unlock(&conn->lock);
        atomic_fetch_add(&g_node_ctx.stats.dropped_full, 1);
        return NOS_ERR_BUSY;
    }

    nos_buffer_retain(buf);
    conn->tx_queue[conn->tail] = buf;
    conn->tail = next_tail;

    /* 2. 自动重连逻辑，退避期内只排队不重复 connect */
    if (conn->fd < 0 || conn->state != NOS_IPC_CONNECTED) {
        nos_ipc_try_connect_locked(conn);
    }
    pthread_mutex_unlock(&conn->lock);

    /* 3. 如果已连接，主动触发一次尝试发送 */
    if (conn->state == NOS_IPC_CONNECTED) {
        nos_ipc_process_tx(conn);
    }
    
    return NOS_OK;
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

    nos_ipc_conn_t *conn = create_passive_conn(client_fd, thread);
    if (!conn) {
        close(client_fd);
        return;
    }
    nos_scheduler_add_fd_ex(thread, client_fd, EPOLLIN | EPOLLOUT | EPOLLET, nos_ipc_event_handler, conn);
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
