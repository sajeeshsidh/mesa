/*
 * Copyright © 2024 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "nir.h"
#include "nir_builder.h"
#include "nir_phi_builder.h"

struct call_liveness_entry {
   struct list_head list;
   nir_call_instr *instr;
   const BITSET_WORD *live_set;
};

static bool
can_remat_instr(nir_instr *instr, struct nir_minimize_call_live_states_options *options)
{
   switch (instr->type) {
   case nir_instr_type_alu:
   case nir_instr_type_load_const:
   case nir_instr_type_undef:
      return true;
   case nir_instr_type_intrinsic:
      return options->can_remat && options->can_remat(instr);
   default:
      return false;
   }
}

static void
remat_ssa_def(nir_builder *b, nir_def *def, struct hash_table *remap_table,
              struct hash_table *phi_value_table, struct nir_phi_builder *phi_builder,
              BITSET_WORD *def_blocks)
{
   memset(def_blocks, 0, BITSET_WORDS(b->impl->num_blocks) * sizeof(BITSET_WORD));
   BITSET_SET(def_blocks, def->parent_instr->block->index);
   BITSET_SET(def_blocks, nir_cursor_current_block(b->cursor)->index);
   struct nir_phi_builder_value *val = nir_phi_builder_add_value(phi_builder, def->num_components, def->bit_size, def_blocks);
   _mesa_hash_table_insert(phi_value_table, def, val);

   nir_instr *clone = nir_instr_clone_deep(b->shader, def->parent_instr, remap_table);
   nir_builder_instr_insert(b, clone);
   nir_def *new_def = nir_instr_def(clone);

   _mesa_hash_table_insert(remap_table, def, new_def);
   if (nir_cursor_current_block(b->cursor)->index != def->parent_instr->block->index)
      nir_phi_builder_value_set_block_def(val, def->parent_instr->block, def);
   nir_phi_builder_value_set_block_def(val, nir_cursor_current_block(b->cursor), new_def);
}

struct remat_chain_check_data {
   struct nir_minimize_call_live_states_options *options;
   struct hash_table *remap_table;
   unsigned chain_length;
};

static bool
can_remat_chain(nir_src *src, void *data)
{
   struct remat_chain_check_data *check_data = data;

   if (_mesa_hash_table_search(check_data->remap_table, src->ssa))
      return true;

   if (check_data->chain_length >= 16)
      return false;

   if (!can_remat_instr(src->ssa->parent_instr, check_data->options))
      return false;

   struct remat_chain_check_data new_check_data = {
      .options = check_data->options,
      .remap_table = check_data->remap_table,
      .chain_length = check_data->chain_length + 1,
   };
   return nir_foreach_src(src->ssa->parent_instr, can_remat_chain, &new_check_data);
}

struct remat_chain_data {
   nir_builder *b;
   struct hash_table *remap_table;
   struct hash_table *phi_value_table;
   struct nir_phi_builder *phi_builder;
   BITSET_WORD *def_blocks;
};

static bool
do_remat_chain(nir_src *src, void *data)
{
   struct remat_chain_data *remat_data = data;

   if (_mesa_hash_table_search(remat_data->remap_table, src->ssa))
      return true;

   nir_foreach_src(src->ssa->parent_instr, do_remat_chain, remat_data);

   remat_ssa_def(remat_data->b, src->ssa, remat_data->remap_table, remat_data->phi_value_table, remat_data->phi_builder, remat_data->def_blocks);
   return true;
}

struct src_rewrite_ctx {
   struct hash_table *phi_value_table;
   nir_instr *instr;
};

static bool
rewrite_instr_src_from_phi_builder(nir_src *src, void *data)
{
   struct src_rewrite_ctx *ctx = data;

   if (nir_src_is_const(*src)) {
      nir_builder b = nir_builder_at(nir_before_instr(ctx->instr));
      nir_src_rewrite(src, nir_build_imm(&b, src->ssa->num_components, src->ssa->bit_size, nir_src_as_const_value(*src)));
      return true;
   }

   nir_block *block;
   if (nir_src_parent_instr(src)->type == nir_instr_type_phi) {
      nir_phi_src *phi_src = exec_node_data(nir_phi_src, src, src);
      block = phi_src->pred;
   } else {
      block = nir_src_parent_instr(src)->block;
   }

   struct hash_entry *entry = _mesa_hash_table_search(ctx->phi_value_table, src->ssa);
   if (!entry)
      return true;

   nir_def *new_def = nir_phi_builder_value_get_block_def(entry->data, block);

   bool can_rewrite = true;
   if (new_def->parent_instr->block == block && new_def->index != UINT32_MAX)
      can_rewrite = nir_src_parent_instr(src) != nir_block_first_instr(block) &&
                    !nir_instr_is_between(nir_block_first_instr(block),
                                          new_def->parent_instr,
                                          nir_src_parent_instr(src));

   if (can_rewrite)
      nir_src_rewrite(src, new_def);
   return true;
}

static bool
nir_minimize_call_live_states_impl(nir_function_impl *impl,
                                   struct nir_minimize_call_live_states_options *options)
{
   nir_metadata_require(impl, nir_metadata_block_index | nir_metadata_live_defs | nir_metadata_dominance);
   bool progress = false;
   void *mem_ctx = ralloc_context(impl->function->shader);

   struct list_head call_list;
   list_inithead(&call_list);
   unsigned num_defs = impl->ssa_alloc;

   nir_def **rematerializable = rzalloc_array_size(mem_ctx, sizeof(nir_def *), num_defs);

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         nir_def *def = nir_instr_def(instr);
         if (def &&
             can_remat_instr(instr, options)) {
            rematerializable[def->index] = def;
         }

         if (instr->type != nir_instr_type_call)
            continue;
         nir_call_instr *call = nir_instr_as_call(instr);
         if (!call->indirect_callee.ssa)
            continue;

         struct call_liveness_entry *entry = ralloc_size(mem_ctx, sizeof(struct call_liveness_entry));
         entry->instr = call;
         entry->live_set = nir_get_live_defs(nir_after_instr(instr), mem_ctx);
         list_addtail(&entry->list, &call_list);
      }
   }

   const unsigned block_words = BITSET_WORDS(impl->num_blocks);
   BITSET_WORD *def_blocks = ralloc_array(mem_ctx, BITSET_WORD, block_words);

   list_for_each_entry(struct call_liveness_entry, entry, &call_list, list) {
      unsigned i;

      nir_builder b = nir_builder_create(impl);
      b.cursor = nir_after_instr(&entry->instr->instr);

      struct nir_phi_builder *builder = nir_phi_builder_create(impl);
      struct hash_table *phi_value_table =
         _mesa_pointer_hash_table_create(mem_ctx);
      struct hash_table *remap_table =
         _mesa_pointer_hash_table_create(mem_ctx);

      BITSET_FOREACH_SET(i, entry->live_set, num_defs) {
         if (!rematerializable[i] || _mesa_hash_table_search(remap_table, rematerializable[i]))
            continue;

         progress = true;
         assert(!_mesa_hash_table_search(phi_value_table, rematerializable[i]));

         struct remat_chain_check_data check_data = {
            .options = options,
            .remap_table = remap_table,
            .chain_length = 1,
         };

         if (!nir_foreach_src(rematerializable[i]->parent_instr, can_remat_chain, &check_data))
            continue;

         struct remat_chain_data remat_data = {
            .b = &b,
            .remap_table = remap_table,
            .phi_value_table = phi_value_table,
            .phi_builder = builder,
            .def_blocks = def_blocks,
         };

         nir_foreach_src(rematerializable[i]->parent_instr, do_remat_chain, &remat_data);

         remat_ssa_def(&b, rematerializable[i], remap_table, phi_value_table, builder, def_blocks);
      }
      _mesa_hash_table_destroy(remap_table, NULL);

      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_phi)
               continue;

            struct src_rewrite_ctx ctx = {
               .phi_value_table = phi_value_table,
               .instr = instr,
            };
            nir_foreach_src(instr, rewrite_instr_src_from_phi_builder, &ctx);
         }
      }

      nir_phi_builder_finish(builder);
      _mesa_hash_table_destroy(phi_value_table, NULL);
   }

   ralloc_free(mem_ctx);

   nir_foreach_block(block, impl)
      nir_block_free_liveness_info(block);
   nir_metadata_preserve(impl, nir_metadata_block_index | nir_metadata_dominance);
   return progress;
}

/* Tries to rematerialize as many live vars as possible after calls.
 * Note: nir_opt_cse will undo any rematerializations done by this pass,
 * so it shouldn't be run afterward.
 */
bool
nir_minimize_call_live_states(nir_shader *shader,
                              struct nir_minimize_call_live_states_options *options)
{
   bool progress = false;

   nir_foreach_function_impl(impl, shader) {
      progress |= nir_minimize_call_live_states_impl(impl, options);
   }

   return progress;
}