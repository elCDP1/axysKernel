#ifndef AXYS_TYPES_H
#define AXYS_TYPES_H

typedef __INT8_TYPE__ axys_int8_t;
typedef __UINT8_TYPE__ axys_uint8_t;
typedef __INT16_TYPE__ axys_int16_t;
typedef __UINT16_TYPE__ axys_uint16_t;
typedef __INT32_TYPE__ axys_int32_t;
typedef __UINT32_TYPE__ axys_uint32_t;
typedef __INT64_TYPE__ axys_int64_t;
typedef __UINT64_TYPE__ axys_uint64_t;
typedef __SIZE_TYPE__ axys_size_t;
typedef __UINTPTR_TYPE__ axys_uintptr_t;
typedef __INTPTR_TYPE__ axys_intptr_t;

#define AXYS_NULL ((void *)0)
#define AXYS_ARRAY_SIZE(value) (sizeof(value) / sizeof((value)[0]))
#define AXYS_PACKED __attribute__((packed))
#define AXYS_NORETURN __attribute__((noreturn))
#define AXYS_UNUSED __attribute__((unused))
#define AXYS_ALWAYS_INLINE __attribute__((always_inline)) inline
#define AXYS_NOINLINE __attribute__((noinline))
#define AXYS_ALIGN(value) __attribute__((aligned(value)))
#define AXYS_LIKELY(condition) __builtin_expect(!!(condition), 1)
#define AXYS_UNLIKELY(condition) __builtin_expect(!!(condition), 0)

/* Compile-time assertion: fails to compile if the condition is false. */
#define AXYS_STATIC_ASSERT(condition, tag) \
    typedef char axys_static_assert_##tag[(condition) ? 1 : -1]

#endif
