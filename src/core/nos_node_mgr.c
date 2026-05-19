#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "nos_node_mgr.h"
#include "nos_node_priv.h"
#include "nos_service.h"
#include "nos_api.h"

/* 外部函数声明 (由 nos_ipc_p2p.c 提供) */
nos_status_t nos_ipc_init(nos_thread_t *thread, const char *uds_path);

/* 外部函数声明 (由 nos_scheduler.c 提供) */
nos_status_t nos_service_unregister_provider(uint32_t service_id);
nos_status_t nos_service_register_provider_bind(uint32_t service_id, nos_component_t *provider, nos_thread_t *thread);

static nos_component_t* node_load_component_internal(uint32_t id, const char *name, const char *lib_name, void **out_handle) {
    char lib_path[256];
    sprintf(lib_path, "./%s", lib_name);

    void *handle = dlopen(lib_path, RTLD_NOW);
    if (!handle) {
        nos_sys_log_error("Failed to load %s: %s", lib_path, dlerror());
        return NULL;
    }

    nos_comp_export_func_t export_func = (nos_comp_export_func_t)dlsym(handle, "nos_export_component");
    if (!export_func) {
        nos_sys_log_error("Symbol 'nos_export_component' not found in %s", lib_path);
        dlclose(handle);
        return NULL;
    }

    nos_component_t *comp = calloc(1, sizeof(nos_component_t));
    if (!comp) { dlclose(handle); return NULL; }
    comp->id = id;
    comp->name = strdup(name);

    if (export_func(comp) != NOS_OK) {
        nos_sys_log_error("Component %s export failed", name);
        free((void*)comp->name); free(comp); dlclose(handle);
        return NULL;
    }

    if (comp->init && comp->init(comp) != NOS_OK) {
        nos_sys_log_error("Component %s init failed", name);
        comp->status = NOS_COMP_ST_ERROR;
        free((void*)comp->name); free(comp); dlclose(handle);
        return NULL;
    }

    comp->status = NOS_COMP_ST_INITED;
    *out_handle = handle;
    
    extern void nos_log_set_comp_info(uint32_t comp_id, const char *name);
    nos_log_set_comp_info(id, name);

    nos_sys_log_info("Loaded %s (ID:%u) from %s", name, id, lib_name);
    return comp;
}

static nos_component_t* node_get_loaded_comp_by_id(uint32_t id) {
    for (uint32_t i = 0; i < g_node_ctx.loaded_count; i++) {
        if (g_node_ctx.loaded_info[i].comp->id == id) return g_node_ctx.loaded_info[i].comp;
    }
    return NULL;
}

void node_init_mgmt(const char *uds_path) {
    g_node_ctx.mgmt_thread = calloc(1, sizeof(nos_thread_t));
    nos_scheduler_init_thread(g_node_ctx.mgmt_thread, 999, "Mgmt-Master");
    nos_ipc_init(g_node_ctx.mgmt_thread, uds_path);
}

void node_init_workers(const nos_node_def_t *node_def) {
    g_node_ctx.worker_threads = calloc(MAX_WORKERS, sizeof(nos_thread_t));
    g_node_ctx.worker_count = 0;
    
    for (int i = 0; node_def->threads[i].name != NULL && i < MAX_WORKERS; i++) {
        const nos_thread_def_t *t_def = &node_def->threads[i];
        nos_thread_t *thread = &g_node_ctx.worker_threads[i];
        nos_scheduler_init_thread(thread, i, t_def->name);
        
        for (int j = 0; t_def->comp_ids[j] != 0 && j < MAX_COMPONENTS_PER_NODE; j++) {
            void *handle = NULL;
            nos_component_t *comp = node_load_component_internal(t_def->comp_ids[j], t_def->comp_names[j], t_def->comp_models[j], &handle);
            if (comp) {
                if (g_node_ctx.loaded_count < MAX_COMPONENTS_PER_NODE) {
                    loaded_comp_info_t *info = &g_node_ctx.loaded_info[g_node_ctx.loaded_count++];
                    info->comp = comp; info->handle = handle; info->lib_name = t_def->comp_models[j];
                    info->owner_thread = thread;
                }
                nos_scheduler_register_component(thread, comp);
            }
        }
        g_node_ctx.worker_count++;
    }
}

void node_setup_routing(const char *current_node_name) {
    const nos_node_def_t *node = g_node_ctx.node_def;
    
    for (uint32_t i = 0; i < node->service_count; i++) {
        const nos_service_def_t *svc = &node->services[i];
        if (strcmp(svc->node_name, current_node_name) == 0) {
            nos_component_t *provider = node_get_loaded_comp_by_id(svc->provider_comp_id);
            nos_thread_t *owner_worker = NULL;
            for (uint32_t t = 0; t < g_node_ctx.worker_count; t++) {
                nos_thread_t *curr_t = &g_node_ctx.worker_threads[t];
                for (uint32_t c = 0; c < curr_t->component_count; c++) {
                    if (curr_t->components[c] && curr_t->components[c]->id == svc->provider_comp_id) {
                        owner_worker = curr_t; break;
                    }
                }
                if (owner_worker) break;
            }
            if (provider && owner_worker) nos_service_register_provider_bind(svc->service_id, provider, owner_worker);
        } else {
            if (svc->remote_uds_path && strlen(svc->remote_uds_path) > 0) {
                nos_service_register_remote(svc->service_id, svc->remote_uds_path);
                nos_sys_log_info("Routing: Remote Service %u -> %s (%s)", svc->service_id, svc->node_name, svc->remote_uds_path);
            }
        }
    }
}

nos_status_t node_unload_component(const char *name) {
    int idx = -1;
    for (uint32_t i = 0; i < g_node_ctx.loaded_count; i++) {
        if (g_node_ctx.loaded_info[i].comp && strcmp(g_node_ctx.loaded_info[i].comp->name, name) == 0) {
            idx = (int)i; break;
        }
    }
    if (idx == -1) return NOS_ERR;

    loaded_comp_info_t *info = &g_node_ctx.loaded_info[idx];
    nos_scheduler_unregister_component(info->owner_thread, info->comp);
    
    for (uint32_t i = 0; i < g_node_ctx.node_def->service_count; i++) {
        if (g_node_ctx.node_def->services[i].provider_comp_id == info->comp->id) {
            nos_service_unregister_provider(g_node_ctx.node_def->services[i].service_id);
        }
    }

    if (info->comp->stop) info->comp->stop(info->comp);
    free((void*)info->comp->name); free(info->comp); dlclose(info->handle);

    for (uint32_t i = idx; i < g_node_ctx.loaded_count - 1; i++) g_node_ctx.loaded_info[i] = g_node_ctx.loaded_info[i+1];
    g_node_ctx.loaded_count--;
    nos_sys_log_info("Component %s unloaded.", name);
    return NOS_OK;
}

nos_status_t node_reload_component(const char *name) {
    int idx = -1;
    for (uint32_t i = 0; i < g_node_ctx.loaded_count; i++) {
        if (strcmp(g_node_ctx.loaded_info[i].comp->name, name) == 0) {
            idx = (int)i; break;
        }
    }
    if (idx == -1) return NOS_ERR;

    /* 关键修复：在 unload 之前必须深度拷贝元数据 */
    uint32_t id = g_node_ctx.loaded_info[idx].comp->id;
    char *lib_name = strdup(g_node_ctx.loaded_info[idx].lib_name);
    char *comp_name = strdup(name);
    nos_thread_t *thread = g_node_ctx.loaded_info[idx].owner_thread;

    node_unload_component(name);

    void *new_handle = NULL;
    nos_component_t *new_comp = node_load_component_internal(id, comp_name, lib_name, &new_handle);
    if (new_comp) {
        if (g_node_ctx.loaded_count < MAX_COMPONENTS_PER_NODE) {
            loaded_comp_info_t *info = &g_node_ctx.loaded_info[g_node_ctx.loaded_count++];
            info->comp = new_comp; info->handle = new_handle; info->lib_name = lib_name; info->owner_thread = thread;
            nos_scheduler_register_component(thread, new_comp);
            /* 注意：此处不手动调用 start，让 run_loop 在下一次迭代自动触发 (确保在正确线程 context) */
            for (uint32_t i = 0; i < g_node_ctx.node_def->service_count; i++) {
                if (g_node_ctx.node_def->services[i].provider_comp_id == id &&
                    strcmp(g_node_ctx.node_def->services[i].node_name, g_node_ctx.node_def->name) == 0) {
                    nos_service_register_provider_bind(g_node_ctx.node_def->services[i].service_id, new_comp, thread);
                }
            }
        }
    }
    
    /* 这里的 lib_name 所有权已经转移给 info 或加载失败需释放 */
    int found = 0;
    for(uint32_t i=0; i<g_node_ctx.loaded_count; i++) if(g_node_ctx.loaded_info[i].lib_name == lib_name) found=1;
    if(!found) free(lib_name);
    free(comp_name);
    
    return (new_comp != NULL) ? NOS_OK : NOS_ERR;
}
