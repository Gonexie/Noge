fn main() {
    let mut a = [0u32; 250000];
    let mut b = [0u32; 250000];
    let mut c = [0u64; 250000];
    let mut x: u32 = 12345;
    let mut i: usize = 0;
    while i < 250000 {
        x = x.wrapping_mul(1664525).wrapping_add(1013904223);
        a[i] = (x >> 16) % 100;
        x = x.wrapping_mul(1664525).wrapping_add(1013904223);
        b[i] = (x >> 16) % 100;
        i = i + 1;
    }
    i = 0;
    while i < 500 {
        let mut k: usize = 0;
        while k < 500 {
            let aik = a[i * 500 + k] as u64;
            let mut j: usize = 0;
            while j < 500 {
                c[i * 500 + j] = c[i * 500 + j] + aik * (b[k * 500 + j] as u64);
                j = j + 1;
            }
            k = k + 1;
        }
        i = i + 1;
    }
    let mut sum: u64 = 0;
    i = 0;
    while i < 250000 {
        sum = sum + c[i] % 1000;
        i = i + 1;
    }
    println!("{}", (sum % 2000000011) as i32);
}
