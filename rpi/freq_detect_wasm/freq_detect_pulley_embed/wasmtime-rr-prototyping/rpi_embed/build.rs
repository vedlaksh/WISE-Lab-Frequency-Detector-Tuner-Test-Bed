fn main() {
    println!("cargo:rerun-if-changed=src/bridge.c");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=RPI_SYSROOT");

    // Path to the aarch64 sysroot that contains libasound (built via debootstrap).
    // Set it on the build machine, e.g.:  export RPI_SYSROOT=/home/you/sysroot
    let sysroot = std::env::var("RPI_SYSROOT")
        .expect("RPI_SYSROOT is not set - point it at your aarch64 sysroot, \
                 e.g. `export RPI_SYSROOT=/path/to/sysroot`");

    cc::Build::new()
        .file("src/bridge.c")
        .compiler("aarch64-linux-gnu-gcc")
        .flag(&format!("--sysroot={}", sysroot))
        .compile("bridge");

    println!("cargo:rustc-link-search=native={}/usr/lib/aarch64-linux-gnu", sysroot);
    println!("cargo:rustc-link-lib=asound");
}
