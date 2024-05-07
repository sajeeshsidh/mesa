#include <libpan.h>

/* repeat bit pattern shifted left by 16 bits */
#define SHORT_REP(x) (x | (x << 16))

uint32_t
libpan_get_morton_index(uint32_t idx, uint32_t src_stride, uint32_t dst_stride) {
   uint32_t x = idx % dst_stride;
   uint32_t y = idx / dst_stride;

   uint32_t offset = (y & ~0x7) * src_stride + ((x >> 3) << 6);

   x = x & 0x7 | ((y & 0x7) << 16);
   x = (x | x << 2) & SHORT_REP(0x13);
   x = (x | x << 1) & SHORT_REP(0x15);

   return offset + ((x & 0xff) | (x >> 15));
}

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
