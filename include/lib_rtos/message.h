#pragma once

#include <stdio.h>

#ifndef NDEBUG
#include <assert.h>

#define DBG_ASSERT_OP(name, op, val1, val2)                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!((val1)op(val2)))                                                                                         \
        {                                                                                                              \
            fprintf(stderr, "[ASSERT] %s:%d: Check failed: %s %s %s (%d vs. %d)\n", __FILE__, __LINE__, #val1, #op,    \
                    #val2, (int)(val1), (int)(val2));                                                                  \
            assert((val1)op(val2));                                                                                    \
        }                                                                                                              \
    } while (0)

#define DBG_ASSERT_COND(condition)                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "[ASSERT] %s:%d: Check failed: %s\n", __FILE__, __LINE__, #condition);                     \
            assert(condition);                                                                                         \
        }                                                                                                              \
    } while (0)

#define DBG_ASSERT_EQ(val1, val2) DBG_ASSERT_OP(EQ, ==, val1, val2)
#define DBG_ASSERT_NE(val1, val2) DBG_ASSERT_OP(NE, !=, val1, val2)
#define DBG_ASSERT_LE(val1, val2) DBG_ASSERT_OP(LE, <=, val1, val2)
#define DBG_ASSERT_LT(val1, val2) DBG_ASSERT_OP(LT, <, val1, val2)
#define DBG_ASSERT_GE(val1, val2) DBG_ASSERT_OP(GE, >=, val1, val2)
#define DBG_ASSERT_GT(val1, val2) DBG_ASSERT_OP(GT, >, val1, val2)
#else
#define DBG_ASSERT_OP(name, op, val1, val2) ((void)0)
#define DBG_ASSERT_COND(condition) ((void)0)
#define DBG_ASSERT_EQ(val1, val2) ((void)0)
#define DBG_ASSERT_NE(val1, val2) ((void)0)
#define DBG_ASSERT_LE(val1, val2) ((void)0)
#define DBG_ASSERT_LT(val1, val2) ((void)0)
#define DBG_ASSERT_GE(val1, val2) ((void)0)
#define DBG_ASSERT_GT(val1, val2) ((void)0)
#endif

#define VIDEO_INFO_PRINT(fmt, ...) printf("[INF] %s(%d): " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#define VIDEO_ERROR_PRINT(fmt, ...)                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("[ERR] %s(%d): " fmt "\n", __func__, __LINE__, ##__VA_ARGS__);                                          \
        fflush(stdout);                                                                                                \
    } while (0)
#define VIDEO_DEBUG_PRINT(fmt, ...) printf("[DEB] %s(%d): " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)