/*
 * Copyright 2023 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#ifndef LIBPAN_H
#define LIBPAN_H

/* Define stdint types compatible between the CPU and GPU for shared headers */
#ifndef __OPENCL_VERSION__
#include <stdint.h>
#include "util/macros.h"
#define GLOBAL(type_)            uint64_t
#define PAN_STATIC_ASSERT(_COND) static_assert(_COND, #_COND)
#else
#pragma OPENCL EXTENSION cl_khr_fp16 : enable
#define PACKED        __attribute__((packed, aligned(4)))
#define GLOBAL(type_) global type_ *

typedef ulong uint64_t;
typedef uint uint32_t;
typedef ushort uint16_t;
typedef uchar uint8_t;

typedef long int64_t;
typedef int int32_t;
typedef short int16_t;
typedef char int8_t;

#define PAN_STATIC_ASSERT(_COND)                                               \
   typedef char static_assertion_##__line__[(_COND) ? 1 : -1]

#endif

#endif
