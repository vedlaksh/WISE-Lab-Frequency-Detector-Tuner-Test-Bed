fn main() {
    println!("cargo:rerun-if-changed=src/bridge.c");
    println!("cargo:rerun-if-changed=build.rs");

    cc::Build::new()
        .file("src/bridge.c")
        .compiler("aarch64-linux-gnu-gcc")
        .flag("--sysroot=/home/jerryfen/sysroot")
        .compile("bridge");

    println!("cargo:rustc-link-search=native=/home/jerryfen//sysroot/usr/lib/aarch64-linux-gnu");
    println!("cargo:rustc-link-lib=asound");
}