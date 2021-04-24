// sync.h - support for synchronization primitives

#pragma once

extern "C" {
#include <base/lock.h>
#include <base/stddef.h>
#include <runtime/sync.h>
}

namespace rt {

// Spin lock support.
class Spin {
 public:
  Spin() { spin_lock_init(&lock_); }
  ~Spin() { assert(!spin_lock_held(&lock_)); }

  // Locks the spin lock.
  void Lock() { spin_lock_np(&lock_); }

  // Unlocks the spin lock.
  void Unlock() { spin_unlock_np(&lock_); }

  // Locks the spin lock only if it is currently unlocked. Returns true if
  // successful.
  bool TryLock() { return spin_try_lock_np(&lock_); }

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

 private:
  mutex_t mu_;

  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;
};

// RAII lock support (works with both Spin and Mutex).
template <typename L>
class ScopedLock {
 public:
  explicit ScopedLock(L *lock) : lock_(lock) { lock_->Lock(); }
  ~ScopedLock() { lock_->Unlock(); }

 private:
  L *const lock_;

  ScopedLock(const ScopedLock&) = delete;
  ScopedLock& operator=(const ScopedLock&) = delete;
};

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
