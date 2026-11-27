//! Badline entry point: hands the command line to the C++ program (program/program.cpp).

// Vulkan renderer: only referenced from C++ (bvk_* symbols), so link the crate explicitly
extern crate badline_vk;

use std::ffi::{CString, c_char, c_int};
use std::os::unix::ffi::OsStringExt;

unsafe extern "C" {
    fn badline_main(argc: c_int, argv: *mut *mut c_char) -> c_int;
    fn signal(signum: c_int, handler: usize) -> usize;
}

const SIGPIPE: c_int = 13;
const SIG_DFL: usize = 0;

fn main() {
    // Rust ignores SIGPIPE at startup; restore the default the C++ code was written for
    unsafe { signal(SIGPIPE, SIG_DFL) };

    let args: Vec<CString> = std::env::args_os()
        .map(|a| CString::new(a.into_vec()).expect("argument contains a NUL byte"))
        .collect();
    let mut argv: Vec<*mut c_char> = args.iter().map(|a| a.as_ptr() as *mut c_char).collect();
    argv.push(std::ptr::null_mut());

    let code = unsafe { badline_main(args.len() as c_int, argv.as_mut_ptr()) };
    std::process::exit(code);
}
