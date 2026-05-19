#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_ids.h"
#include "nos_api.h"

typedef struct {
    uint64_t start_time_ns;
    uint32_t target_count;
    uint32_t current_count;
    uint32_t payload_size;
    uint32_t sweep_count;
    uint32_t sweep_index;
    uint32_t sweep_sizes[8];
} rping_ctx_t;

static uint64_t get_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void send_ping(nos_component_t *self, rping_ctx_t *ctx) {
    uint32_t payload_size = ctx ? ctx->payload_size : 0;
    nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t) + payload_size, 0);
    if (buf) {
        nos_service_msg_t *msg = (nos_service_msg_t *)buf->data;
        msg->magic = NOS_IPC_MAGIC;
        msg->version = NOS_IPC_VERSION;
        msg->dst_service = 114; // SVC_REMOTE_PONG
        msg->src_component = self->id;
        msg->msg_code = 2001; // PING
        msg->seq = ctx ? ctx->current_count + 1 : 0;
        msg->flags = NOS_MSG_F_NEED_REPLY;
        msg->payload_len = payload_size;
        if (payload_size > 0) {
            memset(msg + 1, 0xA5, payload_size);
        }
        nos_service_msg_send(buf);
        nos_buffer_release(buf);
    }
}

static void start_one_test(nos_component_t *self, rping_ctx_t *ctx, uint32_t payload_size) {
    ctx->payload_size = payload_size;
    ctx->current_count = 0;
    nos_log_info(self, "Cross-Process Perf Test Started: %u iterations, payload %u bytes",
                 ctx->target_count, ctx->payload_size);
    ctx->start_time_ns = get_now_ns();
    send_ping(self, ctx);
}

static void comp_on_msg(nos_component_t *self, nos_buffer_t *buf) {
    const nos_service_msg_t *msg = (const nos_service_msg_t *)buf->data;
    rping_ctx_t *ctx = (rping_ctx_t *)self->priv;

    if (msg->msg_code == 3001) { // START_TEST from CLI
        const uint32_t *payload = (const uint32_t *)(msg + 1);
        ctx->target_count = (msg->payload_len >= 4) ? payload[0] : 100000;
        uint32_t requested_payload = (msg->payload_len >= 8) ? payload[1] : 0;
        ctx->sweep_count = 0;
        ctx->sweep_index = 0;
        if (requested_payload == UINT32_MAX) {
            uint32_t defaults[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
            ctx->sweep_count = (uint32_t)(sizeof(defaults) / sizeof(defaults[0]));
            memcpy(ctx->sweep_sizes, defaults, sizeof(defaults));
            nos_log_info(self, "Cross-Process Perf Sweep Started: %u iterations per payload", ctx->target_count);
            start_one_test(self, ctx, ctx->sweep_sizes[ctx->sweep_index]);
        } else {
            start_one_test(self, ctx, requested_payload);
        }
        return;
    }

    if (msg->msg_code == 2002) { // PONG
        ctx->current_count++;
        if (ctx->current_count % 1000 == 0) {
            nos_log_info(self, "Progress: %u/%u", ctx->current_count, ctx->target_count);
        }
        if (ctx->current_count < ctx->target_count) {
            send_ping(self, ctx);
        } else {
            uint64_t end_time = get_now_ns();
            double total_sec = (double)(end_time - ctx->start_time_ns) / 1000000000.0;
            double payload_mbps = total_sec > 0.0 ?
                ((double)ctx->target_count * (double)ctx->payload_size * 8.0) / total_sec / 1000000.0 : 0.0;
            nos_log_info(self, "Cross-Process Perf Test Complete!");
            nos_log_info(self, "  Total Time:  %.3f sec", total_sec);
            nos_log_info(self, "  Packets/sec: %.0f pps", (double)ctx->target_count / total_sec);
            nos_log_info(self, "  Throughput:  %.2f Mbps", payload_mbps);
            nos_log_info(self, "  Avg Latency: %.2f us", (total_sec * 1000000.0) / ctx->target_count);
            if (ctx->sweep_count > 0 && ++ctx->sweep_index < ctx->sweep_count) {
                start_one_test(self, ctx, ctx->sweep_sizes[ctx->sweep_index]);
            } else {
                ctx->sweep_count = 0;
                ctx->sweep_index = 0;
            }
        }
    }
}

static nos_status_t comp_init(nos_component_t *self) {
    self->priv = calloc(1, sizeof(rping_ctx_t));
    return NOS_OK;
}

static void comp_stop(nos_component_t *self) {
    if (self->priv) free(self->priv);
}

nos_status_t nos_export_component(nos_component_t *comp) {
    comp->on_msg = comp_on_msg;
    comp->init = comp_init;
    comp->stop = comp_stop;
    return NOS_OK;
}
