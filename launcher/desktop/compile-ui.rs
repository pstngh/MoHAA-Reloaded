// Compiles the window (ui/app.slint) in Slint's Fluent style, dark, and on Windows the program's icon and
// version (res/launcher.rc).
fn main() {
    let config = slint_build::CompilerConfiguration::new().with_style("fluent-dark".into());
    slint_build::compile_with_config("ui/app.slint", config).expect("ui/app.slint");

    #[cfg(windows)]
    {
        println!("cargo:rerun-if-changed=res/launcher.rc");
        println!("cargo:rerun-if-changed=res/launcher.ico");
        embed_resource::compile("res/launcher.rc", embed_resource::NONE).manifest_optional().unwrap();
    }
}
