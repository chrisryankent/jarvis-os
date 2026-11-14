/* JARVIS AI OS — .xz decompressor (kernel/xz.c) */
#ifndef JARVIS_XZ_H
#define JARVIS_XZ_H
#include <stddef.h>
#include <stdint.h>

/* Receives decompressed data in pieces; return nonzero to stop. */
typedef int (*xz_out_fn)(void *ctx, const uint8_t *data, size_t n);

/* Decompresses a whole .xz file. Returns 0, or -1 with a reason in err. */
int xz_decode(const uint8_t *in, size_t len, xz_out_fn out, void *ctx,
              void *(*alloc)(size_t), void (*release)(void *, size_t), char *err, size_t en);

uint32_t xz_crc32(const uint8_t *p, size_t n, uint32_t crc);

#endif
