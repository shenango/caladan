#![feature(new_uninit)]
#![feature(get_mut_unchecked)]

extern crate byteorder;

use std::arch::asm;
use std::cell::UnsafeCell;
use std::ffi::CString;
use std::mem;
use std::os::raw::{c_int, c_void};
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::Arc;
use std::time::Duration;

mod asm;
pub mod ffi;
pub mod storage;
pub mod tcp;
pub mod thread;
pub mod udp;

pub use crate::asm::*;

fn convert_error(ret: c_int) -> Result<(), i32> {
    if ret == 0 {
        Ok(())
    } else {
        Err(ret as i32)
    }
}

#[inline]
pub fn preempt_enable() {
    let cnt: u32;
    unsafe {
        std::sync::atomic::compiler_fence(std::sync::atomic::Ordering::SeqCst);
        asm!("dec DWORD PTR gs:[rip + {0}]", sym ffi::__perthread_preempt_cnt);
        asm!("mov {0:e}, DWORD PTR gs:[rip + {1}]", out(reg) cnt, sym ffi::__perthread_preempt_cnt);
        if cnt == 0 {
            ffi::preempt();
        }
    }
}

#[inline]
pub fn preempt_disable() {
    unsafe {
        asm!("inc DWORD PTR gs:[rip + {0}]", sym ffi::__perthread_preempt_cnt);
        std::sync::atomic::compiler_fence(std::sync::atomic::Ordering::SeqCst);
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
        ffi::timer_sleep(duration.as_secs() * 1_000_000 + duration.subsec_nanos() as u64 / 1000)
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

#[derive(Clone)]
pub struct WaitGroup {
    inner: Arc<ffi::waitgroup>,
}

impl WaitGroup {
    pub fn new() -> Self {
        let mut inner_uninit = Arc::new_uninit();
        unsafe { ffi::waitgroup_init(Arc::get_mut_unchecked(&mut inner_uninit).as_mut_ptr()) };
        let inner = unsafe { inner_uninit.assume_init() };
        Self { inner }
    }
    pub fn add(&self, count: i32) {
        unsafe { ffi::waitgroup_add(&*self.inner as *const _ as *mut _, count as c_int) }
    }
    pub fn wait(&self) {
        unsafe { ffi::waitgroup_wait(&*self.inner as *const _ as *mut _) }
    }
    pub fn done(&self) {
        self.add(-1)
    }
}

impl Default for WaitGroup {
    fn default() -> Self {
        WaitGroup::new()
    }
}

unsafe impl Send for WaitGroup {}
unsafe impl Sync for WaitGroup {}

pub struct SpinLock {
    inner: UnsafeCell<ffi::spinlock_t>,
}

impl SpinLock {
    pub fn new() -> Self {
        Self {
            inner: UnsafeCell::new(ffi::spinlock_t { locked: 0 }),
        }
    }

    #[allow(clippy::mut_from_ref)]
    #[inline]
    unsafe fn as_atomic(&self) -> &mut AtomicI32 {
        mem::transmute(&mut (*self.inner.get()).locked)
    }

    #[inline]
    fn as_raw(&self) -> *mut ffi::spinlock_t {
        self.inner.get()
    }

    #[inline]
    pub fn lock_np(&self) {
        preempt_disable();
        self.lock();
    }

    #[inline]
    pub fn unlock_np(&self) {
        self.unlock();
        preempt_enable();
    }

    #[inline]
    pub fn lock(&self) {
        let inner = unsafe { self.as_atomic() };
        while inner.swap(1, Ordering::Acquire) != 0 {
            while inner.load(Ordering::Relaxed) != 0 {
                cpu_relax();
            }
        }
    }

    #[inline]
    pub fn try_lock(&self) -> bool {
        let inner = unsafe { self.as_atomic() };
        inner.swap(1, Ordering::Acquire) == 0
    }

    #[inline]
    pub fn unlock(&self) {
        let inner = unsafe { self.as_atomic() };
        assert_eq!(inner.swap(0, Ordering::Release), 1);
    }
}

impl Default for SpinLock {
    fn default() -> Self {
        Self::new()
    }
}

unsafe impl Send for SpinLock {}
unsafe impl Sync for SpinLock {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn spinlock() {
        let lock = SpinLock::new();

        lock.lock();
        assert!(!lock.try_lock());
        lock.unlock();
        assert!(lock.try_lock());
    }
}
