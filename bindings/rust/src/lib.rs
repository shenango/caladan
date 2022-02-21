#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![feature(llvm_asm)]
#![feature(integer_atomics)]
#![feature(thread_local)]
#![feature(new_uninit)]
#![feature(get_mut_unchecked)]

use std::ffi::CString;
use std::os::raw::{c_int, c_void};
use std::time::Duration;

pub mod ffi {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

mod asm;
pub mod poll;
pub mod storage;
pub mod sync;
// preserve import paths.
pub use sync::{SpinLock, WaitGroup};
pub mod tcp;
pub mod thread;
pub mod udp;

pub use asm::*;

fn convert_error(ret: c_int) -> Result<(), i32> {
    if ret == 0 {
        Ok(())
    } else {
        Err(ret as i32)
    }
}

#[inline]
pub fn preempt_enable() {
    unsafe {
        llvm_asm!("" ::: "memory" : "volatile");
        llvm_asm!("subl $$1, %fs:preempt_cnt@tpoff" : : : "memory", "cc" : "volatile");
        if ffi::preempt_cnt == 0 {
            ffi::preempt();
        }
    }
}

#[inline]
pub fn preempt_disable() {
    unsafe {
        llvm_asm!("addl $$1, %fs:preempt_cnt@tpoff" : : : "memory", "cc" : "volatile");
        llvm_asm!("" ::: "memory" : "volatile");
    }
}

#[allow(unused)]
pub fn base_init() -> Result<(), i32> {
    convert_error(unsafe { ffi::base_init() })
}

#[allow(unused)]
pub fn base_init_thread() -> Result<(), i32> {
    convert_error(unsafe { ffi::base_init_thread() })
}

pub fn delay_us(microseconds: u64) {
    unsafe { ffi::__time_delay_us(microseconds) }
}

#[inline]
pub fn microtime() -> u64 {
    unsafe { (rdtsc() - ffi::start_tsc as u64) / ffi::cycles_per_us as u64 }
}

pub fn sleep(duration: Duration) {
    unsafe {
        ffi::timer_sleep(duration.as_secs() * 1000_000 + duration.subsec_nanos() as u64 / 1000)
    }
}

pub fn runtime_init<F>(cfgpath: String, f: F) -> Result<(), i32>
where
    F: FnOnce(),
    F: Send + 'static,
{
    convert_error(unsafe {
        ffi::runtime_init(
            CString::new(cfgpath).unwrap().into_raw(),
            Some(thread::box_trampoline::<F>),
            Box::into_raw(Box::new(f)) as *mut c_void,
        )
    })
}
