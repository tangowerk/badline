//! Present feedback for frame pacing (M-Pacing1).
//!
//! ash 0.38 has no bindings for `VK_KHR_present_id2`, `VK_KHR_present_wait2` and
//! `VK_EXT_present_timing`; the few structs and entry points used here are written by hand
//! from vulkan_core.h (Vulkan-Headers main, `VK_EXT_PRESENT_TIMING_SPEC_VERSION` 3).
//!
//! Two feedback paths:
//! - V2: present_id2 + present_wait2 + present_timing: real display timestamps per present.
//! - V1: present_id + present_wait: the completion time of the wait is the display estimate
//!   (only known for presents the renderer actually waits for).

use std::collections::VecDeque;
use std::ffi::{CStr, c_void};

use ash::vk;

pub const PRESENT_ID_2: &CStr = c"VK_KHR_present_id2";
pub const PRESENT_WAIT_2: &CStr = c"VK_KHR_present_wait2";
pub const PRESENT_TIMING: &CStr = c"VK_EXT_present_timing";
pub const CALIBRATED_TIMESTAMPS: &CStr = c"VK_KHR_calibrated_timestamps";

const ST_SURFACE_CAPABILITIES_PRESENT_ID_2: i32 = 1000479000;
const ST_PRESENT_ID_2: i32 = 1000479001;
const ST_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES: i32 = 1000479002;
const ST_SURFACE_CAPABILITIES_PRESENT_WAIT_2: i32 = 1000480000;
const ST_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES: i32 = 1000480001;
const ST_PRESENT_WAIT_2_INFO: i32 = 1000480002;
const ST_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES: i32 = 1000208000;
const ST_SWAPCHAIN_TIMING_PROPERTIES: i32 = 1000208001;
const ST_SWAPCHAIN_TIME_DOMAIN_PROPERTIES: i32 = 1000208002;
const ST_PRESENT_TIMINGS_INFO: i32 = 1000208003;
const ST_PRESENT_TIMING_INFO: i32 = 1000208004;
const ST_PAST_PRESENTATION_TIMING_INFO: i32 = 1000208005;
const ST_PAST_PRESENTATION_TIMING_PROPERTIES: i32 = 1000208006;
const ST_PAST_PRESENTATION_TIMING: i32 = 1000208007;
const ST_PRESENT_TIMING_SURFACE_CAPABILITIES: i32 = 1000208008;

pub const SWAPCHAIN_CREATE_PRESENT_ID_2: u32 = 0x40;
pub const SWAPCHAIN_CREATE_PRESENT_WAIT_2: u32 = 0x80;
pub const SWAPCHAIN_CREATE_PRESENT_TIMING: u32 = 0x200;

pub const STAGE_QUEUE_OPERATIONS_END: u32 = 0x1;
pub const STAGE_FIRST_PIXEL_OUT: u32 = 0x4;
pub const STAGE_FIRST_PIXEL_VISIBLE: u32 = 0x8;

pub const TIME_DOMAIN_CLOCK_MONOTONIC: i32 = 1;
pub const TIME_DOMAIN_CLOCK_MONOTONIC_RAW: i32 = 2;

const SUCCESS: i32 = 0;
pub const ERROR_PRESENT_TIMING_QUEUE_FULL: i32 = -1000208000;
const INCOMPLETE: i32 = 5;

fn st(v: i32) -> vk::StructureType {
    vk::StructureType::from_raw(v)
}

/// inserts `item` at the head of the pNext chain of `head` (like ash's `push_next`)
///
/// # Safety
/// `head` and `item` must be Vulkan structs starting with sType/pNext and outlive the call using them.
pub unsafe fn chain<H, T>(head: &mut H, item: &mut T) {
    #[repr(C)]
    struct Base {
        s_type: vk::StructureType,
        p_next: *mut c_void,
    }
    let h = head as *mut H as *mut Base;
    let i = item as *mut T as *mut Base;
    unsafe {
        (*i).p_next = (*h).p_next;
        (*h).p_next = i as *mut c_void;
    }
}

#[repr(C)]
pub struct Bool {
    pub s_type: vk::StructureType,
    pub p_next: *mut c_void,
    pub value: vk::Bool32,
}

impl Bool {
    fn new(s_type: i32) -> Self {
        Bool {
            s_type: st(s_type),
            p_next: std::ptr::null_mut(),
            value: vk::FALSE,
        }
    }
    pub fn feature_present_id2() -> Self {
        Self::new(ST_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES)
    }
    pub fn feature_present_wait2() -> Self {
        Self::new(ST_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES)
    }
    pub fn surface_present_id2() -> Self {
        Self::new(ST_SURFACE_CAPABILITIES_PRESENT_ID_2)
    }
    pub fn surface_present_wait2() -> Self {
        Self::new(ST_SURFACE_CAPABILITIES_PRESENT_WAIT_2)
    }
    pub fn on(mut self) -> Self {
        self.value = vk::TRUE;
        self
    }
}

#[repr(C)]
pub struct PresentTimingFeatures {
    pub s_type: vk::StructureType,
    pub p_next: *mut c_void,
    pub present_timing: vk::Bool32,
    pub present_at_absolute_time: vk::Bool32,
    pub present_at_relative_time: vk::Bool32,
}

impl Default for PresentTimingFeatures {
    fn default() -> Self {
        PresentTimingFeatures {
            s_type: st(ST_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES),
            p_next: std::ptr::null_mut(),
            present_timing: vk::FALSE,
            present_at_absolute_time: vk::FALSE,
            present_at_relative_time: vk::FALSE,
        }
    }
}

#[repr(C)]
pub struct PresentTimingSurfaceCaps {
    pub s_type: vk::StructureType,
    pub p_next: *mut c_void,
    pub present_timing_supported: vk::Bool32,
    pub present_at_absolute_time_supported: vk::Bool32,
    pub present_at_relative_time_supported: vk::Bool32,
    pub present_stage_queries: u32,
}

impl Default for PresentTimingSurfaceCaps {
    fn default() -> Self {
        PresentTimingSurfaceCaps {
            s_type: st(ST_PRESENT_TIMING_SURFACE_CAPABILITIES),
            p_next: std::ptr::null_mut(),
            present_timing_supported: vk::FALSE,
            present_at_absolute_time_supported: vk::FALSE,
            present_at_relative_time_supported: vk::FALSE,
            present_stage_queries: 0,
        }
    }
}

#[repr(C)]
pub struct PresentId2 {
    pub s_type: vk::StructureType,
    pub p_next: *const c_void,
    pub swapchain_count: u32,
    pub present_ids: *const u64,
}

impl PresentId2 {
    pub fn new(id: &u64) -> Self {
        PresentId2 {
            s_type: st(ST_PRESENT_ID_2),
            p_next: std::ptr::null(),
            swapchain_count: 1,
            present_ids: id,
        }
    }
}

#[repr(C)]
struct PresentWait2Info {
    s_type: vk::StructureType,
    p_next: *const c_void,
    present_id: u64,
    timeout: u64,
}

#[repr(C)]
pub struct PresentTimingInfo {
    pub s_type: vk::StructureType,
    pub p_next: *const c_void,
    pub flags: u32,
    pub target_time: u64,
    pub time_domain_id: u64,
    pub present_stage_queries: u32,
    pub target_time_domain_present_stage: u32,
}

#[repr(C)]
pub struct PresentTimingsInfo {
    pub s_type: vk::StructureType,
    pub p_next: *const c_void,
    pub swapchain_count: u32,
    pub timing_infos: *const PresentTimingInfo,
}

impl PresentTimingsInfo {
    pub fn new(info: &PresentTimingInfo) -> Self {
        PresentTimingsInfo {
            s_type: st(ST_PRESENT_TIMINGS_INFO),
            p_next: std::ptr::null(),
            swapchain_count: 1,
            timing_infos: info,
        }
    }
}

#[repr(C)]
struct SwapchainTimingProperties {
    s_type: vk::StructureType,
    p_next: *mut c_void,
    refresh_duration: u64,
    refresh_interval: u64,
}

#[repr(C)]
struct SwapchainTimeDomainProperties {
    s_type: vk::StructureType,
    p_next: *mut c_void,
    time_domain_count: u32,
    time_domains: *mut i32,
    time_domain_ids: *mut u64,
}

#[repr(C)]
struct PastPresentationTimingInfo {
    s_type: vk::StructureType,
    p_next: *const c_void,
    flags: u32,
    swapchain: vk::SwapchainKHR,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct PresentStageTime {
    stage: u32,
    time: u64,
}

#[repr(C)]
struct PastPresentationTiming {
    s_type: vk::StructureType,
    p_next: *mut c_void,
    present_id: u64,
    target_time: u64,
    present_stage_count: u32,
    present_stages: *mut PresentStageTime,
    time_domain: i32,
    time_domain_id: u64,
    report_complete: vk::Bool32,
}

#[repr(C)]
struct PastPresentationTimingProperties {
    s_type: vk::StructureType,
    p_next: *mut c_void,
    timing_properties_counter: u64,
    time_domains_counter: u64,
    presentation_timing_count: u32,
    presentation_timings: *mut PastPresentationTiming,
}

type PfnWaitForPresent2 = unsafe extern "system" fn(vk::Device, vk::SwapchainKHR, *const PresentWait2Info) -> i32;
type PfnSetQueueSize = unsafe extern "system" fn(vk::Device, vk::SwapchainKHR, u32) -> i32;
type PfnGetTimingProperties =
    unsafe extern "system" fn(vk::Device, vk::SwapchainKHR, *mut SwapchainTimingProperties, *mut u64) -> i32;
type PfnGetTimeDomainProperties =
    unsafe extern "system" fn(vk::Device, vk::SwapchainKHR, *mut SwapchainTimeDomainProperties, *mut u64) -> i32;
type PfnGetPastPresentationTiming =
    unsafe extern "system" fn(vk::Device, *const PastPresentationTimingInfo, *mut PastPresentationTimingProperties) -> i32;

/// device entry points of the V2 path
pub struct Fns {
    wait_for_present2: PfnWaitForPresent2,
    set_queue_size: PfnSetQueueSize,
    get_timing_properties: PfnGetTimingProperties,
    get_time_domain_properties: PfnGetTimeDomainProperties,
    get_past_presentation_timing: PfnGetPastPresentationTiming,
}

impl Fns {
    pub fn load(instance: &ash::Instance, device: vk::Device) -> Option<Self> {
        let get = |name: &CStr| unsafe { instance.get_device_proc_addr(device, name.as_ptr()) };
        unsafe {
            Some(Fns {
                wait_for_present2: std::mem::transmute::<vk::PFN_vkVoidFunction, Option<PfnWaitForPresent2>>(get(
                    c"vkWaitForPresent2KHR",
                ))?,
                set_queue_size: std::mem::transmute::<vk::PFN_vkVoidFunction, Option<PfnSetQueueSize>>(get(
                    c"vkSetSwapchainPresentTimingQueueSizeEXT",
                ))?,
                get_timing_properties: std::mem::transmute::<vk::PFN_vkVoidFunction, Option<PfnGetTimingProperties>>(
                    get(c"vkGetSwapchainTimingPropertiesEXT"),
                )?,
                get_time_domain_properties: std::mem::transmute::<
                    vk::PFN_vkVoidFunction,
                    Option<PfnGetTimeDomainProperties>,
                >(get(c"vkGetSwapchainTimeDomainPropertiesEXT"))?,
                get_past_presentation_timing: std::mem::transmute::<
                    vk::PFN_vkVoidFunction,
                    Option<PfnGetPastPresentationTiming>,
                >(get(c"vkGetPastPresentationTimingEXT"))?,
            })
        }
    }
}

/// CLOCK_MONOTONIC in nanoseconds (same clock as Vulkan's CLOCK_MONOTONIC time domain)
pub fn now_ns() -> u64 {
    #[repr(C)]
    struct Timespec {
        sec: i64,
        nsec: i64,
    }
    unsafe extern "C" {
        fn clock_gettime(clock: i32, ts: *mut Timespec) -> i32;
    }
    let mut ts = Timespec { sec: 0, nsec: 0 };
    unsafe { clock_gettime(1, &mut ts) };
    ts.sec as u64 * 1_000_000_000 + ts.nsec as u64
}

/// sleeps until CLOCK_MONOTONIC reaches `ns` (returns at once when it is in the past)
pub fn sleep_until(ns: u64) {
    #[repr(C)]
    struct Timespec {
        sec: i64,
        nsec: i64,
    }
    unsafe extern "C" {
        fn clock_nanosleep(clock: i32, flags: i32, req: *const Timespec, rem: *mut Timespec) -> i32;
    }
    const TIMER_ABSTIME: i32 = 1;
    const EINTR: i32 = 4;
    let ts = Timespec { sec: (ns / 1_000_000_000) as i64, nsec: (ns % 1_000_000_000) as i64 };
    while unsafe { clock_nanosleep(1, TIMER_ABSTIME, &ts, std::ptr::null_mut()) } == EINTR {}
}

/// flags of a present record (see BVK_PRESENT_* in bvk.h)
pub const REC_REPEAT: u32 = 1;
pub const REC_REDRAW: u32 = 2;
pub const REC_TIMING: u32 = 4;
pub const REC_WAITED: u32 = 8;
pub const REC_LOST: u32 = 16;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Record {
    pub present_id: u64,
    /// C64 frame sequence number (0: redraw without a new emulator frame)
    pub frame: u64,
    /// CLOCK_MONOTONIC ns just before vkQueuePresentKHR
    pub submit_ns: u64,
    /// display time (ns, CLOCK_MONOTONIC unless REC_TIMING with another domain); 0 = unknown
    pub display_ns: u64,
    pub flags: u32,
    pub copy: u32,
}

const MAX_PENDING: usize = 64;
const MAX_DONE: usize = 4096;

/// per swapchain feedback state; records wait in `pending` until their display time is known
pub struct Feedback {
    pub v2: Option<Fns>,
    /// V2: time domain id used for the queries (CLOCK_MONOTONIC preferred)
    pub domain_id: u64,
    pub domain: i32,
    pub stage: u32,
    pub refresh_ns: u64,
    pub timing_active: bool,
    /// presents with timing info whose results were not collected yet (driver queue usage)
    outstanding: u32,
    /// last seen `timingPropertiesCounter`
    timing_counter: u64,
    pending: VecDeque<Record>,
    done: VecDeque<Record>,
    stages: Vec<PresentStageTime>,
    timings: Vec<PastPresentationTiming>,
    /// grid cadence detection (VRR): period of the target time grid (0 = off)
    grid_period: u64,
    /// last displayed first present of a picture: (frame, display time)
    grid_last: (u64, u64),
    /// recent picture intervals, true = within CADENCE_TOLERANCE of the grid period
    grid_hits: VecDeque<bool>,
}

/// picture intervals looked at for the cadence decision (~1 s at 50 fps)
const CADENCE_WINDOW: usize = 50;
const CADENCE_TOLERANCE: u64 = 1_000_000;

impl Feedback {
    pub fn new(v2: Option<Fns>, stage: u32) -> Self {
        Feedback {
            v2,
            domain_id: 0,
            domain: 0,
            stage,
            refresh_ns: 0,
            timing_active: false,
            outstanding: 0,
            timing_counter: 0,
            pending: VecDeque::new(),
            done: VecDeque::new(),
            stages: Vec::new(),
            timings: Vec::new(),
            grid_period: 0,
            grid_last: (0, 0),
            grid_hits: VecDeque::new(),
        }
    }

    /// after (re)creating a swapchain with the present-timing flag: queue size, time domain, refresh
    pub fn setup_swapchain(&mut self, device: vk::Device, swapchain: vk::SwapchainKHR) {
        self.timing_active = false;
        self.outstanding = 0;
        self.timing_counter = 0;
        let Some(f) = &self.v2 else { return };
        if self.stage == 0 {
            return;
        }
        unsafe {
            if (f.set_queue_size)(device, swapchain, MAX_PENDING as u32) != SUCCESS {
                return;
            }
            let mut props = SwapchainTimeDomainProperties {
                s_type: st(ST_SWAPCHAIN_TIME_DOMAIN_PROPERTIES),
                p_next: std::ptr::null_mut(),
                time_domain_count: 0,
                time_domains: std::ptr::null_mut(),
                time_domain_ids: std::ptr::null_mut(),
            };
            let mut counter = 0u64;
            if (f.get_time_domain_properties)(device, swapchain, &mut props, &mut counter) != SUCCESS {
                return;
            }
            let n = props.time_domain_count as usize;
            let mut domains = vec![0i32; n];
            let mut ids = vec![0u64; n];
            props.time_domains = domains.as_mut_ptr();
            props.time_domain_ids = ids.as_mut_ptr();
            let res = (f.get_time_domain_properties)(device, swapchain, &mut props, &mut counter);
            if res != SUCCESS && res != INCOMPLETE {
                return;
            }
            let n = (props.time_domain_count as usize).min(n);
            let pick = (0..n)
                .find(|&i| domains[i] == TIME_DOMAIN_CLOCK_MONOTONIC)
                .or_else(|| (0..n).find(|&i| domains[i] == TIME_DOMAIN_CLOCK_MONOTONIC_RAW))
                .or(if n > 0 { Some(0) } else { None });
            let Some(i) = pick else { return };
            self.domain = domains[i];
            self.domain_id = ids[i];
            self.update_refresh(device, swapchain);
        }
        self.timing_active = true;
    }

    fn update_refresh(&mut self, device: vk::Device, swapchain: vk::SwapchainKHR) {
        let Some(f) = &self.v2 else { return };
        let mut props = SwapchainTimingProperties {
            s_type: st(ST_SWAPCHAIN_TIMING_PROPERTIES),
            p_next: std::ptr::null_mut(),
            refresh_duration: 0,
            refresh_interval: 0,
        };
        let mut counter = 0u64;
        if unsafe { (f.get_timing_properties)(device, swapchain, &mut props, &mut counter) } == SUCCESS {
            self.refresh_ns = props.refresh_duration;
        }
    }

    /// timing info for the next present (V2 with an active timing queue); None while the
    /// driver's timing queue is nearly full (results not reported, e.g. hidden window), a
    /// present with a full queue would fail with VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT
    pub fn timing_info(&self) -> Option<PresentTimingInfo> {
        (self.timing_active && (self.outstanding as usize) < MAX_PENDING - 2).then(|| PresentTimingInfo {
            s_type: st(ST_PRESENT_TIMING_INFO),
            p_next: std::ptr::null(),
            flags: 0,
            target_time: 0,
            time_domain_id: self.domain_id,
            present_stage_queries: self.stage,
            target_time_domain_present_stage: 0,
        })
    }

    /// `timed`: the present carried timing info (its result will be reported)
    pub fn push(&mut self, mut rec: Record, timed: bool) {
        if timed {
            rec.flags |= REC_TIMING;
            self.outstanding += 1;
        }
        if self.pending.len() >= MAX_PENDING {
            let mut old = self.pending.pop_front().unwrap();
            old.flags |= REC_LOST;
            self.finish(old);
        }
        self.pending.push_back(rec);
    }

    pub fn set_grid(&mut self, period: u64) {
        if self.grid_period != period {
            self.grid_period = period;
            self.grid_last = (0, 0);
            self.grid_hits.clear();
        }
    }

    /// most of the recent pictures were displayed one grid period after their predecessor:
    /// the display follows the target times (VRR, or a refresh equal to the grid)
    pub fn cadence(&self) -> bool {
        self.grid_hits.len() >= CADENCE_WINDOW / 2
            && self.grid_hits.iter().filter(|&&h| h).count() * 10 >= self.grid_hits.len() * 9
    }

    fn track_cadence(&mut self, rec: &Record) {
        if self.grid_period == 0 || rec.copy != 0 || rec.flags & (REC_REDRAW | REC_LOST) != 0 || rec.display_ns == 0 {
            return;
        }
        let (frame, time) = self.grid_last;
        if frame != 0 && rec.frame == frame + 1 {
            let dt = rec.display_ns.wrapping_sub(time);
            if self.grid_hits.len() >= CADENCE_WINDOW {
                self.grid_hits.pop_front();
            }
            self.grid_hits.push_back(dt.abs_diff(self.grid_period) <= CADENCE_TOLERANCE);
        }
        self.grid_last = (rec.frame, rec.display_ns);
    }

    fn finish(&mut self, rec: Record) {
        self.track_cadence(&rec);
        if self.done.len() >= MAX_DONE {
            self.done.pop_front();
        }
        self.done.push_back(rec);
    }

    /// V1/V2 wait result: `id` was displayed (wait returned) at `ns`
    pub fn waited(&mut self, id: u64, ns: u64) {
        if let Some(r) = self.pending.iter_mut().find(|r| r.present_id == id) {
            r.flags |= REC_WAITED;
            if r.flags & REC_TIMING == 0 {
                r.display_ns = ns;
            }
        }
        if !self.timing_active {
            // without present timing nothing more will be learned about presents up to `id`
            while self.pending.front().is_some_and(|r| r.present_id <= id) {
                let r = self.pending.pop_front().unwrap();
                self.finish(r);
            }
        }
    }

    /// V2: collects finished timings (non-blocking)
    pub fn poll(&mut self, device: vk::Device, swapchain: vk::SwapchainKHR) {
        if !self.timing_active || self.outstanding == 0 {
            return;
        }
        let Some(f) = &self.v2 else { return };
        let cap = self.outstanding as usize;
        self.stages.resize(cap * 4, PresentStageTime::default());
        self.timings.clear();
        for i in 0..cap {
            self.timings.push(PastPresentationTiming {
                s_type: st(ST_PAST_PRESENTATION_TIMING),
                p_next: std::ptr::null_mut(),
                present_id: 0,
                target_time: 0,
                present_stage_count: 4,
                present_stages: unsafe { self.stages.as_mut_ptr().add(i * 4) },
                time_domain: 0,
                time_domain_id: 0,
                report_complete: vk::FALSE,
            });
        }
        let info = PastPresentationTimingInfo {
            s_type: st(ST_PAST_PRESENTATION_TIMING_INFO),
            p_next: std::ptr::null(),
            flags: 0,
            swapchain,
        };
        let mut props = PastPresentationTimingProperties {
            s_type: st(ST_PAST_PRESENTATION_TIMING_PROPERTIES),
            p_next: std::ptr::null_mut(),
            timing_properties_counter: 0,
            time_domains_counter: 0,
            presentation_timing_count: cap as u32,
            presentation_timings: self.timings.as_mut_ptr(),
        };
        let res = unsafe { (f.get_past_presentation_timing)(device, &info, &mut props) };
        if res != SUCCESS && res != INCOMPLETE {
            return;
        }
        let n = (props.presentation_timing_count as usize).min(cap);
        // without ALLOW_PARTIAL_RESULTS only complete results are returned (and dequeued)
        self.outstanding = self.outstanding.saturating_sub(n as u32);
        let mut results = Vec::with_capacity(n);
        for t in &self.timings[..n] {
            let stages = unsafe { std::slice::from_raw_parts(t.present_stages, t.present_stage_count.min(4) as usize) };
            let time = stages.iter().find(|s| s.stage == self.stage).map(|s| s.time).unwrap_or(0);
            results.push((t.present_id, time));
        }
        for (id, time) in results {
            if let Some(r) = self.pending.iter_mut().find(|r| r.present_id == id) {
                r.display_ns = time;
                if time == 0 {
                    r.flags |= REC_LOST;
                }
            }
        }
        // records are complete in present order once their time is known
        while self.pending.front().is_some_and(|r| r.display_ns != 0 || r.flags & REC_LOST != 0) {
            let r = self.pending.pop_front().unwrap();
            self.finish(r);
        }
        if props.timing_properties_counter != self.timing_counter {
            self.timing_counter = props.timing_properties_counter;
            self.update_refresh(device, swapchain);
        }
    }

    /// waits for present `id` (V2 path); returns false on timeout/error
    pub fn wait2(&self, device: vk::Device, swapchain: vk::SwapchainKHR, id: u64, timeout: u64) -> bool {
        let Some(f) = &self.v2 else { return false };
        let info = PresentWait2Info {
            s_type: st(ST_PRESENT_WAIT_2_INFO),
            p_next: std::ptr::null(),
            present_id: id,
            timeout,
        };
        unsafe { (f.wait_for_present2)(device, swapchain, &info) == SUCCESS }
    }

    /// swapchain replaced: outstanding records will never get a time
    pub fn flush(&mut self) {
        while let Some(mut r) = self.pending.pop_front() {
            if r.display_ns == 0 {
                r.flags |= REC_LOST;
            }
            self.finish(r);
        }
    }

    pub fn take(&mut self, out: &mut [Record]) -> usize {
        let n = out.len().min(self.done.len());
        for (o, r) in out.iter_mut().zip(self.done.drain(..n)) {
            *o = r;
        }
        n
    }
}
