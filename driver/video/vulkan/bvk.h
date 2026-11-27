#pragma once

// C API of the Rust Vulkan renderer (crates/vk/src/lib.rs), kept in sync by hand.

#include <cstddef>
#include <cstdint>

extern "C" {

struct BvkRenderer;

enum { BVK_SURFACE_WAYLAND = 1, BVK_SURFACE_XLIB = 2 };
enum { BVK_TEX_ATLAS = 0, BVK_TEX_TEXT = 1 };
enum { BVK_FMT_R8 = 0, BVK_FMT_RGBA8 = 1 };
enum { BVK_OK = 0, BVK_SKIPPED = 1, BVK_NO_SHOT = 2, BVK_ERROR = -1 };
enum { BVK_PRESENT_MAILBOX = 0, BVK_PRESENT_FIFO = 1, BVK_PRESENT_IMMEDIATE = 2 };
enum { BVK_FEEDBACK_NONE = 0, BVK_FEEDBACK_WAIT = 1, BVK_FEEDBACK_TIMING = 2 };
// BvkPresent::flags
enum { BVK_REC_REPEAT = 1, BVK_REC_REDRAW = 2, BVK_REC_TIMING = 4, BVK_REC_WAITED = 8, BVK_REC_LOST = 16, BVK_REC_BLEND = 32 };

struct BvkCreateInfo {
    int kind;
    void* display;
    void* surface;          // wl_surface* (Wayland)
    unsigned long window;   // X11 Window (xlib)
};

struct BvkFrame {
    unsigned window_width;
    unsigned window_height;

    const uint32_t* frame;  // BGRA in memory; null = keep the last uploaded picture
    unsigned frame_width;
    unsigned frame_height;
    unsigned frame_pitch;   // pixels
    int show_frame;
    int vp_x, vp_y, vp_width, vp_height; // window pixels, origin top left
    float mvp[16];
    int linear;

    const float* overlay_vertices; // 8 floats per vertex
    unsigned overlay_vertex_count;
    float overlay_width;
    float overlay_height;

    int text_visible;
    float text_quad[16];
    float text_color[4];
    float text_bg_color[4];

    uint8_t* screenshot;    // RGB, top-down
    int shot_x, shot_y, shot_width, shot_height;

    uint64_t frame_seq;     // changed value = new emulator picture (presented `repeat` times)
};

struct BvkPresent {
    uint64_t present_id;
    uint64_t frame;         // emulator frame sequence (0 = redraw)
    uint64_t submit_ns;     // CLOCK_MONOTONIC before vkQueuePresentKHR
    uint64_t display_ns;    // display time (0 = unknown)
    uint32_t flags;         // BVK_REC_*
    uint32_t copy;          // 0 = first present of the picture
    uint64_t target_ns;     // blend: predicted display time (vblank), 0 = none
};

struct BvkPacingCaps {
    int feedback;           // BVK_FEEDBACK_*
    int time_domain;        // Vulkan time domain of display_ns with timing (1 = CLOCK_MONOTONIC)
    uint64_t refresh_ns;    // from present timing, 0 = unknown
    int stalled;            // 1: a present wait timed out, nothing displayed since (hidden window)
    int absolute_time;      // 1: presents can carry absolute target times (grid pacing possible)
    int cadence;            // 1: grid presents are displayed at the grid cadence (VRR active)
    int blend;              // 1: grid without VRR, every vblank shows a phase-weighted mix (M-Pacing3)
};

BvkRenderer* bvk_create(const BvkCreateInfo* info, char* error, size_t errorLen);
void bvk_destroy(BvkRenderer* r);
int bvk_set_texture(BvkRenderer* r, int slot, int format, unsigned width, unsigned height, const void* data,
                    char* error, size_t errorLen);
int bvk_render(BvkRenderer* r, const BvkFrame* frame, char* error, size_t errorLen);
void bvk_set_present_mode(BvkRenderer* r, int mode);
void bvk_set_pacing(BvkRenderer* r, unsigned repeat, unsigned queue_depth, uint64_t period_ns, uint64_t refresh_ns);
void bvk_pacing_caps(const BvkRenderer* r, BvkPacingCaps* caps);
unsigned bvk_take_presents(BvkRenderer* r, BvkPresent* out, unsigned max);
const char* bvk_device_name(const BvkRenderer* r);

}
