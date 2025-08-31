use std::path::PathBuf;

fn main() {
    let manifest_path: PathBuf = std::env::var("CARGO_MANIFEST_DIR")
        .unwrap()
        .parse()
        .unwrap();
    // manifest_path is now .../caladan/apps/synthetic
    let link_script_path = manifest_path
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .join("base/base.ld");
    println!(
        "cargo:rustc-link-arg=-T{}",
        link_script_path.to_str().unwrap()
    );
}
