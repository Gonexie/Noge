fn main() {
    let mut best_n: u64 = 1;
    let mut best_steps: u64 = 0;
    let mut n: u64 = 1;
    while n <= 3000000 {
        let mut x: u64 = n;
        let mut steps: u64 = 0;
        while x != 1 {
            if x % 2 == 0 { x = x / 2; } else { x = 3 * x + 1; }
            steps = steps + 1;
        }
        if steps > best_steps { best_steps = steps; best_n = n; }
        n = n + 1;
    }
    println!("{}", best_n as i32);
    println!("{}", best_steps as i32);
}
