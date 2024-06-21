/*
 * Copyright © 2024 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#include "asm.h"
#include "isa.h"

struct encode_state {
   uint32_t gen;
};

static inline enum isa_opc
__instruction_case(struct encode_state *s, const struct etna_inst *instr)
{
   return instr->opcode;
}

#include "encode.h"

void isa_assemble_instruction(uint32_t *out, const struct etna_inst *instr, uint32_t halti)
{
   struct encode_state state = {
      .gen = halti
   };

   bitmask_t encoded = encode__instruction(&state, NULL, instr);

   store_instruction(out, encoded);
}
