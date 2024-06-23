# Copyright © 2018 Intel Corporation

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

foreach t : ['tri', 'quad-tex']
  executable(
    t,
    '@0@.c'.format(t),
    include_directories : [
      inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium, inc_gallium_aux,
      inc_gallium_drivers, inc_gallium_winsys,
      include_directories('../../frontends/dri')
    ],
    link_with : [
      libgallium, libpipe_loader_static, libws_null, libwsw, libswdri, libswkmsdri
    ],
    dependencies : [
      idep_mesautil, driver_swrast, driver_r300, driver_r600, driver_radeonsi,
      driver_nouveau, driver_kmsro, driver_v3d, driver_vc4, driver_freedreno,
      driver_etnaviv, driver_tegra, driver_i915, driver_svga, driver_virgl,
      driver_panfrost, driver_iris, driver_lima, driver_zink, driver_d3d12,
      driver_asahi, driver_crocus
    ],
    install : false,
  )
endforeach
