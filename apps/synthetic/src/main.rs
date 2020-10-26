#![feature(integer_atomics)]
#![feature(nll)]
#![feature(test)]
#[macro_use]
extern crate clap;

extern crate byteorder;
extern crate dns_parser;
extern crate itertools;
extern crate libc;
extern crate lockstep;
extern crate mersenne_twister;
extern crate net2;
extern crate rand;
extern crate shenango;
extern crate test;

use std::collections::BTreeMap;
use std::f32::INFINITY;
use std::io;
use std::io::{ErrorKind, Read, Write};
use std::net::{Ipv4Addr, SocketAddrV4};
use std::slice;
use std::str::FromStr;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use clap::{App, Arg};
use itertools::Itertools;
use mersenne_twister::MersenneTwister;
use rand::distributions::{Exp, IndependentSample};
use rand::{Rng, SeedableRng};
use shenango::udp::UdpSpawner;

mod backend;
use backend::*;

mod payload;
use payload::{Payload, SyntheticProtocol, PAYLOAD_SIZE};

#[derive(Default)]
pub struct Packet {
    work_iterations: u64,
    randomness: u64,
    target_start: Duration,
    actual_start: Option<Duration>,
    completion_time_ns: AtomicU64,
    completion_server_tsc: Option<u64>,
    completion_time: Option<Duration>,
}

mod fakework;
use fakework::FakeWorker;

mod memcached;
use memcached::MemcachedProtocol;

mod dns;
use dns::DnsProtocol;

mod reflex;
use reflex::ReflexProtocol;

#[derive(Copy, Clone, Debug)]
enum Distribution {
    Zero,
    Constant(u64),
    Exponential(f64),
    Bimodal1(f64),
    Bimodal2(f64),
    Bimodal3(f64),
}
impl Distribution {
    fn name(&self) -> &'static str {
        match *self {
            Distribution::Zero => "zero",
            Distribution::Constant(_) => "constant",
            Distribution::Exponential(_) => "exponential",
            Distribution::Bimodal1(_) => "bimodal1",
            Distribution::Bimodal2(_) => "bimodal2",
            Distribution::Bimodal3(_) => "bimodal3",
        }
    }
    fn sample<R: Rng>(&self, rng: &mut R) -> u64 {
        match *self {
            Distribution::Zero => 0,
            Distribution::Constant(m) => m,
            Distribution::Exponential(m) => Exp::new(1.0 / m).ind_sample(rng) as u64,
            Distribution::Bimodal1(m) => {
                if rng.gen_weighted_bool(10) {
                    (m * 5.5) as u64
                } else {
                    (m * 0.5) as u64
                }
            }
            Distribution::Bimodal2(m) => {
                if rng.gen_weighted_bool(1000) {
                    (m * 500.5) as u64
                } else {
                    (m * 0.5) as u64
                }
            }

            Distribution::Bimodal3(m) => {
                if rng.gen_weighted_bool(100) {
                    (m * 5.5) as u64
                } else {
                    (m * 0.5) as u64
                }
            }
        }
    }
}

arg_enum! {
#[derive(Copy, Clone)]
pub enum Transport {
    Udp,
    Tcp,
}}

trait LoadgenProtocol: Send + Sync {
    fn gen_req(&self, i: usize, p: &Packet, buf: &mut Vec<u8>);
    fn read_response(&self, sock: &Connection, scratch: &mut [u8]) -> io::Result<(usize, u64)>;
}

arg_enum! {
#[derive(Copy, Clone)]
enum OutputMode {
    Silent,
    Normal,
    Buckets,
    Trace
}}

fn duration_to_ns(duration: Duration) -> u64 {
    duration.as_secs() * 1000_000_000 + duration.subsec_nanos() as u64
}

fn run_linux_udp_server(
    backend: Backend,
    addr: SocketAddrV4,
    nthreads: usize,
    worker: Arc<FakeWorker>,
) {
    let join_handles: Vec<_> = (0..nthreads)
        .map(|_| {
            let worker = worker.clone();
            backend.spawn_thread(move || {
                let socket = backend.create_udp_connection(addr, None).unwrap();
                println!("Bound to address {}", socket.local_addr());
                let mut buf = vec![0; 4096];
                loop {
                    let (len, remote_addr) = socket.recv_from(&mut buf[..]).unwrap();
                    let payload = Payload::deserialize(&mut &buf[..len]).unwrap();
                    worker.work(payload.work_iterations, payload.randomness);
                    socket.send_to(&buf[..len], remote_addr).unwrap();
                }
            })
        })
        .collect();

    for j in join_handles {
        j.join().unwrap();
    }
}

fn socket_worker(socket: &mut Connection, worker: Arc<FakeWorker>) {
    let mut v = vec![0; PAYLOAD_SIZE];
    let mut r = || {
        socket.read_exact(&mut v[..PAYLOAD_SIZE])?;
        let mut payload = Payload::deserialize(&mut &v[..PAYLOAD_SIZE])?;
        v.clear();
        worker.work(payload.work_iterations, payload.randomness);
        payload.randomness = shenango::rdtsc();
        payload.serialize_into(&mut v)?;
        Ok(socket.write_all(&v[..])?)
    };
    loop {
        if let Err(e) = r() as io::Result<()> {
            match e.raw_os_error() {
                Some(-104) | Some(104) => break,
                _ => {}
            }
            if e.kind() != ErrorKind::UnexpectedEof {
                println!("Receive thread: {}", e);
            }
            break;
        }
    }
}

fn run_tcp_server(backend: Backend, addr: SocketAddrV4, worker: Arc<FakeWorker>) {
    let tcpq = backend.create_tcp_listener(addr).unwrap();
    println!("Bound to address {}", addr);
    loop {
        match tcpq.accept() {
            Ok(mut c) => {
                let worker = worker.clone();
                backend.spawn_thread(move || socket_worker(&mut c, worker));
            }
            Err(e) => {
                println!("Listener: {}", e);
            }
        }
    }
}

fn run_spawner_server(addr: SocketAddrV4, workerspec: &str) {
    static mut SPAWNER_WORKER: Option<FakeWorker> = None;
    unsafe {
        SPAWNER_WORKER = Some(FakeWorker::create(workerspec).unwrap());
    }
    extern "C" fn echo(d: *mut shenango::ffi::udp_spawn_data) {
        unsafe {
            let buf = slice::from_raw_parts((*d).buf as *mut u8, (*d).len);
            let payload = Payload::deserialize(&mut &buf[..]).unwrap();
            let worker = SPAWNER_WORKER.as_ref().unwrap();
            worker.work(payload.work_iterations, payload.randomness);
            let _ = UdpSpawner::reply(d, buf);
            UdpSpawner::release_data(d);
        }
    }

    let _s = unsafe { UdpSpawner::new(addr, echo).unwrap() };

    let wg = shenango::WaitGroup::new();
    wg.add(1);
    wg.wait();
}

fn run_memcached_preload(
    proto: MemcachedProtocol,
    backend: Backend,
    tport: Transport,
    addr: SocketAddrV4,
    nthreads: usize,
) -> bool {
    let perthread = (proto.nvalues as usize + nthreads - 1) / nthreads;
    let join_handles: Vec<JoinHandle<_>> = (0..nthreads)
        .map(|i| {
            let proto = proto.clone();
            backend.spawn_thread(move || {
                let sock1 = Arc::new(match tport {
                    Transport::Tcp => backend.create_tcp_connection(None, addr).unwrap(),
                    Transport::Udp => backend
                        .create_udp_connection("0.0.0.0:0".parse().unwrap(), Some(addr))
                        .unwrap(),
                });
                let socket = sock1.clone();
                backend.spawn_thread(move || {
                    backend.sleep(Duration::from_secs(520));
                    if Arc::strong_count(&socket) > 1 {
                        println!("Timing out socket");
                        socket.shutdown();
                    }
                });

                let mut vec_s: Vec<u8> = Vec::with_capacity(4096);
                let mut vec_r: Vec<u8> = vec![0; 4096];
                for n in 0..perthread {
                    vec_s.clear();
                    proto.set_request((i * perthread + n) as u64, 0, &mut vec_s);

                    if let Err(e) = (&*sock1).write_all(&vec_s[..]) {
                        println!("Preload send ({}/{}): {}", n, perthread, e);
                        return false;
                    }

                    if let Err(e) = proto.read_response(&sock1, &mut vec_r[..]) {
                        println!("preload receive ({}/{}): {}", n, perthread, e);
                        return false;
                    }
                }
                true
            })
        })
        .collect();

    return join_handles.into_iter().all(|j| j.join().unwrap());
}

#[derive(Copy, Clone)]
struct RequestSchedule {
    arrival: Distribution,
    service: Distribution,
    output: OutputMode,
    runtime: Duration,
    rps: usize,
    discard_pct: f32,
}

struct TraceResult {
    actual_start: Option<Duration>,
    target_start: Duration,
    completion_time: Option<Duration>,
    server_tsc: u64,
}

impl PartialOrd for TraceResult {
    fn partial_cmp(&self, other: &TraceResult) -> Option<std::cmp::Ordering> {
        self.server_tsc.partial_cmp(&other.server_tsc)
    }
}

impl PartialEq for TraceResult {
    fn eq(&self, other: &TraceResult) -> bool {
        self.server_tsc == other.server_tsc
    }
}

struct ScheduleResult {
    packet_count: usize,
    drop_count: usize,
    never_sent_count: usize,
    first_send: Option<Duration>,
    last_send: Option<Duration>,
    latencies: BTreeMap<u64, usize>,
    first_tsc: Option<u64>,
    trace: Option<Vec<TraceResult>>,
}

fn gen_classic_packet_schedule(
    runtime: Duration,
    packets_per_second: usize,
    output: OutputMode,
    distribution: Distribution,
    ramp_up_seconds: usize,
    nthreads: usize,
    discard_pct: f32,
) -> Vec<RequestSchedule> {
    let mut sched: Vec<RequestSchedule> = Vec::new();

    /* Ramp up in 100ms increments */
    for t in 1..(10 * ramp_up_seconds) {
        let rate = t * packets_per_second / (ramp_up_seconds * 10);

        if rate == 0 {
            continue;
        }

        sched.push(RequestSchedule {
            arrival: Distribution::Exponential((nthreads * 1000_000_000 / rate) as f64),
            service: distribution,
            output: OutputMode::Silent,
            runtime: Duration::from_millis(100),
            rps: rate,
            discard_pct: 0.0,
        });
    }

    let ns_per_packet = nthreads * 1000_000_000 / packets_per_second;

    sched.push(RequestSchedule {
        arrival: Distribution::Exponential(ns_per_packet as f64),
        service: distribution,
        output: output,
        runtime: runtime,
        rps: packets_per_second,
        discard_pct: discard_pct,
    });

    sched
}

fn gen_loadshift_experiment(
    spec: &str,
    service: Distribution,
    nthreads: usize,
    output: OutputMode,
) -> Vec<RequestSchedule> {
    spec.split(",")
        .map(|step_spec| {
            let s: Vec<&str> = step_spec.split(":").collect();
            assert!(s.len() >= 2 && s.len() <= 3);
            let packets_per_second: u64 = s[0].parse().unwrap();
            let ns_per_packet = nthreads as u64 * 1000_000_000 / packets_per_second;
            let micros = s[1].parse().unwrap();
            let output = match s.len() {
                2 => output,
                3 => OutputMode::Silent,
                _ => unreachable!(),
            };
            RequestSchedule {
                arrival: Distribution::Exponential(ns_per_packet as f64),
                service: service,
                output: output,
                runtime: Duration::from_micros(micros),
                rps: packets_per_second as usize,
                discard_pct: 0.0,
            }
        })
        .collect()
}

fn process_result_final(
    sched: &RequestSchedule,
    results: Vec<ScheduleResult>,
    wct_start: SystemTime,
    sched_start: Duration,
) -> bool {
    let mut buckets: BTreeMap<u64, usize> = BTreeMap::new();

    let packet_count = results.iter().map(|res| res.packet_count).sum::<usize>();
    let drop_count = results.iter().map(|res| res.drop_count).sum::<usize>();
    let never_sent_count = results
        .iter()
        .map(|res| res.never_sent_count)
        .sum::<usize>();
    let first_send = results.iter().filter_map(|res| res.first_send).min();
    let last_send = results.iter().filter_map(|res| res.last_send).max();
    let first_tsc = results.iter().filter_map(|res| res.first_tsc).min();
    let start_unix = wct_start + sched_start;

    if let OutputMode::Silent = sched.output {
        return true;
    }

    if packet_count <= 1 {
        println!(
            "{}, {}, 0, {}, {}, {}",
            sched.service.name(),
            sched.rps,
            drop_count,
            never_sent_count,
            start_unix.duration_since(UNIX_EPOCH).unwrap().as_secs()
        );
        return false;
    }

    results.iter().for_each(|res| {
        for (k, v) in &res.latencies {
            *buckets.entry(*k).or_insert(0) += v;
        }
    });

    let percentile = |p| {
        let idx = ((packet_count + drop_count) as f32 * p / 100.0) as usize;
        if idx >= packet_count {
            return INFINITY;
        }

        let mut seen = 0;
        for k in buckets.keys() {
            seen += buckets[k];
            if seen >= idx {
                return *k as f32;
            }
        }
        return INFINITY;
    };

    let last_send = last_send.unwrap();
    let first_send = first_send.unwrap();
    let first_tsc = first_tsc.unwrap();
    let start_unix = wct_start + first_send;

    println!(
        "{}, {}, {}, {}, {}, {:.1}, {:.1}, {:.1}, {:.1}, {:.1}, {}, {}",
        sched.service.name(),
        (packet_count + drop_count) as u64 * 1000_000_000 / duration_to_ns(last_send - first_send),
        packet_count as u64 * 1000_000_000 / duration_to_ns(last_send - first_send),
        drop_count,
        never_sent_count,
        percentile(50.0),
        percentile(90.0),
        percentile(99.0),
        percentile(99.9),
        percentile(99.99),
        start_unix.duration_since(UNIX_EPOCH).unwrap().as_secs(),
        first_tsc
    );

    if let OutputMode::Buckets = sched.output {
        print!("Latencies: ");
        for k in buckets.keys() {
            print!("{}:{} ", k, buckets[k]);
        }
        println!("");
    }

    if let OutputMode::Trace = sched.output {
        print!("Trace: ");
        for p in results.into_iter().filter_map(|p| p.trace).kmerge() {
            if let Some(completion_time) = p.completion_time {
                let actual_start = p.actual_start.unwrap();
                print!(
                    "{}:{}:{}:{} ",
                    duration_to_ns(actual_start),
                    duration_to_ns(actual_start) as i64 - duration_to_ns(p.target_start) as i64,
                    duration_to_ns(completion_time - actual_start),
                    p.server_tsc,
                )
            } else if p.actual_start.is_some() {
                let actual_start = p.actual_start.unwrap();
                print!(
                    "{}:{}:-1:-1 ",
                    duration_to_ns(actual_start),
                    duration_to_ns(actual_start) as i64 - duration_to_ns(p.target_start) as i64,
                )
            } else {
                print!("{}:-1:-1:-1 ", duration_to_ns(p.target_start))
            }
        }
        println!("");
    }

    true
}

fn process_result(sched: &RequestSchedule, packets: &mut [Packet]) -> Option<ScheduleResult> {
    if packets.len() == 0 {
        return None;
    }

    // Discard the first X% of the packets.
    let pidx = packets.len() as f32 * sched.discard_pct / 100.0;
    let packets = &mut packets[pidx as usize..];

    let mut never_sent = 0;
    let mut dropped = 0;
    let mut latencies = BTreeMap::new();
    for p in packets.iter() {
        match (p.actual_start, p.completion_time) {
            (None, _) => never_sent += 1,
            (_, None) => dropped += 1,
            (Some(ref start), Some(ref end)) => {
                *latencies
                    .entry(duration_to_ns(*end - *start) / 1000)
                    .or_insert(0) += 1
            }
        }
    }

    if packets.len() - dropped - never_sent <= 1 {
        return None;
    }

    if let OutputMode::Silent = sched.output {
        return None;
    }

    let first_send = packets.iter().filter_map(|p| p.actual_start).min();
    let last_send = packets.iter().filter_map(|p| p.actual_start).max();

    let first_tsc = packets.iter().filter_map(|p| p.completion_server_tsc).min();

    let trace = match sched.output {
        OutputMode::Trace => {
            let mut traceresults: Vec<_> = packets
                .into_iter()
                .filter_map(|p| {
                    if !p.completion_time.is_some() {
                        return None;
                    }

                    Some(TraceResult {
                        actual_start: p.actual_start,
                        target_start: p.target_start,
                        completion_time: p.completion_time,
                        server_tsc: p.completion_server_tsc.unwrap(),
                    })
                })
                .collect();
            traceresults.sort_by_key(|p| p.server_tsc);
            Some(traceresults)
        }
        _ => None,
    };

    Some(ScheduleResult {
        packet_count: packets.len() - dropped - never_sent,
        drop_count: dropped,
        never_sent_count: never_sent,
        first_send: first_send,
        last_send: last_send,
        latencies: latencies,
        first_tsc: first_tsc,
        trace: trace,
    })
}

fn run_client_worker(
    proto: Arc<Box<dyn LoadgenProtocol>>,
    backend: Backend,
    addr: SocketAddrV4,
    tport: Transport,
    wg: shenango::WaitGroup,
    wg_start: shenango::WaitGroup,
    schedules: Arc<Vec<RequestSchedule>>,
    index: usize,
) -> Vec<Option<ScheduleResult>> {
    let mut packets: Vec<Packet> = Vec::new();
    let mut rng: MersenneTwister = SeedableRng::from_seed(rand::thread_rng().gen::<u64>());
    let mut payload = Vec::with_capacity(4096);

    let mut sched_boundaries = Vec::new();

    let mut last = 100_000_000;
    let mut end = 100_000_000;
    for sched in schedules.iter() {
        end += duration_to_ns(sched.runtime);
        loop {
            let nxt = last + sched.arrival.sample(&mut rng);
            if nxt >= end {
                break;
            }
            last = nxt;
            packets.push(Packet {
                randomness: rng.gen::<u64>(),
                target_start: Duration::from_nanos(last),
                work_iterations: sched.service.sample(&mut rng),
                ..Default::default()
            });
        }
        sched_boundaries.push(packets.len());
    }

    let src_addr = SocketAddrV4::new(Ipv4Addr::new(0, 0, 0, 0), (100 + index) as u16);
    let socket = Arc::new(match tport {
        Transport::Tcp => backend.create_tcp_connection(Some(src_addr), addr).unwrap(),
        Transport::Udp => backend.create_udp_connection(src_addr, Some(addr)).unwrap(),
    });

    let packets_per_thread = packets.len();
    let socket2 = socket.clone();
    let rproto = proto.clone();
    let wg2 = wg.clone();
    let receive_thread = backend.spawn_thread(move || {
        let mut recv_buf = vec![0; 4096];
        let mut receive_times = vec![None; packets_per_thread];
        wg2.done();
        for _ in 0..receive_times.len() {
            match rproto.read_response(&socket2, &mut recv_buf[..]) {
                Ok((idx, tsc)) => receive_times[idx] = Some((Instant::now(), tsc)),
                Err(e) => {
                    match e.raw_os_error() {
                        Some(-103) | Some(-104) => break,
                        _ => (),
                    }
                    if e.kind() != ErrorKind::UnexpectedEof {
                        println!("Receive thread: {}", e);
                    }
                    break;
                }
            }
        }
        receive_times
    });

    // If the send or receive thread is still running 500 ms after it should have finished,
    // then stop it by triggering a shutdown on the socket.
    let last = packets[packets.len() - 1].target_start;
    let socket2 = socket.clone();
    let wg2 = wg.clone();
    let wg3 = wg_start.clone();
    let timer = backend.spawn_thread(move || {
        wg2.done();
        wg3.wait();
        backend.sleep(last + Duration::from_millis(500));
        if Arc::strong_count(&socket2) > 1 {
            socket2.shutdown();
        }
    });

    wg.done();
    wg_start.wait();
    let start = Instant::now();

    for (i, packet) in packets.iter_mut().enumerate() {
        payload.clear();
        proto.gen_req(i, packet, &mut payload);

        let mut t = start.elapsed();
        while t + Duration::from_micros(1) < packet.target_start {
            backend.sleep(packet.target_start - t);
            t = start.elapsed();
        }
        if t > packet.target_start + Duration::from_micros(5) {
            continue;
        }

        packet.actual_start = Some(start.elapsed());
        if let Err(e) = (&*socket).write_all(&payload[..]) {
            packet.actual_start = None;
            match e.raw_os_error() {
                Some(-32) | Some(-103) | Some(-104) => {}
                _ => println!("Send thread ({}/{}): {}", i, packets.len(), e),
            }
            break;
        }
    }

    wg.done();
    wg_start.wait();

    timer.join().unwrap();
    receive_thread
        .join()
        .unwrap()
        .into_iter()
        .zip(packets.iter_mut())
        .for_each(|(c, p)| {
            if let Some((inst, tsc)) = c {
                (*p).completion_time = Some(inst - start);
                (*p).completion_server_tsc = Some(tsc);
            }
        });

    let mut start_index = 0;
    schedules
        .iter()
        .zip(sched_boundaries)
        .map(|(sched, end)| {
            let res = process_result(&sched, &mut packets[start_index..end]);
            start_index = end;
            res
        })
        .collect::<Vec<Option<ScheduleResult>>>()
}

fn run_client(
    proto: Arc<Box<dyn LoadgenProtocol>>,
    backend: Backend,
    addr: SocketAddrV4,
    nthreads: usize,
    tport: Transport,
    barrier_group: &mut Option<lockstep::Group>,
    schedules: Vec<RequestSchedule>,
    index: usize,
) -> bool {
    let schedules = Arc::new(schedules);
    let wg = shenango::WaitGroup::new();
    wg.add(3 * nthreads as i32);
    let wg_start = shenango::WaitGroup::new();
    wg_start.add(1 as i32);

    let conn_threads: Vec<_> = (0..nthreads)
        .into_iter()
        .map(|i| {
            let client_idx = 100 + (index * nthreads) + i;
            let proto = proto.clone();
            let wg = wg.clone();
            let wg_start = wg_start.clone();
            let schedules = schedules.clone();

            backend.spawn_thread(move || {
                run_client_worker(
                    proto, backend, addr, tport, wg, wg_start, schedules, client_idx,
                )
            })
        })
        .collect();

    backend.sleep(Duration::from_secs(10));
    wg.wait();

    if let Some(ref mut g) = *barrier_group {
        g.barrier();
    }

    wg_start.done();
    let start_unix = SystemTime::now();

    wg.add(nthreads as i32);
    wg_start.add(1 as i32);

    wg.wait();
    wg_start.done();

    let mut packets: Vec<Vec<Option<ScheduleResult>>> = conn_threads
        .into_iter()
        .map(|s| s.join().unwrap())
        .collect();

    let mut sched_start = Duration::from_nanos(100_000_000);
    schedules
        .iter()
        .enumerate()
        .map(|(i, sched)| {
            let perthread = packets.iter_mut().filter_map(|p| p[i].take()).collect();
            let r = process_result_final(sched, perthread, start_unix, sched_start);
            sched_start += sched.runtime;
            r
        })
        .collect::<Vec<bool>>()
        .into_iter()
        .all(|p| p)
}

fn run_local(
    backend: Backend,
    nthreads: usize,
    worker: Arc<FakeWorker>,
    schedules: Vec<RequestSchedule>,
) -> bool {
    let mut rng = rand::thread_rng();
    let schedules = Arc::new(schedules);

    let packet_schedules: Vec<Vec<Packet>> = (0..nthreads)
        .map(|_| {
            let mut last = 100_000_000;
            let mut thread_packets: Vec<Packet> = Vec::new();
            for sched in schedules.iter() {
                let end = last + duration_to_ns(sched.runtime);
                while last < end {
                    last += sched.arrival.sample(&mut rng);
                    thread_packets.push(Packet {
                        randomness: rng.gen::<u64>(),
                        target_start: Duration::from_nanos(last),
                        work_iterations: sched.service.sample(&mut rng),
                        ..Default::default()
                    });
                }
            }
            thread_packets
        })
        .collect();

    let start_unix = SystemTime::now();
    let start = Instant::now();

    struct AtomicU64Pointer(*const AtomicU64);
    unsafe impl Send for AtomicU64Pointer {}

    let mut send_threads = Vec::new();
    for mut packets in packet_schedules {
        let worker = worker.clone();
        let schedules = schedules.clone();
        send_threads.push(backend.spawn_thread(move || {
            let remaining = Arc::new(AtomicUsize::new(packets.len()));
            for i in 0..packets.len() {
                let (work_iterations, completion_time_ns, rnd) = {
                    let packet = &mut packets[i];

                    let mut t = start.elapsed();
                    while t < packet.target_start {
                        t = start.elapsed();
                    }

                    packet.actual_start = Some(start.elapsed());
                    (
                        packet.work_iterations,
                        AtomicU64Pointer(&packet.completion_time_ns as *const AtomicU64),
                        packet.randomness,
                    )
                };

                let remaining = remaining.clone();
                let worker = worker.clone();
                backend.spawn_thread(move || {
                    worker.work(work_iterations, rnd);
                    unsafe {
                        (*completion_time_ns.0)
                            .store(start.elapsed().as_nanos() as u64, Ordering::SeqCst);
                    }
                    remaining.fetch_sub(1, Ordering::SeqCst);
                });
            }

            while remaining.load(Ordering::SeqCst) > 0 {
                // do nothing
            }

            for p in packets.iter_mut() {
                p.completion_time = Some(Duration::from_nanos(
                    p.completion_time_ns.load(Ordering::SeqCst),
                ));
            }

            let mut start = Duration::from_nanos(100_000_000);
            let mut start_index = 0;

            schedules
                .iter()
                .map(|sched| {
                    let npackets = packets[start_index..]
                        .iter()
                        .position(|p| p.target_start >= start + sched.runtime)
                        .unwrap_or(packets.len() - start_index - 1)
                        + 1;
                    let res =
                        process_result(&sched, &mut packets[start_index..start_index + npackets]);
                    start = packets[start_index + npackets - 1].target_start;
                    start_index += npackets;
                    res
                })
                .collect::<Vec<Option<ScheduleResult>>>()
        }))
    }

    let mut packets: Vec<Vec<Option<ScheduleResult>>> = send_threads
        .into_iter()
        .map(|s| s.join().unwrap())
        .collect();

    let mut sched_start = Duration::from_nanos(100_000_000);
    schedules
        .iter()
        .enumerate()
        .map(|(i, sched)| {
            let perthread = packets.iter_mut().filter_map(|p| p[i].take()).collect();
            let r = process_result_final(sched, perthread, start_unix, sched_start);
            sched_start += sched.runtime;
            r
        })
        .collect::<Vec<bool>>()
        .into_iter()
        .all(|p| p)
}

fn main() {
    let matches = App::new("Synthetic Workload Application")
        .version("0.1")
        .arg(
            Arg::with_name("ADDR")
                .index(1)
                .help("Address and port to listen on")
                .required(true),
        )
        .arg(
            Arg::with_name("threads")
                .short("t")
                .long("threads")
                .value_name("T")
                .default_value("1")
                .help("Number of client threads"),
        )
        .arg(
            Arg::with_name("discard_pct")
                .long("discard_pct")
                .default_value("10")
                .help("Discard first % of packtets at target QPS from sample"),
        )
        .arg(
            Arg::with_name("mode")
                .short("m")
                .long("mode")
                .value_name("MODE")
                .possible_values(&[
                    "linux-server",
                    "linux-client",
                    "runtime-client",
                    "spawner-server",
                    "local-client",
                ])
                .required(true)
                .requires_ifs(&[("runtime-client", "config"), ("spawner-server", "config")])
                .help("Which mode to run in"),
        )
        .arg(
            Arg::with_name("runtime")
                .short("r")
                .long("runtime")
                .takes_value(true)
                .default_value("10")
                .help("How long the application should run for"),
        )
        .arg(
            Arg::with_name("mpps")
                .long("mpps")
                .takes_value(true)
                .default_value("0.02")
                .help("How many *million* packets should be sent per second"),
        )
        .arg(
            Arg::with_name("start_mpps")
                .long("start_mpps")
                .takes_value(true)
                .default_value("0.0")
                .help("Initial rate to sample at"),
        )
        .arg(
            Arg::with_name("config")
                .short("c")
                .long("config")
                .takes_value(true),
        )
        .arg(
            Arg::with_name("protocol")
                .short("p")
                .long("protocol")
                .value_name("PROTOCOL")
                .possible_values(&["synthetic", "memcached", "dns", "reflex"])
                .default_value("synthetic")
                .help("Server protocol"),
        )
        .arg(
            Arg::with_name("warmup")
                .long("warmup")
                .takes_value(false)
                .help("Run the warmup routine"),
        )
        .arg(
            Arg::with_name("calibrate")
                .long("calibrate")
                .takes_value(true)
                .help("us to calibrate fake work for"),
        )
        .arg(
            Arg::with_name("output")
                .short("o")
                .long("output")
                .value_name("output mode")
                .possible_values(&["silent", "normal", "buckets", "trace"])
                .default_value("normal")
                .help("How to display loadgen results"),
        )
        .arg(
            Arg::with_name("distribution")
                .long("distribution")
                .short("d")
                .takes_value(true)
                .possible_values(&[
                    "zero",
                    "constant",
                    "exponential",
                    "bimodal1",
                    "bimodal2",
                    "bimodal3",
                ])
                .default_value("zero")
                .help("Distribution of request lengths to use"),
        )
        .arg(
            Arg::with_name("mean")
                .long("mean")
                .takes_value(true)
                .default_value("167")
                .help("Mean number of work iterations per request"),
        )
        .arg(
            Arg::with_name("barrier-peers")
                .long("barrier-peers")
                .requires("barrier-leader")
                .takes_value(true)
                .help("Number of peers in barrier group"),
        )
        .arg(
            Arg::with_name("barrier-leader")
                .long("barrier-leader")
                .requires("barrier-peers")
                .takes_value(true)
                .help("Leader of barrier group"),
        )
        .arg(
            Arg::with_name("samples")
                .long("samples")
                .takes_value(true)
                .default_value("20")
                .help("Number of samples to collect"),
        )
        .arg(
            Arg::with_name("fakework")
                .long("fakework")
                .takes_value(true)
                .default_value("stridedmem:1024:7")
                .help("fake worker spec"),
        )
        .arg(
            Arg::with_name("transport")
                .long("transport")
                .takes_value(true)
                .default_value("udp")
                .help("udp or tcp"),
        )
        .arg(
            Arg::with_name("rampup")
                .long("rampup")
                .takes_value(true)
                .default_value("4")
                .help("per-sample ramp up seconds"),
        )
        .arg(
            Arg::with_name("loadshift")
                .long("loadshift")
                .takes_value(true)
                .default_value("")
                .help("loadshift spec"),
        )
        .args(&SyntheticProtocol::args())
        .args(&MemcachedProtocol::args())
        .args(&DnsProtocol::args())
        .args(&ReflexProtocol::args())
        .get_matches();

    let addr: SocketAddrV4 = FromStr::from_str(matches.value_of("ADDR").unwrap()).unwrap();
    let nthreads = value_t_or_exit!(matches, "threads", usize);
    let discard_pct = value_t_or_exit!(matches, "discard_pct", f32);

    let runtime = Duration::from_secs(value_t!(matches, "runtime", u64).unwrap());
    let packets_per_second = (1.0e6 * value_t_or_exit!(matches, "mpps", f32)) as usize;
    let start_packets_per_second = (1.0e6 * value_t_or_exit!(matches, "start_mpps", f32)) as usize;
    assert!(start_packets_per_second <= packets_per_second);
    let config = matches.value_of("config");
    let dowarmup = matches.is_present("warmup");

    let tport = value_t_or_exit!(matches, "transport", Transport);
    let proto: Arc<Box<dyn LoadgenProtocol>> = match matches.value_of("protocol").unwrap() {
        "synthetic" => Arc::new(Box::new(SyntheticProtocol::with_args(&matches, tport))),
        "memcached" => Arc::new(Box::new(MemcachedProtocol::with_args(&matches, tport))),
        "dns" => Arc::new(Box::new(DnsProtocol::with_args(&matches, tport))),
        "reflex" => Arc::new(Box::new(ReflexProtocol::with_args(&matches, tport))),
        _ => unreachable!(),
    };

    let output = value_t_or_exit!(matches, "output", OutputMode);
    let mean = value_t_or_exit!(matches, "mean", f64);
    let distribution = match matches.value_of("distribution").unwrap() {
        "zero" => Distribution::Zero,
        "constant" => Distribution::Constant(mean as u64),
        "exponential" => Distribution::Exponential(mean),
        "bimodal1" => Distribution::Bimodal1(mean),
        "bimodal2" => Distribution::Bimodal2(mean),
        "bimodal3" => Distribution::Bimodal3(mean),
        _ => unreachable!(),
    };
    let samples = value_t_or_exit!(matches, "samples", usize);
    let rampup = value_t_or_exit!(matches, "rampup", usize);
    let mode = matches.value_of("mode").unwrap();
    let backend = match mode {
        "linux-server" | "linux-client" => Backend::Linux,
        "spawner-server" | "runtime-client" | "local-client" => Backend::Runtime,
        _ => unreachable!(),
    };

    let loadshift_spec = value_t_or_exit!(matches, "loadshift", String);
    let fwspec = value_t_or_exit!(matches, "fakework", String);
    let fakeworker = Arc::new(FakeWorker::create(&fwspec).unwrap());

    if matches.is_present("calibrate") {
        let us = value_t_or_exit!(matches, "calibrate", u64);
        backend.init_and_run(config, move || {
            let barrier = Arc::new(AtomicUsize::new(nthreads));
            let join_handles: Vec<_> = (0..nthreads)
                .map(|_| {
                    let fakeworker = fakeworker.clone();
                    let barrier = barrier.clone();
                    backend.spawn_thread(move || {
                        barrier.fetch_sub(1, Ordering::SeqCst);
                        while barrier.load(Ordering::SeqCst) > 0 {}
                        fakeworker.calibrate(us);
                    })
                })
                .collect();
            for j in join_handles {
                j.join().unwrap();
            }
        });
        return;
    }

    match mode {
        "spawner-server" => match tport {
            Transport::Udp => {
                backend.init_and_run(config, move || run_spawner_server(addr, &fwspec))
            }
            Transport::Tcp => {
                backend.init_and_run(config, move || run_tcp_server(backend, addr, fakeworker))
            }
        },
        "linux-server" => match tport {
            Transport::Udp => backend.init_and_run(config, move || {
                run_linux_udp_server(backend, addr, nthreads, fakeworker)
            }),
            Transport::Tcp => {
                backend.init_and_run(config, move || run_tcp_server(backend, addr, fakeworker))
            }
        },
        "local-client" => {
            backend.init_and_run(config, move || {
                println!("Distribution, Target, Actual, Dropped, Never Sent, Median, 90th, 99th, 99.9th, 99.99th, Start");
                if dowarmup {
                    for packets_per_second in (1..3).map(|i| i * 100000) {
                        let sched = gen_classic_packet_schedule(
                            Duration::from_secs(1),
                            packets_per_second,
                            OutputMode::Silent,
                            distribution,
                            0,
                            nthreads,
                            discard_pct,
                        );
                        run_local(
                            backend,
                            nthreads,
                            fakeworker.clone(),
                            sched,
                        );
                    }
                }
                let step_size = (packets_per_second - start_packets_per_second) / samples;
                for j in 1..=samples {
                    let sched = gen_classic_packet_schedule(
                        runtime,
                        start_packets_per_second + step_size * j,
                        output,
                        distribution,
                        0,
                        nthreads,
                        discard_pct,
                    );
                    run_local(
                        backend,
                        nthreads,
                        fakeworker.clone(),
                        sched,
                    );
                    backend.sleep(Duration::from_secs(3));
                }
            });
        }
        "linux-client" | "runtime-client" => {
            let matches = matches.clone();
            backend.init_and_run(config, move || {

                let mut barrier_group = matches.value_of("barrier-leader").map(|leader| {
                    lockstep::Group::from_hostname(
                        leader,
                        23232,
                        value_t_or_exit!(matches, "barrier-peers", usize),
                    )
                    .unwrap()
                });

                println!("Distribution, Target, Actual, Dropped, Never Sent, Median, 90th, 99th, 99.9th, 99.99th, Start");
                match (matches.value_of("protocol").unwrap(), &barrier_group) {
                    (_, Some(lockstep::Group::Client(ref _c))) => (),
                    ("memcached", _) => {
                        let proto = MemcachedProtocol::with_args(&matches, Transport::Tcp);
                        if !run_memcached_preload(proto, backend, Transport::Tcp, addr, nthreads) {
                            panic!("Could not preload memcached");
                        }
                    },
                    _ => (),
                };

                if !loadshift_spec.is_empty() {
                    let sched = gen_loadshift_experiment(&loadshift_spec, distribution, nthreads, output);
                    run_client(
                        proto,
                        backend,
                        addr,
                        nthreads,
                        tport,
                        &mut barrier_group,
                        sched,
                        0,
                    );
                    if let Some(ref mut g) = barrier_group {
                        g.barrier();
                    }
                    return;
                }

                if dowarmup {
                    // Run at full pps 3 times for 20 seconds
                    for _ in 0..1 {
                    let sched = gen_classic_packet_schedule(
                        Duration::from_secs(2),
                        packets_per_second,
                        OutputMode::Silent,
                        distribution,
                        20,
                        nthreads,
                        discard_pct,
                    );
                        run_client(
                            proto.clone(),
                            backend,
                            addr,
                            nthreads,
                            tport,
                            &mut barrier_group,
                            sched,
                            0,
                        );
                    }
                }

                let step_size = (packets_per_second - start_packets_per_second) / samples;
                for j in 1..=samples {
                    let sched = gen_classic_packet_schedule(
                        runtime,
                        start_packets_per_second + step_size * j,
                        output,
                        distribution,
                        rampup,
                        nthreads,
                        discard_pct
                    );
                    if !run_client(
                        proto.clone(),
                        backend,
                        addr,
                        nthreads,
                        tport,
                        &mut barrier_group,
                        sched,
                        j,
                    ) { break; }
                }
                if let Some(ref mut g) = barrier_group {
                    g.barrier();
                }
            });
        }
        _ => unreachable!(),
    };
}
