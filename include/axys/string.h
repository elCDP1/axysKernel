#ifndef AXYS_STRING_H
#define AXYS_STRING_H

#include "axys/types.h"

axys_size_t axys_strlen(const char *string);
int axys_strcmp(const char *left, const char *right);
int axys_strncmp(const char *left, const char *right, axys_size_t length);
axys_size_t axys_strlcpy(char *destination, const char *source, axys_size_t size);
void *axys_memset(void *destination, int value, axys_size_t length);
void *axys_memcpy(void *destination, const void *source, axys_size_t length);
void *axys_memmove(void *destination, const void *source, axys_size_t length);
int axys_memcmp(const void *left, const void *right, axys_size_t length);

#endif
