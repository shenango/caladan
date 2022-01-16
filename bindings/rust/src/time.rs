use crate::ffi::{self, timer_entry};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
use std::time::Duration;

pub struct Timer {
    inner: Arc<timer_entry>,
    done: Arc<AtomicBool>,
}

unsafe impl Send for Timer {}
unsafe impl Sync for Timer {}

impl Clone for Timer {
    fn clone(&self) -> Self {
        Self {
            inner: Arc::clone(&self.inner),
            done: Default::default(),
        }
    }
}

impl Timer {
    pub fn new() -> Self {
        let mut entry = Arc::new_uninit();
        let inner = unsafe {
            ffi::timer_sleep_init(Arc::get_mut_unchecked(&mut entry).as_mut_ptr());
            entry.assume_init()
        };

        Self {
            inner,
            done: Default::default(),
        }
    }

    pub fn sleep(self, dur: Duration) {
        if self.done.load(Ordering::SeqCst) {
            return;
        }

        unsafe { ffi::timer_sleep_wait(&*self.inner as *const _ as *mut _, dur.as_micros() as _) };
        self.done.store(true, Ordering::SeqCst);
    }

    pub fn cancel(self) {
        match self
            .done
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
        {
            Ok(false) => {
                unsafe { ffi::timer_sleep_cancel(&*self.inner as *const _ as *mut _) };
            }
            Err(true) => {
                return;
            }
            _ => unreachable!(),
        }
    }
}

pub fn sleep(duration: Duration) {
    unsafe {
        ffi::timer_sleep(duration.as_secs() * 1_000_000 + duration.subsec_nanos() as u64 / 1000)
    }
}
