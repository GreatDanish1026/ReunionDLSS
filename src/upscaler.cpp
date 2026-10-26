// Replaces TSR's output with DLSS.
//
// TSR's intermediates are transient (their memory is reused within the frame), so the inputs are
// taken while they exist:
//   DilateVelocity (t0 depth, t1 engine velocity): our own motion vectors (motion.hlsl) are
//     computed there into our own texture. The engine's velocity only covers moving objects; the
//     camera's motion comes from the depth and View.ClipToPrevClip (found in the View uniform
//     buffer from TemporalAAJitter). DilateVelocity is recorded in a different command list, so
//     the result is found later through its output u0, which RejectShading reads (t8).
//   UpdateHistory (t0): the scene color with translucency -> copied into our own texture
//   ResolveHistory (u0): the 3840x2160 output, and from its parameters the render size, jitter and
//     motion vector scale (offsets 0, 256 and 272, docs/FINDINGS.md). The game's pass runs first
//     (it also writes a half-resolution copy the post-processing uses), then DLSS overwrites the
//     full output.
//
// NGX records its own compute state and descriptor heaps on the game's command list, and the
// engine assumes its state is unchanged afterwards. So every command list's compute and
// graphics bindings are tracked, and put back after DLSS through ReShade's API (which also
// knows the game's descriptor heaps and root signatures).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>

#include "common.hpp"

#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../third_party/reshade/utils/descriptor_tracking.hpp"
#include "cvars.hpp"
#include "dlss.hpp"
#include "motion_cs.h"  // kMotionCs: motion.hlsl compiled by build.ps1
#include "version.h"

using namespace reshade::api;

namespace rd {
namespace {

// ---------------------------------------------------------------- settings

constexpr int kToggleHotkey = VK_F9;

struct Settings {
    bool enabled = false;  // remembered across launches
    int preset = 11;       // NVSDK_NGX_DLSS_Hint_Render_Preset_K (transformer)
    int mode = 0;          // DlssMode: 0 leaves the render resolution to the game's Resolution Scale
} g_set;

void LoadSettings() {
    reshade::get_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Preset", g_set.preset);
    reshade::get_config_value(nullptr, RD_PRODUCT, "DlssMode", g_set.mode);
}

void SaveSettings() {
    reshade::set_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Preset", g_set.preset);
    reshade::set_config_value(nullptr, RD_PRODUCT, "DlssMode", g_set.mode);
}

// ---------------------------------------------------------------- per command list bindings

struct StageState {
    pipeline_layout layout = {0};
    std::vector<descriptor_table> tables;
    struct Root {
        shader_stage stages;
        descriptor_type type;
        buffer_range range;
    };
    std::map<uint32_t, Root> roots;  // root descriptors by parameter
    struct Constants {
        shader_stage stages;
        std::vector<uint32_t> values;
    };
    std::map<uint32_t, Constants> constants;  // root constants by parameter

    void Layout(pipeline_layout l) {
        if (l == layout) return;
        layout = l;  // a new root signature resets every binding
        tables.clear();
        roots.clear();
        constants.clear();
    }
};

// One TSR run's inputs, gathered in the command list that records RejectShading, UpdateHistory
// and ResolveHistory (in that order).
struct Chain {
    resource depth = {0}, motion = {0}, color = {0};
};

struct __declspec(uuid("3d0f6b1e-8c42-4a57-b1e9-5a7c0d2f9e61")) ListState {
    pipeline pipe = {0};
    pipeline_stage pipeStages = pipeline_stage::all;
    uint32_t computeId = 0;  // shader id of the bound pipeline (0 if graphics or unknown)
    StageState compute, graphics;
    Chain chain;
};

ListState* State(command_list* cl) { return cl->get_private_data<ListState>(); }

StageState* Stage(ListState* s, shader_stage stages) {
    if ((stages & shader_stage::all_compute) != 0) return &s->compute;
    if ((stages & shader_stage::all_graphics) != 0) return &s->graphics;
    return nullptr;
}

void OnInitList(command_list* cl) { cl->create_private_data<ListState>(); }
void OnDestroyList(command_list* cl) { cl->destroy_private_data<ListState>(); }

void OnResetList(command_list* cl) {
    if (ListState* s = State(cl)) *s = ListState();
}

void OnBindPipeline(command_list* cl, pipeline_stage stages, pipeline pipe) {
    ListState* s = State(cl);
    if (!s) return;
    s->pipe = pipe;
    s->pipeStages = stages;
    s->computeId = PipeId(pipe);
}

void OnBindTables(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count,
                  const descriptor_table* tables, uint32_t, const uint32_t*) {
    ListState* s = State(cl);
    if (!s) return;
    StageState* st = Stage(s, stages);
    if (!st) return;
    st->Layout(layout);
    if (st->tables.size() < first + count) st->tables.resize(first + count, descriptor_table{0});
    for (uint32_t i = 0; i < count; ++i) st->tables[first + i] = tables[i];
}

void OnPushDescriptors(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t param,
                       const descriptor_table_update& u) {
    ListState* s = State(cl);
    if (!s) return;
    StageState* st = Stage(s, stages);
    if (!st) return;
    st->Layout(layout);
    if (u.count != 1 || u.table.handle) return;
    if (u.type == descriptor_type::constant_buffer || u.type == descriptor_type::buffer_shader_resource_view ||
        u.type == descriptor_type::buffer_unordered_access_view)
        st->roots[param] = {stages, u.type, *static_cast<const buffer_range*>(u.descriptors)};
}

void OnPushConstants(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t param, uint32_t first,
                     uint32_t count, const void* values) {
    ListState* s = State(cl);
    if (!s) return;
    StageState* st = Stage(s, stages);
    if (!st) return;
    st->Layout(layout);
    StageState::Constants& c = st->constants[param];
    c.stages = stages;
    if (c.values.size() < first + count) c.values.resize(first + count);
    memcpy(c.values.data() + first, values, count * 4);
}

// Puts the game's bindings back after DLSS. A bind with no tables makes ReShade set the game's
// descriptor heaps and root signature again (NGX changed both behind its back).
void RestoreStage(command_list* cl, const StageState& st, shader_stage stages) {
    if (!st.layout.handle) return;
    cl->bind_descriptor_tables(stages, st.layout, 0, 0, nullptr);
    for (uint32_t i = 0; i < st.tables.size(); ++i)
        if (st.tables[i].handle) cl->bind_descriptor_tables(stages, st.layout, i, 1, &st.tables[i]);
    for (const auto& [param, r] : st.roots) {
        descriptor_table_update u = {};
        u.count = 1;
        u.type = r.type;
        u.descriptors = &r.range;
        cl->push_descriptors(r.stages, st.layout, param, u);
    }
    for (const auto& [param, c] : st.constants)
        cl->push_constants(c.stages, st.layout, param, 0, static_cast<uint32_t>(c.values.size()), c.values.data());
}

void Restore(command_list* cl, const ListState& s) {
    RestoreStage(cl, s.graphics, shader_stage::all_graphics);
    RestoreStage(cl, s.compute, shader_stage::all_compute);  // last: the game's next pass is compute or graphics
    if (s.pipe.handle) cl->bind_pipeline(s.pipeStages, s.pipe);
}

// ---------------------------------------------------------------- TSR inputs

// The resource at shader register <reg> of the bound table <param> (SRVs t#, UAVs u#).
resource Bound(command_list* cl, const StageState& st, uint32_t param, uint32_t reg) {
    if (param >= st.tables.size() || !st.tables[param].handle) return {0};
    device* dev = cl->get_device();
    descriptor_tracking* dt = dev->get_private_data<descriptor_tracking>();
    if (!dt) return {0};
    const pipeline_layout_param p = dt->get_pipeline_layout_param(st.layout, param);
    if (p.type != pipeline_layout_param_type::descriptor_table || !p.descriptor_table.ranges) return {0};
    for (uint32_t r = 0; r < p.descriptor_table.count; ++r) {
        const descriptor_range& range = p.descriptor_table.ranges[r];
        if (reg < range.dx_register_index || reg - range.dx_register_index >= range.count) continue;
        descriptor_heap heap = {0};
        uint32_t offset = 0;
        dev->get_descriptor_heap_offset(st.tables[param], range.binding + (reg - range.dx_register_index), 0, &heap, &offset);
        const resource_view v = dt->get_resource_view(heap, offset);
        return v.handle ? dev->get_resource_from_view(v) : resource{0};
    }
    return {0};
}

// <count> floats at byte <offset> of root constant buffer <param>.
bool ReadConstants(command_list* cl, const StageState& st, uint32_t param, uint32_t offset, float* out, uint32_t count) {
    auto it = st.roots.find(param);
    if (it == st.roots.end() || it->second.type != descriptor_type::constant_buffer) return false;
    device* dev = cl->get_device();
    const buffer_range& b = it->second.range;
    const resource_desc bd = dev->get_resource_desc(b.buffer);
    if (bd.type != resource_type::buffer || b.offset + offset + count * 4ull > bd.buffer.size) return false;
    void* data = nullptr;
    if (!dev->map_buffer_region(b.buffer, b.offset + offset, count * 4, map_access::read_only, &data) || !data) return false;
    memcpy(out, data, count * 4);
    dev->unmap_buffer_region(b.buffer);
    return true;
}

// The render size from TSR's parameters (root CBV 2, the same in every TSR pass): InputInfo is
// UE's FScreenPassTextureViewportParameters, with the texture extent at 0, ViewportMin (uint2)
// at 32 and ViewportSize at 48. The extent is rounded up (Balanced's 2227x1253 view sits in a
// larger texture), so the view's size is used. False if the view doesn't start at 0, 0.
bool RenderSize(command_list* cl, const StageState& st, float out[2]) {
    float extent[2] = {0, 0}, size[2] = {0, 0};
    uint32_t min[2] = {1, 1};
    if (!ReadConstants(cl, st, 2, 0, extent, 2) || !ReadConstants(cl, st, 2, 48, size, 2) ||
        !ReadConstants(cl, st, 2, 32, reinterpret_cast<float*>(min), 2))
        return false;
    if (min[0] || min[1] || !(size[0] >= 16 && size[1] >= 16 && size[0] <= extent[0] && size[1] <= extent[1])) {
        static int logged = 0;
        if (logged++ < 5)
            Log("TSR view %.0fx%.0f at %u,%u in %.0fx%.0f: not upscaled", size[0], size[1], min[0], min[1], extent[0], extent[1]);
        return false;
    }
    out[0] = size[0], out[1] = size[1];
    return true;
}

// Textures of our own that keep copies of transient TSR inputs until DLSS runs. Kept in the
// non-pixel shader resource state between uses. Never destroyed while the game runs (a texture
// may still be used by command lists the GPU hasn't run yet): one per size and format.
struct CopyPool {
    struct Entry {
        resource_desc desc;
        resource tex = {0};
    };
    std::vector<Entry> entries;

    resource Get(device* dev, const resource_desc& src) {
        for (const Entry& e : entries)
            if (e.desc.texture.width == src.texture.width && e.desc.texture.height == src.texture.height &&
                e.desc.texture.format == src.texture.format)
                return e.tex;
        if (entries.size() >= 8) return {0};
        Entry e;
        e.desc = resource_desc(src.texture.width, src.texture.height, 1, 1, src.texture.format, 1, memory_heap::default_,
                               resource_usage::shader_resource | resource_usage::copy_dest);
        if (!dev->create_resource(e.desc, nullptr, resource_usage::shader_resource_non_pixel, &e.tex)) {
            Log("couldn't create a %ux%u copy texture", src.texture.width, src.texture.height);
            return {0};
        }
        Log("created %ux%u copy texture (format %u)", src.texture.width, src.texture.height,
            static_cast<uint32_t>(src.texture.format));
        entries.push_back(e);
        return e.tex;
    }

    void Destroy(device* dev) {
        for (Entry& e : entries) dev->destroy_resource(e.tex);
        entries.clear();
    }
};

std::recursive_mutex g_mutex;  // destroying resources can call back into OnDestroyResource
CopyPool g_pool;

// Linear colour formats DLSS takes. The game's scene colour is rgba16f with HDR output and
// r11g11b10f with SDR output.
bool IsHdrColor(format f) { return f == format::r16g16b16a16_float || f == format::r11g11b10_float; }

// Copies <src> (in state <srcState>, format <want>) into a pool texture; {0} if the format isn't
// the expected one (menus and other views bind other textures in these slots). Wanting rgba16f
// also takes r11g11b10f.
resource CopyInput(command_list* cl, resource src, resource_usage srcState, format want, const char* what) {
    if (!src.handle) return {0};
    device* dev = cl->get_device();
    const resource_desc d = dev->get_resource_desc(src);
    const bool ok = d.texture.format == want || (want == format::r16g16b16a16_float && IsHdrColor(d.texture.format));
    if (d.type != resource_type::texture_2d || !ok) {
        static int logged = 0;
        if (logged < 20) {
            ++logged;
            Log("%s: unexpected %ux%u format %u: TSR kept for this view", what, d.texture.width, d.texture.height,
                static_cast<uint32_t>(d.texture.format));
        }
        return {0};
    }
    const resource tex = g_pool.Get(dev, d);
    if (!tex.handle) return {0};
    cl->barrier(src, srcState, resource_usage::copy_source);
    cl->barrier(tex, resource_usage::shader_resource_non_pixel, resource_usage::copy_dest);
    cl->copy_resource(src, tex);
    cl->barrier(src, resource_usage::copy_source, srcState);
    cl->barrier(tex, resource_usage::copy_dest, resource_usage::shader_resource_non_pixel);
    return tex;
}

// DilateVelocity writes a texture (u0) that RejectShading reads (t8), and is recorded in another
// command list, so its depth and our motion vectors are found through that texture.
struct Link {
    resource depth = {0}, motion = {0};
};
std::map<uint64_t, Link> g_links;

std::atomic<uint64_t> g_evaluated{0}, g_skipped{0};

// Counts a view left to TSR, and logs each distinct reason the first few times.
void Skip(const char* reason, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) {
    ++g_skipped;
    static std::map<std::string, int> logged;
    int& n = logged[reason];
    if (n++ < 3) Log("TSR kept for this view: %s (%u %u %u)", reason, a, b, c);
}
std::string g_error;
std::map<uint64_t, bool> g_resetByOutput;  // per view
// Render size per view: a new size (DLSS mode or menu change) starts DLSS and frame generation
// over, instead of blending in the previous size's frame (a one-frame streak at the bottom edge).
std::map<uint64_t, uint64_t> g_sizeByOutput;
bool g_resetAll = true;
float g_lastJitter[2] = {0, 0};
unsigned g_lastRender[2] = {0, 0}, g_lastOutput[2] = {0, 0};  // sizes of the last view DLSS upscaled

constexpr uint32_t kViewScanFloats = 4096;  // the first 16 KB of the View uniform buffer

struct MotionState {
    pipeline_layout layout = {0};
    pipeline pipe = {0};
    bool failed = false;
    int offset = -1;  // byte offset of View.ClipToPrevClip, -1 until found
    int candidate = -1, hits = 0, misses = 0;
    std::map<std::pair<uint64_t, int>, resource_view> views;  // by resource and kind (0 SRV, 1 UAV)
    struct Texture {
        uint32_t width = 0, height = 0;
        resource tex = {0};
    };
    std::vector<Texture> textures;  // rg16f, one per size, kept until shutdown
} g_motion;

// ---------------------------------------------------------------- motion vectors

bool Near(float a, float b) { return std::fabs(a - b) <= b * 2e-3f + 1e-9f; }

// UE's View uniform buffer has ClipToPrevClip, ClipToPrevClipWithAA, TemporalAAJitter in that
// order. TemporalAAJitter is (this frame's jitter, last frame's) in clip units (2 / size, either
// sign). The jittered projection matrices hold this frame's jitter too, but followed by 0 and 1.
// ClipToPrevClip maps a clip position to almost the same one (the camera moves little per frame),
// so its diagonal is near 1. A match at the same place several frames in a row is it.
bool LooksLikeClipToPrevClip(const float* m) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    return std::fabs(m[0] - 1) < 0.2f && std::fabs(m[5] - 1) < 0.2f;
}

// TemporalAAJitter (this frame's jitter, last frame's, in clip units): small, and this frame's
// x:y proportion matches TSR's jitter in pixels (scaled by the aspect ratio). Matching the
// proportion rather than the values keeps it independent of how UE scales the view (the DLSS
// modes change the render size, and an exact match on the size failed there).
bool LooksLikeJitterRow(const float* r, const float jitter[2], const float size[2]) {
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(r[i]) || std::fabs(r[i]) >= 0.01f) return false;
    if (r[0] == 0 || r[1] == 0 || (r[2] == 0 && r[3] == 0)) return false;
    if (std::fabs(jitter[0]) < 0.02f || std::fabs(jitter[1]) < 0.02f) return true;  // can't compare: accept
    const float want = std::fabs(jitter[0] / jitter[1]) * (size[1] / size[0]);
    return std::fabs(std::fabs(r[0] / r[1]) / want - 1) < 0.05f;
}

constexpr int kKnownClipToPrevClip = 2176;  // this game build (docs/FINDINGS.md)

void FindClipToPrevClip(const float* v, const float jitter[2], const float size[2]) {
    const auto valid = [&](int byteOffset) {
        const float* m = v + byteOffset / 4;
        return LooksLikeClipToPrevClip(m) && LooksLikeJitterRow(m + 32, jitter, size);
    };
    int found = -1;
    if (valid(kKnownClipToPrevClip)) {
        found = kKnownClipToPrevClip;
    } else {
        for (uint32_t i = 32; i + 4 <= kViewScanFloats; i += 4)
            if (valid(static_cast<int>(i - 32) * 4)) {
                found = static_cast<int>(i - 32) * 4;
                break;
            }
    }
    if (found < 0) {
        g_motion.candidate = -1, g_motion.hits = 0;
        if (++g_motion.misses == 600)
            Log("motion: the camera matrix wasn't found in the View uniform buffer (600 tries): DLSS can't run "
                "(render size %.0fx%.0f, jitter %.3f %.3f)", size[0], size[1], jitter[0], jitter[1]);
        return;
    }
    if (found == g_motion.candidate) ++g_motion.hits;
    else g_motion.candidate = found, g_motion.hits = 1;
    if (g_motion.hits < 4) return;
    g_motion.offset = found;
    Log("View.ClipToPrevClip at byte %d of the View uniform buffer", found);
}

bool CreateMotionPipeline(device* dev) {
    if (g_motion.pipe.handle) return true;
    if (g_motion.failed) return false;
    constant_range c = {};
    c.count = 20;
    c.visibility = shader_stage::all_compute;
    descriptor_range srv = {};
    srv.count = 2;
    srv.visibility = shader_stage::all_compute;
    srv.type = descriptor_type::shader_resource_view;
    descriptor_range uav = {};
    uav.count = 1;
    uav.visibility = shader_stage::all_compute;
    uav.type = descriptor_type::unordered_access_view;
    const pipeline_layout_param params[3] = {pipeline_layout_param(c), pipeline_layout_param(srv), pipeline_layout_param(uav)};
    shader_desc cs = {};
    cs.code = kMotionCs;
    cs.code_size = sizeof(kMotionCs);
    pipeline_subobject sub = {pipeline_subobject_type::compute_shader, 1, &cs};
    if (!dev->create_pipeline_layout(3, params, &g_motion.layout) ||
        !dev->create_pipeline(g_motion.layout, 1, &sub, &g_motion.pipe)) {
        g_motion.failed = true;
        g_error = "couldn't create the motion vector shader";
        Log("%s", g_error.c_str());
        return false;
    }
    Log("motion vector shader created");
    return true;
}

resource MotionTexture(device* dev, uint32_t w, uint32_t h) {
    for (const MotionState::Texture& t : g_motion.textures)
        if (t.width == w && t.height == h) return t.tex;
    if (g_motion.textures.size() >= 8) return {0};
    MotionState::Texture t;
    t.width = w, t.height = h;
    const resource_desc d(w, h, 1, 1, format::r16g16_float, 1, memory_heap::default_,
                          resource_usage::shader_resource | resource_usage::unordered_access);
    if (!dev->create_resource(d, nullptr, resource_usage::shader_resource_non_pixel, &t.tex)) {
        Log("couldn't create a %ux%u motion vector texture", w, h);
        return {0};
    }
    Log("created %ux%u motion vector texture", w, h);
    g_motion.textures.push_back(t);
    return t.tex;
}

resource_view View(device* dev, resource r, format f, bool uav) {
    const auto key = std::make_pair(r.handle, uav ? 1 : 0);
    auto it = g_motion.views.find(key);
    if (it != g_motion.views.end()) return it->second;
    resource_view v = {0};
    if (!dev->create_resource_view(r, uav ? resource_usage::unordered_access : resource_usage::shader_resource,
                                   resource_view_desc(f), &v))
        return {0};
    g_motion.views[key] = v;
    return v;
}

void LogOnce(int& count, const char* fmt, uint32_t a, uint32_t b) {
    if (count >= 10) return;
    ++count;
    Log(fmt, a, b);
}

// Records motion.hlsl before the game's DilateVelocity (same inputs) and puts its state back.
resource ComputeMotion(command_list* cl, ListState& s, resource depth, resource velocity) {
    static int badFormat = 0, badParams = 0;
    device* dev = cl->get_device();
    if (!velocity.handle) return {0};
    const resource_desc dd = dev->get_resource_desc(depth), vd = dev->get_resource_desc(velocity);
    if (dd.texture.format != format::r32_g8_typeless || vd.texture.format != format::r16g16b16a16_unorm) {
        LogOnce(badFormat, "motion: depth format %u, velocity format %u: TSR kept for this view",
                static_cast<uint32_t>(dd.texture.format), static_cast<uint32_t>(vd.texture.format));
        return {0};
    }
    float size[2] = {0, 0}, jitter[2] = {0, 0};
    if (!RenderSize(cl, s.compute, size) || !ReadConstants(cl, s.compute, 2, 256, jitter, 2) ||
        size[0] < 16 || size[1] < 16 || size[0] > dd.texture.width || size[1] > dd.texture.height ||
        std::fabs(jitter[0]) > 0.5f || std::fabs(jitter[1]) > 0.5f) {
        LogOnce(badParams, "motion: DilateVelocity parameters not usable (size %u x %u)", static_cast<uint32_t>(size[0]),
                static_cast<uint32_t>(size[1]));
        return {0};
    }
    static std::vector<float> view(kViewScanFloats);
    if (!ReadConstants(cl, s.compute, 3, 0, view.data(), kViewScanFloats)) return {0};
    if (g_motion.offset < 0) {
        FindClipToPrevClip(view.data(), jitter, size);
        if (g_motion.offset < 0) return {0};
    }
    if (!CreateMotionPipeline(dev)) return {0};
    const resource tex = MotionTexture(dev, dd.texture.width, dd.texture.height);
    if (!tex.handle) return {0};
    const resource_view srvs[2] = {View(dev, depth, format::r32_float_x8_uint, false),
                                   View(dev, velocity, format::r16g16b16a16_unorm, false)};
    const resource_view uav = View(dev, tex, format::r16g16_float, true);
    if (!srvs[0].handle || !srvs[1].handle || !uav.handle) return {0};

    float params[20];
    const float* m = view.data() + g_motion.offset / 4;
    memcpy(params, m, 16 * sizeof(float));
    FrameGenSetCamera(view.data(), g_motion.offset);
    params[16] = size[0], params[17] = size[1], params[18] = 1 / size[0], params[19] = 1 / size[1];

    cl->barrier(tex, resource_usage::shader_resource_non_pixel, resource_usage::unordered_access);
    cl->bind_pipeline(pipeline_stage::all_compute, g_motion.pipe);
    cl->push_constants(shader_stage::all_compute, g_motion.layout, 0, 0, 20, params);
    descriptor_table_update u = {};
    u.count = 2;
    u.type = descriptor_type::shader_resource_view;
    u.descriptors = srvs;
    cl->push_descriptors(shader_stage::all_compute, g_motion.layout, 1, u);
    u.count = 1;
    u.type = descriptor_type::unordered_access_view;
    u.descriptors = &uav;
    cl->push_descriptors(shader_stage::all_compute, g_motion.layout, 2, u);
    cl->dispatch((static_cast<uint32_t>(size[0]) + 7) / 8, (static_cast<uint32_t>(size[1]) + 7) / 8, 1);
    cl->barrier(tex, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel);
    Restore(cl, s);
    return tex;
}

void OnDestroyResource(device* dev, resource r) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (int kind = 0; kind < 2; ++kind) {
        auto it = g_motion.views.find(std::make_pair(r.handle, kind));
        if (it == g_motion.views.end()) continue;
        dev->destroy_resource_view(it->second);
        g_motion.views.erase(it);
    }
}

void OnDilateVelocity(command_list* cl, ListState& s) {
    const resource depth = Bound(cl, s.compute, 0, 0);
    const resource velocity = Bound(cl, s.compute, 0, 1);
    const resource out = Bound(cl, s.compute, 1, 0);
    if (!depth.handle || !out.handle) return;
    if (g_links.size() > 64) g_links.clear();
    Link& link = g_links[out.handle];
    link.depth = depth;
    link.motion = ComputeMotion(cl, s, depth, velocity);
}

void OnRejectShading(command_list* cl, ListState& s) {
    s.chain = Chain();
    const resource dilated = Bound(cl, s.compute, 0, 8);
    auto it = g_links.find(dilated.handle);
    if (it == g_links.end()) return;
    s.chain.depth = it->second.depth;
    s.chain.motion = it->second.motion;  // none: TSR kept for this view
}

void OnUpdateHistory(command_list* cl, ListState& s) {
    if (s.chain.motion.handle)
        s.chain.color = CopyInput(cl, Bound(cl, s.compute, 0, 0), resource_usage::shader_resource_non_pixel,
                                  format::r16g16b16a16_float, "color");
}

// ---------------------------------------------------------------- DLSS

bool Evaluate(command_list* cl, ListState& s, uint32_t x, uint32_t y, uint32_t z) {
    device* dev = cl->get_device();
    const Chain chain = s.chain;
    s.chain = Chain();  // used once
    const resource output = Bound(cl, s.compute, 1, 0);
    if (!output.handle || !chain.depth.handle || !chain.motion.handle || !chain.color.handle) {
        Skip("an input is missing (output, depth, motion, colour)", output.handle != 0, chain.depth.handle != 0,
             (chain.motion.handle != 0) * 10 + (chain.color.handle != 0));
        return false;
    }
    const resource_desc out = dev->get_resource_desc(output);
    if (!IsHdrColor(out.texture.format)) {
        Skip("TSR's output has an unexpected format (width, height, format)", out.texture.width, out.texture.height,
             static_cast<uint32_t>(out.texture.format));
        return false;
    }
    float common[2] = {0, 0}, jitter[2] = {0, 0};
    if (!RenderSize(cl, s.compute, common) || !ReadConstants(cl, s.compute, 2, 256, jitter, 2)) {
        g_error = "TSR parameters not readable";
        Skip("TSR's parameters aren't readable");
        return false;
    }
    const resource_desc cd = dev->get_resource_desc(chain.color);
    const unsigned rw = static_cast<unsigned>(common[0]), rh = static_cast<unsigned>(common[1]);
    if (!rw || !rh || rw > cd.texture.width || rh > cd.texture.height || std::fabs(jitter[0]) > 0.5f ||
        std::fabs(jitter[1]) > 0.5f) {
        g_error = "TSR parameters out of range";
        Skip("TSR's parameters are out of range (render width, height, colour width)", rw, rh, cd.texture.width);
        return false;
    }
    std::string err;
    if (!dlss::Init(reinterpret_cast<ID3D12Device*>(dev->get_native()), err)) {
        g_error = err;
        ++g_skipped;
        return false;
    }
    g_lastJitter[0] = jitter[0], g_lastJitter[1] = jitter[1];
    if (g_resetAll) g_resetByOutput.clear(), g_resetAll = false;
    auto rs = g_resetByOutput.find(output.handle);
    const uint64_t size = (static_cast<uint64_t>(rw) << 32) | rh;
    auto ss = g_sizeByOutput.find(output.handle);
    const bool resized = ss != g_sizeByOutput.end() && ss->second != size;
    g_sizeByOutput[output.handle] = size;
    if (g_sizeByOutput.size() > 64) g_sizeByOutput.clear();
    const bool reset = rs == g_resetByOutput.end() || rs->second || resized;

    cl->dispatch(x, y, z);  // the game's ResolveHistory, with its own bindings
    cl->barrier(output, resource_usage::unordered_access, resource_usage::unordered_access);

    dlss::Inputs in;
    in.color = reinterpret_cast<ID3D12Resource*>(chain.color.handle);
    in.motion = reinterpret_cast<ID3D12Resource*>(chain.motion.handle);
    in.depth = reinterpret_cast<ID3D12Resource*>(chain.depth.handle);
    in.output = reinterpret_cast<ID3D12Resource*>(output.handle);
    in.renderWidth = rw, in.renderHeight = rh;
    g_lastRender[0] = rw, g_lastRender[1] = rh;
    g_lastOutput[0] = out.texture.width, g_lastOutput[1] = out.texture.height;
    in.outputWidth = out.texture.width, in.outputHeight = out.texture.height;
    in.jitterX = jitter[0], in.jitterY = jitter[1];
    in.motionScaleX = 1, in.motionScaleY = 1;  // motion.hlsl writes previous minus current in pixels
    in.reset = reset;
    in.preset = g_set.preset;
    const bool ok = dlss::Evaluate(reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native()), in, err);
    cl->barrier(output, resource_usage::unordered_access, resource_usage::unordered_access);
    Restore(cl, s);
    if (ok) {
        FrameGenTag(reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native()), in.depth, in.motion, rw, rh, in.jitterX,
                    in.jitterY, reset);
        g_resetByOutput[output.handle] = false;
        g_error.clear();
        if (++g_evaluated == 1) Log("DLSS: first frame evaluated");
    } else {
        ++g_skipped;
        if (g_error != err) {
            g_error = err;
            Log("DLSS: %s", err.c_str());
        }
    }
    return true;  // the game's dispatch was recorded above
}

bool OnDispatch(command_list* cl, uint32_t x, uint32_t y, uint32_t z) {
    if (!g_set.enabled) return false;
    ListState* s = State(cl);
    if (!s) return false;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    switch (s->computeId) {
        case kTsrDilateVelocity:
            OnDilateVelocity(cl, *s);
            return false;
        case kTsrRejectShading:
            OnRejectShading(cl, *s);
            return false;
        case kTsrUpdateHistory:
            OnUpdateHistory(cl, *s);
            return false;
        case kTsrResolveHistory:
            return Evaluate(cl, *s, x, y, z);
        default:
            return false;
    }
}

void OnDestroyDevice(device* dev) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    dlss::Shutdown();
    g_pool.Destroy(dev);
    for (auto& [key, v] : g_motion.views) dev->destroy_resource_view(v);
    g_motion.views.clear();
    for (MotionState::Texture& t : g_motion.textures) dev->destroy_resource(t.tex);
    if (g_motion.pipe.handle) dev->destroy_pipeline(g_motion.pipe);
    if (g_motion.layout.handle) dev->destroy_pipeline_layout(g_motion.layout);
    g_motion = MotionState();
    g_links.clear();
}

bool g_keyWasDown = false;

// ---------------------------------------------------------------- render resolution (DLSS mode)
//
// DLSS renders at the game's r.ScreenPercentage (the menu's Resolution Scale). A DLSS mode sets
// it to NVIDIA's ratio for that mode while DLSS is on, and puts the game's value back when DLSS is
// off (so TSR gets the menu's). The game sets it again when its menu is applied: that value is
// remembered as the game's and the mode's put back.

enum DlssMode { kModeGame, kModeDlaa, kModeQuality, kModeBalanced, kModePerformance, kModeUltraPerformance, kDlssModeCount };
const char* const kDlssModeNames[kDlssModeCount] = {"Game's Resolution Scale", "DLAA (100%)", "Quality (67%)",
                                                    "Balanced (58%)", "Performance (50%)", "Ultra Performance (33%)"};
const float kDlssModePercent[kDlssModeCount] = {0, 100, 66.667f, 58, 50, 33.333f};

struct Resolution {
    void* manager = nullptr;
    void* var = nullptr;  // r.ScreenPercentage
    int sinkFlag = -1;
    bool failed = false;
    bool overriding = false;
    float gameValue = 0, ourValue = 0;
    uint64_t presents = 0;
    std::string error;
} g_res;

bool FindScreenPercentage() {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    void** singleton = cvars::FindConsoleManagerSingleton(base);
    void* manager = nullptr;
    if (singleton) cvars::SafeCopy(&manager, singleton, 8);
    if (!manager || !cvars::HasModuleVtable(base, manager)) {
        g_res.error = "the game's console variables weren't found";
        return false;
    }
    void* aniso = cvars::FindVariable(manager, L"r.MaxAnisotropy");
    void* screen = cvars::FindVariable(manager, L"r.ScreenPercentage");
    float value = -1;
    if (!aniso || !screen || !cvars::HasModuleVtable(base, screen) || !cvars::FindTypeSlots(aniso, screen) ||
        !cvars::GetFloat(screen, value) || !(value >= 10 && value <= 400)) {
        g_res.error = "r.ScreenPercentage didn't look right (see the log)";
        Log("DLSS mode: r.ScreenPercentage %p reads %g: not used", screen, value);
        return false;
    }
    g_res.manager = manager, g_res.var = screen;
    g_res.sinkFlag = cvars::FindSinkFlagOffset(manager);
    Log("DLSS mode: r.ScreenPercentage found (now %g)", value);
    return true;
}

bool SetScreenPercentage(float v) {
    wchar_t text[32];
    swprintf(text, 32, L"%.3f", v);
    if (!cvars::Set(g_res.var, text)) return false;
    if (g_res.sinkFlag >= 0) cvars::RequestSinks(g_res.manager, g_res.sinkFlag);
    g_res.ourValue = v;
    return true;
}

// About twice a second, on ReShade's present.
void UpdateResolution() {
    if (++g_res.presents % 30 != 0) return;
    const bool want = g_set.enabled && g_set.mode > kModeGame && g_set.mode < kDlssModeCount;
    if (!g_res.var) {
        if (!want || g_res.failed) return;
        if (!FindScreenPercentage()) {
            g_res.failed = true;
            return;
        }
    }
    float now = 0;
    if (!cvars::GetFloat(g_res.var, now)) return;
    if (want) {
        const float target = kDlssModePercent[g_set.mode];
        if (!g_res.overriding) {
            g_res.gameValue = now, g_res.overriding = true;
        } else if (std::fabs(now - g_res.ourValue) > 0.01f) {
            g_res.gameValue = now;  // the game's menu set it again
            Log("DLSS mode: the game set its resolution scale to %g%%", now);
        }
        if (std::fabs(now - target) > 0.01f && SetScreenPercentage(target))
            Log("DLSS mode %s: render resolution %g%% (the game's: %g%%)", kDlssModeNames[g_set.mode], target, g_res.gameValue);
    } else if (g_res.overriding) {
        g_res.overriding = false;
        if (std::fabs(now - g_res.ourValue) < 0.01f && SetScreenPercentage(g_res.gameValue))
            Log("DLSS mode: back to the game's resolution scale (%g%%)", g_res.gameValue);
    }
}

void OnPresent(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
    UpdateResolution();
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    dlss::EndFrame();
    if (down && !g_keyWasDown) {
        g_set.enabled = !g_set.enabled;
        g_resetAll = true;
        SaveSettings();
        Log("DLSS %s (F9)", g_set.enabled ? "on" : "off");
    }
    g_keyWasDown = down;
}

}  // namespace

void RestoreListState(command_list* cl) {
    if (ListState* s = State(cl)) Restore(cl, *s);
}

void RegisterUpscaler() {
    LoadSettings();
    reshade::register_event<reshade::addon_event::init_command_list>(&OnInitList);
    reshade::register_event<reshade::addon_event::destroy_command_list>(&OnDestroyList);
    reshade::register_event<reshade::addon_event::reset_command_list>(&OnResetList);
    reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(&OnBindTables);
    reshade::register_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
    reshade::register_event<reshade::addon_event::push_constants>(&OnPushConstants);
    reshade::register_event<reshade::addon_event::dispatch>(&OnDispatch);
    reshade::register_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
    reshade::register_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
    reshade::register_event<reshade::addon_event::present>(&OnPresent);
}

void UnregisterUpscaler() {
    reshade::unregister_event<reshade::addon_event::init_command_list>(&OnInitList);
    reshade::unregister_event<reshade::addon_event::destroy_command_list>(&OnDestroyList);
    reshade::unregister_event<reshade::addon_event::reset_command_list>(&OnResetList);
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::unregister_event<reshade::addon_event::bind_descriptor_tables>(&OnBindTables);
    reshade::unregister_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
    reshade::unregister_event<reshade::addon_event::push_constants>(&OnPushConstants);
    reshade::unregister_event<reshade::addon_event::dispatch>(&OnDispatch);
    reshade::unregister_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
    reshade::unregister_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
    reshade::unregister_event<reshade::addon_event::present>(&OnPresent);
}

void SaveAndLog() {
    g_resetAll = true;
    SaveSettings();
    Log("settings: DLSS %s, preset %d, mode %s", g_set.enabled ? "on" : "off", g_set.preset,
        kDlssModeNames[g_set.mode >= 0 && g_set.mode < kDlssModeCount ? g_set.mode : 0]);
}

void DrawUpscalerSettings() {
    ImGui::SeparatorText("DLSS");
    if (ImGui::Checkbox("DLSS", &g_set.enabled)) SaveAndLog();
    Help("NVIDIA DLSS in place of the game's TSR upscaler. F9 switches it on and off while playing.");
    ImGui::SameLine();
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_set.enabled)
            ImGui::TextDisabled("off: the game's TSR");
        else if (!g_error.empty())
            ImGui::TextColored(kWarn, "%s", g_error.c_str());
        else if (g_lastRender[0])
            ImGui::TextDisabled("%ux%u to %ux%u", g_lastRender[0], g_lastRender[1], g_lastOutput[0], g_lastOutput[1]);
        else
            ImGui::TextDisabled("starts with the next game frame");
    }

    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::Combo("Mode", &g_set.mode, kDlssModeNames, kDlssModeCount)) SaveAndLog();
    Help("The resolution DLSS renders at, as a share of the screen's. \"Game's Resolution Scale\" uses the game's "
         "menu setting; the others override it while DLSS is on.\n\nKeep the game's Secondary Resolution Scale at "
         "100%, so DLSS outputs the full screen resolution.");
    if (g_res.failed) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "%s", g_res.error.c_str());
    }

    const char* presets[] = {"NVIDIA default", "J", "K (recommended)", "L", "M"};
    const int ids[] = {0, 10, 11, 12, 13};
    int cur = 2;
    for (int i = 0; i < 5; ++i)
        if (ids[i] == g_set.preset) cur = i;
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::Combo("Preset", &cur, presets, 5)) g_set.preset = ids[cur], SaveAndLog();
    Help("DLSS's model. K is NVIDIA's transformer model, sharpest in motion. A driver override in the NVIDIA App "
         "takes precedence over this.");
}

void DrawUpscalerStatus() {
    ImGui::SeparatorText("DLSS");
    ImGui::TextDisabled("%s", g_set.enabled ? dlss::Status().c_str() : "off");
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        ImGui::TextDisabled("Views upscaled %llu, left to TSR %llu (menus, other views)",
                            static_cast<unsigned long long>(g_evaluated.load()),
                            static_cast<unsigned long long>(g_skipped.load()));
        if (g_res.var)
            ImGui::TextDisabled("Resolution scale: %s", g_res.overriding ? "set by the DLSS mode" : "the game's");
    }
    if (ImGui::Button("Reset DLSS history")) g_resetAll = true;
}

}  // namespace rd
