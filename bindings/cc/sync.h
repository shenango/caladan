// sync.h - support for synchronization primitives

#pragma once

extern "C" {
#include <base/lock.h>
#include <base/stddef.h>
#include <runtime/sync.h>
#include <runtime/thread.h>
}

#include <type_traits>

namespace rt {

// Force the compiler to access a memory location.
template<typename T>
T volatile &access_once(T &t) {
  static_assert(std::is_integral<T>::value, "Integral required.");
  return static_cast<T volatile &>(t);
}

// Force the compiler to read a memory location.
template<typename T>
T read_once(const T &p) {
  static_assert(std::is_integral<T>::value, "Integral required.");
  return static_cast<const T volatile &>(p);
}

// Force the compiler to write a memory location.
template<typename T>
void write_once(T &p, const T &val) {
  static_assert(std::is_integral<T>::value, "Integral required.");
  static_cast<T volatile &>(p) = val;
}

// ThreadWaker is used to wake the current thread after it parks.
class ThreadWaker {
 public:
  ThreadWaker() : th_(nullptr) {}
  ~ThreadWaker() { assert(th_ == nullptr); }

  // disable copy.
  ThreadWaker(const ThreadWaker&) = delete;
  ThreadWaker& operator=(const ThreadWaker&) = delete;

  // allow move.
  ThreadWaker(ThreadWaker &&w) : th_(w.th_) { w.th_ = nullptr; }
  ThreadWaker& operator=(ThreadWaker &&w) {
    th_ = w.th_;
    w.th_ = nullptr;
    return *this;
  }

  // Prepares the running thread for waking after it parks.
  void Arm() { th_ = thread_self(); }

  // Makes the parked thread runnable. Must be called by another thread after
  // the prior thread has called Arm() and has parked (or will park in the
  // immediate future).
  void Wake(bool head = false) {
    if (th_ == nullptr) return;
    thread_t *th = th_;
    th_ = nullptr;
    if (head) {
      thread_ready_head(th);
    } else {
      thread_ready(th);
    }
  }

 private:
  thread_t *th_;
};

// Disables preemption across a critical section.
class Preempt {
 public:
  Preempt() {}
  ~Preempt() {}

  // disable move and copy.
  Preempt(const Preempt&) = delete;
  Preempt& operator=(const Preempt&) = delete;

  // Disables preemption.
  void Lock() { preempt_disable(); }

  // Enables preemption.
  void Unlock() { preempt_enable(); }

  // Atomically enables preemption and parks the running thread.
  void UnlockAndPark() { thread_park_and_preempt_enable(); }

  // Returns true if preemption is currently disabled.
  bool IsHeld() const { return !preempt_enabled(); }

  // Returns true if preemption is needed. Will be handled on Unlock() or on
  // UnlockAndPark().
  bool PreemptNeeded() const {
    assert(IsHeld());
    return preempt_needed();
  }

  // Gets the current CPU index (not the same as the core number).
  unsigned int get_cpu() const {
    assert(IsHeld());
    return perthread_read(kthread_idx);
  }
};

// Spin lock support.
class Spin {
 public:
  Spin() { spin_lock_init(&lock_); }
  ~Spin() { assert(!spin_lock_held(&lock_)); }

  // Locks the spin lock.
  void Lock() { spin_lock_np(&lock_); }

  // Unlocks the spin lock.
  void Unlock() { spin_unlock_np(&lock_); }

  // Atomically unlocks the spin lock and parks the running thread.
  void UnlockAndPark() { thread_park_and_unlock_np(&lock_); }

  // Locks the spin lock only if it is currently unlocked. Returns true if
  // successful.
  bool TryLock() { return spin_try_lock_np(&lock_); }

  // Returns true if the lock is currently held.
  bool IsHeld() { return spin_lock_held(&lock_); }

  // Returns true if preemption is needed. Will be handled on Unlock() or on
  // UnlockAndPark().
  bool PreemptNeeded() {
    assert(IsHeld());
    return preempt_needed();
  }

  // Gets the current CPU index (not the same as the core number).
  unsigned int get_cpu() {
    assert(IsHeld());
    return perthread_read(kthread_idx);
  }

 private:
  spinlock_t lock_;

  Spin(const Spin&) = delete;
  Spin& operator=(const Spin&) = delete;
};

// Pthread-like mutex support.
class Mutex {
  friend class CondVar;

 public:
  Mutex() { mutex_init(&mu_); }
  ~Mutex() { assert(!mutex_held(&mu_)); }

  // Locks the mutex.
  void Lock() { mutex_lock(&mu_); }

  // Unlocks the mutex.
  void Unlock() { mutex_unlock(&mu_); }

  // Locks the mutex only if it is currently unlocked. Returns true if
  // successful.
  bool TryLock() { return mutex_try_lock(&mu_); }

  // Returns true if the mutex is currently held.
  bool IsHeld() { return mutex_held(&mu_); }

 private:
  mutex_t mu_;

  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;
};

// RAII lock support (works with Spin, Preempt, and Mutex).
template <typename L>
class ScopedLock {
 public:
  explicit ScopedLock(L *lock) : lock_(lock) { lock_->Lock(); }
  ~ScopedLock() { lock_->Unlock(); }

  // Park is useful for blocking and waiting on a condition.
  // Only works with Spin and Preempt (not Mutex).
  // Example:
  // rt::ThreadWaker w;
  // rt::SpinLock l;
  // rt::SpinGuard guard(l);
  // while (condition) guard.Park(&w);
  void Park(ThreadWaker *w) {
    assert(lock_->IsHeld());
    w->Arm();
    lock_->UnlockAndPark();
    lock_->Lock();
  }

 private:
  L *const lock_;

  ScopedLock(const ScopedLock&) = delete;
  ScopedLock& operator=(const ScopedLock&) = delete;
};

using SpinGuard = ScopedLock<Spin>;
using MutexGuard = ScopedLock<Mutex>;
using PreemptGuard = ScopedLock<Preempt>;

// RAII lock and park support (works with both Spin and Preempt).
template <typename L>
class ScopedLockAndPark {
 public:
  explicit ScopedLockAndPark(L *lock) : lock_(lock) { lock_->Lock(); }
  ~ScopedLockAndPark() { lock_->UnlockAndPark(); }

 private:
  L *const lock_;

  ScopedLockAndPark(const ScopedLockAndPark&) = delete;
  ScopedLockAndPark& operator=(const ScopedLockAndPark&) = delete;
};

using SpinGuardAndPark = ScopedLockAndPark<Spin>;
using PreemptGuardAndPark = ScopedLockAndPark<Preempt>;

// Pthread-like condition variable support.
class CondVar {
 public:
  CondVar() { condvar_init(&cv_); };
  ~CondVar() {}

  // Block until the condition variable is signaled. Recheck the condition
  // after wakeup, as no guarantees are made about preventing spurious wakeups.
  void Wait(Mutex *mu) { condvar_wait(&cv_, &mu->mu_); }

  // Wake up one waiter.
  void Signal() { condvar_signal(&cv_); }

  // Wake up all waiters.
  void SignalAll() { condvar_broadcast(&cv_); }

 private:
  condvar_t cv_;

  CondVar(const CondVar&) = delete;
  CondVar& operator=(const CondVar&) = delete;
};

// Golang-like waitgroup support.
class WaitGroup {
 public:
  // initializes a waitgroup with zero jobs.
  WaitGroup() { waitgroup_init(&wg_); };

  // Initializes a waitgroup with @count jobs.
  WaitGroup(int count) {
    waitgroup_init(&wg_);
    waitgroup_add(&wg_, count);
  }

  ~WaitGroup() { assert(wg_.cnt == 0); };

  // Changes the number of jobs (can be negative).
  void Add(int count) { waitgroup_add(&wg_, count); }

  // Decrements the number of jobs by one.
  void Done() { Add(-1); }

  // Block until the number of jobs reaches zero.
  void Wait() { waitgroup_wait(&wg_); }

 private:
  waitgroup_t wg_;

  WaitGroup(const WaitGroup&) = delete;
  WaitGroup& operator=(const WaitGroup&) = delete;
};

}  // namespace rt
