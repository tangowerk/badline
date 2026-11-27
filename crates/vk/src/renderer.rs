//! Vulkan 1.3 renderer: dynamic rendering, synchronization2, FIFO present.
//!
//! Per frame: wait for the frame-in-flight fence, acquire, upload the emulator picture through
//! a per-frame staging buffer, draw picture + overlay + screen text, present. Rare uploads
//! (atlas, screen text) wait for the device to be idle.
//!
//! Pacing (M-Pacing1): with a queue depth d > 0 the present N waits until present N-d is on
//! screen (the display clocks the emulation). Depth 1 is too late on compositors that compose
//! right at vblank (Hyprland): a buffer committed after the "presented" event of N-1 misses the
//! next vblank, so video master uses depth 2. `repeat` presents every
//! emulator frame n times (integer multiple refresh). Every present is recorded with its
//! display time (present timing, else the present-wait completion) for the MCP `pacing` tool.

use std::ffi::{CStr, CString, c_void};
use std::io::Cursor;

use ash::ext::debug_utils;
use ash::khr::{get_surface_capabilities2, present_id, present_wait, surface, swapchain, wayland_surface, xlib_surface};
use ash::{Device, Entry, Instance, vk};

use crate::timing::{self, Feedback, Record};
use crate::{
    BVK_FMT_R8, BVK_PRESENT_FIFO, BVK_PRESENT_IMMEDIATE, BVK_SURFACE_WAYLAND, BVK_SURFACE_XLIB, BVK_TEX_ATLAS,
    BVK_TEX_TEXT, BvkCreateInfo, BvkFrame,
};

const FRAMES: usize = 2;
const PUSH_SIZE: u32 = 128;
const VALIDATION_LAYER: &CStr = c"VK_LAYER_KHRONOS_validation";

type Result<T> = std::result::Result<T, String>;

fn vkerr(what: &str) -> impl Fn(vk::Result) -> String + '_ {
    move |e| format!("{what}: {e:?}")
}

struct Buffer {
    buffer: vk::Buffer,
    memory: vk::DeviceMemory,
    size: u64,
    ptr: *mut u8,
}

struct Texture {
    image: vk::Image,
    memory: vk::DeviceMemory,
    view: vk::ImageView,
    width: u32,
    height: u32,
    format: vk::Format,
    /// descriptor sets: [nearest, linear]
    sets: [vk::DescriptorSet; 2],
    /// false until the first upload (layout UNDEFINED)
    ready: bool,
}

struct PendingTexture {
    format: vk::Format,
    width: u32,
    height: u32,
    data: Vec<u8>,
}

struct Prepared {
    upload: bool,
    overlay_count: u32,
    /// texture slots (atlas, text) with data in the frame's upload buffer, in this order
    textures: [bool; 2],
    shot: Option<(u32, u32, u32, u32)>,
}

pub enum Rendered {
    Presented,
    /// presented with the requested screenshot
    WithScreenshot,
    /// presented, but the requested screenshot could not be taken (region outside the swapchain)
    NoScreenshot,
    /// nothing presented (zero size, out of date, acquire timeout)
    Skipped,
}

struct PerFrame {
    cmd: vk::CommandBuffer,
    fence: vk::Fence,
    acquired: vk::Semaphore,
    staging: Option<Buffer>,
    vertices: Option<Buffer>,
    readback: Option<Buffer>,
    uploads: Option<Buffer>,
}

/// present feedback extensions in use
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum PresentApi {
    None,
    /// VK_KHR_present_id + present_wait
    V1,
    /// VK_KHR_present_id2 + present_wait2 + VK_EXT_present_timing
    V2,
}

struct DeviceChoice {
    physical: vk::PhysicalDevice,
    family: u32,
    v1: bool,
    v2: bool,
    absolute_time: bool,
}

struct Pipelines {
    image: vk::Pipeline,
    overlay: vk::Pipeline,
    text: vk::Pipeline,
}

pub struct Renderer {
    _entry: Entry,
    instance: Instance,
    debug: Option<(debug_utils::Instance, vk::DebugUtilsMessengerEXT)>,
    surface_fn: surface::Instance,
    surface: vk::SurfaceKHR,
    physical: vk::PhysicalDevice,
    memory_props: vk::PhysicalDeviceMemoryProperties,
    device: Device,
    queue: vk::Queue,
    swapchain_fn: swapchain::Device,
    swapchain: vk::SwapchainKHR,
    images: Vec<vk::Image>,
    views: Vec<vk::ImageView>,
    render_done: Vec<vk::Semaphore>,
    extent: vk::Extent2D,
    format: vk::Format,
    composite_alpha: vk::CompositeAlphaFlagsKHR,
    can_read_back: bool,
    swapchain_dirty: bool,
    /// window size the swapchain was last built for (current_extent may lag behind it)
    requested: vk::Extent2D,
    /// BVK_PRESENT_*: FIFO, MAILBOX first (warp) or IMMEDIATE first (lowest latency)
    present_pref: i32,
    present_modes: Vec<vk::PresentModeKHR>,
    present_api: PresentApi,
    present_wait_fn: Option<present_wait::Device>,
    present_counter: u64,
    feedback: Feedback,
    /// presents per emulator frame (integer multiple refresh)
    repeat: u32,
    /// presents allowed in flight: wait for present N-depth before presenting N (0: no wait;
    /// video master, FIFO only)
    queue_depth: u32,
    /// present at absolute target times (VK_EXT_present_timing on the surface, FIFO only)
    absolute_time: bool,
    /// target time grid of the emulated frame rate (VRR / no integer multiple)
    grid: Grid,
    /// target time of the next present (0 = as soon as possible)
    target_time: u64,
    /// emulator frame sequence of the last render with a new picture
    last_frame: u64,
    /// last present id on the current swapchain (0: none yet)
    swapchain_last_id: u64,
    /// first present id on the current swapchain
    swapchain_first_id: u64,
    /// a present wait timed out (hidden window, compositor stopped presenting): no waits until
    /// a present is displayed again or a second passed; the program falls back to audio master
    stalled: bool,
    /// last present id when the stall began
    stall_id: u64,
    cmd_pool: vk::CommandPool,
    desc_pool: vk::DescriptorPool,
    set_layout: vk::DescriptorSetLayout,
    samplers: [vk::Sampler; 2],
    layout: vk::PipelineLayout,
    pipelines: Pipelines,
    frames: Vec<PerFrame>,
    current: usize,
    picture: Option<Texture>,
    atlas: Option<Texture>,
    text: Option<Texture>,
    pending: [Option<PendingTexture>; 2],
    device_name: CString,
    /// wp_content_type_v1 "game" on the Wayland surface (dropped after the Vulkan objects)
    content_type: Option<crate::wl::ContentType>,
    /// BADLINE_VK_DEBUG: log renders that block for long
    debug_slow: bool,
}

unsafe extern "system" fn debug_callback(
    severity: vk::DebugUtilsMessageSeverityFlagsEXT,
    _types: vk::DebugUtilsMessageTypeFlagsEXT,
    data: *const vk::DebugUtilsMessengerCallbackDataEXT<'_>,
    _user: *mut c_void,
) -> vk::Bool32 {
    let msg = unsafe { (*data).message_as_c_str() }
        .map(|m| m.to_string_lossy().into_owned())
        .unwrap_or_default();
    let level = if severity.contains(vk::DebugUtilsMessageSeverityFlagsEXT::ERROR) {
        "error"
    } else {
        "warning"
    };
    eprintln!("badline vulkan validation {level}: {msg}");
    vk::FALSE
}

fn validation_wanted() -> bool {
    match std::env::var("BADLINE_VK_VALIDATION").as_deref() {
        Ok("1") => true,
        Ok("0") => false,
        _ => cfg!(debug_assertions),
    }
}

fn spv(bytes: &[u8]) -> Vec<u32> {
    ash::util::read_spv(&mut Cursor::new(bytes)).expect("invalid SPIR-V")
}

impl Renderer {
    pub fn new(info: &BvkCreateInfo) -> Result<Self> {
        let entry = unsafe { Entry::load() }.map_err(|e| format!("Vulkan loader not found: {e}"))?;

        let version = unsafe { entry.try_enumerate_instance_version() }
            .map_err(vkerr("vkEnumerateInstanceVersion"))?
            .unwrap_or(vk::API_VERSION_1_0);
        if version < vk::API_VERSION_1_3 {
            return Err(format!(
                "Vulkan 1.3 required, the loader offers {}.{}",
                vk::api_version_major(version),
                vk::api_version_minor(version)
            ));
        }

        // instance
        let surface_ext = match info.kind {
            BVK_SURFACE_WAYLAND => wayland_surface::NAME,
            BVK_SURFACE_XLIB => xlib_surface::NAME,
            _ => return Err("unsupported window system (Wayland or X11 needed)".into()),
        };
        let mut extensions = vec![surface::NAME.as_ptr(), surface_ext.as_ptr()];
        // needed to query present id2/wait2/timing surface support
        let caps2 = unsafe { entry.enumerate_instance_extension_properties(None) }
            .unwrap_or_default()
            .iter()
            .any(|e| e.extension_name_as_c_str() == Ok(get_surface_capabilities2::NAME));
        if caps2 {
            extensions.push(get_surface_capabilities2::NAME.as_ptr());
        }
        let mut layers = Vec::new();
        let mut validation = false;
        if validation_wanted() {
            let available = unsafe { entry.enumerate_instance_layer_properties() }.unwrap_or_default();
            if available.iter().any(|l| l.layer_name_as_c_str() == Ok(VALIDATION_LAYER)) {
                layers.push(VALIDATION_LAYER.as_ptr());
                extensions.push(debug_utils::NAME.as_ptr());
                validation = true;
            } else if std::env::var("BADLINE_VK_VALIDATION").as_deref() == Ok("1") {
                eprintln!("badline: Vulkan validation layer not installed (vulkan-validation-layers)");
            }
        }

        let app_name = c"Badline";
        let app = vk::ApplicationInfo::default()
            .application_name(app_name)
            .engine_name(app_name)
            .api_version(vk::API_VERSION_1_3);
        let create = vk::InstanceCreateInfo::default()
            .application_info(&app)
            .enabled_extension_names(&extensions)
            .enabled_layer_names(&layers);
        let instance = unsafe { entry.create_instance(&create, None) }.map_err(vkerr("vkCreateInstance"))?;

        let debug = if validation {
            let loader = debug_utils::Instance::new(&entry, &instance);
            let ci = vk::DebugUtilsMessengerCreateInfoEXT::default()
                .message_severity(
                    vk::DebugUtilsMessageSeverityFlagsEXT::ERROR | vk::DebugUtilsMessageSeverityFlagsEXT::WARNING,
                )
                .message_type(
                    vk::DebugUtilsMessageTypeFlagsEXT::GENERAL
                        | vk::DebugUtilsMessageTypeFlagsEXT::VALIDATION
                        | vk::DebugUtilsMessageTypeFlagsEXT::PERFORMANCE,
                )
                .pfn_user_callback(Some(debug_callback));
            unsafe { loader.create_debug_utils_messenger(&ci, None) }
                .ok()
                .map(|m| (loader, m))
        } else {
            None
        };

        // from here on partial objects are destroyed by `Drop` of a half-built renderer would be
        // complex; instead clean up the instance manually on error
        let result = Self::build(entry.clone(), instance.clone(), debug.as_ref().map(|d| d.1), info, caps2);
        match result {
            Ok(mut r) => {
                r.debug = debug;
                if info.kind == BVK_SURFACE_WAYLAND && std::env::var_os("BADLINE_NO_CONTENT_TYPE").is_none() {
                    r.content_type = unsafe { crate::wl::ContentType::game(info.display, info.surface) };
                }
                Ok(r)
            }
            Err(e) => {
                unsafe {
                    if let Some((loader, m)) = debug {
                        loader.destroy_debug_utils_messenger(m, None);
                    }
                    instance.destroy_instance(None);
                }
                Err(e)
            }
        }
    }

    fn build(
        entry: Entry,
        instance: Instance,
        _messenger: Option<vk::DebugUtilsMessengerEXT>,
        info: &BvkCreateInfo,
        caps2: bool,
    ) -> Result<Self> {
        let surface_fn = surface::Instance::new(&entry, &instance);
        let surface = unsafe {
            match info.kind {
                BVK_SURFACE_WAYLAND => {
                    let ci = vk::WaylandSurfaceCreateInfoKHR::default()
                        .display(info.display)
                        .surface(info.surface);
                    wayland_surface::Instance::new(&entry, &instance).create_wayland_surface(&ci, None)
                }
                _ => {
                    let ci = vk::XlibSurfaceCreateInfoKHR::default()
                        .dpy(info.display as *mut vk::Display)
                        .window(info.window as vk::Window);
                    xlib_surface::Instance::new(&entry, &instance).create_xlib_surface(&ci, None)
                }
            }
        }
        .map_err(vkerr("create surface"))?;

        let fail = |e: String| -> String {
            unsafe { surface_fn.destroy_surface(surface, None) };
            e
        };

        let choice = Self::pick_device(&instance, &surface_fn, surface).map_err(fail)?;
        let (physical, family) = (choice.physical, choice.family);
        // V2 also needs the surface to support it (e.g. not on every X11 setup)
        let mut timing_stage = 0u32;
        let mut absolute_time = false;
        let v2 = choice.v2 && caps2 && {
            let caps2_fn = get_surface_capabilities2::Instance::new(&entry, &instance);
            let mut id2 = timing::Bool::surface_present_id2();
            let mut wait2 = timing::Bool::surface_present_wait2();
            let mut tcaps = timing::PresentTimingSurfaceCaps::default();
            let mut caps2 = vk::SurfaceCapabilities2KHR::default();
            unsafe {
                timing::chain(&mut caps2, &mut id2);
                timing::chain(&mut caps2, &mut wait2);
                timing::chain(&mut caps2, &mut tcaps);
            }
            let info = vk::PhysicalDeviceSurfaceInfo2KHR::default().surface(surface);
            let ok = unsafe { caps2_fn.get_physical_device_surface_capabilities2(physical, &info, &mut caps2) }.is_ok();
            if ok && tcaps.present_timing_supported == vk::TRUE {
                absolute_time = choice.absolute_time && tcaps.present_at_absolute_time_supported == vk::TRUE;
                let q = tcaps.present_stage_queries;
                timing_stage = [timing::STAGE_FIRST_PIXEL_VISIBLE, timing::STAGE_FIRST_PIXEL_OUT, timing::STAGE_QUEUE_OPERATIONS_END]
                    .into_iter()
                    .find(|s| q & s != 0)
                    .unwrap_or(0);
            }
            ok && id2.value == vk::TRUE && wait2.value == vk::TRUE && tcaps.present_timing_supported == vk::TRUE
        };
        let present_api = if v2 {
            PresentApi::V2
        } else if choice.v1 {
            PresentApi::V1
        } else {
            PresentApi::None
        };
        if std::env::var("BADLINE_VK_DEBUG").is_ok() {
            eprintln!("badline vulkan: present feedback {present_api:?}, timing stage {timing_stage:#x}, absolute time {absolute_time}");
        }
        let props = unsafe { instance.get_physical_device_properties(physical) };
        let device_name = props
            .device_name_as_c_str()
            .map(|n| n.to_owned())
            .unwrap_or_else(|_| c"unknown".to_owned());
        let memory_props = unsafe { instance.get_physical_device_memory_properties(physical) };

        // device
        let priorities = [1.0f32];
        let queues = [vk::DeviceQueueCreateInfo::default()
            .queue_family_index(family)
            .queue_priorities(&priorities)];
        let mut dev_ext = vec![swapchain::NAME.as_ptr()];
        match present_api {
            PresentApi::V2 => dev_ext.extend([
                timing::PRESENT_ID_2.as_ptr(),
                timing::PRESENT_WAIT_2.as_ptr(),
                timing::PRESENT_TIMING.as_ptr(),
                timing::CALIBRATED_TIMESTAMPS.as_ptr(),
            ]),
            PresentApi::V1 => dev_ext.extend([present_id::NAME.as_ptr(), present_wait::NAME.as_ptr()]),
            PresentApi::None => {}
        }
        let mut f13 = vk::PhysicalDeviceVulkan13Features::default()
            .dynamic_rendering(true)
            .synchronization2(true);
        let mut f_id = vk::PhysicalDevicePresentIdFeaturesKHR::default().present_id(true);
        let mut f_wait = vk::PhysicalDevicePresentWaitFeaturesKHR::default().present_wait(true);
        let mut f_id2 = timing::Bool::feature_present_id2().on();
        let mut f_wait2 = timing::Bool::feature_present_wait2().on();
        let mut f_timing = timing::PresentTimingFeatures {
            present_timing: vk::TRUE,
            present_at_absolute_time: if choice.absolute_time { vk::TRUE } else { vk::FALSE },
            ..Default::default()
        };
        let mut ci = vk::DeviceCreateInfo::default()
            .queue_create_infos(&queues)
            .enabled_extension_names(&dev_ext)
            .push_next(&mut f13);
        match present_api {
            PresentApi::V2 => unsafe {
                timing::chain(&mut ci, &mut f_id2);
                timing::chain(&mut ci, &mut f_wait2);
                timing::chain(&mut ci, &mut f_timing);
            },
            PresentApi::V1 => ci = ci.push_next(&mut f_id).push_next(&mut f_wait),
            PresentApi::None => {}
        }
        let device = unsafe { instance.create_device(physical, &ci, None) }
            .map_err(vkerr("vkCreateDevice"))
            .map_err(fail)?;
        let queue = unsafe { device.get_device_queue(family, 0) };
        let swapchain_fn = swapchain::Device::new(&instance, &device);
        let present_wait_fn = (present_api == PresentApi::V1).then(|| present_wait::Device::new(&instance, &device));
        let v2_fns = if present_api == PresentApi::V2 {
            let fns = timing::Fns::load(&instance, device.handle());
            if fns.is_none() {
                eprintln!("badline vulkan: present timing entry points missing");
            }
            fns
        } else {
            None
        };
        let feedback = Feedback::new(v2_fns, timing_stage);

        // surface format: 8 bit UNORM (matches the GL default framebuffer)
        let query = || -> Result<(vk::Format, vk::SurfaceCapabilitiesKHR)> {
            let formats = unsafe { surface_fn.get_physical_device_surface_formats(physical, surface) }
                .map_err(vkerr("surface formats"))?;
            let format = [vk::Format::B8G8R8A8_UNORM, vk::Format::R8G8B8A8_UNORM]
                .into_iter()
                .find(|f| {
                    formats
                        .iter()
                        .any(|s| s.format == *f && s.color_space == vk::ColorSpaceKHR::SRGB_NONLINEAR)
                })
                .ok_or_else(|| "no 8 bit UNORM surface format".to_string())?;
            let caps = unsafe { surface_fn.get_physical_device_surface_capabilities(physical, surface) }
                .map_err(vkerr("surface capabilities"))?;
            Ok((format, caps))
        };
        let (format, caps) = match query() {
            Ok(v) => v,
            Err(e) => {
                unsafe { device.destroy_device(None) };
                return Err(fail(e));
            }
        };
        let present_modes = unsafe { surface_fn.get_physical_device_surface_present_modes(physical, surface) }
            .unwrap_or_default();
        let composite_alpha = if caps.supported_composite_alpha.contains(vk::CompositeAlphaFlagsKHR::OPAQUE) {
            vk::CompositeAlphaFlagsKHR::OPAQUE
        } else {
            vk::CompositeAlphaFlagsKHR::from_raw(
                1 << caps.supported_composite_alpha.as_raw().trailing_zeros(),
            )
        };
        let can_read_back = caps.supported_usage_flags.contains(vk::ImageUsageFlags::TRANSFER_SRC);

        let mut r = Renderer {
            _entry: entry,
            instance,
            debug: None,
            surface_fn,
            surface,
            physical,
            memory_props,
            device,
            queue,
            swapchain_fn,
            swapchain: vk::SwapchainKHR::null(),
            images: Vec::new(),
            views: Vec::new(),
            render_done: Vec::new(),
            extent: vk::Extent2D::default(),
            format,
            composite_alpha,
            can_read_back,
            swapchain_dirty: true,
            requested: vk::Extent2D::default(),
            present_pref: BVK_PRESENT_FIFO,
            present_modes,
            present_api,
            present_wait_fn,
            present_counter: 0,
            feedback,
            repeat: 1,
            queue_depth: 0,
            absolute_time: absolute_time && present_api == PresentApi::V2,
            grid: Grid::new(),
            target_time: 0,
            last_frame: 0,
            swapchain_last_id: 0,
            swapchain_first_id: u64::MAX,
            stalled: false,
            stall_id: 0,
            cmd_pool: vk::CommandPool::null(),
            desc_pool: vk::DescriptorPool::null(),
            set_layout: vk::DescriptorSetLayout::null(),
            samplers: [vk::Sampler::null(); 2],
            layout: vk::PipelineLayout::null(),
            pipelines: Pipelines {
                image: vk::Pipeline::null(),
                overlay: vk::Pipeline::null(),
                text: vk::Pipeline::null(),
            },
            frames: Vec::new(),
            current: 0,
            picture: None,
            atlas: None,
            text: None,
            pending: [None, None],
            device_name,
            content_type: None,
            debug_slow: std::env::var_os("BADLINE_VK_DEBUG").is_some(),
        };
        // from here on `Drop` cleans up whatever was created (null handles are skipped);
        // the instance is owned by the caller until build() returns Ok
        match r.init_objects(family) {
            Ok(()) => Ok(r),
            Err(e) => {
                r.destroy_objects();
                // prevent Drop from destroying the instance (done by new())
                std::mem::forget(r);
                Err(e)
            }
        }
    }

    fn pick_device(
        instance: &Instance,
        surface_fn: &surface::Instance,
        surface: vk::SurfaceKHR,
    ) -> Result<DeviceChoice> {
        let devices = unsafe { instance.enumerate_physical_devices() }.map_err(vkerr("enumerate devices"))?;
        let mut best: Option<(u32, DeviceChoice)> = None;
        let mut reasons = Vec::new();

        for pd in devices {
            let props = unsafe { instance.get_physical_device_properties(pd) };
            let name = props
                .device_name_as_c_str()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default();
            if props.api_version < vk::API_VERSION_1_3 {
                reasons.push(format!("{name}: Vulkan 1.3 not supported"));
                continue;
            }
            let exts = unsafe { instance.enumerate_device_extension_properties(pd) }.unwrap_or_default();
            let has = |n: &CStr| exts.iter().any(|e| e.extension_name_as_c_str() == Ok(n));
            if !has(swapchain::NAME) {
                reasons.push(format!("{name}: no swapchain"));
                continue;
            }
            let mut f13 = vk::PhysicalDeviceVulkan13Features::default();
            let mut f_id = vk::PhysicalDevicePresentIdFeaturesKHR::default();
            let mut f_wait = vk::PhysicalDevicePresentWaitFeaturesKHR::default();
            let mut f_id2 = timing::Bool::feature_present_id2();
            let mut f_wait2 = timing::Bool::feature_present_wait2();
            let mut f_timing = timing::PresentTimingFeatures::default();
            let mut f2 = vk::PhysicalDeviceFeatures2::default()
                .push_next(&mut f13)
                .push_next(&mut f_id)
                .push_next(&mut f_wait);
            let v2_exts = has(timing::PRESENT_ID_2)
                && has(timing::PRESENT_WAIT_2)
                && has(timing::PRESENT_TIMING)
                && has(timing::CALIBRATED_TIMESTAMPS);
            if v2_exts {
                unsafe {
                    timing::chain(&mut f2, &mut f_id2);
                    timing::chain(&mut f2, &mut f_wait2);
                    timing::chain(&mut f2, &mut f_timing);
                }
            }
            unsafe { instance.get_physical_device_features2(pd, &mut f2) };
            let v2 = v2_exts
                && f_id2.value == vk::TRUE
                && f_wait2.value == vk::TRUE
                && f_timing.present_timing == vk::TRUE;
            if f13.dynamic_rendering == vk::FALSE || f13.synchronization2 == vk::FALSE {
                reasons.push(format!("{name}: no dynamic rendering/synchronization2"));
                continue;
            }
            let present_ids = has(present_id::NAME)
                && has(present_wait::NAME)
                && f_id.present_id == vk::TRUE
                && f_wait.present_wait == vk::TRUE;

            let families = unsafe { instance.get_physical_device_queue_family_properties(pd) };
            let family = families.iter().enumerate().find_map(|(i, f)| {
                let i = i as u32;
                let present = unsafe { surface_fn.get_physical_device_surface_support(pd, i, surface) }.unwrap_or(false);
                (f.queue_flags.contains(vk::QueueFlags::GRAPHICS) && present).then_some(i)
            });
            let Some(family) = family else {
                reasons.push(format!("{name}: cannot present to the window"));
                continue;
            };
            let score = match props.device_type {
                vk::PhysicalDeviceType::DISCRETE_GPU => 3,
                vk::PhysicalDeviceType::INTEGRATED_GPU => 2,
                vk::PhysicalDeviceType::CPU => 0,
                _ => 1,
            };
            if best.as_ref().is_none_or(|b| score > b.0) {
                best = Some((
                    score,
                    DeviceChoice {
                        physical: pd,
                        family,
                        v1: present_ids,
                        v2,
                        absolute_time: f_timing.present_at_absolute_time == vk::TRUE,
                    },
                ));
            }
        }
        best.map(|b| b.1).ok_or_else(|| {
            if reasons.is_empty() {
                "no Vulkan device found".into()
            } else {
                format!("no usable Vulkan device ({})", reasons.join("; "))
            }
        })
    }

    fn init_objects(&mut self, family: u32) -> Result<()> {
        let d = &self.device;
        unsafe {
            self.cmd_pool = d
                .create_command_pool(
                    &vk::CommandPoolCreateInfo::default()
                        .queue_family_index(family)
                        .flags(vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER),
                    None,
                )
                .map_err(vkerr("command pool"))?;

            let sizes = [
                vk::DescriptorPoolSize::default().ty(vk::DescriptorType::SAMPLED_IMAGE).descriptor_count(16),
                vk::DescriptorPoolSize::default().ty(vk::DescriptorType::SAMPLER).descriptor_count(16),
            ];
            self.desc_pool = d
                .create_descriptor_pool(
                    &vk::DescriptorPoolCreateInfo::default()
                        .flags(vk::DescriptorPoolCreateFlags::FREE_DESCRIPTOR_SET)
                        .max_sets(16)
                        .pool_sizes(&sizes),
                    None,
                )
                .map_err(vkerr("descriptor pool"))?;

            let bindings = [
                vk::DescriptorSetLayoutBinding::default()
                    .binding(0)
                    .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                    .descriptor_count(1)
                    .stage_flags(vk::ShaderStageFlags::FRAGMENT),
                vk::DescriptorSetLayoutBinding::default()
                    .binding(1)
                    .descriptor_type(vk::DescriptorType::SAMPLER)
                    .descriptor_count(1)
                    .stage_flags(vk::ShaderStageFlags::FRAGMENT),
            ];
            self.set_layout = d
                .create_descriptor_set_layout(&vk::DescriptorSetLayoutCreateInfo::default().bindings(&bindings), None)
                .map_err(vkerr("descriptor set layout"))?;

            for (i, filter) in [vk::Filter::NEAREST, vk::Filter::LINEAR].into_iter().enumerate() {
                let ci = vk::SamplerCreateInfo::default()
                    .mag_filter(filter)
                    .min_filter(filter)
                    .mipmap_mode(vk::SamplerMipmapMode::NEAREST)
                    .address_mode_u(vk::SamplerAddressMode::CLAMP_TO_EDGE)
                    .address_mode_v(vk::SamplerAddressMode::CLAMP_TO_EDGE)
                    .address_mode_w(vk::SamplerAddressMode::CLAMP_TO_EDGE)
                    .max_lod(0.0);
                self.samplers[i] = d.create_sampler(&ci, None).map_err(vkerr("sampler"))?;
            }

            let push = [vk::PushConstantRange::default()
                .stage_flags(vk::ShaderStageFlags::VERTEX | vk::ShaderStageFlags::FRAGMENT)
                .size(PUSH_SIZE)];
            let set_layouts = [self.set_layout];
            self.layout = d
                .create_pipeline_layout(
                    &vk::PipelineLayoutCreateInfo::default()
                        .set_layouts(&set_layouts)
                        .push_constant_ranges(&push),
                    None,
                )
                .map_err(vkerr("pipeline layout"))?;

            let cmds = d
                .allocate_command_buffers(
                    &vk::CommandBufferAllocateInfo::default()
                        .command_pool(self.cmd_pool)
                        .level(vk::CommandBufferLevel::PRIMARY)
                        .command_buffer_count(FRAMES as u32),
                )
                .map_err(vkerr("command buffers"))?;
            for cmd in cmds {
                let fence = d
                    .create_fence(&vk::FenceCreateInfo::default().flags(vk::FenceCreateFlags::SIGNALED), None)
                    .map_err(vkerr("fence"))?;
                let acquired = d
                    .create_semaphore(&vk::SemaphoreCreateInfo::default(), None)
                    .map_err(vkerr("semaphore"))?;
                self.frames.push(PerFrame {
                    cmd,
                    fence,
                    acquired,
                    staging: None,
                    vertices: None,
                    readback: None,
                    uploads: None,
                });
            }
        }
        self.create_pipelines()
    }

    fn create_pipelines(&mut self) -> Result<()> {
        let d = &self.device;
        let module = |bytes: &[u8]| -> Result<vk::ShaderModule> {
            let code = spv(bytes);
            unsafe { d.create_shader_module(&vk::ShaderModuleCreateInfo::default().code(&code), None) }
                .map_err(vkerr("shader module"))
        };
        let image_vs = module(include_bytes!(concat!(env!("OUT_DIR"), "/image.vert.spv")))?;
        let image_fs = module(include_bytes!(concat!(env!("OUT_DIR"), "/image.frag.spv")))?;
        let overlay_vs = module(include_bytes!(concat!(env!("OUT_DIR"), "/overlay.vert.spv")))?;
        let overlay_fs = module(include_bytes!(concat!(env!("OUT_DIR"), "/overlay.frag.spv")))?;
        let text_vs = module(include_bytes!(concat!(env!("OUT_DIR"), "/text.vert.spv")))?;
        let text_fs = module(include_bytes!(concat!(env!("OUT_DIR"), "/text.frag.spv")))?;

        let overlay_bindings = [vk::VertexInputBindingDescription::default()
            .binding(0)
            .stride(32)
            .input_rate(vk::VertexInputRate::VERTEX)];
        let overlay_attrs = [
            vk::VertexInputAttributeDescription::default().location(0).format(vk::Format::R32G32_SFLOAT).offset(0),
            vk::VertexInputAttributeDescription::default().location(1).format(vk::Format::R32G32_SFLOAT).offset(8),
            vk::VertexInputAttributeDescription::default()
                .location(2)
                .format(vk::Format::R32G32B32A32_SFLOAT)
                .offset(16),
        ];

        let build = |vs, fs, topology, blend: bool, vertex_input: bool| -> Result<vk::Pipeline> {
            let stages = [
                vk::PipelineShaderStageCreateInfo::default()
                    .stage(vk::ShaderStageFlags::VERTEX)
                    .module(vs)
                    .name(c"main"),
                vk::PipelineShaderStageCreateInfo::default()
                    .stage(vk::ShaderStageFlags::FRAGMENT)
                    .module(fs)
                    .name(c"main"),
            ];
            let vi = if vertex_input {
                vk::PipelineVertexInputStateCreateInfo::default()
                    .vertex_binding_descriptions(&overlay_bindings)
                    .vertex_attribute_descriptions(&overlay_attrs)
            } else {
                vk::PipelineVertexInputStateCreateInfo::default()
            };
            let ia = vk::PipelineInputAssemblyStateCreateInfo::default().topology(topology);
            let vp = vk::PipelineViewportStateCreateInfo::default().viewport_count(1).scissor_count(1);
            let rs = vk::PipelineRasterizationStateCreateInfo::default()
                .polygon_mode(vk::PolygonMode::FILL)
                .cull_mode(vk::CullModeFlags::NONE)
                .line_width(1.0);
            let ms = vk::PipelineMultisampleStateCreateInfo::default().rasterization_samples(vk::SampleCountFlags::TYPE_1);
            // glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA) for color and alpha
            let attachments = [vk::PipelineColorBlendAttachmentState::default()
                .blend_enable(blend)
                .src_color_blend_factor(vk::BlendFactor::SRC_ALPHA)
                .dst_color_blend_factor(vk::BlendFactor::ONE_MINUS_SRC_ALPHA)
                .color_blend_op(vk::BlendOp::ADD)
                .src_alpha_blend_factor(vk::BlendFactor::SRC_ALPHA)
                .dst_alpha_blend_factor(vk::BlendFactor::ONE_MINUS_SRC_ALPHA)
                .alpha_blend_op(vk::BlendOp::ADD)
                .color_write_mask(vk::ColorComponentFlags::RGBA)];
            let cb = vk::PipelineColorBlendStateCreateInfo::default().attachments(&attachments);
            let dynamic = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
            let ds = vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dynamic);
            let formats = [self.format];
            let mut rendering = vk::PipelineRenderingCreateInfo::default().color_attachment_formats(&formats);
            let ci = vk::GraphicsPipelineCreateInfo::default()
                .stages(&stages)
                .vertex_input_state(&vi)
                .input_assembly_state(&ia)
                .viewport_state(&vp)
                .rasterization_state(&rs)
                .multisample_state(&ms)
                .color_blend_state(&cb)
                .dynamic_state(&ds)
                .layout(self.layout)
                .push_next(&mut rendering);
            unsafe { d.create_graphics_pipelines(vk::PipelineCache::null(), &[ci], None) }
                .map(|p| p[0])
                .map_err(|(_, e)| format!("graphics pipeline: {e:?}"))
        };

        let result = (|| -> Result<Pipelines> {
            Ok(Pipelines {
                image: build(image_vs, image_fs, vk::PrimitiveTopology::TRIANGLE_STRIP, false, false)?,
                overlay: build(overlay_vs, overlay_fs, vk::PrimitiveTopology::TRIANGLE_LIST, true, true)?,
                text: build(text_vs, text_fs, vk::PrimitiveTopology::TRIANGLE_STRIP, true, false)?,
            })
        })();
        unsafe {
            for m in [image_vs, image_fs, overlay_vs, overlay_fs, text_vs, text_fs] {
                d.destroy_shader_module(m, None);
            }
        }
        self.pipelines = result?;
        Ok(())
    }

    pub fn device_name(&self) -> &CStr {
        &self.device_name
    }

    // ------------------------------------------------------------ memory helpers

    fn memory_type(&self, bits: u32, flags: vk::MemoryPropertyFlags) -> Result<u32> {
        (0..self.memory_props.memory_type_count)
            .find(|&i| {
                bits & (1 << i) != 0 && self.memory_props.memory_types[i as usize].property_flags.contains(flags)
            })
            .ok_or_else(|| format!("no memory type {flags:?}"))
    }

    fn create_buffer(&self, size: u64, usage: vk::BufferUsageFlags) -> Result<Buffer> {
        let d = &self.device;
        unsafe {
            let buffer = d
                .create_buffer(
                    &vk::BufferCreateInfo::default()
                        .size(size)
                        .usage(usage)
                        .sharing_mode(vk::SharingMode::EXCLUSIVE),
                    None,
                )
                .map_err(vkerr("buffer"))?;
            let req = d.get_buffer_memory_requirements(buffer);
            let ty = self.memory_type(
                req.memory_type_bits,
                vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT,
            );
            let ty = match ty {
                Ok(t) => t,
                Err(e) => {
                    d.destroy_buffer(buffer, None);
                    return Err(e);
                }
            };
            let memory = match d.allocate_memory(
                &vk::MemoryAllocateInfo::default().allocation_size(req.size).memory_type_index(ty),
                None,
            ) {
                Ok(m) => m,
                Err(e) => {
                    d.destroy_buffer(buffer, None);
                    return Err(format!("buffer memory: {e:?}"));
                }
            };
            let bound = d.bind_buffer_memory(buffer, memory, 0);
            let ptr = bound.and_then(|_| d.map_memory(memory, 0, vk::WHOLE_SIZE, vk::MemoryMapFlags::empty()));
            match ptr {
                Ok(p) => Ok(Buffer {
                    buffer,
                    memory,
                    size,
                    ptr: p as *mut u8,
                }),
                Err(e) => {
                    d.destroy_buffer(buffer, None);
                    d.free_memory(memory, None);
                    Err(format!("map buffer: {e:?}"))
                }
            }
        }
    }

    fn destroy_buffer(&self, b: Buffer) {
        unsafe {
            self.device.destroy_buffer(b.buffer, None);
            self.device.free_memory(b.memory, None);
        }
    }

    /// makes sure `slot` holds a buffer of at least `size` bytes (frame fence already waited)
    fn ensure_buffer(&self, slot: &mut Option<Buffer>, size: u64, usage: vk::BufferUsageFlags) -> Result<()> {
        if slot.as_ref().is_some_and(|b| b.size >= size) {
            return Ok(());
        }
        if let Some(old) = slot.take() {
            self.destroy_buffer(old);
        }
        // grow in steps to avoid reallocating on every small change
        let size = size.next_power_of_two().max(4096);
        *slot = Some(self.create_buffer(size, usage)?);
        Ok(())
    }

    fn create_texture(&self, width: u32, height: u32, format: vk::Format) -> Result<Texture> {
        let d = &self.device;
        unsafe {
            let image = d
                .create_image(
                    &vk::ImageCreateInfo::default()
                        .image_type(vk::ImageType::TYPE_2D)
                        .format(format)
                        .extent(vk::Extent3D { width, height, depth: 1 })
                        .mip_levels(1)
                        .array_layers(1)
                        .samples(vk::SampleCountFlags::TYPE_1)
                        .tiling(vk::ImageTiling::OPTIMAL)
                        .usage(vk::ImageUsageFlags::SAMPLED | vk::ImageUsageFlags::TRANSFER_DST)
                        .initial_layout(vk::ImageLayout::UNDEFINED),
                    None,
                )
                .map_err(vkerr("image"))?;
            let req = d.get_image_memory_requirements(image);
            let mut tex = Texture {
                image,
                memory: vk::DeviceMemory::null(),
                view: vk::ImageView::null(),
                width,
                height,
                format,
                sets: [vk::DescriptorSet::null(); 2],
                ready: false,
            };
            let res = (|| -> Result<()> {
                let ty = self.memory_type(req.memory_type_bits, vk::MemoryPropertyFlags::DEVICE_LOCAL)?;
                tex.memory = d
                    .allocate_memory(
                        &vk::MemoryAllocateInfo::default().allocation_size(req.size).memory_type_index(ty),
                        None,
                    )
                    .map_err(vkerr("image memory"))?;
                d.bind_image_memory(image, tex.memory, 0).map_err(vkerr("bind image"))?;
                tex.view = d
                    .create_image_view(
                        &vk::ImageViewCreateInfo::default()
                            .image(image)
                            .view_type(vk::ImageViewType::TYPE_2D)
                            .format(format)
                            .subresource_range(color_range()),
                        None,
                    )
                    .map_err(vkerr("image view"))?;
                let layouts = [self.set_layout; 2];
                let sets = d
                    .allocate_descriptor_sets(
                        &vk::DescriptorSetAllocateInfo::default()
                            .descriptor_pool(self.desc_pool)
                            .set_layouts(&layouts),
                    )
                    .map_err(vkerr("descriptor sets"))?;
                tex.sets = [sets[0], sets[1]];
                for (i, set) in tex.sets.iter().enumerate() {
                    let img = [vk::DescriptorImageInfo::default()
                        .image_view(tex.view)
                        .image_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)];
                    let smp = [vk::DescriptorImageInfo::default().sampler(self.samplers[i])];
                    let writes = [
                        vk::WriteDescriptorSet::default()
                            .dst_set(*set)
                            .dst_binding(0)
                            .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                            .image_info(&img),
                        vk::WriteDescriptorSet::default()
                            .dst_set(*set)
                            .dst_binding(1)
                            .descriptor_type(vk::DescriptorType::SAMPLER)
                            .image_info(&smp),
                    ];
                    d.update_descriptor_sets(&writes, &[]);
                }
                Ok(())
            })();
            match res {
                Ok(()) => Ok(tex),
                Err(e) => {
                    self.destroy_texture(tex);
                    Err(e)
                }
            }
        }
    }

    fn destroy_texture(&self, t: Texture) {
        let d = &self.device;
        unsafe {
            if t.sets[0] != vk::DescriptorSet::null() {
                let _ = d.free_descriptor_sets(self.desc_pool, &t.sets);
            }
            if t.view != vk::ImageView::null() {
                d.destroy_image_view(t.view, None);
            }
            d.destroy_image(t.image, None);
            if t.memory != vk::DeviceMemory::null() {
                d.free_memory(t.memory, None);
            }
        }
    }

    // ------------------------------------------------------------ textures (atlas, text)

    /// Queues a texture upload; it is recorded into the next frame's command buffer (no wait
    /// for the device unless the size changes). Width/height 0 removes the texture.
    pub fn set_texture(&mut self, slot: i32, format: i32, width: u32, height: u32, data: &[u8]) -> Result<()> {
        let index = match slot {
            BVK_TEX_ATLAS => 0,
            BVK_TEX_TEXT => 1,
            _ => return Err(format!("unknown texture slot {slot}")),
        };
        if width == 0 || height == 0 || data.is_empty() {
            self.pending[index] = None;
            unsafe { self.device.device_wait_idle() }.map_err(vkerr("wait idle"))?;
            if let Some(t) = self.slot_mut(index).take() {
                self.destroy_texture(t);
            }
            return Ok(());
        }
        let format = if format == BVK_FMT_R8 {
            vk::Format::R8_UNORM
        } else {
            vk::Format::R8G8B8A8_UNORM
        };
        self.pending[index] = Some(PendingTexture {
            format,
            width,
            height,
            data: data.to_vec(),
        });
        Ok(())
    }

    fn slot_mut(&mut self, index: usize) -> &mut Option<Texture> {
        if index == 0 { &mut self.atlas } else { &mut self.text }
    }

    /// creates textures for pending uploads whose size or format changed (device idle then)
    fn prepare_textures(&mut self) -> Result<()> {
        for index in 0..2 {
            let Some((format, width, height)) = self.pending[index].as_ref().map(|p| (p.format, p.width, p.height)) else {
                continue;
            };
            let fits = self.slot_mut(index).as_ref().is_some_and(|t| t.width == width && t.height == height && t.format == format);
            if !fits {
                unsafe { self.device.device_wait_idle() }.map_err(vkerr("wait idle"))?;
                if let Some(t) = self.slot_mut(index).take() {
                    self.destroy_texture(t);
                }
                let tex = self.create_texture(width, height, format)?;
                *self.slot_mut(index) = Some(tex);
            }
        }
        Ok(())
    }

    // ------------------------------------------------------------ swapchain

    fn destroy_swapchain_views(&mut self) {
        unsafe {
            for v in self.views.drain(..) {
                self.device.destroy_image_view(v, None);
            }
            for s in self.render_done.drain(..) {
                self.device.destroy_semaphore(s, None);
            }
        }
        self.images.clear();
    }

    /// returns false when the window has no size (nothing can be presented)
    fn recreate_swapchain(&mut self, width: u32, height: u32) -> Result<bool> {
        unsafe { self.device.device_wait_idle() }.map_err(vkerr("wait idle"))?;
        let caps = unsafe {
            self.surface_fn
                .get_physical_device_surface_capabilities(self.physical, self.surface)
        }
        .map_err(vkerr("surface capabilities"))?;

        let extent = if caps.current_extent.width != u32::MAX {
            caps.current_extent
        } else {
            vk::Extent2D {
                width: width.clamp(caps.min_image_extent.width, caps.max_image_extent.width.max(1)),
                height: height.clamp(caps.min_image_extent.height, caps.max_image_extent.height.max(1)),
            }
        };
        self.requested = vk::Extent2D { width, height };
        if extent.width == 0 || extent.height == 0 {
            return Ok(false);
        }

        let mut count = caps.min_image_count + 1;
        if caps.max_image_count > 0 {
            count = count.min(caps.max_image_count);
        }
        let mut usage = vk::ImageUsageFlags::COLOR_ATTACHMENT;
        if self.can_read_back {
            usage |= vk::ImageUsageFlags::TRANSFER_SRC;
        }
        // no vsync: warp prefers MAILBOX (no tearing), lowest latency IMMEDIATE (tearing allowed)
        let order = match self.present_pref {
            BVK_PRESENT_FIFO => [vk::PresentModeKHR::FIFO; 2],
            BVK_PRESENT_IMMEDIATE => [vk::PresentModeKHR::IMMEDIATE, vk::PresentModeKHR::MAILBOX],
            _ => [vk::PresentModeKHR::MAILBOX, vk::PresentModeKHR::IMMEDIATE],
        };
        let present_mode = order
            .into_iter()
            .find(|m| self.present_modes.contains(m))
            .unwrap_or(vk::PresentModeKHR::FIFO);
        let flags = if self.present_api == PresentApi::V2 {
            vk::SwapchainCreateFlagsKHR::from_raw(
                timing::SWAPCHAIN_CREATE_PRESENT_ID_2
                    | timing::SWAPCHAIN_CREATE_PRESENT_WAIT_2
                    | if self.feedback.stage != 0 { timing::SWAPCHAIN_CREATE_PRESENT_TIMING } else { 0 },
            )
        } else {
            vk::SwapchainCreateFlagsKHR::empty()
        };
        let old = self.swapchain;
        let ci = vk::SwapchainCreateInfoKHR::default()
            .flags(flags)
            .surface(self.surface)
            .min_image_count(count)
            .image_format(self.format)
            .image_color_space(vk::ColorSpaceKHR::SRGB_NONLINEAR)
            .image_extent(extent)
            .image_array_layers(1)
            .image_usage(usage)
            .image_sharing_mode(vk::SharingMode::EXCLUSIVE)
            .pre_transform(caps.current_transform)
            .composite_alpha(self.composite_alpha)
            .present_mode(present_mode)
            .clipped(true)
            .old_swapchain(old);
        let swapchain = unsafe { self.swapchain_fn.create_swapchain(&ci, None) }.map_err(vkerr("vkCreateSwapchainKHR"))?;

        self.destroy_swapchain_views();
        if old != vk::SwapchainKHR::null() {
            unsafe { self.swapchain_fn.destroy_swapchain(old, None) };
        }
        self.swapchain = swapchain;
        self.extent = extent;
        self.swapchain_last_id = 0;
        self.swapchain_first_id = u64::MAX;
        self.feedback.flush();
        self.feedback.setup_swapchain(self.device.handle(), swapchain);
        self.images = unsafe { self.swapchain_fn.get_swapchain_images(swapchain) }.map_err(vkerr("swapchain images"))?;
        for &image in &self.images {
            let view = unsafe {
                self.device.create_image_view(
                    &vk::ImageViewCreateInfo::default()
                        .image(image)
                        .view_type(vk::ImageViewType::TYPE_2D)
                        .format(self.format)
                        .subresource_range(color_range()),
                    None,
                )
            }
            .map_err(vkerr("swapchain view"))?;
            self.views.push(view);
            let sem = unsafe { self.device.create_semaphore(&vk::SemaphoreCreateInfo::default(), None) }
                .map_err(vkerr("semaphore"))?;
            self.render_done.push(sem);
        }
        self.swapchain_dirty = false;
        Ok(true)
    }

    // ------------------------------------------------------------ frame

    pub fn set_present_mode(&mut self, pref: i32) {
        if self.present_pref != pref {
            self.present_pref = pref;
            self.swapchain_dirty = true;
        }
    }

    pub fn set_pacing(&mut self, repeat: u32, queue_depth: u32, period_ns: u64) {
        self.repeat = repeat.clamp(1, 8);
        self.queue_depth = queue_depth.min(3);
        if self.grid.period != period_ns {
            self.grid = Grid::new();
            self.grid.period = period_ns;
        }
        self.feedback.set_grid(self.grid.period);
    }

    /// target times can be used (the program only enables the grid then)
    pub fn absolute_time(&self) -> bool {
        self.absolute_time && self.feedback.timing_active
    }

    /// grid presents are displayed at the grid cadence (VRR follows the emulation)
    pub fn grid_cadence(&self) -> bool {
        self.feedback.cadence()
    }

    pub fn present_api(&self) -> PresentApi {
        self.present_api
    }

    pub fn stalled(&self) -> bool {
        self.stalled
    }

    pub fn timing_active(&self) -> bool {
        self.feedback.timing_active
    }

    pub fn timing_domain(&self) -> i32 {
        self.feedback.domain
    }

    pub fn refresh_ns(&self) -> u64 {
        self.feedback.refresh_ns
    }

    pub fn take_presents(&mut self, out: &mut [Record]) -> usize {
        if self.swapchain != vk::SwapchainKHR::null() {
            self.feedback.poll(self.device.handle(), self.swapchain);
        }
        self.feedback.take(out)
    }

    /// Renders `f` and presents it; a new emulator picture (`frame_seq` changed) is presented
    /// `repeat` times, redraws once.
    ///
    /// # Safety
    /// pointers in `f` must be valid for their sizes.
    pub unsafe fn render(&mut self, f: &BvkFrame) -> Result<Rendered> {
        let t0 = timing::now_ns();
        let res = unsafe { self.render_inner(f) };
        if self.debug_slow {
            let dt = timing::now_ns() - t0;
            if dt > 30_000_000 {
                eprintln!("badline vulkan: render took {} ms (seq {})", dt / 1_000_000, f.frame_seq);
            }
        }
        res
    }

    unsafe fn render_inner(&mut self, f: &BvkFrame) -> Result<Rendered> {
        // recovered once a present is displayed again (polled without waiting); no time based
        // retry: every stall/recover flip re-evaluates the pacing in the program
        // (only presents made after the stall count: older ones may have been shown before)
        if self.stalled
            && self.swapchain != vk::SwapchainKHR::null()
            && self.swapchain_last_id > self.stall_id
            && self.swapchain_last_id >= self.swapchain_first_id
            && self.present_done(self.swapchain_last_id, 0)
        {
            self.feedback.waited(self.swapchain_last_id, timing::now_ns());
            self.stalled = false;
        }
        let new_frame = f.frame_seq != 0 && f.frame_seq != self.last_frame;
        if new_frame {
            self.last_frame = f.frame_seq;
        }
        let flags = if new_frame { 0 } else { timing::REC_REDRAW };
        // grid: a new picture waits for its slot and is presented with the slot as target
        // time; redraws (paused emulation) go out at once. The wait also clocks the emulation
        // when a rebuilt swapchain lost the target time support (presented as soon as possible)
        self.target_time = 0;
        if new_frame && self.grid.period > 0 && !self.stalled && self.present_pref == BVK_PRESENT_FIFO {
            let slot = self.grid.next_slot();
            if self.absolute_time() {
                self.target_time = slot;
            }
        }
        let first = unsafe { self.render_once(f, 0, flags) }?;
        self.target_time = 0;
        if !new_frame || self.repeat <= 1 || matches!(first, Rendered::Skipped) {
            return Ok(first);
        }
        // the copies show the same picture (already uploaded) without screenshot
        let mut copy = *f;
        copy.frame = std::ptr::null();
        copy.screenshot = std::ptr::null_mut();
        for n in 1..self.repeat {
            unsafe { self.render_once(&copy, n, timing::REC_REPEAT) }?;
        }
        Ok(first)
    }

    unsafe fn render_once(&mut self, f: &BvkFrame, copy: u32, flags: u32) -> Result<Rendered> {
        let (ww, wh) = (f.window_width, f.window_height);
        if ww == 0 || wh == 0 {
            return Ok(Rendered::Skipped);
        }
        let fi = self.current;
        let fence = self.frames[fi].fence;
        unsafe { self.device.wait_for_fences(&[fence], true, u64::MAX) }.map_err(vkerr("wait fence"))?;

        let wanted = vk::Extent2D { width: ww, height: wh };
        if (self.swapchain_dirty || self.requested != wanted) && !self.recreate_swapchain(ww, wh)? {
            return Ok(Rendered::Skipped);
        }

        // everything that can fail or wait happens before the acquire: an acquired image must
        // be presented, its semaphore must be waited on
        let upload = !f.frame.is_null() && f.frame_width > 0 && f.frame_height > 0;
        if upload
            && self
                .picture
                .as_ref()
                .is_none_or(|t| t.width != f.frame_width || t.height != f.frame_height)
        {
            unsafe { self.device.device_wait_idle() }.map_err(vkerr("wait idle"))?;
            if let Some(t) = self.picture.take() {
                self.destroy_texture(t);
            }
            self.picture = Some(self.create_texture(f.frame_width, f.frame_height, vk::Format::B8G8R8A8_UNORM)?);
        }
        self.prepare_textures()?;

        let mut frame = std::mem::replace(
            &mut self.frames[fi],
            PerFrame {
                cmd: vk::CommandBuffer::null(),
                fence,
                acquired: vk::Semaphore::null(),
                staging: None,
                vertices: None,
                readback: None,
                uploads: None,
            },
        );
        let res = unsafe { self.prepare(f, &mut frame, upload) };
        let prep = match res {
            Ok(p) => p,
            Err(e) => {
                self.frames[fi] = frame;
                return Err(e);
            }
        };

        // finite timeout: FIFO may block for long while the window is hidden; the emu thread
        // must not hang there (the frame is dropped instead). Once stalled, do not wait at all,
        // otherwise every frame costs the timeout (~10 fps while hidden)
        let timeout = if self.stalled { 0 } else { 100_000_000 };
        let acquired = unsafe {
            self.swapchain_fn
                .acquire_next_image(self.swapchain, timeout, frame.acquired, vk::Fence::null())
        };
        let index = match acquired {
            Ok((i, suboptimal)) => {
                if suboptimal {
                    self.swapchain_dirty = true;
                }
                i
            }
            Err(vk::Result::ERROR_OUT_OF_DATE_KHR) => {
                self.swapchain_dirty = true;
                self.frames[fi] = frame;
                return Ok(Rendered::Skipped);
            }
            Err(vk::Result::TIMEOUT) | Err(vk::Result::NOT_READY) => {
                self.frames[fi] = frame;
                self.set_stalled();
                return Ok(Rendered::Skipped);
            }
            Err(e) => {
                self.frames[fi] = frame;
                return Err(format!("vkAcquireNextImageKHR: {e:?}"));
            }
        };

        let res = unsafe { self.record_and_submit(f, &mut frame, index, &prep, copy, flags) };
        self.frames[fi] = frame;
        res?;
        self.current = (fi + 1) % FRAMES;

        if let Some((_, _, w, h)) = prep.shot {
            unsafe { self.device.wait_for_fences(&[fence], true, u64::MAX) }.map_err(vkerr("wait fence"))?;
            let buf = self.frames[fi].readback.as_ref().unwrap();
            let bgr = self.format == vk::Format::B8G8R8A8_UNORM;
            let (w, h) = (w as usize, h as usize);
            let src = unsafe { std::slice::from_raw_parts(buf.ptr, w * h * 4) };
            let dst = unsafe { std::slice::from_raw_parts_mut(f.screenshot, w * h * 3) };
            for (s, d) in src.chunks_exact(4).zip(dst.chunks_exact_mut(3)) {
                if bgr {
                    d.copy_from_slice(&[s[2], s[1], s[0]]);
                } else {
                    d.copy_from_slice(&s[..3]);
                }
            }
            return Ok(Rendered::WithScreenshot);
        }
        Ok(if f.screenshot.is_null() { Rendered::Presented } else { Rendered::NoScreenshot })
    }

    /// host side of a frame (fence already waited): staging copies, overlay vertices,
    /// pending texture data, screenshot buffer
    unsafe fn prepare(&mut self, f: &BvkFrame, frame: &mut PerFrame, upload: bool) -> Result<Prepared> {
        let extent = self.extent;
        if upload {
            let (w, h, pitch) = (f.frame_width as usize, f.frame_height as usize, f.frame_pitch as usize);
            self.ensure_buffer(&mut frame.staging, (w * h * 4) as u64, vk::BufferUsageFlags::TRANSFER_SRC)?;
            let dst = frame.staging.as_ref().unwrap().ptr as *mut u32;
            unsafe {
                if pitch == w {
                    std::ptr::copy_nonoverlapping(f.frame, dst, w * h);
                } else {
                    for y in 0..h {
                        std::ptr::copy_nonoverlapping(f.frame.add(y * pitch), dst.add(y * w), w);
                    }
                }
            }
        }
        let overlay_count = if self.atlas.is_some() && !f.overlay_vertices.is_null() {
            f.overlay_vertex_count
        } else {
            0
        };
        if overlay_count > 0 {
            let bytes = overlay_count as usize * 32;
            self.ensure_buffer(&mut frame.vertices, bytes as u64, vk::BufferUsageFlags::VERTEX_BUFFER)?;
            unsafe {
                std::ptr::copy_nonoverlapping(
                    f.overlay_vertices as *const u8,
                    frame.vertices.as_ref().unwrap().ptr,
                    bytes,
                )
            };
        }

        let mut textures = [false; 2];
        let total: usize = self.pending.iter().flatten().map(|p| p.data.len()).sum();
        if total > 0 {
            self.ensure_buffer(&mut frame.uploads, total as u64, vk::BufferUsageFlags::TRANSFER_SRC)?;
            let mut offset = 0;
            for (index, done) in textures.iter_mut().enumerate() {
                if let Some(p) = self.pending[index].take() {
                    unsafe {
                        std::ptr::copy_nonoverlapping(p.data.as_ptr(), frame.uploads.as_ref().unwrap().ptr.add(offset), p.data.len())
                    };
                    offset += p.data.len();
                    *done = true;
                }
            }
        }

        let mut shot = None;
        if !f.screenshot.is_null() && self.can_read_back && f.shot_x >= 0 && f.shot_y >= 0 {
            let (x, y) = (f.shot_x as u32, f.shot_y as u32);
            let (w, h) = (f.shot_width.max(0) as u32, f.shot_height.max(0) as u32);
            if w > 0 && h > 0 && x + w <= extent.width && y + h <= extent.height {
                self.ensure_buffer(&mut frame.readback, (w as u64) * (h as u64) * 4, vk::BufferUsageFlags::TRANSFER_DST)?;
                shot = Some((x, y, w, h));
            }
        }
        Ok(Prepared {
            upload,
            overlay_count,
            textures,
            shot,
        })
    }

    unsafe fn record_and_submit(
        &mut self,
        f: &BvkFrame,
        frame: &mut PerFrame,
        index: u32,
        prep: &Prepared,
        copy: u32,
        flags: u32,
    ) -> Result<()> {
        let d = self.device.clone();
        let cmd = frame.cmd;
        let target = self.images[index as usize];
        let extent = self.extent;
        let upload = prep.upload;
        let overlay_count = prep.overlay_count;
        let shot = prep.shot;

        unsafe {
            d.reset_command_buffer(cmd, vk::CommandBufferResetFlags::empty())
                .map_err(vkerr("reset command buffer"))?;
            d.begin_command_buffer(
                cmd,
                &vk::CommandBufferBeginInfo::default().flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT),
            )
            .map_err(vkerr("begin command buffer"))?;

            if upload {
                let tex = self.picture.as_mut().unwrap();
                record_upload(&d, cmd, tex, frame.staging.as_ref().unwrap().buffer, 0, f.frame_width);
            }
            let mut offset = 0u64;
            for (index, &queued) in prep.textures.iter().enumerate() {
                if queued {
                    let buffer = frame.uploads.as_ref().unwrap().buffer;
                    let tex = self.slot_mut(index).as_mut().unwrap();
                    let bytes = tex.width as u64 * tex.height as u64 * if tex.format == vk::Format::R8_UNORM { 1 } else { 4 };
                    record_upload(&d, cmd, tex, buffer, offset, tex.width);
                    offset += bytes;
                }
            }

            image_barrier(
                &d,
                cmd,
                target,
                vk::ImageLayout::UNDEFINED,
                vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
                (vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT, vk::AccessFlags2::NONE),
                (
                    vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
                    vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
                ),
            );

            let attachments = [vk::RenderingAttachmentInfo::default()
                .image_view(self.views[index as usize])
                .image_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
                .load_op(vk::AttachmentLoadOp::CLEAR)
                .store_op(vk::AttachmentStoreOp::STORE)
                .clear_value(vk::ClearValue {
                    color: vk::ClearColorValue {
                        float32: [0.0, 0.0, 0.0, 1.0],
                    },
                })];
            let full = vk::Rect2D {
                offset: vk::Offset2D { x: 0, y: 0 },
                extent,
            };
            d.cmd_begin_rendering(
                cmd,
                &vk::RenderingInfo::default()
                    .render_area(full)
                    .layer_count(1)
                    .color_attachments(&attachments),
            );
            d.cmd_set_scissor(cmd, 0, &[full]);

            let picture_vp = vk::Viewport {
                x: f.vp_x as f32,
                y: f.vp_y as f32,
                width: f.vp_width.max(1) as f32,
                height: f.vp_height.max(1) as f32,
                min_depth: 0.0,
                max_depth: 1.0,
            };

            if let Some(tex) = self.picture.as_ref().filter(|t| t.ready && f.show_frame != 0) {
                d.cmd_set_viewport(cmd, 0, &[picture_vp]);
                d.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, self.pipelines.image);
                let set = tex.sets[(f.linear != 0) as usize];
                d.cmd_bind_descriptor_sets(cmd, vk::PipelineBindPoint::GRAPHICS, self.layout, 0, &[set], &[]);
                d.cmd_push_constants(
                    cmd,
                    self.layout,
                    vk::ShaderStageFlags::VERTEX | vk::ShaderStageFlags::FRAGMENT,
                    0,
                    as_bytes(&f.mvp),
                );
                d.cmd_draw(cmd, 4, 1, 0, 0);
            }

            if overlay_count > 0 {
                let atlas = self.atlas.as_ref().unwrap();
                d.cmd_set_viewport(
                    cmd,
                    0,
                    &[vk::Viewport {
                        x: 0.0,
                        y: 0.0,
                        width: extent.width as f32,
                        height: extent.height as f32,
                        min_depth: 0.0,
                        max_depth: 1.0,
                    }],
                );
                d.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, self.pipelines.overlay);
                d.cmd_bind_descriptor_sets(cmd, vk::PipelineBindPoint::GRAPHICS, self.layout, 0, &[atlas.sets[0]], &[]);
                let size = [f.overlay_width.max(1.0), f.overlay_height.max(1.0)];
                d.cmd_push_constants(
                    cmd,
                    self.layout,
                    vk::ShaderStageFlags::VERTEX | vk::ShaderStageFlags::FRAGMENT,
                    0,
                    as_bytes(&size),
                );
                d.cmd_bind_vertex_buffers(cmd, 0, &[frame.vertices.as_ref().unwrap().buffer], &[0]);
                d.cmd_draw(cmd, overlay_count, 1, 0, 0);
            }

            if let Some(text) = self.text.as_ref().filter(|_| f.text_visible != 0) {
                // GL draws the text with the picture viewport still bound
                d.cmd_set_viewport(cmd, 0, &[picture_vp]);
                d.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, self.pipelines.text);
                d.cmd_bind_descriptor_sets(cmd, vk::PipelineBindPoint::GRAPHICS, self.layout, 0, &[text.sets[0]], &[]);
                let mut push = [0f32; 24];
                push[..16].copy_from_slice(&f.text_quad);
                push[16..20].copy_from_slice(&f.text_color);
                push[20..24].copy_from_slice(&f.text_bg_color);
                d.cmd_push_constants(
                    cmd,
                    self.layout,
                    vk::ShaderStageFlags::VERTEX | vk::ShaderStageFlags::FRAGMENT,
                    0,
                    as_bytes(&push),
                );
                d.cmd_draw(cmd, 4, 1, 0, 0);
            }

            d.cmd_end_rendering(cmd);

            if let Some((x, y, w, h)) = shot {
                image_barrier(
                    &d,
                    cmd,
                    target,
                    vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
                    vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                    (
                        vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
                        vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
                    ),
                    (vk::PipelineStageFlags2::COPY, vk::AccessFlags2::TRANSFER_READ),
                );
                let region = vk::BufferImageCopy::default()
                    .image_subresource(color_layers())
                    .image_offset(vk::Offset3D {
                        x: x as i32,
                        y: y as i32,
                        z: 0,
                    })
                    .image_extent(vk::Extent3D {
                        width: w,
                        height: h,
                        depth: 1,
                    });
                d.cmd_copy_image_to_buffer(
                    cmd,
                    target,
                    vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                    frame.readback.as_ref().unwrap().buffer,
                    &[region],
                );
                image_barrier(
                    &d,
                    cmd,
                    target,
                    vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                    vk::ImageLayout::PRESENT_SRC_KHR,
                    (vk::PipelineStageFlags2::COPY, vk::AccessFlags2::TRANSFER_READ),
                    (vk::PipelineStageFlags2::NONE, vk::AccessFlags2::NONE),
                );
                // host read of the readback buffer after the fence
                let barrier = [vk::MemoryBarrier2::default()
                    .src_stage_mask(vk::PipelineStageFlags2::COPY)
                    .src_access_mask(vk::AccessFlags2::TRANSFER_WRITE)
                    .dst_stage_mask(vk::PipelineStageFlags2::HOST)
                    .dst_access_mask(vk::AccessFlags2::HOST_READ)];
                d.cmd_pipeline_barrier2(cmd, &vk::DependencyInfo::default().memory_barriers(&barrier));
            } else {
                image_barrier(
                    &d,
                    cmd,
                    target,
                    vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
                    vk::ImageLayout::PRESENT_SRC_KHR,
                    (
                        vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
                        vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
                    ),
                    (vk::PipelineStageFlags2::NONE, vk::AccessFlags2::NONE),
                );
            }

            d.end_command_buffer(cmd).map_err(vkerr("end command buffer"))?;

            let waits = [vk::SemaphoreSubmitInfo::default()
                .semaphore(frame.acquired)
                .stage_mask(vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT)];
            let signals = [vk::SemaphoreSubmitInfo::default()
                .semaphore(self.render_done[index as usize])
                .stage_mask(vk::PipelineStageFlags2::ALL_COMMANDS)];
            let cmds = [vk::CommandBufferSubmitInfo::default().command_buffer(cmd)];
            let submit = vk::SubmitInfo2::default()
                .wait_semaphore_infos(&waits)
                .command_buffer_infos(&cmds)
                .signal_semaphore_infos(&signals);
            d.reset_fences(&[frame.fence]).map_err(vkerr("reset fence"))?;
            d.queue_submit2(self.queue, &[submit], frame.fence)
                .map_err(vkerr("vkQueueSubmit2"))?;

            // video master: the display clocks the emulation, at most one present queued
            // ids on one swapchain are consecutive from `swapchain_first_id`
            let depth = self.queue_depth as u64;
            if depth > 0 && !self.stalled && self.present_pref == BVK_PRESENT_FIFO && self.swapchain_last_id > 0 {
                let target = (self.swapchain_last_id + 1).saturating_sub(depth);
                if target >= self.swapchain_first_id {
                    self.wait_present(target);
                }
            }

            let wait = [self.render_done[index as usize]];
            let swapchains = [self.swapchain];
            let indices = [index];
            self.present_counter += 1;
            let id = self.present_counter;
            let ids = [id];
            let mut present_ids = vk::PresentIdKHR::default().present_ids(&ids);
            let mut present_id2 = timing::PresentId2::new(&ids[0]);
            let mut timing_info = self.feedback.timing_info();
            if let Some(t) = timing_info.as_mut()
                && self.target_time != 0
            {
                // the target refers to the stage that is queried (display), required for the
                // PRESENT_STAGE_LOCAL time domain
                t.target_time = self.target_time;
                t.target_time_domain_present_stage = t.present_stage_queries;
            }
            let mut timings = timing_info.as_ref().map(timing::PresentTimingsInfo::new);
            let mut present = vk::PresentInfoKHR::default()
                .wait_semaphores(&wait)
                .swapchains(&swapchains)
                .image_indices(&indices);
            match self.present_api {
                PresentApi::V1 => present = present.push_next(&mut present_ids),
                PresentApi::V2 => {
                    timing::chain(&mut present, &mut present_id2);
                    if let Some(t) = timings.as_mut() {
                        timing::chain(&mut present, t);
                    }
                }
                PresentApi::None => {}
            }
            let submit_ns = timing::now_ns();
            self.swapchain_last_id = id;
            self.swapchain_first_id = self.swapchain_first_id.min(id);
            let timed = timings.is_some();
            match self.swapchain_fn.queue_present(self.queue, &present) {
                Ok(false) => {}
                Ok(true) | Err(vk::Result::ERROR_OUT_OF_DATE_KHR) => self.swapchain_dirty = true,
                // guarded by Feedback::timing_info; a rebuilt swapchain starts with an empty queue
                Err(e) if e.as_raw() == timing::ERROR_PRESENT_TIMING_QUEUE_FULL => {
                    self.swapchain_dirty = true;
                    return Ok(());
                }
                Err(e) => return Err(format!("vkQueuePresentKHR: {e:?}")),
            }
            if self.present_api != PresentApi::None {
                self.feedback.push(Record {
                    present_id: id,
                    frame: f.frame_seq,
                    submit_ns,
                    display_ns: 0,
                    flags,
                    copy,
                }, timed);
                self.feedback.poll(self.device.handle(), self.swapchain);
            }
        }
        Ok(())
    }

    fn set_stalled(&mut self) {
        if !self.stalled {
            self.stalled = true;
            self.stall_id = self.swapchain_last_id;
        }
    }

    fn present_done(&self, id: u64, timeout: u64) -> bool {
        match self.present_api {
            PresentApi::V2 => self.feedback.wait2(self.device.handle(), self.swapchain, id, timeout),
            PresentApi::V1 => self
                .present_wait_fn
                .as_ref()
                .is_some_and(|w| unsafe { w.wait_for_present(self.swapchain, id, timeout) }.is_ok()),
            PresentApi::None => false,
        }
    }

    /// blocks until present `id` is displayed (100 ms timeout: hidden window)
    fn wait_present(&mut self, id: u64) {
        let ok = self.present_done(id, 100_000_000);
        if ok {
            self.feedback.waited(id, timing::now_ns());
        } else {
            self.set_stalled();
        }
    }

    fn destroy_objects(&mut self) {
        let d = &self.device;
        unsafe {
            let _ = d.device_wait_idle();
            for t in [self.picture.take(), self.atlas.take(), self.text.take()].into_iter().flatten() {
                self.destroy_texture(t);
            }
            let frames = std::mem::take(&mut self.frames);
            for f in frames {
                for b in [f.staging, f.vertices, f.readback, f.uploads].into_iter().flatten() {
                    self.destroy_buffer(b);
                }
                d.destroy_fence(f.fence, None);
                d.destroy_semaphore(f.acquired, None);
            }
            let d = &self.device;
            for p in [self.pipelines.image, self.pipelines.overlay, self.pipelines.text] {
                d.destroy_pipeline(p, None);
            }
            d.destroy_pipeline_layout(self.layout, None);
            for s in self.samplers {
                d.destroy_sampler(s, None);
            }
            d.destroy_descriptor_set_layout(self.set_layout, None);
            d.destroy_descriptor_pool(self.desc_pool, None);
            d.destroy_command_pool(self.cmd_pool, None);
        }
        self.destroy_swapchain_views();
        unsafe {
            if self.swapchain != vk::SwapchainKHR::null() {
                self.swapchain_fn.destroy_swapchain(self.swapchain, None);
            }
            self.device.destroy_device(None);
            self.surface_fn.destroy_surface(self.surface, None);
        }
    }
}

impl Drop for Renderer {
    fn drop(&mut self) {
        self.destroy_objects();
        unsafe {
            if let Some((loader, m)) = self.debug.take() {
                loader.destroy_debug_utils_messenger(m, None);
            }
            self.instance.destroy_instance(None);
        }
    }
}

fn color_range() -> vk::ImageSubresourceRange {
    vk::ImageSubresourceRange {
        aspect_mask: vk::ImageAspectFlags::COLOR,
        base_mip_level: 0,
        level_count: 1,
        base_array_layer: 0,
        layer_count: 1,
    }
}

fn color_layers() -> vk::ImageSubresourceLayers {
    vk::ImageSubresourceLayers {
        aspect_mask: vk::ImageAspectFlags::COLOR,
        mip_level: 0,
        base_array_layer: 0,
        layer_count: 1,
    }
}

fn as_bytes<T>(v: &[T]) -> &[u8] {
    unsafe { std::slice::from_raw_parts(v.as_ptr() as *const u8, std::mem::size_of_val(v)) }
}

fn image_barrier(
    d: &Device,
    cmd: vk::CommandBuffer,
    image: vk::Image,
    old: vk::ImageLayout,
    new: vk::ImageLayout,
    src: (vk::PipelineStageFlags2, vk::AccessFlags2),
    dst: (vk::PipelineStageFlags2, vk::AccessFlags2),
) {
    let barrier = [vk::ImageMemoryBarrier2::default()
        .src_stage_mask(src.0)
        .src_access_mask(src.1)
        .dst_stage_mask(dst.0)
        .dst_access_mask(dst.1)
        .old_layout(old)
        .new_layout(new)
        .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .image(image)
        .subresource_range(color_range())];
    unsafe { d.cmd_pipeline_barrier2(cmd, &vk::DependencyInfo::default().image_memory_barriers(&barrier)) };
}

/// buffer (tightly packed, `row_length` pixels per row) -> texture, ends in SHADER_READ_ONLY
fn record_upload(d: &Device, cmd: vk::CommandBuffer, tex: &mut Texture, buffer: vk::Buffer, offset: u64, row_length: u32) {
    let old = if tex.ready {
        vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL
    } else {
        vk::ImageLayout::UNDEFINED
    };
    image_barrier(
        d,
        cmd,
        tex.image,
        old,
        vk::ImageLayout::TRANSFER_DST_OPTIMAL,
        (vk::PipelineStageFlags2::FRAGMENT_SHADER, vk::AccessFlags2::SHADER_SAMPLED_READ),
        (vk::PipelineStageFlags2::COPY, vk::AccessFlags2::TRANSFER_WRITE),
    );
    let region = vk::BufferImageCopy::default()
        .buffer_offset(offset)
        .buffer_row_length(row_length)
        .image_subresource(color_layers())
        .image_extent(vk::Extent3D {
            width: tex.width,
            height: tex.height,
            depth: 1,
        });
    unsafe {
        d.cmd_copy_buffer_to_image(cmd, buffer, tex.image, vk::ImageLayout::TRANSFER_DST_OPTIMAL, &[region]);
    }
    image_barrier(
        d,
        cmd,
        tex.image,
        vk::ImageLayout::TRANSFER_DST_OPTIMAL,
        vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
        (vk::PipelineStageFlags2::COPY, vk::AccessFlags2::TRANSFER_WRITE),
        (vk::PipelineStageFlags2::FRAGMENT_SHADER, vk::AccessFlags2::SHADER_SAMPLED_READ),
    );
    tex.ready = true;
}

/// Target time grid (M-Pacing2): slot k is displayed at `base + k * period`; the emulation thread
/// sleeps until `lead` before its slot, so the emulated frame rate follows the monotonic clock
/// (audio follows via DRC). With VRR the display refreshes at the slot cadence, without it the
/// compositor shows each picture at the first vblank after its slot.
struct Grid {
    period: u64,
    /// submit this long before the slot (render + compositor latency)
    lead: u64,
    next: u64,
}

impl Grid {
    fn new() -> Self {
        let lead = std::env::var("BADLINE_VK_LEAD_US")
            .ok()
            .and_then(|v| v.parse::<u64>().ok())
            .map(|us| us * 1000)
            .unwrap_or(4_000_000);
        Grid { period: 0, lead, next: 0 }
    }

    /// waits for the next slot and returns its target time; a late frame (slot closer than
    /// half the lead, e.g. after a pause) restarts the grid
    fn next_slot(&mut self) -> u64 {
        let now = timing::now_ns();
        if self.next == 0 || self.next < now + self.lead / 2 || self.next > now + self.lead + 2 * self.period {
            self.next = now + self.lead;
        }
        let slot = self.next;
        self.next += self.period;
        timing::sleep_until(slot - self.lead);
        slot
    }
}
