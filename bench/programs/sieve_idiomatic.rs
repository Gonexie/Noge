fn main() {
    let mut is_prime = [true; 20000001];
    is_prime[0] = false;
    is_prime[1] = false;
    let mut i: usize = 2;
    while i * i <= 20000000 {
        if is_prime[i] {
            for p in is_prime[i * i..].iter_mut().step_by(i) {
                *p = false;
            }
        }
        i += 1;
    }
    let count = is_prime.iter().filter(|&&p| p).count() as u32;
    println!("{}", count as i32);
}
