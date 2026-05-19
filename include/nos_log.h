#ifndef __NOS_LOG_H__
#define __NOS_LOG_H__

#include "nos_types.h"

/**
 * @brief 日志级别定义
 */
typedef enum {
    NOS_LOG_LEVEL_DEBUG,
    NOS_LOG_LEVEL_INFO,
    NOS_LOG_LEVEL_WARN,
    NOS_LOG_LEVEL_ERROR
} nos_log_level_t;

/**
 * @brief 嵌入式日志服务操作接口 (API Table)
 */
typedef struct {
    /**
     * @brief 打印带级别的日志
     * @param level 级别
     * @param comp_id 调用者组件ID
     * @param fmt 格式化字符串
     */
    void (*log)(nos_log_level_t level, uint32_t comp_id, const char *fmt, ...);
    
    /**
     * @brief 设置全局过滤级别
     */
    void (*set_filter_level)(nos_log_level_t level);

    /**
     * @brief 设置特定组件的过滤级别
     */
    void (*set_comp_level)(uint32_t comp_id, nos_log_level_t level);
} nos_log_ops_t;

/**
 * @brief 内部接口：设置组件ID与名称的映射 (供框架使用)
 */
void nos_log_set_comp_info(uint32_t comp_id, const char *name);

/**
 * @brief 内部接口：初始化日志引擎
 */
void nos_log_init(void);

/**
 * @brief 内部接口：清理并关闭日志引擎
 */
void nos_log_deinit(void);

/**
 * @brief 内部接口：获取日志系统内存占用 (Byte)
 */
size_t nos_log_get_mem_usage(void);

#endif /* __NOS_LOG_H__ */
