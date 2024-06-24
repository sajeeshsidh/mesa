/*
 * Copyright © 2018 Valve Corporation
 * Copyright © 2018 Google
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_ir.h"

#include "util/u_math.h"

#include <set>
#include <vector>

namespace aco {
RegisterDemand
get_live_changes(aco_ptr<Instruction>& instr)
{
   RegisterDemand changes;
   for (const Definition& def : instr->definitions) {
      if (!def.isTemp() || def.isKill())
         continue;
      changes += def.getTemp();
   }

   for (const Operand& op : instr->operands) {
      if (!op.isTemp() || !op.isFirstKill())
         continue;
      changes -= op.getTemp();
   }

   return changes;
}

void
handle_def_fixed_to_op(RegisterDemand* demand, RegisterDemand demand_before, Instruction* instr,
                       int op_idx)
{
   /* Usually the register demand before an instruction would be considered part of the previous
    * instruction, since it's not greater than the register demand for that previous instruction.
    * Except, it can be greater in the case of an definition fixed to a non-killed operand: the RA
    * needs to reserve space between the two instructions for the definition (containing a copy of
    * the operand).
    */
   demand_before += instr->definitions[0].getTemp();
   demand->update(demand_before);
}

RegisterDemand
get_temp_registers(aco_ptr<Instruction>& instr)
{
   RegisterDemand temp_registers;

   for (Definition def : instr->definitions) {
      if (!def.isTemp())
         continue;
      if (def.isKill())
         temp_registers += def.getTemp();
   }

   for (Operand op : instr->operands) {
      if (op.isTemp() && op.isLateKill() && op.isFirstKill())
         temp_registers += op.getTemp();
   }

   int op_idx = get_op_fixed_to_def(instr.get());
   if (op_idx != -1 && !instr->operands[op_idx].isKill()) {
      RegisterDemand before_instr;
      before_instr -= get_live_changes(instr);
      handle_def_fixed_to_op(&temp_registers, before_instr, instr.get(), op_idx);
   }

   return temp_registers;
}

RegisterDemand
get_demand_before(RegisterDemand demand, aco_ptr<Instruction>& instr,
                  aco_ptr<Instruction>& instr_before)
{
   demand -= get_live_changes(instr);
   demand -= get_temp_registers(instr);
   if (instr_before)
      demand += get_temp_registers(instr_before);
   return demand;
}

namespace {
struct PhiInfo {
   uint16_t logical_phi_sgpr_ops = 0;
   uint16_t linear_phi_ops = 0;
   uint16_t linear_phi_defs = 0;
};

struct live_ctx {
   Program* program;
   std::vector<PhiInfo> phi_info;
   std::vector<bool> live_out_complete;
   std::vector<bool> reg_demand_complete;
};

bool
instr_needs_vcc(Instruction* instr)
{
   if (instr->isVOPC())
      return true;
   if (instr->isVOP2() && !instr->isVOP3()) {
      if (instr->operands.size() == 3 && instr->operands[2].isTemp() &&
          instr->operands[2].regClass().type() == RegType::sgpr)
         return true;
      if (instr->definitions.size() == 2)
         return true;
   }
   return false;
}

void
handle_phi_operands(live_ctx& ctx)
{
   for (Block& block : ctx.program->blocks) {
      for (aco_ptr<Instruction>& phi : block.instructions) {
         if (!is_phi(phi))
            break;

         /* Directly insert into the predecessors' live-out set. */
         Block::edge_vec& preds =
            phi->opcode == aco_opcode::p_phi ? block.logical_preds : block.linear_preds;

         for (unsigned i = 0; i < preds.size(); ++i) {
            Operand& operand = phi->operands[i];
            if (!operand.isTemp())
               continue;
            if (operand.isFixed() && operand.physReg() == vcc)
               ctx.program->needs_vcc = true;

            ctx.program->live.live_out[preds[i]].insert(operand.tempId());
         }
      }
   }
}

void
procsss_phi_reg_changes(live_ctx& ctx, Block* block, IDSet& live)
{
   uint16_t linear_phi_defs = 0;

   for (unsigned j = 0; j < block->instructions.size(); j++) {
      Instruction* insn = block->instructions[j].get();
      if (!is_phi(insn))
         break;

      Block::edge_vec& preds =
         insn->opcode == aco_opcode::p_phi ? block->logical_preds : block->linear_preds;
      for (unsigned i = 0; i < preds.size(); ++i) {
         Operand& operand = insn->operands[i];
         if (!operand.isTemp())
            continue;

         const bool kill = !live.count(operand.tempId());
         operand.setKill(kill);
         if (kill) {
            if (insn->opcode == aco_opcode::p_phi && operand.getTemp().type() == RegType::sgpr) {
               ctx.phi_info[preds[i]].logical_phi_sgpr_ops += operand.size();
            } else if (insn->opcode == aco_opcode::p_linear_phi) {
               assert(operand.getTemp().type() == RegType::sgpr);
               ctx.phi_info[preds[i]].linear_phi_ops += operand.size();
            }
         }
      }

      if (insn->opcode == aco_opcode::p_linear_phi && insn->definitions[0].isTemp()) {
         assert(insn->definitions[0].getTemp().type() == RegType::sgpr);
         linear_phi_defs += insn->definitions[0].size();
      }
   }

   for (unsigned pred_idx : block->linear_preds)
      ctx.phi_info[pred_idx].linear_phi_defs = linear_phi_defs;
}

/* For reducible CFGs it holds that
 * - If a variable is live-in at the header of a loop then
 *   it is live at all nodes inside the loop.
 *
 * We use this property to directly insert the live-out variables into all
 * blocks of the loop.
 */
void
insert_loop_lives(live_ctx& ctx, Block* loop_header, IDSet& live)
{
   unsigned loop_header_idx = loop_header->index;

   /* Insert into preheader */
   ctx.program->live.live_out[loop_header_idx - 1].insert(live);
   ctx.live_out_complete[loop_header_idx - 1] = true;

   std::vector<uint32_t> logical_blocks = {loop_header_idx};
   std::vector<uint32_t> linear_blocks;

   while (!logical_blocks.empty()) {
      unsigned block_idx = logical_blocks.back();
      logical_blocks.pop_back();
      if (ctx.live_out_complete[block_idx])
         continue;

      ctx.program->live.live_out[block_idx].insert(live);
      ctx.live_out_complete[block_idx] = true;

      const Block& block = ctx.program->blocks[block_idx];
      for (const unsigned pred : block.logical_preds) {
         if (!ctx.live_out_complete[pred])
            logical_blocks.emplace_back(pred);
      }
      for (const unsigned pred : block.linear_preds)
         if (!ctx.live_out_complete[pred])
            linear_blocks.emplace_back(pred);
   }

   while (!linear_blocks.empty() && ctx.live_out_complete[linear_blocks.back()])
      linear_blocks.pop_back();

   if (!linear_blocks.empty()) {
      IDSet linear_set = IDSet(ctx.program->live.memory);
      for (unsigned id : live) {
         if (ctx.program->temp_rc[id].is_linear())
            linear_set.insert(id);
      }

      while (!linear_blocks.empty()) {
         unsigned block_idx = linear_blocks.back();
         linear_blocks.pop_back();
         if (ctx.live_out_complete[block_idx])
            continue;

         ctx.program->live.live_out[block_idx].insert(linear_set);
         ctx.live_out_complete[block_idx] = true;

         const Block& block = ctx.program->blocks[block_idx];
         for (unsigned pred : block.linear_preds)
            if (!ctx.live_out_complete[pred])
               linear_blocks.emplace_back(pred);
      }
   }

   unsigned loop_exit_idx = loop_header->linear_preds.back() + 1;
   assert(ctx.program->blocks[loop_exit_idx].kind & block_kind_loop_exit);
   const bool live_out_complete = ctx.reg_demand_complete[loop_exit_idx];
   std::fill(std::next(ctx.live_out_complete.begin(), loop_header_idx),
             std::next(ctx.live_out_complete.begin(), loop_exit_idx), live_out_complete);
   ctx.live_out_complete[loop_header_idx - 1] = false;
}

void
process_live_temps_per_block(live_ctx& ctx, Block* block)
{
   std::vector<RegisterDemand>& register_demand = ctx.program->live.register_demand[block->index];
   RegisterDemand new_demand;

   register_demand.resize(block->instructions.size());
   IDSet live = ctx.program->live.live_out[block->index];

   /* initialize register demand */
   const bool live_out_complete = ctx.live_out_complete[block->index];
   const bool update_reg_demand =
      live_out_complete || std::all_of(block->linear_succs.begin(), block->linear_succs.end(),
                                       [&](unsigned succ) { return ctx.live_out_complete[succ]; });
   if (update_reg_demand) {
      for (unsigned t : live)
         new_demand += Temp(t, ctx.program->temp_rc[t]);

      ctx.live_out_complete[block->index] = true;
      ctx.reg_demand_complete[block->index] = true;
   }

   /* traverse the instructions backwards */
   int idx;
   for (idx = block->instructions.size() - 1; idx >= 0; idx--) {
      Instruction* insn = block->instructions[idx].get();

      ctx.program->needs_vcc |= instr_needs_vcc(insn);
      register_demand[idx] = RegisterDemand(new_demand.vgpr, new_demand.sgpr);

      /* KILL */
      for (Definition& definition : insn->definitions) {
         if (!definition.isTemp()) {
            continue;
         }
         if (definition.isFixed() && definition.physReg() == vcc)
            ctx.program->needs_vcc = true;

         const Temp temp = definition.getTemp();
         const size_t n = live.erase(temp.id());

         if (!is_phi(insn)) {
            if (n)
               new_demand -= temp;
            else
               register_demand[idx] += temp;
         }
         definition.setKill(!n);
      }

      /* GEN */
      if (!is_phi(insn)) {
         /* we need to do this in a separate loop because the next one can
          * setKill() for several operands at once and we don't want to
          * overwrite that in a later iteration */
         for (Operand& op : insn->operands)
            op.setKill(false);

         for (unsigned i = 0; i < insn->operands.size(); ++i) {
            Operand& operand = insn->operands[i];
            if (!operand.isTemp())
               continue;
            if (operand.isFixed() && operand.physReg() == vcc)
               ctx.program->needs_vcc = true;
            const Temp temp = operand.getTemp();
            const bool inserted = live.insert(temp.id()).second;
            if (inserted) {
               operand.setFirstKill(true);
               for (unsigned j = i + 1; j < insn->operands.size(); ++j) {
                  if (insn->operands[j].isTemp() &&
                      insn->operands[j].tempId() == operand.tempId()) {
                     insn->operands[j].setFirstKill(false);
                     insn->operands[j].setKill(true);
                  }
               }
               if (operand.isLateKill())
                  register_demand[idx] += temp;
               new_demand += temp;
            }
         }
      }

      int op_idx = get_op_fixed_to_def(insn);
      if (op_idx != -1 && !insn->operands[op_idx].isKill()) {
         RegisterDemand before_instr = new_demand;
         handle_def_fixed_to_op(&register_demand[idx], before_instr, insn, op_idx);
      }
   }

   if (update_reg_demand) {
      /* Handle phis: fixup final register demand calculations */
      procsss_phi_reg_changes(ctx, block, live);
   }

   /* now, we need to merge the live-ins into the live-out sets */
   bool fast_merge =
      block->logical_preds.size() == 0 || block->logical_preds == block->linear_preds;

#ifndef NDEBUG
   if ((block->linear_preds.empty() && !live.empty()) ||
       (block->logical_preds.empty() && new_demand.vgpr > 0))
      fast_merge = false; /* we might have errors */
#endif

   if (live_out_complete) {
      /* Live-out have been inserted in a previous iteration. */
   } else if (block->kind & block_kind_loop_header && block->linear_preds.size() > 1) {
      insert_loop_lives(ctx, block, live);
   } else if (fast_merge) {
      for (unsigned pred_idx : block->linear_preds)
         ctx.program->live.live_out[pred_idx].insert(live);
   } else {
      for (unsigned t : live) {
         RegClass rc = ctx.program->temp_rc[t];
         Block::edge_vec& preds = rc.is_linear() ? block->linear_preds : block->logical_preds;

#ifndef NDEBUG
         if (preds.empty())
            aco_err(ctx.program, "Temporary never defined or are defined after use: %%%d in BB%d",
                    t, block->index);
#endif

         for (unsigned pred_idx : preds)
            ctx.program->live.live_out[pred_idx].insert(t);
      }
   }

   assert(!block->linear_preds.empty() || (new_demand == RegisterDemand() && live.empty()));
}

unsigned
calc_waves_per_workgroup(Program* program)
{
   /* When workgroup size is not known, just go with wave_size */
   unsigned workgroup_size =
      program->workgroup_size == UINT_MAX ? program->wave_size : program->workgroup_size;

   return align(workgroup_size, program->wave_size) / program->wave_size;
}
} /* end namespace */

bool
uses_scratch(Program* program)
{
   /* RT uses scratch but we don't yet know how much. */
   return program->config->scratch_bytes_per_wave || program->stage == raytracing_cs;
}

uint16_t
get_extra_sgprs(Program* program)
{
   /* We don't use this register on GFX6-8 and it's removed on GFX10+. */
   bool needs_flat_scr = uses_scratch(program) && program->gfx_level == GFX9;

   if (program->gfx_level >= GFX10) {
      assert(!program->dev.xnack_enabled);
      return 0;
   } else if (program->gfx_level >= GFX8) {
      if (needs_flat_scr)
         return 6;
      else if (program->dev.xnack_enabled)
         return 4;
      else if (program->needs_vcc)
         return 2;
      else
         return 0;
   } else {
      assert(!program->dev.xnack_enabled);
      if (needs_flat_scr)
         return 4;
      else if (program->needs_vcc)
         return 2;
      else
         return 0;
   }
}

uint16_t
get_sgpr_alloc(Program* program, uint16_t addressable_sgprs)
{
   uint16_t sgprs = addressable_sgprs + get_extra_sgprs(program);
   uint16_t granule = program->dev.sgpr_alloc_granule;
   return ALIGN_NPOT(std::max(sgprs, granule), granule);
}

uint16_t
get_vgpr_alloc(Program* program, uint16_t addressable_vgprs)
{
   assert(addressable_vgprs <= program->dev.vgpr_limit);
   uint16_t granule = program->dev.vgpr_alloc_granule;
   return ALIGN_NPOT(std::max(addressable_vgprs, granule), granule);
}

unsigned
round_down(unsigned a, unsigned b)
{
   return a - (a % b);
}

uint16_t
get_addr_sgpr_from_waves(Program* program, uint16_t waves)
{
   /* it's not possible to allocate more than 128 SGPRs */
   uint16_t sgprs = std::min(program->dev.physical_sgprs / waves, 128);
   sgprs = round_down(sgprs, program->dev.sgpr_alloc_granule);
   sgprs -= get_extra_sgprs(program);
   return std::min(sgprs, program->dev.sgpr_limit);
}

uint16_t
get_addr_vgpr_from_waves(Program* program, uint16_t waves)
{
   uint16_t vgprs = program->dev.physical_vgprs / waves;
   vgprs = vgprs / program->dev.vgpr_alloc_granule * program->dev.vgpr_alloc_granule;
   vgprs -= program->config->num_shared_vgprs / 2;
   return std::min(vgprs, program->dev.vgpr_limit);
}

void
calc_min_waves(Program* program)
{
   unsigned waves_per_workgroup = calc_waves_per_workgroup(program);
   unsigned simd_per_cu_wgp = program->dev.simd_per_cu * (program->wgp_mode ? 2 : 1);
   program->min_waves = DIV_ROUND_UP(waves_per_workgroup, simd_per_cu_wgp);
}

uint16_t
max_suitable_waves(Program* program, uint16_t waves)
{
   unsigned num_simd = program->dev.simd_per_cu * (program->wgp_mode ? 2 : 1);
   unsigned waves_per_workgroup = calc_waves_per_workgroup(program);
   unsigned num_workgroups = waves * num_simd / waves_per_workgroup;

   /* Adjust #workgroups for LDS */
   unsigned lds_per_workgroup = align(program->config->lds_size * program->dev.lds_encoding_granule,
                                      program->dev.lds_alloc_granule);

   if (program->stage == fragment_fs) {
      /* PS inputs are moved from PC (parameter cache) to LDS before PS waves are launched.
       * Each PS input occupies 3x vec4 of LDS space. See Figure 10.3 in GCN3 ISA manual.
       * These limit occupancy the same way as other stages' LDS usage does.
       */
      unsigned lds_bytes_per_interp = 3 * 16;
      unsigned lds_param_bytes = lds_bytes_per_interp * program->info.ps.num_interp;
      lds_per_workgroup += align(lds_param_bytes, program->dev.lds_alloc_granule);
   }
   unsigned lds_limit = program->wgp_mode ? program->dev.lds_limit * 2 : program->dev.lds_limit;
   if (lds_per_workgroup)
      num_workgroups = std::min(num_workgroups, lds_limit / lds_per_workgroup);

   /* Hardware limitation */
   if (waves_per_workgroup > 1)
      num_workgroups = std::min(num_workgroups, program->wgp_mode ? 32u : 16u);

   /* Adjust #waves for workgroup multiples:
    * In cases like waves_per_workgroup=3 or lds=65536 and
    * waves_per_workgroup=1, we want the maximum possible number of waves per
    * SIMD and not the minimum. so DIV_ROUND_UP is used
    */
   unsigned workgroup_waves = num_workgroups * waves_per_workgroup;
   return DIV_ROUND_UP(workgroup_waves, num_simd);
}

void
update_vgpr_sgpr_demand(Program* program, const RegisterDemand new_demand)
{
   assert(program->min_waves >= 1);
   uint16_t sgpr_limit = get_addr_sgpr_from_waves(program, program->min_waves);
   uint16_t vgpr_limit = get_addr_vgpr_from_waves(program, program->min_waves);

   /* this won't compile, register pressure reduction necessary */
   if (new_demand.vgpr > vgpr_limit || new_demand.sgpr > sgpr_limit) {
      program->num_waves = 0;
      program->max_reg_demand = new_demand;
   } else {
      program->num_waves = program->dev.physical_sgprs / get_sgpr_alloc(program, new_demand.sgpr);
      uint16_t vgpr_demand =
         get_vgpr_alloc(program, new_demand.vgpr) + program->config->num_shared_vgprs / 2;
      program->num_waves =
         std::min<uint16_t>(program->num_waves, program->dev.physical_vgprs / vgpr_demand);
      program->num_waves = std::min(program->num_waves, program->dev.max_waves_per_simd);

      /* Adjust for LDS and workgroup multiples and calculate max_reg_demand */
      program->num_waves = max_suitable_waves(program, program->num_waves);
      program->max_reg_demand.vgpr = get_addr_vgpr_from_waves(program, program->num_waves);
      program->max_reg_demand.sgpr = get_addr_sgpr_from_waves(program, program->num_waves);
   }
}

void
live_var_analysis(Program* program)
{
   /* This algorithm implements 'Liveness Sets On Reducible Graphs' from
    * "Computing Liveness Sets for SSA-Form Programs" by F. Brandner et al.
    *
    * Note, that this implementation assumes that the block idx corresponds to the
    * block's position in program->blocks vector.
    */
   program->live.live_out.clear();
   program->live.memory.release();
   program->live.live_out.resize(program->blocks.size(), IDSet(program->live.memory));
   program->live.register_demand.resize(program->blocks.size());

   live_ctx ctx{
      program,
      std::vector<PhiInfo>(program->blocks.size()),
      std::vector<bool>(program->blocks.size()),
      std::vector<bool>(program->blocks.size()),
   };

   program->needs_vcc = program->gfx_level >= GFX10;

   /* First, insert all phi operands into live-out sets of the predecessors. */
   handle_phi_operands(ctx);

   /* Second, calculate complete live-out sets of all blocks by
    * - computing partial liveness sets using postorder traversal.
    * - propagating live variables withing loop bodies.
    */
   for (int i = program->blocks.size() - 1; i >= 0; i--)
      process_live_temps_per_block(ctx, &program->blocks[i]);

   /* Third, calculate register demand within loop bodies. */
   for (int i = program->blocks.size() - 1; i >= 0; i--) {
      assert(ctx.live_out_complete[i]);
      if (!ctx.reg_demand_complete[i])
         process_live_temps_per_block(ctx, &program->blocks[i]);
   }

   /* Final register demand calculation. */
   RegisterDemand new_demand;
   for (Block& block : program->blocks) {
      /* Handle branches: Fixup the register demand changes caused by phis.
       *
       * We will insert copies created for linear phis just before the branch.
       * SGPR->VGPR copies for logical phis happen just before p_logical_end.
       */
      std::vector<RegisterDemand>& reg_demand = program->live.register_demand[block.index];
      reg_demand.back().sgpr += ctx.phi_info[block.index].linear_phi_defs;
      reg_demand.back().sgpr -= ctx.phi_info[block.index].linear_phi_ops;
      if (ctx.phi_info[block.index].logical_phi_sgpr_ops) {
         for (int i = block.instructions.size() - 1; i >= 0; i--) {
            reg_demand[i].sgpr -= ctx.phi_info[block.index].logical_phi_sgpr_ops;
            if (block.instructions[i]->opcode == aco_opcode::p_logical_end)
               break;
         }
      }

      /* Update block's register demand */
      if (program->progress < CompilationProgress::after_ra) {
         block.register_demand = RegisterDemand();
         for (RegisterDemand& demand : program->live.register_demand[block.index])
            block.register_demand.update(demand);
      }

      new_demand.update(block.register_demand);
   }

   /* calculate the program's register demand and number of waves */
   if (program->progress < CompilationProgress::after_ra)
      update_vgpr_sgpr_demand(program, new_demand);
}

} // namespace aco
