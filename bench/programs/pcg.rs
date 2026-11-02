struct Rng { state: u64, inc: u64 }

fn step(r: &mut Rng) {
    r.state = r.state.wrapping_mul(6364136223846793005).wrapping_add(r.inc);
}

fn seed_stream(initstate: u64, initseq: u64) -> Rng {
    let mut r = Rng { state: 0, inc: (initseq << 1) | 1 };
    step(&mut r);
    r.state = r.state.wrapping_add(initstate);
    step(&mut r);
    r
}

fn next(r: &mut Rng) -> u32 {
    let old = r.state;
    step(r);
    let xorshifted = (((old >> 18) ^ old) >> 27) as u32;
    let rot = (old >> 59) as u32;
    (xorshifted >> rot) | (xorshifted << ((32 - rot) & 31))
}

fn main() {
    let mut bins = [0u32; 1024];
    let mut r = seed_stream(42, 54);
    let mut n: u32 = 0;
    while n < 200000000 {
        let v = next(&mut r);
        let b = (v >> 22) as usize;
        bins[b] = bins[b] + 1;
        n = n + 1;
    }
    let mut chk: u32 = 0;
    let mut i: usize = 0;
    while i < 1024 {
        chk = chk.wrapping_mul(31).wrapping_add(bins[i]);
        i = i + 1;
    }
    println!("{}", chk as i32);
    println!("{}", bins[0] as i32);
}
