//! Vulkan filter chain for slang presets (M-VK3a), modelled on RetroArch's Vulkan driver.
//!
//! The renderer draws the emulator picture (plain or frame-blended) 1:1 into `original`, then
//! `record` runs the passes: pass i renders into its output (intermediate size from the preset
//! scale), the last pass into `output` at viewport size with the final MVP (rotation); the
//! renderer copies `output` 1:1 into the swapchain viewport. History (OriginalHistoryN) and
//! feedback (PassFeedbackN) advance once per new input; redraws and repeated presents reuse the
//! output, a changed viewport or parameter re-runs the passes without advancing.

use std::collections::{HashMap, VecDeque};

use ash::vk;

use crate::gpu::{self, Buffer, Gpu, Image, Result, vkerr};
use crate::slang::preset::{ScaleType, Wrap};
use crate::slang::reflect::Base;
use crate::slang::{Compiled, CompiledPass, Param, Slot, Tex, Uni};

/// frames in flight (renderer::FRAMES)
pub const FRAMES: usize = 2;

/// Position (vec2, z = 0, w = 1) + TexCoord, triangle strip, top left first (RetroArch)
const QUAD: [f32; 16] = [0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0];

/// MVP of the intermediate passes: [0, 1] -> [-1, 1] (column major)
pub(crate) const MVP_OFFSCREEN: [f32; 16] = [2.0, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, -1.0, -1.0, 0.0, 1.0];

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct SamplerKey {
    linear: bool,
    mip_linear: bool,
    wrap: Wrap,
}

struct Pass {
    data: CompiledPass,
    /// the last pass renders to the chain output
    last: bool,
    set_layout: vk::DescriptorSetLayout,
    layout: vk::PipelineLayout,
    pipeline: vk::Pipeline,
    sets: [vk::DescriptorSet; FRAMES],
    ubos: Vec<Buffer>,
    output: Option<Image>,
    /// previous output (PassFeedback), swapped with `output` on every advance
    feedback: Option<Image>,
    /// size of the output for the current input/viewport
    size: (u32, u32),
    /// the output needs mipmaps (the next pass samples its input with them)
    mipmaps: bool,
    /// some pass reads the previous output (PassFeedbackN)
    has_feedback: bool,
}

struct LutImage {
    image: Image,
    key: SamplerKey,
}

/// per run inputs from the renderer
pub struct RunInfo {
    /// MVP of the last pass (Vulkan clip space, rotation included)
    pub mvp: [f32; 16],
    /// global filter for passes without filter_linearN
    pub linear: bool,
    /// new input: advance history, feedback and FrameCount
    pub advance: bool,
    /// 0..3 (90 degree steps)
    pub rotation: u32,
    pub fps: f32,
}

pub struct Chain {
    gpu: Gpu,
    passes: Vec<Pass>,
    luts: Vec<LutImage>,
    pub params: Vec<Param>,
    history_depth: usize,
    /// picture of the current run (drawn by the renderer)
    original: Option<Image>,
    /// [0] = OriginalHistory1
    history: VecDeque<Image>,
    output: Option<Image>,
    out_format: vk::Format,
    original_format: vk::Format,
    original_mipmaps: bool,
    vertices: Buffer,
    samplers: HashMap<SamplerKey, vk::Sampler>,
    desc_pool: vk::DescriptorPool,
    frame_count: u32,
    last_advance_ns: u64,
    frame_time_us: u32,
    /// sizes the passes were last run for: (original, viewport)
    ran_for: Option<((u32, u32), (u32, u32))>,
    /// a parameter changed since the last run
    dirty: bool,
    pub path: std::path::PathBuf,
}

fn wrap_mode(w: Wrap) -> vk::SamplerAddressMode {
    match w {
        Wrap::Border => vk::SamplerAddressMode::CLAMP_TO_BORDER,
        Wrap::Edge => vk::SamplerAddressMode::CLAMP_TO_EDGE,
        Wrap::Repeat => vk::SamplerAddressMode::REPEAT,
        Wrap::Mirror => vk::SamplerAddressMode::MIRRORED_REPEAT,
    }
}

fn axis(t: ScaleType, scale: f32, source: u32, viewport: u32) -> u32 {
    let v = match t {
        ScaleType::Source => source as f32 * scale,
        ScaleType::Viewport => viewport as f32 * scale,
        ScaleType::Absolute => scale,
    };
    (v.round() as u32).clamp(1, 16384)
}

impl Chain {
    /// Builds pipelines, descriptor layouts and LUT textures. `out_format` is the format of the
    /// output (and of the original, drawn with the renderer's pipelines).
    pub fn new(
        gpu: &Gpu,
        compiled: Compiled,
        cache: vk::PipelineCache,
        out_format: vk::Format,
        queue: vk::Queue,
        cmd_pool: vk::CommandPool,
    ) -> Result<Chain> {
        let mut chain = Chain {
            gpu: gpu.clone(),
            passes: Vec::new(),
            luts: Vec::new(),
            params: compiled.params,
            history_depth: compiled.history as usize,
            original: None,
            history: VecDeque::new(),
            output: None,
            out_format,
            original_format: out_format,
            original_mipmaps: compiled.passes.first().is_some_and(|p| p.config.mipmap_input),
            vertices: gpu.create_buffer(std::mem::size_of_val(&QUAD) as u64, vk::BufferUsageFlags::VERTEX_BUFFER)?,
            samplers: HashMap::new(),
            desc_pool: vk::DescriptorPool::null(),
            frame_count: 0,
            last_advance_ns: 0,
            frame_time_us: 0,
            ran_for: None,
            dirty: true,
            path: compiled.path,
        };
        unsafe {
            std::ptr::copy_nonoverlapping(QUAD.as_ptr() as *const u8, chain.vertices.ptr, std::mem::size_of_val(&QUAD));
        }
        // on error `Drop` releases whatever was created
        chain.build(compiled.passes, &compiled.feedback, cache)?;
        chain.upload_luts(compiled.luts, queue, cmd_pool)?;
        Ok(chain)
    }

    fn build(&mut self, passes: Vec<CompiledPass>, feedback: &[bool], cache: vk::PipelineCache) -> Result<()> {
        let d = self.gpu.device.clone();
        let count = passes.len();
        let textures: usize = passes.iter().map(|p| p.textures.len()).sum();
        let sizes = [
            vk::DescriptorPoolSize::default()
                .ty(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
                .descriptor_count((textures * FRAMES).max(1) as u32),
            vk::DescriptorPoolSize::default()
                .ty(vk::DescriptorType::UNIFORM_BUFFER)
                .descriptor_count((count * FRAMES) as u32),
        ];
        self.desc_pool = unsafe {
            d.create_descriptor_pool(
                &vk::DescriptorPoolCreateInfo::default().max_sets((count * FRAMES) as u32).pool_sizes(&sizes),
                None,
            )
        }
        .map_err(vkerr("descriptor pool"))?;

        let mipmaps: Vec<bool> = (0..count).map(|i| passes.get(i + 1).is_some_and(|n| n.config.mipmap_input)).collect();
        for (i, data) in passes.into_iter().enumerate() {
            let last = i + 1 == count;
            let format = if last { self.out_format } else { data.format };
            let feats = self.gpu.format_features(format);
            let needed = vk::FormatFeatureFlags::COLOR_ATTACHMENT | vk::FormatFeatureFlags::SAMPLED_IMAGE;
            if !feats.contains(needed) {
                return Err(format!("{}: render target format {format:?} not supported", data.config.shader.display()));
            }
            let mut pass = Pass {
                data,
                last,
                set_layout: vk::DescriptorSetLayout::null(),
                layout: vk::PipelineLayout::null(),
                pipeline: vk::Pipeline::null(),
                sets: [vk::DescriptorSet::null(); FRAMES],
                ubos: Vec::new(),
                output: None,
                feedback: None,
                size: (0, 0),
                mipmaps: mipmaps[i] && !last,
                has_feedback: feedback[i],
            };
            pass.data.format = format;
            let res = self.build_pass(&d, &mut pass, cache);
            self.passes.push(pass);
            res?;
        }
        Ok(())
    }

    fn build_pass(&self, d: &ash::Device, pass: &mut Pass, cache: vk::PipelineCache) -> Result<()> {
        let stages_all = vk::ShaderStageFlags::VERTEX | vk::ShaderStageFlags::FRAGMENT;
        let mut bindings = Vec::new();
        if let Some(b) = pass.data.ubo_binding {
            bindings.push(
                vk::DescriptorSetLayoutBinding::default()
                    .binding(b)
                    .descriptor_type(vk::DescriptorType::UNIFORM_BUFFER)
                    .descriptor_count(1)
                    .stage_flags(stages_all),
            );
        }
        for t in &pass.data.textures {
            bindings.push(
                vk::DescriptorSetLayoutBinding::default()
                    .binding(t.binding)
                    .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
                    .descriptor_count(1)
                    .stage_flags(stages_all),
            );
        }
        unsafe {
            pass.set_layout = d
                .create_descriptor_set_layout(&vk::DescriptorSetLayoutCreateInfo::default().bindings(&bindings), None)
                .map_err(vkerr("descriptor set layout"))?;
            let push = [vk::PushConstantRange::default()
                .stage_flags(stages_all)
                .size(pass.data.push_size.div_ceil(4) * 4)];
            let set_layouts = [pass.set_layout];
            let mut ci = vk::PipelineLayoutCreateInfo::default().set_layouts(&set_layouts);
            if pass.data.push_size > 0 {
                ci = ci.push_constant_ranges(&push);
            }
            pass.layout = d.create_pipeline_layout(&ci, None).map_err(vkerr("pipeline layout"))?;
            let layouts = [pass.set_layout; FRAMES];
            let sets = d
                .allocate_descriptor_sets(
                    &vk::DescriptorSetAllocateInfo::default().descriptor_pool(self.desc_pool).set_layouts(&layouts),
                )
                .map_err(vkerr("descriptor sets"))?;
            pass.sets.copy_from_slice(&sets);
        }
        if pass.data.ubo_binding.is_some() {
            for _ in 0..FRAMES {
                let size = (pass.data.ubo_size.max(16) as u64).div_ceil(16) * 16;
                pass.ubos.push(self.gpu.create_buffer(size, vk::BufferUsageFlags::UNIFORM_BUFFER)?);
            }
        }

        let module = |code: &[u32]| unsafe {
            d.create_shader_module(&vk::ShaderModuleCreateInfo::default().code(code), None)
                .map_err(vkerr("shader module"))
        };
        let vs = module(&pass.data.vertex)?;
        let fs = match module(&pass.data.fragment) {
            Ok(f) => f,
            Err(e) => {
                unsafe { d.destroy_shader_module(vs, None) };
                return Err(e);
            }
        };
        let stages = [
            vk::PipelineShaderStageCreateInfo::default().stage(vk::ShaderStageFlags::VERTEX).module(vs).name(c"main"),
            vk::PipelineShaderStageCreateInfo::default().stage(vk::ShaderStageFlags::FRAGMENT).module(fs).name(c"main"),
        ];
        let vbind = [vk::VertexInputBindingDescription::default().binding(0).stride(16).input_rate(vk::VertexInputRate::VERTEX)];
        let vattr = [
            vk::VertexInputAttributeDescription::default().location(0).format(vk::Format::R32G32_SFLOAT).offset(0),
            vk::VertexInputAttributeDescription::default().location(1).format(vk::Format::R32G32_SFLOAT).offset(8),
        ];
        let vi = vk::PipelineVertexInputStateCreateInfo::default()
            .vertex_binding_descriptions(&vbind)
            .vertex_attribute_descriptions(&vattr);
        let ia = vk::PipelineInputAssemblyStateCreateInfo::default().topology(vk::PrimitiveTopology::TRIANGLE_STRIP);
        let vp = vk::PipelineViewportStateCreateInfo::default().viewport_count(1).scissor_count(1);
        let rs = vk::PipelineRasterizationStateCreateInfo::default()
            .polygon_mode(vk::PolygonMode::FILL)
            .cull_mode(vk::CullModeFlags::NONE)
            .line_width(1.0);
        let ms = vk::PipelineMultisampleStateCreateInfo::default().rasterization_samples(vk::SampleCountFlags::TYPE_1);
        let attachments = [vk::PipelineColorBlendAttachmentState::default().color_write_mask(vk::ColorComponentFlags::RGBA)];
        let cb = vk::PipelineColorBlendStateCreateInfo::default().attachments(&attachments);
        let dynamic = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
        let ds = vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dynamic);
        let formats = [pass.data.format];
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
            .layout(pass.layout)
            .push_next(&mut rendering);
        let res = unsafe { d.create_graphics_pipelines(cache, &[ci], None) };
        unsafe {
            d.destroy_shader_module(vs, None);
            d.destroy_shader_module(fs, None);
        }
        pass.pipeline = res
            .map(|p| p[0])
            .map_err(|(_, e)| format!("{}: graphics pipeline: {e:?}", pass.data.config.shader.display()))?;
        Ok(())
    }

    fn mip_capable(&self, format: vk::Format) -> bool {
        let f = self.gpu.format_features(format);
        f.contains(
            vk::FormatFeatureFlags::BLIT_SRC
                | vk::FormatFeatureFlags::BLIT_DST
                | vk::FormatFeatureFlags::SAMPLED_IMAGE_FILTER_LINEAR,
        )
    }

    fn upload_luts(&mut self, luts: Vec<crate::slang::Lut>, queue: vk::Queue, pool: vk::CommandPool) -> Result<()> {
        if luts.is_empty() {
            return Ok(());
        }
        let d = self.gpu.device.clone();
        let total: usize = luts.iter().map(|l| l.rgba.len()).sum();
        let staging = self.gpu.create_buffer(total as u64, vk::BufferUsageFlags::TRANSFER_SRC)?;
        let res = (|| -> Result<()> {
            let cmd = unsafe {
                d.allocate_command_buffers(
                    &vk::CommandBufferAllocateInfo::default()
                        .command_pool(pool)
                        .level(vk::CommandBufferLevel::PRIMARY)
                        .command_buffer_count(1),
                )
            }
            .map_err(vkerr("command buffer"))?[0];
            let res = (|| -> Result<()> {
                unsafe {
                    d.begin_command_buffer(
                        cmd,
                        &vk::CommandBufferBeginInfo::default().flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT),
                    )
                    .map_err(vkerr("begin command buffer"))?;
                }
                let mut offset = 0usize;
                for l in luts {
                    let format = vk::Format::R8G8B8A8_UNORM;
                    let levels = if l.mipmap && self.mip_capable(format) { gpu::mip_levels(l.width, l.height) } else { 1 };
                    let usage = vk::ImageUsageFlags::SAMPLED
                        | vk::ImageUsageFlags::TRANSFER_DST
                        | vk::ImageUsageFlags::TRANSFER_SRC;
                    let mut image = self.gpu.create_image(l.width, l.height, format, levels, usage)?;
                    unsafe {
                        std::ptr::copy_nonoverlapping(l.rgba.as_ptr(), staging.ptr.add(offset), l.rgba.len());
                    }
                    gpu::barrier(
                        &d,
                        cmd,
                        image.image,
                        gpu::range(0, levels),
                        vk::ImageLayout::UNDEFINED,
                        vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                        (vk::PipelineStageFlags2::NONE, vk::AccessFlags2::NONE),
                        (vk::PipelineStageFlags2::COPY, vk::AccessFlags2::TRANSFER_WRITE),
                    );
                    let region = vk::BufferImageCopy::default()
                        .buffer_offset(offset as u64)
                        .image_subresource(gpu::layers(0))
                        .image_extent(vk::Extent3D { width: l.width, height: l.height, depth: 1 });
                    unsafe {
                        d.cmd_copy_buffer_to_image(cmd, staging.buffer, image.image, vk::ImageLayout::TRANSFER_DST_OPTIMAL, &[region]);
                    }
                    gpu::barrier(
                        &d,
                        cmd,
                        image.image,
                        gpu::range(0, 1),
                        vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                        vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                        (vk::PipelineStageFlags2::COPY, vk::AccessFlags2::TRANSFER_WRITE),
                        (vk::PipelineStageFlags2::BLIT, vk::AccessFlags2::TRANSFER_READ),
                    );
                    gpu::blit_levels(&d, cmd, &image, 0);
                    image.ready = true;
                    offset += l.rgba.len();
                    self.luts.push(LutImage {
                        image,
                        key: SamplerKey {
                            linear: l.linear,
                            mip_linear: l.mipmap && l.linear,
                            wrap: l.wrap,
                        },
                    });
                }
                unsafe {
                    d.end_command_buffer(cmd).map_err(vkerr("end command buffer"))?;
                    let cmds = [cmd];
                    d.queue_submit(queue, &[vk::SubmitInfo::default().command_buffers(&cmds)], vk::Fence::null())
                        .map_err(vkerr("submit"))?;
                    d.queue_wait_idle(queue).map_err(vkerr("wait idle"))?;
                }
                Ok(())
            })();
            unsafe { d.free_command_buffers(pool, &[cmd]) };
            res
        })();
        self.gpu.destroy_buffer(staging);
        res
    }

    fn sampler(&mut self, key: SamplerKey) -> Result<vk::Sampler> {
        if let Some(&s) = self.samplers.get(&key) {
            return Ok(s);
        }
        let filter = if key.linear { vk::Filter::LINEAR } else { vk::Filter::NEAREST };
        let mode = wrap_mode(key.wrap);
        let ci = vk::SamplerCreateInfo::default()
            .mag_filter(filter)
            .min_filter(filter)
            .mipmap_mode(if key.mip_linear { vk::SamplerMipmapMode::LINEAR } else { vk::SamplerMipmapMode::NEAREST })
            .address_mode_u(mode)
            .address_mode_v(mode)
            .address_mode_w(mode)
            .border_color(vk::BorderColor::FLOAT_TRANSPARENT_BLACK)
            .max_lod(vk::LOD_CLAMP_NONE);
        let s = unsafe { self.gpu.device.create_sampler(&ci, None) }.map_err(vkerr("sampler"))?;
        self.samplers.insert(key, s);
        Ok(s)
    }

    /// sampler settings for the input of pass `i` (RetroArch: the output of pass i - 1 and the
    /// feedback of it are sampled with the settings of pass i)
    fn input_key(&self, i: usize, linear: bool) -> SamplerKey {
        let p = &self.passes[i.min(self.passes.len() - 1)].data.config;
        let lin = p.filter_linear.unwrap_or(linear);
        SamplerKey {
            linear: lin,
            mip_linear: p.mipmap_input && lin,
            wrap: p.wrap,
        }
    }

    pub fn set_param(&mut self, id: &str, value: f32) -> bool {
        match self.params.iter_mut().find(|p| p.id == id) {
            Some(p) => {
                p.value = value;
                self.dirty = true;
                true
            }
            None => false,
        }
    }

    /// the passes must run for this input/viewport even without a new picture
    pub fn needs_run(&self, original: (u32, u32), viewport: (u32, u32)) -> bool {
        self.dirty || self.output.is_none() || self.ran_for != Some((original, viewport))
    }

    /// (Re)creates the images for an input of `original` and a viewport of `viewport` pixels.
    /// Called before recording; images that changed size are replaced after a device wait.
    pub fn prepare(&mut self, original: (u32, u32), viewport: (u32, u32)) -> Result<()> {
        let mut source = original;
        let mut sizes = Vec::with_capacity(self.passes.len());
        for p in &self.passes {
            let size = match (&p.data.config.scale, p.last) {
                (_, true) => viewport,
                (None, false) => source,
                (Some(s), false) => (axis(s.x.0, s.x.1, source.0, viewport.0), axis(s.y.0, s.y.1, source.1, viewport.1)),
            };
            sizes.push(size);
            source = size;
        }
        let fits = |img: &Option<Image>, (w, h): (u32, u32)| img.as_ref().is_some_and(|i| i.width == w && i.height == h);
        let original_ok = fits(&self.original, original)
            && self.history.len() == self.history_depth
            && self.history.iter().all(|h| h.width == original.0 && h.height == original.1);
        let passes_ok = self.passes.iter().zip(&sizes).all(|(p, &s)| {
            (p.last || fits(&p.output, s)) && (if p.has_feedback { fits(&p.feedback, s) } else { p.feedback.is_none() })
        });
        let output_ok = fits(&self.output, viewport);
        if original_ok && passes_ok && output_ok {
            return Ok(());
        }
        unsafe { self.gpu.device.device_wait_idle() }.map_err(vkerr("wait idle"))?;
        let attachment = vk::ImageUsageFlags::COLOR_ATTACHMENT
            | vk::ImageUsageFlags::SAMPLED
            | vk::ImageUsageFlags::TRANSFER_SRC
            | vk::ImageUsageFlags::TRANSFER_DST;
        if !original_ok {
            for img in self.original.take().into_iter().chain(self.history.drain(..)) {
                self.gpu.destroy_image(img);
            }
            let levels = if self.original_mipmaps && self.mip_capable(self.original_format) {
                gpu::mip_levels(original.0, original.1)
            } else {
                1
            };
            self.original = Some(self.gpu.create_image(original.0, original.1, self.original_format, levels, attachment)?);
            for _ in 0..self.history_depth {
                let img = self.gpu.create_image(original.0, original.1, self.original_format, levels, attachment)?;
                self.history.push_back(img);
            }
        }
        for i in 0..self.passes.len() {
            let size = sizes[i];
            let p = &self.passes[i];
            let need_feedback = p.has_feedback;
            let levels = if p.mipmaps && self.mip_capable(p.data.format) { gpu::mip_levels(size.0, size.1) } else { 1 };
            let format = p.data.format;
            let output_ok = p.last || fits(&p.output, size);
            let feedback_ok = if need_feedback { fits(&p.feedback, size) } else { p.feedback.is_none() };
            let p = &mut self.passes[i];
            p.size = size;
            if !output_ok || !feedback_ok {
                for img in [p.output.take(), p.feedback.take()].into_iter().flatten() {
                    self.gpu.destroy_image(img);
                }
                if !p.last {
                    p.output = Some(self.gpu.create_image(size.0, size.1, format, levels, attachment)?);
                }
                if need_feedback {
                    p.feedback = Some(self.gpu.create_image(size.0, size.1, format, levels, attachment)?);
                }
            }
        }
        if !output_ok {
            if let Some(img) = self.output.take() {
                self.gpu.destroy_image(img);
            }
            self.output = Some(self.gpu.create_image(viewport.0, viewport.1, self.out_format, 1, attachment)?);
        }
        Ok(())
    }

    /// target for the renderer's picture draw (after `prepare`); rotates the history first on
    /// an advance. The image is in COLOR_ATTACHMENT_OPTIMAL afterwards.
    pub fn begin_original(&mut self, cmd: vk::CommandBuffer, advance: bool) -> (vk::ImageView, u32, u32) {
        let d = &self.gpu.device;
        if advance && self.history_depth > 0 && self.original.as_ref().is_some_and(|o| o.ready) {
            let old = self.original.take().unwrap();
            self.original = self.history.pop_back();
            self.history.push_front(old);
        }
        let o = self.original.as_ref().unwrap();
        gpu::to_target(d, cmd, o);
        (o.target, o.width, o.height)
    }

    /// Records all passes (after the picture was drawn into the original). The output is in
    /// SHADER_READ_ONLY_OPTIMAL afterwards.
    pub fn record(&mut self, cmd: vk::CommandBuffer, fi: usize, info: &RunInfo) -> Result<()> {
        let d = self.gpu.device.clone();
        gpu::to_sampled(&d, cmd, self.original.as_mut().unwrap());
        for h in self.history.iter_mut().filter(|h| !h.ready) {
            gpu::clear(&d, cmd, h);
        }
        if info.advance {
            let now = crate::timing::now_ns();
            self.frame_time_us = if self.last_advance_ns == 0 {
                0
            } else {
                ((now - self.last_advance_ns) / 1000).min(u32::MAX as u64) as u32
            };
            self.last_advance_ns = now;
            if self.ran_for.is_some() {
                self.frame_count = self.frame_count.wrapping_add(1);
            }
            for p in &mut self.passes {
                if p.feedback.is_some() && !p.last {
                    std::mem::swap(&mut p.output, &mut p.feedback);
                }
            }
        }
        for p in &mut self.passes {
            if let Some(f) = p.feedback.as_mut().filter(|f| !f.ready) {
                gpu::clear(&d, cmd, f);
            }
        }
        let viewport = {
            let o = self.output.as_ref().unwrap();
            (o.width, o.height)
        };
        for i in 0..self.passes.len() {
            self.record_pass(&d, cmd, fi, i, info, viewport)?;
        }
        // feedback of the last pass: copy of the output (it renders into the shared output)
        let last = self.passes.len() - 1;
        if let Some(fb) = self.passes[last].feedback.as_ref()
            && info.advance
        {
            let out = self.output.as_ref().unwrap();
            copy_image(&d, cmd, out, fb);
        }
        let o = self.original.as_ref().unwrap();
        self.ran_for = Some(((o.width, o.height), viewport));
        self.dirty = false;
        Ok(())
    }

    fn texture(&self, tex: Tex, pass: usize) -> Option<&Image> {
        match tex {
            Tex::Original => self.original.as_ref(),
            Tex::Source if pass == 0 => self.original.as_ref(),
            Tex::Source => self.passes[pass - 1].output.as_ref(),
            Tex::History(n) => self.history.get(n as usize - 1),
            Tex::PassOutput(n) => self.passes[n as usize].output.as_ref(),
            Tex::PassFeedback(n) => self.passes[n as usize].feedback.as_ref(),
            Tex::Lut(n) => Some(&self.luts[n as usize].image),
        }
    }

    fn texture_key(&self, tex: Tex, pass: usize, linear: bool) -> SamplerKey {
        match tex {
            Tex::Original | Tex::History(_) => self.input_key(0, linear),
            Tex::Source => self.input_key(pass, linear),
            Tex::PassOutput(n) | Tex::PassFeedback(n) => self.input_key(n as usize + 1, linear),
            Tex::Lut(n) => self.luts[n as usize].key,
        }
    }

    fn size_of(&self, tex: Tex, pass: usize) -> (u32, u32) {
        if let Tex::PassOutput(n) | Tex::PassFeedback(n) = tex
            && self.passes[n as usize].last
        {
            let o = self.output.as_ref().unwrap();
            return (o.width, o.height);
        }
        self.texture(tex, pass).map_or((1, 1), |i| (i.width, i.height))
    }

    fn record_pass(
        &mut self,
        d: &ash::Device,
        cmd: vk::CommandBuffer,
        fi: usize,
        i: usize,
        info: &RunInfo,
        viewport: (u32, u32),
    ) -> Result<()> {
        // descriptors
        let mut images = Vec::new();
        for t in &self.passes[i].data.textures {
            let key = self.texture_key(t.tex, i, info.linear);
            // the output of the last pass is only readable as feedback (copied after the run)
            let view = self.texture(t.tex, i).map(|img| img.view);
            images.push((t.binding, key, view));
        }
        let mut infos = Vec::new();
        for (binding, key, view) in images {
            let sampler = self.sampler(key)?;
            let view = view.unwrap_or(self.original.as_ref().unwrap().view);
            infos.push((
                binding,
                [vk::DescriptorImageInfo::default()
                    .image_view(view)
                    .sampler(sampler)
                    .image_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)],
            ));
        }
        let p = &self.passes[i];
        let set = p.sets[fi];
        let ubo_info = p.ubos.get(fi).map(|b| [vk::DescriptorBufferInfo::default().buffer(b.buffer).range(vk::WHOLE_SIZE)]);
        let mut writes = Vec::new();
        if let (Some(b), Some(ui)) = (p.data.ubo_binding, ubo_info.as_ref()) {
            writes.push(
                vk::WriteDescriptorSet::default()
                    .dst_set(set)
                    .dst_binding(b)
                    .descriptor_type(vk::DescriptorType::UNIFORM_BUFFER)
                    .buffer_info(ui),
            );
        }
        for (binding, ii) in &infos {
            writes.push(
                vk::WriteDescriptorSet::default()
                    .dst_set(set)
                    .dst_binding(*binding)
                    .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
                    .image_info(ii),
            );
        }
        unsafe { d.update_descriptor_sets(&writes, &[]) };

        // uniforms
        let size = p.size;
        let mut push = [0u8; 128];
        if let Some(ubo) = p.ubos.get(fi) {
            let buf = unsafe { std::slice::from_raw_parts_mut(ubo.ptr, ubo.size as usize) };
            for s in &p.data.ubo {
                self.write_slot(buf, s, i, size, viewport, info);
            }
        }
        for s in &p.data.push {
            self.write_slot(&mut push, s, i, size, viewport, info);
        }

        let p = &self.passes[i];
        let target = if p.last { self.output.as_ref().unwrap() } else { p.output.as_ref().unwrap() };
        gpu::to_target(d, cmd, target);
        let attachments = [vk::RenderingAttachmentInfo::default()
            .image_view(target.target)
            .image_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
            .load_op(vk::AttachmentLoadOp::CLEAR)
            .store_op(vk::AttachmentStoreOp::STORE)
            .clear_value(vk::ClearValue { color: vk::ClearColorValue { float32: [0.0; 4] } })];
        let area = vk::Rect2D {
            offset: vk::Offset2D::default(),
            extent: vk::Extent2D { width: size.0, height: size.1 },
        };
        unsafe {
            d.cmd_begin_rendering(cmd, &vk::RenderingInfo::default().render_area(area).layer_count(1).color_attachments(&attachments));
            d.cmd_set_viewport(
                cmd,
                0,
                &[vk::Viewport {
                    x: 0.0,
                    y: 0.0,
                    width: size.0 as f32,
                    height: size.1 as f32,
                    min_depth: 0.0,
                    max_depth: 1.0,
                }],
            );
            d.cmd_set_scissor(cmd, 0, &[area]);
            d.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, p.pipeline);
            d.cmd_bind_descriptor_sets(cmd, vk::PipelineBindPoint::GRAPHICS, p.layout, 0, &[set], &[]);
            if p.data.push_size > 0 {
                let n = (p.data.push_size.div_ceil(4) * 4) as usize;
                d.cmd_push_constants(
                    cmd,
                    p.layout,
                    vk::ShaderStageFlags::VERTEX | vk::ShaderStageFlags::FRAGMENT,
                    0,
                    &push[..n],
                );
            }
            d.cmd_bind_vertex_buffers(cmd, 0, &[self.vertices.buffer], &[0]);
            d.cmd_draw(cmd, 4, 1, 0, 0);
            d.cmd_end_rendering(cmd);
        }
        let last = p.last;
        let target = if last { self.output.as_mut().unwrap() } else { self.passes[i].output.as_mut().unwrap() };
        gpu::to_sampled(d, cmd, target);
        Ok(())
    }

    fn write_slot(&self, buf: &mut [u8], s: &Slot, pass: usize, size: (u32, u32), viewport: (u32, u32), info: &RunInfo) {
        let vec4 = |(w, h): (u32, u32)| [w as f32, h as f32, 1.0 / w.max(1) as f32, 1.0 / h.max(1) as f32];
        let p = &self.passes[pass];
        let aspect = viewport.0 as f32 / viewport.1.max(1) as f32;
        let values: Vec<f32> = match s.uni {
            Uni::Mvp => (if p.last { info.mvp } else { MVP_OFFSCREEN }).to_vec(),
            Uni::OutputSize => vec4(size).to_vec(),
            Uni::FinalViewportSize => vec4(viewport).to_vec(),
            Uni::FrameCount => {
                let m = p.data.config.frame_count_mod;
                vec![(if m > 0 { self.frame_count % m } else { self.frame_count }) as f32]
            }
            Uni::FrameDirection => vec![1.0],
            Uni::FrameTimeDelta => vec![self.frame_time_us as f32],
            Uni::OriginalFps => vec![info.fps],
            Uni::Rotation => vec![info.rotation as f32],
            Uni::OriginalAspect => vec![if info.rotation % 2 == 1 { 1.0 / aspect } else { aspect }],
            Uni::OriginalAspectRotated => vec![aspect],
            Uni::TotalSubFrames | Uni::CurrentSubFrame => vec![1.0],
            Uni::Hdr(v) => vec![v],
            Uni::Size(t) => vec4(self.size_of(t, pass)).to_vec(),
            Uni::Param(n) => vec![self.params[n as usize].value],
            Uni::Unknown => vec![0.0],
        };
        let n = (s.components as usize).clamp(1, 16).min(values.len());
        for (k, v) in values[..n].iter().enumerate() {
            let off = s.offset as usize + k * 4;
            if off + 4 > buf.len() {
                break;
            }
            let bytes = match s.base {
                Base::Int => (*v as i32).to_le_bytes(),
                Base::Uint => (v.max(0.0) as u32).to_le_bytes(),
                _ => v.to_le_bytes(),
            };
            buf[off..off + 4].copy_from_slice(&bytes);
        }
        // integer semantics written exactly (f32 loses precision above 2^24)
        if let (Uni::FrameCount | Uni::FrameTimeDelta, Base::Int | Base::Uint) = (s.uni, s.base) {
            let v = match s.uni {
                Uni::FrameCount => {
                    let m = p.data.config.frame_count_mod;
                    if m > 0 { self.frame_count % m } else { self.frame_count }
                }
                _ => self.frame_time_us,
            };
            let off = s.offset as usize;
            if off + 4 <= buf.len() {
                buf[off..off + 4].copy_from_slice(&v.to_le_bytes());
            }
        }
    }

    pub fn output_view(&self) -> Option<vk::ImageView> {
        self.output.as_ref().map(|o| o.view)
    }

    pub fn pass_count(&self) -> usize {
        self.passes.len()
    }

    /// copies the output (SHADER_READ_ONLY after `record`) into a host buffer, tightly packed
    /// (corpus test readback); the output stays shader readable
    pub(crate) fn copy_output(&self, cmd: vk::CommandBuffer, dst: vk::Buffer) {
        let d = &self.gpu.device;
        let Some(out) = self.output.as_ref() else { return };
        let read = (vk::PipelineStageFlags2::ALL_GRAPHICS, vk::AccessFlags2::SHADER_SAMPLED_READ);
        let copy = (vk::PipelineStageFlags2::COPY, vk::AccessFlags2::TRANSFER_READ);
        gpu::barrier(
            d,
            cmd,
            out.image,
            gpu::range(0, 1),
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
            vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
            (vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT, vk::AccessFlags2::COLOR_ATTACHMENT_WRITE),
            copy,
        );
        let region = vk::BufferImageCopy::default()
            .image_subresource(gpu::layers(0))
            .image_extent(vk::Extent3D { width: out.width, height: out.height, depth: 1 });
        unsafe { d.cmd_copy_image_to_buffer(cmd, out.image, vk::ImageLayout::TRANSFER_SRC_OPTIMAL, dst, &[region]) };
        gpu::barrier(
            d,
            cmd,
            out.image,
            gpu::range(0, 1),
            vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
            copy,
            read,
        );
    }
}

/// output (SHADER_READ_ONLY) -> feedback copy (SHADER_READ_ONLY), same size
fn copy_image(d: &ash::Device, cmd: vk::CommandBuffer, src: &Image, dst: &Image) {
    let read = (vk::PipelineStageFlags2::ALL_GRAPHICS, vk::AccessFlags2::SHADER_SAMPLED_READ);
    gpu::barrier(
        d,
        cmd,
        src.image,
        gpu::range(0, 1),
        vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
        vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
        (vk::PipelineStageFlags2::ALL_GRAPHICS, vk::AccessFlags2::NONE),
        (vk::PipelineStageFlags2::COPY, vk::AccessFlags2::TRANSFER_READ),
    );
    gpu::barrier(
        d,
        cmd,
        dst.image,
        gpu::range(0, dst.levels),
        vk::ImageLayout::UNDEFINED,
        vk::ImageLayout::TRANSFER_DST_OPTIMAL,
        (vk::PipelineStageFlags2::ALL_COMMANDS, vk::AccessFlags2::MEMORY_WRITE),
        (vk::PipelineStageFlags2::COPY, vk::AccessFlags2::TRANSFER_WRITE),
    );
    let region = vk::ImageCopy::default()
        .src_subresource(gpu::layers(0))
        .dst_subresource(gpu::layers(0))
        .extent(vk::Extent3D { width: src.width.min(dst.width), height: src.height.min(dst.height), depth: 1 });
    unsafe {
        d.cmd_copy_image(
            cmd,
            src.image,
            vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
            dst.image,
            vk::ImageLayout::TRANSFER_DST_OPTIMAL,
            &[region],
        );
    }
    gpu::barrier(
        d,
        cmd,
        src.image,
        gpu::range(0, 1),
        vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
        vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
        (vk::PipelineStageFlags2::COPY, vk::AccessFlags2::TRANSFER_READ),
        read,
    );
    gpu::barrier(
        d,
        cmd,
        dst.image,
        gpu::range(0, dst.levels),
        vk::ImageLayout::TRANSFER_DST_OPTIMAL,
        vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
        (vk::PipelineStageFlags2::COPY, vk::AccessFlags2::TRANSFER_WRITE),
        read,
    );
}

impl Drop for Chain {
    fn drop(&mut self) {
        let d = &self.gpu.device;
        unsafe {
            let _ = d.device_wait_idle();
        }
        for p in std::mem::take(&mut self.passes) {
            for img in [p.output, p.feedback].into_iter().flatten() {
                self.gpu.destroy_image(img);
            }
            for b in p.ubos {
                self.gpu.destroy_buffer(b);
            }
            unsafe {
                d.destroy_pipeline(p.pipeline, None);
                d.destroy_pipeline_layout(p.layout, None);
                d.destroy_descriptor_set_layout(p.set_layout, None);
            }
        }
        for l in std::mem::take(&mut self.luts) {
            self.gpu.destroy_image(l.image);
        }
        for img in self.original.take().into_iter().chain(self.history.drain(..)).chain(self.output.take()) {
            self.gpu.destroy_image(img);
        }
        unsafe {
            for (_, s) in self.samplers.drain() {
                d.destroy_sampler(s, None);
            }
            d.destroy_descriptor_pool(self.desc_pool, None);
        }
        let vertices = std::mem::replace(
            &mut self.vertices,
            Buffer {
                buffer: vk::Buffer::null(),
                memory: vk::DeviceMemory::null(),
                size: 0,
                ptr: std::ptr::null_mut(),
            },
        );
        self.gpu.destroy_buffer(vertices);
    }
}
