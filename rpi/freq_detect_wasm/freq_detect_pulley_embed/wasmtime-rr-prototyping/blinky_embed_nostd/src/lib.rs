#![feature(alloc_error_handler)]
#![no_std]
#![no_main]

extern crate alloc;

use wasmtime::*;
// struct HostState;
use wasmtime::component::{bindgen, Component, Linker};
// use zephyr::raw::ZR_GPIO_OUTPUT_ACTIVE;
// use zephyr::time::{sleep, Duration};
// use zephyr::drivers::gpio;

use alloc::{boxed::Box, vec::Vec};
use core::panic::PanicInfo;
use core::arch::naked_asm;
use core::fmt::{self, Write};
use core::alloc::{GlobalAlloc, Layout};
use core::ptr::{self, NonNull};
use core::option::Option::Some;
use core::any::Any;
use core::ffi::c_void;

bindgen!("host" in "/home/jerryfen/zephyrproject/zephyr/apps/blinky/wasm_component/blinky.wit");

struct ZephyrAlloc;


struct Uart;
// use core::fmt;

unsafe extern "C" {
    fn printk(fmt: *const u8, ...);
}

pub struct BufWriter; 

impl BufWriter {
    pub fn new() -> Self {
        Self
    }
}

impl RecordWriter for BufWriter
{

    fn write(&mut self, mut data: &[u8]) -> Result<usize, Error> {

        for &b in data {
            unsafe {
                // printk(c"%c".as_ptr(), b as i32);
                printk(c"%02x".as_ptr(), b as i32);
            }
        }
        Ok(data.len())
    }

    fn flush(&mut self) -> Result<(), Error> {
        Ok(())
    }
}

unsafe impl GlobalAlloc for ZephyrAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        extern "C" {
            fn k_malloc(size: usize) -> *mut u8;
            fn k_aligned_alloc(align: usize, size: usize) -> *mut u8;
        }

        // Rust requires non-null for zero-sized allocations
        if layout.size() == 0 {
            return NonNull::<u8>::dangling().as_ptr();
        }

        // k_malloc is only pointer-size aligned; use aligned alloc when needed
        let align = layout.align();
        let ptr = if align <= core::mem::align_of::<usize>() {
            k_malloc(layout.size())
        } else {
            k_aligned_alloc(align, layout.size())
        };

        ptr
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        extern "C" {
            fn k_free(ptr: *mut u8);
        }

        // It's OK to ignore layout; Zephyr frees either way.
        // Also, Rust may pass dangling for size==0; freeing it would be wrong.
        if layout.size() != 0 {
            k_free(ptr);
        }
    }
}

#[global_allocator]
static ALLOCATOR: ZephyrAlloc = ZephyrAlloc;

// static TRAP: [u8; 7] = *b"trap!\n\0";

// // addition function
// const CWASM_BLINKY: &[u8] = include_bytes!("/home/jerryfen/zephyrproject/zephyr/apps/blinky/wasm_component/blinky.cwasm");

#[repr(align(16))]
struct AlignedResource {
    // data: [u8; 37456],
    // blinky byte size
    // data: [u8; 5248],
    // blinky_5times byte size
    data: [u8; 5520],
}

// normal blinky
// static COMP_ADD: AlignedResource = AlignedResource {
//     // data: *include_bytes!("/home/jerryfen/wasmtime-rr-prototyping/comp_check_guest/comp_check_guest.cwasm"),
//     data: *include_bytes!("/home/jerryfen/zephyrproject/zephyr/apps/blinky/wasm_component/blinky.component.rr.cwasm"),
// };

// blinky_5times
static COMP_ADD: AlignedResource = AlignedResource {
    // data: *include_bytes!("/home/jerryfen/zephyrproject/zephyr/apps/blinky_5times/wasm_component/blinky_5times.component.rr.cwasm"),
    data: *include_bytes!("/home/jerryfen/zephyrproject/zephyr/apps/blinky_5times/blink101_wasm_component/blink101.component.rr.cwasm"),
};


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

#[alloc_error_handler]
fn oom(layout: Layout) -> ! {
    println!("alloc failed: size={} align={}", layout.size(), layout.align());
    // panic!();
    loop {}
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    println!("PANIC");
    if let Some(loc) = info.location() {
        println!("file: {}", loc.file());
        println!("line: {}", loc.line());
    } else {
        println!("(no loc)");
    }

    // if let Some(loc) = info.location() {
    //     uart_write_bytes(loc.file().as_bytes());
    //     uart_putc(b':');
    //     uart_put_dec_u32(loc.line());
    // } else {
    //     uart_puts(b"(no loc)");
    // }

    loop {}
}

extern "C" {
    fn host_pin_init() -> i32;
    fn host_pin_toggle_dt() -> i32;
    fn host_dev_msleep(ms: i32) -> i32;
}

// module only

// fn create_zephyr_linker(engine: &Engine) -> Linker<HostState> {
//     let mut linker = Linker::new(engine);

//     // --- Link: host_pin_init ---
//     linker.func_wrap("zephyr", "host_pin_init", 
//         |mut caller: Caller<'_, HostState>| -> i32 {
//             let mem = caller.get_export("memory").unwrap().into_memory().unwrap();
//             // let host_ptr = unsafe { mem.data_ptr(&caller).add(spec_ptr as usize) as *const GpioDtSpec };
            
//             unsafe {
//                 host_pin_init()
//             }
//         }
//     ).unwrap();

//     // --- Link: host_pin_toggle_dt ---
//     linker.func_wrap("zephyr", "host_pin_toggle_dt", 
//         |mut caller: Caller<'_, HostState>| -> i32 {
//             let mem = caller.get_export("memory").unwrap().into_memory().unwrap();
//             // let host_ptr = unsafe { mem.data_ptr(&caller).add(spec_ptr as usize) as *const GpioDtSpec };
            
//             unsafe {
//                 // Call the actual Zephyr C function (or your proxy)
//                 host_pin_toggle_dt()
//             }
//         }
//     ).unwrap();

//     // --- Link: host_dev_msleep ---
//     linker.func_wrap("zephyr", "host_dev_msleep", |_caller: Caller<'_, HostState>, ms: i32| -> i32 {
//         unsafe {
//             host_dev_msleep(ms)
//         }
//     }).unwrap();

//     linker
// }

// pub struct Knobs<R, S> {
//     pub config: Config,
//     pub buf: R,
//     pub settings: S,
//     pub cli_file: String,
// }

// extern "C" {
//     fn esp_get_free_heap_size() -> u32;
//     fn esp_get_minimum_free_heap_size() -> u32;
// }

// component
#[unsafe(no_mangle)]
// pub extern "C" fn rust_main() -> ! {  
pub extern "C" fn rust_main() -> () {
    
    println!("Starting Embedding\n");

    let mut config = Config::new();
    // config.wasm_gc(false);
    config.gc_support(false);
    config.target("pulley32").unwrap();
    config.memory_init_cow(false);
    config.signals_based_traps(false);
    config.wasm_component_model(true);
    config.max_wasm_stack(26 * 1024);
    config.memory_reservation(0);
    config.memory_reservation_for_growth(0);
    config.memory_guard_size(0);
    config.debug_info(false);
    // config.wasm_relaxed_simd(false);
    // config.wasm_simd(false);
    // config.relaxed_simd_deterministic(false);
    config.rr(RRConfig::Recording);

    let mut rs = RecordSettings::default();
    // rs.validate = false;
    // let file = "/home/jerryfen/zephyrproject/zephyr/apps/blinky/wasm_component/blinky.component.wasm";
    // let file = "/home/jerryfen/wasmtime-rr-prototyping/comp_check_guest/comp_check_guest.wasm";

    // println!("Configs ok!");

    // let engine = Engine::new(&config).unwrap();

    let engine = match Engine::new(&config) {
        Ok(eng) => eng,
        Err(e) => {
            println!("Deserialize Error: {}", e);
            panic!("Instantiation failed");
        }
    };

    // let engine = Engine::new(&knobs.config).unwrap();

    // println!("Engine ok!");

    let mut store = Store::new(&engine, ());

    // let bufWriter_inst = BufWriter;
    // println!("Store ok!");

    store.record(BufWriter::new(), rs).unwrap();
    // store.record()

    // println!("store.record ok!");

    // let v = Box::new(23);

    // let component = Component::from_file(&engine, "/home/jerryfen/wasmtime-rr-prototyping/target/wasm32-wasip1/release/comp_check_guest.wasm")?;
    // let wasm_bytes = std::fs::read("/home/jerryfen/wasmtime-rr-prototyping/target/wasm32-wasip1/release/comp_check_guest.wasm")?;
    // let component = wasmtime::component::Component::new(&engine, wasm_bytes)?;
    
    // let is_ok = wasmtime::Engine::detect_precompiled(COMP_ADD);
    // if is_ok.is_none() {
    //     // If you reach here, the bytes in COMP_ADD are mathematically 
    //     // incompatible with the Engine you just created.
    //     panic!("Engine and .cwasm are not friends."); 
    // }

    // 1. Ensure bytes are aligned (see struct above)
    let bytes: &[u8] = &COMP_ADD.data;


    // 3. Create the pointer
    let slice_ptr = ptr::slice_from_raw_parts_mut(bytes.as_ptr() as *mut u8, bytes.len());
    let non_null_ptr = NonNull::new(slice_ptr).unwrap();

    // let mut buf = UartBuffer { data: [0; 512], pos: 0 };
    // let _ = core::write!(&mut buf, "Address 0x{:x} is not 16-byte aligned!", addr); // Format the error 'e' into the buffer

    // Now send the buffer to your UART
    // uart_write_bytes(&buf.data[..buf.pos]);

    // println!("Pre-Deserialize ok!");

    // let mut stats = HeapStats {
    //     free_bytes: 0,
    //     allocated_bytes: 0,
    //     max_allocated_bytes: 0,
    //     num_allocs: 0,
    //     num_frees: 0,
    //     max_free_block_size: 0,
    // };

    // unsafe {
    //     println!("Heap Pointer Address: {:p}", _system_heap);
    //     if sys_heap_runtime_stats_get(_system_heap, &mut stats) == 0 {
    //         println!("--- HEAP STATS ---");
    //         println!("Free: {} bytes", stats.free_bytes);
    //         println!("Max Free Block: {} bytes", stats.max_free_block_size);
    //         println!("Total Allocs: {}", stats.num_allocs);
    //         println!("------------------");
    //     } else {
    //         println!("Failed to get heap stats");
    //     }
    // }

    // unsafe {
    //     println!("ESP Free Heap: {} bytes", esp_get_free_heap_size());
    // }

    // 4. The call
    let component = unsafe { 
        // wasmtime::component::Component::deserialize_raw(&engine, non_null_ptr).expect("This should return an Err, not a panic") 
        match wasmtime::component::Component::deserialize_raw(&engine, non_null_ptr) {
            Ok(comp) => comp,
            Err(e) => {
                println!("Deserialize Error: {}", e);
                panic!("Instantiation failed");
            }
        }
    };

    // println!("Deserialize ok!");

    // let component = unsafe {
    //     Component::deserialize(&engine, CWASM_ADD).unwrap()
    // };


    // 3. Setup Linker (empty since we have no imports like WASI)
    let mut linker = Linker::new(&engine);

    let mut zephyr = linker.instance("zephyr").unwrap();

    zephyr.func_wrap("host-pin-init", |_caller, (): ()| {
        // Note: No 'Caller' here to grab memory. 
        // Data is passed as arguments directly.
        unsafe { host_pin_init() };
        Ok((0i32,)) // Must return a Result for the Linker
    }).unwrap();

    zephyr.func_wrap("host-pin-toggle-dt", |_caller, (): ()| {
        // println!("Initializing Pin...");
        unsafe { host_pin_toggle_dt() };
        Ok((0i32,))
    }).unwrap();

    zephyr.func_wrap("host-dev-msleep", |_caller, (ms,): (u32,)| {
        // println!("Initializing Pin...");
        unsafe { host_dev_msleep(ms as i32) };
        Ok((0i32,))
    }).unwrap();

    // println!("Linker Create ok!");

    let instance = match linker.instantiate(&mut store, &component) {
        Ok(inst) => inst,
        Err(e) => {
            println!("Linker.instantiate Error: {}", e);
            panic!("Instantiation failed");
        }
    };

    // println!("Linker Instantiate ok!");

    // basic.wasm
    // let blink = instance.get_typed_func::<(u32,), (u32,)>(&mut store, "comp_add").unwrap();

    let blink = instance.get_typed_func::<(), (i32,)>(&mut store, "blink").unwrap();

    // let blink = match instance.get_typed_func::<(), (i32,)>(&mut store, "blink"){
    //     Ok(inst) => inst,
    //     Err(e) => {
    //         println!("Instance Error: {}", e);
    //         panic!("Instantiation failed");
    //     }
    // };

    // println!("Blink Create ok!");

    let (result,) = blink.call(&mut store, ()).unwrap();

    // let (result,) = blink.call(&mut store, (2u32,)).unwrap();

    // let (result,) = match blink.call(&mut store, ()) {
    //     Ok(inst) => (inst, ),
    //     Err(e) => {
    //         println!("Instance Error: {}", e);
    //         panic!("Instantiation failed");
    //     }
    // };

    // println!("Call ok!");

    blink.post_return(&mut store).unwrap();
    // println!("Post Return ok!");
    // println!("Finished Embedding\n");

    // loop {} 

}


// module
// #[unsafe(no_mangle)]
// pub extern "C" fn rust_main() -> ! {  
    
//     println!("In rust_main!");

//     let mut config = Config::new();
//     // config.wasm_gc(false);
//     config.gc_support(false);
//     config.target("pulley32").unwrap();
//     config.memory_init_cow(false);
//     config.signals_based_traps(false);
//     config.max_wasm_stack(32 * 1024);
//     config.memory_reservation(0);
//     config.memory_reservation_for_growth(0);
//     config.memory_guard_size(0);

//     println!("Configs ok!");

//     let engine = Engine::new(&config).unwrap();

//     println!("Engine ok!");

//     let mut store = Store::new(&engine, HostState);

//     println!("Store ok!");

//     // 1. Deserialize (NO COMPILATION)
//     // let module = unsafe {
//     //     Module::deserialize(&engine, CWASM_BLINKY).unwrap()
//     // };

//     let module = unsafe {
//     wasmtime::Module::deserialize(&engine, CWASM_BLINKY).unwrap_or_else(|e| {
//         println!("Module::deserialize failed: {e:?}");
//         panic!("Module::deserialize failed"); // or panic!("{e:?}");
//     })
// };

//     println!("Deserialize ok!");

//     let linker = create_zephyr_linker(&engine);

//     println!("linker Create ok!");

//     // 2. Instantiate
//     // let instance = Instance::new(&mut store, &module, &[]).unwrap();
    
//     let instance = linker.instantiate(&mut store, &module).unwrap_or_else(|e| {
//         println!("linker.instantiate failed: {e:?}");
//         panic!("linker.instantiate failed"); // or panic!("{e:?}");
//     });

//     println!("linker.instantiate ok!");

//     // 3. Call function
//     let blink = instance
//         .get_typed_func::<(), i32>(&mut store, "blink").unwrap_or_else(|e| {
//         println!("instance failed: {e:?}");
//         panic!("instance failed"); // or panic!("{e:?}");
//     });

//     println!("blink instance ok!");

//     let result = blink.call(&mut store, ()).unwrap();

//     // 4. Signal success
//     // if result == 8 {
//     //     // panic!("panic but the good kind");
//     //     println!("add worked!\n")
//     // }

//     loop {} 

// }

