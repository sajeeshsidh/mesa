/*
 * Copyright © 2021 Collabora Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#include "genxml/gen_macros.h"

#include "vk_log.h"

#include "panvk_descriptor_set.h"
#include "panvk_device.h"
#include "panvk_entrypoints.h"
#include "panvk_macros.h"
#include "panvk_pipeline_layout.h"
#include "panvk_sampler.h"
#include "panvk_shader.h"

#include "util/mesa-sha1.h"

/*
 * Pipeline layouts.  These have nothing to do with the pipeline.  They are
 * just multiple descriptor set layouts pasted together.
 */

VKAPI_ATTR VkResult VKAPI_CALL
panvk_per_arch(CreatePipelineLayout)(
   VkDevice _device, const VkPipelineLayoutCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator, VkPipelineLayout *pPipelineLayout)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_pipeline_layout *layout;
   struct mesa_sha1 ctx;

   layout =
      vk_pipeline_layout_zalloc(&device->vk, sizeof(*layout), pCreateInfo);
   if (layout == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   _mesa_sha1_init(&ctx);
   panvk_per_arch(set_collection_layout_fill)(
      &layout->set_layout, layout->vk.set_count, layout->vk.set_layouts);
   panvk_per_arch(set_collection_layout_hash_state)(
      &layout->set_layout, layout->vk.set_layouts, &ctx);
   _mesa_sha1_final(&ctx, layout->sha1);

   *pPipelineLayout = panvk_pipeline_layout_to_handle(layout);
   return VK_SUCCESS;
}
