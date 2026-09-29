// Finds X4 code by its bytes instead of by address, so one build works on every X4 version whose
// code at these spots is unchanged, wherever the linker placed it. "??" is a wildcard byte, used
// for RIP-relative displacements, which change with every relink. Struct offsets and jumps stay
// literal: if they differ, the code around them changed and the mod leaves it alone.
// Adding an X4 version: run code_scan_tests on its X4.exe; for each MISSING entry, find the same
// code in that version (same instructions, other offsets) and add a row with its bytes and the
// struct offsets they use. Then check it in the headset.
#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string_view>
#include <vector>

namespace x4vr::code {
using Pattern = std::vector<int>; // byte value, or -1 for a wildcard

inline Pattern parse(std::string_view text) {
    Pattern bytes;
    for (size_t i = 0; i+1 < text.size(); i += 3) {
        const auto nibble = [](char c) { return c <= '9' ? c-'0' : (c|0x20)-'a'+10; };
        bytes.push_back(text[i] == '?' ? -1 : nibble(text[i])*16+nibble(text[i+1]));
    }
    return bytes;
}
inline bool matches(const void* code, const Pattern& pattern) {
    const auto* p = static_cast<const unsigned char*>(code);
    for (size_t i = 0; i < pattern.size(); ++i) if (pattern[i] >= 0 && p[i] != pattern[i]) return false;
    return true;
}
// Every match in the executable sections of the PE image mapped at `base`.
inline std::vector<const unsigned char*> find_all(const void* base, const Pattern& pattern) {
    // Search for the longest literal run, then check the whole pattern around each hit.
    size_t anchor = 0, length = 0;
    for (size_t i = 0; i < pattern.size();) {
        size_t j = i;
        while (j < pattern.size() && pattern[j] >= 0) ++j;
        if (j-i > length) anchor = i, length = j-i;
        i = j+1;
    }
    std::vector<unsigned char> literal;
    for (size_t i = anchor; i < anchor+length; ++i) literal.push_back(static_cast<unsigned char>(pattern[i]));
    const std::boyer_moore_horspool_searcher searcher(literal.begin(), literal.end());
    std::vector<const unsigned char*> found;
    const auto* image = static_cast<const unsigned char*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image+reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const auto* begin = image+section->VirtualAddress;
        const auto* end = begin+(std::min)(section->Misc.VirtualSize, section->SizeOfRawData);
        for (auto hit = begin; (hit = std::search(hit, end, searcher)) != end; ++hit) {
            if (size_t(hit-begin) >= anchor && size_t(end-hit)+anchor >= pattern.size() && matches(hit-anchor, pattern))
                found.push_back(hit-anchor);
        }
    }
    return found;
}
// The only match, or nullptr for none or several.
inline const unsigned char* find_unique(const void* base, const Pattern& pattern) {
    const auto found = find_all(base, pattern);
    return found.size() == 1 ? found.front() : nullptr;
}
// Address a RIP-relative operand refers to: the instruction at `code` is `length` bytes long
// with its 32-bit displacement at `disp_at`.
inline const unsigned char* rip_target(const unsigned char* code, size_t disp_at, size_t length) {
    int32_t disp;
    std::memcpy(&disp, code+disp_at, sizeof(disp));
    return code+length+disp;
}

// X4 code the mod relies on. Where versions differ, each row is one X4 version's code plus the
// struct layout that code implies (addresses in comments are that version's).
// tests/code_scan_tests.cpp checks them against any X4.exe. To add a version, see its header.
namespace x4 {
// Head-tracker bridge (9.00: 0x9fdb3e, 8.00: 0x9bebd0): zeroes backward head position; `jae` at +15.
inline constexpr std::string_view backward_clamp = "f3 0f 10 45 67 0f 57 05 ?? ?? ?? ?? 0f 2f c6 73 04 44 89 65 67";
// Readers of the per-frame half global (9.00: 0x77a47f and 4 more; 8.00: 4): `movsxd rax, [half];
// xor rax, 1; imul rcx, rax, 0x270`. All must name the same global.
inline constexpr std::string_view frame_half = "48 63 05 ?? ?? ?? ?? 48 83 f0 01 48 69 c8 70 02 00 00";

struct OnFoot {
    const char* version;
    // Head-tracker bridge: zero pose without a ship; `je` at +101. Starts with `mov rcx, [camera manager]`.
    std::string_view zeroing;
    // Camera::GetOffset: exits without a movement controller; `je` displacement at +7 (0x18a
    // reaches the head-offset block in both versions). `mov rcx, [camera manager]` at +11.
    std::string_view offset;
    size_t camera_mode; // Camera field compared at zeroing+24: 0 while walking
};
inline constexpr OnFoot on_foot[] = {
    {"9.00", // 0x9fd9ae, 0x97a413
     "48 8b 0d ?? ?? ?? ?? 48 85 c9 74 19 48 8b 81 d0 03 00 00 48 85 c0 74 0d 44 39 a0 68 08 00 00 0f 85 bb 00 00 00 "
     "f7 05 ?? ?? ?? ?? 00 04 00 00 0f 85 ab 00 00 00 48 85 c9 48 8d 81 30 02 00 00 75 07 48 8d 05 ?? ?? ?? ?? 48 8b 10 "
     "48 85 d2 0f 84 8c 00 00 00 8b 05 ?? ?? ?? ?? 39 82 b8 6a 00 00 0f 94 c0 84 c0 74 79",
     "48 83 7e 20 00 0f 84 76 02 00 00 48 8b 0d ?? ?? ?? ?? 48 8d 91 30 02 00 00 48 85 c9 75 07 48 8d 15 ?? ?? ?? ?? "
     "48 8b 86 70 07 00 00 48 39 02 74 15 48 85 c9 74 09 48 8b 81 d0 03 00 00",
     0x868},
    {"8.00", // 0x9be9fe, 0x947230
     "48 8b 0d ?? ?? ?? ?? 48 85 c9 74 19 48 8b 81 d0 03 00 00 48 85 c0 74 0d 44 39 a0 78 08 00 00 0f 85 bb 00 00 00 "
     "f7 05 ?? ?? ?? ?? 00 04 00 00 0f 85 ab 00 00 00 48 85 c9 48 8d 81 30 02 00 00 75 07 48 8d 05 ?? ?? ?? ?? 48 8b 10 "
     "48 85 d2 0f 84 8c 00 00 00 8b 05 ?? ?? ?? ?? 39 82 b8 6a 00 00 0f 94 c0 84 c0 74 79",
     "48 83 7e 20 00 0f 84 6e 02 00 00 48 8b 0d ?? ?? ?? ?? 48 8d 91 30 02 00 00 48 85 c9 75 07 48 8d 15 ?? ?? ?? ?? "
     "48 8b 86 80 07 00 00 48 39 02 74 15 48 85 c9 74 09 48 8b 81 d0 03 00 00",
     0x878},
};

// X4's FreeTrack tracker (RTTI VR::FreeTrack).
struct Tracker {
    const char* version;
    // Update: `movss xmm0, [gain]`, then position = xyz * [scale] * gain.
    std::string_view update;
    std::string_view position; // vtable slot: position accessor, reads [position]
    std::string_view still;    // vtable slot: still check, `cmp [counter], 30; setae al; ret`
    size_t position_slot, still_slot; // vtable byte offsets
    size_t data;       // FreeTrackData inside the tracker, passed to FTGetData
    size_t get_data;   // the FTGetData pointer
    size_t position_field, scale_field;
};
inline constexpr Tracker trackers[] = {
    {"9.00", // 0xf37583, 0xf377b0, 0xf376c0
     "f3 0f 10 05 ?? ?? ?? ?? 0f 16 f2 0f 11 b3 d0 00 00 00 f3 0f 10 a3 04 01 00 00 0f 28 d4 0f 28 dc f3 0f 59 63 48 "
     "f3 0f 59 5b 4c f3 0f 59 53 50 f3 0f 59 e0 f3 0f 59 d8 f3 0f 59 d0 0f 28 ec 0f 14 eb 0f 16 ea 0f 11 ab a0 00 00 00",
     "0f 10 81 c0 00 00 00 48 8d 44 24 08 f3 0f 10 15 ?? ?? ?? ?? f3 0f 10 1d ?? ?? ?? ?? f3 0f 59 c2",
     "83 b9 9c 00 00 00 1e 0f 93 c0 c3",
     0x108, 0x28, 0x30, 0x10, 0xc0, 0x104},
    {"8.00", // 0xedaa65, 0xedaca0, 0xedabb0
     "f3 0f 10 05 ?? ?? ?? ?? 0f 16 f2 0f 11 b3 10 01 00 00 f3 0f 10 a3 44 01 00 00 0f 28 d4 0f 28 dc "
     "f3 0f 59 a3 80 00 00 00 f3 0f 59 9b 84 00 00 00 f3 0f 59 93 88 00 00 00 f3 0f 59 e0 f3 0f 59 d8 f3 0f 59 d0 "
     "0f 28 ec 0f 14 eb 0f 16 ea 0f 11 ab e0 00 00 00",
     "0f 10 81 00 01 00 00 48 8d 44 24 08 f3 0f 10 15 ?? ?? ?? ?? f3 0f 10 1d ?? ?? ?? ?? f3 0f 59 c2",
     "83 b9 d4 00 00 00 1e 0f 93 c0 c3",
     0x1b8, 0x28, 0x68, 0x48, 0x100, 0x144},
};
}
}
