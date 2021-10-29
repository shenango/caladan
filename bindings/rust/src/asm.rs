#[inline]
pub fn cpu_relax() {
    unsafe { core::arch::x86_64::_mm_pause() }
}

#[inline]
pub fn cpu_serialize() {
    unsafe {
        core::arch::x86_64::__cpuid(0);
    }
}

#[inline]
pub fn rdtsc() -> u64 {
    unsafe { core::arch::x86_64::_rdtsc() }
}

#[inline]
pub fn rdtscp() -> (u64, u32) {
    let mut aux: u32 = 0;
    let tsc = unsafe { core::arch::x86_64::__rdtscp(&mut aux as *mut u32) };
    (tsc, aux)
}
