use wasmtime::*;
use wasmtime::component::{Component, Linker};
use std::error::Error;
use std::sync::atomic::{AtomicBool, Ordering};

extern "C" {
    fn host_alsa_capture_init() -> i32;
    fn host_read_sample() -> i32;
    fn host_snd_pcm_close();
    fn host_printf(ptr: *const u8, len: usize);
    fn host_sin(x: f64) -> f64;
    fn host_cos(x: f64) -> f64;
}

// Cleared by SIGINT (Ctrl-C). The guest polls host-should-continue() once per
// analysis block; because that return value is recorded by the RR engine,
// replay stops at the exact same iteration, so the trace stays replay-clean.
static KEEP_RUNNING: AtomicBool = AtomicBool::new(true);

extern "C" fn handle_sigint(_sig: core::ffi::c_int) {
    KEEP_RUNNING.store(false, Ordering::SeqCst);
}

#[no_mangle]
pub extern "C" fn wasmtime_tls_get() -> *mut core::ffi::c_void {
    static mut DUMMY: [u8; 1024] = [0; 1024];
    unsafe { DUMMY.as_mut_ptr() as *mut core::ffi::c_void }
}

#[no_mangle]
pub extern "C" fn wasmtime_tls_set(_ptr: *mut core::ffi::c_void) {}

fn main() -> Result<(), Box<dyn Error>> {
    // First Ctrl-C asks the guest to stop cleanly (so the trace can be finalized).
    // signals_based_traps(false) means wasmtime isn't using signal handlers, so
    // SIGINT is ours.
    unsafe { libc::signal(libc::SIGINT, handle_sigint as usize); }

    let mut config = Config::new();
    config.gc_support(false);
    config.memory_init_cow(false);
    config.signals_based_traps(false);
    config.wasm_component_model(true);
    config.max_wasm_stack(16 * 1024);
    config.memory_reservation(0);
    config.memory_reservation_for_growth(0);
    config.memory_guard_size(0);
    config.rr(RRConfig::Recording);

    let rs = RecordSettings::default();
    let engine = Engine::new(&config)?;

    // The compiled component is embedded at build time (path is relative to this
    // source file). Regenerate it (clang -> wasm-tools) before rebuilding.
    static WASM_COMPONENT: &[u8] =
        include_bytes!("../../../../freq_detect_embed/wasm_component/tuner.component.wasm");

    let mut store = Store::new(&engine, ());

    // Real record sink: stream the trace to a file. std's BufWriter<File>
    // satisfies the blanket RecordWriter impl (Write + Send + Sync + Any).
    // Override the path with RPI_TRACE if desired.
    let trace_path = std::env::var("RPI_TRACE").unwrap_or_else(|_| "tuner.trace".to_string());
    let trace_file = std::fs::File::create(&trace_path)?;
    let trace_writer = std::io::BufWriter::new(trace_file);
    store.record(trace_writer, rs).unwrap();

    let component = Component::from_binary(&engine, WASM_COMPONENT)?;

    let mut linker = Linker::new(&engine);
    let mut rpi = linker.instance("rpi").unwrap();

    rpi.func_wrap("host-alsa-capture-init", |_caller, (): ()| {
        let retval = unsafe { host_alsa_capture_init() };
        Ok((retval,))
    }).unwrap();

    rpi.func_wrap("host-read-sample", |_caller, (): ()| {
        let retval = unsafe { host_read_sample() };
        Ok((retval,))
    }).unwrap();

    rpi.func_wrap("host-snd-pcm-close", |_caller, (): ()| {
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

    rpi.func_wrap("host-should-continue", |_caller, (): ()| {
        let go = if KEEP_RUNNING.load(Ordering::Relaxed) { 1i32 } else { 0i32 };
        Ok((go,))
    }).unwrap();

    println!("Linker Create ok!");

    let instance = match linker.instantiate(&mut store, &component) {
        Ok(inst) => inst,
        Err(e) => {
            println!("Linker.instantiate Error: {}", e);
            panic!("Instantiation failed");
        }
    };

    println!("Linker Instantiate ok! Recording to '{}' - press Ctrl-C to stop.", trace_path);

    let run = match instance.get_typed_func::<(), (i32,)>(&mut store, "run") {
        Ok(f) => f,
        Err(e) => {
            println!("Instance Error: {}", e);
            panic!("Instantiation failed");
        }
    };

    match run.call(&mut store, ()) {
        Ok(_inst) => {}
        Err(e) => {
            println!("Call Error: {}", e);
            panic!("Call failed");
        }
    };

    // Finalize the trace: writes the RREvent::Eof marker and flushes the tail.
    // Without this the last (<16) events and the terminator never reach disk,
    // and replay would not see a clean end-of-trace.
    match store.into_record_writer() {
        Ok(w) => { drop(w); println!("Recording finalized: {}", trace_path); }
        Err(e) => println!("Recording finalize error: {}", e),
    }

    Ok(())
}
