//! Sync facilities.

use crate::{cpu_relax, ffi, preempt_disable, preempt_enable};
use std::cell::UnsafeCell;
use std::mem;
use std::ops::{Deref, DerefMut};
use std::os::raw::c_int;
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::Arc;

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
    pub fn as_raw(&self) -> *mut ffi::spinlock_t {
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
