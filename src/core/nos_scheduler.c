#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <stdatomic.h>
#include <errno.h>
#include <time.h>
#include "nos_scheduler.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_api.h"
#include "nos_node_priv.h"

#define MAX_QUEUE_SIZE 1024
#define MAX_EVENTS 10
#define MAX_COMP_PER_THREAD 32

/* 线程局部变量：存储当前线程的调度器指针 */
static __thread nos_thread_t *t_local_scheduler = NULL;

/* 服务注册表项 */
typedef struct {
    uint32_t service_id;
    nos_component_t *local_provider;
    nos_thread_t *local_thread;
    char remote_uds_path[108];
    int is_remote;
} service_entry_t;

static service_entry_t g_service_registry[64];
static uint32_t g_service_count = 0;

static service_entry_t* find_service_entry(uint32_t service_id) {
    for (uint32_t i = 0; i < g_service_count; i++) {
        if (g_service_registry[i].service_id == service_id) return &g_service_registry[i];
    }
    return NULL;
}

/* --- 定时器对象实现 --- */

struct nos_timer_s {
    uint64_t expire_at_ms;
    uint32_t interval_ms;
    int      is_periodic;
    nos_timer_cb_t callback;
    nos_timer_free_arg_t free_arg;
    void    *arg;
    int      is_running;
    nos_thread_t *owner_thread; // 归属线程
};

static uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void heapify_up(nos_thread_t *thread, uint32_t idx) {
    while (idx > 0) {
        uint32_t parent = (idx - 1) / 2;
        if (thread->timer_heap.nodes[idx]->expire_at_ms >= thread->timer_heap.nodes[parent]->expire_at_ms) break;
        nos_timer_t *tmp = thread->timer_heap.nodes[idx];
        thread->timer_heap.nodes[idx] = thread->timer_heap.nodes[parent];
        thread->timer_heap.nodes[parent] = tmp;
        idx = parent;
    }
}

static void heapify_down(nos_thread_t *thread, uint32_t idx) {
    while (idx * 2 + 1 < thread->timer_heap.size) {
        uint32_t smallest = idx * 2 + 1;
        uint32_t right = idx * 2 + 2;
        if (right < thread->timer_heap.size && thread->timer_heap.nodes[right]->expire_at_ms < thread->timer_heap.nodes[smallest]->expire_at_ms) {
            smallest = right;
        }
        if (thread->timer_heap.nodes[idx]->expire_at_ms <= thread->timer_heap.nodes[smallest]->expire_at_ms) break;
        nos_timer_t *tmp = thread->timer_heap.nodes[idx];
        thread->timer_heap.nodes[idx] = thread->timer_heap.nodes[smallest];
        thread->timer_heap.nodes[smallest] = tmp;
        idx = smallest;
    }
}

static void process_timers(nos_thread_t *self) {
    uint64_t now = get_monotonic_ms();
    while (self->timer_heap.size > 0 && now >= self->timer_heap.nodes[0]->expire_at_ms) {
        nos_timer_t *timer = self->timer_heap.nodes[0];
        self->timer_heap.nodes[0] = self->timer_heap.nodes[--self->timer_heap.size];
        heapify_down(self, 0);
        timer->is_running = 0;

        if (timer->callback) timer->callback(timer->arg);

        if (timer->is_periodic) {
            timer->expire_at_ms = get_monotonic_ms() + timer->interval_ms;
            timer->is_running = 1;
            self->timer_heap.nodes[self->timer_heap.size] = timer;
            heapify_up(self, self->timer_heap.size++);
        }
        now = get_monotonic_ms();
    }
}

/* --- 调度器核心实现 --- */

static size_t get_thread_mem_usage(nos_thread_t *t) {
    if (!t) return 0;
    size_t total = sizeof(nos_thread_t);
    total += MAX_COMP_PER_THREAD * sizeof(nos_component_t*);
    total += 32 * sizeof(nos_fd_entry_t);
    total += MAX_QUEUE_SIZE * sizeof(nos_buffer_t*);
    total += t->timer_heap.capacity * sizeof(nos_timer_t*);
    return total;
}

size_t nos_scheduler_get_total_mem_usage(void) {
    extern nos_node_ctx_t g_node_ctx;
    size_t total = get_thread_mem_usage(g_node_ctx.mgmt_thread);
    for (uint32_t i = 0; i < g_node_ctx.worker_count; i++) {
        total += get_thread_mem_usage(&g_node_ctx.worker_threads[i]);
    }
    return total;
}

nos_status_t nos_scheduler_init_thread(nos_thread_t *thread, uint32_t id, const char *name) {
    if (!thread) return NOS_ERR;
    thread->thread_id = id;
    thread->name = name;
    thread->components = calloc(MAX_COMP_PER_THREAD, sizeof(nos_component_t*));
    thread->component_count = 0;
    thread->fd_entries = calloc(32, sizeof(nos_fd_entry_t));
    thread->fd_count = 0;
    thread->msg_queue = calloc(MAX_QUEUE_SIZE, sizeof(nos_buffer_t*));
    thread->head = thread->tail = 0;
    thread->stop_requested = 0;
    atomic_init(&thread->is_sleeping, 0);
    pthread_mutex_init(&thread->queue_lock, NULL);
    thread->epoll_fd = epoll_create1(0);

    thread->notify_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (thread->notify_fd < 0) return NOS_ERR;

    /* 为 notify_fd 建立专门的 entry */
    thread->notify_entry.fd = thread->notify_fd;
    thread->notify_entry.callback = NULL;
    thread->notify_entry.arg = thread;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &thread->notify_entry;
    epoll_ctl(thread->epoll_fd, EPOLL_CTL_ADD, thread->notify_fd, &ev);

    thread->timer_heap.capacity = 128;
    thread->timer_heap.size = 0;
    thread->timer_heap.nodes = calloc(thread->timer_heap.capacity, sizeof(nos_timer_t*));
    return NOS_OK;
}

void nos_scheduler_deinit_thread(nos_thread_t *thread) {
    if (!thread) return;

    /* 1. 释放 FDs 和 epoll */
    close(thread->epoll_fd);
    close(thread->notify_fd);

    /* 2. 释放内部队列的消息 (如果有) */
    pthread_mutex_lock(&thread->queue_lock);
    while (thread->head != thread->tail) {
        nos_buffer_release(thread->msg_queue[thread->head]);
        thread->head = (thread->head + 1) % MAX_QUEUE_SIZE;
    }
    pthread_mutex_unlock(&thread->queue_lock);
    pthread_mutex_destroy(&thread->queue_lock);


    /* 3. 释放定时器堆中的定时器 (由框架创建的容器，内部 timer 需由组件释放或在此兜底) */
    /* 注意：工业级实现中，定时器通常由组件管理，此处仅释放容器数组 */
    for (uint32_t i = 0; i < thread->timer_heap.size; i++) {
        // nos_timer_delete(thread->timer_heap.nodes[i]); // 不能盲目删除，可能已被组件 delete
    }

    /* 4. 释放动态内存 */
    if (thread->components) free(thread->components);
    if (thread->fd_entries) free(thread->fd_entries);
    if (thread->msg_queue)  free(thread->msg_queue);
    if (thread->timer_heap.nodes) free(thread->timer_heap.nodes);
    
    nos_sys_log_debug("Scheduler thread resources for '%s' released.", thread->name);
}

static nos_thread_t* find_thread_of_component(nos_component_t *comp) {
    extern nos_node_ctx_t g_node_ctx;
    if (g_node_ctx.mgmt_thread) {
        for (uint32_t i = 0; i < g_node_ctx.mgmt_thread->component_count; i++) {
            if (g_node_ctx.mgmt_thread->components[i] == comp) return g_node_ctx.mgmt_thread;
        }
    }
    if (g_node_ctx.worker_threads) {
        for (uint32_t t = 0; t < g_node_ctx.worker_count; t++) {
            nos_thread_t *thread = &g_node_ctx.worker_threads[t];
            for (uint32_t c = 0; c < thread->component_count; c++) {
                if (thread->components[c] == comp) return thread;
            }
        }
    }
    return NULL;
}

nos_status_t nos_service_register_provider_bind(uint32_t service_id, nos_component_t *provider, nos_thread_t *thread) {
    service_entry_t *entry = find_service_entry(service_id);
    if (!entry) {
        if (g_service_count >= 64) return NOS_ERR;
        entry = &g_service_registry[g_service_count++];
    }
    entry->service_id = service_id;
    entry->local_provider = provider;
    entry->local_thread = thread;
    entry->is_remote = 0;
    return NOS_OK;
}

nos_status_t nos_service_register_remote(uint32_t service_id, const char *uds_path) {
    service_entry_t *entry = find_service_entry(service_id);
    if (!entry) {
        if (g_service_count >= 64) return NOS_ERR;
        entry = &g_service_registry[g_service_count++];
    }
    entry->service_id = service_id;
    entry->is_remote = 1;
    strncpy(entry->remote_uds_path, uds_path, sizeof(entry->remote_uds_path) - 1);
    return NOS_OK;
}

nos_status_t nos_scheduler_register_component(nos_thread_t *thread, nos_component_t *comp) {
    if (!thread || !comp) return NOS_ERR;
    if (thread->component_count >= MAX_COMP_PER_THREAD) return NOS_ERR;
    thread->components[thread->component_count++] = comp;
    return NOS_OK;
}

nos_status_t nos_scheduler_unregister_component(nos_thread_t *thread, nos_component_t *comp) {
    if (!thread || !comp) return NOS_ERR;
    for (uint32_t i = 0; i < thread->component_count; i++) {
        if (thread->components[i] == comp) {
            thread->components[i] = thread->components[thread->component_count - 1];
            thread->component_count--;
            return NOS_OK;
        }
    }
    return NOS_ERR;
}

nos_status_t nos_service_unregister_provider(uint32_t service_id) {
    for (uint32_t i = 0; i < g_service_count; i++) {
        if (g_service_registry[i].service_id == service_id) {
            g_service_registry[i] = g_service_registry[--g_service_count];
            return NOS_OK;
        }
    }
    return NOS_ERR;
}

nos_status_t nos_scheduler_add_fd_ex(nos_thread_t *thread, int fd, uint32_t events, nos_fd_callback_t callback, void *arg) {
    if (!thread || thread->fd_count >= 32) return NOS_ERR;
    nos_fd_entry_t *entry = &thread->fd_entries[thread->fd_count++];
    entry->fd = fd; entry->callback = callback; entry->arg = arg;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.ptr = entry;
    return (epoll_ctl(thread->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) ? NOS_ERR : NOS_OK;
}

nos_status_t nos_scheduler_add_fd(nos_thread_t *thread, int fd, nos_fd_callback_t callback, void *arg) {
    return nos_scheduler_add_fd_ex(thread, fd, EPOLLIN, callback, arg);
}

nos_status_t nos_scheduler_remove_fd(nos_thread_t *thread, int fd) {
    if (!thread) return NOS_ERR;
    epoll_ctl(thread->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    for (uint32_t i = 0; i < thread->fd_count; i++) {
        if (thread->fd_entries[i].fd == fd) {
            thread->fd_entries[i] = thread->fd_entries[--thread->fd_count];
            return NOS_OK;
        }
    }
    return NOS_ERR;
}

static nos_status_t nos_transport_local_send(nos_buffer_t *buf, nos_thread_t *target_thread) {
    if (!target_thread) return NOS_ERR;
    pthread_mutex_lock(&target_thread->queue_lock);
    
    int next_tail = (target_thread->tail + 1) % MAX_QUEUE_SIZE;
    if (next_tail == target_thread->head) { pthread_mutex_unlock(&target_thread->queue_lock); return NOS_ERR_BUSY; }
    nos_buffer_retain(buf);
    target_thread->msg_queue[target_thread->tail] = buf;
    target_thread->tail = next_tail;
    pthread_mutex_unlock(&target_thread->queue_lock);
    
    /* Zero-syscall optimization: Only write to eventfd if target thread is sleeping */
    if (atomic_load_explicit(&target_thread->is_sleeping, memory_order_acquire)) {
        uint64_t val = 1;
        if (write(target_thread->notify_fd, &val, sizeof(val)) < 0) { /* Ignore */ }
    }
    return NOS_OK;
}

static nos_status_t nos_transport_remote_uds_send(nos_buffer_t *buf, const char *uds_path) {
    extern nos_status_t nos_ipc_send_enqueue(const char *uds_path, nos_buffer_t *buf);
    return nos_ipc_send_enqueue(uds_path, buf);
}

nos_status_t nos_service_msg_send(nos_buffer_t *buf) {
    if (!buf) return NOS_ERR;
    nos_service_msg_t *header = (nos_service_msg_t *)buf->data;
    service_entry_t *entry = find_service_entry(header->dst_service);
    if (entry) {
        return entry->is_remote ? nos_transport_remote_uds_send(buf, entry->remote_uds_path) : nos_transport_local_send(buf, entry->local_thread);
    }
    return NOS_ERR;
}

static void process_thread_messages(nos_thread_t *self) {
    while (1) {
        nos_buffer_t *buf = NULL;
        pthread_mutex_lock(&self->queue_lock);
        if (self->head != self->tail) {
            buf = self->msg_queue[self->head];
            self->head = (self->head + 1) % MAX_QUEUE_SIZE;
        }
        pthread_mutex_unlock(&self->queue_lock);
        
        if (!buf) break;

        nos_service_msg_t *header = (nos_service_msg_t *)buf->data;
        service_entry_t *entry = find_service_entry(header->dst_service);
        if (entry && entry->local_provider && entry->local_provider->on_msg) {
            entry->local_provider->on_msg(entry->local_provider, header);
        }
        nos_buffer_release(buf);
    }
}

void nos_scheduler_stop(nos_thread_t *thread) {
    if (thread) { 
        thread->stop_requested = 1;
        uint64_t val = 1;
        if (write(thread->notify_fd, &val, sizeof(val)) < 0) { /* Ignore */ }
    }
}

nos_status_t nos_scheduler_run_loop(nos_thread_t *self) {
    if (!self) return NOS_ERR;
    t_local_scheduler = self;
    for (uint32_t i = 0; i < self->component_count; i++) {
        nos_component_t *comp = self->components[i];
        if (comp && comp->start && comp->status != NOS_COMP_ST_ACTIVE) {
            if (comp->start(comp) == NOS_OK) comp->status = NOS_COMP_ST_ACTIVE;
        }
    }
    nos_sys_log_info("Thread '%s' loop started.", self->name);
    struct epoll_event events[MAX_EVENTS];
    extern nos_node_ctx_t g_node_ctx;
    uint32_t poll_cycles = (g_node_ctx.node_def) ? g_node_ctx.node_def->busy_poll_cycles : 500;
    
    while (!self->stop_requested) {
        /* Busy spin to process messages without kernel intervention if possible */
        int spun = 0;
        for (uint32_t spin = 0; spin < poll_cycles; spin++) {
            if (self->head != self->tail) {
                spun = 1;
                break;
            }
            __asm__ volatile("pause" ::: "memory");
        }

        if (spun || self->head != self->tail) {
            process_thread_messages(self);
            process_timers(self);
            
            /* Fast path: poll epoll immediately without sleeping to handle eventfd and other FDs */
            int nfds = epoll_wait(self->epoll_fd, events, MAX_EVENTS, 0);
            for (int i = 0; i < nfds; i++) {
                nos_fd_entry_t *entry = (nos_fd_entry_t *)events[i].data.ptr;
                if (!entry) continue;
                if (entry->fd == self->notify_fd) {
                    uint64_t val; if (read(self->notify_fd, &val, sizeof(val)) < 0) {}
                } else {
                    if (entry->callback) entry->callback(entry->fd, entry->arg);
                }
            }
            continue;
        }

        /* Go to sleep */
        atomic_store_explicit(&self->is_sleeping, 1, memory_order_release);
        atomic_thread_fence(memory_order_seq_cst);
        
        /* Double check to prevent race condition (lost wakeup) */
        if (self->head != self->tail || self->stop_requested) {
            atomic_store_explicit(&self->is_sleeping, 0, memory_order_release);
            continue;
        }

        int timeout = -1;
        if (self->timer_heap.size > 0) {
            uint64_t now = get_monotonic_ms();
            timeout = (self->timer_heap.nodes[0]->expire_at_ms <= now) ? 0 : (int)(self->timer_heap.nodes[0]->expire_at_ms - now);
        }

        int nfds = epoll_wait(self->epoll_fd, events, MAX_EVENTS, timeout);
        atomic_store_explicit(&self->is_sleeping, 0, memory_order_release);
        
        process_timers(self);
        
        for (int i = 0; i < nfds; i++) {
            nos_fd_entry_t *entry = (nos_fd_entry_t *)events[i].data.ptr;
            if (!entry) continue;

            if (entry->fd == self->notify_fd) {
                uint64_t val;
                if (read(self->notify_fd, &val, sizeof(val)) > 0) {
                    process_thread_messages(self);
                }
            } else {
                if (entry->callback) entry->callback(entry->fd, entry->arg);
            }
        }
    }
    nos_sys_log_info("Thread '%s' loop terminated.", self->name);
    return NOS_OK;
}

nos_timer_t* nos_timer_create(nos_timer_cb_t cb, void *arg, nos_timer_free_arg_t free_arg) {
    nos_timer_t *timer = calloc(1, sizeof(nos_timer_t));
    if (timer) { timer->callback = cb; timer->arg = arg; timer->free_arg = free_arg; }
    return timer;
}

nos_status_t nos_timer_start(nos_component_t *self, nos_timer_t *timer, uint32_t interval_ms, int is_periodic) {
    nos_thread_t *thread = find_thread_of_component(self);
    if (!thread || !timer || timer->is_running || thread->timer_heap.size >= thread->timer_heap.capacity) return NOS_ERR;
    timer->interval_ms = interval_ms; timer->expire_at_ms = get_monotonic_ms() + interval_ms;
    timer->is_periodic = is_periodic; timer->is_running = 1; timer->owner_thread = thread;
    thread->timer_heap.nodes[thread->timer_heap.size] = timer;
    heapify_up(thread, thread->timer_heap.size++);
    
    uint64_t val = 1;
    if (write(thread->notify_fd, &val, sizeof(val)) < 0) { /* Ignore */ }
    return NOS_OK;
}

nos_status_t nos_timer_stop(nos_component_t *self, nos_timer_t *timer) {
    nos_thread_t *thread = timer->owner_thread; // 优先使用自持的线程上下文
    if (!thread) thread = find_thread_of_component(self);
    if (!thread || !timer || !timer->is_running) return NOS_ERR;
    for (uint32_t i = 0; i < thread->timer_heap.size; i++) {
        if (thread->timer_heap.nodes[i] == timer) {
            thread->timer_heap.nodes[i] = thread->timer_heap.nodes[--thread->timer_heap.size];
            heapify_down(thread, i); timer->is_running = 0; return NOS_OK;
        }
    }
    return NOS_ERR;
}

void nos_timer_delete(nos_timer_t *timer) {
    if (!timer) return;
    if (timer->is_running && timer->owner_thread) {
        nos_thread_t *t = timer->owner_thread;
        for (uint32_t i = 0; i < t->timer_heap.size; i++) {
            if (t->timer_heap.nodes[i] == timer) {
                t->timer_heap.nodes[i] = t->timer_heap.nodes[--t->timer_heap.size];
                heapify_down(t, i); break;
            }
        }
    }
    if (timer->free_arg) timer->free_arg(timer->arg);
    free(timer);
}
