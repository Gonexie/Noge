#[derive(Clone, Copy)]
enum Op { AddI(i64), MulI(i64), ModI(i64), XorC, DecJnz(usize), Halt }

fn main() {
    let code: [Op; 6] = [Op::AddI(7), Op::MulI(3), Op::ModI(1000003), Op::XorC, Op::DecJnz(0), Op::Halt];
    let mut acc: i64 = 1;
    let mut counter: i64 = 40000000;
    let mut pc: usize = 0;
    let mut running = true;
    while running {
        match code[pc] {
            Op::AddI(v) => { acc = acc + v; pc = pc + 1; }
            Op::MulI(v) => { acc = acc * v; pc = pc + 1; }
            Op::ModI(v) => { acc = acc % v; pc = pc + 1; }
            Op::XorC => { acc = acc ^ counter; pc = pc + 1; }
            Op::DecJnz(target) => {
                counter = counter - 1;
                if counter != 0 { pc = target; } else { pc = pc + 1; }
            }
            Op::Halt => { running = false; }
        }
    }
    println!("{}", acc as i32);
    println!("{}", counter as i32);
}
