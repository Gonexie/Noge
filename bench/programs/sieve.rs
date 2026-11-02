fn main() {
    let mut is_prime = [true; 20000001];
    is_prime[0] = false;
    is_prime[1] = false;
    let mut i: usize = 2;
    while i * i <= 20000000 {
        if is_prime[i] {
            let mut j: usize = i * i;
            while j <= 20000000 {
                is_prime[j] = false;
                j = j + i;
            }
        }
        i = i + 1;
    }
    let mut count: u32 = 0;
    let mut k: usize = 0;
    while k <= 20000000 {
        if is_prime[k] { count = count + 1; }
        k = k + 1;
    }
    println!("{}", count as i32);
}
