#include "pan_cl.h"

#include "gen_macros.h"
#include "gen_shaders.h"

const unsigned *
GENX(panfrost_get_libpanfrost_shaders_nir)(void)
{
   return libpanfrost_shaders_nir;
}

size_t
GENX(panfrost_get_libpanfrost_shaders_nir_size)(void)
{
   return sizeof(libpanfrost_shaders_nir);
}
