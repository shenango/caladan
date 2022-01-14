use shenango::sync::WaitGroup;
use shenango::udp::{udp_accept, UdpConnection};
use std::net::{Ipv4Addr, SocketAddrV4};

fn looper(t: UdpConnection) {
    println!("new conn: {:?}", t.remote_addr());
    let mut buf = [0u8; 128];
    loop {
        let len = t.recv(&mut buf).unwrap();
        t.send(&buf[..len]).unwrap();
    }
}

fn client(addr: SocketAddrV4, i: u16, wg: WaitGroup) {
    println!("starting client {:?}", i,);
    let cn = UdpConnection::dial(SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, 5151 + i), addr).unwrap();
    println!("{:?} -> {:?}", cn.local_addr(), cn.remote_addr());
    let mut buf = [0u8; 8];
    for i in 0..5 {
        let snd = [i; 8];
        cn.send(&snd).unwrap();
        let l = cn.recv(&mut buf).unwrap();
        assert_eq!(&buf[..l], &snd);
    }

    println!("client {:?} done", i);
    wg.done();
}

fn main_handler() {
    shenango::thread::spawn(move || {
        println!("starting spawner");
        udp_accept(SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, 5151), looper).unwrap();
    });

    let wg = WaitGroup::new();
    for i in 1..6 {
        let wg = wg.clone();
        wg.add(1);
        shenango::sleep(std::time::Duration::from_millis(1));
        shenango::thread::spawn(move || client("10.1.1.2:5151".parse().unwrap(), i, wg));
    }

    wg.wait();
    println!("done");
}

fn main() {
    let args: Vec<_> = ::std::env::args().collect();
    assert!(args.len() >= 2, "arg must be config file");
    shenango::runtime_init(args[1].clone(), main_handler).unwrap();
}
