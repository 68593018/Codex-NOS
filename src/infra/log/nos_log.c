#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "nos_log.h"

#define LOG_BUFFER_SIZE 1024
#define LOG_MSG_MAX 256
#define MAX_COMP_ID 256

typedef struct {
    nos_log_level_t level;
    uint32_t comp_id;
    char time_str[16];
    char message[LOG_MSG_MAX];
    atomic_int ready;
} nos_log_entry_t;

typedef struct {
    char name[32];
    nos_log_level_t filter_level;
    int is_registered;
} nos_comp_info_t;

/**
 * @brief 日志系统内部上下文
 */
typedef struct {
    nos_log_entry_t ring[LOG_BUFFER_SIZE];
    _Atomic unsigned int write_idx;
    uint32_t read_idx;

    nos_log_level_t global_filter_level;
    nos_comp_info_t comp_info[MAX_COMP_ID];

    pthread_t consumer_tid;
    _Atomic unsigned long dropped_count;
    _Atomic int keep_running;
    int initialized;
} nos_log_context_t;

static nos_log_context_t g_log_ctx = {
    .write_idx = 0,
    .read_idx = 0,
    .global_filter_level = NOS_LOG_LEVEL_DEBUG,
    .dropped_count = 0,
    .keep_running = 1,
    .initialized = 0
};

static const char* level_to_str(nos_log_level_t level) {
    switch(level) {
        case NOS_LOG_LEVEL_DEBUG: return "DEBUG";
        case NOS_LOG_LEVEL_INFO:  return "INFO ";
        case NOS_LOG_LEVEL_WARN:  return "WARN ";
        case NOS_LOG_LEVEL_ERROR: return "ERROR";
        default: return "UNKN ";
    }
}

static void* nos_log_consumer_thread(void *arg) {
    nos_log_context_t *ctx = &g_log_ctx;
    while (atomic_load(&ctx->keep_running) || atomic_load(&ctx->ring[ctx->read_idx % LOG_BUFFER_SIZE].ready) == 2) {
        nos_log_entry_t *entry = &ctx->ring[ctx->read_idx % LOG_BUFFER_SIZE];
        if (atomic_load(&entry->ready) == 2) { // Ready to read
            const char *name = "System";
            if (entry->comp_id > 0 && entry->comp_id < MAX_COMP_ID) {
                if (ctx->comp_info[entry->comp_id].is_registered) {
                    name = ctx->comp_info[entry->comp_id].name;
                }
            }
            printf("[%s] [%s] [%-8s] %s\n", 
                   entry->time_str, level_to_str(entry->level), name, entry->message);
            fflush(stdout);
            atomic_store(&entry->ready, 0); // Back to empty
            ctx->read_idx++;
        } else {
            usleep(1000); // 1ms sleep to yield CPU
        }
    }
    return NULL;
}

static void nos_log_impl(nos_log_level_t level, uint32_t comp_id, const char *fmt, ...) {
    nos_log_context_t *ctx = &g_log_ctx;
    nos_log_level_t filter_level = ctx->global_filter_level;

    if (comp_id > 0 && comp_id < MAX_COMP_ID) {
        if (ctx->comp_info[comp_id].is_registered) {
            filter_level = ctx->comp_info[comp_id].filter_level;
        }
    }

    if (level < filter_level) return;

    unsigned int idx = atomic_fetch_add(&ctx->write_idx, 1) % LOG_BUFFER_SIZE;
    nos_log_entry_t *entry = &ctx->ring[idx];

    /* 
     * Industrial Grade: Triple-state CAS (Empty -> Writing -> Ready) 
     * to prevent race conditions during rapid wrap-around.
     */
    int expected = 0; // Empty
    if (!atomic_compare_exchange_strong(&entry->ready, &expected, 1)) {
        // Slot is either already being written (1) or full (2)
        atomic_fetch_add(&ctx->dropped_count, 1);
        return;
    }

    entry->level = level;
    entry->comp_id = comp_id;
    
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(entry->time_str, sizeof(entry->time_str), "%H:%M:%S", t);

    va_list args;
    va_start(args, fmt);
    vsnprintf(entry->message, sizeof(entry->message), fmt, args);
    va_end(args);

    atomic_store(&entry->ready, 2); // Mark as Ready
}

void nos_log_deinit(void) {
    nos_log_context_t *ctx = &g_log_ctx;
    if (!ctx->initialized) return;

    atomic_store(&ctx->keep_running, 0);
    
    /* Wait for consumer thread to finish and join */
    pthread_join(ctx->consumer_tid, NULL);
    
    ctx->initialized = 0;
}

static void nos_set_level_impl(nos_log_level_t level) {
    g_log_ctx.global_filter_level = level;
    nos_log_impl(NOS_LOG_LEVEL_INFO, 0, "Global filter level set to %s", level_to_str(level));
}

static void nos_set_comp_level_impl(uint32_t comp_id, nos_log_level_t level) {
    if (comp_id == 0 || comp_id >= MAX_COMP_ID) return;
    nos_log_context_t *ctx = &g_log_ctx;

    ctx->comp_info[comp_id].filter_level = level;
    
    nos_log_impl(NOS_LOG_LEVEL_INFO, 0, "Component ID [%u] filter level set to %s", comp_id, level_to_str(level));
}

void nos_log_get_stats(unsigned long *total_dropped) {
    if (total_dropped) *total_dropped = atomic_load(&g_log_ctx.dropped_count);
}


void nos_log_set_comp_info(uint32_t comp_id, const char *name) {
    if (comp_id == 0 || comp_id >= MAX_COMP_ID || !name) return;
    nos_log_context_t *ctx = &g_log_ctx;

    strncpy(ctx->comp_info[comp_id].name, name, sizeof(ctx->comp_info[comp_id].name) - 1);
    ctx->comp_info[comp_id].filter_level = ctx->global_filter_level;
    ctx->comp_info[comp_id].is_registered = 1;
}

static nos_log_ops_t g_log_ops = {
    .log = nos_log_impl,
    .set_filter_level = nos_set_level_impl,
    .set_comp_level = nos_set_comp_level_impl
};

void nos_log_init(void) {
    nos_log_context_t *ctx = &g_log_ctx;
    if (ctx->initialized) return;

    for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
        atomic_init(&ctx->ring[i].ready, 0);
    }

    atomic_store(&ctx->keep_running, 1);
    pthread_create(&ctx->consumer_tid, NULL, nos_log_consumer_thread, NULL);

    extern nos_status_t nos_embedded_service_register(const char *name, void *ops);
    nos_embedded_service_register("SVC_LOG", &g_log_ops);
    
    ctx->initialized = 1;
}

size_t nos_log_get_mem_usage(void) {
    return sizeof(nos_log_context_t);
}