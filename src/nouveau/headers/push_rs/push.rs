// Copyright © 2024 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use nvidia_headers::ArrayMthd;
use nvidia_headers::Mthd;

use std::fmt;

type Result<T> = std::result::Result<T, Error>;

#[derive(Debug)]
pub enum Error {
    Overflow,
    CountExceeded,
    UnterminatedMethod,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match *self {
            Error::Overflow => write!(f, "overflowed the current dword"),
            Error::CountExceeded => {
                write!(f, "exceeded the maximum dword count")
            }
            Error::UnterminatedMethod => {
                write!(f, "the current method is empty and cannot be issued")
            }
        }
    }
}

impl std::error::Error for Error {}

pub enum PushType {
    /// Each dword increments the address by one
    NInc,
    /// The first dword increments the address by one
    OneInc,
    /// The address is not incremented
    ZeroInc,
    /// Push an immediate
    Immd(u32),
}

pub enum PushMethodType<'a> {
    Method(&'a dyn Mthd),
    ArrayMthd(&'a dyn ArrayMthd, usize),
}

/// Used to issue methods
pub struct PushMethod<'a> {
    /// The subchannel to issue the method against
    pub subchannel: Subchannel,
    /// The method type
    pub method_type: PushMethodType<'a>,
    /// The type of push to use
    pub push_type: PushType,
}

impl<'a> PushMethod<'a> {
    fn to_bits(self) -> u32 {
        let addr = match self.method_type {
            PushMethodType::Method(method) => method.addr(),
            PushMethodType::ArrayMthd(method, index) => method.addr(index),
        };

        let addr = u32::from(addr);

        // We update the size in place (through update_size())
        let size = 0;
        let subc = u32::from(self.subchannel);
        match self.push_type {
            PushType::NInc => {
                0x20000000 | (size << 16) | (subc << 13) | (addr >> 2)
            }
            PushType::OneInc => {
                0xa0000000 | (size << 16) | (subc << 13) | (addr >> 2)
            }
            PushType::ZeroInc => {
                0x60000000 | (size << 16) | (subc << 13) | (addr >> 2)
            }
            PushType::Immd(immd) => {
                0x80000000 | (immd << 16) | (subc << 13) | (addr >> 2)
            }
        }
    }
}

/// The subchannels to issue methods against.
pub enum Subchannel {
    Nv9097,
    NvA097,
    NvB097,
    NvB197,
    NvC097,
    NvC397,
    Nv90C0,
    NvA0C0,
    NvB0C0,
    NvC0C0,
    NvC3C0,
    NvC6C0,
    Nv9039,
    Nv902D,
    Nv90B5,
    NvC1B5,
}

impl From<Subchannel> for u32 {
    fn from(v: Subchannel) -> Self {
        match v {
            Subchannel::Nv9097
            | Subchannel::NvA097
            | Subchannel::NvB097
            | Subchannel::NvB197
            | Subchannel::NvC097
            | Subchannel::NvC397 => 0,
            Subchannel::Nv90C0
            | Subchannel::NvA0C0
            | Subchannel::NvB0C0
            | Subchannel::NvC0C0
            | Subchannel::NvC3C0
            | Subchannel::NvC6C0 => 1,
            Subchannel::Nv9039 => 2,
            Subchannel::Nv902D => 3,
            Subchannel::Nv90B5 | Subchannel::NvC1B5 => 4,
        }
    }
}

pub struct Push {
    /// The internal memory. Has to be uploaded to a BO through flush().
    mem: Vec<u32>,
    /// Where the last size can be found in the internal memory. This is used to
    /// update the current size in place.
    last_size: usize,
}

impl Push {
    pub const MAX_COUNT: u32 = 0x1fff;

    /// Construct a empty push buffer. Does not allocate any storage.
    pub fn new() -> Self {
        Self {
            mem: vec![],
            last_size: 0,
        }
    }

    /// Flushes the internal memory to `out`. Can be used to upload the push
    /// buffer.
    pub fn flush(&mut self, out: &mut [u32]) {
        out.copy_from_slice(&mut self.mem);
        self.mem.clear();
        self.last_size = 0;
    }

    /// Returns an error if the previous method was unterminated.
    fn verify(&self) -> Result<()> {
        if self.last_size == 0 {
            return Ok(());
        }

        let last_hdr = self.mem[self.last_size];

        // Check for immd
        if last_hdr >> 29 == 4 {
            return Ok(());
        }

        // make sure we don't add a new method if the last one wasn't used
        let last_count = last_hdr & 0x1fff0000;
        if last_count == 0 {
            return Err(Error::UnterminatedMethod);
        }

        Ok(())
    }

    /// Returns the number of dwords in the push buffer
    pub fn dw_count(&self) -> usize {
        self.mem.len()
    }

    /// Begins a method.
    pub fn push_method(&mut self, method: PushMethod) -> Result<()> {
        self.verify()?;
        self.last_size = self.mem.len();
        match method.push_type {
            PushType::Immd(immd) => {
                if immd & !0x1fff == 0 {
                    self.mem.push(method.to_bits());
                } else {
                    let method = PushMethod {
                        push_type: PushType::NInc,
                        ..method
                    };
                    self.mem.push(method.to_bits());
                    self.push_inline_data(immd)?;
                }
            }
            _ => {
                self.mem.push(method.to_bits());
            }
        }

        Ok(())
    }

    /// Update the size of the current method to account for new data.
    pub fn update_size(&mut self, count: usize) -> Result<()> {
        let count = u32::try_from(count).unwrap();
        if count > Self::MAX_COUNT {
            return Err(Error::CountExceeded);
        }

        let mut last_hdr_val = self.mem[self.last_size];
        // size is encoded at 28:16
        let new_count = (count + (last_hdr_val >> 16)) & Self::MAX_COUNT;

        let overflow = new_count < count;
        if overflow {
            return Err(Error::Overflow);
        }

        last_hdr_val &= !0x1fff0000;
        last_hdr_val |= new_count << 16;
        self.mem[self.last_size] = last_hdr_val;
        Ok(())
    }

    /// Pushes a u32 into the push buffer without checking the current method.
    pub fn push_inline_data(&mut self, value: u32) -> Result<()> {
        self.update_size(1)?;
        self.mem.push(value);
        Ok(())
    }

    /// Pushes an inline float into the push buffer
    pub fn push_inline_float(&mut self, value: f32) -> Result<()> {
        self.push_inline_data(value.to_bits())
    }

    /// Pushes an inline array into the push buffer
    pub fn push_inline_array(&mut self, data: &[u32]) -> Result<()> {
        self.update_size(data.len())?;
        self.mem.extend_from_slice(data);
        Ok(())
    }

    /// Pushes the contents of `mthd` into the push buffer.
    pub fn push_value(&mut self, method: PushMethodType) -> Result<()> {
        let last_hdr_val = self.mem[self.last_size];
        let is_0inc = (last_hdr_val & 0xe0000000) == 0x60000000;
        let is_1inc = (last_hdr_val & 0xe0000000) == 0xa0000000;
        let is_immd = (last_hdr_val & 0xe0000000) == 0x80000000;
        let last_method = (last_hdr_val & 0x1fff) << 2;

        let mut distance =
            u32::try_from(self.mem.len() - self.last_size - 1).unwrap();
        if is_0inc {
            distance = 0;
        } else if is_1inc {
            distance = std::cmp::min(1, distance);
        }

        let current_address = last_method + distance * 4;

        assert!(last_hdr_val != 0, "empty headers: push a method first");
        assert!(!is_immd, "use push_immd()");

        let val = match method {
            PushMethodType::Method(mthd) => {
                assert!(
                    current_address == u32::from(mthd.addr()),
                    "address mismatch"
                );
                mthd.to_bits()
            }

            PushMethodType::ArrayMthd(mthd, index) => {
                assert!(
                    current_address == u32::from(mthd.addr(index)),
                    "address mismatch"
                );
                mthd.to_bits()
            }
        };
        self.push_inline_data(val)
    }

    /// Pushes raw data into the push buffer
    pub fn push_raw(&mut self, data: &[u32]) {
        self.mem.extend_from_slice(data);
        self.last_size = 0;
    }
}
