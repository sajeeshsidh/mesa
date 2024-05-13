/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_PIPELINE_LAYOUT_H
#define PANVK_PIPELINE_LAYOUT_H

#ifndef PAN_ARCH
#error "PAN_ARCH must be defined"
#endif

#include <stdint.h>

#include "vk_pipeline_layout.h"

#include "panvk_descriptor_set_layout.h"
#include "panvk_macros.h"
#include "panvk_set_collection_layout.h"

struct panvk_pipeline_layout {
   struct vk_pipeline_layout vk;

   unsigned char sha1[20];
   struct panvk_set_collection_layout set_layout;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(panvk_pipeline_layout, vk.base, VkPipelineLayout,
                               VK_OBJECT_TYPE_PIPELINE_LAYOUT)

#endif
