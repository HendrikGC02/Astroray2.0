/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ASTRORAY_H
#define ASTRORAY_H
#include <stdint.h>
#ifdef _WIN32
#ifdef AR_BUILDING_CORE
#define AR_API __declspec(dllexport)
#else
#define AR_API __declspec(dllimport)
#endif
#else
#define AR_API
#endif
#define AR_QUERY
#define AR_IN
#define AR_OUT
#define AR_IN_ARRAY(n, T)
#define AR_OUT_ARRAY(n, T)
#define AR_STAGED(n, T)
typedef uint32_t ar_status;
typedef uint64_t ar_key;
typedef struct ar_context_s* ar_context;
typedef struct ar_x_desc {
    uint32_t count;
    AR_IN_ARRAY(count, uint32_t) const float* items;
} ar_x_desc;
#endif
