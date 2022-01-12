use shenango::sync::Mutex;

fn mutex_one(m: Mutex<u32>, start: u64) {
    shenango::thread::spawn_detached(move || {
        println!("{:?} lock 1", shenango::microtime() - start);
        let mut g = m.lock();
        println!("{:?} locked 1", shenango::microtime() - start);
        *g = 1;
        shenango::sleep(std::time::Duration::from_millis(100));
        println!("{:?} 1 done", shenango::microtime() - start);
    });
}

fn mutex_two(m: Mutex<u32>, start: u64) {
    shenango::thread::spawn_detached(move || {
        shenango::sleep(std::time::Duration::from_millis(50));
        println!("{:?} lock 2", shenango::microtime() - start);
        let mut g = m.lock();
        println!("{:?} locked 2", shenango::microtime() - start);
        *g = 2;
        shenango::sleep(std::time::Duration::from_millis(100));
        println!("{:?} 2 done", shenango::microtime() - start);
    });
}

fn main_handler() {
    let start = shenango::microtime();
    println!("making mutex");

    //let mux = Arc::new(Mutex::new());
    let mux = Mutex::new(0);

    println!("start");
    mutex_one(mux.clone(), start);
    mutex_two(mux.clone(), start);

    shenango::sleep(std::time::Duration::from_millis(10));
    assert!(mux.try_lock().is_err());
    println!("{:?} try lock ok", shenango::microtime() - start);

    shenango::sleep(std::time::Duration::from_millis(100));
    println!("{:?} try read", shenango::microtime() - start);
    let v = { *mux.lock() };
    println!("{:?} read {:?}", shenango::microtime() - start, v);
    assert_eq!(v, 2);
    println!("done");
}

fn main() {
    let args: Vec<_> = ::std::env::args().collect();
    assert!(args.len() >= 2, "arg must be config file");
    shenango::runtime_init(args[1].clone(), main_handler).unwrap();
}
