/*
 * Copyright © 2023 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMDGPU_USERQ_H
#define AMDGPU_USERQ_H

#ifdef __cplusplus
extern "C" {
#endif

/* ring size should be power of 2 and enough to hold AMDGPU_FENCE_RING_SIZE ibs */
#define AMDGPU_USERQ_RING_SIZE 0x10000
#define AMDGPU_USERQ_RING_SIZE_DW (AMDGPU_USERQ_RING_SIZE >> 2)
#define AMDGPU_USERQ_RING_SIZE_DW_MASK (AMDGPU_USERQ_RING_SIZE_DW - 1)

#define AMDGPU_USERQ_DOORBELL_INDEX 4

#define amdgpu_pkt_begin() uint32_t num_dw_written = 0; \
   uint32_t ring_start = (uint32_t)*userq->mono_wptr & AMDGPU_USERQ_RING_SIZE_DW_MASK; \
   uint32_t *ring_wptr_cur;

#define amdgpu_pkt_add_dw(value) do { \
   ring_wptr_cur = userq->ring_base_ptr + \
      ((ring_start + num_dw_written) & AMDGPU_USERQ_RING_SIZE_DW_MASK); \
   *ring_wptr_cur = value; \
   num_dw_written++; \
} while (0)

#define amdgpu_pkt_end() do { \
   *userq->mono_wptr += num_dw_written; \
} while (0)

struct amdgpu_winsys;

struct amdgpu_userq_gfx_data {
   struct pb_buffer_lean *gds_bo;
   struct pb_buffer_lean *csa_bo;
   struct pb_buffer_lean *shadow_bo;
};

struct amdgpu_userq_compute_data {
   struct pb_buffer_lean *eop_bo;
};

struct amdgpu_userq {
   /* ring buffer allocation */
   struct pb_buffer_lean *ring_bo;
   uint32_t *ring_base_ptr;

   /* rptr allocation */
   struct pb_buffer_lean *rptr_bo;
   uint64_t *mono_rptr;

   /* wptr allocation */
   struct pb_buffer_lean *wptr_bo;
   uint64_t *mono_wptr;

   amdgpu_bo_handle doorbell_bo_handle;
   uint64_t *doorbell_ptr;

   uint32_t q_id;
   enum amd_ip_type ip_type;
   uint32_t init_once;
   simple_mtx_t lock;

   uint64_t user_fence_seq_num;

   union {
      struct amdgpu_userq_gfx_data gfx_data;
      struct amdgpu_userq_compute_data compute_data;
   };
};

bool
amdgpu_userq_init(struct amdgpu_winsys *aws, struct amdgpu_userq *userq, enum amd_ip_type ip_type);
void
amdgpu_userq_free(struct amdgpu_winsys *aws, struct amdgpu_userq *userq);

#ifdef __cplusplus
}
#endif

#endif
