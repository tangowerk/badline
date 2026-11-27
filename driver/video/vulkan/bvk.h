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
};

BvkRenderer* bvk_create(const BvkCreateInfo* info, char* error, size_t errorLen);
void bvk_destroy(BvkRenderer* r);
int bvk_set_texture(BvkRenderer* r, int slot, int format, unsigned width, unsigned height, const void* data,
                    char* error, size_t errorLen);
int bvk_render(BvkRenderer* r, const BvkFrame* frame, char* error, size_t errorLen);
void bvk_set_vsync(BvkRenderer* r, int on);
const char* bvk_device_name(const BvkRenderer* r);

}
