fn main() {
    let mut table = [0u32; 256];
    let mut n: usize = 0;
    while n < 256 {
        let mut c: u32 = n as u32;
        let mut k: u32 = 0;
        while k < 8 {
            if c & 1 == 1 { c = (c >> 1) ^ 0xEDB8_8320; } else { c = c >> 1; }
            k = k + 1;
        }
        table[n] = c;
        n = n + 1;
    }
    let mut buf = [0u8; 16777216];
    let mut x: u32 = 99;
    let mut i: usize = 0;
    while i < 16777216 {
        x = x.wrapping_mul(1664525).wrapping_add(1013904223);
        buf[i] = (x >> 24) as u8;
        i = i + 1;
    }
    let mut crc: u32 = 0xFFFF_FFFF;
    let mut pass: u32 = 0;
    while pass < 8 {
        i = 0;
        while i < 16777216 {
            crc = table[((crc ^ (buf[i] as u32)) & 255) as usize] ^ (crc >> 8);
            i = i + 1;
        }
        pass = pass + 1;
    }
    println!("{}", (crc ^ 0xFFFF_FFFF) as i32);
}
