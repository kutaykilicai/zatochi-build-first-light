// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <xxhash.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/logging/log.h"
#include "common/stb.h"
#include "core/memory.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/tex_replace.h"

namespace VideoCore::TexReplace {

namespace {

const bool g_flag = [] {
    const char* v = std::getenv("SHADPS4_TEX_REPLACE");
    return v && v[0] == '1';
}();

std::vector<Target> g_targets;
std::once_flag g_load_once;
std::atomic<u32> g_logged{0};

const bool g_auto = [] {
    const char* v = std::getenv("SHADPS4_TEX_REPLACE_AUTO");
    return !(v && v[0] == '0');
}();
std::atomic<bool> g_gamepad{false};
std::mutex g_seen_mutex;
std::vector<std::pair<VAddr, u32>> g_seen; ///< guest ranges of matched textures

std::filesystem::path ExeDir() {
#ifdef _WIN32
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return std::filesystem::current_path();
        }
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

bool LoadPng(const std::filesystem::path& path, std::vector<u8>& rgba, int& w, int& h) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    int channels = 0;
    u8* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h,
                                       &channels, 4);
    if (!pixels) {
        return false;
    }
    rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    stbi_image_free(pixels);
    return true;
}

void Load() {
    const char* dir_env = std::getenv("SHADPS4_TEX_REPLACE_DIR");
    const std::filesystem::path dir =
        dir_env && dir_env[0] ? std::filesystem::path(dir_env) : ExeDir() / "tex_replace";
    const auto manifest = dir / "replacements.txt";
    std::ifstream in(manifest);
    if (!in) {
        LOG_WARNING(Render_Vulkan, "TexReplace: no manifest at {} - nothing replaced",
                    manifest.string());
        return;
    }
    std::string line;
    u32 line_no = 0;
    u32 loaded = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.resize(hash);
        }
        std::istringstream ls(line);
        std::string key_str, file, opt;
        u32 tex_w = 0, tex_h = 0, x = 0, y = 0;
        if (!(ls >> key_str)) {
            continue; // blank or comment
        }
        if (!(ls >> tex_w >> tex_h >> x >> y >> file)) {
            LOG_WARNING(Render_Vulkan, "TexReplace: line {} not understood", line_no);
            continue;
        }
        ls >> opt;
        const bool flip = opt != "noflip";
        const u64 key = std::strtoull(key_str.c_str(), nullptr, 16);

        Patch patch{};
        int w = 0, h = 0;
        std::vector<u8> rgba;
        if (!LoadPng(dir / file, rgba, w, h)) {
            LOG_WARNING(Render_Vulkan, "TexReplace: line {}: cannot read {} as PNG", line_no, file);
            continue;
        }
        if (w <= 0 || h <= 0 || x + u32(w) > tex_w || y + u32(h) > tex_h) {
            LOG_WARNING(Render_Vulkan,
                        "TexReplace: line {}: {} is {}x{} at {},{} - outside the {}x{} texture",
                        line_no, file, w, h, x, y, tex_w, tex_h);
            continue;
        }
        patch.x = x;
        patch.width = u32(w);
        patch.height = u32(h);
        patch.y = flip ? tex_h - y - u32(h) : y;
        patch.rgba.resize(rgba.size());
        const size_t row = size_t(w) * 4;
        for (u32 r = 0; r < u32(h); ++r) {
            const u32 src_row = flip ? u32(h) - 1 - r : r;
            std::memcpy(patch.rgba.data() + r * row, rgba.data() + src_row * row, row);
        }

        Target* target = nullptr;
        for (auto& t : g_targets) {
            if (t.key == key && t.width == tex_w && t.height == tex_h) {
                target = &t;
            }
        }
        if (!target) {
            target = &g_targets.emplace_back(Target{key, tex_w, tex_h, {}});
        }
        target->patches.push_back(std::move(patch));
        ++loaded;
    }
    LOG_INFO(Render_Vulkan, "TexReplace: {} pictures for {} textures from {}", loaded,
             g_targets.size(), manifest.string());
}

} // Anonymous namespace

bool Enabled() {
    if (!g_flag) {
        return false;
    }
    std::call_once(g_load_once, Load);
    return !g_targets.empty();
}

const Target* Find(const ImageInfo& info) {
    // 2026-10-31: BC formats (pictures encoded per block) and mip chains (pictures scaled per mip).
    if (info.num_samples != 1 || info.resources.layers != 1 ||
        (info.props.is_block ? BlockBytes(info) == 0 : info.num_bits != 32) ||
        info.type != AmdGpu::ImageType::Color2D) {
        return nullptr;
    }
    bool size_hit = false;
    for (const auto& t : g_targets) {
        size_hit |= t.width == info.size.width && t.height == info.size.height;
    }
    if (!size_hit || info.guest_address == 0 || info.guest_size == 0) {
        return nullptr;
    }
    const u64 key =
        XXH3_64bits(reinterpret_cast<const void*>(info.guest_address), info.guest_size);
    const Target* found = nullptr;
    for (const auto& t : g_targets) {
        if (t.key == key && t.width == info.size.width && t.height == info.size.height) {
            found = &t;
        }
    }
    if (found) {
        std::scoped_lock lk{g_seen_mutex};
        const std::pair<VAddr, u32> range{info.guest_address, info.guest_size};
        if (std::find(g_seen.begin(), g_seen.end(), range) == g_seen.end()) {
            g_seen.push_back(range);
        }
    }
    if (g_logged.fetch_add(1, std::memory_order_relaxed) < 32) {
        LOG_INFO(Render_Vulkan, "TexReplace: {}x{} texture at {:#x} ({} bytes, vk format {}) key {:016x}: {}",
                 info.size.width, info.size.height, info.guest_address, info.guest_size,
                 static_cast<u32>(info.pixel_format), key,
                 found ? fmt::format("{} pictures applied", found->patches.size()) : "no match");
    }
    return found;
}

bool AutoSwitch() {
    return g_flag && g_auto;
}

bool PicturesActive() {
    return !g_gamepad.load(std::memory_order_relaxed);
}

void NoteInputDevice(bool gamepad) {
    if (g_gamepad.load(std::memory_order_relaxed) == gamepad ||
        g_gamepad.exchange(gamepad, std::memory_order_relaxed) == gamepad) {
        return;
    }
    std::vector<std::pair<VAddr, u32>> ranges;
    {
        std::scoped_lock lk{g_seen_mutex};
        ranges = g_seen;
    }
    LOG_INFO(Render_Vulkan, "TexReplace: {} input - {} glyphs ({} textures re-uploaded)",
             gamepad ? "pad" : "keyboard / mouse", gamepad ? "game's own" : "replaced",
             ranges.size());
    if (ranges.empty()) {
        return;
    }
    // On a thread of its own, like a game thread's CPU write (Opus review 10-18): the invalidation
    // may wait on the command processor (a GPU-written range), and the caller is the SDL thread,
    // which the present thread can wait on (ImGui IME callback) - a cycle. Nobody waits on this one.
    std::thread([ranges = std::move(ranges)] {
        for (const auto& [addr, size] : ranges) {
            Core::Memory::Instance()->InvalidateMemory(addr, size);
        }
    }).detach();
}

bool SwapRedBlue(const ImageInfo& info) {
    switch (info.pixel_format) {
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
    case vk::Format::eB8G8R8A8Snorm:
    case vk::Format::eB8G8R8A8Uint:
        return true;
    default:
        return false;
    }
}

// ---- 2026-10-31: texture dump and BC encoding -------------------------------------------------

namespace {

const bool g_dump = [] {
    if (const char* v = std::getenv("SHADPS4_TEX_DUMP"); v && v[0] == '1') {
        return true;
    }
    std::error_code ec;
    const auto flag = ExeDir() / "tex_dump" / "dump_next_run.txt";
    if (std::filesystem::exists(flag, ec)) {
        std::filesystem::path used = flag;
        used.replace_extension(".used");
        std::filesystem::remove(used, ec);
        std::filesystem::rename(flag, used, ec);
        return true;
    }
    return false;
}();
std::mutex g_dump_mutex;
std::vector<u64> g_dumped;
constexpr size_t MaxDumps = 8000;

u32 Crc32(const u8* p, size_t n, u32 crc = 0xffffffffu) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

void Be32(std::vector<u8>& v, u32 x) {
    for (int s = 24; s >= 0; s -= 8) {
        v.push_back(u8(x >> s));
    }
}

void PngChunk(std::vector<u8>& out, const char* type, const std::vector<u8>& data) {
    Be32(out, u32(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    Be32(out, Crc32(out.data() + start, out.size() - start) ^ 0xffffffffu);
}

/// RGBA8 PNG with stored (uncompressed) deflate blocks - no zlib needed.
std::vector<u8> MakePng(const u8* rgba, u32 w, u32 h) {
    std::vector<u8> raw;
    raw.reserve(size_t(w * 4 + 1) * h);
    for (u32 y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + size_t(y) * w * 4, rgba + size_t(y + 1) * w * 4);
    }
    std::vector<u8> z{0x78, 0x01};
    for (size_t pos = 0; pos < raw.size() || raw.empty();) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n >= raw.size() ? 1 : 0);
        z.push_back(u8(n));
        z.push_back(u8(n >> 8));
        z.push_back(u8(~n));
        z.push_back(u8(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (raw.empty()) {
            break;
        }
    }
    u32 a = 1, b = 0;
    for (const u8 x : raw) {
        a = (a + x) % 65521;
        b = (b + a) % 65521;
    }
    Be32(z, (b << 16) | a);
    std::vector<u8> out{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::vector<u8> ihdr;
    Be32(ihdr, w);
    Be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    PngChunk(out, "IHDR", ihdr);
    PngChunk(out, "IDAT", z);
    PngChunk(out, "IEND", {});
    return out;
}

u32 DxgiFormat(vk::Format f) {
    switch (f) {
    case vk::Format::eBc1RgbaUnormBlock:
    case vk::Format::eBc1RgbUnormBlock:
        return 71;
    case vk::Format::eBc1RgbaSrgbBlock:
    case vk::Format::eBc1RgbSrgbBlock:
        return 72;
    case vk::Format::eBc2UnormBlock:
        return 74;
    case vk::Format::eBc2SrgbBlock:
        return 75;
    case vk::Format::eBc3UnormBlock:
        return 77;
    case vk::Format::eBc3SrgbBlock:
        return 78;
    case vk::Format::eBc4UnormBlock:
        return 80;
    case vk::Format::eBc4SnormBlock:
        return 81;
    case vk::Format::eBc5UnormBlock:
        return 83;
    case vk::Format::eBc5SnormBlock:
        return 84;
    case vk::Format::eBc6HUfloatBlock:
        return 95;
    case vk::Format::eBc6HSfloatBlock:
        return 96;
    case vk::Format::eBc7UnormBlock:
        return 98;
    case vk::Format::eBc7SrgbBlock:
        return 99;
    default:
        return 0;
    }
}

u32 AnyBlockBytes(vk::Format f) {
    switch (DxgiFormat(f)) {
    case 71:
    case 72:
    case 80:
    case 81:
        return 8;
    case 0:
        return 0;
    default:
        return 16;
    }
}

void Le32(std::vector<u8>& v, u32 x) {
    for (int s = 0; s < 32; s += 8) {
        v.push_back(u8(x >> s));
    }
}

std::vector<u8> MakeDds(u32 dxgi, u32 w, u32 h, const u8* data, size_t size) {
    std::vector<u8> out{'D', 'D', 'S', ' '};
    Le32(out, 124);
    Le32(out, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000); // caps height width pixelformat linearsize
    Le32(out, h);
    Le32(out, w);
    Le32(out, u32(size));
    Le32(out, 0);
    Le32(out, 1);
    for (int i = 0; i < 11; ++i) {
        Le32(out, 0);
    }
    Le32(out, 32);
    Le32(out, 0x4); // FOURCC
    out.insert(out.end(), {'D', 'X', '1', '0'});
    for (int i = 0; i < 5; ++i) {
        Le32(out, 0);
    }
    Le32(out, 0x1000);
    for (int i = 0; i < 4; ++i) {
        Le32(out, 0);
    }
    Le32(out, dxgi);
    Le32(out, 3); // TEXTURE2D
    Le32(out, 0);
    Le32(out, 1);
    Le32(out, 0);
    out.insert(out.end(), data, data + size);
    return out;
}

// -- encoders: simple bounding-box fits, good enough for flat UI-like pictures --

int Dist(const int* a, const int* b, int n) {
    int d = 0;
    for (int i = 0; i < n; ++i) {
        d += (a[i] - b[i]) * (a[i] - b[i]);
    }
    return d;
}

u16 To565(const int* c) {
    return u16(((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 63 + 127) / 255) << 5 |
               ((c[2] * 31 + 127) / 255));
}

void From565(u16 v, int* c) {
    c[0] = ((v >> 11) & 31) * 255 / 31;
    c[1] = ((v >> 5) & 63) * 255 / 63;
    c[2] = (v & 31) * 255 / 31;
}

/// px: 16 RGBA pixels. alpha_mode: 3-colour + transparent when some alpha < 128.
void EncodeBc1Color(const u8* px, bool punch, u8* out) {
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    bool any_transparent = false;
    for (int i = 0; i < 16; ++i) {
        if (punch && px[i * 4 + 3] < 128) {
            any_transparent = true;
            continue;
        }
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min<int>(lo[k], px[i * 4 + k]);
            hi[k] = std::max<int>(hi[k], px[i * 4 + k]);
        }
    }
    if (lo[0] > hi[0]) {
        lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = 0;
    }
    u16 c0 = To565(hi), c1 = To565(lo);
    int pal[4][3];
    if (any_transparent) {
        if (c0 > c1) {
            std::swap(c0, c1);
        }
        From565(c0, pal[0]);
        From565(c1, pal[1]);
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = (pal[0][k] + pal[1][k]) / 2;
            pal[3][k] = 0;
        }
    } else {
        if (c0 < c1) {
            std::swap(c0, c1);
        }
        if (c0 == c1) {
            c0 == 0xffff ? --c1 : ++c0; // keep 4-colour mode
            if (c0 < c1) {
                std::swap(c0, c1);
            }
        }
        From565(c0, pal[0]);
        From565(c1, pal[1]);
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
            pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
        }
    }
    u32 idx = 0;
    for (int i = 0; i < 16; ++i) {
        u32 best = 0;
        if (any_transparent && px[i * 4 + 3] < 128) {
            best = 3;
        } else {
            const int c[3] = {px[i * 4], px[i * 4 + 1], px[i * 4 + 2]};
            int bd = 1 << 30;
            for (u32 p = 0; p < (any_transparent ? 3u : 4u); ++p) {
                const int d = Dist(c, pal[p], 3);
                if (d < bd) {
                    bd = d;
                    best = p;
                }
            }
        }
        idx |= best << (2 * i);
    }
    std::memcpy(out, &c0, 2);
    std::memcpy(out + 2, &c1, 2);
    std::memcpy(out + 4, &idx, 4);
}

void EncodeBc4(const u8* px, int ch, u8* out) {
    int a0 = 0, a1 = 255;
    for (int i = 0; i < 16; ++i) {
        a0 = std::max<int>(a0, px[i * 4 + ch]);
        a1 = std::min<int>(a1, px[i * 4 + ch]);
    }
    if (a0 == a1) {
        a0 == 255 ? --a1 : ++a0;
    }
    int pal[8] = {a0, a1};
    for (int k = 1; k < 7; ++k) {
        pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
    }
    u64 bits = u64(a0) | u64(a1) << 8;
    for (int i = 0; i < 16; ++i) {
        int best = 0, bd = 1 << 30;
        for (int p = 0; p < 8; ++p) {
            const int d = std::abs(px[i * 4 + ch] - pal[p]);
            if (d < bd) {
                bd = d;
                best = p;
            }
        }
        bits |= u64(best) << (16 + 3 * i);
    }
    std::memcpy(out, &bits, 8);
}

/// BC7 mode 6: one subset, RGBA 7.7.7.7 + p-bit endpoints, 4-bit indices.
void EncodeBc7(const u8* px, u8* out) {
    static constexpr int W[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    int lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        for (int k = 0; k < 4; ++k) {
            lo[k] = std::min<int>(lo[k], px[i * 4 + k]);
            hi[k] = std::max<int>(hi[k], px[i * 4 + k]);
        }
    }
    // endpoints with p-bit: value = (v7 << 1) | p, same p for all channels of an endpoint
    int e[2][4], q[2][4], p[2];
    for (int n = 0; n < 2; ++n) {
        const int* src = n == 0 ? lo : hi;
        int best_err = 1 << 30;
        for (int pb = 0; pb < 2; ++pb) {
            int err = 0, tq[4];
            for (int k = 0; k < 4; ++k) {
                tq[k] = std::clamp((src[k] - pb + 1) / 2, 0, 127);
                err += std::abs(((tq[k] << 1) | pb) - src[k]);
            }
            if (err < best_err) {
                best_err = err;
                p[n] = pb;
                std::memcpy(q[n], tq, sizeof(tq));
            }
        }
        for (int k = 0; k < 4; ++k) {
            e[n][k] = (q[n][k] << 1) | p[n];
        }
    }
    int idx[16];
    for (int i = 0; i < 16; ++i) {
        int bd = 1 << 30;
        for (int j = 0; j < 16; ++j) {
            int c[4];
            for (int k = 0; k < 4; ++k) {
                c[k] = ((64 - W[j]) * e[0][k] + W[j] * e[1][k] + 32) >> 6;
            }
            const int px_c[4] = {px[i * 4], px[i * 4 + 1], px[i * 4 + 2], px[i * 4 + 3]};
            const int d = Dist(px_c, c, 4);
            if (d < bd) {
                bd = d;
                idx[i] = j;
            }
        }
    }
    if (idx[0] >= 8) { // anchor index must have its top bit clear: swap the endpoints
        std::swap(q[0], q[1]);
        std::swap(p[0], p[1]);
        for (int& i : idx) {
            i = 15 - i;
        }
    }
    u64 w0 = 0, w1 = 0;
    u32 bit = 0;
    const auto put = [&](u64 v, u32 n) {
        for (u32 b = 0; b < n; ++b, ++bit) {
            const u64 x = (v >> b) & 1;
            if (bit < 64) {
                w0 |= x << bit;
            } else {
                w1 |= x << (bit - 64);
            }
        }
    };
    put(1ull << 6, 7); // mode 6
    for (int k = 0; k < 4; ++k) {
        put(u64(q[0][k]), 7);
        put(u64(q[1][k]), 7);
    }
    put(u64(p[0]), 1);
    put(u64(p[1]), 1);
    put(u64(idx[0]), 3);
    for (int i = 1; i < 16; ++i) {
        put(u64(idx[i]), 4);
    }
    std::memcpy(out, &w0, 8);
    std::memcpy(out + 8, &w1, 8);
}

} // Anonymous namespace

bool DumpEnabled() {
    return g_dump;
}

bool NoteForDump(const ImageInfo& info, u64& key) {
    if (info.guest_address == 0 || info.guest_size == 0 || info.size.width < 4 ||
        info.size.height < 4 || DumpSize(info) == 0) {
        return false;
    }
    key = XXH3_64bits(reinterpret_cast<const void*>(info.guest_address), info.guest_size);
    std::scoped_lock lk{g_dump_mutex};
    if (g_dumped.size() >= MaxDumps || std::find(g_dumped.begin(), g_dumped.end(), key) != g_dumped.end()) {
        return false;
    }
    g_dumped.push_back(key);
    return true;
}

u32 DumpSize(const ImageInfo& info) {
    if (info.num_samples != 1 || info.type != AmdGpu::ImageType::Color2D || info.props.is_depth) {
        return 0;
    }
    const u32 w = info.size.width, h = info.size.height;
    if (info.props.is_block) {
        const u32 bb = AnyBlockBytes(info.pixel_format);
        return bb ? ((w + 3) / 4) * ((h + 3) / 4) * bb : 0;
    }
    return info.num_bits == 32 ? w * h * 4 : 0;
}

void WriteDump(const ImageInfo& info, u64 key, const u8* data, size_t size) {
    std::error_code ec;
    const auto dir = ExeDir() / "tex_dump";
    std::filesystem::create_directories(dir, ec);
    const u32 w = info.size.width, h = info.size.height;
    std::vector<u8> file;
    std::string ext;
    if (info.props.is_block) {
        file = MakeDds(DxgiFormat(info.pixel_format), w, h, data, size);
        ext = "dds";
    } else {
        std::vector<u8> rgba(data, data + size);
        if (SwapRedBlue(info)) {
            for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
                std::swap(rgba[i], rgba[i + 2]);
            }
        }
        file = MakePng(rgba.data(), w, h);
        ext = "png";
    }
    const auto path = dir / fmt::format("{:016x}_{}x{}_{}_m{}.{}", key, w, h,
                                        vk::to_string(info.pixel_format), info.resources.levels, ext);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(file.data()),
                                                std::streamsize(file.size()));
}

u32 BlockBytes(const ImageInfo& info) {
    switch (DxgiFormat(info.pixel_format)) {
    case 71:
    case 72:
        return 8;
    case 74:
    case 75:
    case 77:
    case 78:
    case 98:
    case 99:
        return 16;
    default:
        return 0;
    }
}

std::vector<u8> EncodeBlocks(const ImageInfo& info, const u8* rgba, u32 w, u32 h) {
    const u32 dxgi = DxgiFormat(info.pixel_format);
    const u32 bb = BlockBytes(info);
    std::vector<u8> out(size_t(w / 4) * (h / 4) * bb);
    u8 px[64];
    for (u32 by = 0; by < h / 4; ++by) {
        for (u32 bx = 0; bx < w / 4; ++bx) {
            for (u32 r = 0; r < 4; ++r) {
                std::memcpy(px + r * 16, rgba + (size_t(by * 4 + r) * w + bx * 4) * 4, 16);
            }
            u8* o = out.data() + (size_t(by) * (w / 4) + bx) * bb;
            switch (dxgi) {
            case 71:
            case 72:
                EncodeBc1Color(px, info.pixel_format == vk::Format::eBc1RgbaUnormBlock ||
                                       info.pixel_format == vk::Format::eBc1RgbaSrgbBlock,
                               o);
                break;
            case 74:
            case 75: {
                u64 a = 0;
                for (int i = 0; i < 16; ++i) {
                    a |= u64((px[i * 4 + 3] * 15 + 127) / 255) << (4 * i);
                }
                std::memcpy(o, &a, 8);
                EncodeBc1Color(px, false, o + 8);
                break;
            }
            case 77:
            case 78:
                EncodeBc4(px, 3, o);
                EncodeBc1Color(px, false, o + 8);
                break;
            default:
                EncodeBc7(px, o);
                break;
            }
        }
    }
    return out;
}

} // namespace VideoCore::TexReplace
