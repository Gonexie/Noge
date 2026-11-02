fn main() {
    let mut s = [0u8; 15000];
    let mut t = [0u8; 15000];
    let mut x: u32 = 7;
    let mut i: usize = 0;
    while i < 15000 {
        x = x.wrapping_mul(1664525).wrapping_add(1013904223);
        s[i] = ((x >> 24) % 4) as u8;
        x = x.wrapping_mul(1664525).wrapping_add(1013904223);
        t[i] = ((x >> 24) % 4) as u8;
        i = i + 1;
    }
    let mut rows = [0u32; 30002];
    let mut prev: usize = 0;
    let mut cur: usize = 15001;
    i = 1;
    while i <= 15000 {
        rows[cur] = 0;
        let mut j: usize = 1;
        while j <= 15000 {
            if s[i - 1] == t[j - 1] {
                rows[cur + j] = rows[prev + j - 1] + 1;
            } else if rows[prev + j] >= rows[cur + j - 1] {
                rows[cur + j] = rows[prev + j];
            } else {
                rows[cur + j] = rows[cur + j - 1];
            }
            j = j + 1;
        }
        let tmp = prev;
        prev = cur;
        cur = tmp;
        i = i + 1;
    }
    println!("{}", rows[prev + 15000] as i32);
}
