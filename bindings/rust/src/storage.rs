use std::io::{Error, ErrorKind, Result};

use super::*;

extern "C" {
    #[link_name = "block_size"]
    static block_size: ffi::u_int32_t;

    #[link_name = "num_blocks"]
    static num_blocks: ffi::u_int64_t;
}

pub fn storage_block_size() -> Result<usize> {
    let bsize = unsafe { block_size };
    if bsize == 0 {
        return Err(Error::new(ErrorKind::Other, "storage not enabled"));
    }
    Ok(bsize as usize)
}

pub fn storage_num_blocks() -> Result<usize> {
    let nblocks = unsafe { num_blocks };
    if nblocks == 0 {
        return Err(Error::new(ErrorKind::Other, "storage not enabled"));
    }
    Ok(nblocks as usize)
}

pub fn storage_read(buf: &mut [u8], lba: u64) -> Result<usize> {
    let bsize = storage_block_size()?;
    let nblocks = buf.len() / bsize;
    let res = unsafe { ffi::storage_read(buf.as_mut_ptr() as *mut c_void, lba, nblocks as u32) };
    if res < 0 {
        Err(Error::from_raw_os_error(-res))
    } else {
        Ok((nblocks * bsize) as usize)
    }
}

pub fn storage_write(buf: &[u8], lba: u64) -> Result<usize> {
    let bsize = storage_block_size()?;
    let nblocks = buf.len() / bsize;
    let res = unsafe { ffi::storage_write(buf.as_ptr() as *const c_void, lba, nblocks as u32) };
    if res < 0 {
        Err(Error::from_raw_os_error(-res))
    } else {
        Ok((nblocks * bsize) as usize)
    }
}
