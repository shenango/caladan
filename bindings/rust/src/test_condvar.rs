use shenango::sync::{Condvar, Mutex};

const NUM: u32 = 20;

fn sender(m: Mutex<Vec<u32>>, cv: Condvar) {
    shenango::thread::spawn_detached(move || {
        for i in 0..NUM {
            shenango::sleep(std::time::Duration::from_millis(10));
            println!("pushing {:?}", i);
            m.lock().push(i);
            cv.signal();
        }
    });
}

fn main_handler() {
    println!("starting");

    let mux = Mutex::new(Vec::new());
    let cv = Condvar::new();

    sender(mux.clone(), cv.clone());

    let mut expected = 0;
    let start = shenango::microtime();
    loop {
        let mut g = cv.wait(&mux);
        match g.pop() {
            Some(i) if i == expected => {
                println!("rcvd, elapsed = {:?}", shenango::microtime() - start);
                expected += 1;
            }
            e => {
                println!("err: {:?}", e);
            }
        }

        if expected == NUM {
            break;
        }
    }

    println!("done");
}

fn main() {
    let args: Vec<_> = ::std::env::args().collect();
    assert!(args.len() >= 2, "arg must be config file");
    shenango::runtime_init(args[1].clone(), main_handler).unwrap();
}
