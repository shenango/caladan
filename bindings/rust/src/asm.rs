#[inline]
pub fn cpu_relax() {
    unsafe { llvm_asm!("pause" :::: "volatile") }
}
#[inline]
pub fn cpu_serialize() {
    unsafe { llvm_asm!("cpuid" : : : "rax", "rbx", "rcx", "rdx": "volatile") }
}
#[inline]
pub fn rdtsc() -> u64 {
    let a: u32;
    let d: u32;
    unsafe { llvm_asm!("rdtsc" : "={eax}"(a), "={edx}"(d) : : : "volatile" ) };
    (a as u64) | ((d as u64) << 32)
}
#[inline]
pub fn rdtscp() -> (u64, u32) {
    let a: u32;
    let d: u32;
    let c: u32;
    unsafe { llvm_asm!("rdtscp" : "={eax}"(a), "={edx}"(d), "={ecx}"(c) : : : "volatile") };

    ((a as u64) | ((d as u64) << 32), c)
}
