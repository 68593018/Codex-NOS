#ifndef __NOS_SCHEDULER_H__
#define __NOS_SCHEDULER_H__

#include <pthread.h>
#include <stdatomic.h>
#include "nos_component.h"
#include "nos_buffer.h"
#include "nos_timer_api.h"

/**
 * @brief 文件描述符事件回调
 */
typedef void (*nos_fd_callback_t)(int fd, void *arg);

typedef struct {
    int fd;
    nos_fd_callback_t callback;
    void *arg;
} nos_fd_entry_t;

/**
 * @brief 定时器最小堆
 */
typedef struct {
    nos_timer_t      **nodes;
    uint32_t           size;
    uint32_t           capacity;
} nos_timer_heap_t;

/**
 * @brief 调度器线程对象
 */
typedef struct nos_thread_s {
    uint32_t thread_id;         /**< 逻辑线程 ID */
    const char *name;           /**< 线程名称 */
    
    int epoll_fd;               /**< epoll 句柄 */
    int notify_fd;              /**< 唤醒句柄 (eventfd) */
    volatile int stop_requested; /**< 停止请求标志 */
    _Atomic int is_sleeping;     /**< 线程是否睡眠 */
    nos_fd_entry_t notify_entry; /**< 专门用于唤醒句柄的 entry */
    
    pthread_mutex_t queue_lock; /**< 队列锁 */
    nos_buffer_t **msg_queue;
    int head, tail;

    nos_component_t **components;
    uint32_t component_count;

    nos_fd_entry_t *fd_entries;
    uint32_t fd_count;

    nos_timer_heap_t timer_heap; /**< 本地定时器堆 */

    /**
     * @brief 启动调度循环
     */
    nos_status_t (*run_loop)(struct nos_thread_s *self);

} nos_thread_t;

/**
 * @brief 向调度器添加需要监听的文件描述符（支持自定义事件，如 EPOLLOUT）
 */
nos_status_t nos_scheduler_add_fd_ex(nos_thread_t *thread, int fd, uint32_t events, nos_fd_callback_t callback, void *arg);

/**
 * @brief 向调度器添加需要监听的文件描述符 (默认 EPOLLIN)
 */
nos_status_t nos_scheduler_add_fd(nos_thread_t *thread, int fd, nos_fd_callback_t callback, void *arg);

/**
 * @brief 从调度器移除监听的文件描述符
 */
nos_status_t nos_scheduler_remove_fd(nos_thread_t *thread, int fd);

/**
 * @brief 注册组件到线程调度器
 */
nos_status_t nos_scheduler_register_component(nos_thread_t *thread, nos_component_t *comp);

/**
 * @brief 从调度器注销组件
 */
nos_status_t nos_scheduler_unregister_component(nos_thread_t *thread, nos_component_t *comp);

/**
 * @brief 启动调度循环
 */
nos_status_t nos_scheduler_run_loop(nos_thread_t *self);

/**
 * @brief 通知调度器线程停止运行
 */
void nos_scheduler_stop(nos_thread_t *thread);

/**
 * @brief 初始化线程调度器对象
 */
nos_status_t nos_scheduler_init_thread(nos_thread_t *thread, uint32_t id, const char *name);

/**
 * @brief 清理线程调度器资源
 */
void nos_scheduler_deinit_thread(nos_thread_t *thread);

/**
 * @brief 获取所有线程调度器总内存占用 (Byte)
 */
size_t nos_scheduler_get_total_mem_usage(void);

#endif /* __NOS_SCHEDULER_H__ */
