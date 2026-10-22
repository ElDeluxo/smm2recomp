#pragma once
// zstd (third_party/zstd, BSD): built once, in runtime/zstd_lib.c. The calls the runtime and the games use.
#include <stddef.h>

size_t ZSTD_compress(void *dst, size_t cap, const void *src, size_t n, int level);
size_t ZSTD_decompress(void *dst, size_t cap, const void *src, size_t n);
unsigned long long ZSTD_getFrameContentSize(const void *src, size_t n);
size_t ZSTD_compressBound(size_t n);
unsigned ZSTD_isError(size_t code);
#ifndef ZSTD_CONTENTSIZE_UNKNOWN
#define ZSTD_CONTENTSIZE_UNKNOWN (0ULL - 1)
#define ZSTD_CONTENTSIZE_ERROR (0ULL - 2)
#endif
