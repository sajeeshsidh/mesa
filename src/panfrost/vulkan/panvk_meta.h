/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_META_H
#define PANVK_META_H

#include "panvk_image.h"
#include "panvk_mempool.h"

#include "vk_format.h"
#include "vk_meta.h"

static inline bool
panvk_meta_copy_to_image_use_gfx_pipeline(struct panvk_image *dst_img)
{
   /* Writes to AFBC images must go through the graphics pipeline. */
   if (drm_is_afbc(dst_img->pimage.layout.modifier))
      return true;

   /* We could map depth/stencil images to color images, but vk_image
    * is picky and refuses to do that because in Vulkan depth/stencil
    * layout are supposed to be opaque and can only be copied from/to
    * other depth/stencil images. Let's use a graphics pipeline
    * for those. */
   if (vk_format_is_depth_or_stencil(dst_img->vk.format))
      return true;

   return false;
}

static inline VkFormat
panvk_meta_get_uint_format_for_blk_size(unsigned blk_sz)
{
   switch (blk_sz) {
   case 1:
      return VK_FORMAT_R8_UINT;
   case 2:
      return VK_FORMAT_R16_UINT;
   case 3:
      return VK_FORMAT_R8G8B8_UINT;
   case 4:
      return VK_FORMAT_R32_UINT;
   case 6:
      return VK_FORMAT_R16G16B16_UINT;
   case 8:
      return VK_FORMAT_R32G32_UINT;
   case 12:
      return VK_FORMAT_R32G32B32_UINT;
   case 16:
      return VK_FORMAT_R32G32B32A32_UINT;
   default:
      return VK_FORMAT_UNDEFINED;
   }
}

static inline struct vk_meta_copy_image_properties
panvk_meta_copy_get_image_properties(struct panvk_image *img)
{
   uint64_t mod = img->pimage.layout.modifier;
   enum pipe_format pfmt = vk_format_to_pipe_format(img->vk.format);
   unsigned blk_sz = util_format_get_blocksize(pfmt);
   struct vk_meta_copy_image_properties props = {0};

   if (drm_is_afbc(mod) || vk_format_is_depth_or_stencil(img->vk.format))
      props.view_format = img->vk.format;
   else
      props.view_format = panvk_meta_get_uint_format_for_blk_size(blk_sz);

   if (mod == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED ||
       drm_is_afbc(mod)) {
      props.tile_size.width = 16;
      props.tile_size.height = 16;
      props.tile_size.depth = 1;
   } else {
      /* When linear, pretend we have a 1D-tile so we end up with a <64,1,1>
       * workgroup. */
      props.tile_size.width = 64;
      props.tile_size.height = 1;
      props.tile_size.depth = 1;
   }

   return props;
}

#endif
