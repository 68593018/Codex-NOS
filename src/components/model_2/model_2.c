#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_ids.h"
#include "nos_api.h"

typedef struct {
    nos_kv_table_t *state_table;
    nos_kv_table_t *test_table;
    int local_counter;
} comp_ctx_t;

static void comp_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    comp_ctx_t *ctx = (comp_ctx_t *)self->priv;
    if (!ctx) return;
    
    /* 原有业务逻辑 */
    ctx->local_counter++;
    
    /* 1. 同步组件运行状态 (Key 是字符串名称，需填充对齐) */
    if (ctx->state_table) {
        char key[32] = {0};
        strncpy(key, self->name, 31);
        nos_kv_put_auto(ctx->state_table, key, &ctx->local_counter, sizeof(int));
    }

    /* 2. 写入测试数据 (Key 和 Value 都是当前的 counter) */
    if (ctx->test_table) {
        nos_kv_put_auto(ctx->test_table, &ctx->local_counter, &ctx->local_counter, sizeof(int));
    }
    
    nos_log_info(self, "Msg received, counter: %d. Data synced to KV DB.", ctx->local_counter);
}

static nos_status_t comp_init(nos_component_t *self) {
    comp_ctx_t *ctx = calloc(1, sizeof(comp_ctx_t));
    if (!ctx) return NOS_ERR;
    self->priv = ctx;

    /* 初始化状态表 (KeySize=32 支持字符串名称) */
    ctx->state_table = nos_kv_table_create_auto("CompStates", 32, sizeof(int), 128);
    
    /* 初始化测试数据表 (KeySize=4 适配 int 类型 Key) */
    ctx->test_table = nos_kv_table_create_auto("TestData", sizeof(int), sizeof(int), 1024);

    /* 恢复状态 */
    if (ctx->state_table) {
        char key[32] = {0};
        strncpy(key, self->name, 31);
        uint32_t len = sizeof(int);
        if (nos_kv_get_auto(ctx->state_table, key, &ctx->local_counter, &len) == NOS_OK) {
            nos_log_info(self, "State recovered from KV DB: counter = %d", ctx->local_counter);
        } else {
            /* 初始写入，便于其他组件订阅 */
            int initial_val = 0;
            nos_kv_put_auto(ctx->state_table, key, &initial_val, sizeof(int));
        }
    }
    nos_log_debug(self, "Component instance with multiple KV tables initialized");
    return NOS_OK;
}

static nos_status_t comp_start(nos_component_t *self) { return NOS_OK; }

static void comp_stop(nos_component_t *self) {
    if (self->priv) { free(self->priv); self->priv = NULL; }
}

nos_status_t nos_export_component(nos_component_t *comp) {
    if (!comp) return NOS_ERR;
    comp->on_msg = comp_on_msg;
    comp->init = comp_init;
    comp->start = comp_start;
    comp->stop = comp_stop;
    return NOS_OK;
}
