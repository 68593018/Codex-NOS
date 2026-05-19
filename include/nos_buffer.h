#ifndef __NOS_BUFFER_H__
#define __NOS_BUFFER_H__

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include "nos_types.h"
#include "nos_manifest.h"

/**
 * @brief NOS 统一内存 Buffer 结构
 */
typedef struct {
    uint8_t *raw_data;         /**< 原始分配的数据起始地址 */
    uint8_t *data;             /**< 当前有效数据起始地址 (raw_data + headroom) */
    uint32_t capacity;         /**< 原始分配的总容量 */
    uint32_t len;              /**< 当前有效数据长度 */
    uint32_t headroom;         /**< 头部预留空间大小 */
    uint32_t tailroom;         /**< 尾部剩余空间大小 */
    _Atomic int ref_cnt;       /**< 引用计数 (原子操作) */
    uint32_t bin_idx;          /**< 属于哪个规格池 (bin) */
    uint32_t pool_idx;         /**< 在 bin 内的索引 */
} nos_buffer_t;

/**
 * @brief 缓冲区池统计信息
 */
typedef struct {
    uint32_t chunk_size;
    uint32_t total_count;
    uint32_t used_count;
    uint32_t peak_count;
} nos_buffer_stats_t;

/**
 * @brief 初始化全局 Buffer 池 (根据清单配置)
 */
nos_status_t nos_buffer_init_pool(const nos_buffer_pool_def_t *config);

/**
 * @brief 释放所有 Buffer 池资源
 */
void nos_buffer_deinit_pool(void);

/**
 * @brief 从池中申请一个最匹配大小的 Buffer
 * @param size 业务需要的最小字节数
 * @param headroom 预留的头部字节数 (用于协议头封装)
 * @return nos_buffer_t* 成功返回指针，失败返回 NULL
 */
nos_buffer_t* nos_buffer_alloc(uint32_t size, uint32_t headroom);

/**
 * @brief 增加 Buffer 的引用计数
 */
void nos_buffer_retain(nos_buffer_t *buf);

/**
 * @brief 释放/减少 Buffer 的引用计数
 * @note 当计数减至 0 时，Buffer 自动归还至池中
 */
void nos_buffer_release(nos_buffer_t *buf);

/**
 * @brief 在 Buffer 头部“推入”数据 (封装)
 * @return 成功返回新的 data 指针，失败返回 NULL
 */
void* nos_buffer_push(nos_buffer_t *buf, uint32_t size);

/**
 * @brief 在 Buffer 头部“弹出”数据 (解封装)
 * @return 成功返回新的 data 指针，失败返回 NULL
 */
void* nos_buffer_pull(nos_buffer_t *buf, uint32_t size);

/**
 * @brief 打印当前 Buffer 池的状态统计
 */
void nos_buffer_dump_stats(void);

/**
 * @brief 获取 Buffer 池总内存占用 (Byte)
 */
size_t nos_buffer_get_total_mem_usage(void);

#endif /* __NOS_BUFFER_H__ */
