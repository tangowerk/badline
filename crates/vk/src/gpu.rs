//! Small Vulkan helpers shared by the renderer and the slang filter chain.

use ash::{Device, Instance, vk};

pub type Result<T> = std::result::Result<T, String>;

pub fn vkerr(what: &str) -> impl Fn(vk::Result) -> String + '_ {
    move |e| format!("{what}: {e:?}")
}

/// device handles needed to create resources
#[derive(Clone)]
pub struct Gpu {
    pub instance: Instance,
    pub physical: vk::PhysicalDevice,
    pub device: Device,
    pub memory_props: vk::PhysicalDeviceMemoryProperties,
}

/// host visible, coherent, persistently mapped
pub struct Buffer {
    pub buffer: vk::Buffer,
    pub memory: vk::DeviceMemory,
    pub size: u64,
    pub ptr: *mut u8,
}

impl Gpu {
    pub fn memory_type(&self, bits: u32, flags: vk::MemoryPropertyFlags) -> Result<u32> {
        (0..self.memory_props.memory_type_count)
            .find(|&i| bits & (1 << i) != 0 && self.memory_props.memory_types[i as usize].property_flags.contains(flags))
            .ok_or_else(|| format!("no memory type {flags:?}"))
    }

    pub fn create_buffer(&self, size: u64, usage: vk::BufferUsageFlags) -> Result<Buffer> {
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
            let ty = match self.memory_type(
                req.memory_type_bits,
                vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT,
            ) {
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

    pub fn destroy_buffer(&self, b: Buffer) {
        unsafe {
            self.device.destroy_buffer(b.buffer, None);
            self.device.free_memory(b.memory, None);
        }
    }

    pub fn format_features(&self, format: vk::Format) -> vk::FormatFeatureFlags {
        unsafe { self.instance.get_physical_device_format_properties(self.physical, format) }.optimal_tiling_features
    }

    /// device local 2D image with a view over all levels and, for render targets, a view of level 0
    pub fn create_image(
        &self,
        width: u32,
        height: u32,
        format: vk::Format,
        levels: u32,
        usage: vk::ImageUsageFlags,
    ) -> Result<Image> {
        let d = &self.device;
        unsafe {
            let image = d
                .create_image(
                    &vk::ImageCreateInfo::default()
                        .image_type(vk::ImageType::TYPE_2D)
                        .format(format)
                        .extent(vk::Extent3D { width, height, depth: 1 })
                        .mip_levels(levels)
                        .array_layers(1)
                        .samples(vk::SampleCountFlags::TYPE_1)
                        .tiling(vk::ImageTiling::OPTIMAL)
                        .usage(usage)
                        .initial_layout(vk::ImageLayout::UNDEFINED),
                    None,
                )
                .map_err(vkerr("image"))?;
            let mut img = Image {
                image,
                memory: vk::DeviceMemory::null(),
                view: vk::ImageView::null(),
                target: vk::ImageView::null(),
                width,
                height,
                levels,
                format,
                ready: false,
            };
            let res = (|| -> Result<()> {
                let req = d.get_image_memory_requirements(image);
                let ty = self.memory_type(req.memory_type_bits, vk::MemoryPropertyFlags::DEVICE_LOCAL)?;
                img.memory = d
                    .allocate_memory(
                        &vk::MemoryAllocateInfo::default().allocation_size(req.size).memory_type_index(ty),
                        None,
                    )
                    .map_err(vkerr("image memory"))?;
                d.bind_image_memory(image, img.memory, 0).map_err(vkerr("bind image"))?;
                let view = |levels| {
                    d.create_image_view(
                        &vk::ImageViewCreateInfo::default()
                            .image(image)
                            .view_type(vk::ImageViewType::TYPE_2D)
                            .format(format)
                            .subresource_range(range(0, levels)),
                        None,
                    )
                    .map_err(vkerr("image view"))
                };
                img.view = view(levels)?;
                if usage.contains(vk::ImageUsageFlags::COLOR_ATTACHMENT) {
                    img.target = if levels == 1 { img.view } else { view(1)? };
                }
                Ok(())
            })();
            match res {
                Ok(()) => Ok(img),
                Err(e) => {
                    self.destroy_image(img);
                    Err(e)
                }
            }
        }
    }

    pub fn destroy_image(&self, img: Image) {
        let d = &self.device;
        unsafe {
            if img.target != vk::ImageView::null() && img.target != img.view {
                d.destroy_image_view(img.target, None);
            }
            if img.view != vk::ImageView::null() {
                d.destroy_image_view(img.view, None);
            }
            d.destroy_image(img.image, None);
            if img.memory != vk::DeviceMemory::null() {
                d.free_memory(img.memory, None);
            }
        }
    }
}

pub struct Image {
    pub image: vk::Image,
    pub memory: vk::DeviceMemory,
    /// all levels (sampling)
    pub view: vk::ImageView,
    /// level 0 (color attachment), null without COLOR_ATTACHMENT usage
    pub target: vk::ImageView,
    pub width: u32,
    pub height: u32,
    pub levels: u32,
    pub format: vk::Format,
    /// has defined content in SHADER_READ_ONLY_OPTIMAL (false: needs a clear before it is read)
    pub ready: bool,
}

pub fn mip_levels(width: u32, height: u32) -> u32 {
    32 - width.max(height).max(1).leading_zeros()
}

pub fn range(base: u32, count: u32) -> vk::ImageSubresourceRange {
    vk::ImageSubresourceRange {
        aspect_mask: vk::ImageAspectFlags::COLOR,
        base_mip_level: base,
        level_count: count,
        base_array_layer: 0,
        layer_count: 1,
    }
}

pub fn layers(level: u32) -> vk::ImageSubresourceLayers {
    vk::ImageSubresourceLayers {
        aspect_mask: vk::ImageAspectFlags::COLOR,
        mip_level: level,
        base_array_layer: 0,
        layer_count: 1,
    }
}

pub fn barrier(
    d: &Device,
    cmd: vk::CommandBuffer,
    image: vk::Image,
    levels: vk::ImageSubresourceRange,
    old: vk::ImageLayout,
    new: vk::ImageLayout,
    src: (vk::PipelineStageFlags2, vk::AccessFlags2),
    dst: (vk::PipelineStageFlags2, vk::AccessFlags2),
) {
    let b = [vk::ImageMemoryBarrier2::default()
        .src_stage_mask(src.0)
        .src_access_mask(src.1)
        .dst_stage_mask(dst.0)
        .dst_access_mask(dst.1)
        .old_layout(old)
        .new_layout(new)
        .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .image(image)
        .subresource_range(levels)];
    unsafe { d.cmd_pipeline_barrier2(cmd, &vk::DependencyInfo::default().image_memory_barriers(&b)) };
}

/// any earlier use -> color attachment (content discarded)
pub fn to_target(d: &Device, cmd: vk::CommandBuffer, img: &Image) {
    barrier(
        d,
        cmd,
        img.image,
        range(0, img.levels),
        vk::ImageLayout::UNDEFINED,
        vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
        (vk::PipelineStageFlags2::ALL_COMMANDS, vk::AccessFlags2::MEMORY_WRITE),
        (vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT, vk::AccessFlags2::COLOR_ATTACHMENT_WRITE),
    );
}

/// rendered level 0 -> all levels sampled (mipmaps generated by blits when the image has them)
pub fn to_sampled(d: &Device, cmd: vk::CommandBuffer, img: &mut Image) {
    let attachment = (vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT, vk::AccessFlags2::COLOR_ATTACHMENT_WRITE);
    let read = (vk::PipelineStageFlags2::ALL_GRAPHICS, vk::AccessFlags2::SHADER_SAMPLED_READ);
    let src = (vk::PipelineStageFlags2::BLIT, vk::AccessFlags2::TRANSFER_READ);
    img.ready = true;
    if img.levels == 1 {
        barrier(
            d,
            cmd,
            img.image,
            range(0, 1),
            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
            attachment,
            read,
        );
        return;
    }
    barrier(
        d,
        cmd,
        img.image,
        range(0, 1),
        vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
        vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
        attachment,
        src,
    );
    blit_levels(d, cmd, img, 0);
}

/// level `base` in TRANSFER_SRC -> levels below generated, everything SHADER_READ_ONLY
pub fn blit_levels(d: &Device, cmd: vk::CommandBuffer, img: &Image, base: u32) {
    let read = (vk::PipelineStageFlags2::ALL_GRAPHICS, vk::AccessFlags2::SHADER_SAMPLED_READ);
    let src = (vk::PipelineStageFlags2::BLIT, vk::AccessFlags2::TRANSFER_READ);
    let dst = (vk::PipelineStageFlags2::BLIT, vk::AccessFlags2::TRANSFER_WRITE);
    let (mut w, mut h) = (img.width as i32, img.height as i32);
    for level in base + 1..img.levels {
        barrier(
            d,
            cmd,
            img.image,
            range(level, 1),
            vk::ImageLayout::UNDEFINED,
            vk::ImageLayout::TRANSFER_DST_OPTIMAL,
            (vk::PipelineStageFlags2::ALL_COMMANDS, vk::AccessFlags2::NONE),
            dst,
        );
        let (nw, nh) = ((w / 2).max(1), (h / 2).max(1));
        let region = vk::ImageBlit::default()
            .src_subresource(layers(level - 1))
            .src_offsets([vk::Offset3D::default(), vk::Offset3D { x: w, y: h, z: 1 }])
            .dst_subresource(layers(level))
            .dst_offsets([vk::Offset3D::default(), vk::Offset3D { x: nw, y: nh, z: 1 }]);
        unsafe {
            d.cmd_blit_image(
                cmd,
                img.image,
                vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                img.image,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                &[region],
                vk::Filter::LINEAR,
            );
        }
        barrier(
            d,
            cmd,
            img.image,
            range(level, 1),
            vk::ImageLayout::TRANSFER_DST_OPTIMAL,
            vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
            dst,
            src,
        );
        (w, h) = (nw, nh);
    }
    barrier(
        d,
        cmd,
        img.image,
        range(0, img.levels),
        vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
        vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
        src,
        read,
    );
}

/// undefined image -> cleared to transparent black, sampled
pub fn clear(d: &Device, cmd: vk::CommandBuffer, img: &mut Image) {
    let all = range(0, img.levels);
    barrier(
        d,
        cmd,
        img.image,
        all,
        vk::ImageLayout::UNDEFINED,
        vk::ImageLayout::TRANSFER_DST_OPTIMAL,
        (vk::PipelineStageFlags2::ALL_COMMANDS, vk::AccessFlags2::MEMORY_WRITE),
        (vk::PipelineStageFlags2::CLEAR, vk::AccessFlags2::TRANSFER_WRITE),
    );
    unsafe {
        d.cmd_clear_color_image(
            cmd,
            img.image,
            vk::ImageLayout::TRANSFER_DST_OPTIMAL,
            &vk::ClearColorValue { float32: [0.0; 4] },
            &[all],
        );
    }
    barrier(
        d,
        cmd,
        img.image,
        all,
        vk::ImageLayout::TRANSFER_DST_OPTIMAL,
        vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
        (vk::PipelineStageFlags2::CLEAR, vk::AccessFlags2::TRANSFER_WRITE),
        (vk::PipelineStageFlags2::ALL_GRAPHICS, vk::AccessFlags2::SHADER_SAMPLED_READ),
    );
    img.ready = true;
}

pub fn as_bytes<T>(v: &[T]) -> &[u8] {
    unsafe { std::slice::from_raw_parts(v.as_ptr() as *const u8, std::mem::size_of_val(v)) }
}
