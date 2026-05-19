#ifndef __NOS_KV_API_H__
#define __NOS_KV_API_H__

#include "nos_types.h"

/**
 * @brief KV 表句柄 (不透明指针)
 */
typedef struct nos_kv_table nos_kv_table_t;

/**
 * @brief 创建/打开一个定长 Key 的数据库表
 * @param name 表名
 * @param key_size 固定的 Key 长度 (Byte)
 * @param max_val_size 允许的 Value 最大长度
 * @param capacity 总容量 (最大条目数)
 * @return nos_kv_table_t* 表句柄，失败返回 NULL
 */
nos_kv_table_t* nos_kv_table_create(const char *name, uint32_t key_size, uint32_t max_val_size, uint32_t capacity);

/**
 * @brief 写入/更新记录
 */
nos_status_t nos_kv_put(nos_kv_table_t *table, const void *key, const void *val, uint32_t val_len);

/**
 * @brief 读取记录 (拷贝模式)
 */
nos_status_t nos_kv_get(nos_kv_table_t *table, const void *key, void *val_buf, uint32_t *val_len);

/**
 * @brief 读取记录 (只读指针模式，零拷贝)
 * @param out_ptr 输出指向 Value 的指针
 * @param out_handle 输出锁句柄，使用完后必须调用 nos_kv_release_ptr
 */
nos_status_t nos_kv_get_ptr(nos_kv_table_t *table, const void *key, const void **out_ptr, uint32_t *out_len, void **out_handle);

/**
 * @brief 释放只读锁
 */
void nos_kv_release_ptr(void *handle);

/**
 * @brief 删除记录
 */
nos_status_t nos_kv_del(nos_kv_table_t *table, const void *key);

/**
 * @brief 打印表统计信息
 */
void nos_kv_table_dump(nos_kv_table_t *table);

/**
 * @brief 打印所有表的统计信息
 */
void nos_kv_dump_all(void);

/**
 * @brief 获取 KV 引擎总内存占用 (Byte)
 */
size_t nos_kv_get_total_mem_usage(void);

/**
 * @brief KV 变更通知回调函数
 */
typedef void (*nos_kv_notify_fn)(nos_kv_table_t *table, const void *key, const void *val, uint32_t val_len, void *arg);

/**
 * @brief KV 服务操作接口 (API Table)
 */
typedef struct {
    nos_kv_table_t* (*table_create)(const char *name, uint32_t key_size, uint32_t max_val_size, uint32_t capacity);
    nos_status_t    (*put)(nos_kv_table_t *table, const void *key, const void *val, uint32_t val_len);
    nos_status_t    (*get)(nos_kv_table_t *table, const void *key, void *val_buf, uint32_t *val_len);
    nos_status_t    (*get_ptr)(nos_kv_table_t *table, const void *key, const void **out_ptr, uint32_t *out_len, void **out_handle);
    void            (*release_ptr)(void *handle);
    nos_status_t    (*del)(nos_kv_table_t *table, const void *key);
    nos_status_t    (*subscribe)(nos_kv_table_t *table, const void *key, nos_kv_notify_fn notify, void *arg);
    nos_status_t    (*unsubscribe)(nos_kv_table_t *table, const void *key, nos_kv_notify_fn notify, void *arg);
} nos_kv_ops_t;

/**
 * @brief 内部接口：初始化 KV 数据库引擎并注册服务
 */
void nos_kv_db_init(void);

/**
 * @brief 内部接口：释放所有 KV 数据库资源
 */
void nos_kv_db_deinit(void);

#endif /* __NOS_KV_API_H__ */
