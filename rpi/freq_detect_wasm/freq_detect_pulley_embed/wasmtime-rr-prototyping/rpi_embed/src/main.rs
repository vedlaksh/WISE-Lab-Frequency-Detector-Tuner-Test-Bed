// #![feature(alloc_error_handler)]
// #![no_std]
// #![no_main]

extern crate alloc;

use wasmtime::*;
use wasmtime::Error as WasmtimeError;
use std::fs;
use std::error::Error;
use wasmtime::component::{bindgen, Component, Linker};
use core::panic::PanicInfo;
use core::alloc::Layout;
use core::ptr::{self, NonNull};
use core::option::Option::Some;
// use core::any::Any;
// use core::ffi::c_void;
// use core::fmt::{self, Write};
use alloc::string::String;
use std::io::{self, Write};

bindgen!("host" in "/home/jerryfen/zephyrproject/rpi/freq_detect_wasm/freq_detect_embed/adc.wit");

extern "C" {
    fn host_alsa_capture_init() -> i32;
    fn host_read_sample() -> i32;
    fn host_snd_pcm_close();
    fn host_printf(ptr: *const u8, len: usize);
    fn host_sin(x: f64) -> f64;
    fn host_cos(x: f64) -> f64;
}


// pub struct BufWriter; 

// impl BufWriter {
//     pub fn new() -> Self {
//         Self
//     }
// }

// impl RecordWriter for BufWriter
// {
//     fn write(&mut self, data: &[u8]) -> Result<usize, Error> {
//         // unsafe { host_rr_write(data.as_ptr(), data.len()) };
//         // Ok(data.len())
//         // unsafe { send_bytes(data.as_ptr(), data.len()) };
//         // Ok(data.len())
//         Ok(0)
//     }
//     fn flush(&mut self) -> Result<(), Error> { Ok(()) }
// }

pub struct BufWriter;

impl BufWriter {
    pub fn new() -> Self {
        Self
    }
}


impl Write for BufWriter {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        // consume or store the data
        Ok(data.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}


#[no_mangle]
pub extern "C" fn wasmtime_tls_get() -> *mut core::ffi::c_void {
    static mut DUMMY: [u8; 1024] = [0; 1024];
    unsafe { DUMMY.as_mut_ptr() as *mut core::ffi::c_void }
}

#[no_mangle]
pub extern "C" fn wasmtime_tls_set(_ptr: *mut core::ffi::c_void) {}

// #[alloc_error_handler]
// fn oom(layout: Layout) -> ! {
//     println!("alloc failed: size={} align={}", layout.size(), layout.align());
//     // panic!();
//     loop {}
// }

// #[panic_handler]
// fn panic(info: &core::panic::PanicInfo) -> ! {
//     println!("PANIC");
//     if let Some(loc) = info.location() {
//         println!("file: {}", loc.file());
//         println!("line: {}", loc.line());
//     } else {
//         println!("(no loc)");
//     }

//     // if let Some(loc) = info.location() {
//     //     uart_write_bytes(loc.file().as_bytes());
//     //     uart_putc(b':');
//     //     uart_put_dec_u32(loc.line());
//     // } else {
//     //     uart_puts(b"(no loc)");
//     // }

//     loop {}
// }


fn main() -> Result<(), Box<dyn Error>> {
    let mut config = Config::new();
    // for use with rr
    config.gc_support(false);
    // config.target("pulley32").unwrap();
    config.memory_init_cow(false);
    config.signals_based_traps(false);
    config.wasm_component_model(true);
    config.max_wasm_stack(16 * 1024);
    config.memory_reservation(0);
    config.memory_reservation_for_growth(0);
    config.memory_guard_size(0);
    // config.debug_info(true);
    // config.relaxed_simd_deterministic(false);
    config.rr(RRConfig::Recording);
    // config.rr(RRConfig::None);

    let mut rs = RecordSettings::default();

    let engine = Engine::new(&config)?;

    // let wasm = fs::read("/home/jerryfen/zephyrproject/rpi/freq_detect_wasm/freq_detect_embed/wasm_component/tuner.component.wasm")?;

    // let wasm = fs::read("tuner.component.wasm")?;
    static WASM_COMPONENT: &[u8] = include_bytes!("/home/jerryfen/zephyrproject/rpi/freq_detect_wasm/freq_detect_embed/wasm_component/tuner.component.wasm");

    // let wasm = WASM_COMPONENT;

    let mut store = Store::new(&engine, ());

    store.record(BufWriter::new(), rs).unwrap();

    // engine::new on the raw wasm file, not the cwasm
    // let module = Module::new(&engine, wasm)?;
    let component = Component::from_binary(&engine, WASM_COMPONENT)?;

    let mut linker = Linker::new(&engine);

    let mut rpi = linker.instance("rpi").unwrap();

    rpi.func_wrap("host-alsa-capture-init", |_caller, (): ()| {
        // Note: No 'Caller' here to grab memory. 
        // Data is passed as arguments directly.
        // println!("host-i2s-configure");
        let retval = unsafe { host_alsa_capture_init() };
        Ok((retval,)) // Must return a Result for the Linker
    }).unwrap();

    rpi.func_wrap("host-read-sample", |_caller, (): ()| {
        let retval = unsafe { host_read_sample() };
        Ok((retval,)) // Must return a Result for the Linker
    }).unwrap();

    rpi.func_wrap("host-snd-pcm-close", |_caller, (): ()| {
        // println!("host_adc_raw_to_millivolts");
        unsafe { host_snd_pcm_close() };
        Ok(())
    }).unwrap();

    rpi.func_wrap("host-printf", |_caller, (msg,): (String,)| {
        unsafe { host_printf(msg.as_ptr(), msg.len()) };
        Ok(())
    }).unwrap();

    rpi.func_wrap("host-sin", |_caller, (x,): (f64,)| {
        let y = unsafe { host_sin(x) };
        Ok((y,))
    }).unwrap();

    rpi.func_wrap("host-cos", |_caller, (x,): (f64,)| {
        let y = unsafe { host_cos(x) };
        Ok((y,))
    }).unwrap();

    println!("Linker Create ok!");

    let instance = match linker.instantiate(&mut store, &component) {
        Ok(inst) => inst,
        Err(e) => {
            println!("Linker.instantiate Error: {}", e);
            panic!("Instantiation failed");
        }
    };

    println!("Linker Instantiate ok!");

    // let run = instance.get_typed_func::<(), (i32,)>(&mut store, "run").unwrap();

    let run = match instance.get_typed_func::<(), (i32,)>(&mut store, "run"){
        Ok(inst) => inst,
        Err(e) => {
            println!("Instance Error: {}", e);
            panic!("Instantiation failed");
        }
    };

    println!("run Create ok!");

    // let (result,) = run.call(&mut store, ()).unwrap();

    match run.call(&mut store, ()) {
        Ok(inst) => (inst, ),
        Err(e) => {
            println!("Call Error: {}", e);
            panic!("Call failed");
        }
    };

    Ok(())
}
