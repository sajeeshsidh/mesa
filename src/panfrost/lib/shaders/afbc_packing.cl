#include <libpan.h>
#include "gen_macros.h"

uint32_t
libpan_get_superblock_size(__constant uint32_t *src,
                           uint32_t block_idx, uint32_t uncompressed_size)
{
   uint4 block_packed = ((__constant uint4*)src)[block_idx];
   pan_unpack(&block_packed, AFBC_HEADER, header);

   /* When the first subblock size is set to zero, the whole superblock is
    * filled with a solid color specified in the header */
   if (PAN_ARCH >= 7 && header.sublock_size_0 == 0)
      return 0;
   uint32_t size = 0;
#define ADD_SUBLOCK(n)                                                                   \
   size += header.sublock_size_##n == 1 ? uncompressed_size : header.sublock_size_##n;   \

   ADD_SUBLOCK(0);
   ADD_SUBLOCK(1);
   ADD_SUBLOCK(2);
   ADD_SUBLOCK(3);
   ADD_SUBLOCK(4);
   ADD_SUBLOCK(5);
   ADD_SUBLOCK(6);
   ADD_SUBLOCK(7);
   ADD_SUBLOCK(8);
   ADD_SUBLOCK(9);
   ADD_SUBLOCK(10);
   ADD_SUBLOCK(11);
   ADD_SUBLOCK(12);
   ADD_SUBLOCK(13);
   ADD_SUBLOCK(14);
   ADD_SUBLOCK(15);

   return size;
}

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

void
libpan_copy_superblock(uint32_t *dst, uint32_t dst_idx, uint64_t hdr_sz,
                       uint32_t *src, uint32_t src_idx,
                       __constant struct mali_afbc_block_info_packed *metadata, uint32_t meta_idx)
{

   uint4 hdr = vload4(src_idx, src);
   uint64_t src_bodyptr = (uint64_t)src + hdr.x;

   struct mali_afbc_block_info_packed block_info_packed = metadata[meta_idx];
   pan_unpack(&block_info_packed, AFBC_BLOCK_INFO, meta_entry);
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
