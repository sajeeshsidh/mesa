# Copyright 2017 Intel Corporation
# SPDX-License-Identifier: MIT

if get_option('panfrost-clc') == 'system'
  prog_panfrost_clc = find_program('panfrost_clc', native : true)
elif with_panfrost_clc
  prog_panfrost_clc = executable(
    'panfrost_clc',
    ['panfrost_clc.c', sha1_h],
    link_with : [],
    include_directories : [inc_include, inc_src],
    c_args : [pre_args, no_override_init_args],
    link_args : [ld_args_build_id],
    dependencies : [idep_mesaclc, dep_llvm, dep_spirv_tools, idep_nir],
  )
endif
