/*
 * Copyright ©2021 Collabora Ltd.
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef GEN_SHADERS_H
#define GEN_SHADERs_H

/* Macros for handling per-gen compilation.
 *
 * The macro GENX() automatically suffixes whatever you give it with _vX
 *
 * You can do pseudo-runtime checks in your function such as
 *
 * if (PAN_ARCH == 4) {
 *    // Do something
 * }
 *
 * The contents of the if statement must be valid regardless of gen, but
 * the if will get compiled away on everything except first-generation Midgard.
 *
 * For places where you really do have a compile-time conflict, you can
 * use preprocessor logic:
 *
 * #if (PAN_ARCH == 75)
 *    // Do something
 * #endif
 *
 * However, it is strongly recommended that the former be used whenever
 * possible.
 */

/* Returns the architecture version given a GPU ID, either from a table for
 * old-style Midgard versions or directly for new-style Bifrost/Valhall
 * versions */

/* Base macro defined on the command line. */
/* Suffixing macros */
#ifndef PAN_ARCH
#else
#if (PAN_ARCH == 4)
#define GENX(X) X##_v4
#include "libpanfrost_shaders_v4.h"
#elif (PAN_ARCH == 5)
#define GENX(X) X##_v5
#include "libpanfrost_shaders_v5.h"
#elif (PAN_ARCH == 6)
#define GENX(X) X##_v6
#include "libpanfrost_shaders_v6.h"
#elif (PAN_ARCH == 7)
#define GENX(X) X##_v7
#include "libpanfrost_shaders_v7.h"
#elif (PAN_ARCH == 9)
#define GENX(X) X##_v9
#include "libpanfrost_shaders_v9.h"
#elif (PAN_ARCH == 10)
#define GENX(X) X##_v10
#include "libpanfrost_shaders_v10.h"
#else
#error "Need to add suffixing macro for this architecture"
#endif


#include <util/macros.h>
#include <string.h>
#include <git_sha1.h>

static_assert(strcmp(PANCLC_MESA_GIT_SHA1, MESA_GIT_SHA1) == 0);

#endif

#endif /* GEN_MACROS_H */
