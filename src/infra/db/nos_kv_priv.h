#ifndef __NOS_KV_PRIV_H__
#define __NOS_KV_PRIV_H__

#include <pthread.h>
#include "nos_kv_api.h"

#define KV_INVALID_IDX 0xFFFFFFFF

/**
 * @brief 订阅者节点
 */
typedef struct nos_kv_sub {
    nos_kv_notify_fn notify;
    void            *arg;
    struct nos_kv_sub *next;
} nos_kv_sub_t;

/**
 * @brief 单个 KV 条目
 * 内存布局: [Header] + [Key_Data] + [Value_Data]
 */
typedef struct {
    uint32_t next_idx;     // 冲突链索引
    uint32_t val_len;      // 当前有效 Value 长度
    nos_kv_sub_t *sub_list; // 订阅者链表
    uint8_t  data[];       // 柔性数组
} nos_kv_entry_t;

/**
 * @brief 哈希桶
 */
typedef struct {
    pthread_rwlock_t lock;
    uint32_t         first_idx;
} nos_kv_bucket_t;

/**
 * @brief KV 表内部上下文
 */
struct nos_kv_table {
    char     name[32];
    uint32_t key_size;
    uint32_t max_val_size;
    uint32_t entry_size;   // 单个 entry 占用的总字节数
    uint32_t capacity;

    nos_kv_bucket_t *buckets;
    uint32_t         bucket_count;

    void            *pool_mem;   // 条目内存池
    uint32_t        *free_stack; // 空闲索引栈
    int              free_top;
    
    /* 统计 */
    uint32_t         used_count;
};

/**
 * @brief 内部锁句柄 (用于 nos_kv_get_ptr)
 */
typedef struct {
    pthread_rwlock_t *lock;
} nos_kv_lock_handle_t;

#endif /* __NOS_KV_PRIV_H__ */
