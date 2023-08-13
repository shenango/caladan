use rand::Rng;
use rand_distr::{Distribution as DistR, Exp};

#[derive(Copy, Clone, Debug)]
pub enum Distribution {
    Zero,
    Constant(u64),
    Exponential(f64),
    Bimodal(f64, u64, u64),
}

impl Distribution {
    pub fn name(&self) -> &'static str {
        match *self {
            Distribution::Zero => "zero",
            Distribution::Constant(_) => "constant",
            Distribution::Exponential(_) => "exponential",
            Distribution::Bimodal(_, _, _) => "bimodal",
        }
    }
    pub fn sample<R: Rng>(&self, rng: &mut R) -> u64 {
        match *self {
            Distribution::Zero => 0,
            Distribution::Constant(m) => m,
            Distribution::Exponential(m) => Exp::new(1.0 / m).unwrap().sample(rng) as u64,
            Distribution::Bimodal(p, v1, v2) => {
                if rng.gen_bool(p) {
                    v1
                } else {
                    v2
                }
            }
        }
    }

    pub fn create(spec: &str) -> Result<Self, &str> {
        let tokens: Vec<&str> = spec.split(":").collect();
        assert!(tokens.len() > 0);
        match tokens[0] {
            "zero" => Ok(Distribution::Zero),
            "constant" => {
                assert!(tokens.len() == 2);
                let val: u64 = tokens[1].parse().unwrap();
                Ok(Distribution::Constant(val))
            }
            "exponential" => {
                assert!(tokens.len() == 2);
                let val: f64 = tokens[1].parse().unwrap();
                Ok(Distribution::Exponential(val))
            }
            "bimodal" => {
                assert!(tokens.len() == 4);
                let prob: f64 = tokens[1].parse().unwrap();
                let val1: u64 = tokens[2].parse().unwrap();
                let val2: u64 = tokens[3].parse().unwrap();
                Ok(Distribution::Bimodal(prob, val1, val2))
            }
            _ => Err("bad distribution spec"),
        }
    }
}
