// SPDX-License-Identifier: GPL-2.0-or-later
// Vulkan layer for native-Linux BeamNG.drive: replaces the output of the game's "FSR 3 (custom)" upscaler with
// FSR 4.1.1 at the RCAS dispatch and skips FSR 3's own passes. Every identification or consistency check fails
// open to the game's FSR 3.
//
// The manifest only loads the layer when BNG_FSR4=1 is set, and the layer stays inert in any process other than
// BeamNG.drive.

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include "fsr411.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef BNG_FSR4_ASSET_DIR
#define BNG_FSR4_ASSET_DIR ""
#endif

namespace {

// ---- configuration -------------------------------------------------------------------------------

enum class Sharpen { FollowGame, Fixed, Off };

struct Config {
    bool active = false;  // BNG_FSR4 is set and this process is BeamNG
    std::string log_dir;  // events.log and the runtime control files
    std::string asset_dir;
    std::string user_folder;  // BeamNG user folder, for the menu-label status file
    Sharpen sharpen = Sharpen::FollowGame;
    float fixed_sharpness = 0.0f;
    bool keep_fsr3 = false;  // keep dispatching FSR 3's passes under FSR 4
};
Config g_cfg;
FILE* g_log = nullptr;

std::string Env(const char* name) {
    const char* v = getenv(name);
    return v ? v : "";
}

void MakeDirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

std::string LibraryDir() {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&LibraryDir), &info) || !info.dli_fname) return ".";
    std::string path = info.dli_fname;
    return path.substr(0, path.find_last_of('/'));
}

__attribute__((constructor)) void LoadConfig() {
    const std::string on = Env("BNG_FSR4");
    if (on.empty() || on == "0") return;
    char exe[4096] = {};
    if (readlink("/proc/self/exe", exe, sizeof exe - 1) < 0 || !strstr(exe, "BeamNG.drive")) return;
    g_cfg.active = true;

    const std::string home = Env("HOME");
    const std::string cache = Env("XDG_CACHE_HOME").empty() ? home + "/.cache" : Env("XDG_CACHE_HOME");
    const std::string data = Env("XDG_DATA_HOME").empty() ? home + "/.local/share" : Env("XDG_DATA_HOME");
    g_cfg.log_dir = cache + "/bng-fsr4";
    g_cfg.asset_dir = !Env("BNG_FSR4_ASSETS").empty() ? Env("BNG_FSR4_ASSETS")
                      : *BNG_FSR4_ASSET_DIR       ? BNG_FSR4_ASSET_DIR
                                                  : LibraryDir() + "/../share/bng-fsr4/assets";
    g_cfg.user_folder = !Env("BNG_FSR4_USERFOLDER").empty() ? Env("BNG_FSR4_USERFOLDER")
                                                            : data + "/BeamNG/BeamNG.drive/current";
    if (!Env("BNG_FSR4_NO_SHARPEN").empty()) {
        g_cfg.sharpen = Sharpen::Off;
    } else if (!Env("BNG_FSR4_SHARPNESS").empty()) {
        g_cfg.sharpen = Sharpen::Fixed;
        g_cfg.fixed_sharpness = std::clamp(float(atof(Env("BNG_FSR4_SHARPNESS").c_str())), 0.0f, 1.0f);
    }
    g_cfg.keep_fsr3 = !Env("BNG_FSR4_KEEP_FSR3").empty();

    MakeDirs(g_cfg.log_dir);
    g_log = fopen((g_cfg.log_dir + "/events.log").c_str(), "w");
}

std::mutex g_log_mutex;
__attribute__((format(printf, 1, 2))) void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---- what BeamNG 0.40 records ------------------------------------------------------------------------
// Shader modules are recognised by their debug names (HLSL paths) and FNV-1a hashes of the SPIR-V. Bindings are
// in descriptor set 0 of the upscale passes.

enum Role : uint32_t { kPrepare = 1, kAccumulate = 2, kRcas = 4, kSkippable = 8 };

constexpr uint64_t kPrepareHashes[] = {0x731ab603aeae09afull};
constexpr uint64_t kAccumulateHashes[] = {0x3f70673d45a7efcbull, 0xcd2a8432c2c24191ull};
constexpr uint64_t kRcasHashes[] = {0xdfe007620205b3b2ull};

constexpr uint32_t kBindUpscalerCb = 16;  // cbFSR3Upscaler (PrepareInputs, Accumulate)
constexpr uint32_t kBindRcasCb = 17;      // cbRCAS
constexpr uint32_t kBindOutput = 31;      // rw_upscaled_output (RCAS)
constexpr uint32_t kBindMotion = 47;      // r_input_motion_vectors (PrepareInputs)
constexpr uint32_t kBindDepth = 48;       // r_input_depth
constexpr uint32_t kBindColor = 49;       // r_input_color_jittered
constexpr uint32_t kTrackedBindings[] = {kBindUpscalerCb, kBindRcasCb, kBindOutput, kBindMotion, kBindDepth, kBindColor};

// cbFSR3Upscaler, in 32-bit words.
constexpr int kCbRenderSize = 0, kCbUpscaleSize = 4, kCbJitter = 16, kCbJitterSeqLen = 27, kCbFrameIndex = 31;

uint32_t RoleOf(const std::string& name) {
    const auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
    uint32_t role = 0;
    if (has("fsr3PrepareInputs.cs.hlsl")) role |= kPrepare;
    if (has("fsr3Accumulate.cs.hlsl")) role |= kAccumulate;
    if (has("fsr3Rcas.cs.hlsl")) role |= kRcas;
    // FSR 3's own passes, which FSR 4 does not read. The engine's reactive mask (dynamicDisocclusion) only feeds
    // FSR 3; its history goes stale while skipped, which the handback to FSR 3 absorbs.
    if ((has("/upscale/fsr3/") && !(role & kRcas) && !has("fsr3DebugView.cs.hlsl")) ||
        has("/upscale/dynamicDisocclusion.cs.hlsl"))
        role |= kSkippable;
    return role;
}

bool KnownHash(uint32_t role, uint64_t hash) {
    const auto in = [hash](const auto& list) { return std::find(std::begin(list), std::end(list), hash) != std::end(list); };
    if (role & kPrepare) return in(kPrepareHashes);
    if (role & kAccumulate) return in(kAccumulateHashes);
    if (role & kRcas) return in(kRcasHashes);
    return false;
}

uint64_t Fnv1a(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) h = (h ^ bytes[i]) * 1099511628211ull;
    return h;
}

// ---- object tables -------------------------------------------------------------------------------

struct ImageInfo { VkFormat format = VK_FORMAT_UNDEFINED; uint32_t width = 0, height = 0; VkImageUsageFlags usage = 0; };
struct ModuleInfo { uint64_t hash = 0; std::string name; };
struct PipelineInfo { VkPipelineLayout layout = VK_NULL_HANDLE; uint32_t role = 0; bool known = false; };
struct Descriptor { uint64_t handle = 0; VkDeviceSize offset = 0, range = 0; VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; };
using SetInfo = std::array<Descriptor, std::size(kTrackedBindings)>;
struct CmdInfo { VkPipeline compute = VK_NULL_HANDLE; VkDescriptorSet set0 = VK_NULL_HANDLE; };

int TrackedSlot(uint32_t binding) {
    for (size_t i = 0; i < std::size(kTrackedBindings); ++i)
        if (kTrackedBindings[i] == binding) return int(i);
    return -1;
}

// ---- per-device state ----------------------------------------------------------------------------

#define DEVICE_FUNCS(X)                                                                                          \
    X(DestroyDevice) X(CreateShaderModule) X(DestroyShaderModule) X(CreateComputePipelines) X(DestroyPipeline)    \
    X(CreateImage) X(DestroyImage) X(CreateImageView) X(DestroyImageView) X(CreateBuffer) X(DestroyBuffer)        \
    X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory)               \
    X(UpdateDescriptorSets) X(BeginCommandBuffer) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdDispatch)       \
    X(CmdPipelineBarrier) X(CmdCopyBuffer) X(QueuePresentKHR) X(DeviceWaitIdle) X(SetDebugUtilsObjectNameEXT)

constexpr int kReadbackSlots = 16;
constexpr VkDeviceSize kUpscalerCbBytes = 256, kRcasCbBytes = 16;
constexpr int kHandbackFrames = 32;  // FSR 3 re-accumulates this long before a planned switch back to it

struct HostBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    uint64_t counter[kReadbackSlots] = {};  // the FSR 4 frame counter that each slot was recorded at
};

struct Inputs {
    Fsr411::Image color, depth, motion;
    ImageInfo color_info, depth_info, motion_info;
    VkImageLayout color_layout{}, depth_layout{}, motion_layout{};
    bool ok = false;
};

// Learned from the late copies of cbFSR3Upscaler: the periodic jitter table and frameIdx - counter.
struct Jitter {
    uint32_t seq_len = 0, render_w = 0, render_h = 0, output_w = 0, output_h = 0;
    int64_t offset = 0;
    bool have_offset = false;
    std::vector<std::array<float, 2>> table;
    std::vector<bool> learned;
    struct Prediction { uint64_t counter = 0; float x = 0, y = 0; bool valid = false; } predictions[kReadbackSlots];
    uint64_t checked = 0, mismatches = 0;
};

struct Device {
    VkDevice handle = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
#define X(name) PFN_vk##name name = nullptr;
    DEVICE_FUNCS(X)
#undef X
    Fsr411::Dispatch fsr_vk;
    VkPhysicalDeviceMemoryProperties memory{};
    bool features = false, mixed_dot = false, wave_size = false;

    std::mutex mutex;  // guards the tables below
    std::unordered_map<VkShaderModule, ModuleInfo> modules;
    std::unordered_map<VkPipeline, PipelineInfo> pipelines;
    std::unordered_map<VkImage, ImageInfo> images;
    std::unordered_map<VkImageView, VkImage> views;
    std::unordered_map<VkDescriptorSet, SetInfo> sets;
    std::unordered_map<VkCommandBuffer, CmdInfo> cmds;

    // Upscale-chain state. BeamNG records the chain on one thread per frame.
    std::unique_ptr<Fsr411::Upscaler> upscaler;
    std::atomic<bool> failed{false};
    HostBuffer cb_readback, rcas_readback;
    Inputs in;
    Jitter jitter;
    uint64_t counter = 0, last_used = 0;  // counter: Accumulate dispatches seen
    bool need_reset = true;
    float sharpness = -1.0f;  // < 0: no RCAS
    uint32_t backend_w = 0, backend_h = 0;
    bool backend_ultra = false;
    std::atomic<uint64_t> used{0}, fail_open{0};
    std::string fail_reason;
    // FSR 3 skipping
    bool chain_open = false, skip = false, last_soft = false;
    uint64_t chain_frame = 0, skipped_chains = 0, skipped_dispatches = 0, soft = 0, hard = 0;
    int handback = 0;
    // present
    std::atomic<uint64_t> frame{0};
    std::atomic<bool> fsr4_disabled{false}, skip_disabled{false};
    double last_status = -1e9;
};

std::mutex g_devices_mutex;
std::unordered_map<void*, Device*> g_devices;  // dispatch key -> device
struct Instance { PFN_vkGetInstanceProcAddr gipa; };
std::unordered_map<void*, Instance> g_instances;
VkInstance g_last_instance = VK_NULL_HANDLE;

void* Key(const void* handle) { return *static_cast<void* const*>(handle); }

Device* Get(const void* handle) {
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    const auto it = g_devices.find(Key(handle));
    return it == g_devices.end() ? nullptr : it->second;
}

PFN_vkGetInstanceProcAddr InstanceGipa(VkInstance instance) {
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    const auto it = g_instances.find(Key(instance));
    return it == g_instances.end() ? nullptr : it->second.gipa;
}

// ---- helpers -------------------------------------------------------------------------------------

void MemoryBarrier(Device* d, VkCommandBuffer c, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                   VkAccessFlags src, VkAccessFlags dst) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, src, dst};
    d->CmdPipelineBarrier(c, src_stage, dst_stage, 0, 1, &b, 0, nullptr, 0, nullptr);
}

void ImageBarrier(Device* d, VkCommandBuffer c, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
                  VkImageLayout to, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage, VkAccessFlags src,
                  VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    d->CmdPipelineBarrier(c, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// A persistently mapped transfer target for GPU -> CPU copies, filled with the 0xEE "not written" sentinel.
bool MakeHostBuffer(Device* d, VkDeviceSize size, HostBuffer& out) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (d->CreateBuffer(d->handle, &bi, nullptr, &out.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    d->GetBufferMemoryRequirements(d->handle, out.buffer, &req);
    const VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (VkMemoryPropertyFlags want : {need | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, need}) {
        for (uint32_t i = 0; i < d->memory.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (d->memory.memoryTypes[i].propertyFlags & want) == want) type = i;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, type};
    void* mapped = nullptr;
    if (type == UINT32_MAX || d->AllocateMemory(d->handle, &ai, nullptr, &out.memory) != VK_SUCCESS ||
        d->BindBufferMemory(d->handle, out.buffer, out.memory, 0) != VK_SUCCESS ||
        d->MapMemory(d->handle, out.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        if (out.memory) d->FreeMemory(d->handle, out.memory, nullptr);
        d->DestroyBuffer(d->handle, out.buffer, nullptr);
        out = {};
        return false;
    }
    out.mapped = static_cast<uint8_t*>(mapped);
    memset(out.mapped, 0xEE, size);
    return true;
}

void FreeHostBuffer(Device* d, HostBuffer& b) {
    if (b.buffer) d->DestroyBuffer(d->handle, b.buffer, nullptr);
    if (b.memory) d->FreeMemory(d->handle, b.memory, nullptr);
    b = {};
}

bool Unwritten(const uint8_t* slot) {
    uint32_t w[2];
    memcpy(w, slot, sizeof w);
    return w[0] == 0xEEEEEEEEu && w[1] == 0xEEEEEEEEu;
}

Descriptor BoundDescriptor(Device* d, VkCommandBuffer c, uint32_t binding) {
    std::lock_guard<std::mutex> lock(d->mutex);
    const auto set = d->sets.find(d->cmds[c].set0);
    return set == d->sets.end() ? Descriptor{} : set->second[TrackedSlot(binding)];
}

bool BoundImage(Device* d, VkCommandBuffer c, uint32_t binding, Fsr411::Image* image, ImageInfo* info,
                VkImageLayout* layout) {
    const Descriptor desc = BoundDescriptor(d, c, binding);
    if (!desc.handle) return false;
    std::lock_guard<std::mutex> lock(d->mutex);
    const auto view = d->views.find(VkImageView(desc.handle));
    if (view == d->views.end()) return false;
    const auto img = d->images.find(view->second);
    if (img == d->images.end()) return false;
    *image = {img->first, view->first, img->second.width, img->second.height};
    *info = img->second;
    *layout = desc.layout;
    return true;
}

// Copies the bound uniform range at `binding` into readback slot `counter % kReadbackSlots`.
void CopyUniform(Device* d, VkCommandBuffer c, uint32_t binding, HostBuffer& rb, VkDeviceSize bytes) {
    const Descriptor desc = BoundDescriptor(d, c, binding);
    if (!desc.handle || (!rb.buffer && !MakeHostBuffer(d, kReadbackSlots * bytes, rb))) return;
    const int slot = int(d->counter % kReadbackSlots);
    MemoryBarrier(d, c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferCopy region{desc.offset, VkDeviceSize(slot) * bytes, std::min(desc.range, bytes)};
    d->CmdCopyBuffer(c, VkBuffer(desc.handle), rb.buffer, 1, &region);
    MemoryBarrier(d, c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_HOST_READ_BIT);
    rb.counter[slot] = d->counter;
}

// ---- jitter and sharpness from the readbacks -----------------------------------------------------

// The copies land a few frames late. Learn the periodic jitter table and the frameIdx <-> counter offset, and
// check earlier predictions against what the game actually used.
void ProcessCbReadback(Device* d) {
    HostBuffer& rb = d->cb_readback;
    if (!rb.mapped) return;
    struct Snapshot { uint64_t counter; int slot; std::array<uint32_t, kUpscalerCbBytes / 4> w; };
    std::vector<Snapshot> landed;
    for (int s = 0; s < kReadbackSlots; ++s) {
        uint8_t* p = rb.mapped + s * kUpscalerCbBytes;
        if (Unwritten(p)) continue;
        Snapshot snap{rb.counter[s], s, {}};
        memcpy(snap.w.data(), p, kUpscalerCbBytes);
        memset(p, 0xEE, kUpscalerCbBytes);
        landed.push_back(snap);
    }
    std::sort(landed.begin(), landed.end(), [](const Snapshot& a, const Snapshot& b) { return a.counter < b.counter; });
    Jitter& j = d->jitter;
    for (const Snapshot& snap : landed) {
        const auto f = [&](int i) { float v; memcpy(&v, &snap.w[i], 4); return v; };
        const float seq_f = f(kCbJitterSeqLen), idx_f = f(kCbFrameIndex);
        if (!std::isfinite(seq_f) || seq_f < 1 || seq_f > 64 || !std::isfinite(idx_f) || idx_f < 0 || idx_f > 16777216.0f)
            continue;
        const uint32_t seq = uint32_t(seq_f + 0.5f);
        const int64_t idx = int64_t(idx_f + 0.5f);
        const uint32_t rw = snap.w[kCbRenderSize], rh = snap.w[kCbRenderSize + 1];
        const uint32_t ow = snap.w[kCbUpscaleSize], oh = snap.w[kCbUpscaleSize + 1];
        if (!rw || !rh) continue;
        if (seq != j.seq_len || rw != j.render_w || rh != j.render_h || ow != j.output_w || oh != j.output_h) {
            j.seq_len = seq; j.render_w = rw; j.render_h = rh; j.output_w = ow; j.output_h = oh;
            j.table.assign(seq, {0, 0});
            j.learned.assign(seq, false);
            j.have_offset = false;
            d->need_reset = true;
            Log("FSR4: render %ux%u output %ux%u jitter sequence %u; relearning jitter", rw, rh, ow, oh, seq);
        }
        const int64_t offset = idx - int64_t(snap.counter);
        if (!j.have_offset || offset != j.offset) {
            // A reset can restart the sequence at a different phase: never reuse the old table.
            j.offset = offset;
            j.have_offset = true;
            std::fill(j.learned.begin(), j.learned.end(), false);
            d->need_reset = true;
            Log("FSR4: frameIdx offset now %lld (counter %llu); relearning jitter", (long long)offset,
                (unsigned long long)snap.counter);
        }
        const float jx = f(kCbJitter), jy = f(kCbJitter + 1);
        if (!std::isfinite(jx) || !std::isfinite(jy) || std::abs(jx) > 1.0f || std::abs(jy) > 1.0f) continue;
        auto& prediction = j.predictions[snap.slot];
        if (prediction.valid && prediction.counter == snap.counter) {
            const bool mismatch = prediction.x != jx || prediction.y != jy;
            ++j.checked;
            j.mismatches += mismatch;
            if (mismatch)
                Log("FSR4: jitter mismatch at counter %llu: predicted [%.9g %.9g], actual [%.9g %.9g]",
                    (unsigned long long)snap.counter, prediction.x, prediction.y, jx, jy);
            prediction.valid = false;
        }
        const uint32_t phase = uint32_t(idx % int64_t(seq));
        j.table[phase] = {jx, jy};
        j.learned[phase] = true;
    }
}

// cbRCAS.x = exp2(-(2 - 2 * sharpness)) (FidelityFX FsrRcasCon); FSR 4's RCAS takes the same 0..1 sharpness.
void ProcessRcasReadback(Device* d) {
    HostBuffer& rb = d->rcas_readback;
    if (g_cfg.sharpen != Sharpen::FollowGame || !rb.mapped) return;
    uint64_t newest = 0;
    float value = 0;
    bool found = false;
    for (int s = 0; s < kReadbackSlots; ++s) {
        uint8_t* p = rb.mapped + s * kRcasCbBytes;
        if (Unwritten(p)) continue;
        float v;
        memcpy(&v, p, 4);
        memset(p, 0xEE, kRcasCbBytes);
        if (!found || rb.counter[s] > newest) { newest = rb.counter[s]; value = v; found = true; }
    }
    if (!found || !std::isfinite(value) || value <= 0.0f || value > 1.0001f) return;
    const float s = std::clamp((std::log2(value) + 2.0f) * 0.5f, 0.0f, 1.0f);
    if (d->sharpness < 0.0f || std::abs(s - d->sharpness) > 1e-4f) Log("FSR4: sharpness %.3f (from the game)", s);
    d->sharpness = s;
}

// ---- the FSR 4 replacement -----------------------------------------------------------------------

bool ShouldSkipChain(Device* d) {
    return !g_cfg.keep_fsr3 && !d->failed && d->upscaler && !d->last_soft && d->used && d->last_used == d->counter &&
           !d->fsr4_disabled && !d->skip_disabled;
}

void CaptureInputs(Device* d, VkCommandBuffer c) {
    Inputs& in = d->in;
    in.ok = BoundImage(d, c, kBindColor, &in.color, &in.color_info, &in.color_layout) &&
            BoundImage(d, c, kBindDepth, &in.depth, &in.depth_info, &in.depth_layout) &&
            BoundImage(d, c, kBindMotion, &in.motion, &in.motion_info, &in.motion_layout);
}

// Records FSR 4 in place of the RCAS dispatch. Returns false when the game's RCAS should run instead.
bool ReplaceRcas(Device* d, VkCommandBuffer c, const PipelineInfo& rcas) {
    // A skipped FSR 3 chain left no FSR 3 result: falling back then shows a stale image for one frame.
    const bool skipped = d->skip;
    const auto fail = [&](const char* why) {
        ++d->fail_open;
        if (d->fail_reason != why) { d->fail_reason = why; Log("FSR4: using FSR 3 (%s)", why); }
        if (skipped) { ++d->hard; Log("FSR4: FSR 3 chain was skipped; stale output this frame (%s)", why); }
        return false;
    };
    // Soft reasons come from the late readback (resets, size changes). On a skipped frame FSR 4 still runs, with
    // a history reset, and the next chain runs FSR 3.
    const char* soft = nullptr;
    const auto soft_fail = [&](const char* why) {
        if (!skipped) return fail(why);
        if (!soft) soft = why;
        return true;
    };

    if (d->failed) return false;
    if (!rcas.known) return fail("unknown RCAS shader");
    if (!d->features) return fail("device features not enabled");
    ProcessCbReadback(d);
    ProcessRcasReadback(d);
    if (d->fsr4_disabled) {
        // FSR 3's history is stale after skipping: keep FSR 4 on screen while it re-accumulates.
        if (g_cfg.keep_fsr3 || d->handback >= kHandbackFrames) return fail("fsr4.disable");
        ++d->handback;
    } else {
        d->handback = 0;
    }
    const Inputs& in = d->in;
    if (!in.ok) return fail("inputs not captured");
    Fsr411::Image out;
    ImageInfo out_info;
    VkImageLayout out_layout;
    if (!BoundImage(d, c, kBindOutput, &out, &out_info, &out_layout) || out_layout != VK_IMAGE_LAYOUT_GENERAL)
        return fail("unexpected output binding");
    const Jitter& j = d->jitter;
    if (!j.have_offset || !j.seq_len) {
        if (!soft_fail("no constant buffer readback yet")) return false;
    } else if (in.color.width != j.render_w || in.color.height != j.render_h || out.width != j.output_w ||
               out.height != j.output_h) {
        if (!soft_fail("sizes differ from the constant buffer")) return false;
    }
    if (in.color_info.format != VK_FORMAT_R16G16B16A16_SFLOAT) return fail("color format");
    if (out_info.format != VK_FORMAT_R16G16B16A16_SFLOAT || !(out_info.usage & VK_IMAGE_USAGE_STORAGE_BIT))
        return fail("output format or usage");
    if (in.depth_info.format != VK_FORMAT_D32_SFLOAT_S8_UINT || in.motion_info.format != VK_FORMAT_R16G16_SFLOAT)
        return fail("depth or motion format");
    if (in.depth.width != in.color.width || in.depth.height != in.color.height || in.motion.width != in.color.width ||
        in.motion.height != in.color.height)
        return fail("depth or motion size");
    if (!(in.color_info.usage & in.depth_info.usage & in.motion_info.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
        return fail("input usage");
    uint32_t phase = 0;
    if (!soft) {
        const int64_t idx = int64_t(d->counter) + j.offset, n = int64_t(j.seq_len);
        phase = uint32_t(((idx % n) + n) % n);
        if (!j.learned[phase] && !soft_fail("jitter not learned yet")) return false;
    }

    if (!d->upscaler) {
        d->upscaler = std::make_unique<Fsr411::Upscaler>(d->fsr_vk, d->physical, d->handle, g_cfg.asset_dir,
                                                         d->mixed_dot, d->wave_size);
        Log("FSR4: backend constructed (assets %s, mixed float dot %d, wave size control %d)", g_cfg.asset_dir.c_str(),
            int(d->mixed_dot), int(d->wave_size));
    }
    VkPipeline game_pipeline;
    VkDescriptorSet game_set;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        game_pipeline = d->cmds[c].compute;
        game_set = d->cmds[c].set0;
    }

    // The backend wants every image in GENERAL.
    constexpr VkAccessFlags rw = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    constexpr VkImageAspectFlags color = VK_IMAGE_ASPECT_COLOR_BIT;
    constexpr VkImageAspectFlags depth = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    constexpr VkPipelineStageFlags all = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    MemoryBarrier(d, c, all, compute, VK_ACCESS_MEMORY_WRITE_BIT, rw);
    ImageBarrier(d, c, in.color.image, color, in.color_layout, VK_IMAGE_LAYOUT_GENERAL, all, compute, VK_ACCESS_MEMORY_WRITE_BIT, rw);
    ImageBarrier(d, c, in.depth.image, depth, in.depth_layout, VK_IMAGE_LAYOUT_GENERAL, all, compute, VK_ACCESS_MEMORY_WRITE_BIT, rw);
    ImageBarrier(d, c, in.motion.image, color, in.motion_layout, VK_IMAGE_LAYOUT_GENERAL, all, compute, VK_ACCESS_MEMORY_WRITE_BIT, rw);

    Fsr411::Frame frame;
    frame.cmdbuf = c;
    frame.color = in.color;
    frame.depth = in.depth;
    frame.motion = in.motion;
    frame.output = out;
    frame.render_width = in.color.width;
    frame.render_height = in.color.height;
    frame.ultra_performance = float(out.width) / float(in.color.width) >= 2.9f;
    // A soft run has no trustworthy jitter; with the history reset a wrong guess affects only this frame.
    if (!soft) { frame.jitter[0] = j.table[phase][0]; frame.jitter[1] = j.table[phase][1]; }
    frame.motion_scale[0] = float(in.color.width);  // BeamNG's motion vectors are in UV units
    frame.motion_scale[1] = float(in.color.height);
    const float sharpness = g_cfg.sharpen == Sharpen::Fixed ? g_cfg.fixed_sharpness
                            : g_cfg.sharpen == Sharpen::Off ? -1.0f
                                                            : d->sharpness;
    frame.sharpen = sharpness >= 0.0f;
    frame.sharpness = std::max(sharpness, 0.0f);
    frame.reset = soft || d->need_reset || d->last_used + 1 != d->counter;
    // A size or model change destroys the backend's images and constant ring: retire submitted work first.
    if (d->used && (d->backend_w != out.width || d->backend_h != out.height || d->backend_ultra != frame.ultra_performance))
        d->DeviceWaitIdle(d->handle);
    const bool ok = d->upscaler->Record(frame);
    if (!ok) {
        Log("FSR4: backend failed (%s); using FSR 3 from now on", d->upscaler->Error().c_str());
        d->failed = true;
    }
    d->need_reset = false;
    d->last_used = d->counter;

    // Return the inputs to the game's layouts and make the output visible to later passes.
    constexpr VkAccessFlags read = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    ImageBarrier(d, c, in.color.image, color, VK_IMAGE_LAYOUT_GENERAL, in.color_layout, compute, all, rw, read);
    ImageBarrier(d, c, in.depth.image, depth, VK_IMAGE_LAYOUT_GENERAL, in.depth_layout, compute, all, rw, read);
    ImageBarrier(d, c, in.motion.image, color, VK_IMAGE_LAYOUT_GENERAL, in.motion_layout, compute, all, rw, read);
    MemoryBarrier(d, c, compute, all, VK_ACCESS_SHADER_WRITE_BIT, rw | VK_ACCESS_MEMORY_READ_BIT);
    // The backend's pipelines and push descriptors replaced the game's compute state. Restore it even when Record
    // failed, so that the original dispatch is a valid fallback.
    d->CmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, game_pipeline);
    d->CmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, rcas.layout, 0, 1, &game_set, 0, nullptr);
    if (!ok) return false;

    d->backend_w = out.width;
    d->backend_h = out.height;
    d->backend_ultra = frame.ultra_performance;
    if (!d->fail_reason.empty()) {
        Log("FSR4: replacing FSR 3 again (after: %s)", d->fail_reason.c_str());
        d->fail_reason.clear();
    }
    if (soft) {
        ++d->soft;
        d->last_soft = true;
        Log("FSR4: history reset on a skipped FSR 3 chain at counter %llu (%s)", (unsigned long long)d->counter, soft);
    } else {
        d->last_soft = false;
        d->jitter.predictions[d->counter % kReadbackSlots] = {d->counter, frame.jitter[0], frame.jitter[1], true};
    }
    if (++d->used % 600 == 1)
        Log("FSR4: frames replaced %llu, fail-open %llu, skipped chains %llu, soft %llu, hard %llu, jitter checked %llu "
            "mismatched %llu",
            (unsigned long long)d->used, (unsigned long long)d->fail_open, (unsigned long long)d->skipped_chains,
            (unsigned long long)d->soft, (unsigned long long)d->hard, (unsigned long long)d->jitter.checked,
            (unsigned long long)d->jitter.mismatches);
    return true;
}

// ---- intercepted device functions ----------------------------------------------------------------

VKAPI_ATTR void VKAPI_CALL L_CmdDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z) {
    Device* d = Get(c);
    PipelineInfo pipe;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        const auto it = d->pipelines.find(d->cmds[c].compute);
        if (it != d->pipelines.end()) pipe = it->second;
    }
    if (pipe.role & kPrepare) {
        d->in.ok = false;
        if (pipe.known) CaptureInputs(d, c);
    } else if (pipe.role & kAccumulate) {
        ++d->counter;
        if (pipe.known) CopyUniform(d, c, kBindUpscalerCb, d->cb_readback, kUpscalerCbBytes);
        else d->in.ok = false;
    } else if (pipe.role & kRcas) {
        if (g_cfg.sharpen == Sharpen::FollowGame) CopyUniform(d, c, kBindRcasCb, d->rcas_readback, kRcasCbBytes);
        const bool replaced = ReplaceRcas(d, c, pipe);
        d->chain_open = d->skip = false;
        if (replaced) return;
    }
    if (pipe.role & kSkippable) {
        // One decision per frame's chain; a new present means a new chain even without RCAS.
        const uint64_t frame = d->frame;
        if (!d->chain_open || d->chain_frame != frame) {
            d->chain_open = true;
            d->chain_frame = frame;
            d->skip = ShouldSkipChain(d);
            d->skipped_chains += d->skip;
        }
        if (d->skip) {
            ++d->skipped_dispatches;
            return;
        }
    }
    d->CmdDispatch(c, x, y, z);
}

VKAPI_ATTR void VKAPI_CALL L_CmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipeline p) {
    Device* d = Get(c);
    if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->cmds[c].compute = p;
    }
    d->CmdBindPipeline(c, bp, p);
}

VKAPI_ATTR void VKAPI_CALL L_CmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout l,
                                                   uint32_t first, uint32_t n, const VkDescriptorSet* sets,
                                                   uint32_t dyn_count, const uint32_t* dyn) {
    Device* d = Get(c);
    if (bp == VK_PIPELINE_BIND_POINT_COMPUTE && first == 0 && n) {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->cmds[c].set0 = sets[0];
    }
    d->CmdBindDescriptorSets(c, bp, l, first, n, sets, dyn_count, dyn);
}

VKAPI_ATTR VkResult VKAPI_CALL L_BeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo* info) {
    Device* d = Get(c);
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->cmds[c] = {};
    }
    return d->BeginCommandBuffer(c, info);
}

VKAPI_ATTR void VKAPI_CALL L_UpdateDescriptorSets(VkDevice dev, uint32_t write_count, const VkWriteDescriptorSet* writes,
                                                  uint32_t copy_count, const VkCopyDescriptorSet* copies) {
    Device* d = Get(dev);
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        for (uint32_t i = 0; i < write_count; ++i) {
            const VkWriteDescriptorSet& w = writes[i];
            // Only single descriptors are tracked; arrays never hold the upscaler's bindings.
            const int slot = TrackedSlot(w.dstBinding);
            if (slot < 0 || w.dstArrayElement != 0 || !w.descriptorCount) continue;
            Descriptor& desc = d->sets[w.dstSet][slot];
            switch (w.descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                desc = {uint64_t(w.pImageInfo[0].imageView), 0, 0, w.pImageInfo[0].imageLayout};
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                desc = {uint64_t(w.pBufferInfo[0].buffer), w.pBufferInfo[0].offset, w.pBufferInfo[0].range, {}};
                break;
            default:
                desc = {};
                break;
            }
        }
        for (uint32_t i = 0; i < copy_count; ++i) {
            const VkCopyDescriptorSet& cp = copies[i];
            const int src = TrackedSlot(cp.srcBinding), dst = TrackedSlot(cp.dstBinding);
            if (src < 0 || dst < 0 || cp.srcArrayElement || cp.dstArrayElement) continue;
            const auto it = d->sets.find(cp.srcSet);
            d->sets[cp.dstSet][dst] = it == d->sets.end() ? Descriptor{} : it->second[src];
        }
    }
    d->UpdateDescriptorSets(dev, write_count, writes, copy_count, copies);
}

VKAPI_ATTR VkResult VKAPI_CALL L_CreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo* ci,
                                                    const VkAllocationCallbacks* a, VkShaderModule* m) {
    Device* d = Get(dev);
    const VkResult r = d->CreateShaderModule(dev, ci, a, m);
    if (r == VK_SUCCESS) {
        const uint64_t hash = Fnv1a(ci->pCode, ci->codeSize);
        std::lock_guard<std::mutex> lock(d->mutex);
        d->modules[*m] = {hash, {}};
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL L_DestroyShaderModule(VkDevice dev, VkShaderModule m, const VkAllocationCallbacks* a) {
    Device* d = Get(dev);
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->modules.erase(m);
    }
    d->DestroyShaderModule(dev, m, a);
}

VKAPI_ATTR VkResult VKAPI_CALL L_SetDebugUtilsObjectNameEXT(VkDevice dev, const VkDebugUtilsObjectNameInfoEXT* info) {
    Device* d = Get(dev);
    if (info->objectType == VK_OBJECT_TYPE_SHADER_MODULE && info->pObjectName) {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->modules[VkShaderModule(info->objectHandle)].name = info->pObjectName;
    }
    return d->SetDebugUtilsObjectNameEXT(dev, info);
}

VKAPI_ATTR VkResult VKAPI_CALL L_CreateComputePipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
                                                        const VkComputePipelineCreateInfo* ci,
                                                        const VkAllocationCallbacks* a, VkPipeline* p) {
    Device* d = Get(dev);
    const VkResult r = d->CreateComputePipelines(dev, cache, n, ci, a, p);
    if (r != VK_SUCCESS) return r;
    std::lock_guard<std::mutex> lock(d->mutex);
    for (uint32_t i = 0; i < n; ++i) {
        const ModuleInfo& module = d->modules[ci[i].stage.module];
        const uint32_t role = RoleOf(module.name);
        if (role) d->pipelines[p[i]] = {ci[i].layout, role, KnownHash(role, module.hash)};
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL L_DestroyPipeline(VkDevice dev, VkPipeline p, const VkAllocationCallbacks* a) {
    Device* d = Get(dev);
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->pipelines.erase(p);
    }
    d->DestroyPipeline(dev, p, a);
}

VKAPI_ATTR VkResult VKAPI_CALL L_CreateImage(VkDevice dev, const VkImageCreateInfo* ci, const VkAllocationCallbacks* a,
                                             VkImage* image) {
    Device* d = Get(dev);
    const VkResult r = d->CreateImage(dev, ci, a, image);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->images[*image] = {ci->format, ci->extent.width, ci->extent.height, ci->usage};
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL L_DestroyImage(VkDevice dev, VkImage image, const VkAllocationCallbacks* a) {
    Device* d = Get(dev);
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->images.erase(image);
    }
    d->DestroyImage(dev, image, a);
}

VKAPI_ATTR VkResult VKAPI_CALL L_CreateImageView(VkDevice dev, const VkImageViewCreateInfo* ci,
                                                 const VkAllocationCallbacks* a, VkImageView* view) {
    Device* d = Get(dev);
    const VkResult r = d->CreateImageView(dev, ci, a, view);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->views[*view] = ci->image;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL L_DestroyImageView(VkDevice dev, VkImageView view, const VkAllocationCallbacks* a) {
    Device* d = Get(dev);
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->views.erase(view);
    }
    d->DestroyImageView(dev, view, a);
}

// The constant buffers are copied back for the jitter and the sharpness, so uniform buffers need TRANSFER_SRC.
VKAPI_ATTR VkResult VKAPI_CALL L_CreateBuffer(VkDevice dev, const VkBufferCreateInfo* ci, const VkAllocationCallbacks* a,
                                              VkBuffer* buffer) {
    Device* d = Get(dev);
    VkBufferCreateInfo copy = *ci;
    if (copy.usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) copy.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    return d->CreateBuffer(dev, &copy, a, buffer);
}

bool FileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// The BeamNG menu-label mod cannot read the environment, so the layer reports itself in
// <user folder>/temp/bng_fsr4/status.json about every 5 s.
void WriteStatus(Device* d) {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
    const double t = double(now.tv_sec) + now.tv_nsec * 1e-9;
    if (t - d->last_status < 5.0) return;
    d->last_status = t;
    if (!FileExists(g_cfg.user_folder)) return;
    const std::string dir = g_cfg.user_folder + "/temp/bng_fsr4", path = dir + "/status.json", tmp = path + ".tmp";
    MakeDirs(dir);
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    fprintf(f, "{\"layer\":\"bng-fsr4\",\"time\":%lld,\"ok\":%s,\"replaced\":%llu,\"failopen\":%llu}\n",
            (long long)time(nullptr), d->features && !d->failed ? "true" : "false", (unsigned long long)d->used,
            (unsigned long long)d->fail_open);
    fclose(f);
    rename(tmp.c_str(), path.c_str());
}

VKAPI_ATTR VkResult VKAPI_CALL L_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* info) {
    Device* d = Get(q);
    // The control files are checked every 8th frame.
    if (d->frame++ % 8 == 0) {
        d->fsr4_disabled = FileExists(g_cfg.log_dir + "/fsr4.disable");
        d->skip_disabled = FileExists(g_cfg.log_dir + "/fsr3skip.disable");
        WriteStatus(d);
    }
    return d->QueuePresentKHR(q, info);
}

VKAPI_ATTR void VKAPI_CALL L_DestroyDevice(VkDevice dev, const VkAllocationCallbacks* a) {
    Device* d = Get(dev);
    if (!d) return;
    {
        std::lock_guard<std::mutex> lock(g_devices_mutex);
        g_devices.erase(Key(dev));
    }
    if (d->upscaler || d->cb_readback.buffer || d->rcas_readback.buffer) d->DeviceWaitIdle(dev);
    d->upscaler.reset();
    FreeHostBuffer(d, d->cb_readback);
    FreeHostBuffer(d, d->rcas_readback);
    const PFN_vkDestroyDevice destroy = d->DestroyDevice;
    delete d;
    destroy(dev, a);
}

// ---- device creation -----------------------------------------------------------------------------

template <typename T>
T* FindInChain(const void* next, VkStructureType type) {
    for (auto* p = static_cast<VkBaseOutStructure*>(const_cast<void*>(next)); p; p = p->pNext)
        if (p->sType == type) return reinterpret_cast<T*>(p);
    return nullptr;
}

bool HasExtension(const VkDeviceCreateInfo* ci, const char* name) {
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i)
        if (!strcmp(ci->ppEnabledExtensionNames[i], name)) return true;
    return false;
}

// Creates the device with what the FSR 4 INT8 path needs on top of the game's request. Returns false (with the
// request untouched) when that is not possible; the caller then creates the device as the game asked.
bool CreateWithFsr4Features(PFN_vkCreateDevice create, PFN_vkGetInstanceProcAddr gipa, VkInstance instance,
                            VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a,
                            VkDevice* out, VkLayerDeviceCreateInfo* link, Device* d, VkResult* result) {
    auto features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(instance, "vkGetPhysicalDeviceFeatures2"));
    auto props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2"));
    auto* game12 = FindInChain<VkPhysicalDeviceVulkan12Features>(ci->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    auto* game13 = FindInChain<VkPhysicalDeviceVulkan13Features>(ci->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    if (!features2 || !props2 || !game12 || !game13) {
        Log("FSR4: the game's device request has no Vulkan 1.2/1.3 feature structs; leaving it unmodified");
        return false;
    }
    const bool push_descriptor = HasExtension(ci, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    const bool derivatives = HasExtension(ci, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME) ||
                             HasExtension(ci, VK_NV_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
    if (!push_descriptor || !derivatives) {
        Log("FSR4: the game did not enable %s; leaving the device unmodified",
            !push_descriptor ? VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME : VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
        return false;
    }

    VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE mixed{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MIXED_FLOAT_DOT_PRODUCT_FEATURES_VALVE};
    VkPhysicalDeviceVulkan13Features has13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &mixed};
    VkPhysicalDeviceVulkan12Features has12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &has13};
    VkPhysicalDeviceFeatures2 has{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &has12};
    features2(pd, &has);
    if (!has12.shaderInt8 || !has13.shaderIntegerDotProduct) {
        Log("FSR4: the GPU lacks shaderInt8 or shaderIntegerDotProduct");
        return false;
    }
    VkPhysicalDeviceSubgroupSizeControlProperties sizes{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sizes};
    props2(pd, &props);
    // Required wave sizes per FSR 4 pass; enabling the feature does not change the game's pipelines.
    const bool wave = has13.subgroupSizeControl && (sizes.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) &&
                      sizes.minSubgroupSize <= 32 && sizes.maxSubgroupSize >= 64;
    const bool mixed_dot = mixed.shaderMixedFloatDotProductFloat16AccFloat32 == VK_TRUE;

    const VkPhysicalDeviceVulkan12Features saved12 = *game12;
    const VkPhysicalDeviceVulkan13Features saved13 = *game13;
    game12->shaderInt8 = VK_TRUE;
    game13->shaderIntegerDotProduct = VK_TRUE;
    if (wave) game13->subgroupSizeControl = VK_TRUE;

    VkDeviceCreateInfo modified = *ci;
    std::vector<const char*> extensions(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
    VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE enable_mixed{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MIXED_FLOAT_DOT_PRODUCT_FEATURES_VALVE};
    auto* game_mixed = FindInChain<VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE>(
        ci->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MIXED_FLOAT_DOT_PRODUCT_FEATURES_VALVE);
    const VkBool32 saved_mixed = game_mixed ? game_mixed->shaderMixedFloatDotProductFloat16AccFloat32 : VK_FALSE;
    if (mixed_dot) {
        if (!HasExtension(ci, VK_VALVE_SHADER_MIXED_FLOAT_DOT_PRODUCT_EXTENSION_NAME))
            extensions.push_back(VK_VALVE_SHADER_MIXED_FLOAT_DOT_PRODUCT_EXTENSION_NAME);
        modified.enabledExtensionCount = uint32_t(extensions.size());
        modified.ppEnabledExtensionNames = extensions.data();
        if (game_mixed) {
            game_mixed->shaderMixedFloatDotProductFloat16AccFloat32 = VK_TRUE;
        } else {
            enable_mixed.shaderMixedFloatDotProductFloat16AccFloat32 = VK_TRUE;
            enable_mixed.pNext = const_cast<void*>(ci->pNext);
            modified.pNext = &enable_mixed;
        }
    }
    const VkLayerDeviceLink* next_link = link->u.pLayerInfo;
    *result = create(pd, &modified, a, out);
    // The game's structs are its own memory: put them back whatever happened.
    *game12 = saved12;
    *game13 = saved13;
    if (game_mixed) game_mixed->shaderMixedFloatDotProductFloat16AccFloat32 = saved_mixed;
    if (*result != VK_SUCCESS) {
        // The layers below advanced the link on the way down; rewind it for the retry.
        link->u.pLayerInfo = const_cast<VkLayerDeviceLink*>(next_link);
        Log("FSR4: device creation with the FSR 4 features failed (%d); retrying unmodified", int(*result));
        return false;
    }
    d->features = true;
    d->mixed_dot = mixed_dot;
    d->wave_size = wave;
    Log("FSR4: device created for FSR 4 (%s, mixed float dot %d, wave size control %d)", props.properties.deviceName,
        int(mixed_dot), int(wave));
    return true;
}

VKAPI_ATTR VkResult VKAPI_CALL L_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                              const VkAllocationCallbacks* a, VkDevice* out) {
    auto* link = FindInChain<VkLayerDeviceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    while (link && link->function != VK_LAYER_LINK_INFO)
        link = FindInChain<VkLayerDeviceCreateInfo>(link->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;  // the next layer reads its own link

    VkInstance instance;
    {
        std::lock_guard<std::mutex> lock(g_devices_mutex);
        instance = g_last_instance;
    }
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"));
    auto d = std::make_unique<Device>();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (!g_cfg.active || !CreateWithFsr4Features(create, gipa, instance, pd, ci, a, out, link, d.get(), &r))
        r = create(pd, ci, a, out);
    if (r != VK_SUCCESS) return r;

    d->handle = *out;
    d->physical = pd;
    d->gdpa = gdpa;
#define X(name) d->name = reinterpret_cast<PFN_vk##name>(gdpa(*out, "vk" #name));
    DEVICE_FUNCS(X)
#undef X
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(instance, "vkGetPhysicalDeviceMemoryProperties"))(pd, &d->memory);
    if (d->features && !d->fsr_vk.Load(gipa, instance, gdpa, *out)) {
        Log("FSR4: a Vulkan entry point the backend needs is missing");
        d->features = false;
    }
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    g_devices[Key(*out)] = d.release();
    return VK_SUCCESS;
}

// ---- proc-addr plumbing --------------------------------------------------------------------------

struct Hook { const char* name; PFN_vkVoidFunction fn; };
#define HOOK(name) {"vk" #name, reinterpret_cast<PFN_vkVoidFunction>(L_##name)}
const Hook kHooks[] = {
    HOOK(CreateShaderModule), HOOK(DestroyShaderModule), HOOK(SetDebugUtilsObjectNameEXT),
    HOOK(CreateComputePipelines), HOOK(DestroyPipeline), HOOK(CreateImage), HOOK(DestroyImage), HOOK(CreateImageView),
    HOOK(DestroyImageView), HOOK(CreateBuffer), HOOK(UpdateDescriptorSets), HOOK(BeginCommandBuffer),
    HOOK(CmdBindPipeline), HOOK(CmdBindDescriptorSets), HOOK(CmdDispatch), HOOK(QueuePresentKHR),
};
#undef HOOK

PFN_vkVoidFunction FindHook(const char* name) {
    if (!strcmp(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(L_DestroyDevice);  // frees the Device record
    if (!g_cfg.active) return nullptr;
    for (const Hook& h : kHooks)
        if (!strcmp(h.name, name)) return h.fn;
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL L_GetDeviceProcAddr(VkDevice dev, const char* name) {
    Device* d = Get(dev);
    if (!d) return nullptr;
    const PFN_vkVoidFunction next = d->gdpa(dev, name);
    if (!next) return nullptr;
    const PFN_vkVoidFunction hook = FindHook(name);
    return hook ? hook : next;
}

VKAPI_ATTR VkResult VKAPI_CALL L_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a,
                                                VkInstance* out);
VKAPI_ATTR void VKAPI_CALL L_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* a);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL L_GetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!strcmp(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(L_CreateInstance);
    if (!strcmp(name, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(L_GetInstanceProcAddr);
    if (!instance) return nullptr;
    if (!strcmp(name, "vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(L_DestroyInstance);
    if (!strcmp(name, "vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(L_CreateDevice);
    if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(L_GetDeviceProcAddr);
    const PFN_vkGetInstanceProcAddr gipa = InstanceGipa(instance);
    if (!gipa) return nullptr;
    const PFN_vkVoidFunction next = gipa(instance, name);
    // Device functions can also be fetched through the instance.
    if (next)
        if (const PFN_vkVoidFunction hook = FindHook(name)) return hook;
    return next;
}

VKAPI_ATTR VkResult VKAPI_CALL L_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a,
                                                VkInstance* out) {
    auto* link = FindInChain<VkLayerInstanceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    while (link && link->function != VK_LAYER_LINK_INFO)
        link = FindInChain<VkLayerInstanceCreateInfo>(link->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    const VkResult r = create(ci, a, out);
    if (r != VK_SUCCESS) return r;
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    g_instances[Key(*out)] = {gipa};
    g_last_instance = *out;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL L_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* a) {
    const PFN_vkGetInstanceProcAddr gipa = InstanceGipa(instance);
    {
        std::lock_guard<std::mutex> lock(g_devices_mutex);
        g_instances.erase(Key(instance));
        if (g_last_instance == instance) g_last_instance = VK_NULL_HANDLE;
    }
    if (gipa) reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"))(instance, a);
}

}  // namespace

extern "C" __attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if (v->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = L_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = L_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
