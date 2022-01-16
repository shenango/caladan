//! Sync facilities.

use crate::{cpu_relax, ffi, preempt_disable, preempt_enable};
use std::cell::UnsafeCell;
use std::mem;
use std::ops::{Deref, DerefMut};
use std::os::raw::c_int;
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::Arc;

pub struct Mutex<T> {
    data: Arc<UnsafeCell<T>>,
    inner: Arc<ffi::mutex>,
}

impl<T> Clone for Mutex<T> {
    fn clone(&self) -> Self {
        Self {
            data: Arc::clone(&self.data),
            inner: Arc::clone(&self.inner),
        }
    }
}

unsafe impl<T> Send for Mutex<T> {}
unsafe impl<T> Sync for Mutex<T> {}

impl<T: Default> Default for Mutex<T> {
    fn default() -> Self {
        Mutex::new(Default::default())
    }
}

impl<T> Mutex<T> {
    pub fn new(data: T) -> Self {
        let mut uninit = Arc::new_uninit();
        unsafe {
            ffi::mutex_init(Arc::get_mut_unchecked(&mut uninit).as_mut_ptr());
            Self {
                data: Arc::new(UnsafeCell::new(data)),
                inner: uninit.assume_init(),
            }
        }
    }

    pub fn lock(&self) -> MutexGuard<T> {
        unsafe {
            ffi::mutex_lock_(&*self.inner as *const _ as *mut _);
            MutexGuard { mutex: &self }
        }
    }

    pub fn try_lock(&self) -> Result<MutexGuard<T>, ()> {
        if unsafe { ffi::mutex_try_lock_(&*self.inner as *const _ as *mut _) } {
            Ok(MutexGuard { mutex: &self })
        } else {
            Err(())
        }
    }

    pub unsafe fn lock_manual(&self) {
        ffi::mutex_lock_(&*self.inner as *const _ as *mut _);
    }

    pub unsafe fn unlock_manual(&self) {
        ffi::assert_mutex_held_(&*self.inner as *const _ as *mut _);
        ffi::mutex_unlock_(&*self.inner as *const _ as *mut _);
    }

    pub unsafe fn inner(&self) -> *mut T {
        self.data.get()
    }
}

pub struct MutexGuard<'a, T> {
    mutex: &'a Mutex<T>,
}

impl<'a, T> Deref for MutexGuard<'a, T> {
    type Target = T;
    fn deref(&self) -> &Self::Target {
        unsafe { &*self.mutex.data.get() }
    }
}

impl<'a, T: Sized> DerefMut for MutexGuard<'a, T> {
    fn deref_mut(&mut self) -> &mut T {
        unsafe { &mut *self.mutex.data.get() }
    }
}

impl<'a, T> Drop for MutexGuard<'a, T> {
    fn drop(&mut self) {
        unsafe {
            ffi::assert_mutex_held_(&*self.mutex.inner as *const _ as *mut _);
            ffi::mutex_unlock_(&*self.mutex.inner as *const _ as *mut _)
        }
    }
}

#[derive(Clone)]
pub struct Condvar {
    inner: Arc<ffi::condvar_t>,
}

unsafe impl Send for Condvar {}
unsafe impl Sync for Condvar {}

impl Condvar {
    pub fn new() -> Self {
        let mut inner_uninit = Arc::new_uninit();
        unsafe { ffi::condvar_init(Arc::get_mut_unchecked(&mut inner_uninit).as_mut_ptr()) };
        let inner = unsafe { inner_uninit.assume_init() };
        Self { inner }
    }

    pub fn signal(&self) {
        unsafe { ffi::condvar_signal(&*self.inner as *const _ as *mut _) }
    }

    pub fn broadcast(&self) {
        unsafe { ffi::condvar_broadcast(&*self.inner as *const _ as *mut _) }
    }

    pub fn wait<'m, T>(&self, mux: &'m Mutex<T>) -> MutexGuard<'m, T> {
        unsafe {
            // 1. lock the mutex.
            mux.lock_manual();
            // 2. condvar_wait will unlock the mutex and sleep internally.
            // When it returns, the mutex will be re-locked.
            ffi::condvar_wait(
                &*self.inner as *const _ as *mut _,
                &*mux.inner as *const _ as *mut _,
            );
        }

        MutexGuard { mutex: mux }
    }
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
