#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_ids.h"
#include "nos_api.h"

typedef struct {
    nos_timer_t *heartbeat_timer;
    nos_kv_table_t *comp2_state_table;
} comp_ctx_t;

static void on_comp2_state_change(nos_kv_table_t *table, const void *key, const void *val, uint32_t val_len, void *arg) {
    nos_component_t *self = (nos_component_t *)arg;
    if (val_len == sizeof(int)) {
        int counter = *(int *)val;
        nos_log_info(self, "DETECTED: Comp-2 state changed to %d via KV Sub/Notify!", counter);
    }
}

/* 真正的定时器到期回调 */
static void heartbeat_callback(void *arg) {
    nos_component_t *self = (nos_component_t *)arg;
    
    nos_log_debug(self, "Timer callback: sending heartbeat to Comp-2");
    nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t), 0);
    if (buf) {
        nos_service_msg_t *hb = (nos_service_msg_t *)buf->data;
        hb->magic = NOS_IPC_MAGIC;
        hb->dst_service = SVC_ROUTING_V4; // 发送给 Comp-2
        hb->src_component = self->id;
        hb->msg_code = 1001;
        hb->payload_len = 0;
        nos_service_msg_send(buf);
        nos_buffer_release(buf);
    }
}

static void comp_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    nos_log_info(self, "Received msg from Comp %u, Code %u", msg->src_component, msg->msg_code);

    if (msg->msg_code == 1001) {
        nos_log_info(self, "Replying to MGMT Service");
        nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t) + 32, 0);
        if (buf) {
            nos_service_msg_t *rsp = (nos_service_msg_t *)buf->data;
            rsp->magic = NOS_IPC_MAGIC; rsp->dst_service = SVC_MGMT; 
            rsp->src_component = self->id; rsp->msg_code = 1002;
            nos_service_msg_send(buf);
            nos_buffer_release(buf);
        }
    }
}

static nos_status_t comp_init(nos_component_t *self) {
    self->priv = calloc(1, sizeof(comp_ctx_t));
    if (!self->priv) return NOS_ERR;
    nos_log_debug(self, "Initialized via Auto-Injection API");
    return NOS_OK;
}

static nos_status_t comp_start(nos_component_t *self) {
    comp_ctx_t *ctx = (comp_ctx_t *)self->priv;
    /* 1. 创建定时器对象，绑定到纯回调函数 */
    ctx->heartbeat_timer = nos_timer_create_auto(heartbeat_callback, self, NULL);
    
    /* 2. 启动定时器 */
    if (ctx->heartbeat_timer) {
        nos_timer_start_auto(self, ctx->heartbeat_timer, 10000, 1);
        nos_log_info(self, "Heartbeat timer object created and started (10s)");
    }

    /* 3. 订阅 Comp-2 的状态 (KV Pub/Sub 验证) */
    ctx->comp2_state_table = nos_kv_table_create_auto("CompStates", 32, sizeof(int), 128);
    if (ctx->comp2_state_table) {
        char key[32] = "Comp-2"; // 对应 Model-2 的名称
        if (nos_kv_subscribe_auto(ctx->comp2_state_table, key, on_comp2_state_change, self) == NOS_OK) {
            nos_log_info(self, "Subscribed to Comp-2 state changes in KV DB");
        } else {
            nos_log_warn(self, "Failed to subscribe to Comp-2 state (might not exist yet)");
        }
    }
    return NOS_OK;
}

static void comp_stop(nos_component_t *self) {
    comp_ctx_t *ctx = (comp_ctx_t *)self->priv;
    if (ctx) {
        if (ctx->heartbeat_timer) {
            nos_timer_delete_auto(ctx->heartbeat_timer);
            ctx->heartbeat_timer = NULL;
        }
        if (ctx->comp2_state_table) {
            char key[32] = "Comp-2";
            nos_kv_unsubscribe_auto(ctx->comp2_state_table, key, on_comp2_state_change, self);
            ctx->comp2_state_table = NULL;
        }
    }
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
