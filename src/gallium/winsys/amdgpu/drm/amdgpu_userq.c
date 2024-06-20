/*
 * Copyright © 2023 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "amdgpu_bo.h"
#include <amdgpu_drm.h>

static bool
amdgpu_ring_init(struct amdgpu_winsys *aws, struct amdgpu_userq *userq)
{
   /* allocate memory for ring */
   userq->ring_bo = amdgpu_bo_create(aws, AMDGPU_USERQ_RING_SIZE, 256, RADEON_DOMAIN_GTT,
                                     RADEON_FLAG_GL2_BYPASS |
                                     RADEON_FLAG_NO_SUBALLOC);
   if (!userq->ring_bo)
      return false;

   userq->ring_base_ptr = (uint32_t*)amdgpu_bo_map(&aws->dummy_sws.base, userq->ring_bo, NULL,
                                                   PIPE_MAP_WRITE | PIPE_MAP_UNSYNCHRONIZED);
   if (!userq->ring_base_ptr)
      return false;

   /* allocate memory for rptr */
   userq->rptr_bo = amdgpu_bo_create(aws, aws->info.gart_page_size, 256, RADEON_DOMAIN_GTT,
                                     RADEON_FLAG_GL2_BYPASS |
                                     RADEON_FLAG_NO_SUBALLOC);
   if (!userq->rptr_bo)
      return false;

   userq->mono_rptr = amdgpu_bo_map(&aws->dummy_sws.base, userq->rptr_bo, NULL,
                                    PIPE_MAP_READ | PIPE_MAP_UNSYNCHRONIZED);
   if (!userq->mono_rptr)
      return false;

   /* allocate memory for wptr */
   userq->wptr_bo = amdgpu_bo_create(aws, aws->info.gart_page_size, 256, RADEON_DOMAIN_GTT,
                                     RADEON_FLAG_GL2_BYPASS |
                                     RADEON_FLAG_NO_SUBALLOC);
   if (!userq->wptr_bo)
      return false;

   userq->mono_wptr = amdgpu_bo_map(&aws->dummy_sws.base, userq->wptr_bo, NULL,
                                    PIPE_MAP_READ | PIPE_MAP_WRITE | PIPE_MAP_UNSYNCHRONIZED);
   if (!userq->mono_wptr)
      return false;

   *userq->mono_rptr = 0;
   *userq->mono_wptr = 0;

   return true;
}

bool
amdgpu_userq_init(struct amdgpu_winsys *aws, struct amdgpu_userq *userq, enum amd_ip_type ip_type)
{
   int r = -1;
   uint32_t hw_ip_type;
   struct drm_amdgpu_userq_mqd_gfx_v11_0 mqd;

   simple_mtx_lock(&userq->lock);

   if (userq->init_once) {
      simple_mtx_unlock(&userq->lock);
      return true;
   }

   userq->ip_type = ip_type;

   if (!amdgpu_ring_init(aws, userq)) {
      fprintf(stderr, "amdgpu: userq ring init failed\n");
      goto fail;
   }

   switch (userq->ip_type) {
   case AMD_IP_GFX:
      hw_ip_type = AMDGPU_HW_IP_GFX;
      userq->gfx_data.gds_bo = amdgpu_bo_create(aws, aws->info.gart_page_size, 256,
                                                RADEON_DOMAIN_VRAM, RADEON_FLAG_NO_SUBALLOC);
      if (!userq->gfx_data.gds_bo) {
         fprintf(stderr, "amdgpu: failed to allocate gds\n");
         goto fail;
      }

      userq->gfx_data.csa_bo = amdgpu_bo_create(aws, aws->info.fw_based_mcbp.csa_size,
                                                aws->info.fw_based_mcbp.csa_alignment,
                                                RADEON_DOMAIN_VRAM,
                                                RADEON_FLAG_NO_SUBALLOC);
      if (!userq->gfx_data.csa_bo) {
         fprintf(stderr, "amdgpu: failed to allocate csa\n");
         goto fail;
      }

      userq->gfx_data.shadow_bo = amdgpu_bo_create(aws, aws->info.fw_based_mcbp.shadow_size,
                                                   aws->info.fw_based_mcbp.shadow_alignment,
                                                   RADEON_DOMAIN_VRAM,
                                                   RADEON_FLAG_NO_SUBALLOC);
      if (!userq->gfx_data.shadow_bo) {
         fprintf(stderr, "amdgpu: failed to allocate shadow space\n");
         goto fail;
      }

      mqd.shadow_va = amdgpu_bo_get_va(userq->gfx_data.shadow_bo);
      mqd.gds_va = amdgpu_bo_get_va(userq->gfx_data.gds_bo);
      mqd.csa_va = amdgpu_bo_get_va(userq->gfx_data.csa_bo);
      break;
   case AMD_IP_COMPUTE:
      hw_ip_type = AMDGPU_HW_IP_COMPUTE;
      userq->compute_data.eop_bo = amdgpu_bo_create(aws, aws->info.gart_page_size, 256,
                                                    RADEON_DOMAIN_VRAM, RADEON_FLAG_NO_SUBALLOC);
      if (!userq->compute_data.eop_bo) {
         fprintf(stderr, "amdgpu: failed to allocate eop\n");
         goto fail;
      }

      mqd.eop_va = amdgpu_bo_get_va(userq->compute_data.eop_bo);
      break;
   case AMD_IP_SDMA:
      hw_ip_type = AMDGPU_HW_IP_DMA;
      break;
   default:
      fprintf(stderr, "amdgpu: unsupported userq for ip = %d\n", userq->ip_type);
   };

   struct amdgpu_bo_alloc_request req = {0};
   req.alloc_size = aws->info.gart_page_size;
   req.preferred_heap = AMDGPU_GEM_DOMAIN_DOORBELL;
   req.flags = 0;
   r = amdgpu_bo_alloc(aws->dev, &req, &userq->doorbell_bo_handle);
   if (r) {
      fprintf(stderr, "amdgpu: failed to allocate doorbell\n");
      goto fail;
   }

   r = amdgpu_bo_cpu_map(userq->doorbell_bo_handle, (void**)&userq->doorbell_ptr);
   if (r) {
      fprintf(stderr, "amdgpu: failed to map doorbell\n");
      goto fail;
   }

   mqd.queue_va = amdgpu_bo_get_va(userq->ring_bo);
   mqd.rptr_va = amdgpu_bo_get_va(userq->rptr_bo);
   mqd.wptr_va = amdgpu_bo_get_va(userq->wptr_bo);
   mqd.queue_size = AMDGPU_USERQ_RING_SIZE;

   uint32_t db_handle;
   amdgpu_userqueue_get_bo_handle(userq->doorbell_bo_handle, &db_handle);

   /* Create the Usermode Queue */
   r = amdgpu_create_userqueue(aws->dev, &mqd, hw_ip_type, db_handle,
                               AMDGPU_USERQ_DOORBELL_INDEX, &userq->q_id);
   if (r)
      fprintf(stderr, "amdgpu: failed to create userq\n");
fail:
   userq->init_once = true;
   simple_mtx_unlock(&userq->lock);
   if (r)
      return false;
   else
      return true;
}

void
amdgpu_userq_free(struct amdgpu_winsys *aws, struct amdgpu_userq *userq)
{
   if (userq->q_id)
      amdgpu_free_userqueue(aws->dev, userq->q_id);

   radeon_bo_reference(&aws->dummy_sws.base, &userq->ring_bo, NULL);
   radeon_bo_reference(&aws->dummy_sws.base, &userq->rptr_bo, NULL);
   radeon_bo_reference(&aws->dummy_sws.base, &userq->wptr_bo, NULL);

   if (userq->doorbell_ptr)
      amdgpu_bo_cpu_unmap(userq->doorbell_bo_handle);
   if (userq->doorbell_bo_handle)
      amdgpu_bo_free(userq->doorbell_bo_handle);

   switch (userq->ip_type) {
   case AMD_IP_GFX:
      radeon_bo_reference(&aws->dummy_sws.base, &userq->gfx_data.gds_bo, NULL);
      radeon_bo_reference(&aws->dummy_sws.base, &userq->gfx_data.csa_bo, NULL);
      radeon_bo_reference(&aws->dummy_sws.base, &userq->gfx_data.shadow_bo, NULL);
      break;
   case AMD_IP_COMPUTE:
      radeon_bo_reference(&aws->dummy_sws.base, &userq->compute_data.eop_bo, NULL);
      break;
   case AMD_IP_SDMA:
      break;
   default:
      fprintf(stderr, "amdgpu: unsupported userq for ip = %d\n", userq->ip_type);
   };
}
