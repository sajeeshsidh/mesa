/*
 * Copyright (C) 2013 Rob Clark <robclark@freedesktop.org>
 * Copyright (C) 2014-2016 Emil Velikov <emil.l.velikov@gmail.com>
 * Copyright (C) 2016 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/param.h>
#ifdef MAJOR_IN_MKDEV
#include <sys/mkdev.h>
#endif
#ifdef MAJOR_IN_SYSMACROS
#include <sys/sysmacros.h>
#endif
#include <GL/gl.h>
#include "mesa_interface.h"
#include "loader.h"
#include "util/libdrm.h"
#include "util/os_file.h"
#include "util/os_misc.h"
#include "util/u_debug.h"
#include "git_sha1.h"

#include "drm-uapi/nouveau_drm.h"

#define MAX_DRM_DEVICES 64

#ifdef USE_DRICONF
#include "util/xmlconfig.h"
#include "util/driconf.h"
#endif

#include "util/macros.h"

#define __IS_LOADER
#include "pci_id_driver_map.h"

/* For systems like Hurd */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static void default_logger(int level, const char *fmt, ...)
{
   if (level <= _LOADER_WARNING) {
      va_list args;
      va_start(args, fmt);
      vfprintf(stderr, fmt, args);
      va_end(args);
   }
}

static loader_logger *log_ = default_logger;

int
loader_open_device(const char *device_name)
{
   int fd;
#ifdef O_CLOEXEC
   fd = open(device_name, O_RDWR | O_CLOEXEC);
   if (fd == -1 && errno == EINVAL)
#endif
   {
      fd = open(device_name, O_RDWR);
      if (fd != -1)
         fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
   }
   if (fd == -1 && errno == EACCES) {
      log_(_LOADER_WARNING, "failed to open %s: %s\n",
           device_name, strerror(errno));
   }
   return fd;
}

char *
loader_get_kernel_driver_name(int fd)
{
   char *driver;
   drmVersionPtr version = drmGetVersion(fd);

   if (!version) {
      log_(_LOADER_WARNING, "failed to get driver name for fd %d\n", fd);
      return NULL;
   }

   driver = strndup(version->name, version->name_len);
   log_(driver ? _LOADER_DEBUG : _LOADER_WARNING, "using driver %s for %d\n",
        driver, fd);

   drmFreeVersion(version);
   return driver;
}

bool
iris_predicate(int fd, const char *driver)
{
   char *kernel_driver = loader_get_kernel_driver_name(fd);
   bool ret = kernel_driver && (strcmp(kernel_driver, "i915") == 0 ||
                                strcmp(kernel_driver, "xe") == 0);

   free(kernel_driver);
   return ret;
}

/* choose zink or nouveau GL */
bool
nouveau_zink_predicate(int fd, const char *driver)
{
#if !defined(HAVE_NVK) || !defined(HAVE_ZINK)
   if (!strcmp(driver, "zink"))
      return false;
   return true;
#else

   bool prefer_zink = false;

   /* enable this once zink is up to speed.
    * struct drm_nouveau_getparam r = { .param = NOUVEAU_GETPARAM_CHIPSET_ID };
    * int ret = drmCommandWriteRead(fd, DRM_NOUVEAU_GETPARAM, &r, sizeof(r));
    * if (ret == 0 && (r.value & ~0xf) >= 0x160)
    *    prefer_zink = true;
    */

   prefer_zink = debug_get_bool_option("NOUVEAU_USE_ZINK", prefer_zink);

   if (prefer_zink && !strcmp(driver, "zink"))
      return true;

   if (!prefer_zink && !strcmp(driver, "nouveau"))
      return true;
   return false;
#endif
}


/**
 * Goes through all the platform devices whose driver is on the given list and
 * try to open their render node. It returns the fd of the first device that
 * it can open.
 */
int
loader_open_render_node_platform_device(const char * const drivers[],
                                        unsigned int n_drivers)
{
   drmDevicePtr devices[MAX_DRM_DEVICES], device;
   int num_devices, fd = -1;
   int i, j;
   bool found = false;

   num_devices = drmGetDevices2(0, devices, MAX_DRM_DEVICES);
   if (num_devices <= 0)
      return -ENOENT;

   for (i = 0; i < num_devices; i++) {
      device = devices[i];

      if ((device->available_nodes & (1 << DRM_NODE_RENDER)) &&
          (device->bustype == DRM_BUS_PLATFORM)) {
         drmVersionPtr version;

         fd = loader_open_device(device->nodes[DRM_NODE_RENDER]);
         if (fd < 0)
            continue;

         version = drmGetVersion(fd);
         if (!version) {
            close(fd);
            continue;
         }

         for (j = 0; j < n_drivers; j++) {
            if (strcmp(version->name, drivers[j]) == 0) {
               found = true;
               break;
            }
         }
         if (!found) {
            drmFreeVersion(version);
            close(fd);
            continue;
         }

         drmFreeVersion(version);
         break;
      }
   }
   drmFreeDevices(devices, num_devices);

   if (i == num_devices)
      return -ENOENT;

   return fd;
}

bool
loader_is_device_render_capable(int fd)
{
   drmDevicePtr dev_ptr;
   bool ret;

   if (drmGetDevice2(fd, 0, &dev_ptr) != 0)
      return false;

   ret = (dev_ptr->available_nodes & (1 << DRM_NODE_RENDER));

   drmFreeDevice(&dev_ptr);

   return ret;
}

char *
loader_get_render_node(dev_t device)
{
   char *render_node = NULL;
   drmDevicePtr dev_ptr;

   if (drmGetDeviceFromDevId(device, 0, &dev_ptr) < 0)
      return NULL;

   if (dev_ptr->available_nodes & (1 << DRM_NODE_RENDER)) {
      render_node = strdup(dev_ptr->nodes[DRM_NODE_RENDER]);
      if (!render_node)
         log_(_LOADER_DEBUG, "MESA-LOADER: failed to allocate memory for render node\n");
   }

   drmFreeDevice(&dev_ptr);

   return render_node;
}

#ifdef USE_DRICONF
static const driOptionDescription __driConfigOptionsLoader[] = {
    DRI_CONF_SECTION_INITIALIZATION
        DRI_CONF_DEVICE_ID_PATH_TAG()
        DRI_CONF_DRI_DRIVER()
    DRI_CONF_SECTION_END
};

static char *loader_get_dri_config_driver(int fd)
{
   driOptionCache defaultInitOptions;
   driOptionCache userInitOptions;
   char *dri_driver = NULL;
   char *kernel_driver = loader_get_kernel_driver_name(fd);

   driParseOptionInfo(&defaultInitOptions, __driConfigOptionsLoader,
                      ARRAY_SIZE(__driConfigOptionsLoader));
   driParseConfigFiles(&userInitOptions, &defaultInitOptions, 0,
                       "loader", kernel_driver, NULL, NULL, 0, NULL, 0);
   if (driCheckOption(&userInitOptions, "dri_driver", DRI_STRING)) {
      char *opt = driQueryOptionstr(&userInitOptions, "dri_driver");
      /* not an empty string */
      if (*opt)
         dri_driver = strdup(opt);
   }
   driDestroyOptionCache(&userInitOptions);
   driDestroyOptionInfo(&defaultInitOptions);

   free(kernel_driver);
   return dri_driver;
}

static char *loader_get_dri_config_device_id(void)
{
   driOptionCache defaultInitOptions;
   driOptionCache userInitOptions;
   char *prime = NULL;

   driParseOptionInfo(&defaultInitOptions, __driConfigOptionsLoader,
                      ARRAY_SIZE(__driConfigOptionsLoader));
   driParseConfigFiles(&userInitOptions, &defaultInitOptions, 0,
                       "loader", NULL, NULL, NULL, 0, NULL, 0);
   if (driCheckOption(&userInitOptions, "device_id", DRI_STRING)) {
      char *opt = driQueryOptionstr(&userInitOptions, "device_id");
      if (*opt)
         prime = strdup(opt);
   }
   driDestroyOptionCache(&userInitOptions);
   driDestroyOptionInfo(&defaultInitOptions);

   return prime;
}
#endif

static char *drm_construct_id_path_tag(drmDevicePtr device)
{
   char *tag = NULL;

   if (device->bustype == DRM_BUS_PCI) {
      if (asprintf(&tag, "pci-%04x_%02x_%02x_%1u",
                   device->businfo.pci->domain,
                   device->businfo.pci->bus,
                   device->businfo.pci->dev,
                   device->businfo.pci->func) < 0) {
         return NULL;
      }
   } else if (device->bustype == DRM_BUS_PLATFORM ||
              device->bustype == DRM_BUS_HOST1X) {
      char *fullname, *name, *address;

      if (device->bustype == DRM_BUS_PLATFORM)
         fullname = device->businfo.platform->fullname;
      else
         fullname = device->businfo.host1x->fullname;

      name = strrchr(fullname, '/');
      if (!name)
         name = strdup(fullname);
      else
         name = strdup(name + 1);

      address = strchr(name, '@');
      if (address) {
         *address++ = '\0';

         if (asprintf(&tag, "platform-%s_%s", address, name) < 0)
            tag = NULL;
      } else {
         if (asprintf(&tag, "platform-%s", name) < 0)
            tag = NULL;
      }

      free(name);
   }
   return tag;
}

static bool drm_device_matches_tag(drmDevicePtr device, const char *prime_tag)
{
   char *tag = drm_construct_id_path_tag(device);
   int ret;

   if (tag == NULL)
      return false;

   ret = strcmp(tag, prime_tag);

   free(tag);
   return ret == 0;
}

static char *drm_get_id_path_tag_for_fd(int fd)
{
   drmDevicePtr device;
   char *tag;

   if (drmGetDevice2(fd, 0, &device) != 0)
       return NULL;

   tag = drm_construct_id_path_tag(device);
   drmFreeDevice(&device);
   return tag;
}

bool loader_get_user_preferred_fd(int *fd_render_gpu, int *original_fd)
{
   const char *dri_prime = getenv("DRI_PRIME");
   bool debug = debug_get_bool_option("DRI_PRIME_DEBUG", false);
   char *default_tag = NULL;
   drmDevicePtr devices[MAX_DRM_DEVICES];
   int i, num_devices, fd = -1;
   struct {
      enum {
         PRIME_IS_INTEGER,
         PRIME_IS_VID_DID,
         PRIME_IS_PCI_TAG
      } semantics;
      union {
         int as_integer;
         struct {
            uint16_t v, d;
         } as_vendor_device_ids;
      } v;
      char *str;
   } prime = {};
   prime.str = NULL;

   if (dri_prime)
      prime.str = strdup(dri_prime);
#ifdef USE_DRICONF
   else
      prime.str = loader_get_dri_config_device_id();
#endif

   if (prime.str == NULL) {
      goto no_prime_gpu_offloading;
   } else {
      uint16_t vendor_id, device_id;
      if (sscanf(prime.str, "%hx:%hx", &vendor_id, &device_id) == 2) {
         prime.semantics = PRIME_IS_VID_DID;
         prime.v.as_vendor_device_ids.v = vendor_id;
         prime.v.as_vendor_device_ids.d = device_id;
      } else {
         int i = atoi(prime.str);
         if (i < 0 || strcmp(prime.str, "0") == 0) {
            printf("Invalid value (%d) for DRI_PRIME. Should be > 0\n", i);
            goto err;
         } else if (i == 0) {
            prime.semantics = PRIME_IS_PCI_TAG;
         } else {
            prime.semantics = PRIME_IS_INTEGER;
            prime.v.as_integer = i;
         }
      }
   }

   default_tag = drm_get_id_path_tag_for_fd(*fd_render_gpu);
   if (default_tag == NULL)
      goto err;

   num_devices = drmGetDevices2(0, devices, MAX_DRM_DEVICES);
   if (num_devices <= 0)
      goto err;

   if (debug) {
      log_(_LOADER_WARNING, "DRI_PRIME: %d devices\n", num_devices);
      for (i = 0; i < num_devices; i++) {
         log_(_LOADER_WARNING, "  %d:", i);
         if (!(devices[i]->available_nodes & 1 << DRM_NODE_RENDER)) {
            log_(_LOADER_WARNING, "not a render node -> not usable\n");
            continue;
         }
         char *tag = drm_construct_id_path_tag(devices[i]);
         if (tag) {
            log_(_LOADER_WARNING, " %s", tag);
            free(tag);
         }
         if (devices[i]->bustype == DRM_BUS_PCI) {
            log_(_LOADER_WARNING, " %4x:%4x",
               devices[i]->deviceinfo.pci->vendor_id,
               devices[i]->deviceinfo.pci->device_id);
         }
         log_(_LOADER_WARNING, " %s", devices[i]->nodes[DRM_NODE_RENDER]);

         if (drm_device_matches_tag(devices[i], default_tag)) {
            log_(_LOADER_WARNING, " [default]");
         }
         log_(_LOADER_WARNING, "\n");
      }
   }

   if (prime.semantics == PRIME_IS_INTEGER &&
       prime.v.as_integer >= num_devices) {
      printf("Inconsistent value (%d) for DRI_PRIME. Should be < %d "
             "(GPU devices count). Using: %d\n",
             prime.v.as_integer, num_devices, num_devices - 1);
      prime.v.as_integer = num_devices - 1;
   }

   for (i = 0; i < num_devices; i++) {
      if (!(devices[i]->available_nodes & 1 << DRM_NODE_RENDER))
         continue;

      log_(debug ? _LOADER_WARNING : _LOADER_INFO, "DRI_PRIME: device %d ", i);

      /* three formats of DRI_PRIME are supported:
       * "N": a >= 1 integer value. Select the Nth GPU, skipping the
       *      default one.
       * id_path_tag: (for example "pci-0000_02_00_0") choose the card
       * with this id_path_tag.
       * vendor_id:device_id
       */
      switch (prime.semantics) {
         case PRIME_IS_INTEGER: {
            /* Skip the default device */
            if (drm_device_matches_tag(devices[i], default_tag)) {
               log_(debug ? _LOADER_WARNING : _LOADER_INFO,
                    "skipped (default device)\n");
               continue;
            }
            prime.v.as_integer--;

            /* Skip more GPUs? */
            if (prime.v.as_integer) {
               log_(debug ? _LOADER_WARNING : _LOADER_INFO,
                    "skipped (%d more to skip)\n", prime.v.as_integer - 1);
               continue;
            }
            log_(debug ? _LOADER_WARNING : _LOADER_INFO, " -> ");
            break;
         }
         case PRIME_IS_VID_DID: {
            if (devices[i]->bustype == DRM_BUS_PCI &&
                devices[i]->deviceinfo.pci->vendor_id == prime.v.as_vendor_device_ids.v &&
                devices[i]->deviceinfo.pci->device_id == prime.v.as_vendor_device_ids.d) {
               /* Update prime for the "different_device"
                * determination below. */
               free(prime.str);
               prime.str = drm_construct_id_path_tag(devices[i]);
               log_(debug ? _LOADER_WARNING : _LOADER_INFO,
                    " - vid:did match -> ");
               break;
            } else {
               log_(debug ? _LOADER_WARNING : _LOADER_INFO,
                    "skipped (vid:did didn't match)\n");
            }
            continue;
         }
         case PRIME_IS_PCI_TAG: {
            if (!drm_device_matches_tag(devices[i], prime.str)) {
               log_(debug ? _LOADER_WARNING : _LOADER_INFO,
                    "skipped (pci id tag didn't match)\n");
               continue;
            }
            log_(debug ? _LOADER_WARNING : _LOADER_INFO, " - pci tag match -> ");
            break;
         }
      }

      log_(debug ? _LOADER_WARNING : _LOADER_INFO,
           "selected (%s)\n", devices[i]->nodes[DRM_NODE_RENDER]);
      fd = loader_open_device(devices[i]->nodes[DRM_NODE_RENDER]);
      break;
   }
   drmFreeDevices(devices, num_devices);

   if (i == num_devices)
      goto err;

   if (fd < 0) {
      log_(debug ? _LOADER_WARNING : _LOADER_INFO,
           "DRI_PRIME: failed to open '%s'\n",
           devices[i]->nodes[DRM_NODE_RENDER]);

      goto err;
   }

   bool is_render_and_display_gpu_diff = !!strcmp(default_tag, prime.str);
   if (original_fd) {
      if (is_render_and_display_gpu_diff) {
         *original_fd = *fd_render_gpu;
         *fd_render_gpu = fd;
      } else {
         *original_fd = *fd_render_gpu;
         close(fd);
      }
   } else {
      close(*fd_render_gpu);
      *fd_render_gpu = fd;
   }

   free(default_tag);
   free(prime.str);
   return is_render_and_display_gpu_diff;
 err:
   log_(debug ? _LOADER_WARNING : _LOADER_INFO,
        "DRI_PRIME: error. Using the default GPU\n");
   free(default_tag);
   free(prime.str);
 no_prime_gpu_offloading:
   if (original_fd)
      *original_fd = *fd_render_gpu;
   return false;
}

static bool
drm_get_pci_id_for_fd(int fd, int *vendor_id, int *chip_id)
{
   drmDevicePtr device;

   if (drmGetDevice2(fd, 0, &device) != 0) {
      log_(_LOADER_WARNING, "MESA-LOADER: failed to retrieve device information\n");
      return false;
   }

   if (device->bustype != DRM_BUS_PCI) {
      drmFreeDevice(&device);
      log_(_LOADER_DEBUG, "MESA-LOADER: device is not located on the PCI bus\n");
      return false;
   }

   *vendor_id = device->deviceinfo.pci->vendor_id;
   *chip_id = device->deviceinfo.pci->device_id;
   drmFreeDevice(&device);
   return true;
}

#ifdef __linux__
static int loader_get_linux_pci_field(int maj, int min, const char *field)
{
   char path[PATH_MAX + 1];
   snprintf(path, sizeof(path), "/sys/dev/char/%d:%d/device/%s", maj, min, field);

   char *field_str = os_read_file(path, NULL);
   if (!field_str) {
      /* Probably non-PCI device. */
      return 0;
   }

   int value = (int)strtoll(field_str, NULL, 16);
   free(field_str);

   return value;
}

static bool
loader_get_linux_pci_id_for_fd(int fd, int *vendor_id, int *chip_id)
{
   struct stat sbuf;
   if (fstat(fd, &sbuf) != 0) {
      log_(_LOADER_DEBUG, "MESA-LOADER: failed to fstat fd\n");
      return false;
   }

   int maj = major(sbuf.st_rdev);
   int min = minor(sbuf.st_rdev);

   *vendor_id = loader_get_linux_pci_field(maj, min, "vendor");
   *chip_id = loader_get_linux_pci_field(maj, min, "device");

   return *vendor_id && *chip_id;
}
#endif /* __linux__ */

bool
loader_get_pci_id_for_fd(int fd, int *vendor_id, int *chip_id)
{
#ifdef __linux__
   /* Implementation without causing full enumeration of DRM devices. */
   if (loader_get_linux_pci_id_for_fd(fd, vendor_id, chip_id))
      return true;
#endif

   return drm_get_pci_id_for_fd(fd, vendor_id, chip_id);
}

char *
loader_get_device_name_for_fd(int fd)
{
   return drmGetDeviceNameFromFd2(fd);
}

static char *
loader_get_pci_driver(int fd)
{
   int vendor_id, chip_id, i, j;
   char *driver = NULL;

   if (!loader_get_pci_id_for_fd(fd, &vendor_id, &chip_id))
      return NULL;

   for (i = 0; i < ARRAY_SIZE(driver_map); i++) {
      if (vendor_id != driver_map[i].vendor_id)
         continue;

      if (driver_map[i].predicate && !driver_map[i].predicate(fd, driver_map[i].driver))
         continue;

      if (driver_map[i].num_chips_ids == -1) {
         driver = strdup(driver_map[i].driver);
         goto out;
      }

      for (j = 0; j < driver_map[i].num_chips_ids; j++)
         if (driver_map[i].chip_ids[j] == chip_id) {
            driver = strdup(driver_map[i].driver);
            goto out;
         }
   }

out:
   log_(driver ? _LOADER_DEBUG : _LOADER_WARNING,
         "pci id for fd %d: %04x:%04x, driver %s\n",
         fd, vendor_id, chip_id, driver);
   return driver;
}

char *
loader_get_driver_for_fd(int fd)
{
   char *driver;

   /* Allow an environment variable to force choosing a different driver
    * binary.  If that driver binary can't survive on this FD, that's the
    * user's problem, but this allows vc4 simulator to run on an i965 host,
    * and may be useful for some touch testing of i915 on an i965 host.
    */
   if (__normal_user()) {
      const char *override = os_get_option("MESA_LOADER_DRIVER_OVERRIDE");
      if (override)
         return strdup(override);
   }

#if defined(USE_DRICONF)
   driver = loader_get_dri_config_driver(fd);
   if (driver)
      return driver;
#endif

   driver = loader_get_pci_driver(fd);
   if (!driver)
      driver = loader_get_kernel_driver_name(fd);

   return driver;
}

void
loader_set_logger(loader_logger *logger)
{
   log_ = logger;
}

#define DEFINE_LOADER_DRM_ENTRYPOINT(drivername)                          \
const __DRIextension **__driDriverGetExtensions_##drivername(void);       \

#if defined(HAVE_SOFTPIPE) || defined(HAVE_LLVMPIPE)
DEFINE_LOADER_DRM_ENTRYPOINT(swrast);

#if defined(HAVE_LIBDRM)
DEFINE_LOADER_DRM_ENTRYPOINT(kms_swrast);
#endif
#endif

#if defined(HAVE_I915)
DEFINE_LOADER_DRM_ENTRYPOINT(i915)
#endif

#if defined(HAVE_IRIS)
DEFINE_LOADER_DRM_ENTRYPOINT(iris)
#endif

#if defined(HAVE_CROCUS)
DEFINE_LOADER_DRM_ENTRYPOINT(crocus)
#endif

#if defined(HAVE_NOUVEAU)
DEFINE_LOADER_DRM_ENTRYPOINT(nouveau)
#endif

#if defined(HAVE_R300)
DEFINE_LOADER_DRM_ENTRYPOINT(r300)
#endif

#if defined(HAVE_R600)
DEFINE_LOADER_DRM_ENTRYPOINT(r600)
#endif

#if defined(HAVE_RADEONSI)
DEFINE_LOADER_DRM_ENTRYPOINT(radeonsi)
#endif

#if defined(HAVE_VMWGFX)
DEFINE_LOADER_DRM_ENTRYPOINT(vmwgfx)
#endif

#if defined(HAVE_FREEDRENO)
DEFINE_LOADER_DRM_ENTRYPOINT(msm)
DEFINE_LOADER_DRM_ENTRYPOINT(kgsl)
#endif

#if defined(HAVE_VIRGL) || (defined(HAVE_FREEDRENO) && !defined(PIPE_LOADER_DYNAMIC))
DEFINE_LOADER_DRM_ENTRYPOINT(virtio_gpu)
#endif

#if defined(HAVE_V3D)
DEFINE_LOADER_DRM_ENTRYPOINT(v3d)
#endif

#if defined(HAVE_VC4)
DEFINE_LOADER_DRM_ENTRYPOINT(vc4)
#endif

#if defined(HAVE_PANFROST)
DEFINE_LOADER_DRM_ENTRYPOINT(panfrost)
DEFINE_LOADER_DRM_ENTRYPOINT(panthor)
#endif

#if defined(HAVE_ASAHI)
DEFINE_LOADER_DRM_ENTRYPOINT(asahi)
#endif

#if defined(HAVE_ETNAVIV)
DEFINE_LOADER_DRM_ENTRYPOINT(etnaviv)
#endif

#if defined(HAVE_TEGRA)
DEFINE_LOADER_DRM_ENTRYPOINT(tegra);
#endif

#if defined(HAVE_KMSRO)
DEFINE_LOADER_DRM_ENTRYPOINT(armada_drm)
DEFINE_LOADER_DRM_ENTRYPOINT(exynos)
DEFINE_LOADER_DRM_ENTRYPOINT(gm12u320)
DEFINE_LOADER_DRM_ENTRYPOINT(hdlcd)
DEFINE_LOADER_DRM_ENTRYPOINT(hx8357d)
DEFINE_LOADER_DRM_ENTRYPOINT(ili9163)
DEFINE_LOADER_DRM_ENTRYPOINT(ili9225)
DEFINE_LOADER_DRM_ENTRYPOINT(ili9341)
DEFINE_LOADER_DRM_ENTRYPOINT(ili9486)
DEFINE_LOADER_DRM_ENTRYPOINT(imx_drm)
DEFINE_LOADER_DRM_ENTRYPOINT(imx_dcss)
DEFINE_LOADER_DRM_ENTRYPOINT(imx_lcdif)
DEFINE_LOADER_DRM_ENTRYPOINT(ingenic_drm)
DEFINE_LOADER_DRM_ENTRYPOINT(kirin)
DEFINE_LOADER_DRM_ENTRYPOINT(komeda)
DEFINE_LOADER_DRM_ENTRYPOINT(mali_dp)
DEFINE_LOADER_DRM_ENTRYPOINT(mcde)
DEFINE_LOADER_DRM_ENTRYPOINT(mediatek)
DEFINE_LOADER_DRM_ENTRYPOINT(meson)
DEFINE_LOADER_DRM_ENTRYPOINT(mi0283qt)
DEFINE_LOADER_DRM_ENTRYPOINT(mxsfb_drm)
DEFINE_LOADER_DRM_ENTRYPOINT(panel_mipi_dbi)
DEFINE_LOADER_DRM_ENTRYPOINT(pl111)
DEFINE_LOADER_DRM_ENTRYPOINT(rcar_du)
DEFINE_LOADER_DRM_ENTRYPOINT(repaper)
DEFINE_LOADER_DRM_ENTRYPOINT(rockchip)
DEFINE_LOADER_DRM_ENTRYPOINT(rzg2l_du)
DEFINE_LOADER_DRM_ENTRYPOINT(ssd130x)
DEFINE_LOADER_DRM_ENTRYPOINT(st7586)
DEFINE_LOADER_DRM_ENTRYPOINT(st7735r)
DEFINE_LOADER_DRM_ENTRYPOINT(sti)
DEFINE_LOADER_DRM_ENTRYPOINT(stm)
DEFINE_LOADER_DRM_ENTRYPOINT(sun4i_drm)
DEFINE_LOADER_DRM_ENTRYPOINT(udl)
DEFINE_LOADER_DRM_ENTRYPOINT(vkms)
DEFINE_LOADER_DRM_ENTRYPOINT(zynqmp_dpsub)
#endif

#if defined(HAVE_LIMA)
DEFINE_LOADER_DRM_ENTRYPOINT(lima)
#endif

#if defined(HAVE_ZINK)
DEFINE_LOADER_DRM_ENTRYPOINT(zink);
#endif

#if defined(HAVE_D3D12)
DEFINE_LOADER_DRM_ENTRYPOINT(d3d12);
#endif


const __DRIextension **
loader_get_extensions(const char *driver_name, bool warn_on_fail)
{
#if defined(HAVE_I915)
   if (!strcmp(driver_name, "i915"))
      return __driDriverGetExtensions_i915();
#endif

#if defined(HAVE_IRIS)
   if (!strcmp(driver_name, "iris"))
      return __driDriverGetExtensions_iris();
#endif

#if defined(HAVE_CROCUS)
   if (!strcmp(driver_name, "crocus"))
      return __driDriverGetExtensions_crocus();
#endif

#if defined(HAVE_NOUVEAU)
   if (!strcmp(driver_name, "nouveau"))
      return __driDriverGetExtensions_nouveau();
#endif

#if defined(HAVE_R300)
   if (!strcmp(driver_name, "r300"))
      return __driDriverGetExtensions_r300();
#endif

#if defined(HAVE_R600)
   if (!strcmp(driver_name, "r600"))
      return __driDriverGetExtensions_r600();
#endif

#if defined(HAVE_RADEONSI)
   if (!strcmp(driver_name, "radeonsi"))
      return __driDriverGetExtensions_radeonsi();
#endif

#if defined(HAVE_VMWGFX)
   if (!strcmp(driver_name, "vmwgfx"))
      return __driDriverGetExtensions_vmwgfx();
#endif

#if defined(HAVE_FREEDRENO)
   if (!strcmp(driver_name, "msm"))
      return __driDriverGetExtensions_msm();
   if (!strcmp(driver_name, "kgsl"))
      return __driDriverGetExtensions_kgsl();
#endif

#if defined(HAVE_VIRGL) || (defined(HAVE_FREEDRENO) && !defined(PIPE_LOADER_DYNAMIC))
   if (!strcmp(driver_name, "virtio_gpu"))
      return __driDriverGetExtensions_virtio_gpu();
#endif

#if defined(HAVE_V3D)
   if (!strcmp(driver_name, "v3d"))
      return __driDriverGetExtensions_v3d();
#endif

#if defined(HAVE_VC4)
   if (!strcmp(driver_name, "vc4"))
      return __driDriverGetExtensions_vc4();
#endif

#if defined(HAVE_PANFROST)
   if (!strcmp(driver_name, "panfrost"))
      return __driDriverGetExtensions_panfrost();
   if (!strcmp(driver_name, "panthor"))
      return __driDriverGetExtensions_panthor();
#endif

#if defined(HAVE_ASAHI)
   if (!strcmp(driver_name, "asahi"))
      return __driDriverGetExtensions_asahi();
#endif

#if defined(HAVE_ETNAVIV)
   if (!strcmp(driver_name, "etnaviv"))
      return __driDriverGetExtensions_etnaviv();
#endif

#if defined(HAVE_TEGRA)
   if (!strcmp(driver_name, "tegra"))
      return __driDriverGetExtensions_tegra();
#endif

#if defined(HAVE_KMSRO)
   if (!strcmp(driver_name, "armada_drm"))
      return __driDriverGetExtensions_armada_drm();
   if (!strcmp(driver_name, "exynos"))
      return __driDriverGetExtensions_exynos();
   if (!strcmp(driver_name, "gm12u320"))
      return __driDriverGetExtensions_gm12u320();
   if (!strcmp(driver_name, "hdlcd"))
      return __driDriverGetExtensions_hdlcd();
   if (!strcmp(driver_name, "hx8357d"))
      return __driDriverGetExtensions_hx8357d();
   if (!strcmp(driver_name, "ili9163"))
      return __driDriverGetExtensions_ili9163();
   if (!strcmp(driver_name, "ili9225"))
      return __driDriverGetExtensions_ili9225();
   if (!strcmp(driver_name, "ili9341"))
      return __driDriverGetExtensions_ili9341();
   if (!strcmp(driver_name, "ili9486"))
      return __driDriverGetExtensions_ili9486();
   if (!strcmp(driver_name, "imx_drm"))
      return __driDriverGetExtensions_imx_drm();
   if (!strcmp(driver_name, "imx_dcss"))
      return __driDriverGetExtensions_imx_dcss();
   if (!strcmp(driver_name, "imx_lcdif"))
      return __driDriverGetExtensions_imx_lcdif();
   if (!strcmp(driver_name, "ingenic_drm"))
      return __driDriverGetExtensions_ingenic_drm();
   if (!strcmp(driver_name, "kirin"))
      return __driDriverGetExtensions_kirin();
   if (!strcmp(driver_name, "komeda"))
      return __driDriverGetExtensions_komeda();
   if (!strcmp(driver_name, "mali_dp"))
      return __driDriverGetExtensions_mali_dp();
   if (!strcmp(driver_name, "mcde"))
      return __driDriverGetExtensions_mcde();
   if (!strcmp(driver_name, "mediatek"))
      return __driDriverGetExtensions_mediatek();
   if (!strcmp(driver_name, "meson"))
      return __driDriverGetExtensions_meson();
   if (!strcmp(driver_name, "mi0283qt"))
      return __driDriverGetExtensions_mi0283qt();
   if (!strcmp(driver_name, "mxsfb_drm"))
      return __driDriverGetExtensions_mxsfb_drm();
   if (!strcmp(driver_name, "panel_mipi_dbi"))
      return __driDriverGetExtensions_panel_mipi_dbi();
   if (!strcmp(driver_name, "pl111"))
      return __driDriverGetExtensions_pl111();
   if (!strcmp(driver_name, "rcar_du"))
      return __driDriverGetExtensions_rcar_du();
   if (!strcmp(driver_name, "repaper"))
      return __driDriverGetExtensions_repaper();
   if (!strcmp(driver_name, "rockchip"))
      return __driDriverGetExtensions_rockchip();
   if (!strcmp(driver_name, "rzg2l_du"))
      return __driDriverGetExtensions_rzg2l_du();
   if (!strcmp(driver_name, "ssd130x"))
      return __driDriverGetExtensions_ssd130x();
   if (!strcmp(driver_name, "st7586"))
      return __driDriverGetExtensions_st7586();
   if (!strcmp(driver_name, "st7735r"))
      return __driDriverGetExtensions_st7735r();
   if (!strcmp(driver_name, "sti"))
      return __driDriverGetExtensions_sti();
   if (!strcmp(driver_name, "stm"))
      return __driDriverGetExtensions_stm();
   if (!strcmp(driver_name, "sun4i_drm"))
      return __driDriverGetExtensions_sun4i_drm();
   if (!strcmp(driver_name, "udl"))
      return __driDriverGetExtensions_udl();
   if (!strcmp(driver_name, "vkms"))
      return __driDriverGetExtensions_vkms();
   if (!strcmp(driver_name, "zynqmp_dpsub"))
      return __driDriverGetExtensions_zynqmp_dpsub();
#endif

#if defined(HAVE_LIMA)
   if (!strcmp(driver_name, "lima"))
      return __driDriverGetExtensions_lima();
#endif

#if defined(HAVE_ZINK)
   if (!strcmp(driver_name, "zink"))
      return __driDriverGetExtensions_zink();
#endif

#if defined(HAVE_D3D12)
   if (!strcmp(driver_name, "d3d12"))
      return __driDriverGetExtensions_d3d12();
#endif

#if defined(HAVE_SOFTPIPE) || defined(HAVE_LLVMPIPE)
   if (!strcmp(driver_name, "swrast"))
      return __driDriverGetExtensions_swrast();

#if defined(HAVE_LIBDRM)
   if (!strcmp(driver_name, "kms_swrast"))
      return __driDriverGetExtensions_kms_swrast();
#endif
#endif

   if (warn_on_fail) {
      log_(_LOADER_WARNING,
           "MESA-LOADER: failed to open %s: driver not built!)\n", driver_name);
   }
   return NULL;
}

char *
loader_get_extensions_name(const char *driver_name)
{
   char *name = NULL;

   if (asprintf(&name, "%s_%s", __DRI_DRIVER_GET_EXTENSIONS, driver_name) < 0)
      return NULL;

   const size_t len = strlen(name);
   for (size_t i = 0; i < len; i++) {
      if (name[i] == '-')
         name[i] = '_';
   }

   return name;
}

bool
loader_bind_extensions(void *data,
                       const struct dri_extension_match *matches, size_t num_matches,
                       const __DRIextension **extensions)
{
   bool ret = true;

   for (size_t j = 0; j < num_matches; j++) {
      const struct dri_extension_match *match = &matches[j];
      const __DRIextension **field = (const __DRIextension **)((char *)data + matches[j].offset);
      for (size_t i = 0; extensions[i]; i++) {
         if (strcmp(extensions[i]->name, match->name) == 0 &&
             extensions[i]->version >= match->version) {
            *field = extensions[i];
            break;
         }
      }

      if (!*field) {
         log_(match->optional ? _LOADER_DEBUG : _LOADER_FATAL, "did not find extension %s version %d\n",
               match->name, match->version);
         if (!match->optional)
            ret = false;
         continue;
      }

      /* The loaders rely on the loaded DRI drivers being from the same Mesa
       * build so that we can reference the same structs on both sides.
       */
      if (strcmp(match->name, __DRI_MESA) == 0) {
         const __DRImesaCoreExtension *mesa = (const __DRImesaCoreExtension *)*field;
         if (strcmp(mesa->version_string, MESA_INTERFACE_VERSION_STRING) != 0) {
            log_(_LOADER_FATAL, "DRI driver not from this Mesa build ('%s' vs '%s')\n",
                 mesa->version_string, MESA_INTERFACE_VERSION_STRING);
            ret = false;
         }
      }
   }

   return ret;
}
/**
 * Opens a driver or backend using its name, returning the library handle.
 *
 * \param driverName - a name like "i965", "radeon", "nouveau", etc.
 * \param lib_suffix - a suffix to append to the driver name to generate the
 * full library name.
 * \param search_path_vars - NULL-terminated list of env vars that can be used
 * \param default_search_path - a colon-separted list of directories used if
 * search_path_vars is NULL or none of the vars are set in the environment.
 * \param warn_on_fail - Log a warning if the driver is not found.
 */
void *
loader_open_driver_lib(const char *driver_name,
                       const char *lib_suffix,
                       const char **search_path_vars,
                       const char *default_search_path,
                       bool warn_on_fail)
{
   char path[PATH_MAX];
   const char *search_paths, *next, *end;

   search_paths = NULL;
   if (__normal_user() && search_path_vars) {
      for (int i = 0; search_path_vars[i] != NULL; i++) {
         search_paths = os_get_option(search_path_vars[i]);
         if (search_paths)
            break;
      }
   }
   if (search_paths == NULL)
      search_paths = default_search_path;

   void *driver = NULL;
   const char *dl_error = NULL;
   end = search_paths + strlen(search_paths);
   for (const char *p = search_paths; p < end; p = next + 1) {
      int len;
      next = strchr(p, ':');
      if (next == NULL)
         next = end;

      len = next - p;
      snprintf(path, sizeof(path), "%.*s/tls/%s%s.so", len,
               p, driver_name, lib_suffix);
      driver = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
      if (driver == NULL) {
         snprintf(path, sizeof(path), "%.*s/%s%s.so", len,
                  p, driver_name, lib_suffix);
         driver = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
         if (driver == NULL) {
            dl_error = dlerror();
            log_(_LOADER_DEBUG, "MESA-LOADER: failed to open %s: %s\n",
                 path, dl_error);
         }
      }
      /* not need continue to loop all paths once the driver is found */
      if (driver != NULL)
         break;
   }

   if (driver == NULL) {
      if (warn_on_fail) {
         log_(_LOADER_WARNING,
              "MESA-LOADER: failed to open %s: %s (search paths %s, suffix %s)\n",
              driver_name, dl_error, search_paths, lib_suffix);
      }
      return NULL;
   }

   log_(_LOADER_DEBUG, "MESA-LOADER: dlopen(%s)\n", path);

   return driver;
}
