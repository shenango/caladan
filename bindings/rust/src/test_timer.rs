use shenango::sync::WaitGroup;
use shenango::time::Timer;
use std::time::Duration;

fn sleep(t: Timer, wg: WaitGroup) {
    wg.add(1);
    shenango::thread::spawn_detached(move || {
        let then = shenango::microtime();
        t.sleep(Duration::from_millis(100));
        println!("woke up after {:?} us", shenango::microtime() - then);
        wg.done();
    });
}

fn main_handler() {
    println!("starting");

    println!("test immediate cancel");
    let t = Timer::new();
    t.cancel();

    let wg = WaitGroup::new();
    let t = Timer::new();
    sleep(t.clone(), wg.clone());

    let start = shenango::microtime();
    Timer::new().sleep(Duration::from_millis(10));
    println!("slept for {:?} us", shenango::microtime() - start);
    t.cancel();
    wg.wait();
    println!("done after {:?} us", shenango::microtime() - start);
}

fn main() {
    let args: Vec<_> = ::std::env::args().collect();
    assert!(args.len() >= 2, "arg must be config file");
    shenango::runtime_init(args[1].clone(), main_handler).unwrap();
}
