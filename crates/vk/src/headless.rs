//! Window-less Vulkan context for the slang corpus test (M-VK3c): builds a preset's chain,
//! renders a few frames from a test picture and reads the output back. Picks the first Vulkan
//! 1.3 device with a graphics queue (select a driver with `VK_DRIVER_FILES`, e.g. lavapipe).

use std::ffi::{CStr, c_void};
use std::sync::Mutex;

use ash::ext::debug_utils;
use ash::{Device, Entry, Instance, vk};

use crate::chain::{Chain, MVP_OFFSCREEN, RunInfo};
use crate::gpu::{Gpu, Result, vkerr};
use crate::slang::Compiled;

const VALIDATION_LAYER: &CStr = c"VK_LAYER_KHRONOS_validation";

/// validation messages since the last `take_messages` (one process-wide context)
static MESSAGES: Mutex<Vec<String>> = Mutex::new(Vec::new());

unsafe extern "system" fn collect(
    severity: vk::DebugUtilsMessageSeverityFlagsEXT,
    _types: vk::DebugUtilsMessageTypeFlagsEXT,
    data: *const vk::DebugUtilsMessengerCallbackDataEXT<'_>,
    _user: *mut c_void,
) -> vk::Bool32 {
    if severity.contains(vk::DebugUtilsMessageSeverityFlagsEXT::ERROR) {
        let msg = unsafe { (*data).message_as_c_str() }
            .map(|m| m.to_string_lossy().into_owned())
            .unwrap_or_default();
        MESSAGES.lock().unwrap().push(msg);
    }
    vk::FALSE
}

/// result of one preset run
pub struct Rendered {
    pub passes: usize,
    /// output pixels of the last frame, B8G8R8A8
    pub width: u32,
    pub height: u32,
    pub pixels: Vec<u8>,
    /// validation errors during build + render (empty without the layer)
    pub validation: Vec<String>,
}

impl Rendered {
    /// distinct values over all colour channels of the output (1 = a flat picture)
    pub fn distinct(&self) -> usize {
        let mut seen = [false; 256];
        for px in self.pixels.chunks_exact(4) {
            for &c in &px[..3] {
                seen[c as usize] = true;
            }
        }
        seen.iter().filter(|&&s| s).count()
    }
}

pub struct Headless {
    _entry: Entry,
    instance: Instance,
    debug: Option<(debug_utils::Instance, vk::DebugUtilsMessengerEXT)>,
    gpu: Gpu,
    device: Device,
    queue: vk::Queue,
    pool: vk::CommandPool,
    cache: vk::PipelineCache,
    pub device_name: String,
}

const FORMAT: vk::Format = vk::Format::B8G8R8A8_UNORM;

impl Headless {
    pub fn new(validation: bool) -> Result<Headless> {
        let entry = unsafe { Entry::load() }.map_err(|e| format!("Vulkan loader: {e}"))?;
        let mut layers = Vec::new();
        let mut extensions = Vec::new();
        if validation {
            let available = unsafe { entry.enumerate_instance_layer_properties() }.unwrap_or_default();
            if !available.iter().any(|l| l.layer_name_as_c_str() == Ok(VALIDATION_LAYER)) {
                return Err("validation layer not installed (vulkan-validation-layers)".into());
            }
            layers.push(VALIDATION_LAYER.as_ptr());
            extensions.push(debug_utils::NAME.as_ptr());
        }
        let app = vk::ApplicationInfo::default()
            .application_name(c"badline-corpus")
            .api_version(vk::API_VERSION_1_3);
        // report every message (the layer stops after 10 of a kind by default)
        let off = vk::FALSE.to_ne_bytes();
        let settings = [vk::LayerSettingEXT::default()
            .layer_name(VALIDATION_LAYER)
            .setting_name(c"enable_message_limit")
            .ty(vk::LayerSettingTypeEXT::BOOL32)
            .values(&off)];
        let mut layer_settings = vk::LayerSettingsCreateInfoEXT::default().settings(&settings);
        let mut create = vk::InstanceCreateInfo::default()
            .application_info(&app)
            .enabled_layer_names(&layers)
            .enabled_extension_names(&extensions);
        if validation {
            create = create.push_next(&mut layer_settings);
        }
        let instance = unsafe { entry.create_instance(&create, None) }.map_err(vkerr("vkCreateInstance"))?;
        let debug = if validation {
            let du = debug_utils::Instance::new(&entry, &instance);
            let ci = vk::DebugUtilsMessengerCreateInfoEXT::default()
                .message_severity(vk::DebugUtilsMessageSeverityFlagsEXT::ERROR)
                .message_type(vk::DebugUtilsMessageTypeFlagsEXT::GENERAL | vk::DebugUtilsMessageTypeFlagsEXT::VALIDATION)
                .pfn_user_callback(Some(collect));
            let m = unsafe { du.create_debug_utils_messenger(&ci, None) }.map_err(vkerr("debug messenger"))?;
            Some((du, m))
        } else {
            None
        };

        let mut pick = None;
        for physical in unsafe { instance.enumerate_physical_devices() }.map_err(vkerr("devices"))? {
            let props = unsafe { instance.get_physical_device_properties(physical) };
            if props.api_version < vk::API_VERSION_1_3 {
                continue;
            }
            let families = unsafe { instance.get_physical_device_queue_family_properties(physical) };
            if let Some(f) = families.iter().position(|f| f.queue_flags.contains(vk::QueueFlags::GRAPHICS)) {
                let name = props.device_name_as_c_str().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
                pick = Some((physical, f as u32, name));
                break;
            }
        }
        let Some((physical, family, device_name)) = pick else {
            return Err("no Vulkan 1.3 device with a graphics queue".into());
        };
        let priorities = [1.0f32];
        let queues = [vk::DeviceQueueCreateInfo::default()
            .queue_family_index(family)
            .queue_priorities(&priorities)];
        let mut f13 = vk::PhysicalDeviceVulkan13Features::default()
            .dynamic_rendering(true)
            .synchronization2(true);
        let ci = vk::DeviceCreateInfo::default().queue_create_infos(&queues).push_next(&mut f13);
        let device = unsafe { instance.create_device(physical, &ci, None) }.map_err(vkerr("vkCreateDevice"))?;
        let queue = unsafe { device.get_device_queue(family, 0) };
        let pool = unsafe {
            device.create_command_pool(
                &vk::CommandPoolCreateInfo::default()
                    .queue_family_index(family)
                    .flags(vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER),
                None,
            )
        }
        .map_err(vkerr("command pool"))?;
        let cache = unsafe { device.create_pipeline_cache(&vk::PipelineCacheCreateInfo::default(), None) }
            .map_err(vkerr("pipeline cache"))?;
        let memory_props = unsafe { instance.get_physical_device_memory_properties(physical) };
        let gpu = Gpu {
            instance: instance.clone(),
            physical,
            device: device.clone(),
            memory_props,
        };
        Ok(Headless {
            _entry: entry,
            instance,
            debug,
            gpu,
            device,
            queue,
            pool,
            cache,
            device_name,
        })
    }

    /// Builds the chain and renders `frames` frames of a test picture (`original` pixels, colour
    /// bars + a grid) into a `viewport` sized output; returns the last output.
    pub fn render(&self, compiled: Compiled, original: (u32, u32), viewport: (u32, u32), frames: u32) -> Result<Rendered> {
        MESSAGES.lock().unwrap().clear();
        let result = self.run(compiled, original, viewport, frames);
        let validation = std::mem::take(&mut *MESSAGES.lock().unwrap());
        result.map(|mut r| {
            r.validation = validation;
            r
        })
    }

    fn run(&self, compiled: Compiled, original: (u32, u32), viewport: (u32, u32), frames: u32) -> Result<Rendered> {
        let d = &self.device;
        let mut chain = Chain::new(&self.gpu, compiled, self.cache, FORMAT, self.queue, self.pool)?;
        let passes = chain.pass_count();
        let size = viewport.0 as u64 * viewport.1 as u64 * 4;
        let readback = self.gpu.create_buffer(size, vk::BufferUsageFlags::TRANSFER_DST)?;
        let cmd = unsafe {
            d.allocate_command_buffers(
                &vk::CommandBufferAllocateInfo::default()
                    .command_pool(self.pool)
                    .level(vk::CommandBufferLevel::PRIMARY)
                    .command_buffer_count(1),
            )
        }
        .map_err(vkerr("command buffer"))?[0];
        let result = (|| -> Result<()> {
            chain.prepare(original, viewport)?;
            for frame in 0..frames {
                let last = frame + 1 == frames;
                unsafe {
                    d.begin_command_buffer(cmd, &vk::CommandBufferBeginInfo::default())
                        .map_err(vkerr("begin"))?;
                    let (view, w, h) = chain.begin_original(cmd, true);
                    test_picture(d, cmd, view, w, h, frame);
                    let info = RunInfo {
                        mvp: MVP_OFFSCREEN,
                        linear: false,
                        advance: true,
                        rotation: 0,
                        fps: 50.0,
                    };
                    chain.record(cmd, frame as usize % crate::chain::FRAMES, &info)?;
                    if last {
                        chain.copy_output(cmd, readback.buffer);
                    }
                    d.end_command_buffer(cmd).map_err(vkerr("end"))?;
                    let cmds = [cmd];
                    d.queue_submit(self.queue, &[vk::SubmitInfo::default().command_buffers(&cmds)], vk::Fence::null())
                        .map_err(vkerr("submit"))?;
                    d.queue_wait_idle(self.queue).map_err(vkerr("wait"))?;
                }
            }
            Ok(())
        })();
        let pixels = unsafe { std::slice::from_raw_parts(readback.ptr, size as usize) }.to_vec();
        unsafe { d.free_command_buffers(self.pool, &[cmd]) };
        self.gpu.destroy_buffer(readback);
        drop(chain);
        result?;
        Ok(Rendered {
            passes,
            width: viewport.0,
            height: viewport.1,
            pixels,
            validation: Vec::new(),
        })
    }
}

/// C64-like test picture: 8 colour bars, a white grid every 16 pixels, moving with the frame
/// (history/feedback presets see changing input). The image is a colour attachment and becomes
/// shader readable in `Chain::record`.
unsafe fn test_picture(d: &Device, cmd: vk::CommandBuffer, view: vk::ImageView, w: u32, h: u32, frame: u32) {
    let attachments = [vk::RenderingAttachmentInfo::default()
        .image_view(view)
        .image_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
        .load_op(vk::AttachmentLoadOp::CLEAR)
        .clear_value(vk::ClearValue {
            color: vk::ClearColorValue { float32: [0.21, 0.16, 0.47, 1.0] },
        })
        .store_op(vk::AttachmentStoreOp::STORE)];
    let area = vk::Rect2D {
        offset: vk::Offset2D::default(),
        extent: vk::Extent2D { width: w, height: h },
    };
    const BARS: [[f32; 3]; 8] = [
        [1.0, 1.0, 1.0],
        [0.53, 0.0, 0.0],
        [0.67, 1.0, 0.93],
        [0.8, 0.27, 0.8],
        [0.0, 0.8, 0.33],
        [0.0, 0.0, 0.67],
        [0.93, 0.93, 0.47],
        [0.0, 0.0, 0.0],
    ];
    let mut clears = Vec::new();
    let mut rects = Vec::new();
    let bar_w = (w / 8).max(1);
    for (i, c) in BARS.iter().enumerate() {
        clears.push(vk::ClearAttachment {
            aspect_mask: vk::ImageAspectFlags::COLOR,
            color_attachment: 0,
            clear_value: vk::ClearValue {
                color: vk::ClearColorValue { float32: [c[0], c[1], c[2], 1.0] },
            },
        });
        rects.push(vk::ClearRect {
            rect: vk::Rect2D {
                offset: vk::Offset2D { x: (i as u32 * bar_w).min(w - 1) as i32, y: (h / 4) as i32 },
                extent: vk::Extent2D { width: bar_w.min(w - (i as u32 * bar_w).min(w - 1)), height: (h / 2).max(1) },
            },
            base_array_layer: 0,
            layer_count: 1,
        });
    }
    let white = vk::ClearAttachment {
        aspect_mask: vk::ImageAspectFlags::COLOR,
        color_attachment: 0,
        clear_value: vk::ClearValue {
            color: vk::ClearColorValue { float32: [1.0, 1.0, 1.0, 1.0] },
        },
    };
    let shift = frame * 2;
    let mut lines = Vec::new();
    for x in ((shift % 16)..w).step_by(16) {
        lines.push(vk::ClearRect {
            rect: vk::Rect2D {
                offset: vk::Offset2D { x: x as i32, y: 0 },
                extent: vk::Extent2D { width: 1, height: h },
            },
            base_array_layer: 0,
            layer_count: 1,
        });
    }
    for y in (0..h).step_by(16) {
        lines.push(vk::ClearRect {
            rect: vk::Rect2D {
                offset: vk::Offset2D { x: 0, y: y as i32 },
                extent: vk::Extent2D { width: w, height: 1 },
            },
            base_array_layer: 0,
            layer_count: 1,
        });
    }
    unsafe {
        d.cmd_begin_rendering(cmd, &vk::RenderingInfo::default().render_area(area).layer_count(1).color_attachments(&attachments));
        for (c, r) in clears.iter().zip(&rects) {
            d.cmd_clear_attachments(cmd, std::slice::from_ref(c), std::slice::from_ref(r));
        }
        d.cmd_clear_attachments(cmd, &[white], &lines);
        d.cmd_end_rendering(cmd);
    }
}

impl Drop for Headless {
    fn drop(&mut self) {
        unsafe {
            let _ = self.device.device_wait_idle();
            self.device.destroy_pipeline_cache(self.cache, None);
            self.device.destroy_command_pool(self.pool, None);
            self.device.destroy_device(None);
            if let Some((du, m)) = self.debug.take() {
                du.destroy_debug_utils_messenger(m, None);
            }
            self.instance.destroy_instance(None);
        }
    }
}
