extern crate test;

use std::result::Result;

extern crate mersenne_twister;
extern crate rand;
use mersenne_twister::MersenneTwister;
use rand::{Rng, SeedableRng};

pub enum FakeWorker {
    Sqrt,
    StridedMem(Vec<u8>, usize),
    RandomMem(Vec<u8>, Vec<usize>),
    StreamingMem(Vec<u8>),
}

impl FakeWorker {
    pub fn create(spec: &str) -> Result<Self, &str> {
        let seed: u64 = rand::thread_rng().gen();
        let mut rng: MersenneTwister = SeedableRng::from_seed(seed);

        let tokens: Vec<&str> = spec.split(":").collect();
        assert!(tokens.len() > 0);

        match tokens[0] {
            "sqrt" => Ok(FakeWorker::Sqrt),
            "stridedmem" | "randmem" | "memstream" => {
                assert!(tokens.len() > 1);
                let size: usize = tokens[1].parse().unwrap();
                let buf = (0..size).map(|_| rng.gen()).collect();
                match tokens[0] {
                    "stridedmem" => {
                        assert!(tokens.len() > 2);
                        let stride: usize = tokens[2].parse().unwrap();
                        Ok(FakeWorker::StridedMem(buf, stride))
                    }
                    "randmem" => {
                        let sched = (0..size).map(|_| rng.gen::<usize>() % size).collect();
                        Ok(FakeWorker::RandomMem(buf, sched))
                    }
                    "memstream" => Ok(FakeWorker::StreamingMem(buf)),
                    _ => unreachable!(),
                }
            }
            _ => Err("bad fakework spec"),
        }
    }

    pub fn work(&self, iters: u64) {
        match *self {
            FakeWorker::Sqrt => {
                let k = 2350845.545;
                for i in 0..iters {
                    test::black_box(f64::sqrt(k * i as f64));
                }
            }
            FakeWorker::StridedMem(ref buf, stride) => {
                for i in 0..iters as usize {
                    test::black_box::<u8>(buf[(i * stride) % buf.len()]);
                }
            }
            FakeWorker::RandomMem(ref buf, ref sched) => {
                for i in 0..iters as usize {
                    test::black_box::<u8>(buf[sched[i % sched.len()]]);
                }
            }
            FakeWorker::StreamingMem(ref buf) => {
                for _ in 0..iters {
                    for i in (0..buf.len()).step_by(64) {
                        test::black_box::<u8>(buf[i]);
                    }
                }
            }
        }
    }
}
