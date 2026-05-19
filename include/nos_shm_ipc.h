#ifndef __NOS_SHM_IPC_H__
#define __NOS_SHM_IPC_H__

#include <stddef.h>
#include <stdint.h>
#include "nos_types.h"

#define NOS_SHM_NAME_MAX 64
#define NOS_SHM_SLOT_SIZE 8192
#define NOS_SHM_SLOT_COUNT 256

typedef struct nos_shm_channel_s nos_shm_channel_t;

nos_shm_channel_t* nos_shm_channel_create(const char *name, int tx_ab);
nos_shm_channel_t* nos_shm_channel_open(const char *name, int tx_ab);
void nos_shm_channel_close(nos_shm_channel_t *channel);
nos_status_t nos_shm_channel_write(nos_shm_channel_t *channel, const void *data, uint32_t len);
uint32_t nos_shm_channel_drain(nos_shm_channel_t *channel, void (*on_msg)(const void *data, uint32_t len, void *arg), void *arg);
void nos_shm_register_stats(void);

#endif /* __NOS_SHM_IPC_H__ */
