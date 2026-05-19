#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nos_service.h"
#include "nos_node_mgr.h"
#include "nos_node_priv.h"
#include "nos_api.h"

#define MAX_EMBEDDED_SERVICES 32

/* 嵌入式服务注册表 */
static nos_embedded_service_t g_embedded_registry[MAX_EMBEDDED_SERVICES];
static uint32_t g_embedded_count = 0;

nos_status_t nos_embedded_service_register(const char *name, void *ops) {
    if (g_embedded_count >= MAX_EMBEDDED_SERVICES) return NOS_ERR;
    g_embedded_registry[g_embedded_count].service_name = name;
    g_embedded_registry[g_embedded_count].ops = ops;
    g_embedded_count++;
    nos_sys_log_info("Registered embedded service: %s", name);
    return NOS_OK;
}

void* nos_embedded_service_get(const char *name) {
    for (uint32_t i = 0; i < g_embedded_count; i++) {
        if (strcmp(g_embedded_registry[i].service_name, name) == 0) {
            return g_embedded_registry[i].ops;
        }
    }
    return NULL;
}

/* 后续可将 nos_node_mgr.c 中的路由/加载逻辑逐步迁移至此或保持协同 */
