use std::arch::asm;

#[inline]
pub fn cpu_relax() {
    unsafe { asm!("pause", options(att_syntax)) };
}
#[inline]
pub fn cpu_serialize() {
    unsafe { asm!("cpuid", out("rax") _, out("rcx") _, out("rdx")_, options(att_syntax)) };
}
#[inline]
pub fn rdtsc() -> u64 {
    let a: u32;
    let d: u32;
    unsafe { asm!("rdtsc", out("eax") a, out("edx") d, options(att_syntax)) };
    (a as u64) | ((d as u64) << 32)
}
#[inline]
pub fn rdtscp() -> (u64, u32) {
    let a: u32;
    let d: u32;
    let c: u32;
    unsafe { asm!("rdtscp", out("eax") a, out("edx") d, out("ecx") c, options(att_syntax)) };
    ((a as u64) | ((d as u64) << 32), c)
}
