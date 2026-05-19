#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include "nos_stats.h"

#define NOS_STATS_MAX_ITEMS 128
#define NOS_STATS_NAME_MAX 32

typedef enum {
    NOS_STATS_COUNTER,
    NOS_STATS_GAUGE
} nos_stats_type_t;

typedef struct {
    char module[NOS_STATS_NAME_MAX];
    char name[NOS_STATS_NAME_MAX];
    nos_stats_type_t type;
    const volatile uint64_t *counter;
    nos_stats_gauge_read_fn read_fn;
    void *arg;
} nos_stats_item_t;

static nos_stats_item_t g_stats[NOS_STATS_MAX_ITEMS];
static uint32_t g_stats_count = 0;
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;

void nos_stats_init(void) {
    pthread_mutex_lock(&g_stats_lock);
    g_stats_count = 0;
    memset(g_stats, 0, sizeof(g_stats));
    pthread_mutex_unlock(&g_stats_lock);
}

static nos_status_t stats_register_item(
    const char *module,
    const char *name,
    nos_stats_type_t type,
    const volatile uint64_t *counter,
    nos_stats_gauge_read_fn read_fn,
    void *arg
) {
    if (!module || !name) return NOS_ERR;

    pthread_mutex_lock(&g_stats_lock);
    for (uint32_t i = 0; i < g_stats_count; i++) {
        if (strcmp(g_stats[i].module, module) == 0 && strcmp(g_stats[i].name, name) == 0) {
            pthread_mutex_unlock(&g_stats_lock);
            return NOS_OK;
        }
    }

    if (g_stats_count >= NOS_STATS_MAX_ITEMS) {
        pthread_mutex_unlock(&g_stats_lock);
        return NOS_ERR_BUSY;
    }

    nos_stats_item_t *item = &g_stats[g_stats_count++];
    strncpy(item->module, module, sizeof(item->module) - 1);
    strncpy(item->name, name, sizeof(item->name) - 1);
    item->type = type;
    item->counter = counter;
    item->read_fn = read_fn;
    item->arg = arg;
    pthread_mutex_unlock(&g_stats_lock);
    return NOS_OK;
}

nos_status_t nos_stats_register_counter(const char *module, const char *name, const volatile uint64_t *value) {
    if (!value) return NOS_ERR;
    return stats_register_item(module, name, NOS_STATS_COUNTER, value, NULL, NULL);
}

nos_status_t nos_stats_register_gauge(const char *module, const char *name, nos_stats_gauge_read_fn read_fn, void *arg) {
    if (!read_fn) return NOS_ERR;
    return stats_register_item(module, name, NOS_STATS_GAUGE, NULL, read_fn, arg);
}

static uint64_t stats_read_item(const nos_stats_item_t *item) {
    if (item->type == NOS_STATS_COUNTER) {
        return atomic_load((const volatile _Atomic uint64_t *)item->counter);
    }
    return item->read_fn ? item->read_fn(item->arg) : 0;
}

static void stats_dump_locked(const char *module) {
    printf("\n--- NOS Stats%s%s ---\n", module ? ": " : "", module ? module : "");
    printf("%-16s %-28s %-10s %-20s\n", "Module", "Name", "Type", "Value");
    printf("----------------------------------------------------------------------------\n");

    for (uint32_t i = 0; i < g_stats_count; i++) {
        nos_stats_item_t *item = &g_stats[i];
        if (module && strcmp(item->module, module) != 0) continue;
        printf("%-16s %-28s %-10s %-20llu\n",
               item->module,
               item->name,
               item->type == NOS_STATS_COUNTER ? "counter" : "gauge",
               (unsigned long long)stats_read_item(item));
    }
}

void nos_stats_dump_all(void) {
    pthread_mutex_lock(&g_stats_lock);
    stats_dump_locked(NULL);
    pthread_mutex_unlock(&g_stats_lock);
}

void nos_stats_dump_module(const char *module) {
    pthread_mutex_lock(&g_stats_lock);
    stats_dump_locked(module);
    pthread_mutex_unlock(&g_stats_lock);
}
