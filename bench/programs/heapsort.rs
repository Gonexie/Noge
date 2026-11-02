fn sift_down(a: &mut [u32; 1000000], start: usize, end: usize) {
    let mut root = start;
    while 2 * root + 1 < end {
        let mut child = 2 * root + 1;
        if child + 1 < end && a[child] < a[child + 1] { child = child + 1; }
        if a[root] < a[child] {
            let t = a[root];
            a[root] = a[child];
            a[child] = t;
            root = child;
        } else {
            return;
        }
    }
}

fn main() {
    let mut a = [0u32; 1000000];
    let mut x: u32 = 2024;
    let mut i: usize = 0;
    while i < 1000000 {
        x = x.wrapping_mul(1664525).wrapping_add(1013904223);
        a[i] = x;
        i = i + 1;
    }
    let mut start: usize = 1000000 / 2;
    while start > 0 {
        start = start - 1;
        sift_down(&mut a, start, 1000000);
    }
    let mut end: usize = 1000000;
    while end > 1 {
        end = end - 1;
        let t = a[0];
        a[0] = a[end];
        a[end] = t;
        sift_down(&mut a, 0, end);
    }
    let mut sorted: u32 = 1;
    let mut sum: u64 = 0;
    i = 0;
    while i < 1000000 {
        if i > 0 && a[i] < a[i - 1] { sorted = 0; }
        sum = sum + (a[i] as u64) * ((i % 1000) as u64);
        i = i + 1;
    }
    println!("{}", sorted as i32);
    println!("{}", (sum % 2000000011) as i32);
}
