/*
 * =============================================================================
 * JARVIS AI OS — Block device layer (kernel/block.c)
 * Language: C17
 *
 * One registry for everything that stores data in 512-byte sectors (SATA
 * disks today; USB storage registers here too once its driver exists).
 * File systems talk to this layer, never to a driver directly.
 * =============================================================================
 */

#include "kernel.h"
#include <string.h>

#define BLOCK_MAX 8

typedef struct {
    block_info_t info;
    int (*read)(void *, uint64_t, uint32_t, void *);
    int (*write)(void *, uint64_t, uint32_t, const void *);
    int (*flush)(void *);
    void *ctx;
} block_dev_t;

static block_dev_t g_block[BLOCK_MAX];
static int g_block_count = 0;

int block_register(const char *name, const char *model, uint64_t sectors,
                   int (*read)(void *, uint64_t, uint32_t, void *),
                   int (*write)(void *, uint64_t, uint32_t, const void *),
                   int (*flush)(void *), void *ctx)
{
    if (g_block_count >= BLOCK_MAX) return -1;
    block_dev_t *b = &g_block[g_block_count];
    memset(b, 0, sizeof(*b));
    strncpy(b->info.name, name, sizeof(b->info.name) - 1);
    strncpy(b->info.model, model ? model : "", sizeof(b->info.model) - 1);
    b->info.sectors = sectors;
    b->read = read; b->write = write; b->flush = flush; b->ctx = ctx;
    return g_block_count++;
}

int block_count(void) { return g_block_count; }

int block_get_info(int dev, block_info_t *out)
{
    if (dev < 0 || dev >= g_block_count) return -1;
    *out = g_block[dev].info;
    return 0;
}

int block_read(int dev, uint64_t lba, uint32_t count, void *buf)
{
    if (dev < 0 || dev >= g_block_count) return -1;
    block_dev_t *b = &g_block[dev];
    int rc = b->read(b->ctx, lba, count, buf);
    if (rc == 0) b->info.sectors_read += count; else b->info.errors++;
    return rc;
}

int block_write(int dev, uint64_t lba, uint32_t count, const void *buf)
{
    if (dev < 0 || dev >= g_block_count) return -1;
    block_dev_t *b = &g_block[dev];
    int rc = b->write(b->ctx, lba, count, buf);
    if (rc == 0) b->info.sectors_written += count; else b->info.errors++;
    return rc;
}

int block_flush(int dev)
{
    if (dev < 0 || dev >= g_block_count) return -1;
    return g_block[dev].flush ? g_block[dev].flush(g_block[dev].ctx) : 0;
}
