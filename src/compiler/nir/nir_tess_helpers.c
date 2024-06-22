/*
 * Copyright 2021 Advanced Micro Devices, Inc.
 * Copyright 2024 Valve Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "nir.h"

static unsigned
get_inst_tessfactor_writemask(nir_intrinsic_instr *intrin)
{
   if (intrin->intrinsic != nir_intrinsic_store_output)
      return 0;

   unsigned writemask = nir_intrinsic_write_mask(intrin) << nir_intrinsic_component(intrin);
   unsigned location = nir_intrinsic_io_semantics(intrin).location;

   if (location == VARYING_SLOT_TESS_LEVEL_OUTER)
      return writemask << 4;
   else if (location == VARYING_SLOT_TESS_LEVEL_INNER)
      return writemask;

   return 0;
}

static void
walk_cf_node(nir_cf_node *cf_node, unsigned *upper_block_tf_writemask,
             unsigned *cond_block_tf_writemask,
             bool *tessfactors_are_def_in_all_invocs, bool is_nested_cf)
{
   switch (cf_node->type) {
   case nir_cf_node_block: {
      nir_block *block = nir_cf_node_as_block(cf_node);
      nir_foreach_instr (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic == nir_intrinsic_barrier &&
             nir_intrinsic_execution_scope(intrin) >= SCOPE_WORKGROUP) {

            /* If we find a barrier in nested control flow put this in the
             * too hard basket. In GLSL this is not possible but it is in
             * SPIR-V.
             */
            if (is_nested_cf) {
               *tessfactors_are_def_in_all_invocs = false;
               return;
            }

            /* The following case must be prevented:
             *    gl_TessLevelInner = ...;
             *    barrier();
             *    if (gl_InvocationID == 1)
             *       gl_TessLevelInner = ...;
             *
             * If you consider disjoint code segments separated by barriers, each
             * such segment that writes tess factor channels should write the same
             * channels in all codepaths within that segment.
             */
            if (*upper_block_tf_writemask || *cond_block_tf_writemask) {
               /* Accumulate the result: */
               *tessfactors_are_def_in_all_invocs &=
                  !(*cond_block_tf_writemask & ~(*upper_block_tf_writemask));

               /* Analyze the next code segment from scratch. */
               *upper_block_tf_writemask = 0;
               *cond_block_tf_writemask = 0;
            }
         } else
            *upper_block_tf_writemask |= get_inst_tessfactor_writemask(intrin);
      }

      break;
   }
   case nir_cf_node_if: {
      unsigned then_tessfactor_writemask = 0;
      unsigned else_tessfactor_writemask = 0;

      nir_if *if_stmt = nir_cf_node_as_if(cf_node);
      foreach_list_typed(nir_cf_node, nested_node, node, &if_stmt->then_list)
      {
         walk_cf_node(nested_node, &then_tessfactor_writemask, cond_block_tf_writemask,
                      tessfactors_are_def_in_all_invocs, true);
      }

      foreach_list_typed(nir_cf_node, nested_node, node, &if_stmt->else_list)
      {
         walk_cf_node(nested_node, &else_tessfactor_writemask, cond_block_tf_writemask,
                      tessfactors_are_def_in_all_invocs, true);
      }

      if (then_tessfactor_writemask || else_tessfactor_writemask) {
         /* If both statements write the same tess factor channels,
          * we can say that the upper block writes them too.
          */
         *upper_block_tf_writemask |= then_tessfactor_writemask & else_tessfactor_writemask;
         *cond_block_tf_writemask |= then_tessfactor_writemask | else_tessfactor_writemask;
      }

      break;
   }
   case nir_cf_node_loop: {
      nir_loop *loop = nir_cf_node_as_loop(cf_node);
      assert(!nir_loop_has_continue_construct(loop));
      foreach_list_typed(nir_cf_node, nested_node, node, &loop->body)
      {
         walk_cf_node(nested_node, cond_block_tf_writemask, cond_block_tf_writemask,
                      tessfactors_are_def_in_all_invocs, true);
      }

      break;
   }
   default:
      unreachable("unknown cf node type");
   }
}

bool
nir_tess_levels_defined_in_all_invocations(const struct nir_shader *nir)
{
   assert(nir->info.stage == MESA_SHADER_TESS_CTRL);

   /* The pass works as follows:
    * If all codepaths write tess factors, we can say that all
    * invocations define tess factors.
    *
    * Each tess factor channel is tracked separately.
    */
   unsigned main_block_tf_writemask = 0; /* if main block writes tess factors */
   unsigned cond_block_tf_writemask = 0; /* if cond block writes tess factors */

   /* Initial value = true. Here the pass will accumulate results from
    * multiple segments surrounded by barriers. If tess factors aren't
    * written at all, it's a shader bug and we don't care if this will be
    * true.
    */
   bool tessfactors_are_def_in_all_invocs = true;

   nir_foreach_function (function, nir) {
      if (function->impl) {
         foreach_list_typed(nir_cf_node, node, node, &function->impl->body)
         {
            walk_cf_node(node, &main_block_tf_writemask, &cond_block_tf_writemask,
                         &tessfactors_are_def_in_all_invocs, false);
         }
      }
   }

   /* Accumulate the result for the last code segment separated by a
    * barrier.
    */
   if (main_block_tf_writemask || cond_block_tf_writemask) {
      tessfactors_are_def_in_all_invocs &= !(cond_block_tf_writemask & ~main_block_tf_writemask);
   }

   return tessfactors_are_def_in_all_invocs;
}