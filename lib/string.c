#include "axys/string.h"

axys_size_t axys_strlen(const char *string)
{
    const char *cursor = string;

    while (*cursor != '\0') {
        ++cursor;
    }
    return (axys_size_t)(cursor - string);
}

int axys_strcmp(const char *left, const char *right)
{
    const unsigned char *left_cursor = (const unsigned char *)left;
    const unsigned char *right_cursor = (const unsigned char *)right;

    while (*left_cursor == *right_cursor) {
        if (*left_cursor == '\0') {
            return 0;
        }
        ++left_cursor;
        ++right_cursor;
    }
    if (*left_cursor < *right_cursor) {
        return -1;
    }
    return 1;
}

int axys_strncmp(const char *left, const char *right, axys_size_t length)
{
    const unsigned char *left_cursor = (const unsigned char *)left;
    const unsigned char *right_cursor = (const unsigned char *)right;

    while (length != 0) {
        if (*left_cursor != *right_cursor) {
            if (*left_cursor < *right_cursor) {
                return -1;
            }
            return 1;
        }
        if (*left_cursor == '\0') {
            return 0;
        }
        ++left_cursor;
        ++right_cursor;
        --length;
    }
    return 0;
}

axys_size_t axys_strlcpy(char *destination, const char *source, axys_size_t size)
{
    axys_size_t source_length = axys_strlen(source);
    axys_size_t copy_length = source_length;

    if (size != 0) {
        if (copy_length >= size) {
            copy_length = size - 1;
        }
        for (axys_size_t index = 0; index < copy_length; ++index) {
            destination[index] = source[index];
        }
        destination[copy_length] = '\0';
    }
    return source_length;
}

void *axys_memset(void *destination, int value, axys_size_t length)
{
    axys_uint8_t *bytes = (axys_uint8_t *)destination;

    for (axys_size_t index = 0; index < length; ++index) {
        bytes[index] = (axys_uint8_t)value;
    }
    return destination;
}

void *axys_memcpy(void *destination, const void *source, axys_size_t length)
{
    axys_uint8_t *destination_bytes = (axys_uint8_t *)destination;
    const axys_uint8_t *source_bytes = (const axys_uint8_t *)source;

    for (axys_size_t index = 0; index < length; ++index) {
        destination_bytes[index] = source_bytes[index];
    }
    return destination;
}

void *axys_memmove(void *destination, const void *source, axys_size_t length)
{
    axys_uint8_t *destination_bytes = (axys_uint8_t *)destination;
    const axys_uint8_t *source_bytes = (const axys_uint8_t *)source;

    if (destination_bytes == source_bytes || length == 0) {
        return destination;
    }
    if (destination_bytes < source_bytes) {
        for (axys_size_t index = 0; index < length; ++index) {
            destination_bytes[index] = source_bytes[index];
        }
    } else {
        for (axys_size_t index = length; index != 0; --index) {
            destination_bytes[index - 1] = source_bytes[index - 1];
        }
    }
    return destination;
}

int axys_memcmp(const void *left, const void *right, axys_size_t length)
{
    const axys_uint8_t *left_bytes = (const axys_uint8_t *)left;
    const axys_uint8_t *right_bytes = (const axys_uint8_t *)right;

    for (axys_size_t index = 0; index < length; ++index) {
        if (left_bytes[index] != right_bytes[index]) {
            if (left_bytes[index] < right_bytes[index]) {
                return -1;
            }
            return 1;
        }
    }
    return 0;
}
