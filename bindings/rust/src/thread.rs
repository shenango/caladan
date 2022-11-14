use std::any::Any;
use std::cell::UnsafeCell;
use std::os::raw::c_void;
use std::{mem, panic, ptr};

use super::*;

pub(crate) fn thread_self() -> *mut ffi::thread_t {
    let t: * mut ffi::thread_t;
    unsafe {
        asm!("mov {0:r}, QWORD PTR gs:[rip + {1}]", out(reg) t, sym ffi::__perthread___self);
    }
    t
}

pub fn thread_yield() {
    unsafe { ffi::thread_yield() }
}

pub(crate) extern "C" fn trampoline<F>(arg: *mut c_void)
where
    F: FnOnce(),
    F: Send + 'static,
{
    let f = arg as *mut F;
    let f: F = unsafe { mem::transmute_copy(&*f as &F) };
    let _result = panic::catch_unwind(panic::AssertUnwindSafe(f));
}

pub(crate) extern "C" fn box_trampoline<F>(arg: *mut c_void)
where
    F: FnOnce(),
    F: Send + 'static,
{
    let f = unsafe { Box::from_raw(arg as *mut F) };
    let _result = panic::catch_unwind(panic::AssertUnwindSafe(f));
}
pub(crate) extern "C" fn base_trampoline<T, F>(arg: *mut c_void)
where
    F: FnOnce() -> T,
    F: Send + 'static,
    T: Send + 'static,
{
    // Run closure.
    let base = arg as *mut StackBase<T, F>;
    let base = unsafe { &mut *base };
    let f: F = base.f.take().unwrap();
    let result = panic::catch_unwind(panic::AssertUnwindSafe(f));

    // Set return value.
    let d = unsafe { &mut *base.join_data.get() };
    d.lock.lock_np();
    d.data = Some(result);

    // If another thread called detach on this one, exit immediately.
    if d.done && d.waiter.is_null() {
        preempt_enable();
        return;
    }

    // If another thread called join on this one, wake it now.
    if d.done {
        let waiter = d.waiter;
        assert!(!waiter.is_null());
        unsafe { ffi::thread_ready(waiter) };
    }

    // Don't exit until the parent thread calls join or detach.
    d.done = true;
    d.waiter = thread_self();
    unsafe { ffi::thread_park_and_unlock_np(d.lock.as_raw()) };
}

struct JoinData<T: Send + 'static> {
    lock: SpinLock,
    done: bool,
    waiter: *mut ffi::thread_t,
    data: Option<Result<T, Box<dyn Any + Send + 'static>>>,
}

struct StackBase<T, F>
where
    F: FnOnce() -> T,
    F: Send + 'static,
    T: Send + 'static,
{
    f: Option<F>,
    join_data: UnsafeCell<JoinData<T>>,
}

pub struct JoinHandle<T: Send + 'static> {
    join_data: *const UnsafeCell<JoinData<T>>,
}
impl<T: Send + 'static> JoinHandle<T> {
    pub fn join(mut self) -> Result<T, Box<dyn Any + Send + 'static>> {
        unsafe {
            let join_data = (&*self.join_data).get();
            self.join_data = ptr::null();
            (*join_data).lock.lock_np();

            // If the thread isn't done, block until it is.
            if !(*join_data).done {
                (*join_data).done = true;
                (*join_data).waiter = thread_self();
                ffi::thread_park_and_unlock_np((*join_data).lock.as_raw());
                (*join_data).lock.lock_np();
            }

            // Extract the return value, and let the other thread exit.
            let data = (*join_data).data.take().unwrap();
            assert!((*join_data).waiter != ptr::null_mut());
            ffi::thread_ready((*join_data).waiter);
            preempt_enable();
            data
        }
    }
}
impl<T: Send + 'static> Drop for JoinHandle<T> {
    fn drop(&mut self) {
        if !self.join_data.is_null() {
            let join_data: &mut JoinData<T> = unsafe { &mut *(&*self.join_data).get() };

            join_data.lock.lock_np();
            if join_data.done {
                join_data.lock.unlock_np();
                assert!(join_data.waiter != ptr::null_mut());
                unsafe { ffi::thread_ready(join_data.waiter) };
            } else {
                join_data.done = true;
                join_data.waiter = ptr::null_mut();
                join_data.lock.unlock_np();
            }
        }
    }
}

pub fn spawn_detached<F>(mut f: F)
where
    F: FnOnce(),
    F: Send + 'static,
{
    let mut buf: *mut F = ptr::null_mut();
    let th = unsafe {
        ffi::thread_create_with_buf(
            Some(trampoline::<F>),
            &mut buf as *mut *mut F as *mut *mut c_void,
            mem::size_of::<F>() as _,
        )
    };
    assert!(!th.is_null());
    assert!(!buf.is_null());
    unsafe {
        ffi::memcpy(
            buf as *mut c_void,
            &mut f as *mut F as *mut c_void,
            mem::size_of::<F>() as _,
        );
        mem::forget(f);
        ffi::thread_ready(th)
    };
}

pub fn spawn<T, F>(f: F) -> JoinHandle<T>
where
    F: FnOnce() -> T,
    F: Send + 'static,
    T: Send + 'static,
{
    // Create thread and get a pointer to a buffer allocated on its stack.
    let mut buf: *mut StackBase<T, F> = ptr::null_mut();
    let th = unsafe {
        ffi::thread_create_with_buf(
            Some(base_trampoline::<T, F>),
            &mut buf as *mut *mut StackBase<T, F> as *mut *mut c_void,
            mem::size_of::<StackBase<T, F>>() as _,
        )
    };
    assert!(!th.is_null());
    assert!(!buf.is_null());

    // Push the closure and join data onto the threads stack.
    let mut base = StackBase {
        f: Some(f),
        join_data: UnsafeCell::new(JoinData {
            lock: SpinLock::new(),
            done: false,
            waiter: ptr::null_mut(),
            data: None,
        }),
    };
    unsafe {
        ffi::memcpy(
            buf as *mut c_void,
            &mut base as *mut StackBase<T, F> as *mut c_void,
            mem::size_of::<StackBase<T, F>>() as _,
        );
    }
    mem::forget(base);

    // Start the thread.
    unsafe { ffi::thread_ready(th) };

    // Construct a JoinHandle for the new thread.
    #[rustfmt::skip]
    let &mut StackBase { ref mut join_data, .. } = unsafe { &mut *buf };
    JoinHandle {
        join_data: join_data as *const UnsafeCell<JoinData<T>>,
    }
}
