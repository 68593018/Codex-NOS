#ifndef __NOS_STATS_H__
#define __NOS_STATS_H__

#include <stdint.h>
#include "nos_types.h"

typedef uint64_t (*nos_stats_gauge_read_fn)(void *arg);

nos_status_t nos_stats_register_counter(const char *module, const char *name, const volatile uint64_t *value);
nos_status_t nos_stats_register_gauge(const char *module, const char *name, nos_stats_gauge_read_fn read_fn, void *arg);
void nos_stats_dump_all(void);
void nos_stats_dump_module(const char *module);
void nos_stats_init(void);

#endif /* __NOS_STATS_H__ */
