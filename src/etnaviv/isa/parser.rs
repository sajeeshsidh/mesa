// Copyright © 2024 Igalia S.L.
// SPDX-License-Identifier: MIT

extern crate etnaviv_isa_proc;
extern crate isa_bindings;
extern crate pest;

use crate::util::EtnaAsmResultExt;

use etnaviv_isa_proc::IsaParser;
use isa_bindings::*;
use pest::iterators::Pair;
use pest::Parser;
use std::fs;

#[derive(IsaParser)]
#[isa = "etnaviv.xml"]
#[static_rules_file = "static_rules.pest"]
struct Isa;

fn fill_swizzle(item: Pair<Rule>) -> u32 {
    let mut byte: u32 = 0b0000_0000;

    assert!(item.as_rule() == Rule::SrcSwizzle);

    for (index, comp) in item.into_inner().enumerate() {
        match comp.as_rule() {
            Rule::Swiz => match comp.as_str() {
                "x" => {
                    // Nothing to do
                }
                "y" => byte |= 1 << (2 * index),
                "z" => byte |= 2 << (2 * index),
                "w" => byte |= 3 << (2 * index),
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    byte
}

fn fill_destination(pair: Pair<Rule>, dest: &mut etna_inst_dst) {
    dest.set_use(1);

    for item in pair.into_inner() {
        match item.as_rule() {
            Rule::RegAddressingMode => {
                let rule = item.into_inner().next().unwrap().as_rule();
                dest.set_amode(isa_reg_addressing_mode::from_rule(rule));
            }
            Rule::Register => {
                dest.set_reg(item.as_str().parse::<u32>().unwrap());
            }
            Rule::Wrmask => {
                let rule = item.into_inner().next().unwrap().as_rule();
                dest.set_write_mask(isa_wrmask::from_rule(rule));
            }
            _ => todo!(),
        }
    }
}

fn fill_mem_destination(pair: Pair<Rule>, dest: &mut etna_inst_dst) {
    dest.set_use(1);

    for item in pair.into_inner() {
        match item.as_rule() {
            Rule::Wrmask => {
                let rule = item.into_inner().next().unwrap().as_rule();
                dest.set_write_mask(isa_wrmask::from_rule(rule));
            }
            _ => todo!(),
        }
    }
}

fn fill_tex(pair: Pair<Rule>, tex: &mut etna_inst_tex) {
    for item in pair.into_inner() {
        match item.as_rule() {
            Rule::Register => {
                let r = item.as_str().parse::<u32>().unwrap();
                tex.set_id(r)
            }
            Rule::SrcSwizzle => {
                let bytes = fill_swizzle(item);
                tex.set_swiz(bytes);
            }
            _ => todo!(),
        }
    }
}

fn fill_source(pair: Pair<Rule>, src: &mut etna_inst_src, dual_16_mode: bool) {
    src.set_use(1);

    for item in pair.into_inner() {
        match item.as_rule() {
            Rule::Absolute => unsafe {
                src.__bindgen_anon_1.__bindgen_anon_1.set_abs(1);
            },
            Rule::Negate => unsafe {
                src.__bindgen_anon_1.__bindgen_anon_1.set_neg(1);
            },
            Rule::RegGroup => {
                let rule = item.into_inner().next().unwrap().as_rule();
                src.set_rgroup(isa_reg_group::from_rule(rule));
            }
            Rule::RegAddressingMode => {
                let rule = item.into_inner().next().unwrap().as_rule();
                unsafe {
                    src.__bindgen_anon_1
                        .__bindgen_anon_1
                        .set_amode(isa_reg_addressing_mode::from_rule(rule));
                }
            }
            Rule::Register => {
                let r = item.as_str().parse::<u32>().unwrap();
                unsafe {
                    src.__bindgen_anon_1.__bindgen_anon_1.set_reg(r);
                }
            }
            Rule::Immediate_Minus_Nan => {
                src.set_rgroup(isa_reg_group::ISA_REG_GROUP_IMMED);

                unsafe {
                    src.__bindgen_anon_1.__bindgen_anon_2.set_imm_type(0);
                }
                unsafe {
                    src.__bindgen_anon_1.__bindgen_anon_2.set_imm_val(0xfffff);
                }
            }
            Rule::Immediate_float => {
                let value = item.as_str().parse::<f32>().unwrap();
                let bits = value.to_bits();

                assert!((bits & 0xfff) == 0); /* 12 lsb cut off */
                src.set_rgroup(isa_reg_group::ISA_REG_GROUP_IMMED);

                if dual_16_mode {
                    unsafe {
                        src.__bindgen_anon_1.__bindgen_anon_2.set_imm_type(3);
                    }
                    unsafe {
                        src.__bindgen_anon_1
                            .__bindgen_anon_2
                            .set_imm_val(bits >> 12);
                    }
                } else {
                    unsafe {
                        src.__bindgen_anon_1.__bindgen_anon_2.set_imm_type(0);
                    }
                    unsafe {
                        src.__bindgen_anon_1
                            .__bindgen_anon_2
                            .set_imm_val(bits >> 12);
                    }
                }
            }
            Rule::Immediate_int => {
                let value = item.as_str().parse::<i32>().unwrap();

                src.set_rgroup(isa_reg_group::ISA_REG_GROUP_IMMED);
                unsafe {
                    src.__bindgen_anon_1.__bindgen_anon_2.set_imm_type(1);
                }
                unsafe {
                    src.__bindgen_anon_1
                        .__bindgen_anon_2
                        .set_imm_val(value as u32);
                }
            }
            Rule::Immediate_uint => {
                let value = item.as_str().parse::<u32>().unwrap();

                src.set_rgroup(isa_reg_group::ISA_REG_GROUP_IMMED);
                unsafe {
                    src.__bindgen_anon_1.__bindgen_anon_2.set_imm_type(1);
                }
                unsafe {
                    src.__bindgen_anon_1.__bindgen_anon_2.set_imm_val(value);
                }
            }
            Rule::SrcSwizzle => {
                let bytes = fill_swizzle(item);
                unsafe {
                    src.__bindgen_anon_1.__bindgen_anon_1.set_swiz(bytes);
                }
            }
            _ => todo!(),
        }
    }
}

fn process(input: Pair<Rule>, dual_16_mode: bool) -> Option<etna_inst> {
    // The assembler and disassembler are both using the
    // 'full' form of the ISA which contains void's and
    // use the HW ordering of instruction src arguments.

    if input.as_rule() == Rule::EOI {
        return None;
    }

    // Create instruction with sane defaults.
    let mut instr = etna_inst::default();
    instr.dst.set_write_mask(isa_wrmask::ISA_WRMASK_XYZW);

    instr.opcode = isa_opc::from_rule(input.as_rule());
    let mut src_index = 0;

    for p in input.into_inner() {
        match p.as_rule() {
            Rule::Dst_full => {
                instr.set_dst_full(1);
            }
            Rule::Sat => {
                instr.set_sat(1);
            }
            Rule::Cond => {
                let rule = p.into_inner().next().unwrap().as_rule();
                instr.set_cond(isa_cond::from_rule(rule));
            }
            Rule::Skphp => {
                instr.set_skphp(1);
            }
            Rule::Pmode => {
                instr.set_pmode(1);
            }
            Rule::Denorm => {
                instr.set_denorm(1);
            }
            Rule::Local => {
                instr.set_local(1);
            }
            Rule::Left_shift => {
                let item = p.into_inner().next().unwrap();
                let amount = item.as_str().parse::<u32>().unwrap();
                instr.set_left_shift(amount);
            }
            Rule::Type => {
                let rule = p.into_inner().next().unwrap().as_rule();
                instr.type_ = isa_type::from_rule(rule);
            }
            Rule::Thread => {
                let rule = p.into_inner().next().unwrap().as_rule();
                instr.set_thread(isa_thread::from_rule(rule));
            }
            Rule::Rounding => {
                let rule = p.into_inner().next().unwrap().as_rule();
                instr.rounding = isa_rounding::from_rule(rule);
            }
            Rule::DestVoid => {
                // Nothing to do
            }
            Rule::DstRegister => {
                fill_destination(p, &mut instr.dst);
            }
            Rule::DstMemAddr => {
                fill_mem_destination(p, &mut instr.dst);
            }
            Rule::SrcVoid => {
                // Nothing to do
            }
            Rule::SrcRegister => {
                fill_source(p, &mut instr.src[src_index], dual_16_mode);
                src_index += 1;
            }
            Rule::TexSrc => {
                fill_tex(p, &mut instr.tex);
            }
            Rule::Target => {
                let target = p.as_str().parse::<u32>().unwrap();
                instr.imm = target;
            }
            _ => panic!(),
        }
    }

    Some(instr)
}

pub fn asm_process_str(string: &str, dual_16_mode: bool, asm_result: &mut etna_asm_result) {
    let result = Isa::parse(Rule::instruction, string);

    match result {
        Ok(mut instruction) => {
            if let Some(i) = instruction.next() {
                if let Some(result) = process(i, dual_16_mode) {
                    asm_result.append_instruction(result);
                }
            }

            asm_result.success = true;
        }
        Err(e) => {
            asm_result.set_error(&format!("{}", e));
            asm_result.success = false;
        }
    }
}

pub fn asm_process_file(file: &str, dual_16_mode: bool, asm_result: &mut etna_asm_result) {
    let content = fs::read_to_string(file).expect("cannot read file");
    let result = Isa::parse(Rule::instructions, &content);

    match result {
        Ok(program) => {
            for line in program {
                if let Some(result) = process(line, dual_16_mode) {
                    asm_result.append_instruction(result);
                }
            }

            asm_result.success = true;
        }
        Err(e) => {
            asm_result.set_error(&format!("{}", e));
            asm_result.success = false;
        }
    }
}
