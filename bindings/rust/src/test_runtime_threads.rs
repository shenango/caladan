extern crate shenango;

use shenango::WaitGroup;
use std::sync::Arc;

const N: usize = 1000000;
const NCORES: usize = 4;

fn leaf_handler(wg_parent: Arc<WaitGroup>) {
    shenango::delay_us(1);
    wg_parent.done();
}

fn work_handler(wg_parent: Arc<WaitGroup>) {
    let wg = Arc::new(WaitGroup::new());
    wg.add(N as i32);
    for _ in 0..N {
        let wg2 = wg.clone();
        shenango::thread::spawn_detached(move || leaf_handler(wg2));
        shenango::thread::thread_yield();
    }

    wg.wait();
    wg_parent.done();
}

fn main_handler() {
    println!("started main_handler() thread");
    println!("creating threads with 1us of fake work.");

    let wg = Arc::new(WaitGroup::new());
    wg.add(NCORES as i32);

    let start_us = shenango::microtime();
    for _ in 0..NCORES {
        let wg2 = wg.clone();
        shenango::thread::spawn_detached(move || work_handler(wg2));
    }

    wg.wait();

    let threads_per_second =
        (NCORES * N) as f64 / ((shenango::microtime() - start_us) as f64 * 0.000001);
    println!(
        "spawned {} threads / second, efficiency {}",
        threads_per_second,
        threads_per_second / (NCORES * N) as f64
    );
}

fn main() {
    let args: Vec<_> = ::std::env::args().collect();
    assert!(args.len() >= 2, "arg must be config file");
    shenango::runtime_init(args[1].clone(), main_handler).unwrap();
}
