#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "nos_buffer.h"
#include "nos_api.h"

#define MAX_BINS 8
#define CACHE_LINE_SIZE 64

typedef struct {
    uint32_t chunk_size;
    uint32_t chunk_count;
    nos_buffer_t *buffers;
    uint8_t *raw_mem;
    uint32_t *free_stack;
    int free_top;
    
    /* 统计信息 */
    uint32_t used_count;
    uint32_t peak_count;
} nos_buffer_bin_t;

static nos_buffer_bin_t g_bins[MAX_BINS];
static uint32_t g_bin_count = 0;
static pthread_mutex_t g_buffer_lock = PTHREAD_MUTEX_INITIALIZER;

nos_status_t nos_buffer_init_pool(const nos_buffer_pool_def_t *config) {
    if (!config) return NOS_ERR;

    pthread_mutex_lock(&g_buffer_lock);
    if (g_bin_count > 0) {
        pthread_mutex_unlock(&g_buffer_lock);
        return NOS_OK; /* 已初始化 */
    }

    for (int i = 0; config[i].chunk_size > 0 && i < MAX_BINS; i++) {
        nos_buffer_bin_t *bin = &g_bins[i];
        bin->chunk_size = (config[i].chunk_size + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
        bin->chunk_count = config[i].chunk_count;
        
        bin->buffers = calloc(bin->chunk_count, sizeof(nos_buffer_t));
        
        /* 使用 posix_memalign 确保内存对齐 */
        if (posix_memalign((void**)&bin->raw_mem, CACHE_LINE_SIZE, bin->chunk_size * bin->chunk_count) != 0) {
            perror("posix_memalign failed");
            exit(EXIT_FAILURE);
        }
        
        bin->free_stack = malloc(sizeof(uint32_t) * bin->chunk_count);
        bin->free_top = -1;
        bin->used_count = 0;
        bin->peak_count = 0;

        for (uint32_t j = 0; j < bin->chunk_count; j++) {
            bin->buffers[j].raw_data = bin->raw_mem + (j * bin->chunk_size);
            bin->buffers[j].data = bin->buffers[j].raw_data;
            bin->buffers[j].capacity = bin->chunk_size;
            bin->buffers[j].len = 0;
            bin->buffers[j].headroom = 0;
            bin->buffers[j].tailroom = bin->chunk_size;
            bin->buffers[j].bin_idx = i;
            bin->buffers[j].pool_idx = j;
            atomic_init(&bin->buffers[j].ref_cnt, 0);
            
            bin->free_stack[++bin->free_top] = j;
        }
        g_bin_count++;
        nos_sys_log_info("Buffer Bin[%d] initialized: %u bytes * %u chunks (Aligned to %d)", 
               i, bin->chunk_size, bin->chunk_count, CACHE_LINE_SIZE);
    }
    pthread_mutex_unlock(&g_buffer_lock);
    return NOS_OK;
}

void nos_buffer_deinit_pool(void) {
    pthread_mutex_lock(&g_buffer_lock);
    for (uint32_t i = 0; i < g_bin_count; i++) {
        nos_buffer_bin_t *bin = &g_bins[i];
        if (bin->buffers) free(bin->buffers);
        if (bin->raw_mem) free(bin->raw_mem);
        if (bin->free_stack) free(bin->free_stack);
        memset(bin, 0, sizeof(nos_buffer_bin_t));
    }
    g_bin_count = 0;
    pthread_mutex_unlock(&g_buffer_lock);
    nos_sys_log_debug("Buffer pool deinitialized.");
}

nos_buffer_t* nos_buffer_alloc(uint32_t size, uint32_t headroom) {
    nos_buffer_t *buf = NULL;
    uint32_t total_needed = size + headroom;

    pthread_mutex_lock(&g_buffer_lock);
    for (uint32_t i = 0; i < g_bin_count; i++) {
        nos_buffer_bin_t *bin = &g_bins[i];
        if (bin->chunk_size >= total_needed && bin->free_top >= 0) {
            uint32_t idx = bin->free_stack[bin->free_top--];
            buf = &bin->buffers[idx];
            
            atomic_store(&buf->ref_cnt, 1);
            buf->headroom = headroom;
            buf->data = buf->raw_data + headroom;
            buf->len = size;
            buf->tailroom = buf->capacity - headroom - size;
            
            bin->used_count++;
            if (bin->used_count > bin->peak_count) bin->peak_count = bin->used_count;
            break;
        }
    }
    pthread_mutex_unlock(&g_buffer_lock);
    
    if (!buf) {
        nos_sys_log_warn("Buffer allocation failed for size %u + headroom %u", size, headroom);
    }
    return buf;
}

void nos_buffer_retain(nos_buffer_t *buf) {
    if (buf) {
        atomic_fetch_add(&buf->ref_cnt, 1);
    }
}

void nos_buffer_release(nos_buffer_t *buf) {
    if (!buf) return;

    if (atomic_fetch_sub(&buf->ref_cnt, 1) == 1) {
        pthread_mutex_lock(&g_buffer_lock);
        nos_buffer_bin_t *bin = &g_bins[buf->bin_idx];
        bin->free_stack[++bin->free_top] = buf->pool_idx;
        bin->used_count--;
        
        /* 重置 Buffer 状态 */
        buf->data = buf->raw_data;
        buf->len = 0;
        buf->headroom = 0;
        buf->tailroom = buf->capacity;
        
        pthread_mutex_unlock(&g_buffer_lock);
    }
}

void* nos_buffer_push(nos_buffer_t *buf, uint32_t size) {
    if (!buf || buf->headroom < size) return NULL;
    buf->headroom -= size;
    buf->data -= size;
    buf->len += size;
    return buf->data;
}

void* nos_buffer_pull(nos_buffer_t *buf, uint32_t size) {
    if (!buf || buf->len < size) return NULL;
    buf->headroom += size;
    buf->data += size;
    buf->len -= size;
    return buf->data;
}

size_t nos_buffer_get_total_mem_usage(void) {
    size_t total = 0;
    pthread_mutex_lock(&g_buffer_lock);
    for (uint32_t i = 0; i < g_bin_count; i++) {
        nos_buffer_bin_t *bin = &g_bins[i];
        total += sizeof(nos_buffer_bin_t);
        total += bin->chunk_count * (bin->chunk_size + sizeof(nos_buffer_t));
        total += bin->chunk_count * sizeof(uint32_t); // free_stack
    }
    pthread_mutex_unlock(&g_buffer_lock);
    return total;
}

void nos_buffer_dump_stats(void) {
    nos_sys_log_info("--- NOS Buffer Pool Statistics ---");
    nos_sys_log_info("%-5s %-12s %-8s %-8s %-8s", "Bin", "ChunkSize", "Total", "Used", "Peak");
    pthread_mutex_lock(&g_buffer_lock);
    for (uint32_t i = 0; i < g_bin_count; i++) {
        nos_buffer_bin_t *bin = &g_bins[i];
        nos_sys_log_info("[%2d] %-12u %-8u %-8u %-8u", 
               i, bin->chunk_size, bin->chunk_count, bin->used_count, bin->peak_count);
    }
    pthread_mutex_unlock(&g_buffer_lock);
}
