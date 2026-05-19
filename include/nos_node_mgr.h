#ifndef __NOS_NODE_MGR_H__
#define __NOS_NODE_MGR_H__

#include "nos_manifest.h"
#include "nos_types.h"

/**
 * @brief 初始化管理线程
 */
void node_init_mgmt(const char *uds_path);

/**
 * @brief 根据清单初始化工作线程与组件
 */
void node_init_workers(const nos_node_def_t *node_def);

/**
 * @brief 设置服务路由
 */
void node_setup_routing(const char *current_node_name);

/**
 * @brief 运行时加载/重载组件
 */
nos_status_t node_reload_component(const char *name);

/**
 * @brief 运行时卸载组件
 */
nos_status_t node_unload_component(const char *name);

#endif /* __NOS_NODE_MGR_H__ */
