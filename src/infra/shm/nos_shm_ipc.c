#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "nos_api.h"
#include "nos_shm_ipc.h"
#include "nos_stats.h"

typedef struct {
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
    uint32_t slot_count;
    uint32_t slot_size;
} nos_shm_ring_hdr_t;

typedef struct {
    _Atomic uint32_t ready;
    uint32_t len;
    uint8_t data[NOS_SHM_SLOT_SIZE];
} nos_shm_slot_t;

typedef struct {
    nos_shm_ring_hdr_t hdr;
    nos_shm_slot_t slots[NOS_SHM_SLOT_COUNT];
} nos_shm_ring_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    nos_shm_ring_t ab;
    nos_shm_ring_t ba;
} nos_shm_region_t;

struct nos_shm_channel_s {
    char name[NOS_SHM_NAME_MAX];
    int fd;
    int owner;
    int tx_ab;
    size_t size;
    nos_shm_region_t *region;
    nos_shm_ring_t *tx;
    nos_shm_ring_t *rx;
};

static _Atomic uint64_t g_shm_tx_packets;
static _Atomic uint64_t g_shm_rx_packets;
static _Atomic uint64_t g_shm_tx_bytes;
static _Atomic uint64_t g_shm_rx_bytes;
static _Atomic uint64_t g_shm_ring_full;
static _Atomic uint64_t g_shm_fallback_uds;
static _Atomic uint64_t g_shm_open_errors;

#define NOS_SHM_MAGIC 0x4E53484Du
#define NOS_SHM_VERSION 1u

static void ring_init(nos_shm_ring_t *ring) {
    atomic_store(&ring->hdr.head, 0);
    atomic_store(&ring->hdr.tail, 0);
    ring->hdr.slot_count = NOS_SHM_SLOT_COUNT;
    ring->hdr.slot_size = NOS_SHM_SLOT_SIZE;
    for (uint32_t i = 0; i < NOS_SHM_SLOT_COUNT; i++) {
        atomic_store(&ring->slots[i].ready, 0);
        ring->slots[i].len = 0;
    }
}

static nos_shm_channel_t* channel_map(const char *name, int tx_ab, int create) {
    int flags = O_RDWR | (create ? O_CREAT : 0);
    int fd = shm_open(name, flags, 0600);
    if (fd < 0) {
        atomic_fetch_add(&g_shm_open_errors, 1);
        nos_sys_log_warn("SHM %s %s failed: %s", create ? "create" : "open", name, strerror(errno));
        return NULL;
    }

    size_t size = sizeof(nos_shm_region_t);
    if (create && ftruncate(fd, (off_t)size) != 0) {
        atomic_fetch_add(&g_shm_open_errors, 1);
        nos_sys_log_warn("SHM truncate %s failed: %s", name, strerror(errno));
        close(fd);
        return NULL;
    }

    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        atomic_fetch_add(&g_shm_open_errors, 1);
        nos_sys_log_warn("SHM mmap %s failed: %s", name, strerror(errno));
        close(fd);
        return NULL;
    }

    nos_shm_channel_t *channel = calloc(1, sizeof(nos_shm_channel_t));
    if (!channel) {
        munmap(mem, size);
        close(fd);
        return NULL;
    }

    strncpy(channel->name, name, sizeof(channel->name) - 1);
    channel->fd = fd;
    channel->owner = create;
    channel->tx_ab = tx_ab;
    channel->size = size;
    channel->region = (nos_shm_region_t *)mem;

    if (create || channel->region->magic != NOS_SHM_MAGIC) {
        channel->region->magic = NOS_SHM_MAGIC;
        channel->region->version = NOS_SHM_VERSION;
        ring_init(&channel->region->ab);
        ring_init(&channel->region->ba);
    }

    channel->tx = tx_ab ? &channel->region->ab : &channel->region->ba;
    channel->rx = tx_ab ? &channel->region->ba : &channel->region->ab;
    return channel;
}

nos_shm_channel_t* nos_shm_channel_create(const char *name, int tx_ab) {
    if (!name || name[0] != '/') return NULL;
    shm_unlink(name);
    return channel_map(name, tx_ab, 1);
}

nos_shm_channel_t* nos_shm_channel_open(const char *name, int tx_ab) {
    if (!name || name[0] != '/') return NULL;
    return channel_map(name, tx_ab, 0);
}

void nos_shm_channel_close(nos_shm_channel_t *channel) {
    if (!channel) return;
    if (channel->region) munmap(channel->region, channel->size);
    if (channel->fd >= 0) close(channel->fd);
    if (channel->owner && channel->name[0] != '\0') shm_unlink(channel->name);
    free(channel);
}

nos_status_t nos_shm_channel_write(nos_shm_channel_t *channel, const void *data, uint32_t len) {
    if (!channel || !data || len == 0 || len > NOS_SHM_SLOT_SIZE) return NOS_ERR;
    nos_shm_ring_t *ring = channel->tx;
    uint32_t head = atomic_load_explicit(&ring->hdr.head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&ring->hdr.tail, memory_order_relaxed);
    uint32_t next_tail = (tail + 1) % NOS_SHM_SLOT_COUNT;
    if (next_tail == head) {
        atomic_fetch_add(&g_shm_ring_full, 1);
        atomic_fetch_add(&g_shm_fallback_uds, 1);
        return NOS_ERR_BUSY;
    }

    nos_shm_slot_t *slot = &ring->slots[tail];
    memcpy(slot->data, data, len);
    slot->len = len;
    atomic_store_explicit(&slot->ready, 1, memory_order_release);
    atomic_store_explicit(&ring->hdr.tail, next_tail, memory_order_release);
    atomic_fetch_add(&g_shm_tx_packets, 1);
    atomic_fetch_add(&g_shm_tx_bytes, len);
    return NOS_OK;
}

uint32_t nos_shm_channel_drain(nos_shm_channel_t *channel, void (*on_msg)(const void *data, uint32_t len, void *arg), void *arg) {
    if (!channel || !on_msg) return 0;
    nos_shm_ring_t *ring = channel->rx;
    uint32_t drained = 0;

    while (1) {
        uint32_t head = atomic_load_explicit(&ring->hdr.head, memory_order_relaxed);
        uint32_t tail = atomic_load_explicit(&ring->hdr.tail, memory_order_acquire);
        if (head == tail) break;

        nos_shm_slot_t *slot = &ring->slots[head];
        if (!atomic_load_explicit(&slot->ready, memory_order_acquire)) break;
        on_msg(slot->data, slot->len, arg);
        atomic_store_explicit(&slot->ready, 0, memory_order_release);
        atomic_store_explicit(&ring->hdr.head, (head + 1) % NOS_SHM_SLOT_COUNT, memory_order_release);
        atomic_fetch_add(&g_shm_rx_packets, 1);
        atomic_fetch_add(&g_shm_rx_bytes, slot->len);
        drained++;
    }

    return drained;
}

void nos_shm_register_stats(void) {
    nos_stats_register_counter("shm", "tx_packets", (const volatile uint64_t *)&g_shm_tx_packets);
    nos_stats_register_counter("shm", "rx_packets", (const volatile uint64_t *)&g_shm_rx_packets);
    nos_stats_register_counter("shm", "tx_bytes", (const volatile uint64_t *)&g_shm_tx_bytes);
    nos_stats_register_counter("shm", "rx_bytes", (const volatile uint64_t *)&g_shm_rx_bytes);
    nos_stats_register_counter("shm", "ring_full", (const volatile uint64_t *)&g_shm_ring_full);
    nos_stats_register_counter("shm", "fallback_uds", (const volatile uint64_t *)&g_shm_fallback_uds);
    nos_stats_register_counter("shm", "open_errors", (const volatile uint64_t *)&g_shm_open_errors);
}
