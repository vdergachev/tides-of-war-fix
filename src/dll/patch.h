#ifndef TOW_PATCH_H
#define TOW_PATCH_H
#include <stdint.h>
#include <stddef.h>

int patch_check(void *addr, const uint8_t *expected, size_t n);
int patch_write(void *addr, const uint8_t *expected, const uint8_t *bytes, size_t n);
int patch_jmp(void *site, const uint8_t *expected5, void *target);
int patch_jmp_padded(void *site, const uint8_t *expected, size_t n, void *target);
int patch_call(void *site, const uint8_t *expected5, void *target);
#endif
