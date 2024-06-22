// Copyright © 2024 Igalia S.L.
// SPDX-License-Identifier: MIT

use crate::parser::*;
use crate::util::EtnaAsmResultExt;

use isa_bindings::*;
use std::ffi::CStr;
use std::os::raw::c_char;

#[no_mangle]
pub extern "C" fn isa_parse_str(c_str: *const c_char, dual_16_mode: bool) -> *mut etna_asm_result {
    let mut result = Box::new(etna_asm_result::default());
    assert!(!result.success);

    if c_str.is_null() {
        result.set_error("str pointer is NULL");
        return Box::into_raw(result);
    }

    let c_str_safe = unsafe { CStr::from_ptr(c_str) };

    if let Ok(str) = c_str_safe.to_str() {
        asm_process_str(str, dual_16_mode, &mut result);
    } else {
        result.set_error("Failed to convert CStr to &str");
        result.success = false;
    }

    Box::into_raw(result)
}

#[no_mangle]
pub extern "C" fn isa_parse_file(
    c_filepath: *const c_char,
    dual_16_mode: bool,
) -> *mut etna_asm_result {
    let mut result = Box::new(etna_asm_result::default());
    assert!(!result.success);

    if c_filepath.is_null() {
        result.set_error("filepath pointer is NULL");
        return Box::into_raw(result);
    }

    let c_filepath_safe = unsafe { CStr::from_ptr(c_filepath) };

    if let Ok(filepath) = c_filepath_safe.to_str() {
        asm_process_file(filepath, dual_16_mode, &mut result);
    } else {
        result.set_error("Failed to convert CStr to &str");
        result.success = false;
    }

    Box::into_raw(result)
}

#[no_mangle]
pub extern "C" fn isa_asm_result_destroy(result: *mut etna_asm_result) {
    unsafe {
        let mut r = Box::from_raw(result);
        r.dealloc_instructions();
        r.dealloc_error();
    };
}
