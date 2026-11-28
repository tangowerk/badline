//! GLSL -> SPIR-V through the C API of the system glslang (`glslang/Include/glslang_c_interface.h`,
//! Arch package `glslang`). The structs below mirror glslang 16.x by hand; only calls without
//! version dependent option structs are used (no `glslang_spv_options_t`).

use std::ffi::{CStr, CString, c_char, c_int, c_uint, c_void};
use std::sync::Once;

#[repr(C)]
struct Version {
    major: c_int,
    minor: c_int,
    patch: c_int,
    flavor: *const c_char,
}

#[repr(C)]
struct IncludeCallbacks {
    include_system: *const c_void,
    include_local: *const c_void,
    free_include_result: *const c_void,
}

#[repr(C)]
struct Input {
    language: c_int,
    stage: c_int,
    client: c_int,
    client_version: c_int,
    target_language: c_int,
    target_language_version: c_int,
    code: *const c_char,
    default_version: c_int,
    default_profile: c_int,
    force_default_version_and_profile: c_int,
    forward_compatible: c_int,
    messages: c_int,
    resource: *const c_void,
    callbacks: IncludeCallbacks,
    callbacks_ctx: *mut c_void,
}

#[repr(C)]
struct Shader {
    _p: [u8; 0],
}

#[repr(C)]
struct Program {
    _p: [u8; 0],
}

#[link(name = "glslang")]
#[link(name = "glslang-default-resource-limits")]
unsafe extern "C" {
    fn glslang_get_version(version: *mut Version);
    fn glslang_initialize_process() -> c_int;
    fn glslang_default_resource() -> *const c_void;
    fn glslang_shader_create(input: *const Input) -> *mut Shader;
    fn glslang_shader_delete(shader: *mut Shader);
    fn glslang_shader_preprocess(shader: *mut Shader, input: *const Input) -> c_int;
    fn glslang_shader_parse(shader: *mut Shader, input: *const Input) -> c_int;
    fn glslang_shader_get_info_log(shader: *mut Shader) -> *const c_char;
    fn glslang_program_create() -> *mut Program;
    fn glslang_program_delete(program: *mut Program);
    fn glslang_program_add_shader(program: *mut Program, shader: *mut Shader);
    fn glslang_program_link(program: *mut Program, messages: c_int) -> c_int;
    fn glslang_program_SPIRV_generate(program: *mut Program, stage: c_int);
    fn glslang_program_SPIRV_get_size(program: *mut Program) -> usize;
    fn glslang_program_SPIRV_get(program: *mut Program, out: *mut c_uint);
    fn glslang_program_get_info_log(program: *mut Program) -> *const c_char;
}

const SOURCE_GLSL: c_int = 1;
const CLIENT_VULKAN: c_int = 1;
const TARGET_VULKAN_1_0: c_int = 1 << 22;
const TARGET_SPV: c_int = 1;
const TARGET_SPV_1_0: c_int = 1 << 16;
const NO_PROFILE: c_int = 1;
const MSG_SPV_RULES: c_int = 1 << 3;
const MSG_VULKAN_RULES: c_int = 1 << 4;

#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum Stage {
    Vertex,
    Fragment,
}

impl Stage {
    fn raw(self) -> c_int {
        match self {
            Stage::Vertex => 0,
            Stage::Fragment => 4,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Stage::Vertex => "vertex",
            Stage::Fragment => "fragment",
        }
    }
}

static INIT: Once = Once::new();

fn init() {
    // never finalized: initialized once for the process lifetime
    INIT.call_once(|| unsafe {
        glslang_initialize_process();
    });
}

/// "glslang 16.6.0" (part of the shader cache key)
pub fn version() -> String {
    let mut v = Version {
        major: 0,
        minor: 0,
        patch: 0,
        flavor: std::ptr::null(),
    };
    unsafe { glslang_get_version(&mut v) };
    let flavor = if v.flavor.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(v.flavor) }.to_string_lossy().into_owned()
    };
    format!("glslang {}.{}.{}{}", v.major, v.minor, v.patch, flavor)
}

fn log(p: *const c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(p) }.to_string_lossy().trim().to_string()
    }
}

/// Compiles one stage (Vulkan 1.0 / SPIR-V 1.0 like RetroArch; includes are resolved before).
/// Thread safe (glslang keeps its pools per thread).
pub fn compile(source: &str, stage: Stage) -> Result<Vec<u32>, String> {
    init();
    let code = CString::new(source).map_err(|_| "shader source contains a NUL byte".to_string())?;
    let messages = MSG_SPV_RULES | MSG_VULKAN_RULES;
    let input = Input {
        language: SOURCE_GLSL,
        stage: stage.raw(),
        client: CLIENT_VULKAN,
        client_version: TARGET_VULKAN_1_0,
        target_language: TARGET_SPV,
        target_language_version: TARGET_SPV_1_0,
        code: code.as_ptr(),
        default_version: 100,
        default_profile: NO_PROFILE,
        force_default_version_and_profile: 0,
        forward_compatible: 0,
        messages,
        resource: unsafe { glslang_default_resource() },
        callbacks: IncludeCallbacks {
            include_system: std::ptr::null(),
            include_local: std::ptr::null(),
            free_include_result: std::ptr::null(),
        },
        callbacks_ctx: std::ptr::null_mut(),
    };
    unsafe {
        let shader = glslang_shader_create(&input);
        if shader.is_null() {
            return Err("glslang_shader_create failed".into());
        }
        let result = (|| {
            if glslang_shader_preprocess(shader, &input) == 0 || glslang_shader_parse(shader, &input) == 0 {
                return Err(log(glslang_shader_get_info_log(shader)));
            }
            let program = glslang_program_create();
            glslang_program_add_shader(program, shader);
            let result = if glslang_program_link(program, messages) == 0 {
                Err(log(glslang_program_get_info_log(program)))
            } else {
                glslang_program_SPIRV_generate(program, stage.raw());
                let mut words = vec![0u32; glslang_program_SPIRV_get_size(program)];
                glslang_program_SPIRV_get(program, words.as_mut_ptr());
                Ok(words)
            };
            glslang_program_delete(program);
            result
        })();
        glslang_shader_delete(shader);
        result.map_err(|e| format!("{} shader: {e}", stage.name()))
    }
}
