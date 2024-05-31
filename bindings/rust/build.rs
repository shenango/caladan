extern crate bindgen;
extern crate build_deps;

use std::env;
use std::fs::canonicalize;
use std::path::PathBuf;

use anyhow::Context;
use std::process::Command;

fn main() -> anyhow::Result<()> {
    build_deps::rerun_if_changed_paths(
        canonicalize("./build.rs")
            .context("canonicalize error")?
            .to_str()
            .unwrap(),
    )
    .map_err(|e| anyhow::anyhow!("failed to add rerun command: {:?}", e))?;

    let root_dir = canonicalize("../../").context("failed to canonicalize root dir")?;
    let inc_glob = format!("{}/inc/**", root_dir.to_str().unwrap());
    build_deps::rerun_if_changed_paths(&inc_glob)
        .map_err(|e| anyhow::anyhow!("failed to add rerun command: {:?}", e))?;

    let static_lib_glob = format!("{}/inc/**", root_dir.to_str().unwrap());
    build_deps::rerun_if_changed_paths(&static_lib_glob)
        .map_err(|e| anyhow::anyhow!("failed to add rerun command: {:?}", e))?;

    // Tell cargo to tell rustc to link the library.
    println!("cargo::rustc-link-lib=static=base");
    println!("cargo::rustc-link-lib=static=net");
    println!("cargo::rustc-link-lib=static=runtime");
    println!("cargo::rustc-flags=-L {}", root_dir.to_str().unwrap());
    let link_script_path = root_dir.join("base/base.ld");
    println!(
        "cargo::rustc-link-arg=-T{}",
        link_script_path.to_str().unwrap()
    );

    println!(
        "cargo::rustc-flags=-L {}",
        root_dir.join("shim").to_str().unwrap()
    );
    println!("cargo::rustc-link-lib=static=shim");

    // consult shared.mk for other libraries... sorry y'all.
    let output = Command::new("make")
        .args([
            "-f",
            "../../Makefile",
            "print-RUNTIME_LIBS",
            "ROOT_PATH=../../",
        ])
        .output()
        .unwrap();
    for t in String::from_utf8_lossy(&output.stdout).split_whitespace() {
        if t.starts_with("-L") {
            println!("cargo::rustc-flags={}", t.replace("-L", "-L "));
        } else if t == "-lmlx5" || t == "-libverbs" || t.contains("spdk") {
            println!("cargo::rustc-link-lib=static={}", t.replace("-l", ""));
        } else if t.starts_with("-l:lib") {
            println!(
                "cargo::rustc-link-lib=static={}",
                t.replace("-l:lib", "").replace(".a", "")
            );
        } else if t == "-lpthread" {
        } else if t.starts_with("-l") {
            println!("cargo::rustc-link-lib={}", t.replace("-l", ""));
        }
    }

    // The bindgen::Builder is the main entry point
    // to bindgen, and lets you build up options for
    // the resulting bindings.
    let bindings = bindgen::Builder::default()
        .clang_arg("-I../../inc/")
        // The input header we would like to generate
        // bindings for.
        .header("shenango.h")
        .blocklist_function("q.cvt(_r)?")
        .blocklist_function("strtold")
        .generate_comments(false)
        // Finish the builder and generate the bindings.
        .generate()
        // Unwrap the Result and panic on failure.
        .expect("Unable to generate bindings");

    // Write the bindings to the $OUT_DIR/bindings.rs file.
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");

    Ok(())
}
