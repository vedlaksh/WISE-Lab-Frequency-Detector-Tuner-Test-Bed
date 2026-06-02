#![feature(alloc_error_handler)]
#![no_std]
#![no_main]

extern crate alloc;

use wasmtime::*

use zephyr::raw::ZR_GPIO_OUTPUT_ACTIVE;
use zephyr::time::{sleep, Duration};
use zephyr::drivers::gpio;

use alloc::{boxed::Box, vec::Vec};
use core::panic::PanicInfo;
use core::arch::naked_asm;
use core::fmt::{self, Write};
use core::alloc::{GlobalAlloc, Layout};



#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct GpioDtSpec {
    pub port: *const core::ffi::c_void, // 4 bytes
    pub pin: u32,                       // 4 bytes
    pub extra_flags: u32,           // 4 bytes
}

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

use wasmtime::*;

static TRAP: [u8; 7] = *b"trap!\n\0";

// addition function
const CWASM_BLINKY: &[u8] = include_bytes!("/home/jerryfen/zephyrproject/zephyr/apps/blinky/src/blinky.cwasm");

#[no_mangle]
pub extern "C" fn wasmtime_tls_get() -> *mut core::ffi::c_void {
    static mut DUMMY: [u8; 1024] = [0; 1024];
    unsafe { DUMMY.as_mut_ptr() as *mut core::ffi::c_void }
}

#[no_mangle]
pub extern "C" fn wasmtime_tls_set(_ptr: *mut core::ffi::c_void) {}

pub extern "C" fn gpio_is_ready_dt(spec: *const GpioDtSpec) -> i32;
extern "C" {
    pub fn gpio_pin_configure_dt(
        spec: *const GpioDtSpec, 
        extra_flags: u32
    ) -> i32;
}
pub extern "C" fn gpio_pin_toggle_dt(spec: *const GpioDtSpec) -> i32;
pub extern "C" fn k_msleep(ms: i32) -> i32;



#[repr(C)]
pub struct GpioDtSpec {
    pub port: *const core::ffi::c_void, 
    pub pin: u32,
    pub dt_flags: u32,
}

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

fn create_zephyr_linker(engine: &Engine) -> Linker<HostState> {
    let mut linker = Linker::new(engine);

    // --- Link: gpio_is_ready_dt ---
    linker.func_wrap("zephyr", "gpio_is_ready_dt", 
        |mut caller: Caller<'_, HostState>, spec_ptr: u32| -> i32 {
            let mem = caller.get_export("memory").unwrap().into_memory().unwrap();
            let host_ptr = unsafe { mem.data_ptr(&caller).add(spec_ptr as usize) as *const GpioDtSpec };
            
            unsafe {
                gpio_is_ready_dt(host_ptr)
            }
        }
    ).unwrap();

    // --- Link: gpio_pin_configure_dt ---
    linker.func_wrap("zephyr", "gpio_pin_configure_dt", 
        |mut caller: Caller<'_, HostState>, spec_ptr: u32, extra_flags: u32| -> i32 {
            let mem = caller.get_export("memory").unwrap().into_memory().unwrap();
            let host_ptr = unsafe { mem.data_ptr(&caller).add(spec_ptr as usize) as *const GpioDtSpec };
            
            unsafe {
                // Call the actual Zephyr C function (or your proxy)
                gpio_pin_configure_dt(host_ptr, extra_flags)
            }
        }
    ).unwrap();

    // --- Link: gpio_pin_toggle_dt ---
    linker.func_wrap("zephyr", "gpio_pin_toggle_dt", 
        |mut caller: Caller<'_, HostState>, spec_ptr: u32| -> i32 {
            let mem = caller.get_export("memory").unwrap().into_memory().unwrap();
            let host_ptr = unsafe { mem.data_ptr(&caller).add(spec_ptr as usize) as *const GpioDtSpec };
            
            unsafe {
                gpio_pin_toggle_dt(host_ptr)
            }
        }
    ).unwrap();

    // --- Link: k_msleep ---
    linker.func_wrap("zephyr", "k_msleep", |_caller: Caller<'_, HostState>, ms: i32| -> i32 {
        unsafe {
            k_msleep(ms)
        }
    }).unwrap();

    linker
}

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
        Module::deserialize(&engine, CWASM_BLINKY).unwrap()
    };

    let linker = create_zephyr_linker(&engine);

    // 2. Instantiate
    // let instance = Instance::new(&mut store, &module, &[]).unwrap();
    
    let instance = linker.instantiate(&mut store, &module)?;

    // 3. Call function
    let blink = instance
        .get_typed_func::<(), i32>(&mut store, "blink")
        .unwrap();

    let result = blink.call(&mut store, ()).unwrap();

    // 4. Signal success
    // if result == 8 {
    //     // panic!("panic but the good kind");
    //     println!("add worked!\n")
    // }

    loop {} 

}

