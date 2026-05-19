#ifndef __NOS_LOG_API_H__
#define __NOS_LOG_API_H__

#include <stdatomic.h>
#include "nos_log.h"

/**
 * @brief 内部接口：获取日志操作句柄
 */
static inline nos_log_ops_t* _nos_get_log_ops(void) {
    extern void* nos_embedded_service_get(const char *name);
    static _Atomic(nos_log_ops_t*) cache = NULL;
    
    nos_log_ops_t* ops = atomic_load_explicit(&cache, memory_order_relaxed);
    if (!ops) {
        ops = (nos_log_ops_t *)nos_embedded_service_get("SVC_LOG");
        if (ops) {
            atomic_store_explicit(&cache, ops, memory_order_relaxed);
        }
    }
    return ops;
}

/**
 * @brief 核心日志宏 (基于 ID)
 */
#define nos_log_raw(level, id, fmt, ...) \
    do { nos_log_ops_t *ops = _nos_get_log_ops(); if(ops) ops->log(level, id, fmt, ##__VA_ARGS__); } while(0)

/**
 * @brief 系统级日志 API (ID 0)
 * 适用于内核、调度器、内存管理等基础设施
 */
#define nos_sys_log_debug(fmt, ...) nos_log_raw(NOS_LOG_LEVEL_DEBUG, 0, fmt, ##__VA_ARGS__)
#define nos_sys_log_info(fmt, ...)  nos_log_raw(NOS_LOG_LEVEL_INFO,  0, fmt, ##__VA_ARGS__)
#define nos_sys_log_warn(fmt, ...)  nos_log_raw(NOS_LOG_LEVEL_WARN,  0, fmt, ##__VA_ARGS__)
#define nos_sys_log_error(fmt, ...) nos_log_raw(NOS_LOG_LEVEL_ERROR, 0, fmt, ##__VA_ARGS__)

/**
 * @brief 组件便捷日志 API
 * 自动从组件 self 上下文中提取 ID
 */
#define nos_log_debug(self, fmt, ...) nos_log_raw(NOS_LOG_LEVEL_DEBUG, (self)->id, fmt, ##__VA_ARGS__)
#define nos_log_info(self, fmt, ...)  nos_log_raw(NOS_LOG_LEVEL_INFO,  (self)->id, fmt, ##__VA_ARGS__)
#define nos_log_warn(self, fmt, ...)  nos_log_raw(NOS_LOG_LEVEL_WARN,  (self)->id, fmt, ##__VA_ARGS__)
#define nos_log_error(self, fmt, ...) nos_log_raw(NOS_LOG_LEVEL_ERROR, (self)->id, fmt, ##__VA_ARGS__)

#endif /* __NOS_LOG_API_H__ */
