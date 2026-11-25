#pragma once

#include <cstdint>

// Static (read-only) resolution of the game's ray scene object, the value the engine's
// setup_ray() writes to setup + 0x1E8. No stack capture and no template file:
//
//   raw220 = *(uint64_t*)(context + 0x220)
//   sub60  = DecodeRaySceneSub60(raw220)
//   base10 = sub60 - 0x50
//   holder = *(uint64_t*)(base10 + (*(uint8_t*)(base10 + 0x200) ? 0x1F8 : 0x08))
//   setup1e8 = holder ? *(uint64_t*)holder : 0
//
// CN and Nexon share one decode; World 155284 uses a different five-stage decode.
namespace OW::RayScene {

enum class Decode : uint8_t {
    CnNexon = 0,
    World155284,
};

inline uint64_t Rotr(uint64_t value, unsigned bits) {
    bits &= 63;
    if (!bits) return value;
    return (value >> bits) | (value << (64 - bits));
}

inline uint64_t DecodeSub60(Decode decode, uint64_t raw220) {
    if (decode == Decode::CnNexon) {
        // std::rotr((raw - K1) ^ K2, 38)
        return Rotr(((raw220 - 0x1E3575AB0556DA32ull) ^ 0x6EAE2B9069511EFCull), 38);
    }
    // World 155284, verified against encoder 0x552CC0/0x545CC0 and decoder
    // 0x54EFC5..0x54F098/0x5458F0.
    uint64_t v = (raw220 ^ 0x6F711D30A7FE2D51ull) - 0x123200E890EC6BADull;
    v = (v ^ 0xE61D2BCBEA47262Bull) + 0x72DA6B151FEEA273ull;
    v = Rotr(v, 42) ^ 0xC6ull;
    v = (v + 0x56B75F960EFC208Full) ^ 0x5C23DB9136D11647ull;
    return Rotr(v, 39);
}

struct Chain {
    bool ok = false;
    bool vtableOk = false;
    uint64_t raw220 = 0;
    uint64_t sub60 = 0;
    uint64_t base10 = 0;
    uint64_t holder = 0;
    uint64_t setup1e8 = 0;
    uint8_t flag = 0;
};

// Read(address, buffer, size) -> bool. Returns false when any link is unreadable or the
// optional vtable check fails; a partial Chain is still populated for diagnostics.
template <typename Read>
bool Resolve(uint64_t context, uint64_t imageBase, Decode decode, uint64_t sub60VtableRva,
             Read&& read, Chain& out) {
    out = Chain{};
    if (!context) return false;
    if (!read(context + 0x220, &out.raw220, sizeof(out.raw220))) return false;
    out.sub60 = DecodeSub60(decode, out.raw220);
    if (!out.sub60) return false;
    if (sub60VtableRva) {
        uint64_t vtable = 0;
        if (!read(out.sub60, &vtable, sizeof(vtable))) return false;
        if (vtable != imageBase + sub60VtableRva) return false;
        out.vtableOk = true;
    }
    out.base10 = out.sub60 - 0x50;
    if (!read(out.base10 + 0x200, &out.flag, sizeof(out.flag))) return false;
    const uint64_t holderAddress = out.base10 + (out.flag ? 0x1F8 : 0x08);
    if (!read(holderAddress, &out.holder, sizeof(out.holder)) || !out.holder) return false;
    if (!read(out.holder, &out.setup1e8, sizeof(out.setup1e8)) || !out.setup1e8) return false;
    out.ok = true;
    return true;
}

}  // namespace OW::RayScene
