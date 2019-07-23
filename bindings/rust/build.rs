extern crate bindgen;
extern crate build_deps;

use std::env;
use std::path::PathBuf;

fn main() {
    build_deps::rerun_if_changed_paths( "../../inc/**" ).unwrap();
    build_deps::rerun_if_changed_paths( "../../*.a" ).unwrap();

    // Tell cargo to tell rustc to link the library.
    println!("cargo:rustc-link-lib=static=base");
    println!("cargo:rustc-link-lib=static=net");
    println!("cargo:rustc-link-lib=static=runtime");
    println!("cargo:rustc-flags=-L ../..");

    // The bindgen::Builder is the main entry point
    // to bindgen, and lets you build up options for
    // the resulting bindings.
    let bindings = bindgen::Builder::default()
        .clang_arg("-I../../inc/")
        // The input header we would like to generate
        // bindings for.
        .header("shenango.h")
        // Finish the builder and generate the bindings.
        .generate()
        // Unwrap the Result and panic on failure.
        .expect("Unable to generate bindings");

    // Write the bindings to the $OUT_DIR/bindings.rs file.
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");
}
