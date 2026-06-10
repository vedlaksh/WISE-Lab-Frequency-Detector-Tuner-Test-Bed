fn main() {
    println!("cargo:rerun-if-changed=src/bridge.c");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=RPI_SYSROOT");

    let mut build = cc::Build::new();
    build.file("src/bridge.c");

    // Two build modes:
    //  - RPI_SYSROOT set  -> cross-compile for aarch64 against that sysroot
    //                        (building on an x86_64 Linux box).
    //  - RPI_SYSROOT unset -> build natively with the default toolchain, e.g.
    //                        inside an aarch64 Linux container (Apple Silicon)
    //                        or directly on the Pi. Uses the system libasound.
    if let Ok(sysroot) = std::env::var("RPI_SYSROOT") {
        build
            .compiler("aarch64-linux-gnu-gcc")
            .flag(&format!("--sysroot={}", sysroot));
        println!(
            "cargo:rustc-link-search=native={}/usr/lib/aarch64-linux-gnu",
            sysroot
        );
    }

    build.compile("bridge");
    println!("cargo:rustc-link-lib=asound");
}
