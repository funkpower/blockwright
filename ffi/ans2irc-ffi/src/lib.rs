//! C ABI over the ans2irc encoder; the declarations are in include/ans2irc.h.

use std::panic::{catch_unwind, AssertUnwindSafe};

use ans2irc::{encode, palette, Cell};

#[repr(C)]
pub struct Ans2ircCell {
    pub ch: u32,
    pub fg: u8,
    pub bg: u8,
}

#[repr(C)]
pub struct Ans2ircBuf {
    pub data: *mut u8,
    pub len: usize,
}

const OK: i32 = 0;
const BAD_ARGUMENT: i32 = 1;
const PANIC: i32 = 2;

const COLOURS: u8 = palette::MIRC_RGB.len() as u8;

fn give(bytes: Vec<u8>, out: &mut Ans2ircBuf) {
    let boxed = bytes.into_boxed_slice();
    out.len = boxed.len();
    out.data = Box::into_raw(boxed) as *mut u8;
}

fn guarded(out: *mut Ans2ircBuf, f: impl FnOnce() -> Result<Vec<u8>, i32>) -> i32 {
    // SAFETY: the caller passes a valid, writable ans2irc_buf or null.
    let Some(out) = (unsafe { out.as_mut() }) else {
        return BAD_ARGUMENT;
    };
    out.data = std::ptr::null_mut();
    out.len = 0;
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(bytes)) => {
            give(bytes, out);
            OK
        }
        Ok(Err(code)) => code,
        Err(_) => PANIC,
    }
}

/// # Safety
/// `cells` points to `n` cells (or is null with `n` 0); `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn ans2irc_encode_row(
    cells: *const Ans2ircCell,
    n: usize,
    swap_shades: i32,
    out: *mut Ans2ircBuf,
) -> i32 {
    guarded(out, || {
        let raw: &[Ans2ircCell] = if n == 0 {
            &[]
        } else if cells.is_null() {
            return Err(BAD_ARGUMENT);
        } else {
            std::slice::from_raw_parts(cells, n)
        };
        let row = raw
            .iter()
            .map(|c| match char::from_u32(c.ch) {
                Some(ch) if c.fg < COLOURS && c.bg < COLOURS => Ok(Cell { ch, fg: c.fg, bg: c.bg }),
                _ => Err(BAD_ARGUMENT),
            })
            .collect::<Result<Vec<Cell>, i32>>()?;
        let opts = encode::Options {
            swap_shades: swap_shades != 0,
        };
        Ok(encode::encode(&row, &opts).into_bytes())
    })
}

#[no_mangle]
pub extern "C" fn ans2irc_nearest(rgb: u32) -> u8 {
    palette::nearest(rgb & 0xff_ffff)
}

/// # Safety
/// `buf` is null or was filled by this library and not freed since.
#[no_mangle]
pub unsafe extern "C" fn ans2irc_buf_free(buf: *mut Ans2ircBuf) {
    let Some(buf) = buf.as_mut() else { return };
    if !buf.data.is_null() {
        drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(buf.data, buf.len)));
    }
    buf.data = std::ptr::null_mut();
    buf.len = 0;
}
