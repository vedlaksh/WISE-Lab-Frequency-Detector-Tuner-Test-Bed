#![no_std]
#![no_main]

use core::arch::naked_asm;
use core::panic::PanicInfo;

// Rust now requires unsafe extern blocks.
unsafe extern "C" {
    static _stack_end: u8;
}

// QEMU riscv virt: ns16550a UART at 0x1000_0000
const UART_BASE: usize = 0x1000_0000;
const UART_THR: *mut u8 = (UART_BASE + 0x00) as *mut u8;
const UART_LSR: *const u8 = (UART_BASE + 0x05) as *const u8;
const LSR_THRE: u8 = 0x20;

#[inline(always)]
fn uart_putc(c: u8) {
    unsafe {
        while (core::ptr::read_volatile(UART_LSR) & LSR_THRE) == 0 {}
        core::ptr::write_volatile(UART_THR, c);
    }
}

// Stack-free NUL-terminated byte string printer.
fn uart_puts_bytes(mut p: *const u8) {
    unsafe {
        loop {
            let b = core::ptr::read_volatile(p);
            if b == 0 {
                break;
            }
            if b == b'\n' {
                uart_putc(b'\r');
            }
            uart_putc(b);
            p = p.add(1);
        }
    }
}


#[unsafe(no_mangle)]
// #[unsafe(naked)]
pub extern "C" fn rust_main() -> ! { 
	/*
	naked_asm!(
            // ".option norvc",     // forbid compressed instructions
            // ".option norelax",   // forbid linker relaxation in this block

            "la sp, _stack_end",
            "la gp, __global_pointer$",

            "li t1, 0x10000000",

            "1: lbu  t2, 5(t1)",
            "andi t2, t2, 0x20",
            "beqz t2, 1b",
            "li   t3, 'R'",
            "sb   t3, 0(t1)",
	);
	*/
	uart_putc(b'H');
	uart_putc(b'e');
	uart_putc(b'l');
	uart_putc(b'l');
	uart_putc(b'o');
	uart_putc(b'!');
    uart_putc(b'\n');
	loop {} 
}

static BOOT: [u8; 50] = *b"booted: hello from riscv32 rust + qemu virt uart\n\0";
static TRAP: [u8; 7] = *b"trap!\n\0";

#[unsafe(no_mangle)]
#[unsafe(naked)]
pub extern "C" fn _start() -> ! {
    unsafe {
        naked_asm!(
            // ".option norvc",     // forbid compressed instructions
            // ".option norelax",   // forbid linker relaxation in this block

            "la sp, _stack_end",
            "la gp, __global_pointer$",
            
            "la t0, trap_entry",
            "csrw mtvec, t0",
            
            "la  t0, _sbss",
            "la  t1, _ebss",
            "li  t2, 0",
            "1:",
            "beq t0, t1, 2f",
            "sw  t2, 0(t0)",
            "addi t0, t0, 4",
            "j   1b",
            "2:",

            "li t1, 0x10000000",

            "3: lbu  t2, 5(t1)",
            "andi t2, t2, 0x20",
            "beqz t2, 3b",
            "li   t3, 'O'",
            "sb   t3, 0(t1)",

            "4: lbu  t2, 5(t1)",
            "andi t2, t2, 0x20",
            "beqz t2, 4b",
            "li   t3, 'K'",
            "sb   t3, 0(t1)",

            "5: lbu  t2, 5(t1)",
            "andi t2, t2, 0x20",
            "beqz t2, 5b",
            "li   t3, 10",
            "sb   t3, 0(t1)",

            "j rust_main",
        );
    }
}


#[unsafe(no_mangle)]
#[unsafe(naked)]
pub extern "C" fn trap_entry() -> ! {
    unsafe {
        naked_asm!(
            "j {trap_rust}",
            trap_rust = sym trap_rust,
        );
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn trap_rust() -> ! {
    uart_puts_bytes(TRAP.as_ptr());
    loop {}
}

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    loop {}
}

