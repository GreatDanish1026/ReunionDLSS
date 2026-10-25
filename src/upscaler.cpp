// Replaces TSR's output with DLSS.
//
// TSR's intermediates are transient (their memory is reused within the frame), so the inputs are
// taken while they exist:
//   DilateVelocity (t0): the scene depth (DilateVelocity is recorded in a different command list,
//     possibly in parallel, so only the depth resource is taken from it: it lives across frames)
//   RejectShading (t8): TSR's dilated motion vectors -> copied into our own texture
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
#include "dlss.hpp"
#include "version.h"

using namespace reshade::api;

namespace rd {
namespace {

// ---------------------------------------------------------------- settings

constexpr int kToggleHotkey = VK_F9;

struct Settings {
    bool enabled = false;  // off until switched on in the overlay (or F9)
    int preset = 11;       // NVSDK_NGX_DLSS_Hint_Render_Preset_K (transformer)
    bool flipMotionX = false, flipMotionY = false;
    bool flipJitterX = false, flipJitterY = false;
} g_set;

void LoadSettings() {
    reshade::get_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Preset", g_set.preset);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipMotionX", g_set.flipMotionX);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipMotionY", g_set.flipMotionY);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipJitterX", g_set.flipJitterX);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipJitterY", g_set.flipJitterY);
}

void SaveSettings() {
    reshade::set_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Preset", g_set.preset);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipMotionX", g_set.flipMotionX);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipMotionY", g_set.flipMotionY);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitterX", g_set.flipJitterX);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitterY", g_set.flipJitterY);
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

struct __declspec(uuid("3d0f6b1e-8c42-4a57-b1e9-5a7c0d2f9e61")) ListState {
    pipeline pipe = {0};
    pipeline_stage pipeStages = pipeline_stage::all;
    uint32_t computeId = 0;  // shader id of the bound pipeline (0 if graphics or unknown)
    StageState compute, graphics;
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
    void* data = nullptr;
    if (!dev->map_buffer_region(b.buffer, b.offset + offset, count * 4, map_access::read_only, &data) || !data) return false;
    memcpy(out, data, count * 4);
    dev->unmap_buffer_region(b.buffer);
    return true;
}

// A texture of our own that keeps a copy of a transient TSR input until DLSS runs. Kept in the
// non-pixel shader resource state between uses.
struct Copy {
    resource tex = {0};
    resource_desc desc;

    bool Ensure(device* dev, const resource_desc& src) {
        if (tex.handle && desc.texture.width == src.texture.width && desc.texture.height == src.texture.height &&
            desc.texture.format == src.texture.format)
            return true;
        if (tex.handle) dev->destroy_resource(tex);
        tex = {0};
        desc = resource_desc(src.texture.width, src.texture.height, 1, 1, src.texture.format, 1, memory_heap::default_,
                             resource_usage::shader_resource | resource_usage::copy_dest);
        if (!dev->create_resource(desc, nullptr, resource_usage::shader_resource_non_pixel, &tex)) {
            tex = {0};
            Log("couldn't create a %ux%u copy texture", src.texture.width, src.texture.height);
            return false;
        }
        Log("created %ux%u copy texture (format %u)", src.texture.width, src.texture.height,
            static_cast<uint32_t>(src.texture.format));
        return true;
    }

    // Copies <src> (in state <srcState>) into the texture.
    bool From(command_list* cl, resource src, resource_usage srcState) {
        device* dev = cl->get_device();
        if (!src.handle || !Ensure(dev, dev->get_resource_desc(src))) return false;
        cl->barrier(src, srcState, resource_usage::copy_source);
        cl->barrier(tex, resource_usage::shader_resource_non_pixel, resource_usage::copy_dest);
        cl->copy_resource(src, tex);
        cl->barrier(src, resource_usage::copy_source, srcState);
        cl->barrier(tex, resource_usage::copy_dest, resource_usage::shader_resource_non_pixel);
        return true;
    }

    void Destroy(device* dev) {
        if (tex.handle) dev->destroy_resource(tex);
        tex = {0};
    }
};

std::mutex g_mutex;
Copy g_motion, g_color;

struct Frame {
    resource depth = {0};
    float jitter[2] = {0, 0};
    float motionScale[2] = {0, 0};  // TSR's ScreenVelocityToInputPixelVelocity
    float renderSize[2] = {0, 0};
    bool haveDepth = false, haveMotion = false, haveColor = false;
} g_frame;

std::atomic<uint64_t> g_evaluated{0};
std::string g_error;
bool g_reset = true;
unsigned g_lastRenderW = 0, g_lastRenderH = 0;

void OnDilateVelocity(command_list* cl, ListState& s) {
    const resource depth = Bound(cl, s.compute, 0, 0);
    if (!depth.handle) return;
    if (depth != g_frame.depth) Log("scene depth: %p", reinterpret_cast<void*>(depth.handle));
    g_frame.depth = depth;
    g_frame.haveDepth = true;
}

// ---------------------------------------------------------------- DLSS

bool Evaluate(command_list* cl, ListState& s, uint32_t x, uint32_t y, uint32_t z) {
    device* dev = cl->get_device();
    const resource output = Bound(cl, s.compute, 1, 0);
    const bool inputs = output.handle && g_frame.haveDepth && g_frame.haveMotion && g_frame.haveColor;
    g_frame.haveMotion = g_frame.haveColor = false;  // copied again next frame
    if (!inputs) {
        g_error = "TSR inputs not found this frame";
        return false;
    }
    float common[2] = {0, 0};
    if (!ReadConstants(cl, s.compute, 2, 0, common, 2) || !ReadConstants(cl, s.compute, 2, 256, g_frame.jitter, 2) ||
        !ReadConstants(cl, s.compute, 2, 272, g_frame.motionScale, 2)) {
        g_error = "TSR parameters not readable";
        return false;
    }
    g_frame.renderSize[0] = common[0], g_frame.renderSize[1] = common[1];
    std::string err;
    if (!dlss::Init(reinterpret_cast<ID3D12Device*>(dev->get_native()), err)) {
        g_error = err;
        return false;
    }
    const resource_desc out = dev->get_resource_desc(output);
    const unsigned rw = static_cast<unsigned>(g_frame.renderSize[0]), rh = static_cast<unsigned>(g_frame.renderSize[1]);
    if (!rw || !rh || rw > g_color.desc.texture.width || rh > g_color.desc.texture.height) {
        g_error = "render size not found";
        return false;
    }
    if (rw != g_lastRenderW || rh != g_lastRenderH) g_reset = true, g_lastRenderW = rw, g_lastRenderH = rh;

    cl->dispatch(x, y, z);  // the game's ResolveHistory, with its own bindings
    cl->barrier(output, resource_usage::unordered_access, resource_usage::unordered_access);

    dlss::Inputs in;
    in.color = reinterpret_cast<ID3D12Resource*>(g_color.tex.handle);
    in.motion = reinterpret_cast<ID3D12Resource*>(g_motion.tex.handle);
    in.depth = reinterpret_cast<ID3D12Resource*>(g_frame.depth.handle);
    in.output = reinterpret_cast<ID3D12Resource*>(output.handle);
    in.renderWidth = rw, in.renderHeight = rh;
    in.outputWidth = out.texture.width, in.outputHeight = out.texture.height;
    in.jitterX = g_frame.jitter[0] * (g_set.flipJitterX ? -1.0f : 1.0f);
    in.jitterY = g_frame.jitter[1] * (g_set.flipJitterY ? -1.0f : 1.0f);
    // TSR's velocity is current minus previous; DLSS wants previous minus current, in pixels.
    in.motionScaleX = -g_frame.motionScale[0] * (g_set.flipMotionX ? -1.0f : 1.0f);
    in.motionScaleY = -g_frame.motionScale[1] * (g_set.flipMotionY ? -1.0f : 1.0f);
    in.reset = g_reset;
    in.preset = g_set.preset;
    const bool ok = dlss::Evaluate(reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native()), in, err);
    cl->barrier(output, resource_usage::unordered_access, resource_usage::unordered_access);
    Restore(cl, s);
    if (ok) {
        g_reset = false;
        g_error.clear();
        if (++g_evaluated == 1) Log("DLSS: first frame evaluated");
    } else if (g_error != err) {
        g_error = err;
        Log("DLSS: %s", err.c_str());
    }
    return true;  // the game's dispatch was recorded above
}

bool OnDispatch(command_list* cl, uint32_t x, uint32_t y, uint32_t z) {
    if (!g_set.enabled) return false;
    ListState* s = State(cl);
    if (!s) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    switch (s->computeId) {
        case kTsrDilateVelocity:
            OnDilateVelocity(cl, *s);
            return false;
        case kTsrRejectShading:  // t8: dilated motion vectors
            g_frame.haveMotion = g_motion.From(cl, Bound(cl, s->compute, 0, 8), resource_usage::shader_resource_non_pixel);
            return false;
        case kTsrUpdateHistory:  // t0: scene color, translucency included
            g_frame.haveColor = g_color.From(cl, Bound(cl, s->compute, 0, 0), resource_usage::shader_resource_non_pixel);
            return false;
        case kTsrResolveHistory:
            return Evaluate(cl, *s, x, y, z);
        default:
            return false;
    }
}

void OnDestroyDevice(device* dev) {
    std::lock_guard<std::mutex> lock(g_mutex);
    dlss::Shutdown();
    g_motion.Destroy(dev);
    g_color.Destroy(dev);
}

bool g_keyWasDown = false;

void OnPresent(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    if (down && !g_keyWasDown) {
        g_set.enabled = !g_set.enabled;
        g_reset = true;
        SaveSettings();
        Log("DLSS %s (F9)", g_set.enabled ? "on" : "off");
    }
    g_keyWasDown = down;
}

}  // namespace

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
    reshade::unregister_event<reshade::addon_event::present>(&OnPresent);
}

void DrawUpscalerOverlay() {
    bool changed = false;
    ImGui::SeparatorText("DLSS (replaces TSR)");
    changed |= ImGui::Checkbox("DLSS on (F9)", &g_set.enabled);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_set.enabled ? dlss::Status().c_str() : "off: the game's TSR");
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_set.enabled && !g_error.empty()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", g_error.c_str());
        ImGui::TextDisabled("Frames upscaled: %llu. Jitter %.3f, %.3f px; velocity scale %.0f, %.0f",
                            static_cast<unsigned long long>(g_evaluated.load()), g_frame.jitter[0], g_frame.jitter[1],
                            g_frame.motionScale[0], g_frame.motionScale[1]);
    }
    const char* presets[] = {"Default", "J", "K (transformer, recommended)", "L", "M"};
    const int ids[] = {0, 10, 11, 12, 13};
    int cur = 2;
    for (int i = 0; i < 5; ++i)
        if (ids[i] == g_set.preset) cur = i;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::Combo("Preset", &cur, presets, 5)) g_set.preset = ids[cur], changed = true;
    ImGui::SeparatorText("Calibration");
    ImGui::TextDisabled("If the picture smears or shimmers when the camera moves, try these.");
    changed |= ImGui::Checkbox("Flip motion X", &g_set.flipMotionX);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Flip motion Y", &g_set.flipMotionY);
    changed |= ImGui::Checkbox("Flip jitter X", &g_set.flipJitterX);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Flip jitter Y", &g_set.flipJitterY);
    if (ImGui::Button("Reset DLSS history")) g_reset = true;
    if (changed) {
        g_reset = true;
        SaveSettings();
        Log("settings: DLSS %s, preset %d, flip motion %d%d, flip jitter %d%d", g_set.enabled ? "on" : "off", g_set.preset,
            g_set.flipMotionX, g_set.flipMotionY, g_set.flipJitterX, g_set.flipJitterY);
    }
}

}  // namespace rd
