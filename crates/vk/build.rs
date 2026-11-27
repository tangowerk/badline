//! Compiles the GLSL shaders in shaders/ to SPIR-V with naga (no system shader compiler).

use std::env;
use std::fs;
use std::path::PathBuf;

const SHADERS: &[&str] = &[
    "image.vert",
    "image.frag",
    "overlay.vert",
    "overlay.frag",
    "text.vert",
    "text.frag",
];

fn main() {
    let dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap()).join("shaders");
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());

    for name in SHADERS {
        let path = dir.join(name);
        println!("cargo:rerun-if-changed={}", path.display());
        let source = fs::read_to_string(&path).unwrap();
        let stage = if name.ends_with(".vert") {
            naga::ShaderStage::Vertex
        } else {
            naga::ShaderStage::Fragment
        };

        let mut frontend = naga::front::glsl::Frontend::default();
        let module = frontend
            .parse(&naga::front::glsl::Options::from(stage), &source)
            .unwrap_or_else(|e| panic!("{name}: {}", e.emit_to_string(&source)));
        let info = naga::valid::Validator::new(
            naga::valid::ValidationFlags::all(),
            naga::valid::Capabilities::empty() | naga::valid::Capabilities::all(),
        )
        .validate(&module)
        .unwrap_or_else(|e| panic!("{name}: {}", e.emit_to_string(&source)));

        let mut options = naga::back::spv::Options::default();
        // the shaders flip y themselves (GL style coordinates from the C++ side)
        options.flags.remove(naga::back::spv::WriterFlags::ADJUST_COORDINATE_SPACE);
        options.lang_version = (1, 3);
        let words = naga::back::spv::write_vec(&module, &info, &options, None)
            .unwrap_or_else(|e| panic!("{name}: {e}"));
        let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        fs::write(out.join(format!("{name}.spv")), bytes).unwrap();
    }
}
