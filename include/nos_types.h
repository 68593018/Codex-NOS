#ifndef __NOS_TYPES_H__
#define __NOS_TYPES_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief NOS 通用状态/错误码
 */
typedef enum {
    NOS_OK = 0,
    NOS_ERR = -1,
    NOS_ERR_NOMEM = -2,
    NOS_ERR_NOT_FOUND = -3,
    NOS_ERR_BUSY = -4,
    NOS_ERR_TIMEOUT = -5,
} nos_status_t;

/**
 * @brief 常用宏定义
 */
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#endif /* __NOS_TYPES_H__ */
