// Copyright © 2024 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use nvidia_headers::ArrayMthd;
use nvidia_headers::Mthd;

pub const MAX_MTHD_SIZE: u32 = 0x1fff;

fn class_to_subc(class: u16) -> u8 {
    match class {
        0x9097 | 0xa097 | 0xb097 | 0xb197 | 0xc097 | 0xc397 => 0,
        0x90c0 | 0xa0c0 | 0xb0c0 | 0xc0c0 | 0xc3c0 | 0xc6c0 => 1,
        0x9039 => 2,
        0x902d => 3,
        0x90b5 | 0xc1b5 => 4,
        _ => panic!("Invalid class: {class}"),
    }
}

enum IncType {
    /// Each dword increments the address by one
    NInc = 0,
    /// The first dword increments the address by one
    OneInc = 3,
    /// The address is not incremented
    ZeroInc = 5,
}

/// A method header.
///
/// Methods start with a header that can encode the `IncType`, the subclass,
/// an address and the size. Optionally, the header can contain an address
/// and an immediate instead.
#[repr(transparent)]
struct MthdHeader(u32);

impl MthdHeader {
    fn from_bits_mut(bits: &mut u32) -> &mut Self {
        // This is always safe beause a reference is always safe to
        // derefence.
        unsafe { &mut *(bits as *mut u32 as *mut MthdHeader) }
    }

    fn to_bits(self) -> u32 {
        self.0
    }

    fn new(inc_type: IncType, subc: u8, addr: u16, size: u16) -> Self {
        let subc: u32 = subc.into();
        let addr: u32 = addr.into();
        let size: u32 = size.into();

        let bits = match inc_type {
            IncType::NInc => {
                0x20000000 | (size << 16) | (subc << 13) | (addr >> 2)
            }
            IncType::ZeroInc => {
                0xa0000000 | (size << 16) | (subc << 13) | (addr >> 2)
            }
            IncType::OneInc => {
                0x60000000 | (size << 16) | (subc << 13) | (addr >> 2)
            }
        };

        Self(bits)
    }

    fn new_immd(immd: u16, subc: u8, addr: u16) -> u32 {
        let subc: u32 = subc.into();
        let addr: u32 = addr.into();
        let immd: u32 = immd.into();
        0x80000000 | (immd << 16) | (subc << 13) | (addr >> 2)
    }

    fn inc_type(&self) -> Option<IncType> {
        match self.0 >> 29 {
            1 => Some(IncType::NInc),
            3 => Some(IncType::OneInc),
            5 => Some(IncType::ZeroInc),
            4 => None, // Immd
            _ => panic!("Invalid method header"),
        }
    }

    fn set_inc_type(&mut self, inc_type: IncType) {
        let inc = inc_type as u32;
        self.0 &= !0xe0000000;
        self.0 |= inc << 29;
    }

    fn subc(&self) -> u8 {
        (self.0 >> 13 & 0x7) as u8
    }

    fn addr(&self) -> u16 {
        ((self.0 & 0x1fff) << 2) as u16
    }

    fn len(&self) -> u16 {
        (self.0 >> 16 & 0x1fff) as u16
    }

    fn add_len(&mut self, count: u16) {
        let new_len = self.len() + count;

        debug_assert!(u32::from(new_len) <= MAX_MTHD_SIZE);
        self.0 &= !0x1fff0000;
        self.0 |= (new_len as u32) << 16 & MAX_MTHD_SIZE;
    }
}

pub struct Push {
    /// The internal memory. Has to be uploaded to a BO through flush().
    mem: Vec<u32>,
    /// Last DW that is an incrementing type or usize::MAX
    last_inc: usize,
}

impl Push {
    /// Instantiates a new push buffer.
    pub fn new() -> Self {
        Self {
            mem: Vec::new(),
            last_inc: usize::MAX,
        }
    }

    fn mthd_to_bits(&mut self, subc: u8, addr: u16, bits: u32) {
        let current_len = self.mem.len();
        if let Some(last) = self.mem.get_mut(self.last_inc) {
            let last = MthdHeader::from_bits_mut(last);
            debug_assert!(last.len() >= 1);
            debug_assert!(
                usize::from(last.len()) == current_len - self.last_inc
            );
            if subc == last.subc() {
                match last.inc_type() {
                    Some(IncType::NInc) => {
                        if addr == last.addr() + last.len() * 4 {
                            last.add_len(1);
                            self.mem.push(bits);
                            return;
                        } else if last.len() == 1 && addr == last.addr() {
                            last.set_inc_type(IncType::ZeroInc);
                            last.add_len(1);
                            self.mem.push(bits);
                            return;
                        } else if last.len() == 2 && addr == last.addr() + 4 {
                            last.set_inc_type(IncType::OneInc);
                            last.add_len(1);
                            self.mem.push(bits);
                            return;
                        }
                    }
                    Some(IncType::ZeroInc) => {
                        if addr == last.addr() {
                            last.add_len(1);
                            self.mem.push(bits);
                            return;
                        }
                    }
                    Some(IncType::OneInc) => {
                        if addr == last.addr() + 4 {
                            last.add_len(1);
                            self.mem.push(bits);
                            return;
                        }
                    }
                    None => {}
                }
            }
        }

        // Otherwise, we need a new method header.
        //
        // Methods that use 16bits or lower can be encoded as immediates
        // directly.
        if let Ok(bits16) = u16::try_from(bits) {
            self.last_inc = usize::MAX;
            self.mem.push(MthdHeader::new_immd(bits16, subc, addr));
        } else {
            self.last_inc = self.mem.len();
            let header = MthdHeader::new(IncType::NInc, subc, addr, 1);
            self.mem.push(header.to_bits());
            self.mem.push(bits);
        }
    }

    pub fn push_method<M: Mthd>(&mut self, mthd: M) {
        self.mthd_to_bits(class_to_subc(M::CLASS), M::ADDR, mthd.to_bits());
    }

    pub fn push_array_method<M: ArrayMthd>(&mut self, i: usize, mthd: M) {
        self.mthd_to_bits(class_to_subc(M::CLASS), M::addr(i), mthd.to_bits());
    }

    /// Push an array of dwords into the push buffer
    pub fn push_inline_data(&mut self, data: &[u32]) {
        if self.last_inc != usize::MAX {
            panic!("Inline data must only be placed after a method header");
        }
        self.mem.extend_from_slice(data);
    }

    /// Flushes the internal memory to `out`. Can be used to upload the push
    /// buffer.
    pub fn flush(&mut self, out: &mut [u32]) {
        out.copy_from_slice(&mut self.mem);
        self.mem.clear();
        self.last_inc = usize::MAX;
    }
}
