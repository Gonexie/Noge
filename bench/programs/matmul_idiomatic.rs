fn main() {
    let mut a = [0u32; 250000];
    let mut b = [0u32; 250000];
    let mut c = [0u64; 250000];
    let mut x: u32 = 12345;
    for i in 0..250000 {
        x = x.wrapping_mul(1664525).wrapping_add(1013904223);
        a[i] = (x >> 16) % 100;
        x = x.wrapping_mul(1664525).wrapping_add(1013904223);
        b[i] = (x >> 16) % 100;
    }
    for (crow, arow) in c.chunks_mut(500).zip(a.chunks(500)) {
        for (&aik, brow) in arow.iter().zip(b.chunks(500)) {
            let aik = aik as u64;
            for (cj, &bj) in crow.iter_mut().zip(brow.iter()) {
                *cj += aik * bj as u64;
            }
        }
    }
    let sum: u64 = c.iter().map(|&v| v % 1000).sum();
    println!("{}", (sum % 2000000011) as i32);
}
