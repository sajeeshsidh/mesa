/*
 * Copyright © 2024 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_SET_COLLECTION_LAYOUT_H
#define PANVK_SET_COLLECTION_LAYOUT_H

#ifndef PAN_ARCH
#error "PAN_ARCH must be defined"
#endif

#include <stdint.h>

#include <vulkan/vulkan_core.h>
#include "vk_descriptor_set_layout.h"

#include "panvk_macros.h"

#define MAX_SETS 4

struct panvk_set_collection_set_info {
   unsigned sampler_offset;
   unsigned tex_offset;
   unsigned ubo_offset;
   unsigned dyn_ubo_offset;
   unsigned dyn_ssbo_offset;
   unsigned img_offset;
   unsigned dyn_ssbos_desc_offset;

   unsigned num_ubos;
};

struct panvk_set_collection_layout {
   unsigned set_count;

   unsigned num_samplers;
   unsigned num_textures;
   unsigned num_ubos;
   unsigned num_dyn_ubos;
   unsigned num_dyn_ssbos;
   unsigned num_imgs;

   unsigned dyn_ssbos_desc_index;
   unsigned dyn_ubos_offset;
   unsigned total_ubo_count;

   struct panvk_set_collection_set_info sets[MAX_SETS];
};

void panvk_per_arch(set_collection_layout_fill)(
   struct panvk_set_collection_layout *layout, unsigned set_layout_count,
   struct vk_descriptor_set_layout *const *set_layouts);

#endif
