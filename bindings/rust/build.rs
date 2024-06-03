extern crate bindgen;
extern crate build_deps;

use std::env;
use std::fs::canonicalize;
use std::path::{Path, PathBuf};

use anyhow::Context;
use std::process::Command;

fn rerun_if_changed(root_dir: &Path) -> anyhow::Result<()> {
    build_deps::rerun_if_changed_paths(
        canonicalize("./build.rs")
            .context("canonicalize error")?
            .to_str()
            .unwrap(),
    )
    .map_err(|e| anyhow::anyhow!("failed to add rerun command: {:?}", e))?;

    let inc_glob = format!("{}/inc/**", root_dir.to_str().unwrap());
    build_deps::rerun_if_changed_paths(&inc_glob)
        .map_err(|e| anyhow::anyhow!("failed to add rerun command: {:?}", e))?;

    let static_lib_glob = format!("{}/*.a", root_dir.to_str().unwrap());
    build_deps::rerun_if_changed_paths(&static_lib_glob)
        .map_err(|e| anyhow::anyhow!("failed to add rerun command: {:?}", e))?;

    Ok(())
}

// consult shared.mk for other libraries... sorry y'all.
fn consult_makefile(
    root_dir: &Path,
    search_dirs: &mut Vec<PathBuf>,
    static_libs: &mut Vec<String>,
    dyn_libs: &mut Vec<String>,
) -> anyhow::Result<()> {
    let output = Command::new("make")
        .args([
            "-f",
            &format!("{}/Makefile", root_dir.to_str().unwrap()),
            "print-RUNTIME_LIBS",
            &format!("ROOT_PATH={}", root_dir.to_str().unwrap()),
        ])
        .output()
        .context("failed to run `make`")?;

    for t in String::from_utf8_lossy(&output.stdout).split_whitespace() {
        if t.starts_with("-L") {
            let canonicalized =
                canonicalize(t.replace("-L", "")).context("failed to canonicalize dir")?;
            search_dirs.push(canonicalized);
        } else if t == "-lmlx5" || t == "-libverbs" || t.contains("spdk") {
            static_libs.push(t.replace("-l", ""));
        } else if t.starts_with("-l:lib") {
            static_libs.push(t.replace("-l:lib", "").replace(".a", ""));
        } else if t == "-lpthread" {
        } else if t.starts_with("-l") {
            dyn_libs.push(t.replace("-l", ""));
        }
    }

    Ok(())
}

fn linker_arguments(root_dir: &Path) -> anyhow::Result<()> {
    let mut static_libs = vec![
        "base".to_string(),
        "net".to_string(),
        "runtime".to_string(),
        "shim".to_string(),
    ];
    let mut dyn_libs = vec![];
    let mut search_paths = vec![root_dir.to_path_buf(), root_dir.join("shim")];

    consult_makefile(root_dir, &mut search_paths, &mut static_libs, &mut dyn_libs)?;

    for searchd in search_paths {
        println!("cargo::rustc-link-search={}", searchd.to_str().unwrap());
    }
    for static_lib in static_libs {
        println!("cargo::rustc-link-lib=static={}", static_lib);
    }
    for dyn_lib in dyn_libs {
        println!("cargo::rustc-link-lib={}", dyn_lib);
    }

    Ok(())
}

fn gen_bindings(root_dir: &Path) -> anyhow::Result<()> {
    // The bindgen::Builder is the main entry point
    // to bindgen, and lets you build up options for
    // the resulting bindings.
    let inc_dir = root_dir.join("inc");
    let bindings = bindgen::Builder::default()
        .clang_arg(format!("-I{}", inc_dir.to_str().unwrap()))
        // The input header we would like to generate
        // bindings for.
        .header("shenango.h")
        .blocklist_function("q.cvt(_r)?")
        .blocklist_function("strtold")
        .generate_comments(false)
        // Finish the builder and generate the bindings.
        .generate()
        // Unwrap the Result and panic on failure.
        .context("Unable to generate bindings")?;

    // Write the bindings to the $OUT_DIR/bindings.rs file.
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .context("failed to write bindings!")
}

fn main() -> anyhow::Result<()> {
    let root_dir = canonicalize("../../").context("failed to canonicalize root dir")?;
    rerun_if_changed(&root_dir)?;

    // Tell cargo to tell rustc to use the link script
    let link_script_path = root_dir.join("base/base.ld");
    println!(
        "cargo::rustc-link-arg=-T{}",
        link_script_path.to_str().unwrap()
    );

    // emmit linker arguments
    linker_arguments(&root_dir)?;

    gen_bindings(&root_dir)?;

    Ok(())
}
