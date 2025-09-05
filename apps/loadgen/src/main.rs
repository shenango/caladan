#![feature(test)]
extern crate arrayvec;
extern crate byteorder;
extern crate dns_parser;
extern crate itertools;
extern crate libc;
extern crate net2;
extern crate rand;
extern crate rand_distr;
extern crate rand_mt;
extern crate shenango;
extern crate test;

use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, HashMap};
use std::f32::INFINITY;
use std::io;
use std::io::{Error, ErrorKind, Read, Write};
use std::net::{Ipv4Addr, SocketAddrV4};
use std::str::FromStr;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::Arc;
use std::sync::Once;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use clap::{Arg, ArgMatches, Command};
use itertools::Itertools;
use rand::Rng;
use rand_mt::Mt64;

mod backend;
use backend::*;

mod server;

mod lockstep;

mod payload;
use payload::{Payload, SyntheticProtocol, PAYLOAD_SIZE};

const KBUFSIZE: usize = 16384;

// Constants to replace magic numbers
const DEFAULT_RUNTIME_SECONDS: u64 = 10;
const DEFAULT_MPPS: f32 = 0.02;
const DEFAULT_START_MPPS: f32 = 0.0;
const DEFAULT_MEAN_WORK_ITERATIONS: f64 = 167.0;
const DEFAULT_SAMPLES: usize = 20;
const DEFAULT_RAMPUP_SECONDS: usize = 4;
const DEFAULT_DEPTH: usize = 1;
const DEFAULT_NCONNS: usize = 10;
const DEFAULT_INTERSAMPLE_SLEEP_SECONDS: u64 = 0;
const DEFAULT_DISCARD_PCT: f32 = 10.0;
const DEFAULT_FAKEWORK_SPEC: &str = "stridedmem:1024:7";
const DEFAULT_TRANSPORT: &str = "tcp";
const DEFAULT_DISTRIBUTION: &str = "zero";
const DEFAULT_OUTPUT_MODE: &str = "normal";
const DEFAULT_PROTOCOL: &str = "synthetic";
const BARRIER_PORT: u16 = 23232;
const RUNTIME_STAT_PORT: u16 = 40;
const WARMUP_DURATION_SECONDS: usize = 20;
const RAMPUP_STEPS_PER_SECOND: usize = 10;
const SOCKET_TIMEOUT_SECONDS: u64 = 520;
const WARMUP_RAMPUP_SECONDS: usize = 0;
const NS_PER_SEC: u64 = 1_000_000_000;
const DEFAULT_SCHED_START_NS: u64 = 100_000_000;
const CLIENT_WAIT_SECONDS: u64 = 1;
const MAX_CATCH_UP_TIME_MICROS: Duration = Duration::from_micros(5);
const MIN_TIMER_SLEEP_MICROS: Duration = Duration::from_micros(5);
const SCHEDULE_ADHERANCE_THRESH_LOW: f64 = 99.9;
const SCHEDULE_ADHERANCE_THRESH_HIGH: f64 = 95.0;

#[derive(Default)]
pub struct Packet {
    work_iterations: u64,
    randomness: u64,
    target_start: Duration,
    actual_start: Option<Duration>,
    completion_time_ns: Arc<AtomicU64>,
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

mod http;
use http::HttpProtocol;

mod distribution;
use distribution::Distribution;

#[derive(Clone, Copy, Serialize, Deserialize)]
pub enum Protocol {
    Synthetic(SyntheticProtocol),
    Memcached(MemcachedProtocol),
    Dns(DnsProtocol),
    Reflex(ReflexProtocol),
    Http(HttpProtocol),
}

#[derive(Copy, Clone, Serialize, Deserialize)]
pub enum Transport {
    Udp,
    Tcp,
}

impl Transport {
    pub fn dial(
        &self,
        backend: Backend,
        src_addr: Option<SocketAddrV4>,
        addr: SocketAddrV4,
    ) -> io::Result<Arc<Connection>> {
        Ok(Arc::new(match self {
            Transport::Tcp => backend.create_tcp_connection(src_addr, addr)?,
            Transport::Udp => backend.create_udp_connection(src_addr, Some(addr))?,
        }))
    }
}

impl FromStr for Transport {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "udp" => Ok(Transport::Udp),
            "tcp" => Ok(Transport::Tcp),
            _ => Err(format!("Unknown transport: {}", s)),
        }
    }
}

pub struct Buffer<'a> {
    buf: &'a mut [u8],
    head: usize,
    tail: usize,
}

impl<'a> Buffer<'a> {
    pub fn new(inbuf: &mut [u8]) -> Buffer {
        Buffer {
            buf: inbuf,
            head: 0,
            tail: 0,
        }
    }

    pub fn data_size(&self) -> usize {
        self.head - self.tail
    }

    pub fn get_data(&self) -> &[u8] {
        &self.buf[self.tail..self.head]
    }

    pub fn push_data(&mut self, size: usize) {
        self.head += size;
        assert!(self.head <= self.buf.len());
    }

    pub fn pull_data(&mut self, size: usize) {
        assert!(size <= self.data_size());
        self.tail += size;
    }

    pub fn get_free_space(&self) -> usize {
        self.buf.len() - self.head
    }

    pub fn get_empty_buf(&mut self) -> &mut [u8] {
        &mut self.buf[self.head..]
    }

    pub fn try_shrink(&mut self) -> io::Result<()> {
        if self.data_size() == 0 {
            self.head = 0;
            self.tail = 0;
            return Ok(());
        }

        if self.head < self.buf.len() {
            return Ok(());
        }

        if self.data_size() == self.buf.len() {
            return Err(Error::new(ErrorKind::Other, "Need larger buffers"));
        }

        self.buf.as_mut().copy_within(self.tail..self.head, 0);
        self.head = self.data_size();
        self.tail = 0;
        Ok(())
    }
}

trait LoadgenProtocol: Send + Sync {
    fn gen_req(&self, i: usize, p: &Packet, buf: &mut Vec<u8>);
    fn uses_ordered_requests(&self) -> bool;
    fn read_response(&self, sock: &Connection, scratch: &mut Buffer) -> io::Result<(usize, u64)>;
}

impl LoadgenProtocol for Protocol {
    fn gen_req(&self, i: usize, p: &Packet, buf: &mut Vec<u8>) {
        match self {
            Protocol::Synthetic(proto) => proto.gen_req(i, p, buf),
            Protocol::Memcached(proto) => proto.gen_req(i, p, buf),
            Protocol::Dns(proto) => proto.gen_req(i, p, buf),
            Protocol::Reflex(proto) => proto.gen_req(i, p, buf),
            Protocol::Http(proto) => proto.gen_req(i, p, buf),
        }
    }

    fn uses_ordered_requests(&self) -> bool {
        match self {
            Protocol::Synthetic(proto) => proto.uses_ordered_requests(),
            Protocol::Memcached(proto) => proto.uses_ordered_requests(),
            Protocol::Dns(proto) => proto.uses_ordered_requests(),
            Protocol::Reflex(proto) => proto.uses_ordered_requests(),
            Protocol::Http(proto) => proto.uses_ordered_requests(),
        }
    }

    fn read_response(&self, sock: &Connection, scratch: &mut Buffer) -> io::Result<(usize, u64)> {
        match self {
            Protocol::Synthetic(proto) => proto.read_response(sock, scratch),
            Protocol::Memcached(proto) => proto.read_response(sock, scratch),
            Protocol::Dns(proto) => proto.read_response(sock, scratch),
            Protocol::Reflex(proto) => proto.read_response(sock, scratch),
            Protocol::Http(proto) => proto.read_response(sock, scratch),
        }
    }
}

#[derive(Copy, Clone, Serialize, Deserialize)]
pub enum OutputMode {
    Silent,
    Normal,
    Buckets,
    Trace,
    Live,
}

impl FromStr for OutputMode {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "silent" => Ok(OutputMode::Silent),
            "normal" => Ok(OutputMode::Normal),
            "buckets" => Ok(OutputMode::Buckets),
            "trace" => Ok(OutputMode::Trace),
            "live" => Ok(OutputMode::Live),
            _ => Err(format!("Unknown output mode: {}", s)),
        }
    }
}

fn duration_to_ns(duration: Duration) -> u64 {
    duration.as_nanos() as u64
}

#[derive(Clone, Serialize, Deserialize)]
pub struct AppConfig {
    pub addrs: Vec<SocketAddrV4>,
    pub nthreads: usize,
    pub runtime: Duration,
    pub packets_per_second: usize,
    pub start_packets_per_second: usize,
    pub distribution: Distribution,
    pub transport: Transport,
    pub protocol: Protocol,
    pub output_mode: OutputMode,
    pub samples: usize,
    pub rampup: usize,
    pub discard_pct: f32,
    pub fakework_spec: String,
    pub intersample_sleep: u64,
    pub do_runtime_stat: bool,
    pub live_mode: bool,
    pub closed_bench: bool,
    pub depth: usize,
    pub do_warmup: bool,
    pub loadshift_spec: String,
    pub mode: RunMode,
    pub backend: Backend,
    pub calibrate: Option<u64>,
    pub world_size: usize,
    pub rank: usize,
}

impl AppConfig {
    fn fakeworker(&self) -> Arc<FakeWorker> {
        Arc::new(FakeWorker::create(&self.fakework_spec).unwrap())
    }

    fn split(&self) -> Vec<AppConfig> {
        assert!(self.rank == 0);
        (0..self.world_size)
            .map(|i| AppConfig {
                addrs: self.addrs.clone(),
                nthreads: self.nthreads.div_ceil(self.world_size),
                packets_per_second: self.packets_per_second.div_ceil(self.world_size),
                start_packets_per_second: self.start_packets_per_second.div_ceil(self.world_size),
                rank: i,
                do_runtime_stat: self.do_runtime_stat && i == 0,
                ..self.clone()
            })
            .collect()
    }
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

#[derive(Debug, Serialize, Deserialize)]
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

#[derive(Debug, Serialize, Deserialize)]
struct ScheduleResult {
    response_count: usize,
    drop_count: usize,
    sched_miss_count: usize,
    full_stream_count: usize,
    first_send: Option<Duration>,
    last_send: Option<Duration>,
    latencies: BTreeMap<u64, usize>,
    first_tsc: Option<u64>,
    trace: Option<Vec<TraceResult>>,
}

fn memcached_preload_worker(
    proto: MemcachedProtocol,
    this_start: usize,
    nr_values: usize,
    backend: Backend,
    addr: SocketAddrV4,
) -> io::Result<()> {
    let sock1 = Transport::Tcp.dial(backend, None, addr)?;
    let socket = sock1.clone();
    backend.spawn_detached(move || {
        backend.sleep(Duration::from_secs(SOCKET_TIMEOUT_SECONDS));
        if Arc::strong_count(&socket) > 1 {
            println!("Timing out socket");
            socket.shutdown();
        }
    });

    let mut vec_s: Vec<u8> = Vec::with_capacity(KBUFSIZE);
    let mut vec_r: Vec<u8> = vec![0; KBUFSIZE];
    let mut buf = Buffer::new(&mut vec_r[..]);
    let mut socket = &*sock1;
    for n in 0..nr_values {
        vec_s.clear();
        proto.set_request((this_start + n) as u64, 0, &mut vec_s);

        socket.write_all(&vec_s[..]).map_err(|e| {
            io::Error::new(
                e.kind(),
                format!("Preload send ({}/{}): {}", n, nr_values, e),
            )
        })?;

        proto.read_response(&socket, &mut buf).map_err(|e| {
            io::Error::new(
                e.kind(),
                format!("preload receive ({}/{}): {}", n, nr_values, e),
            )
        })?;
    }
    Ok(())
}

/// Join all handles that yield `Result<T, E>`, return the first `E` if any,
/// otherwise collect all `T`s.
///
/// Panics if any thread panicked.
fn join_and_collect<T, E, I>(handles: I) -> Result<Vec<T>, E>
where
    I: IntoIterator<Item = JoinHandle<Result<T, E>>>,
    T: Send + 'static + std::fmt::Debug,
    E: Send + 'static + std::fmt::Debug,
{
    let (successful_results, failed_results): (Vec<_>, Vec<_>) = handles
        .into_iter()
        .map(|h| h.join().unwrap()) // panic on thread panic
        .partition(Result::is_ok);

    if let Some(first_failure) = failed_results.into_iter().next() {
        return Err(first_failure.unwrap_err());
    }

    let values = successful_results.into_iter().map(Result::unwrap).collect();

    Ok(values)
}

fn run_memcached_preload(
    proto: MemcachedProtocol,
    barrier_group: &mut lockstep::Group,
    backend: Backend,
    addr: SocketAddrV4,
    nthreads: usize,
) -> io::Result<()> {
    // Divide keyspace among peers.
    let (rank, size) = barrier_group.rank_and_size();
    let perrank = proto.nvalues.div_ceil(size as u64) as usize;
    let perthread = perrank.div_ceil(nthreads);

    let join_handles: Vec<JoinHandle<_>> = (0..nthreads)
        .map(|i| {
            let this_start = rank * perrank + i * perthread;
            backend.spawn_thread(move || {
                memcached_preload_worker(proto, this_start, perthread, backend, addr)
            })
        })
        .collect();

    let _ = join_and_collect(join_handles)?;
    Ok(())
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
    for t in 1..(RAMPUP_STEPS_PER_SECOND * ramp_up_seconds) {
        let rate = t * packets_per_second / (ramp_up_seconds * RAMPUP_STEPS_PER_SECOND);

        if rate == 0 {
            continue;
        }

        sched.push(RequestSchedule {
            arrival: Distribution::Exponential((nthreads * NS_PER_SEC as usize / rate) as f64),
            service: distribution,
            output: OutputMode::Silent,
            runtime: Duration::from_micros(1_000_000 / RAMPUP_STEPS_PER_SECOND as u64),
            rps: rate,
            discard_pct: 0.0,
        });
    }

    let ns_per_packet = nthreads * NS_PER_SEC as usize / packets_per_second;

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

fn gen_loadshift_experiment(config: &AppConfig) -> Vec<RequestSchedule> {
    config
        .loadshift_spec
        .split(",")
        .map(|step_spec| {
            let s: Vec<&str> = step_spec.split(":").collect();
            assert!(s.len() >= 2 && s.len() <= 3);
            let packets_per_second: u64 = s[0].parse().unwrap();
            let ns_per_packet = config.nthreads as u64 * NS_PER_SEC / packets_per_second;
            let micros = s[1].parse().unwrap();
            let output = match s.len() {
                2 => config.output_mode,
                3 => OutputMode::Silent,
                _ => unreachable!(),
            };
            RequestSchedule {
                arrival: Distribution::Exponential(ns_per_packet as f64),
                service: config.distribution,
                output: output,
                runtime: Duration::from_micros(micros),
                rps: packets_per_second as usize,
                discard_pct: 0.0,
            }
        })
        .collect()
}

fn get_rt_stat_headers() -> Vec<String> {
    let mut headers = Vec::with_capacity(1);
    headers.push("CPU Used (%)".to_string());
    headers
}

fn get_rt_stat_values(map1: &StatMap, map0: &StatMap) -> Vec<String> {
    let cycles1 = map1.get("program_cycles").unwrap() + map1.get("sched_cycles").unwrap();
    let cycles0 = map0.get("program_cycles").unwrap() + map0.get("sched_cycles").unwrap();
    let tsc1 = map1.get("tsc").unwrap();
    let tsc0 = map0.get("tsc").unwrap();

    // Calculate deltas
    let delta_cycles = cycles1 - cycles0;
    let delta_tsc = tsc1 - tsc0;

    let mut values = Vec::with_capacity(1);
    values.push(format!(
        "{:.2}%",
        (100.0 * delta_cycles as f64 / delta_tsc as f64)
    ));
    values
}

fn process_result_final(
    sched: &RequestSchedule,
    results: Vec<ScheduleResult>,
    wct_start: SystemTime,
    sched_start: Duration,
    rt_stats: Option<(StatMap, StatMap)>,
) -> bool {
    let mut buckets: BTreeMap<u64, usize> = BTreeMap::new();

    static HEADER_INIT: Once = Once::new();

    HEADER_INIT.call_once(|| {
        println!("Distribution, Target, Actual, Dropped, Schedule Adherance, Min, Median, 90th, 99th, 99.9th, 99.99th, Max, Start, StartTsc{}", if rt_stats.is_some() {
            ", ".to_owned() + &get_rt_stat_headers().join(",") } else { "".to_owned() });
    });

    if let OutputMode::Silent = sched.output {
        return true;
    }

    let response_count = results.iter().map(|res| res.response_count).sum::<usize>();
    let drop_count = results.iter().map(|res| res.drop_count).sum::<usize>();
    let sched_miss_count = results
        .iter()
        .map(|res| res.sched_miss_count)
        .sum::<usize>();
    let full_stream_count = results
        .iter()
        .map(|res| res.full_stream_count)
        .sum::<usize>();
    let first_send = results.iter().filter_map(|res| res.first_send).min();
    let last_send = results.iter().filter_map(|res| res.last_send).max();
    let first_tsc = results.iter().filter_map(|res| res.first_tsc).min();
    let start_unix = wct_start + sched_start;

    // Count stream-full events as drops.
    let sent_count = response_count + drop_count + full_stream_count;

    // TODO: better handling of this case.
    if response_count <= 1 {
        println!(
            "{}, {}, 0, {}, {}, {}",
            sched.service.name(),
            sched.rps,
            drop_count + full_stream_count,
            sched_miss_count,
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
        let idx = (sent_count as f32 * p / 100.0) as usize;
        if idx >= response_count {
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

    if let OutputMode::Live = sched.output {
        println!(
            "RPS: {}\tMedian (us): {: <7}\t99th (us): {: <7}\t99.9th (us): {: <7}",
            sched.rps,
            percentile(50.0) as usize,
            percentile(99.0) as usize,
            percentile(99.9) as usize
        );
        return true;
    }
    let rt_stat_values = rt_stats.map_or_else(
        || String::new(),
        |(map1, map0)| format!(", {}", get_rt_stat_values(&map1, &map0).join(", ")),
    );

    let schedule_adherance = 100.0 * sent_count as f64 / (sent_count + sched_miss_count) as f64;

    println!(
        "{}, {}, {}, {}, {:.2}, {:.1}, {:.1}, {:.1}, {:.1}, {:.1}, {:.1}, {:.1}, {}, {}{}",
        sched.service.name(),
        sent_count as u64 / (last_send - first_send).as_secs(),
        response_count as u64 / (last_send - first_send).as_secs(),
        drop_count + full_stream_count,
        schedule_adherance,
        percentile(0.0),
        percentile(50.0),
        percentile(90.0),
        percentile(99.0),
        percentile(99.9),
        percentile(99.99),
        *buckets.keys().max().unwrap() as f32,
        start_unix.duration_since(UNIX_EPOCH).unwrap().as_secs(),
        first_tsc,
        rt_stat_values
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

    if schedule_adherance < SCHEDULE_ADHERANCE_THRESH_HIGH {
        eprintln!("Aborting: schedule adherance is low - the client is saturating.");
        return false;
    }

    if schedule_adherance < SCHEDULE_ADHERANCE_THRESH_LOW {
        static ONCE: Once = Once::new();
        ONCE.call_once(|| {
            eprintln!("WARNING: Schedule adherance is low - the client may be saturating.");
        });
    }

    if full_stream_count > 0 {
        static ONCE: Once = Once::new();
        ONCE.call_once(|| {
            eprintln!("WARNING: TCP send buffer was full - requests could not be enqueued and were counted as drops.");
        });
    }

    true
}

fn process_result(sched: &RequestSchedule, packets: &mut [Packet]) -> Option<ScheduleResult> {
    if packets.len() == 0 {
        return None;
    }

    if let OutputMode::Silent = sched.output {
        return None;
    }

    // Discard the first X% of the packets.
    let pidx = packets.len() as f32 * sched.discard_pct / 100.0;
    let packets = &mut packets[pidx as usize..];

    let mut sched_miss = 0;
    let mut full_stream = 0;
    let mut dropped = 0;
    let mut latencies = BTreeMap::new();
    for p in packets.iter() {
        match (p.actual_start, p.completion_time) {
            (None, _) => sched_miss += 1,
            (_, None) => dropped += 1,
            (Some(ref start), Some(ref end)) => {
                if *end == *start {
                    full_stream += 1;
                } else {
                    *latencies
                        .entry((*end - *start).as_micros() as u64)
                        .or_insert(0) += 1
                }
            }
        }
    }

    if packets.len() - dropped - sched_miss - full_stream <= 1 {
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
                    if p.completion_time.is_none() {
                        return None;
                    }

                    if p.actual_start == p.completion_time {
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
        response_count: packets.len() - dropped - sched_miss - full_stream,
        drop_count: dropped,
        sched_miss_count: sched_miss,
        full_stream_count: full_stream,
        first_send: first_send,
        last_send: last_send,
        latencies: latencies,
        first_tsc: first_tsc,
        trace: trace,
    })
}

fn gen_packets_for_schedule(schedules: &Arc<Vec<RequestSchedule>>) -> (Vec<Packet>, Vec<usize>) {
    let mut packets: Vec<Packet> = Vec::new();
    let mut rng: Mt64 = Mt64::new(rand::thread_rng().gen::<u64>());
    let mut sched_boundaries = Vec::new();
    let mut last = DEFAULT_SCHED_START_NS;
    let mut end = last;
    for sched in schedules.iter() {
        end += duration_to_ns(sched.runtime);
        loop {
            packets.push(Packet {
                randomness: rng.gen::<u64>(),
                target_start: Duration::from_nanos(last),
                work_iterations: sched.service.sample(&mut rng),
                ..Default::default()
            });

            let nxt = last + sched.arrival.sample(&mut rng);
            if nxt >= end {
                break;
            }
            last = nxt;
        }
        sched_boundaries.push(packets.len());
    }
    (packets, sched_boundaries)
}

#[derive(Clone)]
enum StatConn {
    Conn(Arc<Connection>),
    None,
}

impl StatConn {
    pub fn trigger_stat(&self) -> io::Result<()> {
        match *self {
            StatConn::Conn(ref conn) => (&**conn).write_all(b"s"),
            StatConn::None => Ok(()),
        }
    }

    pub fn new(conn: Arc<Connection>) -> Self {
        StatConn::Conn(conn)
    }
}

#[derive(Clone)]
struct FanInOutWaitGroup {
    wait_for_followers: shenango::WaitGroup,
    wait_for_leader: shenango::WaitGroup,
}

impl FanInOutWaitGroup {
    pub fn new(group_size: usize) -> Self {
        let wait_for_followers = shenango::WaitGroup::new();
        let wait_for_leader = shenango::WaitGroup::new();
        wait_for_followers.add(group_size as i32);
        wait_for_leader.add(1);
        Self {
            wait_for_followers,
            wait_for_leader,
        }
    }

    pub fn leader_release_all(&self) -> &Self {
        self.wait_for_leader.done();
        self.wait_for_leader.add(1);
        self
    }

    pub fn leader_catch_all(&self) -> &Self {
        self.wait_for_followers.wait();
        self
    }

    pub fn leader_rearm(&self, next_group_size: usize) -> &Self {
        self.wait_for_followers.add(next_group_size as i32);
        self
    }

    pub fn follower_notify(&self) -> &Self {
        self.wait_for_followers.done();
        self
    }

    pub fn follower_wait(&self) -> &Self {
        self.wait_for_leader.wait();
        self
    }
}

fn run_client_worker(
    proto: Protocol,
    backend: Backend,
    addr: SocketAddrV4,
    tport: Transport,
    wg: FanInOutWaitGroup,
    schedules: Arc<Vec<RequestSchedule>>,
    index: usize,
    live_mode_socket: Option<Arc<Connection>>,
    stat_conn: StatConn,
) -> io::Result<Vec<Option<ScheduleResult>>> {
    let mut payload = Vec::with_capacity(KBUFSIZE);
    let (mut packets, sched_boundaries) = gen_packets_for_schedule(&schedules);
    let src_addr = SocketAddrV4::new(Ipv4Addr::new(0, 0, 0, 0), (100 + index) as u16);
    let live_mode = live_mode_socket.is_some();
    let socket = live_mode_socket.unwrap_or(tport.dial(backend, Some(src_addr), addr)?);

    let packets_per_thread = packets.len();
    let socket2 = socket.clone();
    let rproto = proto.clone();
    let wg2 = wg.clone();
    let receive_thread = backend.spawn_thread(move || {
        let mut recv_buf = vec![0; KBUFSIZE];
        let mut receive_times = vec![None; packets_per_thread];
        let mut buf = Buffer::new(&mut recv_buf);
        let use_ordering = rproto.uses_ordered_requests();
        wg2.follower_notify();
        for i in 0..receive_times.len() {
            match rproto.read_response(&socket2, &mut buf) {
                Ok((mut idx, tsc)) => {
                    if use_ordering {
                        idx = i;
                    }
                    receive_times[idx] = Some((Instant::now(), tsc));
                }
                Err(e) => {
                    match e.raw_os_error() {
                        Some(libc::ECONNABORTED) | Some(libc::ECONNRESET) => break,
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
    let live_mode2 = live_mode;
    let timer = backend.spawn_thread(move || {
        wg2.follower_notify().follower_wait();
        if live_mode2 {
            return;
        }
        backend.sleep(last + Duration::from_millis(SOCKET_TIMEOUT_SECONDS));
        if Arc::strong_count(&socket2) > 1 {
            socket2.shutdown();
        }
    });

    wg.follower_notify().follower_wait();
    let start = Instant::now();
    let mut sched_idx = 0;

    stat_conn.trigger_stat()?;

    for (i, packet) in packets.iter_mut().enumerate() {
        payload.clear();
        proto.gen_req(i, packet, &mut payload);

        let mut t = start.elapsed();
        while t + MIN_TIMER_SLEEP_MICROS < packet.target_start {
            backend.sleep(packet.target_start - t - MIN_TIMER_SLEEP_MICROS);
            t = start.elapsed();
        }
        if !live_mode && t > packet.target_start + MAX_CATCH_UP_TIME_MICROS {
            continue;
        }

        if i >= sched_boundaries[sched_idx] {
            sched_idx += 1;
            stat_conn.trigger_stat()?;
        }

        packet.actual_start = Some(start.elapsed());

        if let Err(e) = (&*socket).nonpartial_write(&payload[..], true) {
            match e.raw_os_error() {
                Some(libc::EAGAIN) | Some(libc::ENOSPC) => {
                    // Mark packet as dropped due to stream being full by setting completion_time = actual_start
                    packet.completion_time = packet.actual_start;
                    continue;
                }
                Some(libc::EPIPE) | Some(libc::ECONNABORTED) | Some(libc::ECONNRESET) => {
                    break;
                }
                _ => {
                    println!("Send thread ({}/{}): {}", i, packets.len(), e);
                    break;
                }
            }
        }
    }

    stat_conn.trigger_stat()?;

    wg.follower_notify().follower_wait();

    // Packet status interpretation:
    // - No actual_start: Packet missed deadline and was never sent
    // - Has actual_start but no completion_time: Packet was dropped
    // - Completion_time is set already (and equal to actual_start): Packet not enqueued (stream full)
    timer.join().unwrap();
    receive_thread
        .join()
        .unwrap()
        .into_iter()
        .zip(
            packets
                .iter_mut()
                // Filter packets without responses if protocol doesn't map requests to responses
                .filter(|p| {
                    !proto.uses_ordered_requests()
                        || (p.actual_start.is_some() && p.completion_time.is_none())
                }),
        )
        .for_each(|(c, p)| {
            if let Some((inst, tsc)) = c {
                (*p).completion_time = Some(inst - start);
                (*p).completion_server_tsc = Some(tsc);
            }
        });

    let mut start_index = 0;
    Ok(schedules
        .iter()
        .zip(sched_boundaries)
        .map(|(sched, end)| {
            let res = process_result(&sched, &mut packets[start_index..end]);
            start_index = end;
            res
        })
        .collect::<Vec<Option<ScheduleResult>>>())
}

fn run_live_client(config: AppConfig, barrier_group: &mut lockstep::Group) {
    let sched = gen_classic_packet_schedule(
        config.runtime,
        config.packets_per_second,
        OutputMode::Live,
        config.distribution,
        config.rampup,
        config.nthreads,
        config.discard_pct,
    );

    let proto = config.protocol;
    let backend = config.backend;
    let addrs = config.addrs;
    let nthreads = config.nthreads;
    let tport = config.transport;

    let schedules = Arc::new(sched);

    let sockets: Vec<_> = (0..nthreads)
        .into_iter()
        .map(|i| tport.dial(backend, None, addrs[i % addrs.len()]))
        .collect::<Result<Vec<_>, _>>()
        .unwrap();

    let wg = FanInOutWaitGroup::new(3 * nthreads);

    loop {
        let socks = sockets.clone();

        let conn_threads: Vec<_> = (0..nthreads)
            .into_iter()
            .zip(socks)
            .map(|(i, sock)| {
                let client_idx = 100 + i;
                let proto = proto.clone();
                let wg = wg.clone();
                let schedules = schedules.clone();
                let addr = addrs[i % addrs.len()];

                backend.spawn_thread(move || {
                    run_client_worker(
                        proto,
                        backend,
                        addr,
                        tport,
                        wg,
                        schedules,
                        client_idx,
                        Some(sock.clone()),
                        StatConn::None,
                    )
                })
            })
            .collect();

        wg.leader_catch_all().leader_rearm(nthreads);
        barrier_group.barrier().unwrap();
        let start_unix = SystemTime::now();
        wg.leader_release_all();

        wg.leader_catch_all()
            .leader_rearm(3 * nthreads)
            .leader_release_all();

        let mut packets: Vec<Vec<Option<ScheduleResult>>> = conn_threads
            .into_iter()
            .map(|s| s.join().unwrap().unwrap())
            .collect();

        let mut sched_start = Duration::from_nanos(DEFAULT_SCHED_START_NS);
        schedules
            .iter()
            .enumerate()
            .map(|(i, sched)| {
                let perthread = packets.iter_mut().filter_map(|p| p[i].take()).collect();
                let r = process_result_final(sched, perthread, start_unix, sched_start, None);
                sched_start += sched.runtime;
                r
            })
            .collect::<Vec<bool>>()
            .into_iter()
            .all(|p| p);
    }
}

fn run_closed_loop_worker(
    proto: Protocol,
    backend: Backend,
    addr: SocketAddrV4,
    tport: Transport,
    wg: FanInOutWaitGroup,
    depth: usize,
    start: Instant,
    end_time: Arc<AtomicUsize>,
) -> usize {
    let mut payload = Vec::with_capacity(KBUFSIZE);
    let src_addr = SocketAddrV4::new(Ipv4Addr::new(0, 0, 0, 0), 0);
    let socket = tport.dial(backend, Some(src_addr), addr).unwrap();
    let mut socket = &*socket;

    let mut recv_buf = vec![0; KBUFSIZE];
    let mut buf = Buffer::new(&mut recv_buf);

    wg.follower_notify().follower_wait();

    // Note all packets are the same.
    let p = Packet::default();
    proto.gen_req(0, &p, &mut payload);

    let mut cnt = 0;
    let mut outstanding = 0;

    let done = || {
        return (start.elapsed().as_micros() as usize) >= end_time.load(Ordering::Relaxed);
    };

    loop {
        while outstanding < depth && !done() {
            socket.write_all(&payload[..]).unwrap();
            outstanding += 1;
        }

        if done() {
            return cnt;
        }

        match proto.read_response(&socket, &mut buf) {
            Ok(_) => {
                if done() {
                    return cnt;
                }
                cnt += 1;
                outstanding -= 1;
            }
            Err(e) => {
                panic!("Receive thread: {} host {}", e, addr);
            }
        }
    }
}

fn run_closed_loop_client(config: AppConfig, barrier_group: &mut lockstep::Group) -> bool {
    let proto = config.protocol;
    let backend = config.backend;
    let addrs = config.addrs;
    let conns_per_addr = config.nthreads;
    let tport = config.transport;
    let depth = config.depth;
    let runtime = config.runtime;

    let wg = FanInOutWaitGroup::new(conns_per_addr * addrs.len());

    let end_time = Arc::new(AtomicUsize::new(usize::MAX));

    let start = Instant::now();

    let conn_threads: Vec<_> = addrs
        .clone()
        .iter()
        .flat_map(|addr| {
            (0..conns_per_addr).map(|_| {
                let proto = proto.clone();
                let wg = wg.clone();
                let end_time = end_time.clone();
                let addr = addr.clone();
                backend.spawn_thread(move || {
                    run_closed_loop_worker(proto, backend, addr, tport, wg, depth, start, end_time)
                })
            })
        })
        .collect();

    wg.leader_catch_all();

    barrier_group.barrier().unwrap();

    end_time.store(
        (start.elapsed() + runtime).as_micros() as usize,
        Ordering::SeqCst,
    );
    wg.leader_release_all();

    let total_requests: usize = conn_threads.into_iter().map(|s| s.join().unwrap()).sum();
    println!("RPS: {}", total_requests as u64 / runtime.as_secs());
    true
}

type StatMap = HashMap<String, u64>;

fn stat_receiver(conn: Arc<Connection>, nschedules: usize) -> io::Result<Vec<StatMap>> {
    let mut buf = vec![0; 4096];
    let mut stat_results = Vec::with_capacity(nschedules);

    for _ in 0..nschedules {
        // Read message size (8 bytes)
        if let Err(err) = (&*conn).read_exact(&mut buf[..8]) {
            eprintln!("Stat receive error: {}", err);
            return Err(err);
        }

        // Parse message size
        let data_sz = u64::from_le_bytes(buf[..8].try_into().unwrap());

        // Read message data
        if let Err(err) = (&*conn).read_exact(&mut buf[..data_sz as usize]) {
            eprintln!("Failed to read stats data: {}", err);
            return Err(err);
        }

        // Parse stats string into map
        match String::from_utf8(buf[..(data_sz - 1) as usize].to_vec()) {
            Ok(stats) => {
                let stats_map = stats
                    .split(',')
                    .filter_map(|stat| {
                        let mut parts = stat.split(':');
                        match (parts.next(), parts.next()) {
                            (Some(key), Some(val)) => {
                                val.parse().ok().map(|v| (key.to_string(), v))
                            }
                            _ => None,
                        }
                    })
                    .collect();
                stat_results.push(stats_map);
            }
            Err(err) => eprintln!("Failed to parse stats string: {}", err),
        }
    }

    Ok(stat_results)
}

fn run_sample(
    config: &AppConfig,
    barrier_group: &mut lockstep::Group,
    schedules: Vec<RequestSchedule>,
    index: usize,
) -> io::Result<bool> {
    let proto = config.protocol;
    let backend = config.backend;
    let addrs = &config.addrs;
    let nthreads = config.nthreads;
    let tport = config.transport;
    let do_runtime_stat = config.do_runtime_stat;

    let schedules = Arc::new(schedules);
    let wg = FanInOutWaitGroup::new(3 * nthreads);

    // Dial stat connection.
    let (stat_conn, stat_thread) = if do_runtime_stat {
        let stat_addr = SocketAddrV4::new(*addrs[0].ip(), RUNTIME_STAT_PORT);
        let stat_conn = Arc::new(backend.create_tcp_connection(None, stat_addr)?);
        let stat_recv_conn = stat_conn.clone();
        let sched_len = schedules.len() + 1;
        let stat_thread = backend.spawn_thread(move || stat_receiver(stat_recv_conn, sched_len));
        (StatConn::new(stat_conn), Some(stat_thread))
    } else {
        (StatConn::None, None)
    };

    let conn_threads: Vec<_> = (0..nthreads)
        .into_iter()
        .map(|i| {
            let client_idx = 100 + (index * nthreads) + i;
            let proto = proto.clone();
            let wg = wg.clone();
            let schedules = schedules.clone();
            let addr = addrs[i % addrs.len()];
            let stat_conn = match i {
                0 => stat_conn.clone(),
                _ => StatConn::None,
            };

            backend.spawn_thread(move || {
                run_client_worker(
                    proto, backend, addr, tport, wg, schedules, client_idx, None, stat_conn,
                )
            })
        })
        .collect();

    backend.sleep(Duration::from_secs(CLIENT_WAIT_SECONDS));
    wg.leader_catch_all().leader_rearm(nthreads);
    barrier_group.barrier()?;
    let start_unix = SystemTime::now();
    wg.leader_release_all();

    // In-thread post-communication processing only happens after all comms are done.
    wg.leader_catch_all();
    wg.leader_release_all();

    let mut packets = join_and_collect(conn_threads)?;

    let rt_stats: Vec<Option<(StatMap, StatMap)>> = if do_runtime_stat {
        let rt_stats = stat_thread.unwrap().join().unwrap().unwrap();
        rt_stats
            .windows(2)
            .map(|w| Some((w[1].clone(), w[0].clone())))
            .collect()
    } else {
        vec![None; schedules.len()]
    };

    barrier_group.sync_results(&mut packets)?;

    if barrier_group.rank_and_size().0 > 0 {
        barrier_group.barrier()?;
        return Ok(true);
    }

    let mut sched_start = Duration::from_nanos(DEFAULT_SCHED_START_NS);
    let result = schedules
        .iter()
        .enumerate()
        .zip(rt_stats)
        .map(|((i, sched), rt_stat)| {
            let perthread: Vec<_> = packets.iter_mut().filter_map(|p| p[i].take()).collect();
            if perthread.len() > 0 && perthread.len() < packets.len() {
                panic!("Missing connection data");
            }
            let r = process_result_final(sched, perthread, start_unix, sched_start, rt_stat);
            sched_start += sched.runtime;
            r
        })
        .collect::<Vec<bool>>()
        .into_iter()
        .all(|p| p);

    barrier_group.barrier()?;
    Ok(result)
}

fn run_local(
    backend: Backend,
    nthreads: usize,
    worker: Arc<FakeWorker>,
    schedules: Vec<RequestSchedule>,
) -> bool {
    let schedules = Arc::new(schedules);

    let packet_schedules: Vec<Vec<Packet>> = (0..nthreads)
        .map(|_| gen_packets_for_schedule(&schedules).0)
        .collect();

    let start_unix = SystemTime::now();
    let start = Instant::now();

    let mut send_threads = Vec::new();
    for mut packets in packet_schedules {
        let worker = worker.clone();
        let schedules = schedules.clone();
        send_threads.push(backend.spawn_thread(move || {
            let remaining = Arc::new(AtomicUsize::new(packets.len()));
            for i in 0..packets.len() {
                let (work_iterations, completion_time_ns, rnd) = {
                    let packet = &mut packets[i];

                    {
                        let mut t = start.elapsed();
                        while t < packet.target_start {
                            t = start.elapsed();
                        }
                    }

                    packet.actual_start = Some(start.elapsed());
                    (
                        packet.work_iterations,
                        packet.completion_time_ns.clone(),
                        packet.randomness,
                    )
                };

                let remaining = remaining.clone();
                let worker = worker.clone();
                backend.spawn_thread(move || {
                    worker.work(work_iterations, rnd);
                    completion_time_ns.store(start.elapsed().as_nanos() as u64, Ordering::Relaxed);
                    remaining.fetch_sub(1, Ordering::Relaxed);
                });
            }

            while remaining.load(Ordering::SeqCst) > 0 {
                // do nothing
            }

            for p in packets.iter_mut() {
                p.completion_time = Some(Duration::from_nanos(
                    p.completion_time_ns.load(Ordering::Relaxed),
                ));
            }

            let mut start = Duration::from_nanos(DEFAULT_SCHED_START_NS);
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

    let mut sched_start = Duration::from_nanos(DEFAULT_SCHED_START_NS);
    schedules
        .iter()
        .enumerate()
        .map(|(i, sched)| {
            let perthread = packets.iter_mut().filter_map(|p| p[i].take()).collect();
            let r = process_result_final(sched, perthread, start_unix, sched_start, None);
            sched_start += sched.runtime;
            r
        })
        .collect::<Vec<bool>>()
        .into_iter()
        .all(|p| p)
}

impl AppConfig {
    pub fn from_matches(matches: &ArgMatches) -> Result<Self, Box<dyn std::error::Error>> {
        let calibrate = matches.get_one::<u64>("calibrate").copied();

        let addrs: Vec<SocketAddrV4> = matches
            .get_many::<SocketAddrV4>("ADDR")
            .unwrap_or_default()
            .copied()
            .collect();

        if addrs.is_empty() && calibrate.is_none() {
            return Err("No addresses provided".into());
        }

        let nthreads = {
            if let Some(val) = matches.get_one::<usize>("threads") {
                eprintln!("⚠️ Warning: --threads/-t is deprecated, use --conns/-c instead.");
                *val
            } else {
                matches
                    .get_one::<usize>("conns")
                    .copied()
                    .unwrap_or(DEFAULT_NCONNS)
            }
        };

        let runtime = Duration::from_secs(
            matches
                .get_one::<u64>("runtime")
                .copied()
                .unwrap_or(DEFAULT_RUNTIME_SECONDS),
        );
        let packets_per_second = (1_000_000.0
            * matches
                .get_one::<f32>("mpps")
                .copied()
                .unwrap_or(DEFAULT_MPPS)) as usize;
        let start_packets_per_second = (1_000_000.0
            * matches
                .get_one::<f32>("start_mpps")
                .copied()
                .unwrap_or(DEFAULT_START_MPPS)) as usize;

        if start_packets_per_second > packets_per_second {
            return Err("start_mpps cannot be greater than mpps".into());
        }

        let world_size = matches
            .get_one::<usize>("barrier-peers")
            .copied()
            .unwrap_or(1);

        let discard_pct = matches
            .get_one::<f32>("discard_pct")
            .copied()
            .unwrap_or(DEFAULT_DISCARD_PCT);
        let do_runtime_stat = *matches.get_one::<bool>("runtime_stat").unwrap_or(&false);
        let live_mode = *matches.get_one::<bool>("live").unwrap_or(&false);
        let closed_bench = *matches.get_one::<bool>("closed_bench").unwrap_or(&false);
        let depth = matches
            .get_one::<usize>("depth")
            .copied()
            .unwrap_or(DEFAULT_DEPTH);
        let do_warmup = *matches.get_one::<bool>("warmup").unwrap_or(&false);
        let intersample_sleep = matches
            .get_one::<u64>("intersample_sleep")
            .copied()
            .unwrap_or(DEFAULT_INTERSAMPLE_SLEEP_SECONDS);
        let samples = matches
            .get_one::<usize>("samples")
            .copied()
            .unwrap_or(DEFAULT_SAMPLES);
        let rampup = matches
            .get_one::<usize>("rampup")
            .copied()
            .unwrap_or(DEFAULT_RAMPUP_SECONDS);
        let fakework_spec = matches
            .get_one::<String>("fakework")
            .unwrap_or(&DEFAULT_FAKEWORK_SPEC.to_string())
            .to_string();
        let loadshift_spec = matches
            .get_one::<String>("loadshift")
            .unwrap_or(&"".to_string())
            .to_string();

        let mode_str = matches.get_one::<String>("mode").unwrap();
        let mode = RunMode::from_str(mode_str)?;

        let backend = if mode.requires_runtime_backend() {
            Backend::Runtime
        } else {
            Backend::Linux
        };

        let transport = *matches.get_one::<Transport>("transport").unwrap();
        let output_mode = *matches.get_one::<OutputMode>("output").unwrap();

        let distribution = Self::create_distribution(matches)?;
        let protocol = Self::create_protocol(matches, transport, &distribution)?;

        Ok(AppConfig {
            addrs,
            nthreads,
            runtime,
            packets_per_second,
            start_packets_per_second,
            distribution,
            transport,
            protocol,
            output_mode,
            samples,
            rampup,
            discard_pct,
            fakework_spec,
            intersample_sleep,
            do_runtime_stat,
            live_mode,
            closed_bench,
            depth,
            do_warmup,
            loadshift_spec,
            mode,
            backend,
            calibrate,
            world_size,
            rank: 0,
        })
    }

    fn create_distribution(
        matches: &ArgMatches,
    ) -> Result<Distribution, Box<dyn std::error::Error>> {
        let distspec = if matches.contains_id("distspec") {
            matches
                .get_one::<String>("distspec")
                .ok_or("distspec argument not found")?
                .to_string()
        } else {
            let mean = matches
                .get_one::<String>("mean")
                .unwrap_or(&DEFAULT_MEAN_WORK_ITERATIONS.to_string())
                .parse::<f64>()
                .map_err(|e| format!("Invalid mean value: {}", e))?;
            match matches
                .get_one::<String>("distribution")
                .unwrap_or(&DEFAULT_DISTRIBUTION.to_string())
                .as_str()
            {
                "zero" => "zero".to_string(),
                "constant" => format!("constant:{}", mean),
                "exponential" => format!("exponential:{}", mean),
                "bimodal1" => format!("bimodal:0.9:{}:{}", mean * 0.5, mean * 5.5),
                "bimodal2" => format!("bimodal:0.999:{}:{}", mean * 0.5, mean * 500.5),
                "bimodal3" => format!("bimodal:0.99:{}:{}", mean * 0.5, mean * 5.5),
                _ => return Err("Unknown distribution type".into()),
            }
        };
        Ok(Distribution::create(&distspec)?)
    }

    fn create_protocol(
        matches: &ArgMatches,
        transport: Transport,
        distribution: &Distribution,
    ) -> Result<Protocol, Box<dyn std::error::Error>> {
        let protocol = match matches
            .get_one::<String>("protocol")
            .unwrap_or(&DEFAULT_PROTOCOL.to_string())
            .to_string()
            .as_str()
        {
            "synthetic" => Protocol::Synthetic(SyntheticProtocol::with_args(matches, transport)),
            "memcached" => Protocol::Memcached(MemcachedProtocol::with_args(matches, transport)),
            "dns" => Protocol::Dns(DnsProtocol::with_args(matches, transport)),
            "reflex" => Protocol::Reflex(ReflexProtocol::with_args(
                matches,
                transport,
                distribution.clone(),
            )),
            "http" => Protocol::Http(HttpProtocol::with_args(matches, transport)),
            _ => return Err("Unknown protocol type".into()),
        };
        Ok(protocol)
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub enum RunMode {
    LinuxServer,
    RuntimeClient,
    SpawnerServer,
    LocalClient,
}

impl FromStr for RunMode {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "linux-server" => Ok(RunMode::LinuxServer),
            "runtime-client" => Ok(RunMode::RuntimeClient),
            "spawner-server" => Ok(RunMode::SpawnerServer),
            "local-client" => Ok(RunMode::LocalClient),
            _ => Err(format!("Unknown run mode: {}", s)),
        }
    }
}

impl RunMode {
    pub fn requires_runtime_backend(&self) -> bool {
        matches!(
            self,
            RunMode::RuntimeClient | RunMode::SpawnerServer | RunMode::LocalClient
        )
    }
}

fn do_run_local(config: AppConfig) -> Result<(), Box<dyn std::error::Error>> {
    if config.do_warmup {
        let sched = gen_classic_packet_schedule(
            Duration::from_secs(1),
            config.packets_per_second,
            OutputMode::Silent,
            config.distribution,
            WARMUP_RAMPUP_SECONDS,
            config.nthreads,
            config.discard_pct,
        );
        run_local(config.backend, config.nthreads, config.fakeworker(), sched);
    }
    let step_size = (config.packets_per_second - config.start_packets_per_second) / config.samples;
    for j in 1..=config.samples {
        let sched = gen_classic_packet_schedule(
            config.runtime,
            config.start_packets_per_second + step_size * j,
            config.output_mode,
            config.distribution,
            WARMUP_RAMPUP_SECONDS,
            config.nthreads,
            config.discard_pct,
        );
        run_local(config.backend, config.nthreads, config.fakeworker(), sched);
        config
            .backend
            .sleep(Duration::from_secs(config.intersample_sleep));
    }
    Ok(())
}

fn run_multistep_client(
    config: AppConfig,
    mut barrier_group: &mut lockstep::Group,
) -> io::Result<()> {
    barrier_group.barrier()?;

    println!("Running with rank: {}", barrier_group.rank_and_size().0);
    if let Protocol::Memcached(proto) = config.protocol {
        for addr in &config.addrs {
            run_memcached_preload(
                proto,
                &mut barrier_group,
                config.backend,
                *addr,
                config.nthreads,
            )
            .map_err(|e| {
                io::Error::new(
                    e.kind(),
                    format!("Could not preload memcached: {} ({})", e.kind(), e),
                )
            })?;
        }
    }

    if config.closed_bench {
        run_closed_loop_client(config, &mut barrier_group);
        return Ok(());
    }

    if config.live_mode {
        run_live_client(config, &mut barrier_group);
        unreachable!();
    }

    if !config.loadshift_spec.is_empty() {
        run_sample(
            &config,
            &mut barrier_group,
            gen_loadshift_experiment(&config),
            0,
        )?;
        barrier_group.barrier()?;
        return Ok(());
    }

    if config.do_warmup {
        let sched = gen_classic_packet_schedule(
            Duration::from_secs(1),
            config.packets_per_second,
            OutputMode::Silent,
            config.distribution,
            WARMUP_DURATION_SECONDS,
            config.nthreads,
            config.discard_pct,
        );
        run_sample(&config, &mut barrier_group, sched, 0)?;
    }

    let step_size = (config.packets_per_second - config.start_packets_per_second) / config.samples;
    for j in 1..=config.samples {
        let sched = gen_classic_packet_schedule(
            config.runtime,
            config.start_packets_per_second + step_size * j,
            config.output_mode,
            config.distribution,
            config.rampup,
            config.nthreads,
            config.discard_pct,
        );
        let result = run_sample(&config, &mut barrier_group, sched, j);
        if let Err(e) = result {
            barrier_group.shutdown();
            return Err(e);
        } else if !result.unwrap() {
            barrier_group.shutdown();
            break;
        }

        if j != config.samples {
            config
                .backend
                .sleep(Duration::from_secs(config.intersample_sleep));
        }
    }
    Ok(())
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let matches = Command::new("Synthetic Workload Application")
        .version("0.1")
        .arg(
            Arg::new("ADDR")
                .index(1)
                .num_args(1..)
                .value_parser(clap::value_parser!(SocketAddrV4))
                .help("Address and port to listen on")
                .required_unless_present_any(["calibrate", "leader-ip"]),
        )
        .arg(
            Arg::new("conns")
                .short('c')
                .long("conns")
                .value_parser(clap::value_parser!(usize))
                .help("Number of client connections")
                .conflicts_with("threads"),
        )
        .arg(
            Arg::new("threads")
                .short('t')
                .long("threads")
                .value_parser(clap::value_parser!(usize))
                .help("Number of client connections [deprecated param]")
                .hide(true)
                .conflicts_with("conns"),
        )
        .arg(
            Arg::new("discard_pct")
                .long("discard_pct")
                .value_parser(clap::value_parser!(f32))
                .help("Discard first % of packtets at target QPS from sample"),
        )
        .arg(
            Arg::new("mode")
                .short('m')
                .long("mode")
                .value_name("MODE")
                .default_value("runtime-client")
                .help("Which mode to run in"),
        )
        .arg(
            Arg::new("runtime")
                .short('r')
                .long("runtime")
                .value_parser(clap::value_parser!(u64))
                .help("How long the application should run for"),
        )
        .arg(
            Arg::new("mpps")
                .long("mpps")
                .num_args(1)
                .value_parser(clap::value_parser!(f32))
                .help("How many *million* packets should be sent per second"),
        )
        .arg(
            Arg::new("start_mpps")
                .long("start_mpps")
                .num_args(1)
                .value_parser(clap::value_parser!(f32))
                .help("Initial rate to sample at"),
        )
        .arg(
            Arg::new("config")
                .short('c')
                .long("config")
                .num_args(1)
                .required_if_eq("mode", "runtime-client"),
        )
        .arg(
            Arg::new("protocol")
                .short('p')
                .long("protocol")
                .value_name("PROTOCOL")
                .help("Server protocol"),
        )
        .arg(
            Arg::new("warmup")
                .long("warmup")
                .action(clap::ArgAction::SetTrue)
                .help("Run the warmup routine"),
        )
        .arg(
            Arg::new("calibrate")
                .long("calibrate")
                .num_args(1)
                .value_parser(clap::value_parser!(u64))
                .help("us to calibrate fake work for"),
        )
        .arg(
            Arg::new("output")
                .short('o')
                .long("output")
                .value_name("output mode")
                .value_parser(clap::value_parser!(OutputMode))
                .default_value(DEFAULT_OUTPUT_MODE)
                .help("How to display loadgen results"),
        )
        .arg(
            Arg::new("distspec")
                .long("distspec")
                .num_args(1)
                .help("Distribution of request lengths to use, new format")
                .conflicts_with("distribution")
                .conflicts_with("mean"),
        )
        .arg(
            Arg::new("distribution")
                .long("distribution")
                .short('d')
                .num_args(1)
                .help("Distribution of request lengths to use"),
        )
        .arg(
            Arg::new("mean")
                .long("mean")
                .num_args(1)
                .help("Mean number of work iterations per request"),
        )
        .arg(
            Arg::new("leader-ip")
                .long("leader-ip")
                .num_args(1)
                .value_parser(clap::value_parser!(Ipv4Addr))
                .help("IP address of leader instance")
                .conflicts_with("leader"),
        )
        .arg(
            Arg::new("barrier-peers")
                .long("barrier-peers")
                .num_args(1)
                .value_parser(clap::value_parser!(usize))
                .requires("leader")
                .help("Number of connected loadgen instances"),
        )
        .arg(
            Arg::new("leader")
                .long("leader")
                .requires("barrier-peers")
                .action(clap::ArgAction::SetTrue)
                .help("Leader of barrier group"),
        )
        .arg(
            Arg::new("samples")
                .long("samples")
                .num_args(1)
                .value_parser(clap::value_parser!(usize))
                .help("Number of samples to collect"),
        )
        .arg(
            Arg::new("fakework")
                .long("fakework")
                .num_args(1)
                .default_value(DEFAULT_FAKEWORK_SPEC)
                .help("fake worker spec"),
        )
        .arg(
            Arg::new("transport")
                .long("transport")
                .num_args(1)
                .value_parser(clap::value_parser!(Transport))
                .default_value(DEFAULT_TRANSPORT)
                .help("udp or tcp"),
        )
        .arg(
            Arg::new("rampup")
                .long("rampup")
                .num_args(1)
                .value_parser(clap::value_parser!(usize))
                .help("per-sample ramp up seconds"),
        )
        .arg(
            Arg::new("loadshift")
                .long("loadshift")
                .num_args(1)
                .help("loadshift spec"),
        )
        .arg(
            Arg::new("live")
                .long("live")
                .action(clap::ArgAction::SetTrue)
                .help("run live mode"),
        )
        .arg(
            Arg::new("closed_bench")
                .long("closed_bench")
                .action(clap::ArgAction::SetTrue)
                .help("used close loop tput benchmark routine"),
        )
        .arg(
            Arg::new("depth")
                .long("depth")
                .num_args(1)
                .value_parser(clap::value_parser!(usize))
                .help("depth for closed loop benchmark"),
        )
        .arg(
            Arg::new("intersample_sleep")
                .long("intersample_sleep")
                .num_args(1)
                .value_parser(clap::value_parser!(u64))
                .help("seconds to sleep between samples"),
        )
        .arg(
            Arg::new("runtime_stat")
                .long("runtime_stat")
                .action(clap::ArgAction::SetTrue)
                .help("run runtime stat"),
        )
        .args(&SyntheticProtocol::args())
        .args(&MemcachedProtocol::args())
        .args(&DnsProtocol::args())
        .args(&ReflexProtocol::args())
        .args(&HttpProtocol::args())
        .get_matches();

    let cfg_file = matches.get_one::<String>("config").map(|s| s.to_string());
    let leader_ip: Option<Ipv4Addr> = matches.get_one::<Ipv4Addr>("leader-ip").copied();

    if let Some(leader_ip) = leader_ip {
        Backend::Runtime.init_and_run(cfg_file.as_deref(), move || loop {
            match lockstep::Group::create_client(leader_ip, Backend::Runtime) {
                Ok((mut barrier_group, config)) => {
                    let _ = run_multistep_client(config, &mut barrier_group);
                }
                Err(_) => {
                    println!("Waiting for leader to become available");
                    Backend::Runtime.sleep(Duration::from_secs(1));
                }
            }
        });
        return Ok(());
    }

    let config = AppConfig::from_matches(&matches)?;

    if let Some(us) = config.calibrate {
        config
            .backend
            .clone()
            .init_and_run(cfg_file.as_deref(), move || {
                let barrier = Arc::new(AtomicUsize::new(config.nthreads));
                let join_handles: Vec<_> = (0..config.nthreads)
                    .map(|_| {
                        let fakeworker = config.fakeworker();
                        let barrier = barrier.clone();
                        config.backend.spawn_thread(move || {
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
        return Ok(());
    }

    match config.mode {
        RunMode::SpawnerServer => match config.transport {
            Transport::Udp => config
                .backend
                .clone()
                .init_and_run(cfg_file.as_deref(), move || {
                    server::run_spawner_server(config.addrs[0], &config.fakework_spec)
                }),
            Transport::Tcp => config
                .backend
                .clone()
                .init_and_run(cfg_file.as_deref(), move || server::run_tcp_server(config)),
        },
        RunMode::LinuxServer => match config.transport {
            Transport::Udp => config
                .backend
                .clone()
                .init_and_run(cfg_file.as_deref(), move || {
                    server::run_linux_udp_server(config)
                }),
            Transport::Tcp => config
                .backend
                .clone()
                .init_and_run(cfg_file.as_deref(), move || server::run_tcp_server(config)),
        },
        RunMode::LocalClient => {
            config
                .backend
                .clone()
                .init_and_run(cfg_file.as_deref(), move || {
                    do_run_local(config).unwrap();
                })
        }
        RunMode::RuntimeClient => {
            config
                .backend
                .clone()
                .init_and_run(cfg_file.as_deref(), move || {
                    let (mut barrier_group, config) =
                        lockstep::Group::create_server(config).unwrap();
                    run_multistep_client(config, &mut barrier_group).unwrap();
                });
        }
    };

    Ok(())
}
