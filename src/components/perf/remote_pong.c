#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_ids.h"
#include "nos_api.h"

static void comp_on_msg(nos_component_t *self, nos_buffer_t *req_buf) {
    const nos_service_msg_t *msg = (const nos_service_msg_t *)req_buf->data;
    if (msg->msg_code == 2001) { // PING
        // nos_log_info(self, "Received PING, sending PONG");
        nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t), 0);
        if (buf) {
            nos_service_msg_t *pong = (nos_service_msg_t *)buf->data;
            pong->magic = NOS_IPC_MAGIC;
            pong->version = NOS_IPC_VERSION;
            pong->dst_service = 0;
            pong->src_component = self->id;
            pong->msg_code = 2002; // PONG
            pong->seq = msg->seq;
            pong->flags = 0;
            pong->payload_len = 0;
            nos_service_reply(nos_service_get_reply_ctx(req_buf), buf);
            nos_buffer_release(buf);
        }
    }
}

nos_status_t nos_export_component(nos_component_t *comp) {
    comp->on_msg = comp_on_msg;
    return NOS_OK;
}
