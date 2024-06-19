/*
 * Copyright © 2023 Collabora Ltd.
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "nir/nir_builder.h"
#include "nir/nir_format_convert.h"

#include "vk_buffer.h"
#include "vk_command_buffer.h"
#include "vk_command_pool.h"
#include "vk_device.h"
#include "vk_format.h"
#include "vk_meta.h"
#include "vk_meta_private.h"
#include "vk_physical_device.h"
#include "vk_pipeline.h"

#include "util/format/u_format.h"

struct vk_meta_fill_buffer_key {
   enum vk_meta_object_key_type key_type;
};

struct vk_meta_copy_buffer_key {
   enum vk_meta_object_key_type key_type;

   uint32_t chunk_size;
};

struct vk_meta_copy_buffer_image_key {
   enum vk_meta_object_key_type key_type;

   bool use_gfx_pipeline;

   struct {
      struct {
         VkImageViewType type;
         VkFormat format;
      } view;

      VkImageAspectFlagBits aspect;
   } img;

   uint32_t wg_size[3];
};

struct vk_meta_copy_image_key {
   enum vk_meta_object_key_type key_type;

   bool use_gfx_pipeline;

   struct {
      struct {
         VkImageViewType type;
         VkFormat format;
      } view;
   } src, dst;

   VkSampleCountFlagBits samples;

   uint32_t wg_size[3];
};

#define load_info(__b, __type, __field_name)                                   \
   nir_load_push_constant((__b), 1,                                            \
                          sizeof(((__type *)NULL)->__field_name) * 8,          \
                          nir_imm_int(&b, offsetof(__type, __field_name)))

struct vk_meta_fill_buffer_info {
   uint64_t buf_addr;
   uint32_t data;
   uint32_t size;
};

struct vk_meta_copy_buffer_info {
   uint64_t src_addr;
   uint64_t dst_addr;
   uint32_t size;
};

struct vk_meta_copy_buffer_image_info {
   struct {
      uint64_t addr;
      uint32_t row_stride;
      uint32_t image_stride;
   } buf;

   struct {
      struct {
         uint32_t x, y, z;
      } offset;
   } img;

   /* Workgroup size should be selected based on the image tile size. This
    * means we can issue threads outside the image area we want to copy
    * from/to. This field encodes the copy IDs that should be skipped, and
    * also serve as an adjustment for the buffer/image coordinates. */
   struct {
      struct {
         uint32_t x, y, z;
      } start, end;
   } copy_id_range;
};

struct vk_meta_copy_image_fs_info {
   struct {
      int32_t x, y, z;
   } dst_to_src_offs;
};

struct vk_meta_copy_image_cs_info {
   struct {
      struct {
         uint32_t x, y, z;
      } offset;
   } src_img, dst_img;

   /* Workgroup size should be selected based on the image tile size. This
    * means we can issue threads outside the image area we want to copy
    * from/to. This field encodes the copy IDs that should be skipped, and
    * also serve as an adjustment for the buffer/image coordinates. */
   struct {
      struct {
         uint32_t x, y, z;
      } start, end;
   } copy_id_range;
};

#define COPY_SHADER_BINDING(__binding, __type, __stage)                        \
   {                                                                           \
      .binding = __binding, .descriptorCount = 1,                              \
      .descriptorType = VK_DESCRIPTOR_TYPE_##__type,                           \
      .stageFlags = VK_SHADER_STAGE_##__stage##_BIT,                           \
   }

static VkResult
get_copy_pipeline_layout(struct vk_device *device, struct vk_meta_device *meta,
                         const char *key, VkShaderStageFlagBits shader_stage,
                         size_t push_const_size,
                         const struct VkDescriptorSetLayoutBinding *bindings,
                         uint32_t binding_count, VkPipelineLayout *layout_out)
{
   const VkDescriptorSetLayoutCreateInfo set_layout = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
      .bindingCount = binding_count,
      .pBindings = bindings,
   };

   const VkPushConstantRange push_range = {
      .stageFlags = shader_stage,
      .offset = 0,
      .size = push_const_size,
   };

   return vk_meta_get_pipeline_layout(device, meta, &set_layout, &push_range,
                                      key, strlen(key) + 1, layout_out);
}

#define COPY_PUSH_SET_IMG_DESC(__binding, __type, __iview, __layout)           \
   {                                                                           \
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,                         \
      .dstBinding = __binding,                                                 \
      .descriptorType = VK_DESCRIPTOR_TYPE_##__type##_IMAGE,                   \
      .descriptorCount = 1,                                                    \
      .pImageInfo = &(VkDescriptorImageInfo){                                  \
         .imageView = __iview,                                                 \
         .imageLayout = __layout,                                              \
      },                                                                       \
   }

#define COPY_PUSH_SET_BUF_DESC(__binding, __buffer, __offset, __range)         \
   {                                                                           \
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,                         \
      .dstBinding = __binding,                                                 \
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,                     \
      .descriptorCount = 1,                                                    \
      .pBufferInfo = &(VkDescriptorBufferInfo){                                \
         .buffer = __buffer,                                                   \
         .offset = __offset,                                                   \
         .range = __range,                                                     \
      },                                                                       \
   }

static VkResult
get_gfx_copy_pipeline(struct vk_device *device, struct vk_meta_device *meta,
                      VkPipelineLayout layout,
                      nir_shader *(*build_nir)(const struct vk_meta_device *,
                                               const void *),
                      VkFormat dst_iview_format, const void *key_data,
                      size_t key_size, VkPipeline *pipeline_out)
{
   VkPipeline from_cache = vk_meta_lookup_pipeline(meta, key_data, key_size);
   if (from_cache != VK_NULL_HANDLE) {
      *pipeline_out = from_cache;
      return VK_SUCCESS;
   }

   VkImageAspectFlags aspects = vk_format_aspects(dst_iview_format);
   const VkPipelineShaderStageNirCreateInfoMESA fs_nir_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_NIR_CREATE_INFO_MESA,
      .nir = build_nir(meta, key_data),
   };
   const VkPipelineShaderStageCreateInfo fs_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = &fs_nir_info,
      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .pName = "main",
   };

   VkPipelineDepthStencilStateCreateInfo ds_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
   };
   VkPipelineDynamicStateCreateInfo dyn_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
   };
   struct vk_meta_rendering_info render = {
      .samples = 1,
   };

   const VkGraphicsPipelineCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 1,
      .pStages = &fs_info,
      .pDepthStencilState = &ds_info,
      .pDynamicState = &dyn_info,
      .layout = layout,
   };

   if (aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
      render.color_attachment_count = 1;
      render.color_attachment_formats[0] = dst_iview_format;
   }

   if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
      ds_info.depthTestEnable = VK_TRUE;
      ds_info.depthWriteEnable = VK_TRUE;
      ds_info.depthCompareOp = VK_COMPARE_OP_ALWAYS;
      render.depth_attachment_format = dst_iview_format;
   }

   if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
      /* FIXME: Implement stencil_as_discard */
      assert(meta->use_stencil_export);

      ds_info.stencilTestEnable = VK_TRUE;
      ds_info.front.compareOp = VK_COMPARE_OP_ALWAYS;
      ds_info.front.passOp = VK_STENCIL_OP_REPLACE;
      ds_info.front.compareMask = ~0u;
      ds_info.front.writeMask = ~0u;
      ds_info.front.reference = ~0;
      ds_info.back = ds_info.front;
      render.stencil_attachment_format = dst_iview_format;
   }

   VkResult result = vk_meta_create_graphics_pipeline(
      device, meta, &info, &render, key_data, key_size, pipeline_out);

   ralloc_free(fs_nir_info.nir);

   return result;
}

static VkResult
get_compute_copy_pipeline(
   struct vk_device *device, struct vk_meta_device *meta,
   VkPipelineLayout layout,
   nir_shader *(*build_nir)(const struct vk_meta_device *, const void *),
   const void *key_data, size_t key_size, VkPipeline *pipeline_out)
{
   VkPipeline from_cache = vk_meta_lookup_pipeline(meta, key_data, key_size);
   if (from_cache != VK_NULL_HANDLE) {
      *pipeline_out = from_cache;
      return VK_SUCCESS;
   }

   const VkPipelineShaderStageNirCreateInfoMESA cs_nir_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_NIR_CREATE_INFO_MESA,
      .nir = build_nir(meta, key_data),
   };

   const VkComputePipelineCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage =
         {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &cs_nir_info,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .pName = "main",
         },
      .layout = layout,
   };

   VkResult result = vk_meta_create_compute_pipeline(
      device, meta, &info, key_data, key_size, pipeline_out);

   ralloc_free(cs_nir_info.nir);

   return result;
}

static VkResult
copy_create_src_image_view(struct vk_command_buffer *cmd,
                           struct vk_meta_device *meta, struct vk_image *img,
                           VkFormat format, VkImageAspectFlags aspect,
                           const VkImageSubresourceLayers *subres,
                           VkImageView *view_out)
{
   const VkImageViewUsageCreateInfo usage = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
   };
   const VkImageViewCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .pNext = &usage,
      .image = vk_image_to_handle(img),
      .viewType = vk_image_sampled_view_type(img),
      .format = format,
      .subresourceRange =
         {
            .aspectMask = aspect,
            .baseMipLevel = subres->mipLevel,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = img->array_layers,
         },
   };

   return vk_meta_create_image_view(cmd, meta, &info, view_out);
}

static VkResult
copy_create_dst_image_view(struct vk_command_buffer *cmd,
                           struct vk_meta_device *meta, struct vk_image *img,
                           VkFormat format, VkImageAspectFlags aspects,
                           const VkOffset3D *offset, const VkExtent3D *extent,
                           const VkImageSubresourceLayers *subres,
                           bool use_gfx_pipeline, VkImageView *view_out)
{
   uint32_t layer_count, base_layer;
   const VkImageViewUsageCreateInfo usage = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
      .usage = !use_gfx_pipeline ? VK_IMAGE_USAGE_STORAGE_BIT
               : aspects & VK_IMAGE_ASPECT_COLOR_BIT
                  ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                  : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
   };

   if (use_gfx_pipeline) {
      layer_count =
         MAX2(extent->depth, vk_image_subresource_layer_count(img, subres));
      base_layer = img->image_type == VK_IMAGE_TYPE_3D ? offset->z
                                                       : subres->baseArrayLayer;
   } else {
      /* Always create a view covering the whole image in case of compute. */
      layer_count = img->image_type == VK_IMAGE_TYPE_3D ? 1 : img->array_layers;
      base_layer = 0;
   }

   const VkImageViewCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .pNext = &usage,
      .image = vk_image_to_handle(img),
      .viewType = use_gfx_pipeline ? vk_image_render_view_type(img, layer_count)
                                   : vk_image_storage_view_type(img),
      .format = format,
      .subresourceRange =
         {
            .aspectMask = aspects,
            .baseMipLevel = subres->mipLevel,
            .levelCount = 1,
            .baseArrayLayer = base_layer,
            .layerCount = layer_count,
         },
   };

   return vk_meta_create_image_view(cmd, meta, &info, view_out);
}

static nir_deref_instr *
ssbo_blk_deref(nir_builder *b, const struct vk_meta_device *meta,
               unsigned blk_bit_sz, unsigned blk_num_comps, unsigned binding,
               unsigned idx, nir_def *byte_offset, nir_def *blk_idx)
{
   assert(util_is_power_of_two_nonzero(blk_bit_sz) && blk_bit_sz <= 64);
   assert(blk_num_comps <= NIR_MAX_VEC_COMPONENTS);

   unsigned res_addr_num_comps =
      nir_address_format_num_components(meta->buffer_access.ssbo_addr_format);
   unsigned res_addr_bit_sz =
      nir_address_format_bit_size(meta->buffer_access.ssbo_addr_format);
   const glsl_type *deref_type =
      blk_bit_sz == 8    ? glsl_u8vec_type(blk_num_comps)
      : blk_bit_sz == 16 ? glsl_u16vec_type(blk_num_comps)
      : blk_bit_sz == 32 ? glsl_uvec_type(blk_num_comps)
                         : glsl_u64vec_type(blk_num_comps);
   deref_type = glsl_array_type(deref_type, 0, blk_bit_sz * blk_num_comps / 8);

   nir_def *res = nir_vulkan_resource_index(
      b, res_addr_num_comps, res_addr_bit_sz, nir_imm_int(b, idx),
      .desc_set = 0, .binding = binding,
      .desc_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
   nir_def *desc = nir_load_vulkan_descriptor(
      b, res_addr_num_comps, res_addr_bit_sz, res,
      .desc_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

   nir_def *ptr = desc;

   if (byte_offset) {
      nir_deref_instr *desc_deref =
         nir_build_deref_cast(b, desc, nir_var_mem_ssbo,
                              glsl_array_type(glsl_u8vec_type(1), 0, 1), 1);
      ptr = &nir_build_deref_array(b, desc_deref, byte_offset)->def;
   }

   nir_deref_instr *array = nir_build_deref_cast(
      b, ptr, nir_var_mem_ssbo, deref_type, (blk_bit_sz / 8) * blk_num_comps);

   return nir_build_deref_array(b, array, blk_idx);
}

static nir_shader *
build_fill_buffer_shader(const struct vk_meta_device *meta,
                         UNUSED const void *key_data)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, NULL,
                                                  "vk-meta-fill-buffer");

   b.shader->info.workgroup_size[0] =
      DIV_ROUND_UP(meta->buffer_access.optimal_size_per_wg ?: 64, 4);
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;

   nir_def *global_id = nir_load_global_invocation_id(&b, 32);
   nir_def *copy_id = nir_channel(&b, global_id, 0);
   nir_def *offset = nir_imul_imm(&b, copy_id, 4);
   nir_def *size = load_info(&b, struct vk_meta_fill_buffer_info, size);
   nir_def *data = load_info(&b, struct vk_meta_fill_buffer_info, data);

   nir_push_if(&b, nir_ult(&b, offset, size));

   if (meta->buffer_access.use_global_address) {
      offset = nir_u2u64(&b, offset);

      nir_def *buf_addr =
         load_info(&b, struct vk_meta_fill_buffer_info, buf_addr);

      nir_build_store_global(&b, data, nir_iadd(&b, buf_addr, offset),
                             .align_mul = 4);
   } else {
      nir_deref_instr *buf_deref =
         ssbo_blk_deref(&b, meta, 32, 1, 0, 0, NULL, copy_id);

      nir_store_deref_with_access(&b, buf_deref, data, 1, ACCESS_NON_READABLE);
   }

   nir_pop_if(&b, NULL);

   return b.shader;
}

static nir_shader *
build_copy_buffer_shader(const struct vk_meta_device *meta,
                         const void *key_data)
{
   const struct vk_meta_copy_buffer_key *key = key_data;
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, NULL,
                                                  "vk-meta-copy-buffer");

   b.shader->info.workgroup_size[0] = DIV_ROUND_UP(
      meta->buffer_access.optimal_size_per_wg ?: 64, key->chunk_size);
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;

   uint32_t chunk_bit_size, chunk_comp_count;

   if (key->chunk_size & 1) {
      chunk_bit_size = 8;
      chunk_comp_count = key->chunk_size;
   } else if (key->chunk_size & 2) {
      chunk_bit_size = 16;
      chunk_comp_count = key->chunk_size / 2;
   } else {
      chunk_bit_size = 32;
      chunk_comp_count = key->chunk_size / 4;
   }

   assert(chunk_comp_count < NIR_MAX_VEC_COMPONENTS);

   nir_def *global_id = nir_load_global_invocation_id(&b, 32);
   nir_def *copy_id = nir_channel(&b, global_id, 0);
   nir_def *offset = nir_imul_imm(&b, copy_id, key->chunk_size);
   nir_def *size = load_info(&b, struct vk_meta_copy_buffer_info, size);

   nir_push_if(&b, nir_ult(&b, offset, size));

   if (meta->buffer_access.use_global_address) {
      offset = nir_u2u64(&b, offset);

      nir_def *src_addr =
         load_info(&b, struct vk_meta_copy_buffer_info, src_addr);
      nir_def *dst_addr = nir_load_push_constant(&b, 1, 64, nir_imm_int(&b, 8));
      nir_def *data = nir_build_load_global(
         &b, chunk_comp_count, chunk_bit_size, nir_iadd(&b, src_addr, offset),
         .align_mul = chunk_bit_size / 8);

      nir_build_store_global(&b, data, nir_iadd(&b, dst_addr, offset),
                             .align_mul = chunk_bit_size / 8);
   } else {
      nir_deref_instr *src_deref = ssbo_blk_deref(
         &b, meta, chunk_bit_size, chunk_comp_count, 0, 0, NULL, copy_id);
      nir_deref_instr *dst_deref = ssbo_blk_deref(
         &b, meta, chunk_bit_size, chunk_comp_count, 1, 0, NULL, copy_id);

      nir_copy_deref_with_access(&b, dst_deref, src_deref, ACCESS_NON_READABLE,
                                 ACCESS_NON_WRITEABLE);
   }

   nir_pop_if(&b, NULL);

   return b.shader;
}

static VkResult
get_copy_buffer_pipeline(struct vk_device *device, struct vk_meta_device *meta,
                         const struct vk_meta_copy_buffer_key *key,
                         VkPipelineLayout *layout_out, VkPipeline *pipeline_out)
{
   const VkDescriptorSetLayoutBinding bindings[] = {
      COPY_SHADER_BINDING(0, STORAGE_BUFFER, COMPUTE),
      COPY_SHADER_BINDING(1, STORAGE_BUFFER, COMPUTE),
   };

   VkResult result = get_copy_pipeline_layout(
      device, meta, "vk-meta-copy-buffer-pipeline-layout",
      VK_SHADER_STAGE_COMPUTE_BIT, sizeof(struct vk_meta_copy_buffer_info),
      bindings, ARRAY_SIZE(bindings), layout_out);

   if (unlikely(result != VK_SUCCESS))
      return result;

   return get_compute_copy_pipeline(device, meta, *layout_out,
                                    build_copy_buffer_shader, key, sizeof(*key),
                                    pipeline_out);
}

static void
copy_buffer_region(struct vk_command_buffer *cmd, struct vk_meta_device *meta,
                   VkBuffer src, VkBuffer dst, const VkBufferCopy2 *region)
{
   struct vk_device *dev = cmd->base.device;
   const struct vk_physical_device *pdev = dev->physical;
   const struct vk_device_dispatch_table *disp = &dev->dispatch_table;
   VkResult result;

   struct vk_meta_copy_buffer_key key = {
      .key_type = VK_META_OBJECT_KEY_COPY_BUFFER_PIPELINE,
   };

   VkDeviceAddress src_addr = 0, dst_addr = 0;
   VkDeviceSize size = region->size;
   uint64_t align = 0;

   if (meta->buffer_access.use_global_address) {
      src_addr = vk_meta_buffer_address(dev, src) + region->srcOffset;
      dst_addr = vk_meta_buffer_address(dev, dst) + region->dstOffset;

      /* Combine the size and src/dst address to extract the alignment. */
      align = src_addr | dst_addr | size;
   } else {
      VkMemoryRequirements src_reqs, dst_reqs;

      disp->GetBufferMemoryRequirements(vk_device_to_handle(dev), src,
                                        &src_reqs);
      disp->GetBufferMemoryRequirements(vk_device_to_handle(dev), dst,
                                        &dst_reqs);

      /* Combine the size, src/dst offset and src/dst buffer aligment
       * requirement to guess the aligment. It's a worst case
       * estimation as the buffer address might have a bigger
       * alignment but we can't know it without calling
       * GetBufferDeviceAddress(), and if the driver didn't set
       * use_global_address, it might mean the buffer_address
       * extension is not supported. */
      align = src_reqs.alignment | dst_reqs.alignment | region->srcOffset |
              region->dstOffset | size;
   }

   assert(align != 0);

   /* Pick the first power-of-two of the combined src/dst address and
    * size as our alignment. We limit the chunk size to 16 bytes
    * (a uvec4) for now. */
   key.chunk_size = MIN2(16, 1 << (ffs(align) - 1));

   VkPipelineLayout pipeline_layout;
   VkPipeline pipeline;
   result =
      get_copy_buffer_pipeline(dev, meta, &key, &pipeline_layout, &pipeline);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(cmd, result);
      return;
   }

   disp->CmdBindPipeline(vk_command_buffer_to_handle(cmd),
                         VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   VkDeviceSize src_offset = region->srcOffset, dst_offset = region->dstOffset;

   const uint32_t optimal_wg_size =
      DIV_ROUND_UP(meta->buffer_access.optimal_size_per_wg, key.chunk_size);
   const uint32_t per_wg_copy_size = optimal_wg_size * key.chunk_size;
   uint32_t max_per_dispatch_size =
      pdev->properties.maxComputeWorkGroupCount[0] * per_wg_copy_size;

   assert(optimal_wg_size <= pdev->properties.maxComputeWorkGroupSize[0]);

   while (size) {
      struct vk_meta_copy_buffer_info args = {
         .size = MIN2(size, max_per_dispatch_size),
         .src_addr = src_addr,
         .dst_addr = dst_addr,
      };
      uint32_t wg_count = DIV_ROUND_UP(args.size, per_wg_copy_size);

      disp->CmdPushConstants(vk_command_buffer_to_handle(cmd), pipeline_layout,
                             VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args),
                             &args);

      if (meta->buffer_access.use_global_address) {
         src_addr += args.size;
         dst_addr += args.size;
      } else {
         const VkWriteDescriptorSet descs[] = {
            COPY_PUSH_SET_BUF_DESC(0, src, src_offset, args.size),
            COPY_PUSH_SET_BUF_DESC(1, dst, dst_offset, args.size),
         };

         disp->CmdPushDescriptorSetKHR(
            vk_command_buffer_to_handle(cmd), VK_PIPELINE_BIND_POINT_COMPUTE,
            pipeline_layout, 0, ARRAY_SIZE(descs), descs);

         src_offset += args.size;
         dst_offset += args.size;
      }

      disp->CmdDispatch(vk_command_buffer_to_handle(cmd), wg_count, 1, 1);

      size -= args.size;
   }
}

void
vk_meta_copy_buffer(struct vk_command_buffer *cmd, struct vk_meta_device *meta,
                    const VkCopyBufferInfo2 *info)
{
   for (unsigned i = 0; i < info->regionCount; i++) {
      const VkBufferCopy2 *region = &info->pRegions[i];

      copy_buffer_region(cmd, meta, info->srcBuffer, info->dstBuffer, region);
   }
}

static nir_def *
trim_img_coords(nir_builder *b, VkImageViewType view_type, nir_def *coords)
{
   switch (view_type) {
   case VK_IMAGE_VIEW_TYPE_1D:
      return nir_channel(b, coords, 0);

   case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
   case VK_IMAGE_VIEW_TYPE_2D:
      return nir_trim_vector(b, coords, 2);

   default:
      return nir_trim_vector(b, coords, 3);
   }
}

static nir_def *
coords_to_buf_offset(nir_builder *b, enum pipe_format pfmt, nir_def *coords,
                     nir_def *buf_row_stride, nir_def *buf_img_stride)
{
   unsigned blk_sz = util_format_get_blocksize(pfmt);
   nir_def *offset = nir_imul(b, nir_channel(b, coords, 2), buf_img_stride);
   offset = nir_iadd(b, offset,
                     nir_imul(b, nir_channel(b, coords, 1), buf_row_stride));
   return nir_iadd(b, offset,
                   nir_imul_imm(b, nir_channel(b, coords, 0), blk_sz));
}

static nir_shader *
build_buffer_to_image_cs(const struct vk_meta_device *meta,
                         const void *key_data)
{
   const struct vk_meta_copy_buffer_image_key *key = key_data;

   assert(!key->use_gfx_pipeline);

   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, NULL, "vk-meta-copy-buffer-to-image-compute");

   assert(key->wg_size[0] > 0 && key->wg_size[1] > 0 && key->wg_size[2] > 0);

   b.shader->info.workgroup_size[0] = key->wg_size[0];
   b.shader->info.workgroup_size[1] = key->wg_size[1];
   b.shader->info.workgroup_size[2] = key->wg_size[2];

   VkFormat buf_fmt = key->img.aspect == VK_IMAGE_ASPECT_DEPTH_BIT
                         ? vk_format_depth_only(key->img.view.format)
                      : key->img.aspect == VK_IMAGE_ASPECT_STENCIL_BIT
                         ? vk_format_stencil_only(key->img.view.format)
                         : key->img.view.format;

   enum pipe_format img_pfmt = vk_format_to_pipe_format(key->img.view.format);
   enum pipe_format buf_pfmt = vk_format_to_pipe_format(buf_fmt);
   enum glsl_base_type base_type =
      util_format_is_pure_sint(img_pfmt)   ? GLSL_TYPE_INT
      : util_format_is_pure_uint(img_pfmt) ? GLSL_TYPE_UINT
                                           : GLSL_TYPE_FLOAT;
   enum glsl_sampler_dim sampler_dim =
      vk_image_view_type_to_sampler_dim(key->img.view.type);
   bool is_array = vk_image_view_type_is_array(key->img.view.type);
   const struct glsl_type *image_type =
      glsl_image_type(sampler_dim, is_array, base_type);
   nir_variable *image_var =
      nir_variable_create(b.shader, nir_var_uniform, image_type, NULL);
   image_var->data.descriptor_set = 0;
   image_var->data.binding = 1;
   nir_deref_instr *image_deref = nir_build_deref_var(&b, image_var);

   nir_def *copy_id = nir_load_global_invocation_id(&b, 32);
   nir_def *copy_id_start =
      nir_vec3(&b,
               load_info(&b, struct vk_meta_copy_buffer_image_info,
                         copy_id_range.start.x),
               load_info(&b, struct vk_meta_copy_buffer_image_info,
                         copy_id_range.start.y),
               load_info(&b, struct vk_meta_copy_buffer_image_info,
                         copy_id_range.start.z));
   nir_def *copy_id_end = nir_vec3(
      &b,
      load_info(&b, struct vk_meta_copy_buffer_image_info, copy_id_range.end.x),
      load_info(&b, struct vk_meta_copy_buffer_image_info, copy_id_range.end.y),
      load_info(&b, struct vk_meta_copy_buffer_image_info,
                copy_id_range.end.z));

   nir_def *in_bounds =
      nir_iand(&b, nir_ball(&b, nir_uge(&b, copy_id, copy_id_start)),
               nir_ball(&b, nir_ult(&b, copy_id, copy_id_end)));

   nir_push_if(&b, in_bounds);

   /* Adjust the copy ID such that we can directly deduce the image coords and
    * buffer offset from it. */
   copy_id = nir_isub(&b, copy_id, copy_id_start);

   nir_def *buf_row_stride =
      load_info(&b, struct vk_meta_copy_buffer_image_info, buf.row_stride);
   nir_def *buf_img_stride =
      load_info(&b, struct vk_meta_copy_buffer_image_info, buf.image_stride);
   nir_def *img_offs = nir_vec3(
      &b, load_info(&b, struct vk_meta_copy_buffer_image_info, img.offset.x),
      load_info(&b, struct vk_meta_copy_buffer_image_info, img.offset.y),
      load_info(&b, struct vk_meta_copy_buffer_image_info, img.offset.z));

   nir_def *img_coords =
      trim_img_coords(&b, key->img.view.type, nir_iadd(&b, copy_id, img_offs));

   img_coords = nir_pad_vector_imm_int(&b, img_coords, 0, 4);

   unsigned blk_sz = util_format_get_blocksize(buf_pfmt);
   unsigned bit_sz = blk_sz & 1 ? 8 : blk_sz & 2 ? 16 : 32;
   unsigned comp_count = blk_sz * 8 / bit_sz;

   nir_def *buf_offset = coords_to_buf_offset(&b, buf_pfmt, copy_id,
                                              buf_row_stride, buf_img_stride);
   nir_def *packed;

   if (meta->buffer_access.use_global_address) {
      nir_def *buf_addr =
         load_info(&b, struct vk_meta_copy_buffer_image_info, buf.addr);

      packed = nir_build_load_global(
         &b, comp_count, bit_sz,
         nir_iadd(&b, buf_addr, nir_u2u64(&b, buf_offset)),
         .align_mul = bit_sz / 8);
   } else {
      nir_deref_instr *buf_deref = ssbo_blk_deref(
         &b, meta, bit_sz, comp_count, 0, 0, buf_offset, nir_imm_int(&b, 0));

      packed = nir_load_deref_with_access(&b, buf_deref, ACCESS_NON_WRITEABLE);
   }

   nir_def *unpacked = NULL;

   /* We don't do compressed formats. The driver should select a non-compressed
    * format with the same block size. */
   assert(!util_format_is_compressed(buf_pfmt));

   switch (key->img.aspect) {
   case VK_IMAGE_ASPECT_COLOR_BIT:
      /* FIXME: We need special converters YUV formats. */
      assert(!util_format_is_yuv(buf_pfmt));
      unpacked = nir_format_unpack_rgba(&b, packed, buf_pfmt);
      break;

   case VK_IMAGE_ASPECT_DEPTH_BIT:
   case VK_IMAGE_ASPECT_STENCIL_BIT:
      assert(!"Copy of depth/stencil on compute pipeline not supported");
      break;

   default:
      assert(!"Unsupported aspect");
      break;
   }

   nir_image_deref_store(
      &b, &image_deref->def, img_coords, nir_imm_int(&b, 0), // Sample
      unpacked, nir_imm_int(&b, 0),                          // LOD
      .image_dim = sampler_dim, .image_array = is_array,
      .src_type = nir_get_nir_type_for_glsl_base_type(base_type),
      .format = img_pfmt, .access = ACCESS_NON_READABLE);

   nir_pop_if(&b, NULL);

   return b.shader;
}

static nir_shader *
build_buffer_to_image_fs(const struct vk_meta_device *meta,
                         const void *key_data)
{
   const struct vk_meta_copy_buffer_image_key *key = key_data;

   assert(key->use_gfx_pipeline);

   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, NULL, "vk-meta-copy-buffer-to-image-frag");

   VkFormat buf_fmt = key->img.aspect == VK_IMAGE_ASPECT_DEPTH_BIT
                         ? vk_format_depth_only(key->img.view.format)
                      : key->img.aspect == VK_IMAGE_ASPECT_STENCIL_BIT
                         ? vk_format_stencil_only(key->img.view.format)
                         : key->img.view.format;

   enum pipe_format buf_pfmt = vk_format_to_pipe_format(buf_fmt);
   nir_def *out_coord_xy = nir_f2u32(&b, nir_load_frag_coord(&b));
   nir_def *out_layer = nir_load_layer_id(&b);

   nir_def *buf_row_stride =
      load_info(&b, struct vk_meta_copy_buffer_image_info, buf.row_stride);
   nir_def *buf_img_stride =
      load_info(&b, struct vk_meta_copy_buffer_image_info, buf.image_stride);
   nir_def *img_offs = nir_vec3(
      &b, load_info(&b, struct vk_meta_copy_buffer_image_info, img.offset.x),
      load_info(&b, struct vk_meta_copy_buffer_image_info, img.offset.y),
      load_info(&b, struct vk_meta_copy_buffer_image_info, img.offset.z));

   /* Move the layer ID to the second coordinate if we're dealing with a 1D
    * array, as this is where the texture instruction expects it. */
   nir_def *coords = key->img.view.type == VK_IMAGE_VIEW_TYPE_1D_ARRAY
                        ? nir_vec3(&b, nir_channel(&b, out_coord_xy, 0),
                                   out_layer, nir_imm_int(&b, 0))
                        : nir_vec3(&b, nir_channel(&b, out_coord_xy, 0),
                                   nir_channel(&b, out_coord_xy, 1), out_layer);

   unsigned blk_sz = util_format_get_blocksize(buf_pfmt);
   unsigned bit_sz = blk_sz & 1 ? 8 : blk_sz & 2 ? 16 : 32;
   unsigned comp_count = blk_sz * 8 / bit_sz;

   coords = nir_isub(&b, coords, img_offs);

   nir_def *buf_offset = coords_to_buf_offset(&b, buf_pfmt, coords,
                                              buf_row_stride, buf_img_stride);

   nir_def *packed;

   if (meta->buffer_access.use_global_address) {
      nir_def *buf_addr =
         load_info(&b, struct vk_meta_copy_buffer_image_info, buf.addr);

      packed = nir_build_load_global(
         &b, comp_count, bit_sz,
         nir_iadd(&b, buf_addr, nir_u2u64(&b, buf_offset)),
         .align_mul = blk_sz);
   } else {
      nir_deref_instr *buf_deref = ssbo_blk_deref(
         &b, meta, bit_sz, comp_count, 0, 0, buf_offset, nir_imm_int(&b, 0));

      packed = nir_load_deref_with_access(&b, buf_deref, ACCESS_NON_WRITEABLE);
   }

   unsigned out_location, out_comps;
   const char *out_name;

   /* We don't do compressed formats. The driver should select a non-compressed
    * format with the same block size. */
   assert(!util_format_is_compressed(buf_pfmt));

   enum glsl_base_type base_type;
   nir_def *unpacked = NULL;

   switch (key->img.aspect) {
   case VK_IMAGE_ASPECT_COLOR_BIT:
      /* FIXME: We need special converters YUV formats. */
      assert(!util_format_is_yuv(buf_pfmt));
      unpacked = nir_format_unpack_rgba(&b, packed, buf_pfmt);
      base_type = util_format_is_pure_sint(buf_pfmt)   ? GLSL_TYPE_INT
                  : util_format_is_pure_uint(buf_pfmt) ? GLSL_TYPE_UINT
                                                       : GLSL_TYPE_FLOAT;
      out_name = "gl_FragData[0]";
      out_location = FRAG_RESULT_DATA0;
      out_comps = 4;
      break;

   case VK_IMAGE_ASPECT_DEPTH_BIT:
      unpacked = nir_channel(
         &b, nir_format_unpack_depth_stencil(&b, packed, buf_pfmt), 0);
      out_name = "gl_FragDepth";
      out_location = FRAG_RESULT_DEPTH;
      base_type = GLSL_TYPE_FLOAT;
      out_comps = 1;
      break;

   case VK_IMAGE_ASPECT_STENCIL_BIT:
      unpacked = nir_channel(
         &b, nir_format_unpack_depth_stencil(&b, packed, buf_pfmt), 1);
      out_name = "gl_FragStencilRef";
      out_location = FRAG_RESULT_STENCIL;
      base_type = GLSL_TYPE_UINT;
      out_comps = 1;
      break;

   default:
      assert(!"Unsupported aspect");
   }

   const struct glsl_type *out_type = glsl_vector_type(base_type, out_comps);
   nir_variable *out =
      nir_variable_create(b.shader, nir_var_shader_out, out_type, out_name);
   out->data.location = out_location;

   nir_store_var(&b, out, unpacked, BITFIELD_MASK(out_comps));

   return b.shader;
}

static nir_shader *
build_image_to_buffer_shader(const struct vk_meta_device *meta,
                             const void *key_data)
{
   const struct vk_meta_copy_buffer_image_key *key = key_data;

   assert(!key->use_gfx_pipeline);

   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, NULL, "vk-meta-copy-image-to-buffer");

   assert(key->wg_size[0] > 0 && key->wg_size[1] > 0 && key->wg_size[2] > 0);

   b.shader->info.workgroup_size[0] = key->wg_size[0];
   b.shader->info.workgroup_size[1] = key->wg_size[1];
   b.shader->info.workgroup_size[2] = key->wg_size[2];

   VkFormat buf_fmt = key->img.aspect == VK_IMAGE_ASPECT_DEPTH_BIT
                         ? vk_format_depth_only(key->img.view.format)
                      : key->img.aspect == VK_IMAGE_ASPECT_STENCIL_BIT
                         ? vk_format_stencil_only(key->img.view.format)
                         : key->img.view.format;

   enum pipe_format buf_pfmt = vk_format_to_pipe_format(buf_fmt);
   enum glsl_sampler_dim sampler_dim =
      vk_image_view_type_to_sampler_dim(key->img.view.type);
   bool is_array = vk_image_view_type_is_array(key->img.view.type);

   nir_def *copy_id = nir_load_global_invocation_id(&b, 32);
   nir_def *copy_id_start =
      nir_vec3(&b,
               load_info(&b, struct vk_meta_copy_buffer_image_info,
                         copy_id_range.start.x),
               load_info(&b, struct vk_meta_copy_buffer_image_info,
                         copy_id_range.start.y),
               load_info(&b, struct vk_meta_copy_buffer_image_info,
                         copy_id_range.start.z));
   nir_def *copy_id_end = nir_vec3(
      &b,
      load_info(&b, struct vk_meta_copy_buffer_image_info, copy_id_range.end.x),
      load_info(&b, struct vk_meta_copy_buffer_image_info, copy_id_range.end.y),
      load_info(&b, struct vk_meta_copy_buffer_image_info,
                copy_id_range.end.z));

   nir_def *in_bounds =
      nir_iand(&b, nir_ball(&b, nir_uge(&b, copy_id, copy_id_start)),
               nir_ball(&b, nir_ult(&b, copy_id, copy_id_end)));

   nir_push_if(&b, in_bounds);

   copy_id = nir_isub(&b, copy_id, copy_id_start);

   nir_def *buf_row_stride =
      load_info(&b, struct vk_meta_copy_buffer_image_info, buf.row_stride);
   nir_def *buf_img_stride =
      load_info(&b, struct vk_meta_copy_buffer_image_info, buf.image_stride);
   nir_def *img_offs = nir_vec3(
      &b, load_info(&b, struct vk_meta_copy_buffer_image_info, img.offset.x),
      load_info(&b, struct vk_meta_copy_buffer_image_info, img.offset.y),
      load_info(&b, struct vk_meta_copy_buffer_image_info, img.offset.z));

   nir_def *img_coords =
      trim_img_coords(&b, key->img.view.type, nir_iadd(&b, copy_id, img_offs));

   unsigned blk_sz = util_format_get_blocksize(buf_pfmt);
   unsigned bit_sz = blk_sz & 1 ? 8 : blk_sz & 2 ? 16 : 32;
   unsigned comp_count = blk_sz * 8 / bit_sz;

   nir_def *buf_offset = coords_to_buf_offset(&b, buf_pfmt, copy_id,
                                              buf_row_stride, buf_img_stride);

   enum glsl_base_type base_type;

   switch (key->img.aspect) {
   case VK_IMAGE_ASPECT_COLOR_BIT:
      base_type = util_format_is_pure_sint(buf_pfmt)   ? GLSL_TYPE_INT
                  : util_format_is_pure_uint(buf_pfmt) ? GLSL_TYPE_UINT
                                                       : GLSL_TYPE_FLOAT;
      break;

   case VK_IMAGE_ASPECT_DEPTH_BIT:
      base_type = GLSL_TYPE_FLOAT;
      break;

   case VK_IMAGE_ASPECT_STENCIL_BIT:
      base_type = GLSL_TYPE_UINT;
      break;

   default:
      base_type = GLSL_TYPE_VOID;
      assert(!"Unsupported aspect");
      break;
   }
   const struct glsl_type *texture_type =
      glsl_sampler_type(sampler_dim, false, is_array, base_type);
   nir_variable *texture =
      nir_variable_create(b.shader, nir_var_uniform, texture_type, "tex");
   texture->data.descriptor_set = 0;
   texture->data.binding = 1;

   nir_deref_instr *tex_deref = nir_build_deref_var(&b, texture);

   nir_def *unpacked = nir_txf_deref(&b, tex_deref, img_coords, NULL);

   nir_def *packed = NULL;

   switch (key->img.aspect) {
   case VK_IMAGE_ASPECT_DEPTH_BIT:
      packed = nir_format_pack_depth_stencil(
         &b, buf_pfmt, nir_channel(&b, unpacked, 0), nir_undef(&b, 1, 32));
      break;

   case VK_IMAGE_ASPECT_STENCIL_BIT:
      packed = nir_format_pack_depth_stencil(&b, buf_pfmt, nir_undef(&b, 1, 32),
                                             nir_channel(&b, unpacked, 0));
      break;

   case VK_IMAGE_ASPECT_COLOR_BIT:
      /* FIXME: We need special converters for YUV formats. */
      assert(!util_format_is_yuv(buf_pfmt));

      packed = nir_format_pack_rgba(&b, buf_pfmt, unpacked);
      break;

   default:
      assert(!"Unsupported aspect");
   }

   if (bit_sz < packed->bit_size)
      packed = nir_unpack_bits(&b, packed, bit_sz);

   packed = nir_trim_vector(&b, packed, comp_count);

   if (meta->buffer_access.use_global_address) {
      nir_def *buf_addr =
         load_info(&b, struct vk_meta_copy_buffer_image_info, buf.addr);

      nir_store_global(&b, nir_iadd(&b, buf_addr, nir_u2u64(&b, buf_offset)),
                       bit_sz / 8, packed, BITFIELD_MASK(comp_count));
   } else {
      nir_deref_instr *buf_deref = ssbo_blk_deref(
         &b, meta, bit_sz, comp_count, 0, 0, buf_offset, nir_imm_int(&b, 0));

      nir_store_deref_with_access(
         &b, buf_deref, packed, BITFIELD_MASK(comp_count), ACCESS_NON_READABLE);
   }

   nir_pop_if(&b, NULL);

   return b.shader;
}

static nir_shader *
build_copy_image_fs(const struct vk_meta_device *meta, const void *key_data)
{
   const struct vk_meta_copy_image_key *key = key_data;

   assert(key->use_gfx_pipeline);

   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, NULL,
                                                  "vk-meta-copy-image-frag");

   b.shader->info.fs.uses_sample_shading =
      key->samples != VK_SAMPLE_COUNT_1_BIT;

   VkImageAspectFlags src_aspects = vk_format_aspects(key->src.view.format);
   enum pipe_format src_pfmt = vk_format_to_pipe_format(key->src.view.format);
   unsigned src_blk_sz = util_format_get_blocksize(src_pfmt);
   ASSERTED VkImageAspectFlags dst_aspects =
      vk_format_aspects(key->dst.view.format);
   enum pipe_format dst_pfmt = vk_format_to_pipe_format(key->dst.view.format);
   unsigned dst_blk_sz = util_format_get_blocksize(dst_pfmt);
   nir_def *out_coord_xy = nir_f2u32(&b, nir_load_frag_coord(&b));
   nir_def *out_layer = nir_load_layer_id(&b);

   /* Image copy can only happen between two formats having the same block size.
    */
   assert(src_blk_sz == dst_blk_sz);

   /* We don't do compressed formats. The driver should select a non-compressed
    * format with the same block size. */
   assert(!util_format_is_compressed(src_pfmt));
   assert(!util_format_is_compressed(dst_pfmt));

   nir_def *src_offset = nir_vec3(
      &b, load_info(&b, struct vk_meta_copy_image_fs_info, dst_to_src_offs.x),
      load_info(&b, struct vk_meta_copy_image_fs_info, dst_to_src_offs.y),
      load_info(&b, struct vk_meta_copy_image_fs_info, dst_to_src_offs.z));

   /* Move the layer ID to the second coordinate if we're dealing with a 1D
    * array, as this is where the texture instruction expects it. */
   nir_def *src_coords =
      key->dst.view.type == VK_IMAGE_VIEW_TYPE_1D_ARRAY
         ? nir_vec3(&b, nir_channel(&b, out_coord_xy, 0), out_layer,
                    nir_imm_int(&b, 0))
         : nir_vec3(&b, nir_channel(&b, out_coord_xy, 0),
                    nir_channel(&b, out_coord_xy, 1), out_layer);

   src_coords = trim_img_coords(&b, key->src.view.type,
                                nir_iadd(&b, src_coords, src_offset));

   bool src_is_array = vk_image_view_type_is_array(key->src.view.type);
   enum glsl_sampler_dim src_sampler_dim =
      vk_image_view_type_to_sampler_dim(key->src.view.type);

   if (key->samples != VK_SAMPLE_COUNT_1_BIT) {
      assert(src_sampler_dim == GLSL_SAMPLER_DIM_2D);
      src_sampler_dim = GLSL_SAMPLER_DIM_MS;
   }

   assert(src_aspects == dst_aspects);
   u_foreach_bit(a, src_aspects) {
      unsigned out_location = ~0, out_comps = 0;
      const char *tex_name = NULL, *out_name = NULL;
      enum glsl_base_type src_base_type = GLSL_TYPE_VOID;
      enum glsl_base_type dst_base_type = GLSL_TYPE_VOID;

      switch (1 << a) {
      case VK_IMAGE_ASPECT_COLOR_BIT: {
         src_base_type = util_format_is_pure_sint(src_pfmt)   ? GLSL_TYPE_INT
                         : util_format_is_pure_uint(src_pfmt) ? GLSL_TYPE_UINT
                                                              : GLSL_TYPE_FLOAT;
         dst_base_type = util_format_is_pure_sint(dst_pfmt)   ? GLSL_TYPE_INT
                         : util_format_is_pure_uint(dst_pfmt) ? GLSL_TYPE_UINT
                                                              : GLSL_TYPE_FLOAT;
         out_name = "gl_FragData[0]";
         out_location = FRAG_RESULT_DATA0;
         out_comps = 4;
         tex_name = "color_tex";
         break;
      }

      case VK_IMAGE_ASPECT_DEPTH_BIT:
         assert(src_pfmt == dst_pfmt);
         out_name = "gl_FragDepth";
         out_location = FRAG_RESULT_DEPTH;
         src_base_type = dst_base_type = GLSL_TYPE_FLOAT;
         out_comps = 1;
         tex_name = "depth_tex";
         break;

      case VK_IMAGE_ASPECT_STENCIL_BIT:
         assert(src_pfmt == dst_pfmt);
         out_name = "gl_FragStencilRef";
         out_location = FRAG_RESULT_STENCIL;
         src_base_type = dst_base_type = GLSL_TYPE_UINT;
         out_comps = 1;
         tex_name = "stencil_tex";
         break;

      default:
         assert("!Unsupported aspect");
         break;
      }

      const struct glsl_type *texture_type =
         glsl_sampler_type(src_sampler_dim, false, src_is_array, src_base_type);
      nir_variable *texture =
         nir_variable_create(b.shader, nir_var_uniform, texture_type, tex_name);
      texture->data.descriptor_set = 0;
      texture->data.binding = a;
      nir_deref_instr *tex_deref = nir_build_deref_var(&b, texture);

      nir_def *val = key->samples == VK_SAMPLE_COUNT_1_BIT
                        ? nir_txf_deref(&b, tex_deref, src_coords, NULL)
                        : nir_txf_ms_deref(&b, tex_deref, src_coords,
                                           nir_load_sample_id(&b));

      /* If the src/dst formats differ, pack+unpack to convert to
       * the expected output format. */
      if (src_pfmt != dst_pfmt) {
         assert(!util_format_is_yuv(src_pfmt));
         assert(!util_format_is_yuv(dst_pfmt));

         val = nir_format_unpack_rgba(
            &b, nir_format_pack_rgba(&b, src_pfmt, val), dst_pfmt);
      }

      const struct glsl_type *out_type =
         glsl_vector_type(dst_base_type, out_comps);
      nir_variable *out =
         nir_variable_create(b.shader, nir_var_shader_out, out_type, out_name);
      out->data.location = out_location;

      nir_store_var(&b, out, nir_trim_vector(&b, val, out_comps),
                    BITFIELD_MASK(out_comps));
   }

   return b.shader;
}

static nir_shader *
build_copy_image_cs(const struct vk_meta_device *meta, const void *key_data)
{
   const struct vk_meta_copy_image_key *key = key_data;

   assert(!key->use_gfx_pipeline);

   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, NULL,
                                                  "vk-meta-copy-image-compute");

   b.shader->info.workgroup_size[0] = key->wg_size[0];
   b.shader->info.workgroup_size[1] = key->wg_size[1];
   b.shader->info.workgroup_size[2] = key->wg_size[2];

   VkImageAspectFlags src_aspects = vk_format_aspects(key->src.view.format);
   enum pipe_format src_pfmt = vk_format_to_pipe_format(key->src.view.format);
   unsigned src_blk_sz = util_format_get_blocksize(src_pfmt);
   ASSERTED VkImageAspectFlags dst_aspects =
      vk_format_aspects(key->dst.view.format);
   enum pipe_format dst_pfmt = vk_format_to_pipe_format(key->dst.view.format);
   unsigned dst_blk_sz = util_format_get_blocksize(dst_pfmt);

   /* Image copy can only happen between two formats having the same block size.
    */
   assert(src_blk_sz == dst_blk_sz);

   /* We don't do compressed formats. The driver should select a non-compressed
    * format with the same block size. */
   assert(!util_format_is_compressed(src_pfmt));
   assert(!util_format_is_compressed(dst_pfmt));

   /* We don't support depth/stencil copies with compute. */
   assert(vk_format_is_color(key->dst.view.format));

   nir_def *copy_id = nir_load_global_invocation_id(&b, 32);
   nir_def *copy_id_start = nir_vec3(
      &b,
      load_info(&b, struct vk_meta_copy_image_cs_info, copy_id_range.start.x),
      load_info(&b, struct vk_meta_copy_image_cs_info, copy_id_range.start.y),
      load_info(&b, struct vk_meta_copy_image_cs_info, copy_id_range.start.z));
   nir_def *copy_id_end = nir_vec3(
      &b, load_info(&b, struct vk_meta_copy_image_cs_info, copy_id_range.end.x),
      load_info(&b, struct vk_meta_copy_image_cs_info, copy_id_range.end.y),
      load_info(&b, struct vk_meta_copy_image_cs_info, copy_id_range.end.z));

   nir_def *in_bounds =
      nir_iand(&b, nir_ball(&b, nir_uge(&b, copy_id, copy_id_start)),
               nir_ball(&b, nir_ult(&b, copy_id, copy_id_end)));

   nir_push_if(&b, in_bounds);

   nir_def *src_offset = nir_vec3(
      &b, load_info(&b, struct vk_meta_copy_image_cs_info, src_img.offset.x),
      load_info(&b, struct vk_meta_copy_image_cs_info, src_img.offset.y),
      load_info(&b, struct vk_meta_copy_image_cs_info, src_img.offset.z));
   nir_def *dst_offset = nir_vec3(
      &b, load_info(&b, struct vk_meta_copy_image_cs_info, dst_img.offset.x),
      load_info(&b, struct vk_meta_copy_image_cs_info, dst_img.offset.y),
      load_info(&b, struct vk_meta_copy_image_cs_info, dst_img.offset.z));

   nir_def *src_coords = trim_img_coords(&b, key->src.view.type,
                                         nir_iadd(&b, copy_id, src_offset));
   nir_def *dst_coords = trim_img_coords(&b, key->dst.view.type,
                                         nir_iadd(&b, copy_id, dst_offset));

   bool src_is_array = vk_image_view_type_is_array(key->src.view.type);
   enum glsl_sampler_dim src_sampler_dim =
      vk_image_view_type_to_sampler_dim(key->src.view.type);
   bool dst_is_array = vk_image_view_type_is_array(key->dst.view.type);
   enum glsl_sampler_dim dst_sampler_dim =
      vk_image_view_type_to_sampler_dim(key->dst.view.type);

   if (key->samples != VK_SAMPLE_COUNT_1_BIT) {
      assert(src_sampler_dim == GLSL_SAMPLER_DIM_2D);
      assert(dst_sampler_dim == GLSL_SAMPLER_DIM_2D);
      src_sampler_dim = GLSL_SAMPLER_DIM_MS;
      dst_sampler_dim = GLSL_SAMPLER_DIM_MS;
   }

   dst_coords = nir_pad_vector_imm_int(&b, dst_coords, 0, 4);

   assert(src_aspects == dst_aspects);
   assert(src_aspects == VK_IMAGE_ASPECT_COLOR_BIT);

   enum glsl_base_type src_base_type, dst_base_type;

   src_base_type = util_format_is_pure_sint(src_pfmt)   ? GLSL_TYPE_INT
                   : util_format_is_pure_uint(src_pfmt) ? GLSL_TYPE_UINT
                                                        : GLSL_TYPE_FLOAT;
   dst_base_type = util_format_is_pure_sint(dst_pfmt)   ? GLSL_TYPE_INT
                   : util_format_is_pure_uint(dst_pfmt) ? GLSL_TYPE_UINT
                                                        : GLSL_TYPE_FLOAT;

   const struct glsl_type *texture_type =
      glsl_sampler_type(src_sampler_dim, false, src_is_array, src_base_type);
   nir_variable *texture =
      nir_variable_create(b.shader, nir_var_uniform, texture_type, "color_tex");
   texture->data.descriptor_set = 0;
   texture->data.binding = 0;
   nir_deref_instr *tex_deref = nir_build_deref_var(&b, texture);

   const struct glsl_type *image_type =
      glsl_image_type(dst_sampler_dim, dst_is_array, dst_base_type);
   nir_variable *image_var =
      nir_variable_create(b.shader, nir_var_uniform, image_type, NULL);
   image_var->data.descriptor_set = 0;
   image_var->data.binding = 1;
   nir_deref_instr *image_deref = nir_build_deref_var(&b, image_var);

   for (uint32_t s = 0; s < key->samples; s++) {
      nir_def *val =
         key->samples == VK_SAMPLE_COUNT_1_BIT
            ? nir_txf_deref(&b, tex_deref, src_coords, NULL)
            : nir_txf_ms_deref(&b, tex_deref, src_coords, nir_imm_int(&b, s));

      /* If the src/dst formats differ, pack+unpack to convert to
       * the expected output format. */
      if (src_pfmt != dst_pfmt) {
         assert(!util_format_is_yuv(src_pfmt));
         assert(!util_format_is_yuv(dst_pfmt));

         val = nir_format_unpack_rgba(
            &b, nir_format_pack_rgba(&b, src_pfmt, val), dst_pfmt);
      }

      nir_image_deref_store(
         &b, &image_deref->def, dst_coords, nir_imm_int(&b, s), // Sample
         val, nir_imm_int(&b, 0),                               // LOD
         .image_dim = dst_sampler_dim, .image_array = dst_is_array,
         .format = dst_pfmt, .access = ACCESS_NON_READABLE,
         .src_type = nir_get_nir_type_for_glsl_base_type(dst_base_type));
   }

   nir_pop_if(&b, NULL);

   return b.shader;
}

static VkResult
get_copy_image_to_buffer_pipeline(
   struct vk_device *device, struct vk_meta_device *meta,
   const struct vk_meta_copy_buffer_image_key *key,
   VkPipelineLayout *layout_out, VkPipeline *pipeline_out)
{
   const VkDescriptorSetLayoutBinding bindings[] = {
      COPY_SHADER_BINDING(0, STORAGE_BUFFER, COMPUTE),
      COPY_SHADER_BINDING(1, SAMPLED_IMAGE, COMPUTE),
   };

   VkResult result = get_copy_pipeline_layout(
      device, meta, "vk-meta-copy-image-to-buffer-pipeline-layout",
      VK_SHADER_STAGE_COMPUTE_BIT,
      sizeof(struct vk_meta_copy_buffer_image_info), bindings,
      ARRAY_SIZE(bindings), layout_out);

   if (unlikely(result != VK_SUCCESS))
      return result;

   return get_compute_copy_pipeline(device, meta, *layout_out,
                                    build_image_to_buffer_shader, key,
                                    sizeof(*key), pipeline_out);
}

static VkResult
get_copy_buffer_to_image_gfx_pipeline(
   struct vk_device *device, struct vk_meta_device *meta,
   const struct vk_meta_copy_buffer_image_key *key,
   VkPipelineLayout *layout_out, VkPipeline *pipeline_out)
{
   const struct VkDescriptorSetLayoutBinding bindings[] = {
      COPY_SHADER_BINDING(0, STORAGE_BUFFER, FRAGMENT),
   };

   VkResult result = get_copy_pipeline_layout(
      device, meta, "vk-meta-copy-buffer-to-image-gfx-pipeline-layout",
      VK_SHADER_STAGE_FRAGMENT_BIT,
      sizeof(struct vk_meta_copy_buffer_image_info), bindings,
      ARRAY_SIZE(bindings), layout_out);

   if (unlikely(result != VK_SUCCESS))
      return result;

   return get_gfx_copy_pipeline(device, meta, *layout_out,
                                build_buffer_to_image_fs, key->img.view.format,
                                key, sizeof(*key), pipeline_out);
}

static VkResult
get_copy_buffer_to_image_compute_pipeline(
   struct vk_device *device, struct vk_meta_device *meta,
   const struct vk_meta_copy_buffer_image_key *key,
   VkPipelineLayout *layout_out, VkPipeline *pipeline_out)
{
   const VkDescriptorSetLayoutBinding bindings[] = {
      COPY_SHADER_BINDING(0, STORAGE_BUFFER, COMPUTE),
      COPY_SHADER_BINDING(1, STORAGE_IMAGE, COMPUTE),
   };

   VkResult result = get_copy_pipeline_layout(
      device, meta, "vk-meta-copy-buffer-to-image-compute-pipeline-layout",
      VK_SHADER_STAGE_COMPUTE_BIT,
      sizeof(struct vk_meta_copy_buffer_image_info), bindings,
      ARRAY_SIZE(bindings), layout_out);

   if (unlikely(result != VK_SUCCESS))
      return result;

   return get_compute_copy_pipeline(device, meta, *layout_out,
                                    build_buffer_to_image_cs, key, sizeof(*key),
                                    pipeline_out);
}

static VkResult
copy_buffer_image_prepare_push_const(
   struct vk_command_buffer *cmd, struct vk_meta_device *meta,
   const struct vk_meta_copy_buffer_image_key *key,
   VkPipelineLayout pipeline_layout, VkBuffer buffer,
   const struct vk_image_buffer_layout *buf_layout, struct vk_image *img,
   const VkBufferImageCopy2 *region, uint32_t *wg_count)
{
   struct vk_device *dev = cmd->base.device;
   const struct vk_device_dispatch_table *disp = &dev->dispatch_table;
   uint32_t depth_or_layer_count =
      MAX2(region->imageExtent.depth,
           vk_image_subresource_layer_count(img, &region->imageSubresource));
   VkImageViewType img_view_type =
      key->use_gfx_pipeline
         ? vk_image_render_view_type(img, depth_or_layer_count)
         : key->img.view.type;
   VkOffset3D img_offs = vk_image_view_base_layer_as_offset(
      img_view_type, region->imageOffset,
      region->imageSubresource.baseArrayLayer);
   uint32_t layer_count =
      vk_image_subresource_layer_count(img, &region->imageSubresource);
   VkExtent3D img_extent = vk_image_view_layer_count_as_extent(
      img_view_type, region->imageExtent, layer_count);

   struct vk_meta_copy_buffer_image_info info = {
      .buf =
         {
            .row_stride = buf_layout->row_stride_B,
            .image_stride = buf_layout->image_stride_B,
         },
      .img.offset =
         {
            .x = img_offs.x,
            .y = img_offs.y,
            .z = img_offs.z,
         },
   };

   if (meta->buffer_access.use_global_address)
      info.buf.addr =
         vk_meta_buffer_address(dev, buffer) + region->bufferOffset;

   if (!key->use_gfx_pipeline) {
      info.copy_id_range.start.x = img_offs.x % key->wg_size[0];
      info.copy_id_range.start.y = img_offs.y % key->wg_size[1];
      info.copy_id_range.start.z = img_offs.z % key->wg_size[2];
      info.copy_id_range.end.x = info.copy_id_range.start.x + img_extent.width;
      info.copy_id_range.end.y = info.copy_id_range.start.y + img_extent.height;
      info.copy_id_range.end.z = info.copy_id_range.start.z + img_extent.depth;
      wg_count[0] = DIV_ROUND_UP(info.copy_id_range.end.x, key->wg_size[0]);
      wg_count[1] = DIV_ROUND_UP(info.copy_id_range.end.y, key->wg_size[1]);
      wg_count[2] = DIV_ROUND_UP(info.copy_id_range.end.z, key->wg_size[2]);
   }

   disp->CmdPushConstants(vk_command_buffer_to_handle(cmd), pipeline_layout,
                          key->use_gfx_pipeline ? VK_SHADER_STAGE_FRAGMENT_BIT
                                                : VK_SHADER_STAGE_COMPUTE_BIT,
                          0, sizeof(info), &info);

   return VK_SUCCESS;
}

static void
copy_image_to_buffer_region(
   struct vk_command_buffer *cmd, struct vk_meta_device *meta,
   struct vk_image *img, VkImageLayout img_layout,
   const struct vk_meta_copy_image_properties *img_props, VkBuffer buffer,
   const struct vk_image_buffer_layout *buf_layout,
   const VkBufferImageCopy2 *region)
{
   struct vk_device *dev = cmd->base.device;
   const struct vk_device_dispatch_table *disp = &dev->dispatch_table;
   struct vk_meta_copy_buffer_image_key key = {
      .key_type = VK_META_OBJECT_KEY_COPY_IMAGE_TO_BUFFER_PIPELINE,
      .img =
         {
            .view =
               {
                  .format = img_props->view_format,
                  .type = vk_image_sampled_view_type(img),
               },
            .aspect = region->imageSubresource.aspectMask,
         },
      .wg_size =
         {
            img_props->tile_size.width,
            img_props->tile_size.height,
            img_props->tile_size.depth,
         },
   };

   VkPipelineLayout pipeline_layout;
   VkPipeline pipeline;
   VkResult result = get_copy_image_to_buffer_pipeline(
      dev, meta, &key, &pipeline_layout, &pipeline);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(cmd, result);
      return;
   }

   disp->CmdBindPipeline(vk_command_buffer_to_handle(cmd),
                         VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   VkImageView iview;
   result = copy_create_src_image_view(cmd, meta, img, key.img.view.format,
                                       region->imageSubresource.aspectMask,
                                       &region->imageSubresource, &iview);

   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(cmd, result);
      return;
   }

   if (!meta->buffer_access.use_global_address) {
      VkDeviceSize buffer_range = vk_image_buffer_range(
         img, buf_layout, &region->imageExtent, &region->imageSubresource);
      const VkWriteDescriptorSet descs[] = {
         COPY_PUSH_SET_BUF_DESC(0, buffer, region->bufferOffset, buffer_range),
         COPY_PUSH_SET_IMG_DESC(1, SAMPLED, iview, img_layout),
      };

      disp->CmdPushDescriptorSetKHR(
         vk_command_buffer_to_handle(cmd), VK_PIPELINE_BIND_POINT_COMPUTE,
         pipeline_layout, 0, ARRAY_SIZE(descs), descs);
   } else {
      const VkWriteDescriptorSet descs[] = {
         COPY_PUSH_SET_IMG_DESC(1, SAMPLED, iview, img_layout),
      };

      disp->CmdPushDescriptorSetKHR(
         vk_command_buffer_to_handle(cmd), VK_PIPELINE_BIND_POINT_COMPUTE,
         pipeline_layout, 0, ARRAY_SIZE(descs), descs);
   }

   uint32_t wg_count[3] = {0};

   result = copy_buffer_image_prepare_push_const(
      cmd, meta, &key, pipeline_layout, buffer, buf_layout, img, region,
      wg_count);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(cmd, result);
      return;
   }

   disp->CmdDispatch(vk_command_buffer_to_handle(cmd), wg_count[0], wg_count[1],
                     wg_count[2]);
}

void
vk_meta_copy_image_to_buffer(
   struct vk_command_buffer *cmd, struct vk_meta_device *meta,
   const VkCopyImageToBufferInfo2 *info,
   const struct vk_meta_copy_image_properties *img_props)
{
   VK_FROM_HANDLE(vk_image, img, info->srcImage);

   for (uint32_t i = 0; i < info->regionCount; i++) {
      VkBufferImageCopy2 region = info->pRegions[i];
      struct vk_image_buffer_layout buf_layout =
         vk_image_buffer_copy_layout(img, &region);

      region.imageExtent = vk_image_extent_to_elements(img, region.imageExtent);
      region.imageOffset = vk_image_offset_to_elements(img, region.imageOffset);

      copy_image_to_buffer_region(cmd, meta, img, info->srcImageLayout,
                                  img_props, info->dstBuffer, &buf_layout,
                                  &region);
   }
}

static void
copy_draw(struct vk_command_buffer *cmd, struct vk_meta_device *meta,
          struct vk_image *dst_img, VkImageLayout dst_img_layout,
          const VkImageSubresourceLayers *dst_img_subres,
          const VkOffset3D *dst_img_offset, const VkExtent3D *copy_extent,
          VkFormat dst_view_format)
{
   struct vk_device *dev = cmd->base.device;
   const struct vk_device_dispatch_table *disp = &dev->dispatch_table;
   uint32_t depth_or_layer_count =
      MAX2(copy_extent->depth,
           vk_image_subresource_layer_count(dst_img, dst_img_subres));
   VkImageView iview;
   VkResult result = copy_create_dst_image_view(
      cmd, meta, dst_img, dst_view_format, dst_img_subres->aspectMask,
      dst_img_offset, copy_extent, dst_img_subres, true, &iview);

   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(cmd, result);
      return;
   }

   struct vk_meta_rect rect = {
      .x0 = dst_img_offset->x,
      .x1 = dst_img_offset->x + copy_extent->width,
      .y0 = dst_img_offset->y,
      .y1 = dst_img_offset->y + copy_extent->height,
   };
   VkRenderingInfo vk_render = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea =
         {
            .offset =
               {
                  dst_img_offset->x,
                  dst_img_offset->y,
               },
            .extent =
               {
                  copy_extent->width,
                  copy_extent->height,
               },
         },
      .layerCount = depth_or_layer_count,
   };
   const VkRenderingAttachmentInfo vk_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = iview,
      .imageLayout = dst_img_layout,
      .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
   };

   if (dst_img_subres->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
      vk_render.pColorAttachments = &vk_att;
      vk_render.colorAttachmentCount = 1;
   }

   if (dst_img_subres->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
      vk_render.pDepthAttachment = &vk_att;
   if (dst_img_subres->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT)
      vk_render.pStencilAttachment = &vk_att;

   disp->CmdBeginRendering(vk_command_buffer_to_handle(cmd), &vk_render);
   meta->cmd_draw_volume(cmd, meta, &rect, vk_render.layerCount);
   disp->CmdEndRendering(vk_command_buffer_to_handle(cmd));
}

static void
copy_buffer_to_image_region(
   struct vk_command_buffer *cmd, struct vk_meta_device *meta,
   struct vk_image *img, VkImageLayout img_layout,
   const struct vk_meta_copy_image_properties *img_props, VkBuffer buffer,
   const struct vk_image_buffer_layout *buf_layout, bool use_gfx_pipeline,
   const VkBufferImageCopy2 *region)
{
   struct vk_device *dev = cmd->base.device;
   const struct vk_device_dispatch_table *disp = &dev->dispatch_table;
   struct vk_meta_copy_buffer_image_key key = {
      .key_type = VK_META_OBJECT_KEY_COPY_BUFFER_TO_IMAGE_PIPELINE,
      .use_gfx_pipeline = use_gfx_pipeline,
      .img =
         {
            .view.format = img_props->view_format,
            .aspect = region->imageSubresource.aspectMask,
         },
   };

   if (use_gfx_pipeline) {
      /* We only special-case 1D_ARRAY to move the layer ID to the second
       * component instead of the third. For all other view types, let's pick an
       * invalid VkImageViewType value so we don't end up creating the same
       * pipeline multiple times. */
      key.img.view.type =
         img->image_type == VK_IMAGE_TYPE_1D && img->array_layers > 1
            ? VK_IMAGE_VIEW_TYPE_1D_ARRAY
            : (VkImageViewType)-1;
   } else {
      key.img.view.type = vk_image_storage_view_type(img);
   }

   if (!use_gfx_pipeline) {
      key.wg_size[0] = img_props->tile_size.width;
      key.wg_size[1] = img_props->tile_size.height;
      key.wg_size[2] = img_props->tile_size.depth;
   }

   VkPipelineLayout pipeline_layout;
   VkPipeline pipeline;
   VkResult result = use_gfx_pipeline
                        ? get_copy_buffer_to_image_gfx_pipeline(
                             dev, meta, &key, &pipeline_layout, &pipeline)
                        : get_copy_buffer_to_image_compute_pipeline(
                             dev, meta, &key, &pipeline_layout, &pipeline);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(cmd, result);
      return;
   }

   disp->CmdBindPipeline(vk_command_buffer_to_handle(cmd),
                         key.use_gfx_pipeline ? VK_PIPELINE_BIND_POINT_GRAPHICS
                                              : VK_PIPELINE_BIND_POINT_COMPUTE,
                         pipeline);

   if (!key.use_gfx_pipeline) {
      VkImageView iview;
      VkResult result = copy_create_dst_image_view(
         cmd, meta, img, key.img.view.format,
         region->imageSubresource.aspectMask, &region->imageOffset,
         &region->imageExtent, &region->imageSubresource, false, &iview);

      if (unlikely(result != VK_SUCCESS)) {
         vk_command_buffer_set_error(cmd, result);
         return;
      }

      if (!meta->buffer_access.use_global_address) {
         VkDeviceSize buffer_range = vk_image_buffer_range(
            img, buf_layout, &region->imageExtent, &region->imageSubresource);
         const VkWriteDescriptorSet descs[] = {
            COPY_PUSH_SET_BUF_DESC(0, buffer, region->bufferOffset,
                                   buffer_range),
            COPY_PUSH_SET_IMG_DESC(1, STORAGE, iview, img_layout),
         };

         disp->CmdPushDescriptorSetKHR(
            vk_command_buffer_to_handle(cmd), VK_PIPELINE_BIND_POINT_COMPUTE,
            pipeline_layout, 0, ARRAY_SIZE(descs), descs);
      } else {
         const VkWriteDescriptorSet descs[] = {
            COPY_PUSH_SET_IMG_DESC(1, STORAGE, iview, img_layout),
         };

         disp->CmdPushDescriptorSetKHR(
            vk_command_buffer_to_handle(cmd), VK_PIPELINE_BIND_POINT_COMPUTE,
            pipeline_layout, 0, ARRAY_SIZE(descs), descs);
      }
   } else if (!meta->buffer_access.use_global_address) {
      VkDeviceSize buffer_range = vk_image_buffer_range(
         img, buf_layout, &region->imageExtent, &region->imageSubresource);
      const VkWriteDescriptorSet descs[] = {
         COPY_PUSH_SET_BUF_DESC(0, buffer, region->bufferOffset, buffer_range)};

      disp->CmdPushDescriptorSetKHR(
         vk_command_buffer_to_handle(cmd), VK_PIPELINE_BIND_POINT_GRAPHICS,
         pipeline_layout, 0, ARRAY_SIZE(descs), descs);
   }

   uint32_t wg_count[3] = {0};

   result = copy_buffer_image_prepare_push_const(
      cmd, meta, &key, pipeline_layout, buffer, buf_layout, img, region,
      wg_count);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(cmd, result);
      return;
   }

   if (key.use_gfx_pipeline) {
      copy_draw(cmd, meta, img, img_layout, &region->imageSubresource,
                &region->imageOffset, &region->imageExtent,
                key.img.view.format);
   } else {
      disp->CmdDispatch(vk_command_buffer_to_handle(cmd), wg_count[0],
                        wg_count[1], wg_count[2]);
   }
}

void
vk_meta_copy_buffer_to_image(
   struct vk_command_buffer *cmd, struct vk_meta_device *meta,
   const VkCopyBufferToImageInfo2 *info,
   const struct vk_meta_copy_image_properties *img_props, bool use_gfx_pipeline)
{
   VK_FROM_HANDLE(vk_image, img, info->dstImage);

   for (uint32_t i = 0; i < info->regionCount; i++) {
      VkBufferImageCopy2 region = info->pRegions[i];
      struct vk_image_buffer_layout buf_layout =
         vk_image_buffer_copy_layout(img, &region);

      region.imageExtent = vk_image_extent_to_elements(img, region.imageExtent);
      region.imageOffset = vk_image_offset_to_elements(img, region.imageOffset);
      copy_buffer_to_image_region(cmd, meta, img, info->dstImageLayout,
                                  img_props, info->srcBuffer, &buf_layout,
                                  use_gfx_pipeline, &region);
   }
}

static VkResult
get_copy_image_pipeline(struct vk_device *device, struct vk_meta_device *meta,
                        const struct vk_meta_copy_image_key *key,
                        VkPipelineLayout *layout_out, VkPipeline *pipeline_out)
{
   VkResult result;

   if (key->use_gfx_pipeline) {
      const struct VkDescriptorSetLayoutBinding bindings[] = {
         COPY_SHADER_BINDING(0, SAMPLED_IMAGE, FRAGMENT),
         COPY_SHADER_BINDING(1, SAMPLED_IMAGE, FRAGMENT),
         COPY_SHADER_BINDING(2, SAMPLED_IMAGE, FRAGMENT),
      };

      result = get_copy_pipeline_layout(
         device, meta, "vk-meta-copy-image-gfx-pipeline-layout",
         VK_SHADER_STAGE_FRAGMENT_BIT,
         sizeof(struct vk_meta_copy_image_fs_info), bindings,
         ARRAY_SIZE(bindings), layout_out);
      if (unlikely(result != VK_SUCCESS))
         return result;

      return get_gfx_copy_pipeline(device, meta, *layout_out,
                                   build_copy_image_fs, key->dst.view.format,
                                   key, sizeof(*key), pipeline_out);
   }

   const VkDescriptorSetLayoutBinding bindings[] = {
      COPY_SHADER_BINDING(0, SAMPLED_IMAGE, COMPUTE),
      COPY_SHADER_BINDING(1, STORAGE_IMAGE, COMPUTE),
   };

   result = get_copy_pipeline_layout(
      device, meta, "vk-meta-copy-image-compute-pipeline-layout",
      VK_SHADER_STAGE_COMPUTE_BIT, sizeof(struct vk_meta_copy_image_cs_info),
      bindings, ARRAY_SIZE(bindings), layout_out);

   if (unlikely(result != VK_SUCCESS))
      return result;

   result = key->use_gfx_pipeline
               ? get_gfx_copy_pipeline(
                    device, meta, *layout_out, build_copy_image_fs,
                    key->dst.view.format, key, sizeof(*key), pipeline_out)
               : get_compute_copy_pipeline(device, meta, *layout_out,
                                           build_copy_image_cs, key,
                                           sizeof(*key), pipeline_out);

   return result;
}

static VkResult
copy_image_prepare_desc_set(
   struct vk_command_buffer *cmd, struct vk_meta_device *meta,
   const struct vk_meta_copy_image_key *key, VkPipelineLayout pipeline_layout,
   struct vk_image *src_img, VkImageLayout src_img_layout,
   struct vk_image *dst_img, VkImageLayout dst_img_layout,
   const VkImageCopy2 *region)
{
   struct vk_device *dev = cmd->base.device;
   const struct vk_device_dispatch_table *disp = &dev->dispatch_table;

   if (key->use_gfx_pipeline) {
      VkImageAspectFlags aspects = vk_format_aspects(key->src.view.format);

      u_foreach_bit(a, aspects) {
         VkImageView src_view;
         VkResult result = copy_create_src_image_view(
            cmd, meta, src_img, key->src.view.format, 1 << a,
            &region->srcSubresource, &src_view);
         if (unlikely(result != VK_SUCCESS))
            return result;

         if (key->use_gfx_pipeline) {
            const VkWriteDescriptorSet descs[] = {
               COPY_PUSH_SET_IMG_DESC(a, SAMPLED, src_view, src_img_layout),
            };

            disp->CmdPushDescriptorSetKHR(vk_command_buffer_to_handle(cmd),
                                          VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          pipeline_layout, 0, ARRAY_SIZE(descs),
                                          descs);
         }
      }

      return VK_SUCCESS;
   }

   /* No depth/stencil copies using a compute pipeline. */
   assert(vk_format_is_color(key->dst.view.format));

   VkImageView dst_view, src_view;
   VkResult result = copy_create_src_image_view(
      cmd, meta, src_img, key->src.view.format, VK_IMAGE_ASPECT_COLOR_BIT,
      &region->srcSubresource, &src_view);
   if (unlikely(result != VK_SUCCESS))
      return result;

   result = copy_create_dst_image_view(
      cmd, meta, dst_img, key->dst.view.format, VK_IMAGE_ASPECT_COLOR_BIT,
      &region->dstOffset, &region->extent, &region->dstSubresource, false,
      &dst_view);
   if (unlikely(result != VK_SUCCESS))
      return result;

   const VkWriteDescriptorSet descs[] = {
      COPY_PUSH_SET_IMG_DESC(0, SAMPLED, src_view, src_img_layout),
      COPY_PUSH_SET_IMG_DESC(1, STORAGE, dst_view, dst_img_layout),
   };

   disp->CmdPushDescriptorSetKHR(vk_command_buffer_to_handle(cmd),
                                 VK_PIPELINE_BIND_POINT_COMPUTE,
                                 pipeline_layout, 0, ARRAY_SIZE(descs), descs);

   return VK_SUCCESS;
}

enum vk_meta_copy_image_align_policy {
   VK_META_COPY_IMAGE_ALIGN_ON_SRC_TILE,
   VK_META_COPY_IMAGE_ALIGN_ON_DST_TILE,
};

static VkResult
copy_image_prepare_compute_push_const(
   struct vk_command_buffer *cmd, struct vk_meta_device *meta,
   const struct vk_meta_copy_image_key *key, VkPipelineLayout pipeline_layout,
   const struct vk_image *src, const struct vk_image *dst,
   enum vk_meta_copy_image_align_policy align_policy,
   const VkImageCopy2 *region, uint32_t *wg_count)
{
   struct vk_device *dev = cmd->base.device;
   const struct vk_device_dispatch_table *disp = &dev->dispatch_table;
   VkOffset3D src_offs =
      vk_image_view_base_layer_as_offset(key->src.view.type, region->srcOffset,
                                         region->srcSubresource.baseArrayLayer);
   uint32_t layer_count =
      vk_image_subresource_layer_count(src, &region->srcSubresource);
   VkExtent3D src_extent = vk_image_view_layer_count_as_extent(
      key->src.view.type, region->extent, layer_count);
   VkOffset3D dst_offs =
      vk_image_view_base_layer_as_offset(key->dst.view.type, region->dstOffset,
                                         region->dstSubresource.baseArrayLayer);

   struct vk_meta_copy_image_cs_info info = {0};

   /* We can't necessarily optimize the read+write path, so align things
    * on the biggest tile size. */
   if (align_policy == VK_META_COPY_IMAGE_ALIGN_ON_SRC_TILE) {
      info.copy_id_range.start.x = src_offs.x % key->wg_size[0];
      info.copy_id_range.start.y = src_offs.y % key->wg_size[1];
      info.copy_id_range.start.z = src_offs.z % key->wg_size[2];
   } else {
      info.copy_id_range.start.x = dst_offs.x % key->wg_size[0];
      info.copy_id_range.start.y = dst_offs.y % key->wg_size[1];
      info.copy_id_range.start.z = dst_offs.z % key->wg_size[2];
   }

   info.copy_id_range.end.x = info.copy_id_range.start.x + src_extent.width;
   info.copy_id_range.end.y = info.copy_id_range.start.y + src_extent.height;
   info.copy_id_range.end.z = info.copy_id_range.start.z + src_extent.depth;

   info.src_img.offset.x = src_offs.x - info.copy_id_range.start.x;
   info.src_img.offset.y = src_offs.y - info.copy_id_range.start.y;
   info.src_img.offset.z = src_offs.z - info.copy_id_range.start.z;
   info.dst_img.offset.x = dst_offs.x - info.copy_id_range.start.x;
   info.dst_img.offset.y = dst_offs.y - info.copy_id_range.start.y;
   info.dst_img.offset.z = dst_offs.z - info.copy_id_range.start.z;
   wg_count[0] = DIV_ROUND_UP(info.copy_id_range.end.x, key->wg_size[0]);
   wg_count[1] = DIV_ROUND_UP(info.copy_id_range.end.y, key->wg_size[1]);
   wg_count[2] = DIV_ROUND_UP(info.copy_id_range.end.z, key->wg_size[2]);

   disp->CmdPushConstants(vk_command_buffer_to_handle(cmd), pipeline_layout,
                          key->use_gfx_pipeline ? VK_SHADER_STAGE_FRAGMENT_BIT
                                                : VK_SHADER_STAGE_COMPUTE_BIT,
                          0, sizeof(info), &info);

   return VK_SUCCESS;
}

static VkResult
copy_image_prepare_gfx_push_const(struct vk_command_buffer *cmd,
                                  struct vk_meta_device *meta,
                                  const struct vk_meta_copy_image_key *key,
                                  VkPipelineLayout pipeline_layout,
                                  struct vk_image *src_img,
                                  struct vk_image *dst_img,
                                  const VkImageCopy2 *region)
{
   struct vk_device *dev = cmd->base.device;
   const struct vk_device_dispatch_table *disp = &dev->dispatch_table;
   VkOffset3D src_img_offs =
      vk_image_view_base_layer_as_offset(key->src.view.type, region->srcOffset,
                                         region->srcSubresource.baseArrayLayer);

   /* Render image view only contains the layers needed for rendering,
    * so we consider the coordinate containing the layer to always be
    * zero. */
   VkOffset3D dst_img_offs = {
      .x = region->dstOffset.x,
      .y = dst_img->image_type == VK_IMAGE_TYPE_1D ? 0 : region->dstOffset.y,
      .z = 0,
   };

   struct vk_meta_copy_image_fs_info info = {
      .dst_to_src_offs =
         {
            .x = src_img_offs.x - dst_img_offs.x,
            .y = src_img_offs.x - dst_img_offs.y,
            .z = src_img_offs.z - dst_img_offs.z,
         },
   };

   disp->CmdPushConstants(vk_command_buffer_to_handle(cmd), pipeline_layout,
                          key->use_gfx_pipeline ? VK_SHADER_STAGE_FRAGMENT_BIT
                                                : VK_SHADER_STAGE_COMPUTE_BIT,
                          0, sizeof(info), &info);

   return VK_SUCCESS;
}

static void
copy_image_region(struct vk_command_buffer *cmd, struct vk_meta_device *meta,
                  const struct vk_meta_copy_image_key *key,
                  VkPipelineLayout pipeline_layout,
                  enum vk_meta_copy_image_align_policy align_policy,
                  struct vk_image *src_img, VkImageLayout src_image_layout,
                  struct vk_image *dst_img, VkImageLayout dst_image_layout,
                  const VkImageCopy2 *region)
{
   struct vk_device *dev = cmd->base.device;
   const struct vk_device_dispatch_table *disp = &dev->dispatch_table;

   VkResult result = copy_image_prepare_desc_set(
      cmd, meta, key, pipeline_layout, src_img, src_image_layout, dst_img,
      dst_image_layout, region);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(cmd, result);
      return;
   }

   if (key->use_gfx_pipeline) {
      result = copy_image_prepare_gfx_push_const(
         cmd, meta, key, pipeline_layout, src_img, dst_img, region);
      if (unlikely(result != VK_SUCCESS)) {
         vk_command_buffer_set_error(cmd, result);
         return;
      }

      copy_draw(cmd, meta, dst_img, dst_image_layout, &region->dstSubresource,
                &region->dstOffset, &region->extent, key->dst.view.format);
   } else {
      uint32_t wg_count[3] = {0};

      result = copy_image_prepare_compute_push_const(
         cmd, meta, key, pipeline_layout, src_img, dst_img, align_policy,
         region, wg_count);
      if (unlikely(result != VK_SUCCESS)) {
         vk_command_buffer_set_error(cmd, result);
         return;
      }

      disp->CmdDispatch(vk_command_buffer_to_handle(cmd), wg_count[0],
                        wg_count[1], wg_count[2]);
   }
}

void
vk_meta_copy_image(struct vk_command_buffer *cmd, struct vk_meta_device *meta,
                   const VkCopyImageInfo2 *info,
                   const struct vk_meta_copy_image_properties *src_props,
                   const struct vk_meta_copy_image_properties *dst_props,
                   bool use_gfx_pipeline)
{
   struct vk_device *dev = cmd->base.device;
   const struct vk_device_dispatch_table *disp = &dev->dispatch_table;
   VK_FROM_HANDLE(vk_image, src_img, info->srcImage);
   VK_FROM_HANDLE(vk_image, dst_img, info->dstImage);
   enum vk_meta_copy_image_align_policy align_policy =
      VK_META_COPY_IMAGE_ALIGN_ON_SRC_TILE;
   struct vk_meta_copy_image_key key = {
      .key_type = VK_META_OBJECT_KEY_COPY_IMAGE_PIPELINE,
      .use_gfx_pipeline = use_gfx_pipeline,
      .samples = src_img->samples,
      .src.view =
         {
            .type = vk_image_sampled_view_type(src_img),
            .format = src_props->view_format,
         },
      .dst.view.format = dst_props->view_format,
   };

   if (use_gfx_pipeline) {
      /* We only special-case 1D_ARRAY to move the layer ID to the second
       * component instead of the third. For all other view types, let's pick an
       * invalid VkImageViewType value so we don't end up creating the same
       * pipeline multiple times. */
      key.dst.view.type =
         dst_img->image_type == VK_IMAGE_TYPE_1D && dst_img->array_layers > 1
            ? VK_IMAGE_VIEW_TYPE_1D_ARRAY
            : (VkImageViewType)-1;
   } else {
      uint32_t src_pix_per_tile = src_props->tile_size.width *
                                  src_props->tile_size.height *
                                  src_props->tile_size.depth;
      uint32_t dst_pix_per_tile = dst_props->tile_size.width *
                                  dst_props->tile_size.height *
                                  dst_props->tile_size.depth;

      if (src_pix_per_tile >= dst_pix_per_tile) {
         key.wg_size[0] = src_props->tile_size.width;
         key.wg_size[1] = src_props->tile_size.height;
         key.wg_size[2] = src_props->tile_size.depth;
         align_policy = VK_META_COPY_IMAGE_ALIGN_ON_SRC_TILE;
      } else {
         key.wg_size[0] = dst_props->tile_size.width;
         key.wg_size[1] = dst_props->tile_size.height;
         key.wg_size[2] = dst_props->tile_size.depth;
         align_policy = VK_META_COPY_IMAGE_ALIGN_ON_DST_TILE;
      }
      key.dst.view.type = vk_image_storage_view_type(dst_img);
   }

   VkPipelineLayout pipeline_layout;
   VkPipeline pipeline;
   VkResult result =
      get_copy_image_pipeline(dev, meta, &key, &pipeline_layout, &pipeline);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(cmd, result);
      return;
   }

   disp->CmdBindPipeline(vk_command_buffer_to_handle(cmd),
                         use_gfx_pipeline ? VK_PIPELINE_BIND_POINT_GRAPHICS
                                          : VK_PIPELINE_BIND_POINT_COMPUTE,
                         pipeline);

   for (uint32_t i = 0; i < info->regionCount; i++) {
      VkImageCopy2 region = info->pRegions[i];

      /* Extent always refers to the source image. Pass a NULL extent
       * when patching the dst offset. */
      region.extent = vk_image_extent_to_elements(src_img, region.extent);
      region.srcOffset = vk_image_offset_to_elements(src_img, region.srcOffset);
      region.dstOffset = vk_image_offset_to_elements(dst_img, region.dstOffset);

      copy_image_region(cmd, meta, &key, pipeline_layout, align_policy, src_img,
                        info->srcImageLayout, dst_img, info->dstImageLayout,
                        &region);
   }
}

void
vk_meta_update_buffer(struct vk_command_buffer *cmd,
                      struct vk_meta_device *meta, VkBuffer buffer,
                      VkDeviceSize offset, VkDeviceSize size, const void *data)
{
   VkResult result;

   const VkBufferCreateInfo tmp_buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .queueFamilyIndexCount = 1,
      .pQueueFamilyIndices = &cmd->pool->queue_family_index,
   };

   VkBuffer tmp_buffer;
   result = vk_meta_create_buffer(cmd, meta, &tmp_buffer_info, &tmp_buffer);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(cmd, result);
      return;
   }

   void *tmp_buffer_map;
   result = meta->cmd_bind_map_buffer(cmd, meta, tmp_buffer, &tmp_buffer_map);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(cmd, result);
      return;
   }

   memcpy(tmp_buffer_map, data, size);

   const VkBufferCopy2 copy_region = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
      .srcOffset = 0,
      .dstOffset = offset,
      .size = size,
   };
   const VkCopyBufferInfo2 copy_info = {
      .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
      .srcBuffer = tmp_buffer,
      .dstBuffer = buffer,
      .regionCount = 1,
      .pRegions = &copy_region,
   };

   vk_meta_copy_buffer(cmd, meta, &copy_info);
}

static VkResult
get_fill_buffer_pipeline_layout(struct vk_device *device,
                                struct vk_meta_device *meta,
                                VkPipelineLayout *layout_out)
{
   static const char lkey[] = "vk-meta-fill-buffer-pipeline-layout";
   const VkDescriptorSetLayoutBinding bindings[] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
   };

   const VkDescriptorSetLayoutCreateInfo set_layout = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
      .bindingCount = ARRAY_SIZE(bindings),
      .pBindings = bindings,
   };

   const VkPushConstantRange push_range = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0,
      .size = sizeof(struct vk_meta_fill_buffer_info),
   };

   return vk_meta_get_pipeline_layout(device, meta, &set_layout, &push_range,
                                      lkey, sizeof(lkey), layout_out);
}

static VkResult
get_fill_buffer_pipeline(struct vk_device *device, struct vk_meta_device *meta,
                         const struct vk_meta_copy_buffer_key *key,
                         VkPipelineLayout *layout_out, VkPipeline *pipeline_out)
{
   VkResult result = get_fill_buffer_pipeline_layout(device, meta, layout_out);

   if (unlikely(result != VK_SUCCESS))
      return result;

   return get_compute_copy_pipeline(device, meta, *layout_out,
                                    build_fill_buffer_shader, key, sizeof(*key),
                                    pipeline_out);
}

void
vk_meta_fill_buffer(struct vk_command_buffer *cmd, struct vk_meta_device *meta,
                    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                    uint32_t data)
{
   VK_FROM_HANDLE(vk_buffer, buf, buffer);
   struct vk_device *dev = cmd->base.device;
   const struct vk_physical_device *pdev = dev->physical;
   const struct vk_device_dispatch_table *disp = &dev->dispatch_table;
   VkResult result;

   struct vk_meta_copy_buffer_key key = {
      .key_type = VK_META_OBJECT_KEY_FILL_BUFFER_PIPELINE,
   };

   VkPipelineLayout pipeline_layout;
   VkPipeline pipeline;
   result =
      get_fill_buffer_pipeline(dev, meta, &key, &pipeline_layout, &pipeline);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(cmd, result);
      return;
   }

   disp->CmdBindPipeline(vk_command_buffer_to_handle(cmd),
                         VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   /* Spec says:
    * "If VK_WHOLE_SIZE is used and the remaining size of the buffer is not a
    * multiple of 4, then the nearest smaller multiple is used."
    * hence the mask to align the size on 4 bytes here.
    */
   size = vk_buffer_range(buf, offset, size) & ~3u;
   VkDeviceAddress buf_addr = meta->buffer_access.use_global_address
                                 ? vk_meta_buffer_address(dev, buffer) + offset
                                 : 0;

   const uint32_t optimal_wg_size =
      DIV_ROUND_UP(meta->buffer_access.optimal_size_per_wg, 4);
   const uint32_t per_wg_copy_size = optimal_wg_size * 4;
   uint32_t max_per_dispatch_size =
      pdev->properties.maxComputeWorkGroupCount[0] * per_wg_copy_size;

   while (size > 0) {
      struct vk_meta_fill_buffer_info args = {
         .size = MIN2(size, max_per_dispatch_size),
         .buf_addr = buf_addr,
         .data = data,
      };
      uint32_t wg_count = DIV_ROUND_UP(args.size, per_wg_copy_size);

      disp->CmdPushConstants(vk_command_buffer_to_handle(cmd), pipeline_layout,
                             VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args),
                             &args);

      if (meta->buffer_access.use_global_address) {
         buf_addr += args.size;
      } else {
         VkWriteDescriptorSet write_desc_set[] = {
            {
               .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
               .dstBinding = 0,
               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
               .descriptorCount = 1,
               .pBufferInfo =
                  &(VkDescriptorBufferInfo){
                     .buffer = buffer,
                     .offset = offset,
                     .range = args.size,
                  },
            },
         };

         disp->CmdPushDescriptorSetKHR(
            vk_command_buffer_to_handle(cmd), VK_PIPELINE_BIND_POINT_COMPUTE,
            pipeline_layout, 0, ARRAY_SIZE(write_desc_set), write_desc_set);
      }

      disp->CmdDispatch(vk_command_buffer_to_handle(cmd), wg_count, 1, 1);

      offset += args.size;
      size -= args.size;
   }
}
