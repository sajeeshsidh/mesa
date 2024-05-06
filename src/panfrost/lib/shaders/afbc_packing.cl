#include <libpan.h>

struct pan_afbc_block_info {
   uint32_t size;
   uint32_t offset;
};

PAN_STATIC_ASSERT(sizeof(struct pan_afbc_block_info) == 8);

void
libpan_copy_superblock(uint32_t *dst, uint32_t dst_idx, uint64_t hdr_sz,
                       uint32_t *src, uint32_t src_idx,
                       struct pan_afbc_block_info *metadata, uint32_t meta_idx)
{

   uint4 hdr = vload4(src_idx, src);
   uint64_t src_bodyptr = (uint64_t)src + hdr.x;

   struct pan_afbc_block_info meta_entry = metadata[meta_idx];
   uint64_t dst_body_base_ptr = meta_entry.offset + hdr_sz;
   uint64_t dst_bodyptr = (uint64_t) dst + dst_body_base_ptr;
   uint32_t size = meta_entry.size;

   if (hdr.x != 0)
      hdr.x = dst_body_base_ptr;
   vstore4(hdr, dst_idx, dst);

   uint32_t *wdst = (uint32_t*)dst_bodyptr;
   uint32_t *wsrc = (uint32_t*)src_bodyptr;
   for (uint32_t offset = 0; offset < size / 4; offset += 1) {
      wdst[offset] = wsrc[offset];
   }
}
