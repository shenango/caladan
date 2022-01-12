use shenango::poll::{PollTrigger, PollWaiter};

fn short(t: PollTrigger) {
    shenango::thread::spawn_detached(move || {
        let _t = t;
        shenango::sleep(std::time::Duration::from_millis(10));
    });
}

fn long(t: PollTrigger) {
    shenango::thread::spawn_detached(move || {
        let _t = t;
        shenango::sleep(std::time::Duration::from_millis(50));
    });
}

fn main_handler() {
    println!("making poller");

    let mut poll = PollWaiter::new();
    let short_trigger = poll.trigger(17);
    let long_trigger = poll.trigger(42);

    short(short_trigger);
    long(long_trigger);

    let mut short_count = 0;
    let mut long_count = 0;
    let start = shenango::microtime();
    while shenango::microtime() - start < 1_000_000 {
        match poll.wait() {
            17 => {
                short_count += 1;
                let short_trigger = poll.trigger(17);
                short(short_trigger);
            }
            42 => {
                long_count += 1;
                let long_trigger = poll.trigger(42);
                long(long_trigger);
            }
            _ => unreachable!(),
        }
    }

    assert!(short_count >= (long_count * 5 - 2));
    println!("short {:?} long {:?} done", short_count, long_count);
}

fn main() {
    let args: Vec<_> = ::std::env::args().collect();
    assert!(args.len() >= 2, "arg must be config file");
    shenango::runtime_init(args[1].clone(), main_handler).unwrap();
}
