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
   bool is_partial;
   bool is_missing;
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
   bool is_partial;

   struct panvk_set_collection_set_info sets[MAX_SETS];
};

void panvk_per_arch(set_collection_layout_fill)(
   struct panvk_set_collection_layout *layout, unsigned set_layout_count,
   struct vk_descriptor_set_layout *const *set_layouts);

struct panvk_driver_ubo_set_info {
   unsigned sampler_offset;
   unsigned tex_offset;
   unsigned ubo_offset;
   unsigned img_offset;
   unsigned dyn_ubo_offset;
   unsigned dyn_ssbos_desc_offset;
};

/**
 * Driver UBO.
 *
 * This is used to store any extra descriptor needed by panvk for Bifrost in
 * case of indirection. (with GPL)
 */
struct panvk_driver_ubo {
   unsigned dyn_ssbos_desc_index;
   unsigned num_ubos;

   /* Set 0 layout is always known at compile time */
   struct panvk_driver_ubo_set_info sets[MAX_SETS - 1];
};

#define panvk_driver_ubo_offset(member)                            \
   offsetof(struct panvk_driver_ubo, member)

#define panvk_driver_ubo_set_offset(set_idx, member)                           \
   (panvk_driver_ubo_offset(sets[0].member) +                                  \
    (set_idx) * sizeof(struct panvk_driver_ubo_set_info))
#endif
