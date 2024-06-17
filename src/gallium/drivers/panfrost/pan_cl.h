#ifndef PAN_CL_H_
#define PAN_CL_H_

#include "util/macros.h"

const unsigned * panfrost_get_libpanfrost_shaders_nir_v4(void);
const unsigned * panfrost_get_libpanfrost_shaders_nir_v5(void);
const unsigned * panfrost_get_libpanfrost_shaders_nir_v6(void);
const unsigned * panfrost_get_libpanfrost_shaders_nir_v7(void);
const unsigned * panfrost_get_libpanfrost_shaders_nir_v9(void);
const unsigned * panfrost_get_libpanfrost_shaders_nir_v10(void);

const size_t panfrost_get_libpanfrost_shaders_nir_size_v4(void);
const size_t panfrost_get_libpanfrost_shaders_nir_size_v5(void);
const size_t panfrost_get_libpanfrost_shaders_nir_size_v6(void);
const size_t panfrost_get_libpanfrost_shaders_nir_size_v7(void);
const size_t panfrost_get_libpanfrost_shaders_nir_size_v9(void);
const size_t panfrost_get_libpanfrost_shaders_nir_size_v10(void);

static inline const unsigned *
panfrost_get_libpanfrost_shaders_nir(unsigned arch)
{
   if (arch == 4)
      return panfrost_get_libpanfrost_shaders_nir_v4();
   else if (arch == 5)
      return panfrost_get_libpanfrost_shaders_nir_v5();
   else if (arch == 6)
      return panfrost_get_libpanfrost_shaders_nir_v6();
   else if (arch == 7)
      return panfrost_get_libpanfrost_shaders_nir_v7();
   else if (arch == 9)
      return panfrost_get_libpanfrost_shaders_nir_v9();
   else if (arch == 10)
      return panfrost_get_libpanfrost_shaders_nir_v10();
   else
      unreachable("Unhandled architecture major");
}

static inline size_t
panfrost_get_libpanfrost_shaders_nir_size(unsigned arch)
{
   if (arch == 4)
      return panfrost_get_libpanfrost_shaders_nir_size_v4();
   else if (arch == 5)
      return panfrost_get_libpanfrost_shaders_nir_size_v5();
   else if (arch == 6)
      return panfrost_get_libpanfrost_shaders_nir_size_v6();
   else if (arch == 7)
      return panfrost_get_libpanfrost_shaders_nir_size_v7();
   else if (arch == 9)
      return panfrost_get_libpanfrost_shaders_nir_size_v9();
   else if (arch == 10)
      return panfrost_get_libpanfrost_shaders_nir_size_v10();
   else
      unreachable("Unhandled architecture major");
}

#endif // PAN_CL_H_
