// Copyright © 2024 Igalia S.L.
// SPDX-License-Identifier: MIT

extern crate isa_bindings;

use isa_bindings::*;
use std::alloc::{alloc, dealloc, realloc, Layout};
use std::ffi::{c_char, CString};
use std::ptr;

pub trait EtnaAsmResultExt {
    fn set_error(&mut self, error_message: &str);
    fn dealloc_error(&mut self);

    fn append_instruction(&mut self, new_inst: etna_inst);
    fn dealloc_instructions(&mut self);
}

impl EtnaAsmResultExt for etna_asm_result {
    fn set_error(&mut self, error_message: &str) {
        self.dealloc_error();

        self.error = CString::new(error_message)
            .expect("CString::new failed")
            .into_raw();
    }

    fn dealloc_error(&mut self) {
        if !self.error.is_null() {
            unsafe {
                let _ = CString::from_raw(self.error as *mut c_char);
            }
            self.error = ptr::null();
        }
    }

    fn append_instruction(&mut self, new_inst: etna_inst) {
        unsafe {
            let new_size = self.num_instr + 1;
            let layout = Layout::array::<etna_inst>(new_size as usize).unwrap();

            if self.instr.is_null() {
                self.instr = alloc(layout) as *mut etna_inst;
            } else {
                let old_size = self.num_instr;
                let old_layout = Layout::array::<etna_inst>(old_size as usize).unwrap();
                self.instr =
                    realloc(self.instr as *mut u8, old_layout, layout.size()) as *mut etna_inst;
            }

            if !self.instr.is_null() {
                ptr::write(self.instr.add(self.num_instr as usize), new_inst);
                self.num_instr = new_size;
            } else {
                // Handle allocation failure if needed
                self.success = false;
                self.set_error("Memory allocation failed");
            }
        }
    }

    fn dealloc_instructions(&mut self) {
        if !self.instr.is_null() {
            let layout = Layout::array::<etna_inst>(self.num_instr as usize).unwrap();
            unsafe {
                dealloc(self.instr as *mut u8, layout);
            }
        }
    }
}
