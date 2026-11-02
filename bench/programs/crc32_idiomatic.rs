fn main() {
    let mut table = [0u32; 256];
    for n in 0..256usize {
        let mut c = n as u32;
        for _ in 0..8 {
            c = if c & 1 == 1 { (c >> 1) ^ 0xEDB8_8320 } else { c >> 1 };
        }
        table[n] = c;
    }
    let mut buf = [0u8; 16777216];
    let mut x: u32 = 99;
    for b in buf.iter_mut() {
        x = x.wrapping_mul(1664525).wrapping_add(1013904223);
        *b = (x >> 24) as u8;
    }
    let mut crc: u32 = 0xFFFF_FFFF;
    for _ in 0..8 {
        for &b in buf.iter() {
            crc = table[((crc ^ b as u32) & 255) as usize] ^ (crc >> 8);
        }
    }
    println!("{}", (crc ^ 0xFFFF_FFFF) as i32);
}
