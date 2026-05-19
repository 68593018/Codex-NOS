#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>
#include "nos_node_priv.h"
#include "nos_node_mgr.h"
#include "nos_cli.h"
#include "nos_buffer.h"
#include "nos_log.h"
#include "nos_api.h"

/* 定义并初始化全局上下文 */
nos_node_ctx_t g_node_ctx = {
    .keep_running = 1,
    .loaded_count = 0,
    .worker_count = 0
};

static void signal_handler(int sig) {
    nos_sys_log_info("Received signal %d, initiating graceful shutdown...", sig);
    g_node_ctx.keep_running = 0;
}

static void* scheduler_thread_entry(void *arg) {
    nos_scheduler_run_loop((nos_thread_t *)arg);
    return NULL;
}

int main(int argc, char *argv[]) {
    const nos_node_def_t *node_def = nos_manifest_get_local();
    if (!node_def) { nos_sys_log_error("Local node manifest not found."); return -1; }

    g_node_ctx.node_def = node_def;

    /* 按需初始化平台基础设施 (由清单驱动) */
    if (node_def->platform_inits) {
        for (int i = 0; node_def->platform_inits[i] != NULL; i++) {
            node_def->platform_inits[i]();
        }
    }

    nos_sys_log_info("--- [NOS Node: %s] Starting ---", node_def->name);
    
    nos_buffer_init_pool(node_def->buffer_pools);

    /* 1. 初始化管理面与数据面线程 */
    node_init_mgmt(node_def->uds_path);
    node_init_workers(node_def);
    node_setup_routing(node_def->name);

    /* 3. 注册信号量 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 4. 物理线程启动 */
    pthread_create(&g_node_ctx.mgmt_tid, NULL, scheduler_thread_entry, g_node_ctx.mgmt_thread);
    for (uint32_t i = 0; i < g_node_ctx.worker_count; i++) {
        pthread_create(&g_node_ctx.worker_tids[i], NULL, scheduler_thread_entry, &g_node_ctx.worker_threads[i]);
    }

    /* 5. 启动 CLI 交互界面 */
    node_cli_start(node_def->name);

    /* 6. 等待退出 */
    while (g_node_ctx.keep_running) sleep(1);

    /* 7. 优雅关闭 */
    nos_sys_log_info("Node shutting down...");
    nos_scheduler_stop(g_node_ctx.mgmt_thread);
    for (uint32_t i = 0; i < g_node_ctx.worker_count; i++) nos_scheduler_stop(&g_node_ctx.worker_threads[i]);

    pthread_join(g_node_ctx.mgmt_tid, NULL);
    for (uint32_t i = 0; i < g_node_ctx.worker_count; i++) pthread_join(g_node_ctx.worker_tids[i], NULL);
    pthread_join(g_node_ctx.cli_tid, NULL);

    /* 8. 资源清理 */
    for (int i = (int)g_node_ctx.loaded_count - 1; i >= 0; i--) {
        if (g_node_ctx.loaded_info[i].comp->stop) g_node_ctx.loaded_info[i].comp->stop(g_node_ctx.loaded_info[i].comp);
        free((void*)g_node_ctx.loaded_info[i].comp->name);
        free(g_node_ctx.loaded_info[i].comp);
        dlclose(g_node_ctx.loaded_info[i].handle);
    }

    /* 9. 释放调度器内部资源 */
    nos_scheduler_deinit_thread(g_node_ctx.mgmt_thread);
    for (uint32_t i = 0; i < g_node_ctx.worker_count; i++) {
        nos_scheduler_deinit_thread(&g_node_ctx.worker_threads[i]);
    }

    /* 10. 释放 Buffer 池 */
    nos_buffer_deinit_pool();

    /* 11. 释放 KV 数据库 */
    nos_kv_db_deinit();

    nos_sys_log_info("Shutdown complete.");

    /* 12. 释放日志系统 */
    nos_log_deinit();

    free(g_node_ctx.mgmt_thread);
    free(g_node_ctx.worker_threads);

    return 0;
}
