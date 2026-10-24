#ifndef TOW_SHA256_H
#define TOW_SHA256_H
#include <stddef.h>

int sha256_memory(const void *data, size_t size, char hex[65]);
int sha256_file(const char *path, char hex[65]);
#endif
