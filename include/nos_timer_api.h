#ifndef __NOS_TIMER_API_H__
#define __NOS_TIMER_API_H__

#include "nos_types.h"
#include "nos_component.h"

/**
 * @brief 定时器回调函数定义
 */
typedef void (*nos_timer_cb_t)(void *arg);
typedef void (*nos_timer_free_arg_t)(void *arg);

/**
 * @brief 定时器对象句柄 (不透明指针)
 */
typedef struct nos_timer_s nos_timer_t;

/**
 * @brief 定时器操作接口 (对象化生命周期)
 */
typedef struct {
    nos_timer_t* (*create)(nos_timer_cb_t cb, void *arg, nos_timer_free_arg_t free_arg);
    nos_status_t (*start)(nos_component_t *self, nos_timer_t *timer, uint32_t interval_ms, int is_periodic);
    nos_status_t (*stop)(nos_component_t *self, nos_timer_t *timer);
    void         (*delete)(nos_timer_t *timer);
} nos_timer_ops_t;

/* 实现函数声明 (由 nos_scheduler.c 提供) */
nos_timer_t* nos_timer_create(nos_timer_cb_t cb, void *arg, nos_timer_free_arg_t free_arg);
nos_status_t nos_timer_start(nos_component_t *self, nos_timer_t *timer, uint32_t interval_ms, int is_periodic);
nos_status_t nos_timer_stop(nos_component_t *self, nos_timer_t *timer);
void         nos_timer_delete(nos_timer_t *timer);

/**
 * @brief 内部接口：初始化定时器引擎并注册服务
 */
void nos_timer_init(void);

#endif /* __NOS_TIMER_API_H__ */
