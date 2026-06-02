#![feature(alloc_error_handler)]
#![no_std]
#![no_main]

extern crate alloc;
// use talc::*;
// use alloc::boxed::Box;
// use alloc::vec::Vec;

use alloc::{boxed::Box, vec::Vec};
use core::panic::PanicInfo;
use core::arch::naked_asm;
use core::fmt::{self, Write};
use core::alloc::{GlobalAlloc, Layout};
use core::alloc::Layout;

struct ZephyrAlloc;

unsafe impl GlobalAlloc for ZephyrAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        extern "C" {
            // Zephyr's standard kernel malloc
            fn k_malloc(size: usize) -> *mut u8;
        }
        k_malloc(layout.size())
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        extern "C" {
            fn k_free(ptr: *mut u8);
        }
        k_free(ptr);
    }
}

#[global_allocator]
static ALLOCATOR: ZephyrAlloc = ZephyrAlloc;

#[alloc_error_handler]
fn oom(layout: Layout) -> ! {
    panic!("alloc failed: size={} align={}", layout.size(), layout.align());
}


// #[link_section = ".bss.heap"]
// static mut HEAP: [u8; 16 * 1024] = [0; 16 * 1024]; 

// #[global_allocator]
// static ALLOCATOR: Talck<spin::Mutex<()>, ClaimOnOom> = Talc::new(unsafe {
//     // if we're in a hosted environment, the Rust runtime may allocate before
//     // main() is called, so we need to initialize the arena automatically
//     ClaimOnOom::new(Span::from_array(core::ptr::addr_of!(HEAP).cast_mut()))
// }).lock();

use wasmtime::*;

// static TRAP: [u8; 7] = *b"trap!\n\0";

// addition function
// const CWASM_ADD: &[u8] = include_bytes!("/home/jerryfen/wasmtime-rr-prototyping/pulley_exp_wasmtime_with_std/add.cwasm");

// division function
// const CWASM_DIV: &[u8] = include_bytes!("/home/jerryfen/wasmtime-rr-prototyping/pulley_exp_wasmtime_with_std/divide_prog/divide.cwasm");


#[no_mangle]
pub extern "C" fn wasmtime_tls_get() -> *mut core::ffi::c_void {
    static mut DUMMY: [u8; 1024] = [0; 1024];
    unsafe { DUMMY.as_mut_ptr() as *mut core::ffi::c_void }
}

#[no_mangle]
pub extern "C" fn wasmtime_tls_set(_ptr: *mut core::ffi::c_void) {}



// 1. Create a dummy struct to implement the Write trait
struct ZephyrConsole;

impl Write for ZephyrConsole {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        extern "C" {
            fn printk(fmt: *const u8, ...);
        }
        for chunk in s.as_bytes().chunks(128) {
            // We use a format string "%s" to safely print the Rust string slice
            unsafe {
                printk("%.*s\0".as_ptr(), chunk.len() as i32, chunk.as_ptr());
            }
        }
        Ok(())
    }
}

// 2. Create a println macro for your crate
#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => {{
        let mut console = ZephyrConsole;
        let _ = core::fmt::write(&mut console, format_args!($($arg)*));
    }};
}

#[macro_export]
macro_rules! println {
    ($($arg:tt)*) => {{
        print!($($arg)*);
        print!("\n");
    }};
}
// #[no_mangle]
// pub unsafe extern "C" fn __wrap_malloc(size: usize) -> *mut u8 {
//     // Call Talc's allocation logic here
//     let layout = Layout::from_size_align(1243, 8).unwrap();
//     let a = unsafe { ALLOCATOR.lock().malloc(layout) };
//     a
// }

// #[no_mangle]
// pub unsafe extern "C" fn __wrap_free(ptr: *mut u8) {
//     // Call Talc's free logic here
// }

// QEMU riscv virt: ns16550a UART at 0x1000_0000
const UART_BASE: usize = 0x1000_0000;
const UART_THR: *mut u8 = (UART_BASE + 0x00) as *mut u8;
const UART_LSR: *const u8 = (UART_BASE + 0x05) as *const u8;
const LSR_THRE: u8 = 0x20;

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    println!("PANIC");
    // if let Some(loc) = info.location() {
    //     uart_write_bytes(loc.file().as_bytes());
    //     uart_putc(b':');
    //     uart_put_dec_u32(loc.line());
    // } else {
    //     uart_puts(b"(no loc)");
    // }

    loop {}
}

module
#[unsafe(no_mangle)]
pub extern "C" fn rust_main() -> ! {  
    
    let mut config = Config::new();
    // config.wasm_gc(false);
    config.gc_support(false);
    config.target("pulley64").unwrap();
    config.memory_init_cow(false);
    config.signals_based_traps(false);
    // config.max_wasm_stack(8 * 1024);

    let engine = Engine::new(&config).unwrap();

    let mut store = Store::new(&engine, ());

    // 1. Deserialize (NO COMPILATION)
    let module = unsafe {
        Module::deserialize(&engine, CWASM_ADD).unwrap()
    };

    // 2. Instantiate
    let instance = Instance::new(&mut store, &module, &[]).unwrap();

    // 3. Call function
    let add = instance
        .get_typed_func::<(i32, i32), i32>(&mut store, "add")
        .unwrap();
    // let divide = instance
    //     .get_typed_func::<(i32, i32), i32>(&mut store, "divide")
    //     .unwrap();

    let result = add.call(&mut store, (3, 5)).unwrap();
    // for divide, it is (getting divided, doing dividing)
    // let result = divide.call(&mut store, (6, 0)).unwrap();

    // 4. Signal success
    if result == 8 {
        // panic!("panic but the good kind");
        println!("add worked!\n")
    }

    loop {} 

}

