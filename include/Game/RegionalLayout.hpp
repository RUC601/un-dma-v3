#pragma once

#include <bit>
#include <cstdint>

namespace OW::RegionalLayout {
namespace World155284 {
// Native D0/D4/D8 map cores: C70716/C71016/C71916. Parent also checked
// independently at 544E6F; the getter folds its final add with the +0x28 field.
// Evidence: 03_DOWNP/202611060630/context-map-regions-20261116/static/.
constexpr uint64_t DecodeMapParent(uint64_t raw)
{
    return (std::rotr(raw + 0x1FAE62CADBFE9389ULL, 21) ^ 0x80138B3C582F8286ULL) +
           0x667BE2C557C22212ULL;
}
constexpr uint64_t DecodeMapOwner(uint64_t raw)
{
    return ((raw + 0x385FD7DE851C1CB4ULL) ^ 0xAEC84AE8B395B4ABULL) +
           0x615B0D8D079DE55BULL;
}
constexpr uint64_t DecodeMapBuckets(uint64_t raw)
{
    return std::rotr((std::rotr(raw, 44) ^ 0xFA03FD84B50A0BBDULL) +
                     0x2440492FD3A36FA1ULL, 42);
}

constexpr uint64_t DecodeComponent(uint64_t value, uint64_t key, uint8_t byteKey)
{
    value = (std::rotr(value, 33) ^ key) + 0x061365BA92613D6EULL;
    return (std::rotr(value ^ byteKey, 3) + 0x4ACD89886EC2D109ULL) ^ 0xDE342D5317F56B5DULL;
}

constexpr uint64_t DecodeGlobalAdmin(uint64_t value)
{
    value = (std::rotr(value, 3) ^ 0x6B8F56BF98CC7D09ULL) + 0xCB85DE2B94C4D6FDULL;
    return std::rotr(value, 63) + 0x130305262FA1386AULL;
}

constexpr uint64_t DecodeGameAdmin(uint64_t value)
{
    value = std::rotr(value ^ 0xBC4ECC99AD9EC98DULL, 57) + 0x5ED15F31DC8F9139ULL;
    return std::rotr(value, 48) + 0x7AB10D2DB096D11AULL;
}

// Known-answer pair (live World 155284, 2026-11-25): stored qword -> context.
// The value came from [ClientGame + 0x30] and was identical to a captured
// `setup + 0x1F0` on the same round.
static_assert(DecodeGameAdmin(0x061366D597B1AD38ULL) == 0x16F9CAD5EC0ULL);
}

namespace Cn155284 {
// CN 155284N: ClientGame + 0x30 holds the level context pointer.
// Encrypt site 0x5603F7, decode site 0x56035C, both verified against the live value.
// Evidence: 03_DOWNP/202611060630/context-source-20261125/.
constexpr uint64_t DecodeGameAdmin(uint64_t value)
{
    value = std::rotr(value - 0x5026DD063BC2B35FULL, 48);
    value ^= 0xE729C3158E263901ULL;
    return std::rotr(value, 21) + 0xF2445640CC14CEF6ULL;
}

// Inverse of DecodeGameAdmin: the site encoder at 0x5603D9..0x5603FA.
constexpr uint64_t EncodeGameAdmin(uint64_t value)
{
    value = std::rotr(value + 0x0DBBA9BF33EB310AULL, 43) ^ 0xE729C3158E263901ULL;
    return std::rotr(value, 16) + 0x5026DD063BC2B35FULL;
}

// Known-answer pairs (live CN 155284N, 2026-11-25): plaintext object -> stored qword.
static_assert(DecodeGameAdmin(0xDE9B5F624DF5AAC6ULL) == 0x16C7A9E0AC0ULL);
static_assert(DecodeGameAdmin(0xDE9B5F4A1283AAC6ULL) == 0x16C1CC37AC0ULL);
static_assert(EncodeGameAdmin(0x16C1CC37AC0ULL) == 0xDE9B5F4A1283AAC6ULL);
// Nexon 155284KB uses the same transform with its own ClientGame global and context
// vtable (live 2026-11-25, base + 0x409B580 -> + 0x30).
static_assert(DecodeGameAdmin(0xDE9B6FDA30251AC6ULL) == 0x1F09DD08E40ULL);
}
}
