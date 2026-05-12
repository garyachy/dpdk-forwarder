#pragma once

#ifdef UNIT_TEST
#include <stdio.h>
#define LOG_ERR(fmt, ...)  fprintf(stderr, "[ERR]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_DBG(fmt, ...)  fprintf(stderr, "[DBG]  " fmt "\n", ##__VA_ARGS__)
#else
#include <rte_log.h>
#define FWD_LOGTYPE RTE_LOGTYPE_USER1
#define _FWD_LOG(lvl, fmt, ...) \
    rte_log(RTE_LOG_##lvl, FWD_LOGTYPE, "[%s:%d] " fmt "\n", \
            __func__, __LINE__, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)  _FWD_LOG(ERR,     fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) _FWD_LOG(WARNING, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) _FWD_LOG(INFO,    fmt, ##__VA_ARGS__)
#define LOG_DBG(fmt, ...)  _FWD_LOG(DEBUG,   fmt, ##__VA_ARGS__)
#endif
