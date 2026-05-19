#ifndef __NOS_API_H__
#define __NOS_API_H__

#include <stdatomic.h>
#include <stdlib.h>
#include "nos_types.h"
#include "nos_service.h"
#include "nos_log_api.h"
#include "nos_kv_api.h"
#include "nos_timer_api.h"

/**
 * @brief 内部接口：获取 Timer 操作句柄
 */
static inline nos_timer_ops_t* _nos_get_timer_ops(void) {
    extern void* nos_embedded_service_get(const char *name);
    static _Atomic(nos_timer_ops_t*) cache = NULL;
    nos_timer_ops_t* ops = atomic_load_explicit(&cache, memory_order_relaxed);
    if (!ops) {
        ops = (nos_timer_ops_t *)nos_embedded_service_get("SVC_TIMER");
        if (ops) atomic_store_explicit(&cache, ops, memory_order_relaxed);
    }
    return ops;
}

/**
 * @brief 纯对象化 Timer API
 */
#define nos_timer_create_auto(cb, arg, free_cb) \
    ({ nos_timer_ops_t *ops = _nos_get_timer_ops(); ops ? ops->create(cb, arg, free_cb) : NULL; })

#define nos_timer_start_auto(self, timer, interval, periodic) \
    ({ nos_timer_ops_t *ops = _nos_get_timer_ops(); ops ? ops->start(self, timer, interval, periodic) : NOS_ERR; })

#define nos_timer_stop_auto(self, timer) \
    ({ nos_timer_ops_t *ops = _nos_get_timer_ops(); ops ? ops->stop(self, timer) : NOS_ERR; })

#define nos_timer_delete_auto(timer) \
    do { nos_timer_ops_t *ops = _nos_get_timer_ops(); if(ops) ops->delete(timer); } while(0)

/**
 * @brief 内部接口：获取 KV 操作句柄
 */
static inline nos_kv_ops_t* _nos_get_kv_ops(void) {
    extern void* nos_embedded_service_get(const char *name);
    static _Atomic(nos_kv_ops_t*) cache = NULL;
    nos_kv_ops_t* ops = atomic_load_explicit(&cache, memory_order_relaxed);
    if (!ops) {
        ops = (nos_kv_ops_t *)nos_embedded_service_get("SVC_KV_DB");
        if (ops) atomic_store_explicit(&cache, ops, memory_order_relaxed);
    }
    return ops;
}

#define nos_kv_table_create_auto(name, ks, vs, cap) \
    ({ nos_kv_ops_t *ops = _nos_get_kv_ops(); ops ? ops->table_create(name, ks, vs, cap) : NULL; })

#define nos_kv_put_auto(t, k, v, len) \
    ({ nos_kv_ops_t *ops = _nos_get_kv_ops(); ops ? ops->put(t, k, v, len) : NOS_ERR; })

#define nos_kv_get_auto(t, k, v, len) \
    ({ nos_kv_ops_t *ops = _nos_get_kv_ops(); ops ? ops->get(t, k, v, len) : NOS_ERR; })

#define nos_kv_subscribe_auto(t, k, cb, arg) \
    ({ nos_kv_ops_t *ops = _nos_get_kv_ops(); ops ? ops->subscribe(t, k, cb, arg) : NOS_ERR; })

#define nos_kv_unsubscribe_auto(t, k, cb, arg) \
    ({ nos_kv_ops_t *ops = _nos_get_kv_ops(); ops ? ops->unsubscribe(t, k, cb, arg) : NOS_ERR; })

#endif /* __NOS_API_H__ */
