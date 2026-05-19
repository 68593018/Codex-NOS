#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nos_timer_api.h"

static nos_timer_ops_t g_timer_ops = {
    .create = nos_timer_create,
    .start = nos_timer_start,
    .stop = nos_timer_stop,
    .delete = nos_timer_delete
};

void nos_timer_init(void) {
    static int initialized = 0;
    if (initialized) return;

    extern nos_status_t nos_embedded_service_register(const char *name, void *ops);
    nos_embedded_service_register("SVC_TIMER", &g_timer_ops);
    
    initialized = 1;
}
