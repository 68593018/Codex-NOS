#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_ids.h"
#include "nos_api.h"

static void comp_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    /* 触发压力日志测试 */
    if (msg->msg_code == 3002) {
        uint32_t log_count = (msg->payload_len >= 4) ? *(uint32_t*)(msg + 1) : 2000;
        nos_log_info(self, "Starting Spam Logging: %u messages", log_count);
        for (uint32_t i = 0; i < log_count; i++) {
            nos_log_debug(self, "Spam message #%u for stress testing", i);
        }
        nos_log_info(self, "Spam Logging Complete");
        return;
    }
}

static nos_status_t comp_init(nos_component_t *self) {
    nos_log_debug(self, "Model 3 (Infrastructure Helper) initialized");
    return NOS_OK;
}

nos_status_t nos_export_component(nos_component_t *comp) {
    if (!comp) return NOS_ERR;
    comp->on_msg = comp_on_msg;
    comp->init = comp_init;
    return NOS_OK;
}
