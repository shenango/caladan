use crate::ffi::{poll_arm, poll_init, poll_trigger, poll_trigger_t, poll_wait, poll_waiter_t};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};

pub struct PollWaiter {
    inner: Arc<poll_waiter_t>,
    active_triggers: Vec<Option<(Arc<poll_trigger_t>, u32)>>,
    empty_slots: Vec<usize>,
    done: Arc<AtomicBool>,
}

unsafe impl Send for PollWaiter {}
unsafe impl Sync for PollWaiter {}

impl PollWaiter {
    pub fn new() -> Self {
        let mut poll = Arc::new_uninit();
        let inner = unsafe {
            poll_init(Arc::get_mut_unchecked(&mut poll).as_mut_ptr());
            poll.assume_init()
        };

        PollWaiter {
            inner,
            active_triggers: Default::default(),
            empty_slots: Default::default(),
            done: Default::default(),
        }
    }

    fn get_slot(&mut self) -> usize {
        self.empty_slots.pop().unwrap_or(self.active_triggers.len())
    }

    pub fn trigger(&mut self, data: u32) -> PollTrigger {
        let idx = self.get_slot();
        let trigger = unsafe {
            let trigger = Arc::new_zeroed().assume_init();
            poll_arm(
                &*self.inner as *const _ as *mut _,
                &*trigger as *const _ as *mut _,
                idx as _,
            );
            trigger
        };

        // because triggers happen via drop, we need to ensure that their memory is not freed
        // before wait() can access it.
        if idx >= self.active_triggers.len() {
            self.active_triggers
                .push(Some((Arc::clone(&trigger), data)));
        } else {
            self.active_triggers[idx] = Some((Arc::clone(&trigger), data));
        }

        PollTrigger {
            inner: trigger,
            waiter: Arc::clone(&self.inner),
            waiter_done: Arc::clone(&self.done),
        }
    }

    pub fn wait(&mut self) -> u32 {
        let idx = unsafe { poll_wait(&*self.inner as *const _ as *mut _) as _ };
        assert!(
            idx < self.active_triggers.len(),
            "poll_wait returned bad index"
        );

        let ent: &mut Option<_> = &mut self.active_triggers[idx];
        match ent.take() {
            Some((_, v)) => {
                // the trigger object, _, will get dropped now.
                self.empty_slots.push(idx);
                v
            }
            None => {
                panic!("poll_wait returned index to empty trigger slot");
            }
        }
    }
}

impl Drop for PollWaiter {
    fn drop(&mut self) {
        self.done.store(true, Ordering::SeqCst);
    }
}

pub struct PollTrigger {
    inner: Arc<poll_trigger_t>,
    waiter: Arc<poll_waiter_t>,
    waiter_done: Arc<AtomicBool>,
}

unsafe impl Send for PollTrigger {}
unsafe impl Sync for PollTrigger {}

impl Drop for PollTrigger {
    fn drop(&mut self) {
        if self.waiter_done.load(Ordering::SeqCst) {
            return;
        }

        unsafe {
            poll_trigger(
                &*self.waiter as *const _ as *mut _,
                &*self.inner as *const _ as *mut _,
            );
        }
    }
}
