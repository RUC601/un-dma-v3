#pragma once
#include "game/entities/RegistryResolver.hpp"
#include "Game/CalibrationPolicy.hpp"
#include "Game/RegionalLayout.hpp"
#include "Game/SkillRead.hpp"
#include "memory/PeSections.hpp"
#include "Utils/RegionEnvironment.hpp"
#include <Windows.h>
#include <coroutine>
#include <exception>
#include <intrin.h>
#include <intsafe.h>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <type_traits>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <emmintrin.h>

#ifndef HIDWORD
#define HIDWORD(_ui64) ((DWORD)(((DWORDLONG)(_ui64) >> 32) & 0xFFFFFFFF))
#endif
#ifndef LODWORD
#define LODWORD(_ui64) ((DWORD)((_ui64) & 0xFFFFFFFF))
#endif
#ifndef __ROL4__
#define __ROL4__(x, n) _rotl(x, n)
#endif

#include "Game/Structs.hpp"
#include "Game/Offsets.hpp"
#include "Game/SDK.hpp"
#include "Game/RuntimeState.hpp"
#include "Game/EntityRosterPolicy.hpp"
#include "Game/WorldObjectIds.hpp"
#include "Utils/ProcessConnection.hpp"
#include "Utils/Diagnostics.hpp"
#include "Utils/RuntimeTrace.hpp"

namespace OW {

    static inline uint64_t ROR64(uint64_t x, int bits) {
        bits &= 63;
        if (bits == 0) return x;
        return (x >> bits) | (x << (64 - bits));
    }

    static inline uint64_t ROL64(uint64_t x, int bits) {
        bits &= 63;
        if (bits == 0) return x;
        return (x << bits) | (x >> (64 - bits));
    }

    inline bool GetGlobalKey() {
        Diagnostics::SetKeyStatus(Diagnostics::KeyStatus::Resolving);
        Diagnostics::Info("GetGlobalKey resolution started.");

        auto markResolved = [](const char* method) {
            Diagnostics::SetKeyStatus(
                Diagnostics::KeyStatus::Resolved,
                SDK->GlobalKey1,
                SDK->GlobalKey2);
            Diagnostics::Info("GetGlobalKey succeeded via %s: key1=0x%llX key2=0x%llX",
                method,
                static_cast<unsigned long long>(SDK->GlobalKey1),
                static_cast<unsigned long long>(SDK->GlobalKey2));
            return true;
        };

        static const uint8_t key_sig[] =
            "\x00\x00\x00\x00\x21\x00\x00\x00\x00\x00\x00\x00\x24\x00\x00\x00"
            "\x01\x00\x00\x00\x29\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
            "\x00\x00\x00\x00\x00\x00";
        static const char* key_mask = "xxxxxxxxxxxxx?xxxxxxxxxxxxxxxxxxxxxxxx";

        int pattern_attempts = 0;
        while (RuntimeWorkersRunning() && ProcessConnection::IsConnected()) {
            if (pattern_attempts < 3) {
                uint64_t Key = SDK->FindPatternExReg(
                    reinterpret_cast<const uint8_t*>(key_sig),
                    key_mask,
                    0x100000
                );
                if (Key && Key < 0xF000000000000000 &&
                    SDK->RPM<uint64_t>(Key + 0x38) > 0x1000000000000000 &&
                    SDK->RPM<uint64_t>(Key + 0xB8) > 0x1000000000000000) {
                    SDK->GlobalKey1 = SDK->RPM<uint64_t>(Key + 0x38);
                    SDK->GlobalKey2 = SDK->RPM<uint64_t>(Key + 0xB8);
                    printf("[Decrypt] GlobalKey1: 0x%llX\n", SDK->GlobalKey1.load());
                    printf("[Decrypt] GlobalKey2: 0x%llX\n", SDK->GlobalKey2.load());
                    return markResolved("pattern scan");
                }
                pattern_attempts++;
            }

            {
                uint64_t gk_addr = SDK->dwGameBase + offset::GetGlobalKey_RVA;
                uint8_t code[128] = {};
                if (!SDK->read_buf(gk_addr, (char*)code, sizeof(code))) {
                    Diagnostics::Warn("GetGlobalKey instruction read failed at 0x%llX.",
                        static_cast<unsigned long long>(gk_addr));
                    Diagnostics::SetKeyStatus(Diagnostics::KeyStatus::Failed);
                    RuntimeWorkerSleep(2000);
                    continue;
                }

                uint64_t lea_target = 0;
                for (int i = 0; i < 120; i++) {
                    if (code[i] == 0x48 && code[i+1] == 0x8D &&
                        (code[i+2] & 0xC7) == 0x05) {
                        int32_t disp = *(int32_t*)&code[i+3];
                        lea_target = gk_addr + i + 7 + disp;
                        break;
                    }
                }

                uint64_t const1 = 0, const2 = 0, const3 = 0;
                for (int i = 0; i < 120; i++) {
                    if (code[i] == 0x48 && code[i+1] == 0xB9 && !const1) {
                        memcpy(&const1, code + i + 2, 8);
                        i += 9;
                    } else if (code[i] == 0x48 && code[i+1] == 0xB8) {
                        uint64_t val;
                        memcpy(&val, code + i + 2, 8);
                        if (!const2) { const2 = val; }
                        else if (!const3 && val != const2) { const3 = val; }
                        i += 9;
                    }
                }

                if (lea_target && const1 && const2 && const3) {
                    uint64_t decoded = const1 + ROR64(lea_target, 0x0A);
                    decoded ^= const2;
                    decoded -= const3;

                    printf("[Decrypt] IDA method: decoded struct ptr = 0x%llX (RVA 0x%llX)\n",
                           decoded, decoded - SDK->dwGameBase);

                    bool decoded_in_range = (decoded >= SDK->dwGameBase &&
                                             decoded < SDK->dwGameBase + 0x4000000);

                    if (decoded_in_range || decoded > 0x10000) {
                        uint64_t k1 = SDK->RPM<uint64_t>(decoded + 0x38);
                        uint64_t k2 = SDK->RPM<uint64_t>(decoded + 0xB8);

                        if (k1 > 0x1000000000000000 && k2 > 0x1000000000000000) {
                            SDK->GlobalKey1 = k1;
                            SDK->GlobalKey2 = k2;
                            printf("[Decrypt] GlobalKey1: 0x%llX (from decoded struct)\n", k1);
                            printf("[Decrypt] GlobalKey2: 0x%llX (from decoded struct)\n", k2);
                            return markResolved("decoded struct");
                        }

                        int found = 0;
                        for (int64_t off = -0x200; off <= 0x200 && found < 2; off += 8) {
                            uint64_t v = SDK->RPM<uint64_t>(decoded + off);
                            if (v > 0x1000000000000000) {
                                if (!SDK->GlobalKey1) { SDK->GlobalKey1 = v; found++; }
                                else if (!SDK->GlobalKey2 && v != SDK->GlobalKey1) { SDK->GlobalKey2 = v; found++; }
                            }
                        }
                        if (found >= 2) {
                            printf("[Decrypt] GlobalKey1: 0x%llX (scanned struct)\n", SDK->GlobalKey1.load());
                            printf("[Decrypt] GlobalKey2: 0x%llX (scanned struct)\n", SDK->GlobalKey2.load());
                            return markResolved("struct scan");
                        }
                        SDK->GlobalKey1 = SDK->GlobalKey2 = 0;
                    }

                    for (int i = 0; i < 120; i++) {
                        if (code[i] == 0x48 && code[i+1] == 0x89 && code[i+2] == 0x05) {
                            int32_t disp = *(int32_t*)&code[i+3];
                            uint64_t global_rva = (gk_addr + i + 7 + disp) - SDK->dwGameBase;
                            uint64_t global_val = SDK->RPM<uint64_t>(SDK->dwGameBase + global_rva);
                            printf("[Decrypt] Global store at RVA 0x%llX = 0x%llX\n",
                                   global_rva, global_val);

                            if (global_val > 0x1000000000000000) {
                                SDK->GlobalKey1 = global_val;
                                for (int64_t d = -0x1000; d <= 0x1000; d += 8) {
                                    if (d == 0) continue;
                                    uint64_t v = SDK->RPM<uint64_t>(SDK->dwGameBase + global_rva + d);
                                    if (v > 0x1000000000000000 && v != global_val) {
                                        SDK->GlobalKey2 = v;
                                        printf("[Decrypt] GlobalKey1: 0x%llX (global store)\n", SDK->GlobalKey1.load());
                                        printf("[Decrypt] GlobalKey2: 0x%llX (near global)\n", SDK->GlobalKey2.load());
                                        return markResolved("global store");
                                    }
                                }
                            }
                        }
                    }
                }
                SDK->GlobalKey1 = SDK->GlobalKey2 = 0;
            }

            Diagnostics::SetKeyStatus(Diagnostics::KeyStatus::Failed);
            Diagnostics::Warn("GetGlobalKey resolution attempt failed; retrying in 2s.");
            printf("[Decrypt] Key resolution failed, retrying in 2s...\n");
            RuntimeWorkerSleep(2000);
        }
        return false;
    }

    inline uint64_t GetParent(uint64_t encrypted) {
        if (offset::Active().componentTransform == offset::ComponentTransformMode::Cn155284)
            return Cn155284::Parent(encrypted);
        __try {
            auto result = encrypted;
            result = (result >> 0x20) | (result << 0x20);
            result ^= 0x4B920A7072A077C5;
            result -= 0x107816B001CA79C8;
            result = (result >> 0x23) | (result << 0x1D);
            result += 0xFD2150D0AEF24514;
            return result;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    inline void sub_E8D1A0(uint64_t* bit_mask, uint64_t* lower_mask,
                           uint32_t* shift, uint32_t* bucket,
                           uint32_t componentid) {
        *shift = componentid & 0x3F;
        *bit_mask = 1ull << *shift;
        *lower_mask = *bit_mask - 1;
        *bucket = componentid >> 6;
    }

    static constexpr uint64_t kEntityHeaderSnapshotOffset = 0x30;
    static constexpr size_t kEntityHeaderSnapshotSize = 0x120;

    struct EntityHeaderSnapshot {
        uintptr_t parent = 0;
        MemorySDK::ReadRange range{};
        bool valid = false;

        bool Read(uintptr_t value) {
            parent = value;
            valid = parent != 0 &&
                range.Read(parent + kEntityHeaderSnapshotOffset,
                           kEntityHeaderSnapshotSize);
            return valid;
        }

        bool ReadComponentLayout(std::array<uint64_t,6>& layout) const {
            uint32_t indices=0;
            if(!ReadParentOffset(0x80,layout[0]) || !ReadParentOffset(0x130,indices))return false;
            for(size_t bucket=0;bucket<4;++bucket)
                if(!ReadParentOffset(0x110+bucket*8,layout[1+bucket]))return false;
            layout[5]=indices;
            return layout[0]!=0;
        }

        template <typename T>
        bool ReadParentOffset(uint64_t offset, T& value) const {
            if (!valid) {
                value = {};
                return false;
            }
            return range.ReadAddress(parent + offset, value);
        }
    };

#ifdef UNLEASHED_HEALTH_CACHE_TEST
    inline thread_local uint64_t componentKeyTestTick = 0;
    inline thread_local void (*componentDecryptTestObserver)(uintptr_t,uint32_t,bool) = nullptr;
#endif
    struct Cn155284ComponentKeyCache {
        uint64_t base = 0, epoch = 0, sampled = 0;
        uint8_t a = 0, b = 0;
        bool valid = false;
    };

    inline Cn155284ComponentKeyCache& ThreadCn155284ComponentKeys() {
        static thread_local Cn155284ComponentKeyCache keys;
        return keys;
    }

    inline bool HasFreshCn155284ComponentKeys(uint8_t& outA, uint8_t& outB, uint64_t& outNow, uint64_t& outEpoch) {
#ifdef UNLEASHED_HEALTH_CACHE_TEST
        outNow = componentKeyTestTick;
#else
        outNow = GetTickCount64();
#endif
        outEpoch = ProcessConnection::ConnectionEpoch();
        const auto& keys = ThreadCn155284ComponentKeys();
        if (keys.valid && keys.base == SDK->dwGameBase && keys.epoch == outEpoch && outNow - keys.sampled < 16) {
            outA = keys.a;
            outB = keys.b;
            return true;
        }
        return false;
    }

    inline void StoreCn155284ComponentKeys(uint64_t now, uint64_t epoch, uint8_t a, uint8_t b) {
        ThreadCn155284ComponentKeys() = { SDK->dwGameBase, epoch, now, a, b, true };
    }

    inline void InvalidateCn155284ComponentKeys() {
        ThreadCn155284ComponentKeys().valid = false;
    }

    struct CnComponentSlotPlan {
        bool present = false;
        uint64_t entryAddress = 0;
        uint64_t presentMask = 0;
        uint32_t expectedType = 0;
    };

    inline CnComponentSlotPlan PlanCnComponentSlot(
        uintptr_t parent,
        uint32_t idx,
        const EntityHeaderSnapshot& parentSnapshot)
    {
        CnComponentSlotPlan plan{};
        if (!parent || !parentSnapshot.valid || parentSnapshot.parent != parent)
            return plan;

        uint64_t bit_mask = 0;
        uint64_t lower_mask = 0;
        uint32_t shift = 0;
        uint32_t bucket = 0;
        sub_E8D1A0(&bit_mask, &lower_mask, &shift, &bucket, idx);

        uint64_t component_bits = 0;
        if (!parentSnapshot.ReadParentOffset(8ull * bucket + 0x110, component_bits))
            return plan;
        const uint64_t present = (component_bits & bit_mask) >> shift;
        if (!present)
            return plan;

        uint64_t below = component_bits & lower_mask;
        below -= (below >> 1) & 0x5555555555555555ull;
        below = (below & 0x3333333333333333ull) +
                ((below >> 2) & 0x3333333333333333ull);
        below = (below + (below >> 4)) & 0x0F0F0F0F0F0F0F0Full;

        uint8_t component_index_base = 0;
        if (!parentSnapshot.ReadParentOffset(bucket + 0x130, component_index_base))
            return plan;

        const uint64_t component_index =
            component_index_base +
            ((below * 0x0101010101010101ull) >> 0x38);

        uint64_t component_table = 0;
        if (!parentSnapshot.ReadParentOffset(0x80, component_table))
            return plan;
        if (!component_table) {
            Diagnostics::RecordDecryptFailure();
            return plan;
        }

        plan.present = true;
        plan.entryAddress = component_table + 8ull * component_index;
        plan.presentMask = static_cast<uint64_t>(static_cast<int64_t>(-static_cast<int32_t>(present)));
        plan.expectedType = idx;
        return plan;
    }

    inline uintptr_t DecryptComponent(uintptr_t parent, uint32_t idx,
                                      const EntityHeaderSnapshot* parent_snapshot,
                                      bool requireExactReads = false) {
#ifdef UNLEASHED_HEALTH_CACHE_TEST
        if(componentDecryptTestObserver)componentDecryptTestObserver(parent,idx,requireExactReads);
#endif
        if (!parent)
            return 0;

        uint64_t bit_mask = 0;
        uint64_t lower_mask = 0;
        uint32_t shift = 0;
        uint32_t bucket = 0;
        sub_E8D1A0(&bit_mask, &lower_mask, &shift, &bucket, idx);

        auto readParent = [&](uint64_t offset, auto& value) {
            if (parent_snapshot &&
                parent_snapshot->parent == parent &&
                parent_snapshot->ReadParentOffset(offset, value)) {
                return true;
            }
            return SDK->read_range(parent + offset, &value, sizeof(value));
        };

        uint64_t component_bits = 0;
        if (!readParent(8ull * bucket + 0x110, component_bits))
            return 0;
        const uint64_t present = (component_bits & bit_mask) >> shift;
        if (!present)
            return 0;

        uint64_t below = component_bits & lower_mask;
        below -= (below >> 1) & 0x5555555555555555ull;
        below = (below & 0x3333333333333333ull) +
                ((below >> 2) & 0x3333333333333333ull);
        below = (below + (below >> 4)) & 0x0F0F0F0F0F0F0F0Full;

        uint8_t component_index_base = 0;
        if (!readParent(bucket + 0x130, component_index_base))
            return 0;

        const uint64_t component_index =
            component_index_base +
            ((below * 0x0101010101010101ull) >> 0x38);

        uint64_t component_table = 0;
        if (!readParent(0x80, component_table))
            return 0;
        if (!component_table) {
            Diagnostics::RecordDecryptFailure();
            return 0;
        }

        uint64_t component = 0;
        if (!SDK->read_range(component_table + 8ull * component_index, &component, sizeof(component)))
            return 0;
        if (!component) {
            Diagnostics::RecordDecryptFailure();
            return 0;
        }

        const uint64_t present_mask =
            static_cast<uint64_t>(static_cast<int64_t>(
                -static_cast<int32_t>(present)));
        const auto& activeOffsets = offset::Active();

        if (activeOffsets.componentTransform == offset::ComponentTransformMode::Cn155284) {
            uint8_t keyA = 0, keyB = 0;
            uint64_t now = 0, epoch = 0;
            if (!HasFreshCn155284ComponentKeys(keyA, keyB, now, epoch)) {
                InvalidateCn155284ComponentKeys();
                if (!SDK->read_range(SDK->dwGameBase + activeOffsets.componentKeyByteRva, &keyA, sizeof(keyA)) ||
                    !SDK->read_range(SDK->dwGameBase + activeOffsets.componentKeySecondByteRva, &keyB, sizeof(keyB))) {
                    Diagnostics::RecordDecryptFailure();
                    return 0;
                }
                StoreCn155284ComponentKeys(now, epoch, keyA, keyB);
            }
            const uintptr_t decoded = static_cast<uintptr_t>(present_mask & Cn155284::Component(component, keyA, keyB));
            uint8_t type = 0;
            if (decoded < 0x10000 || decoded >= 0x0000800000000000ULL ||
                !SDK->read_range(decoded + 0x10, &type, sizeof(type)) || type != idx) {
                Diagnostics::RecordDecryptFailure();
                return 0;
            }
            return decoded;
        }

        if (activeOffsets.componentTransform == offset::ComponentTransformMode::Identity) {
            const uintptr_t decoded = static_cast<uintptr_t>(present_mask & component);
            if (!decoded)
                Diagnostics::RecordDecryptFailure();
            return decoded;
        }

        uint64_t component_key_source = 0;
        uint64_t component_key_material_1 = 0;
        uint8_t component_key_byte = 0;
        const bool world155284 = activeOffsets.componentTransform == offset::ComponentTransformMode::World155284;
        const auto keyRoot = world155284 ? activeOffsets.componentKeyRootRva : offset::ComponentXorQword_RVA;
        const auto keyOffset = world155284 ? activeOffsets.componentKeyOffset : offset::ComponentXorQword_Off;
        const auto keyByte = world155284 ? activeOffsets.componentKeyByteRva : offset::ComponentXorByte_RVA;
        if (!SDK->GetCachedComponentKeyMaterial(SDK->dwGameBase + keyRoot, keyOffset, SDK->dwGameBase + keyByte,
                component_key_source,
                component_key_material_1,
                component_key_byte)) {
            Diagnostics::RecordDecryptFailure();
            return 0;
        }

        if (world155284) {
            if (!component_key_material_1)
                return 0;
            const auto decoded = present_mask & RegionalLayout::World155284::DecodeComponent(
                component, component_key_material_1, component_key_byte);
            return decoded > 0x10000 && decoded < 0x800000000000ULL ? decoded : 0;
        }

        if (activeOffsets.componentTransform == offset::ComponentTransformMode::World151177) {
            component = ROR64(component ^ component_key_material_1,
                offset::Component151177_Ror1);
            component = ROR64(component + offset::Component151177_Add1,
                offset::Component151177_Ror2);
            component ^= static_cast<uint64_t>(component_key_byte);
            component ^= offset::Component151177_Xor1;
            component ^= offset::Component151177_Xor2;

            const uintptr_t decoded = static_cast<uintptr_t>(present_mask & component);
            if (!decoded)
                Diagnostics::RecordDecryptFailure();
            return decoded;
        }

        if (activeOffsets.componentTransform == offset::ComponentTransformMode::World151293) {
            component = static_cast<uint64_t>(component_key_byte) ^
                ((component_key_material_1 ^
                    (component + offset::Component151293_Add1)) +
                    offset::Component151293_Add2) ^
                offset::Component151293_Xor1;
            component ^= offset::Component151293_Xor2;
            component = ROR64(component, offset::Component151293_Ror);

            const uintptr_t decoded = static_cast<uintptr_t>(present_mask & component);
            if (!decoded)
                Diagnostics::RecordDecryptFailure();
            return decoded;
        }

        if (activeOffsets.componentTransform == offset::ComponentTransformMode::World151722) {
            component -= offset::Component151722_Sub1;
            component ^= offset::Component151722_Xor1;
            component = ROR64(component, offset::Component151722_Ror1);
            component ^= offset::Component151722_Xor2;
            component = ROR64(component, offset::Component151722_Ror2);
            component = ROR64(component, offset::Component151722_Ror3);
            component = ROR64(component, offset::Component151722_Ror4);
            component ^= component_key_material_1;
            component ^= static_cast<uint64_t>(component_key_byte);
            component += offset::Component151722_Add1;

            const uintptr_t decoded = static_cast<uintptr_t>(present_mask & component);
            if (!decoded)
                Diagnostics::RecordDecryptFailure();
            return decoded;
        }

        if (activeOffsets.componentTransform == offset::ComponentTransformMode::World150818) {
            component = static_cast<uint64_t>(component_key_byte) ^
                ((component_key_material_1 ^
                    (component + offset::Component150818_Add1)) +
                    offset::Component150818_Add2) ^
                offset::Component150818_Xor1;
            component ^= offset::Component150818_Xor2;
            component = ROR64(component, offset::Component150818_Ror);

            const uintptr_t decoded = static_cast<uintptr_t>(present_mask & component);
            if (!decoded)
                Diagnostics::RecordDecryptFailure();
            return decoded;
        }

        component ^= component_key_material_1;
        component ^= offset::Component_Xor1;
        component = ROR64(component, offset::Component_Ror1);
        component += offset::Component_Add1;
        component ^= static_cast<uint64_t>(component_key_byte);
        component -= offset::Component_Sub1;
        component = ROR64(component, offset::Component_Ror2);
        component = ROR64(component, offset::Component_Ror3);

        const uintptr_t decoded = static_cast<uintptr_t>(present_mask & component);
        if (!decoded)
            Diagnostics::RecordDecryptFailure();
        return decoded;
    }

    inline uintptr_t DecryptComponent(uintptr_t parent, uint32_t idx) {
        EntityHeaderSnapshot snapshot{};
        const EntityHeaderSnapshot* active_snapshot =
            snapshot.Read(parent) ? &snapshot : nullptr;
        return DecryptComponent(parent, idx, active_snapshot);
    }

    inline uint64_t DecodeVisibilityRaw(uint64_t raw) {
        if (offset::Active().componentTransform == offset::ComponentTransformMode::Cn155284)
            return Cn155284::Visibility(raw);
        if (const auto mask = offset::Active().visibilityClearMask)
            return raw != 0 && raw != UINT64_MAX && (raw & mask) == 0 ? 1 : 0;
        return offset::UsesProtectedEntityLayout() ? raw : ((raw & offset::VisibilityWorldVisibleMask) != 0 ? 1 : 0);
    }

    inline uint64_t DecryptVis(uint64_t visBase, bool* readable = nullptr) {
        if (readable) *readable = false;
        const auto& activeOffsets = offset::Active();
        if (!activeOffsets.VisibilityValueOffset)
            return 0;

        uint64_t raw = 0;
        if (!SDK->read_range(visBase + activeOffsets.VisibilityValueOffset, &raw, sizeof(raw)))
            return 0;
        if (readable) *readable = true;
        const bool visible = DecodeVisibilityRaw(raw) != 0;

        static bool loggedVisibility = false;
        if (!loggedVisibility) {
            Diagnostics::Info("[VISIBILITY] %s component 0x34 +0x%llX.", activeOffsets.name,
                static_cast<unsigned long long>(activeOffsets.VisibilityValueOffset));
            loggedVisibility = true;
        }

        const uint64_t result = visible ? 1 : 0;
        {
            static uint32_t sampleCount = 0;
            if (sampleCount < 50) {
                Diagnostics::Aim("visibility.raw sample=%u raw=0x%llX visible=%d visbase=0x%llX",
                    sampleCount,
                    static_cast<unsigned long long>(raw),
                    visible ? 1 : 0,
                    static_cast<unsigned long long>(visBase));
                ++sampleCount;
            }
        }
        return result;
    }

    // =========================================================================
    // Input sensitivity helpers
    // =========================================================================

    inline bool IsPlausibleSensitivity(float value) {
        return IsPlausibleUserGameSensitivity(value);
    }

    inline bool IsPlausibleUserPointer(uint64_t value) {
        return value >= 0x10000ull &&
            value < 0x0000800000000000ull &&
            (value % alignof(void*) == 0);
    }

    inline bool IsAddressInRange(uint64_t value, uint64_t start, uint64_t end) {
        return start != 0 && end > start && value >= start && value < end;
    }

    inline bool IsLikelyEntityMatchId(uint32_t value) {
        return (value & 0x80000000u) != 0;
    }

    inline bool IsLikelyCnEntityParent(
        uint64_t value,
        uint64_t entityList,
        size_t entityListReadSize,
        uint64_t moduleBase,
        uint64_t moduleSize) {
        if (!IsPlausibleUserPointer(value))
            return false;

        const uint64_t moduleEnd =
            moduleSize != 0 ? moduleBase + moduleSize : moduleBase + 0x8000000ull;
        if (IsAddressInRange(value, moduleBase, moduleEnd))
            return false;
        if (IsAddressInRange(value, entityList, entityList + entityListReadSize))
            return false;
        return true;
    }

    inline uint64_t ResolveCnWrapperMapBase(uint64_t parent) {
        if (!IsPlausibleUserPointer(parent))
            return 0;

        const uint32_t id = SDK->RPM<uint32_t>(parent + 0x2F0);
        if (id == 0)
            return 0;

        const uint64_t bucketArray = SDK->RPM<uint64_t>(parent + 0x18);
        if (!IsPlausibleUserPointer(bucketArray))
            return 0;

        const uint64_t bucket = id & 0xFFFu;
        const uint64_t entries = SDK->RPM<uint64_t>(bucketArray + 16ull * bucket);
        const uint32_t count = SDK->RPM<uint32_t>(bucketArray + 16ull * bucket + 8);
        if (!IsPlausibleUserPointer(entries) || count == 0 || count > 4096)
            return 0;

        const uint32_t cappedCount = (std::min)(count, 512u);
        struct MapEntry { uint32_t id, padding; uint64_t pointer; };
        std::array<MapEntry,16> values{};
        for(uint32_t first=0;first<cappedCount;) {
            const auto address=entries+uint64_t(first)*sizeof(MapEntry);
            const auto pageEntries=uint32_t((0x1000-(address&0xFFF))/sizeof(MapEntry));
            const auto length=(std::min)({uint32_t(values.size()),cappedCount-first,pageEntries});
            const bool read=length && SDK->read_range(address,values.data(),size_t(length)*sizeof(MapEntry));
            if(read)for (uint32_t index = 0; index < length; ++index) {
            if(values[index].id!=id)continue;
            const uint64_t mapBase=values[index].pointer;
            if (IsPlausibleUserPointer(mapBase))
                return mapBase;
            }
            first+=length?length:1;
        }
        return 0;
    }

    struct CnLinkTargetMapIdentity {
        uint64_t context=0,owner=0,buckets=0,entries=0;
        uint32_t count=0;
        bool operator==(const CnLinkTargetMapIdentity&) const = default;
    };

    inline bool ReadCnMapBucketHeader(uint64_t bucketAddress, uint64_t& entries, uint32_t& count,
                                      bool forceMerged12B = false) {
        entries = 0;
        count = 0;
#if defined(UNLEASHED_DMA_SESSION_SELFTEST)
        if (!forceMerged12B) {
            return SDK->read_range(bucketAddress, &entries, sizeof(entries)) &&
                SDK->read_range(bucketAddress + 8, &count, sizeof(count));
        }
#else
        (void)forceMerged12B;
#endif
        std::array<uint8_t, 12> raw{};
        if (!SDK->read_range(bucketAddress, raw.data(), raw.size()))
            return false;
        std::memcpy(&entries, raw.data(), sizeof(entries));
        std::memcpy(&count, raw.data() + 8, sizeof(count));
        return true;
    }

    struct CnLinkTargetMapBatchRequest {
        uint64_t linkBase = 0;
        uint64_t idOffset = 0;
        bool requireCompleteProof = false;
    };

    struct CnLinkTargetMapBatchResult {
        uint64_t target = 0;
        uint32_t resolvedId = 0;
        CnLinkTargetMapIdentity verifiedMap{};
    };

    // Dependency-wave batched version of ResolveCnLinkTargetMapBase: resolves N link
    // map queries in 5 scatter waves instead of 6*N serial DMA round trips.
    inline std::vector<CnLinkTargetMapBatchResult> ResolveCnLinkTargetMapBatch(
        const std::vector<CnLinkTargetMapBatchRequest>& requests,
        VMMDLL_SCATTER_HANDLE externalScatter = nullptr,
        bool forceMerged12B = false,
        bool* externalScatterPoisoned = nullptr)
    {
        if (externalScatterPoisoned)
            *externalScatterPoisoned = false;
        std::vector<CnLinkTargetMapBatchResult> results(requests.size());
        if (requests.empty())
            return results;

        const auto mapMode = offset::Active().contextMap;
        const bool protectedMap = mapMode == offset::ContextMapMode::Protected155284;
        const bool worldMap = mapMode == offset::ContextMapMode::World155284;
#if defined(UNLEASHED_DMA_SESSION_SELFTEST)
        const bool useMerged12B = forceMerged12B;
#else
        (void)forceMerged12B;
        const bool useMerged12B = true;
#endif

        VMMDLL_SCATTER_HANDLE handle = externalScatter ? externalScatter : mem.CreateScatterHandle();
        if (!handle)
            return results;
        const bool ownedHandle = externalScatter == nullptr;
        struct ScopeClose {
            VMMDLL_SCATTER_HANDLE h;
            bool own;
            ~ScopeClose() { if (own && h) mem.CloseScatterHandle(h); }
        } guard{ handle, ownedHandle };
        const auto clearExternalOnAbort = [&]() {
            if (!ownedHandle && !mem.ClearScatterHandle(handle) && externalScatterPoisoned)
                *externalScatterPoisoned = true;
        };

        struct WaveState {
            bool active = false;
            uint32_t id = 0;
            DWORD idBytes = 0;
            uint64_t rawContext = 0;
            DWORD contextBytes = 0;
            uint64_t context = 0;
            uint64_t rawOwner = 0;
            DWORD ownerBytes = 0;
            uint64_t bucketOwner = 0;
            uint64_t rawBuckets = 0;
            DWORD bucketsBytes = 0;
            uint64_t bucketArray = 0;
            std::array<uint8_t, 12> bucketRaw{};
            uint64_t entries = 0;
            uint32_t count = 0;
            DWORD bucketBytes = 0;
            DWORD entriesBytes = 0;
            DWORD countBytes = 0;
            uint32_t cappedCount = 0;
            uint32_t cursor = 0;
            uint32_t chunkLen = 0;
            DWORD entryChunkBytes = 0;
            uint64_t uniqueTarget = 0;
            struct MapEntry { uint32_t id, padding; uint64_t pointer; };
            std::array<MapEntry, 16> values{};
        };

        std::vector<WaveState> states(requests.size());
        bool anyActive = false;

        // Wave 1: linkBase + idOffset (4B) and linkBase + 0x8 (8B rawContext).
        bool anyPrepFailed = false;
        for (size_t i = 0; i < requests.size(); ++i) {
            if (!IsPlausibleUserPointer(requests[i].linkBase))
                continue;
            const bool p1 = mem.AddScatterReadRequest(handle, requests[i].linkBase + requests[i].idOffset,
                &states[i].id, sizeof(states[i].id), &states[i].idBytes);
            const bool p2 = mem.AddScatterReadRequest(handle, requests[i].linkBase + 0x8,
                &states[i].rawContext, sizeof(states[i].rawContext), &states[i].contextBytes);
            if (p1 && p2) {
                states[i].active = true;
                anyActive = true;
            } else {
                anyPrepFailed = true;
                if (p1 || p2) {
                    // At least one request was queued into handle; must execute/clear before leaving wave.
                    anyActive = true;
                }
            }
        }
        if (!anyActive) {
            if (anyPrepFailed)
                clearExternalOnAbort();
            return results;
        }
        if (!mem.ExecuteReadScatter(handle)) {
            clearExternalOnAbort();
            return results;
        }

        // Wave 2: context + 0x28 (8B rawOwner).
        anyActive = false;
        bool wave2PrepFailed = false;
        for (size_t i = 0; i < requests.size(); ++i) {
            auto& st = states[i];
            if (!st.active) continue;
            if (st.idBytes != sizeof(st.id) || st.id == 0 || st.contextBytes != sizeof(st.rawContext)) {
                st.active = false;
                continue;
            }
            st.context = protectedMap ? Cn155284::Parent(st.rawContext)
                : worldMap ? RegionalLayout::World155284::DecodeMapParent(st.rawContext) : st.rawContext;
            if (!IsPlausibleUserPointer(st.context)) {
                st.active = false;
                continue;
            }
            if (!mem.AddScatterReadRequest(handle, st.context + 0x28,
                    &st.rawOwner, sizeof(st.rawOwner), &st.ownerBytes)) {
                st.active = false;
                wave2PrepFailed = true;
                continue;
            }
            anyActive = true;
        }
        if (!anyActive) {
            if (wave2PrepFailed)
                clearExternalOnAbort();
            return results;
        }
        if (!mem.ExecuteReadScatter(handle)) {
            clearExternalOnAbort();
            return results;
        }

        // Wave 3: bucketOwner + 0x18 (8B rawBuckets).
        anyActive = false;
        bool wave3PrepFailed = false;
        for (size_t i = 0; i < requests.size(); ++i) {
            auto& st = states[i];
            if (!st.active) continue;
            if (st.ownerBytes != sizeof(st.rawOwner)) {
                st.active = false;
                continue;
            }
            st.bucketOwner = protectedMap ? Cn155284::MapOwner(st.rawOwner)
                : worldMap ? RegionalLayout::World155284::DecodeMapOwner(st.rawOwner) : st.rawOwner;
            if (!IsPlausibleUserPointer(st.bucketOwner)) {
                st.active = false;
                continue;
            }
            if (!mem.AddScatterReadRequest(handle, st.bucketOwner + 0x18,
                    &st.rawBuckets, sizeof(st.rawBuckets), &st.bucketsBytes)) {
                st.active = false;
                wave3PrepFailed = true;
                continue;
            }
            anyActive = true;
        }
        if (!anyActive) {
            if (wave3PrepFailed)
                clearExternalOnAbort();
            return results;
        }
        if (!mem.ExecuteReadScatter(handle)) {
            clearExternalOnAbort();
            return results;
        }

        // Wave 4: bucketArray + 16*bucket (merged 12B entries+count in production).
        anyActive = false;
        bool wave4PrepFailed = false;
        for (size_t i = 0; i < requests.size(); ++i) {
            auto& st = states[i];
            if (!st.active) continue;
            if (st.bucketsBytes != sizeof(st.rawBuckets)) {
                st.active = false;
                continue;
            }
            st.bucketArray = protectedMap ? Cn155284::MapBuckets(st.rawBuckets)
                : worldMap ? RegionalLayout::World155284::DecodeMapBuckets(st.rawBuckets) : st.rawBuckets;
            if (!IsPlausibleUserPointer(st.bucketArray)) {
                st.active = false;
                continue;
            }
            const uint64_t bucketAddr = st.bucketArray + 16ull * (st.id & 0xFFFu);
            if (useMerged12B) {
                if (!mem.AddScatterReadRequest(handle, bucketAddr, st.bucketRaw.data(), st.bucketRaw.size(), &st.bucketBytes)) {
                    st.active = false;
                    wave4PrepFailed = true;
                    continue;
                }
                anyActive = true;
            } else {
                const bool p1 = mem.AddScatterReadRequest(handle, bucketAddr, &st.entries, sizeof(st.entries), &st.entriesBytes);
                const bool p2 = mem.AddScatterReadRequest(handle, bucketAddr + 8, &st.count, sizeof(st.count), &st.countBytes);
                if (p1 && p2) {
                    anyActive = true;
                } else {
                    st.active = false;
                    wave4PrepFailed = true;
                    if (p1 || p2) anyActive = true;
                }
            }
        }
        if (!anyActive) {
            if (wave4PrepFailed)
                clearExternalOnAbort();
            return results;
        }
        if (!mem.ExecuteReadScatter(handle)) {
            clearExternalOnAbort();
            return results;
        }

        for (size_t i = 0; i < requests.size(); ++i) {
            auto& st = states[i];
            if (!st.active) continue;
            if (useMerged12B) {
                if (st.bucketBytes != st.bucketRaw.size()) {
                    st.active = false;
                    continue;
                }
                std::memcpy(&st.entries, st.bucketRaw.data(), sizeof(st.entries));
                std::memcpy(&st.count, st.bucketRaw.data() + 8, sizeof(st.count));
            } else {
                if (st.entriesBytes != sizeof(st.entries) || st.countBytes != sizeof(st.count)) {
                    st.active = false;
                    continue;
                }
            }
            if (!IsPlausibleUserPointer(st.entries) || st.count == 0 || st.count > 4096) {
                st.active = false;
                continue;
            }
            st.cappedCount = (std::min)(st.count, 512u);
            if (requests[i].requireCompleteProof && st.count != st.cappedCount) {
                st.active = false;
                continue;
            }
        }

        // Wave 5+: bucket MapEntry chunks until all active buckets complete.
        // 512 entries at 16B each can straddle up to 3 4KB pages, so allow up to 64 rounds.
        for (unsigned round = 0; round < 64; ++round) {
            anyActive = false;
            bool anyUnalignedAdvance = false;
            bool wave5PrepFailed = false;
            for (size_t i = 0; i < requests.size(); ++i) {
                auto& st = states[i];
                if (!st.active || st.cursor >= st.cappedCount)
                    continue;
                const uint64_t address = st.entries + uint64_t(st.cursor) * sizeof(WaveState::MapEntry);
                const uint32_t pageEntries = uint32_t((0x1000 - (address & 0xFFF)) / sizeof(WaveState::MapEntry));
                st.chunkLen = (std::min)({ uint32_t(st.values.size()), st.cappedCount - st.cursor, pageEntries });
                st.entryChunkBytes = 0;
                if (st.chunkLen == 0) {
                    if (requests[i].requireCompleteProof) {
                        st.active = false;
                    } else {
                        ++st.cursor;
                        if (st.cursor < st.cappedCount)
                            anyUnalignedAdvance = true;
                    }
                    continue;
                }
                if (!mem.AddScatterReadRequest(handle, address, st.values.data(),
                        size_t(st.chunkLen) * sizeof(WaveState::MapEntry), &st.entryChunkBytes)) {
                    wave5PrepFailed = true;
                    if (requests[i].requireCompleteProof) {
                        st.active = false;
                    } else {
                        st.cursor += st.chunkLen;
                    }
                    continue;
                }
                anyActive = true;
            }
            if (!anyActive) {
                if (wave5PrepFailed) {
                    clearExternalOnAbort();
                    return results;
                }
                if (anyUnalignedAdvance)
                    continue;
                break;
            }
            if (!mem.ExecuteReadScatter(handle)) {
                clearExternalOnAbort();
                return results;
            }

            for (size_t i = 0; i < requests.size(); ++i) {
                auto& st = states[i];
                if (!st.active || st.cursor >= st.cappedCount)
                    continue;
                const size_t expectedBytes = size_t(st.chunkLen) * sizeof(WaveState::MapEntry);
                const bool chunkRead = st.entryChunkBytes == expectedBytes;
                if (requests[i].requireCompleteProof && !chunkRead) {
                    st.active = false;
                    continue;
                }
                if (chunkRead) {
                    for (uint32_t idx = 0; idx < st.chunkLen; ++idx) {
                        if (st.values[idx].id != st.id)
                            continue;
                        const uint64_t mapBase = st.values[idx].pointer;
                        if (requests[i].requireCompleteProof) {
                            if (st.uniqueTarget || !IsPlausibleUserPointer(mapBase)) {
                                st.uniqueTarget = 0;
                                st.active = false;
                                break;
                            }
                            st.uniqueTarget = mapBase;
                            continue;
                        }
                        if (IsPlausibleUserPointer(mapBase)) {
                            results[i].target = mapBase;
                            results[i].resolvedId = st.id;
                            st.active = false;
                            break;
                        }
                    }
                }
                st.cursor += st.chunkLen ? st.chunkLen : 1;
                if (st.active && st.cursor >= st.cappedCount && requests[i].requireCompleteProof) {
                    if (st.uniqueTarget) {
                        results[i].target = st.uniqueTarget;
                        results[i].resolvedId = st.id;
                        results[i].verifiedMap = { st.context, st.bucketOwner, st.bucketArray, st.entries, st.count };
                    }
                    st.active = false;
                }
            }
        }
        return results;
    }

    inline uint64_t ResolveCnLinkTargetMapBase(
        uint64_t linkBase,
        uint64_t idOffset,
        uint32_t* resolvedId = nullptr,
        CnLinkTargetMapIdentity* verifiedMap = nullptr) {
        if (resolvedId)
            *resolvedId = 0;
        if (verifiedMap)
            *verifiedMap = {};
        const auto mapMode = offset::Active().contextMap;
        const bool protectedMap = mapMode == offset::ContextMapMode::Protected155284;
        const bool worldMap = mapMode == offset::ContextMapMode::World155284;
        RuntimeTrace::MapLookupSpan mapTrace(
            linkBase, idOffset, (verifiedMap ? 1u : 0u) | (protectedMap ? 2u : 0u) | (worldMap ? 4u : 0u));
        if (!IsPlausibleUserPointer(linkBase))
            return 0;

        uint32_t id=0;
        if (!SDK->read_range(linkBase+idOffset,&id,sizeof(id)) || id == 0)
            return 0;

        uint64_t rawContext=0;
        if (!SDK->read_range(linkBase+0x8,&rawContext,sizeof(rawContext)))
            return 0;
        // The historical "context" is the component parent, not a global context.
        const uint64_t context = protectedMap ? Cn155284::Parent(rawContext) :
            worldMap ? RegionalLayout::World155284::DecodeMapParent(rawContext) : rawContext;
        mapTrace.Pointer(1, rawContext, context);
        if (!IsPlausibleUserPointer(context))
            return 0;

        uint64_t rawOwner=0;
        if (!SDK->read_range(context+0x28,&rawOwner,sizeof(rawOwner)))
            return 0;
        const uint64_t bucketOwner = protectedMap ? Cn155284::MapOwner(rawOwner) :
            worldMap ? RegionalLayout::World155284::DecodeMapOwner(rawOwner) : rawOwner;
        mapTrace.Pointer(2, rawOwner, bucketOwner);
        if (!IsPlausibleUserPointer(bucketOwner))
            return 0;

        uint64_t rawBuckets=0;
        if (!SDK->read_range(bucketOwner+0x18,&rawBuckets,sizeof(rawBuckets)))
            return 0;
        const uint64_t bucketArray = protectedMap ? Cn155284::MapBuckets(rawBuckets) :
            worldMap ? RegionalLayout::World155284::DecodeMapBuckets(rawBuckets) : rawBuckets;
        mapTrace.Pointer(3, rawBuckets, bucketArray);
        if (!IsPlausibleUserPointer(bucketArray))
            return 0;

        const uint64_t bucket = id & 0xFFFu;
        uint64_t entries=0;uint32_t count=0;
        if (!ReadCnMapBucketHeader(bucketArray+16ull*bucket,entries,count))
            return 0;
        mapTrace.Bucket(entries, count, id);
        if (!IsPlausibleUserPointer(entries) || count == 0 || count > 4096)
            return 0;

        const uint32_t cappedCount = (std::min)(count, 512u);
        // A replacement proof needs the complete bucket, not the first matching UID.
        if(verifiedMap && count!=cappedCount)return 0;
        uint64_t uniqueTarget=0;
        struct MapEntry { uint32_t id, padding; uint64_t pointer; };
        std::array<MapEntry,16> values{};
        for(uint32_t first=0;first<cappedCount;) {
            const auto address=entries+uint64_t(first)*sizeof(MapEntry);
            const auto pageEntries=uint32_t((0x1000-(address&0xFFF))/sizeof(MapEntry));
            const auto length=(std::min)({uint32_t(values.size()),cappedCount-first,pageEntries});
            const bool read=length && SDK->read_range(address,values.data(),size_t(length)*sizeof(MapEntry));
            if(verifiedMap && !read)return 0;
            if(read)for (uint32_t index = 0; index < length; ++index) {
            if(values[index].id!=id)continue;
            const uint64_t mapBase=values[index].pointer;
            if(verifiedMap) {
                if(uniqueTarget || !IsPlausibleUserPointer(mapBase))return 0;
                uniqueTarget=mapBase;
                continue;
            }
            if (IsPlausibleUserPointer(mapBase)) {
                if (resolvedId)
                    *resolvedId = id;
                mapTrace.Result(mapBase);
                return mapBase;
            }
            }
            first+=length?length:1;
        }

        if(verifiedMap && uniqueTarget) {
            *verifiedMap={context,bucketOwner,bucketArray,entries,count};
            if(resolvedId)*resolvedId=id;
            mapTrace.Result(uniqueTarget);
            return uniqueTarget;
        }

        return 0;
    }

    inline bool AddCnMapBaseCandidate(
        std::vector<uint64_t>& candidates,
        uint64_t value,
        uint64_t entityList,
        size_t entityListReadSize,
        uint64_t moduleBase,
        uint64_t moduleSize) {
        if (!IsLikelyCnEntityParent(
                value,
                entityList,
                entityListReadSize,
                moduleBase,
                moduleSize)) {
            return false;
        }
        if (std::find(candidates.begin(), candidates.end(), value) != candidates.end())
            return false;
        candidates.push_back(value);
        return true;
    }

    inline std::vector<uint64_t> CollectCnMapBaseCandidates(
        uint64_t parent,
        uint64_t entityList,
        size_t entityListReadSize,
        uint64_t moduleBase,
        uint64_t moduleSize,
        Diagnostics::EntityScanDetailStats* scanDetail = nullptr,
        bool mapDiagEnabled = false) {
        std::vector<uint64_t> candidates;
        candidates.reserve(3);

        if (AddCnMapBaseCandidate(
                candidates,
                parent,
                entityList,
                entityListReadSize,
                moduleBase,
                moduleSize) &&
            mapDiagEnabled &&
            scanDetail) {
            ++scanDetail->cnMapCandidateDirectSourceCount;
        }

        const uint64_t plus8 = SDK->RPM<uint64_t>(parent + 0x8);
        if (AddCnMapBaseCandidate(
                candidates,
                plus8,
                entityList,
                entityListReadSize,
                moduleBase,
                moduleSize) &&
            mapDiagEnabled &&
            scanDetail) {
            ++scanDetail->cnMapCandidatePlus8SourceCount;
        }

        const uint64_t wrapperMapBase = ResolveCnWrapperMapBase(parent);
        if (AddCnMapBaseCandidate(
                candidates,
                wrapperMapBase,
                entityList,
                entityListReadSize,
                moduleBase,
                moduleSize) &&
            mapDiagEnabled &&
            scanDetail) {
            ++scanDetail->cnMapCandidateWrapperSourceCount;
        }

        return candidates;
    }

    inline uint64_t DecryptSingletonList(uint64_t raw) {
        return ROR64(
            (raw ^ offset::Singleton_K1_xor) - offset::Singleton_K2_sub,
            offset::Singleton_Ror) + offset::Singleton_K3_add;
    }

    inline uint64_t DecryptLiveGameAdmin(uint64_t raw) {
        return ROR64(
            ((raw + offset::LiveGameAdmin_Add1) ^ offset::LiveGameAdmin_Xor1) +
                offset::LiveGameAdmin_Add2,
            offset::LiveGameAdmin_Ror);
    }

    inline uint64_t DecryptSingletonList151722(uint64_t raw) {
        uint64_t value = ROR64(raw, offset::Singleton151722_Ror1);
        value -= offset::Singleton151722_Sub1;
        value ^= offset::Singleton151722_Xor1;
        return ROR64(value, offset::Singleton151722_Ror2);
    }

    inline bool TryGetLiveGameAdmin(uint64_t& gameAdmin) {
        gameAdmin = 0;
        if (!SDK->dwGameBase)
            return false;

        const auto& activeOffsets = offset::Active();
        const auto clientGameRva = activeOffsets.clientGameRva ? activeOffsets.clientGameRva : activeOffsets.GlobalAdmin_RVA;
        if (!clientGameRva)
            return false;

        const uint64_t clientGame = SDK->RPM<uint64_t>(
            SDK->dwGameBase + clientGameRva);
        if (!IsPlausibleUserPointer(clientGame))
            return false;

        const uint64_t raw = SDK->RPM<uint64_t>(
            clientGame + offset::LiveGameAdmin_InputOffset);
        if (!raw)
            return false;

        uint64_t decoded = 0;
        switch (activeOffsets.gameAdminMode) {
        case offset::GameAdminMode::World155284:
            decoded = RegionalLayout::World155284::DecodeGameAdmin(raw);
            break;
        case offset::GameAdminMode::Cn155284:
            decoded = RegionalLayout::Cn155284::DecodeGameAdmin(raw);
            break;
        default:
            // Legacy World profiles read the encoded admin from GlobalAdmin_RVA.
            decoded = DecryptLiveGameAdmin(raw);
            break;
        }
        gameAdmin = decoded;
        return IsPlausibleUserPointer(gameAdmin);
    }

    // Product-side source for the engine ray context that `setup + 0x1F0` must hold.
    //
    //   ClientGame = [base + clientGameRva]
    //   context    = Decode([ClientGame + 0x30])
    //
    // The pointer is level-scoped: a new map/match builds a new object and rewrites
    // +0x30, so callers must re-resolve per scene and drop cached engines afterwards.
    // Verified live on CN 155284N (2026-11-25): the decoded value was identical to the
    // context captured from a real `setup + 0x1F0`, including +0x08/+0x10.
    inline bool TryGetRayContext(uint64_t& context) {
        context = 0;
        if (!SDK->dwGameBase)
            return false;

        const auto& activeOffsets = offset::Active();
        if (!activeOffsets.clientGameRva ||
            activeOffsets.gameAdminMode == offset::GameAdminMode::None) {
            return false;
        }

        const uint64_t clientGame = SDK->RPM<uint64_t>(
            SDK->dwGameBase + activeOffsets.clientGameRva);
        if (!IsPlausibleUserPointer(clientGame))
            return false;

        const uint64_t raw = SDK->RPM<uint64_t>(
            clientGame + offset::LiveGameAdmin_InputOffset);
        if (!raw)
            return false;

        uint64_t decoded = 0;
        switch (activeOffsets.gameAdminMode) {
        case offset::GameAdminMode::World155284:
            decoded = RegionalLayout::World155284::DecodeGameAdmin(raw);
            break;
        case offset::GameAdminMode::Cn155284:
            decoded = RegionalLayout::Cn155284::DecodeGameAdmin(raw);
            break;
        default:
            return false;
        }
        if (!IsPlausibleUserPointer(decoded))
            return false;

        // Same liveness test the probe uses: the first qword must still be the class
        // vtable inside the module image. A dead level leaves a heap pointer here.
        if (activeOffsets.rayContextVtableRva) {
            const uint64_t vtable = SDK->RPM<uint64_t>(decoded);
            if (vtable != SDK->dwGameBase + activeOffsets.rayContextVtableRva)
                return false;
        }

        context = decoded;
        return true;
    }

    inline uint64_t ResolveSingletonList(uint64_t raw) {
        switch (offset::Active().singletonListMode) {
        case offset::SingletonListMode::Cn155284:
            return Cn155284::Singleton(raw);
        case offset::SingletonListMode::World155284:
            return RegionalLayout::World155284::DecodeGlobalAdmin(raw);
        case offset::SingletonListMode::Plain:
            return raw;
        case offset::SingletonListMode::LiveGameAdminWorld:
            return raw;
        case offset::SingletonListMode::EncryptedWorld151293:
            return DecryptLiveGameAdmin(raw);
        case offset::SingletonListMode::EncryptedWorld151722:
            return DecryptSingletonList151722(raw);
        case offset::SingletonListMode::EncryptedWorld:
        default:
            return DecryptSingletonList(raw);
        }
    }

    inline bool TryGetLiveGameAdminSingleton(uint64_t index, uint64_t& object) {
        object = 0;

        uint64_t gameAdmin = 0;
        if (!TryGetLiveGameAdmin(gameAdmin))
            return false;

        const uint64_t listOffsets[] = {
            offset::Singleton_InputOffset,
            0x168,
            0x2E0,
            0x300,
            0x310,
        };

        for (const uint64_t listOffset : listOffsets) {
            const uint64_t raw = SDK->RPM<uint64_t>(gameAdmin + listOffset);
            const uint64_t candidates[] = {
                raw,
                DecryptSingletonList(raw),
                DecryptLiveGameAdmin(raw),
                DecryptSingletonList151722(raw),
            };
            for (const uint64_t list : candidates) {
                if (!IsPlausibleUserPointer(list))
                    continue;
                const uint64_t candidate = SDK->RPM<uint64_t>(list + 8ull * index);
                if (IsPlausibleUserPointer(candidate)) {
                    object = candidate;
                    return true;
                }
            }
        }

        return false;
    }

    inline bool TryGetGlobalAdminSingleton(uint64_t index, uint64_t& object) {
        object = 0;
        if (!SDK->dwGameBase)
            return false;

        const auto& activeOffsets = offset::Active();
        if (!activeOffsets.GlobalAdmin_RVA)
            return false;

        if (activeOffsets.singletonListMode == offset::SingletonListMode::LiveGameAdminWorld)
            return TryGetLiveGameAdminSingleton(index, object);

        const uint64_t admin = SDK->RPM<uint64_t>(
            SDK->dwGameBase + activeOffsets.GlobalAdmin_RVA);
        if (!IsPlausibleUserPointer(admin))
            return false;

        const uint64_t listRaw = SDK->RPM<uint64_t>(
            admin + offset::Singleton_InputOffset);
        const uint64_t list = ResolveSingletonList(listRaw);
        if (!IsPlausibleUserPointer(list))
            return false;

        object = SDK->RPM<uint64_t>(list + 8ull * index);
        return IsPlausibleUserPointer(object);
    }

    inline bool TryReadSensitivityFromObject(uint64_t object, float& value) {
        value = 0.0f;
        if (!IsPlausibleUserPointer(object))
            return false;

        const float normalized = SDK->RPM<float>(object + offset::Sensitivity);
        float userSensitivity = 0.0f;
        if (!NormalizeSingletonGameSensitivity(
                normalized,
                offset::Sensitivity_NormalizedToUserScale,
                userSensitivity)) {
            return false;
        }

        const float sensX = SDK->RPM<float>(object + offset::SensX_Scale);
        const float sensY = SDK->RPM<float>(object + offset::SensY_Scale);
        if (!std::isfinite(sensX) || !std::isfinite(sensY))
            return false;

        value = userSensitivity;
        return true;
    }

    inline bool TryReadInputMouseScaleSensitivity(float& value, uint64_t* sourceObject) {
        value = 0.0f;
        if (sourceObject)
            *sourceObject = 0;
        if (!SDK->dwGameBase)
            return false;

        const auto& activeOffsets = offset::Active();
        if (!activeOffsets.InputMouseScaleX_RVA || !activeOffsets.InputMouseScaleY_RVA)
            return false;

        const uint64_t xAddress = SDK->dwGameBase + activeOffsets.InputMouseScaleX_RVA;
        const uint64_t yAddress = SDK->dwGameBase + activeOffsets.InputMouseScaleY_RVA;
        const float sensX = SDK->RPM<float>(xAddress);
        const float sensY = SDK->RPM<float>(yAddress);
        if (!IsPlausibleSensitivity(sensX) || !IsPlausibleSensitivity(sensY))
            return false;

        const float tolerance = (std::max)(0.05f, (std::max)(sensX, sensY) * 0.02f);
        if (std::fabs(sensX - sensY) > tolerance)
            return false;

        value = (sensX + sensY) * 0.5f;
        if (sourceObject)
            *sourceObject = xAddress;
        return true;
    }

    inline bool TryReadNormalizedSensitivityObject(
        uint64_t object,
        float& value,
        uint64_t* sourceObject) {
        value = 0.0f;
        if (!IsPlausibleUserPointer(object))
            return false;

        const float normalized = SDK->RPM<float>(object + offset::Sensitivity);
        float userSensitivity = 0.0f;
        if (!NormalizeSingletonGameSensitivity(
                normalized,
                offset::Sensitivity_NormalizedToUserScale,
                userSensitivity)) {
            return false;
        }

        value = userSensitivity;

        if (sourceObject)
            *sourceObject = object;
        return true;
    }

    inline bool TryReadWorldGlobalAdminSensitivity(
        float& value,
        uint64_t* sourceObject) {
        value = 0.0f;
        if (sourceObject)
            *sourceObject = 0;
        if (!SDK->dwGameBase || !offset::SensitivityAdmin_World_RVA)
            return false;

        const uint64_t admin = SDK->RPM<uint64_t>(
            SDK->dwGameBase + offset::SensitivityAdmin_World_RVA);
        if (!IsPlausibleUserPointer(admin))
            return false;

        const uint64_t raw = SDK->RPM<uint64_t>(admin + offset::Singleton_InputOffset);
        if (!raw)
            return false;

        const uint64_t lists[] = {
            raw,
            DecryptLiveGameAdmin(raw),
            DecryptSingletonList(raw),
            DecryptSingletonList151722(raw),
        };
        for (uint64_t list : lists) {
            if (!IsPlausibleUserPointer(list))
                continue;

            const uint64_t object = SDK->RPM<uint64_t>(
                list + 8ull * offset::SensitivitySingletonIndex);
            if (TryReadNormalizedSensitivityObject(object, value, sourceObject))
                return true;
        }

        return false;
    }

    inline bool TryReadLiveGameAdminSensitivity(float& value, uint64_t* sourceObject) {
        value = 0.0f;
        if (sourceObject)
            *sourceObject = 0;

        if (TryReadWorldGlobalAdminSensitivity(value, sourceObject))
            return true;

        uint64_t gameAdmin = 0;
        if (!TryGetLiveGameAdmin(gameAdmin))
            return false;

        uint64_t sensitivityObject = 0;
        if (TryGetLiveGameAdminSingleton(offset::SensitivitySingletonIndex, sensitivityObject) &&
            TryReadSensitivityFromObject(sensitivityObject, value)) {
            if (sourceObject)
                *sourceObject = sensitivityObject;
            return true;
        }

        return false;
    }

    inline bool TryReadGameMouseSensitivity(float& value, uint64_t* sourceObject = nullptr) {
        value = 0.0f;
        if (sourceObject)
            *sourceObject = 0;

        if (offset::Active().singletonListMode == offset::SingletonListMode::LiveGameAdminWorld)
            return TryReadLiveGameAdminSensitivity(value, sourceObject) ||
                TryReadInputMouseScaleSensitivity(value, sourceObject);

        uint64_t sensitivityObject = 0;
        if (!TryGetGlobalAdminSingleton(offset::SensitivitySingletonIndex, sensitivityObject) ||
            !TryReadSensitivityFromObject(sensitivityObject, value))
            return TryReadInputMouseScaleSensitivity(value, sourceObject);

        if (sourceObject)
            *sourceObject = sensitivityObject;
        return true;
    }

    inline uintptr_t GetSenstivePTR() {
        static std::mutex cacheMutex;
        std::lock_guard<std::mutex> cacheLock(cacheMutex);
        if (!SDK->dwGameBase)
            return 0;

        static uintptr_t cached = 0;
        static uint64_t cachedBase = 0;
        static uint64_t cachedEpoch = 0;
        static offset::RuntimeProfile cachedProfile = offset::RuntimeProfile::World151722;
        const offset::RuntimeProfile currentProfile = offset::ActiveProfile();
        const uint64_t epoch = ProcessConnection::ConnectionEpoch();
        if (cached && cachedEpoch == epoch && cachedBase == SDK->dwGameBase && cachedProfile == currentProfile)
            return cached;

        cached = 0;
        cachedBase = SDK->dwGameBase;
        cachedEpoch = epoch;
        cachedProfile = currentProfile;

        const auto& activeOffsets = offset::Active();
        if (!activeOffsets.InputMouseScaleX_RVA)
            return 0;
        const uintptr_t mouse_scale_x =
            SDK->dwGameBase + activeOffsets.InputMouseScaleX_RVA;
        if (IsPlausibleSensitivity(SDK->RPM<float>(mouse_scale_x))) {
            cached = mouse_scale_x;
            return cached;
        }

        return 0;
    }

    inline int ReadEntityScannerEnvFlagState(const char* name)
    {
        char buffer[16] = {};
        const DWORD length = GetCnEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
        if (length == 0 || length >= sizeof(buffer))
            return -1;
        return buffer[0] != '0' &&
            buffer[0] != 'n' &&
            buffer[0] != 'N' &&
            buffer[0] != 'f' &&
            buffer[0] != 'F'
            ? 1
            : 0;
    }

    inline bool EntityScannerEnvFlagEnabled(const char* name)
    {
        return ReadEntityScannerEnvFlagState(name) == 1;
    }

    inline bool EntityLightScanRequested()
    {
        static const int lightScanFlag =
            ReadEntityScannerEnvFlagState("UN_DMA_LIGHT_SCAN");
        if (lightScanFlag >= 0)
            return lightScanFlag == 1;
        return offset::UsesProtectedEntityLayout() ||
            EntityScannerEnvFlagEnabled("UN_DMA_ENTITY_PIPELINE_V2");
    }

    inline size_t EntityLightScanMaxCandidates()
    {
        char buffer[32] = {};
        const DWORD length = GetCnEnvironmentVariableA(
            "UN_DMA_LIGHT_SCAN_MAX_CANDIDATES",
            buffer,
            static_cast<DWORD>(sizeof(buffer)));
        if (length == 0 || length >= sizeof(buffer))
            return 128;

        char* end = nullptr;
        const unsigned long parsed = std::strtoul(buffer, &end, 10);
        if (end == buffer || (end && *end != '\0'))
            return 128;
        return (std::min<size_t>)((std::max<size_t>)(parsed, 16), 512);
    }

    inline DWORD EntityScannerPersistentMapCacheTtlMs()
    {
        static const DWORD ttlMs = []() {
            char buffer[32] = {};
            const DWORD length = GetCnEnvironmentVariableA(
                "UN_DMA_CN_STAGE4B_MAP_CACHE_PERSIST_MS",
                buffer,
                static_cast<DWORD>(sizeof(buffer)));
            if (length == 0 || length >= sizeof(buffer))
                return static_cast<DWORD>(0);

            char* end = nullptr;
            const unsigned long parsed = std::strtoul(buffer, &end, 10);
            if (end == buffer || (end && *end != '\0'))
                return static_cast<DWORD>(0);
            if (parsed == 0)
                return static_cast<DWORD>(0);
            return (std::min<DWORD>)((std::max<DWORD>)(parsed, 50), 2000);
        }();
        return ttlMs;
    }

    inline size_t EntityScannerPersistentMapCacheRefreshBudget()
    {
        static const size_t refreshBudget = []() {
            char buffer[32] = {};
            const DWORD length = GetCnEnvironmentVariableA(
                "UN_DMA_CN_STAGE4B_MAP_CACHE_REFRESH_BUDGET",
                buffer,
                static_cast<DWORD>(sizeof(buffer)));
            if (length == 0 || length >= sizeof(buffer))
                return static_cast<size_t>(0);

            char* end = nullptr;
            const unsigned long parsed = std::strtoul(buffer, &end, 10);
            if (end == buffer || (end && *end != '\0'))
                return static_cast<size_t>(0);
            if (parsed == 0)
                return static_cast<size_t>(0);
            return (std::min<size_t>)((std::max<size_t>)(parsed, 1), 64);
        }();
        return refreshBudget;
    }

    inline DWORD EntityScannerComponentNegativeCacheTtlMs()
    {
        static const DWORD ttlMs = []() {
            char buffer[32] = {};
            const DWORD length = GetCnEnvironmentVariableA(
                "UN_DMA_CN_COMPONENT_NEGATIVE_CACHE_MS",
                buffer,
                static_cast<DWORD>(sizeof(buffer)));
            if (length == 0 || length >= sizeof(buffer))
                return static_cast<DWORD>(0);

            char* end = nullptr;
            const unsigned long parsed = std::strtoul(buffer, &end, 10);
            if (end == buffer || (end && *end != '\0'))
                return static_cast<DWORD>(0);
            if (parsed == 0)
                return static_cast<DWORD>(0);
            return (std::min<DWORD>)((std::max<DWORD>)(parsed, 50), 2000);
        }();
        return ttlMs;
    }

    inline DWORD EntityScannerLinkDecryptNegativeCacheTtlMs()
    {
        static const DWORD ttlMs = []() {
            char buffer[32] = {};
            const DWORD length = GetCnEnvironmentVariableA(
                "UN_DMA_CN_LINK_DECRYPT_NEGATIVE_CACHE_MS",
                buffer,
                static_cast<DWORD>(sizeof(buffer)));
            if (length == 0 || length >= sizeof(buffer))
                return static_cast<DWORD>(0);

            char* end = nullptr;
            const unsigned long parsed = std::strtoul(buffer, &end, 10);
            if (end == buffer || (end && *end != '\0'))
                return static_cast<DWORD>(0);
            if (parsed == 0)
                return static_cast<DWORD>(0);
            return (std::min<DWORD>)((std::max<DWORD>)(parsed, 50), 2000);
        }();
        return ttlMs;
    }

    inline DWORD EntityScannerRecordSnapshotCacheTtlMs()
    {
        static const DWORD ttlMs = []() {
            char buffer[32] = {};
            const DWORD length = GetCnEnvironmentVariableA(
                "UN_DMA_CN_RECORD_SNAPSHOT_CACHE_MS",
                buffer,
                static_cast<DWORD>(sizeof(buffer)));
            if (length == 0 || length >= sizeof(buffer))
                return static_cast<DWORD>(0);

            char* end = nullptr;
            const unsigned long parsed = std::strtoul(buffer, &end, 10);
            if (end == buffer || (end && *end != '\0'))
                return static_cast<DWORD>(0);
            if (parsed == 0)
                return static_cast<DWORD>(0);
            return (std::min<DWORD>)((std::max<DWORD>)(parsed, 50), 2000);
        }();
        return ttlMs;
    }

    inline DWORD EntityScannerEntityListRootCacheTtlMs()
    {
        static const DWORD ttlMs = []() {
            char buffer[32] = {};
            const DWORD length = GetCnEnvironmentVariableA(
                "UN_DMA_CN_ENTITY_LIST_ROOT_CACHE_MS",
                buffer,
                static_cast<DWORD>(sizeof(buffer)));
            if (length == 0 || length >= sizeof(buffer))
                return static_cast<DWORD>(0);

            char* end = nullptr;
            const unsigned long parsed = std::strtoul(buffer, &end, 10);
            if (end == buffer || (end && *end != '\0'))
                return static_cast<DWORD>(0);
            if (parsed == 0)
                return static_cast<DWORD>(0);
            return (std::min<DWORD>)((std::max<DWORD>)(parsed, 50), 10000);
        }();
        return ttlMs;
    }

    inline DWORD EntityScannerEntityListReadNegativeCacheTtlMs()
    {
        static const DWORD ttlMs = []() {
            char buffer[32] = {};
            const DWORD length = GetCnEnvironmentVariableA(
                "UN_DMA_CN_ENTITY_LIST_READ_NEGATIVE_CACHE_MS",
                buffer,
                static_cast<DWORD>(sizeof(buffer)));
            if (length == 0 || length >= sizeof(buffer))
                return static_cast<DWORD>(0);

            char* end = nullptr;
            const unsigned long parsed = std::strtoul(buffer, &end, 10);
            if (end == buffer || (end && *end != '\0'))
                return static_cast<DWORD>(0);
            if (parsed == 0)
                return static_cast<DWORD>(0);
            return (std::min<DWORD>)((std::max<DWORD>)(parsed, 50), 2000);
        }();
        return ttlMs;
    }

    inline DWORD EntityScannerEntityListReadCacheTtlMs()
    {
        static const DWORD ttlMs = []() {
            char buffer[32] = {};
            const DWORD length = GetCnEnvironmentVariableA(
                "UN_DMA_CN_ENTITY_LIST_READ_CACHE_MS",
                buffer,
                static_cast<DWORD>(sizeof(buffer)));
            if (length == 0 || length >= sizeof(buffer))
                return static_cast<DWORD>(0);

            char* end = nullptr;
            const unsigned long parsed = std::strtoul(buffer, &end, 10);
            if (end == buffer || (end && *end != '\0'))
                return static_cast<DWORD>(0);
            if (parsed == 0)
                return static_cast<DWORD>(0);
            return (std::min<DWORD>)((std::max<DWORD>)(parsed, 16), 2000);
        }();
        return ttlMs;
    }

    inline size_t EntityScannerCnEntityListChunkSize()
    {
        static const size_t chunkSize = []() {
            char buffer[32] = {};
            const DWORD length = GetCnEnvironmentVariableA(
                "UN_DMA_CN_ENTITY_LIST_CHUNK_SIZE",
                buffer,
                static_cast<DWORD>(sizeof(buffer)));
            if (length == 0 || length >= sizeof(buffer))
                return static_cast<size_t>(0x10000);

            char* end = nullptr;
            const unsigned long parsed = std::strtoul(buffer, &end, 0);
            if (end == buffer || (end && *end != '\0'))
                return static_cast<size_t>(0x10000);
            size_t value = (std::min<size_t>)(
                (std::max<size_t>)(parsed, 0x1000),
                0x10000);
            value -= value % 0x1000;
            return value == 0 ? static_cast<size_t>(0x1000) : value;
        }();
        return chunkSize;
    }

    inline size_t EntityScannerRecordSnapshotCacheRefreshBudget()
    {
        static const size_t refreshBudget = []() {
            char buffer[32] = {};
            const DWORD length = GetCnEnvironmentVariableA(
                "UN_DMA_CN_RECORD_SNAPSHOT_CACHE_REFRESH_BUDGET",
                buffer,
                static_cast<DWORD>(sizeof(buffer)));
            if (length == 0 || length >= sizeof(buffer))
                return static_cast<size_t>(0);

            char* end = nullptr;
            const unsigned long parsed = std::strtoul(buffer, &end, 10);
            if (end == buffer || (end && *end != '\0'))
                return static_cast<size_t>(0);
            if (parsed == 0)
                return static_cast<size_t>(0);
            return (std::min<size_t>)((std::max<size_t>)(parsed, 1), 64);
        }();
        return refreshBudget;
    }

    inline size_t EntityScannerComponentNegativeCacheRefreshBudget()
    {
        static const size_t refreshBudget = []() {
            char buffer[32] = {};
            const DWORD length = GetCnEnvironmentVariableA(
                "UN_DMA_CN_COMPONENT_NEGATIVE_CACHE_REFRESH_BUDGET",
                buffer,
                static_cast<DWORD>(sizeof(buffer)));
            if (length == 0 || length >= sizeof(buffer))
                return static_cast<size_t>(0);

            char* end = nullptr;
            const unsigned long parsed = std::strtoul(buffer, &end, 10);
            if (end == buffer || (end && *end != '\0'))
                return static_cast<size_t>(0);
            if (parsed == 0)
                return static_cast<size_t>(0);
            return (std::min<size_t>)((std::max<size_t>)(parsed, 1), 64);
        }();
        return refreshBudget;
    }

    inline DWORD EntityScannerStaleMetadataMs()
    {
        static const DWORD staleMs = []() {
            char buffer[32] = {};
            const DWORD length = GetCnEnvironmentVariableA(
                "UN_DMA_CN_SCANNER_STALE_METADATA_MS",
                buffer,
                static_cast<DWORD>(sizeof(buffer)));
            if (length == 0 || length >= sizeof(buffer))
                return static_cast<DWORD>(0);

            char* end = nullptr;
            const unsigned long parsed = std::strtoul(buffer, &end, 10);
            if (end == buffer || (end && *end != '\0'))
                return static_cast<DWORD>(0);
            if (parsed == 0)
                return static_cast<DWORD>(0);
            return (std::min<DWORD>)((std::max<DWORD>)(parsed, 50), 30000);
        }();
        return staleMs;
    }

    // =========================================================================
    // Entity list scanning
    // =========================================================================

    inline bool DynamicEntityRegistryEnabled() {
        return offset::Active().registryLockRva != 0;
    }
#ifdef UNLEASHED_REGISTRY_PRODUCER_TEST
    inline uint64_t (*registryProducerClock)()=&::GetTickCount64;
    inline void (*registryProducerEvent)(const char*)=nullptr;
#define REGISTRY_PRODUCER_EVENT(name) do { if(registryProducerEvent)registryProducerEvent(name); } while(false)
#else
#define REGISTRY_PRODUCER_EVENT(name) do {} while(false)
#endif
    inline uint64_t EntityRegistryNowMs() {
#ifdef UNLEASHED_REGISTRY_PRODUCER_TEST
        return registryProducerClock();
#else
        return GetTickCount64();
#endif
    }
#ifdef UNLEASHED_DISCOVERY_TEST
    inline std::chrono::steady_clock::time_point (*discoveryBudgetClock)()=nullptr;
    inline void (*discoveryProbeTest)(uint64_t,bool,bool,bool)=nullptr;
#endif
    inline std::chrono::steady_clock::time_point EntityDiscoveryBudgetNow() {
#ifdef UNLEASHED_DISCOVERY_TEST
        if(discoveryBudgetClock)return discoveryBudgetClock();
#endif
#ifdef UNLEASHED_REGISTRY_RELOCATION_TEST
        return std::chrono::steady_clock::time_point(std::chrono::milliseconds(EntityRegistryNowMs()));
#else
        return std::chrono::steady_clock::now();
#endif
    }
#ifdef UNLEASHED_REGISTRY_RELOCATION_TEST
    inline void (*registryRelocationEvent)(const char*)=nullptr;
#define REGISTRY_RELOCATION_EVENT(name) do { if(OW::registryRelocationEvent)OW::registryRelocationEvent(name); } while(false)
#else
#define REGISTRY_RELOCATION_EVENT(name) do {} while(false)
#endif
    inline EntityRegistry::State RefreshEntityRegistry(bool force=false) {
        if(!DynamicEntityRegistryEnabled()) {
            EntityRegistry::State legacy;legacy.available=true;legacy.error=EntityRegistry::Error::None;return legacy;
        }
        auto& monitor=entityRegistryMonitor;
        std::unique_lock<std::mutex> readerLock(monitor.readMutex,std::defer_lock);
        REGISTRY_PRODUCER_EVENT("reader_wait");
        if(force)readerLock.lock();
        else if(!readerLock.try_lock()) {
            std::lock_guard<std::mutex> lock(monitor.mutex);return monitor.state;
        }
        REGISTRY_PRODUCER_EVENT("reader_acquired");
#ifdef UNLEASHED_REGISTRY_PRODUCER_TEST
        const uint64_t now=registryProducerClock(),epoch=ProcessConnection::ConnectionEpoch(),module=SDK->dwGameBase;
#else
        const uint64_t now=GetTickCount64(),epoch=ProcessConnection::ConnectionEpoch(),module=SDK->dwGameBase;
#endif
        const bool connected=ProcessConnection::IsConnected() && RuntimeWorkersRunning();
        {
            std::lock_guard<std::mutex> lock(monitor.mutex);
            auto& state=monitor.state;
            if(!connected) { state.available=false;state.error=EntityRegistry::Error::Disconnected;return state; }
            if(!state.ShouldRead(epoch,module,now,force))return state;
        }
        const auto started=std::chrono::steady_clock::now();
        EntityRegistry::Snapshot incoming{};
        const auto contextCurrent=[&] {
            return RuntimeWorkersRunning() && ProcessConnection::IsConnected() &&
                ProcessConnection::ConnectionEpoch()==epoch && SDK->dwGameBase==module;
        };
        const auto readCurrent = [&](uint64_t address, void* output, size_t size) {
            return contextCurrent() && mem.ReadFreshExact(address, output, size);
        };
        const auto& offsets = offset::Active();
        auto error = EntityRegistry::ReadSnapshotRetryBusy(epoch, module, readCurrent, [&] {
            REGISTRY_PRODUCER_EVENT("pause_begin");
            const bool running = RuntimeWorkerSleep(1);
            REGISTRY_PRODUCER_EVENT("pause_end");
            return running && contextCurrent();
        }, incoming, offsets.Address_entity_base - 24, offsets.registryLockRva, offsets.imageSize);
        // V2 恢复定位只运行在串行 reader lane；正式扫描仍消费原有代次快照。
        if(offset::UsesEntityRegistryRecovery() && error!=EntityRegistry::Error::None && error!=EntityRegistry::Error::Busy &&
           error!=EntityRegistry::Error::Unstable && contextCurrent()) {
            static EntityRegistry::RegistryResolver resolver;
            static uint64_t resetAt=0;
            static uint32_t resolverImageSize=0;
            static bool rangesReady=false;
            const bool terminal=resolver.status==EntityRegistry::ResolveStatus::NoMatch ||
                resolver.status==EntityRegistry::ResolveStatus::Incomplete ||
                resolver.status==EntityRegistry::ResolveStatus::Ambiguous ||
                resolver.status==EntityRegistry::ResolveStatus::EmptyUnconfirmed;
            if(!resolver.Matches(epoch,module) || resolverImageSize!=offsets.imageSize || (terminal && now-resetAt>5000)) {
                const auto ranges=V2::Memory::WritableSections(module,offsets.imageSize,
                    [&](uint64_t at,void* dst,size_t n){return contextCurrent() && mem.ReadFreshExact(at,dst,n);});
                std::vector<EntityRegistry::SearchRange> input;
                for(auto r:ranges)input.push_back({r.begin,r.end});
                rangesReady=!input.empty();
                resolver.Reset(epoch,module,std::move(input));resetAt=now;
                resolverImageSize=offsets.imageSize;
                if(!rangesReady)resolver.status=EntityRegistry::ResolveStatus::Incomplete;
            }
            if(rangesReady) {
                error=resolver.Step([&](uint64_t at,void* dst,size_t n){return mem.ReadFreshExact(at,dst,n);},
                    contextCurrent,[&](uint64_t parent,const EntityRegistry::Snapshot& table) {
                        if(!IsLikelyCnEntityParent(parent,table.root,table.Bytes(),module,offsets.imageSize))return false;
                        // 候选必须具有当前地区 parent 的实读身份及组件位图，非零地址不算语义通过。
                        uint32_t match=0;std::array<uint64_t,2> components{};
                        return contextCurrent() && mem.ReadFreshExact(parent+offset::Entity_MatchId,&match,sizeof(match)) &&
                            IsLikelyEntityMatchId(match) &&
                            mem.ReadFreshExact(parent+0x110,components.data(),sizeof(components)) &&
                            (components[0] || components[1]);
                    },incoming);
                if(resolver.status==EntityRegistry::ResolveStatus::Recovered)
                    Diagnostics::Info("V3 registry recovered stateRva=0x%llX pages=%llu bytes=%llu.",
                        resolver.Location(),resolver.pages,resolver.bytes);
            }
        }
        REGISTRY_PRODUCER_EVENT("before_commit");
        // Never hold the publication-state mutex across device I/O.
        // Serialize the running check AND Observe with SetRuntimeWorkersRunning(false).
        std::unique_lock<std::mutex> runningLock(runtimeWorkerWaitMutex);
        std::lock_guard<std::mutex> lock(monitor.mutex);
        if(!contextCurrent())error=EntityRegistry::Error::Disconnected;
        auto& state=monitor.state;
        const auto oldError=state.error;const auto oldGeneration=state.snapshot.generation;
        state.attemptedEpoch=epoch;state.attemptedModule=module;
        state.Observe(error,incoming,now);
        REGISTRY_PRODUCER_EVENT("committed");
        state.readUs=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();
        runningLock.unlock();
        if(error!=oldError || state.snapshot.generation!=oldGeneration)
            Diagnostics::Info("Entity registry status=%u generation=%llu root=0x%llX capacity=%u bytes=%llu pending=%d.",
                unsigned(error),state.snapshot.generation,state.snapshot.root,state.snapshot.capacity,state.snapshot.Bytes(),state.rescanPending);
        return state;
    }
#undef REGISTRY_PRODUCER_EVENT
    inline bool AcceptEntityRegistry(const EntityRegistry::Snapshot& snapshot) {
        if(!DynamicEntityRegistryEnabled())return true;
        std::lock_guard<std::mutex> lock(entityRegistryMonitor.mutex);
        return ProcessConnection::IsConnected() && snapshot.epoch==ProcessConnection::ConnectionEpoch() &&
            entityRegistryMonitor.state.Accept(snapshot);
    }
    inline bool AcknowledgeEntityRegistry(const EntityRegistry::Snapshot& snapshot,EntityRegistry::ScanStatus status) {
        if(!DynamicEntityRegistryEnabled())return status==EntityRegistry::ScanStatus::Complete;
        std::lock_guard<std::mutex> lock(entityRegistryMonitor.mutex);
        return entityRegistryMonitor.state.Acknowledge(snapshot,status);
    }
    struct EntityRegistryScanGuard {
        EntityRegistry::Snapshot snapshot{};
        bool dynamic=DynamicEntityRegistryEnabled(),valid=true;
        uint64_t lastCheckMs=EntityRegistryNowMs(),sliceMs=lastCheckMs;
        EntityRegistryScanGuard() {
            if(dynamic) { const auto state=RefreshEntityRegistry(true);snapshot=state.snapshot;valid=state.available; }
        }
        bool Check(bool force=false) {
            if(!valid)return false;
            const auto now=EntityRegistryNowMs();
            if(dynamic && (force || now-lastCheckMs>=EntityRegistry::pollMs)) {
                RefreshEntityRegistry(true);lastCheckMs=now;
            }
            if(dynamic)valid=AcceptEntityRegistry(snapshot);
            return valid && RuntimeWorkersRunning() && ProcessConnection::IsConnected();
        }
    };

    inline EntityTopologySample ProbeEntityTopologyMembers(bool readMemberIds) {
        EntityTopologySample sample{};
        struct Entity {
            uint64_t entity;
            uint64_t pad;
        };

        EntityRegistryScanGuard guard;
        if(!guard.valid)return sample;
        sample.registry=guard.snapshot;
        const uint64_t entity_list = guard.dynamic?guard.snapshot.root:SDK->RPM<uint64_t>(
            SDK->dwGameBase + offset::Active().Address_entity_base);
        if (!entity_list)
            return sample;

        sample.root=entity_list;
        sample.complete=true;
        const bool cnProfile = offset::UsesProtectedEntityLayout();
        const size_t listReadSize = guard.dynamic?guard.snapshot.Bytes():(cnProfile ? 0x40000 : 0x1E000);
        const size_t chunkSize = cnProfile ? 0x10000 : 0x1000;
        uint64_t moduleSize = 0;
        if (cnProfile)
            moduleSize = static_cast<uint64_t>(mem.GetBaseSize("Overwatch.exe"));

        std::unordered_set<uint64_t> seen{};
        bool membershipClassified=true;
        seen.reserve(256);
        std::vector<uint8_t> chunk(chunkSize);
        for (size_t offset = 0; offset < listReadSize; offset += chunkSize) {
            if(!guard.Check()){sample.complete=false;break;}
            if (!RuntimeWorkersRunning() || !ProcessConnection::IsConnected())
                break;
            const size_t remaining = listReadSize - offset;
            const size_t bytesToRead = guard.dynamic?EntityRegistry::ChunkBytes(guard.snapshot,offset,chunkSize):
                (remaining < chunkSize ? remaining : chunkSize);
            if (!(guard.dynamic?mem.ReadFreshExact(entity_list + offset,chunk.data(),bytesToRead):
                mem.Read(entity_list + offset, chunk.data(), bytesToRead))) { sample.complete=false; continue; }

            for (size_t slotOffset = 0; slotOffset + sizeof(Entity) <= bytesToRead;
                 slotOffset += sizeof(Entity)) {
                if (!RuntimeWorkersRunning() || !ProcessConnection::IsConnected())
                    break;
                uint64_t possibleParent = 0;
                std::memcpy(&possibleParent, chunk.data() + slotOffset, sizeof(possibleParent));
                if (!IsPlausibleUserPointer(possibleParent)) {
                    if(possibleParent!=0)membershipClassified=false;
                    continue;
                }
                if (cnProfile &&
                    !IsLikelyCnEntityParent(
                        possibleParent,
                        entity_list,
                        listReadSize,
                        SDK->dwGameBase,
                        moduleSize)) {
                    membershipClassified=false;
                    continue;
                }
                seen.insert(possibleParent);
            }
        }

        sample.members.assign(seen.begin(),seen.end());
        std::sort(sample.members.begin(),sample.members.end());
        sample.member_ids_complete=readMemberIds && membershipClassified;
        for(size_t begin=0;readMemberIds && begin<sample.members.size() && RuntimeWorkersRunning();begin+=8) {
            if(!guard.Check()){sample.complete=false;sample.member_ids_complete=false;break;}
            const auto count=(std::min)(size_t{8},sample.members.size()-begin);
            std::array<uint32_t,8> ids{};std::array<DWORD,8> bytes{};
            const auto handle=mem.CreateScatterHandle();
            if(!handle){sample.member_ids_complete=false;break;}
            for(size_t i=0;i<count;++i)mem.AddScatterReadRequest(handle,
                sample.members[begin+i]+offset::Entity_MatchId,&ids[i],sizeof(ids[i]),&bytes[i]);
            const bool ok=mem.ExecuteReadScatter(handle);
            mem.CloseScatterHandle(handle);
            if(!ok)sample.member_ids_complete=false;
            for(size_t i=0;i<count;++i)if(bytes[i]!=sizeof(ids[i]))sample.member_ids_complete=false;
            if(ok)for(size_t i=0;i<count;++i)if(bytes[i]==sizeof(ids[i]) && ids[i])
                sample.member_ids.emplace_back(sample.members[begin+i],ids[i]);
            // A mutex is not fair: yielding alone lets the cold scanner reacquire
            // it for every batch and starve the camera for the whole list walk.
            if(begin+count<sample.members.size())RuntimeWorkerSleep(1);
        }
        uint64_t currentRoot=0;
        sample.complete=sample.complete && (guard.dynamic?guard.Check(true):
            (SDK->read_range(SDK->dwGameBase+offset::Active().Address_entity_base,&currentRoot,sizeof(currentRoot)) && currentRoot==entity_list));
        sample.complete=sample.complete && RuntimeWorkersRunning() && ProcessConnection::IsConnected();
        return sample;
    }

    inline size_t ProbeEntityTopologyCandidateCount() { return ProbeEntityTopologyMembers(false).members.size(); }

    inline EntityRosterPolicy::SceneAudit AuditEntityScene(const EntityTopologySample& topology,
        const std::unordered_map<uint64_t,EntityHeaderSnapshot>* headers=nullptr) {
        using Audit = EntityRosterPolicy::SceneAudit;
        if (!topology.complete || !topology.member_ids_complete || !AcceptEntityRegistry(topology.registry))
            return Audit::Unknown;
        EntityRegistryScanGuard guard;
        if (!guard.valid || (guard.dynamic && !guard.snapshot.SameTable(topology.registry))) return Audit::Unknown;
        for (const auto parent : topology.members) {
            if (!guard.Check()) return Audit::Unknown;
            EntityHeaderSnapshot header{};
            if(headers) {
                const auto found=headers->find(parent);
                if(found==headers->end())return Audit::Unknown;
                header=found->second;
            } else if(!header.Read(parent))return Audit::Unknown;
            const auto has = [&](uint32_t type) {
                uint64_t bits = 0;
                return header.ReadParentOffset(0x110 + 8ull * (type >> 6), bits) && (bits & (1ull << (type & 63))) != 0;
            };
            const bool heroComponent=has(offset::Active().typeHeroId);
            const bool actorComponents=has(offset::Active().typeHealth) || has(offset::Active().typeLink) ||
                has(offset::Active().typePlayerController);
            bool knownHero=false,knownDynamic=false;
            if(heroComponent || actorComponents) {
                uint64_t pool=0,id=0;
                if(header.ReadParentOffset(offset::Entity_PoolPtr,pool) && IsPlausibleUserPointer(pool & ~0x3full) &&
                    SDK->read_range((pool & ~0x3full)+offset::Pool_PoolId,&id,sizeof(id)))
                    knownDynamic=V2::WorldObjects::ObjectIndex(id)>=0;
            }
            if (heroComponent && !knownDynamic) {
                const auto base = DecryptComponent(parent, offset::Active().typeHeroId, &header);
                hero_compo_t hero{};
                knownHero=base && SDK->read_range(base,&hero,sizeof(hero)) && GameData::IsKnownHeroId(hero.heroid);
            }
            const auto classification=EntityRosterPolicy::ClassifySceneMember(true,heroComponent,knownHero,actorComponents,knownDynamic);
            if(classification!=Audit::Empty)return classification;
        }
        return guard.Check(true) && AcceptEntityRegistry(topology.registry) ? Audit::Empty : Audit::Unknown;
    }

    inline std::vector<std::pair<uint64_t,uint64_t>> ProbeRegionalMapActor(
        uint64_t parent, const EntityTopologySample& topology, const EntityHeaderSnapshot& header,
        uint32_t parentId, uint64_t link, uint32_t tick,
        std::vector<EntityRosterPolicy::Discovery>* discoveries)
    {
        const auto scene = EntitySceneGeneration();
        const auto profile = offset::ActiveProfile();
        const auto& offsets = offset::Active();
        const auto current = [&] {
            return RuntimeWorkersRunning() && ProcessConnection::IsConnected() &&
                ProcessConnection::ConnectionEpoch() == topology.registry.epoch &&
                EntitySceneGeneration() == scene && offset::ActiveProfile() == profile &&
                AcceptEntityRegistry(topology.registry);
        };
        std::array<uint32_t,2> ids{};
        std::array<uint64_t,6> controllerLayout{};
        if (!link || !current() || !header.ReadComponentLayout(controllerLayout) ||
            !SDK->read_range(link + offset::Link_TargetId, ids.data(), sizeof(ids))) return {};
        const auto linkHero = DecryptComponent(parent, offsets.typeHeroId, &header, true);
        struct Candidate {
            uint64_t actor=0, idOffset=0, health=0, heroBase=0, hero=0;
            uint32_t uid=0;
            CnLinkTargetMapIdentity map;
            std::array<uint64_t,6> layout{};
        };
        std::vector<Candidate> candidates;
        for (size_t index=0; index<ids.size(); ++index) {
            if (!ids[index] || (index && ids[index]==ids[0])) continue;
            Candidate value;
            value.idOffset = index ? offset::Link_UniqueId : offset::Link_TargetId;
            value.actor = ResolveCnLinkTargetMapBase(link, value.idOffset, &value.uid, &value.map);
            if (!value.actor || value.actor==parent) continue;
            if (value.map.context!=parent) return {};
            EntityHeaderSnapshot actorHeader;
            uint32_t actorId=0;
            if (value.uid!=ids[index] || !actorHeader.Read(value.actor) ||
                !actorHeader.ReadParentOffset(offset::Entity_MatchId, actorId) || actorId!=value.uid ||
                !actorHeader.ReadComponentLayout(value.layout)) return {};
            value.health = DecryptComponent(value.actor, offsets.typeHealth, &actorHeader, true);
            value.heroBase = linkHero ? linkHero : DecryptComponent(value.actor, offsets.typeHeroId, &actorHeader, true);
            if (!value.health || !value.heroBase ||
                !SDK->read_range(value.heroBase+0xD0, &value.hero, sizeof(value.hero)) ||
                !GameData::IsKnownHeroId(value.hero)) continue;
            if (!candidates.empty() && candidates.front().actor!=value.actor) return {};
            candidates.push_back(value);
        }
        // A UID index can still name a retired body. Current World155284 bindings
        // require the native map; failed/ambiguous map proof never selects that index.
        if (candidates.empty()) return {};
        EntityHeaderSnapshot finalController;
        uint32_t finalParentId=0;
        std::array<uint64_t,6> finalControllerLayout{};
        std::array<uint32_t,2> finalIds{};
        if (!finalController.Read(parent) ||
            !finalController.ReadParentOffset(offset::Entity_MatchId,finalParentId) || finalParentId!=parentId ||
            !finalController.ReadComponentLayout(finalControllerLayout) || finalControllerLayout!=controllerLayout ||
            DecryptComponent(parent,offsets.typeLink,&finalController,true)!=link ||
            DecryptComponent(parent,offsets.typeHeroId,&finalController,true)!=linkHero ||
            !SDK->read_range(link+offset::Link_TargetId,finalIds.data(),sizeof(finalIds)) || finalIds!=ids) return {};
        for (const auto& value : candidates) {
            EntityHeaderSnapshot finalActor;
            std::array<uint64_t,6> finalLayout{};
            uint32_t actorId=0, mappedId=0;
            uint64_t hero=0;
            CnLinkTargetMapIdentity finalMap;
            if (!finalActor.Read(value.actor) ||
                !finalActor.ReadParentOffset(offset::Entity_MatchId,actorId) || actorId!=value.uid ||
                !finalActor.ReadComponentLayout(finalLayout) || finalLayout!=value.layout ||
                DecryptComponent(value.actor,offsets.typeHealth,&finalActor,true)!=value.health ||
                (!linkHero && DecryptComponent(value.actor,offsets.typeHeroId,&finalActor,true)!=value.heroBase) ||
                !SDK->read_range(value.heroBase+0xD0,&hero,sizeof(hero)) || hero!=value.hero ||
                ResolveCnLinkTargetMapBase(link,value.idOffset,&mappedId,&finalMap)!=value.actor ||
                mappedId!=value.uid || finalMap!=value.map) return {};
        }
        if (!current() || uint32_t(EntityRegistryNowMs()-tick)>100) return {};
        const auto& value=candidates.front();
        if (discoveries) discoveries->push_back({value.actor,parent,value.hero,value.uid,tick,true});
        return {{value.actor,parent}};
    }

    inline std::vector<std::pair<uint64_t,uint64_t>> ProbeRegionalActor(
        uint64_t parent, const EntityTopologySample& topology, const EntityHeaderSnapshot& header,
        uint32_t parentId, uint64_t link, uint32_t tick,
        std::vector<EntityRosterPolicy::Discovery>* discoveries)
    {
        if (offset::Active().contextMap == offset::ContextMapMode::World155284)
            return ProbeRegionalMapActor(parent,topology,header,parentId,link,tick,discoveries);
        if (!link)
            return {};
        uint32_t linkId = 0;
        if (!SDK->read_range(link + offset::Link_UniqueId, &linkId, sizeof(linkId)))
            return {};
        const auto& offsets = offset::Active();
        const auto linkHero = DecryptComponent(parent, offsets.typeHeroId, &header, true);
        // Match the same IDs as the regional scanner, then re-read the actor's identity.
        const std::array<uint32_t,3> ids{parentId + 1, linkId,
            linkId && !(linkId & 0x80000000u) ? linkId | 0x80000000u : 0};
        for (const uint32_t id : ids) {
            if (!id)
                continue;
            for (const auto& [actor, match] : topology.member_ids) {
                if (actor == parent || match != id)
                    continue;
                EntityHeaderSnapshot actorHeader{};
                uint32_t currentMatch = 0;
                if (!actorHeader.Read(actor) || !actorHeader.ReadParentOffset(offset::Entity_MatchId, currentMatch) ||
                    currentMatch != match || !DecryptComponent(actor, offsets.typeHealth, &actorHeader, true))
                    continue;
                const auto heroBase = linkHero ? linkHero : DecryptComponent(actor, offsets.typeHeroId, &actorHeader, true);
                uint64_t hero = 0;
                if (!heroBase || !SDK->read_range(heroBase + 0xD0, &hero, sizeof(hero)) || !GameData::IsKnownHeroId(hero))
                    continue;
                if (discoveries)
                    discoveries->push_back({actor, parent, hero, match, tick});
                return {{actor, parent}};
            }
        }
        return {};
    }

    // Controls inline wave-scatter batching across parent/map/header/component/recheck waves inside ProbePriorityActor.
    // Enabled by default in production; opt-in under selftest so legacy .direct-only fixtures stay compatible
    // while Phase A+ tests exercise the exact production wave-scatter error/cleanup paths.
#if defined(UNLEASHED_DMA_SESSION_SELFTEST)
    inline bool g_probeWaveScatterEnabled = false;
#else
    inline bool g_probeWaveScatterEnabled = true;
#endif

    enum class PriorityProbeUnresolvedKind : uint8_t {
        UnknownOrFailure = 0,
        DefinitiveNonActor = 1,
    };

    // Positive discovery only. Full audits retain responsibility for ambiguous
    // objects; an unresolved parent never removes an existing actor.
    inline std::vector<std::pair<uint64_t,uint64_t>> ProbePriorityActor(uint64_t parent,const EntityTopologySample& topology,bool knownLink,
        std::vector<std::pair<uint64_t,uint64_t>>* bindingMembers=nullptr,bool* bindingComplete=nullptr,
        std::vector<EntityRosterPolicy::Discovery>* discoveries=nullptr,
        const std::unordered_map<uint64_t,EntityHeaderSnapshot>* headers=nullptr,
        const std::vector<EntityRosterPolicy::Discovery>* hotActors=nullptr,bool currentController=false,
        bool currentMapPositiveOnly=false,VMMDLL_SCATTER_HANDLE waveScatter=nullptr,
        PriorityProbeUnresolvedKind* unresolvedKind=nullptr,
        bool* waveScatterPoisoned=nullptr) {
        std::vector<std::pair<uint64_t,uint64_t>> result;
        const DWORD discoveryTick = static_cast<DWORD>(EntityRegistryNowMs());
        if (discoveries) discoveries->clear();
        if(bindingMembers)bindingMembers->clear();
        if(bindingComplete)*bindingComplete=false;
        if(unresolvedKind)*unresolvedKind=PriorityProbeUnresolvedKind::UnknownOrFailure;
        if(waveScatterPoisoned)*waveScatterPoisoned=false;
        const auto trace=[&](uint64_t reason,uint64_t component=0) {
            RuntimeTrace::Emit(RuntimeTrace::DiscoveryProbe,parent,reason,component,knownLink);
        };
        if((!offset::UsesProtectedEntityLayout() && !offset::Active().capacityBoundRegistry) || !topology.complete ||
            !AcceptEntityRegistry(topology.registry) ||
            !std::binary_search(topology.members.begin(),topology.members.end(),parent))return result;
        EntityHeaderSnapshot header{};
        if(headers && headers->contains(parent))header=headers->at(parent);
        else if(!header.Read(parent)){trace(1);return result;}
        uint32_t match=0;if(!header.ReadParentOffset(offset::Entity_MatchId,match) || !match){trace(2);return result;}

        const auto& activeOffsets = offset::Active();
        const bool useWave = waveScatter != nullptr && g_probeWaveScatterEnabled;
        const bool useCnWave = useWave && !activeOffsets.capacityBoundRegistry &&
            activeOffsets.componentTransform == offset::ComponentTransformMode::Cn155284;
        bool scatterPoisoned = false;
        const auto clearWaveScatter = [&]() {
            if (waveScatter && !mem.ClearScatterHandle(waveScatter)) {
                scatterPoisoned = true;
                if (waveScatterPoisoned)
                    *waveScatterPoisoned = true;
            }
        };

        const auto runWave = [&](bool prepOk) -> bool {
            if (scatterPoisoned)
                return false;
            if (!prepOk) {
                // Never leave stack-local buffer pointers queued in reusable waveScatter!
                clearWaveScatter();
                return false;
            }
            const bool execOk = mem.ExecuteReadScatter(waveScatter);
            if (!execOk)
                clearWaveScatter();
            return execOk;
        };

        const auto resolveMapBatch = [&](const std::vector<CnLinkTargetMapBatchRequest>& reqs) {
            if (useWave && !scatterPoisoned) {
                bool poisoned = false;
                auto out = ResolveCnLinkTargetMapBatch(reqs, waveScatter, false, &poisoned);
                if (poisoned) {
                    scatterPoisoned = true;
                    if (waveScatterPoisoned)
                        *waveScatterPoisoned = true;
                }
                return out;
            }
            std::vector<CnLinkTargetMapBatchResult> out(reqs.size());
            for (size_t i = 0; i < reqs.size(); ++i) {
                uint32_t resolvedId = 0;
                CnLinkTargetMapIdentity mapId{};
                out[i].target = ResolveCnLinkTargetMapBase(
                    reqs[i].linkBase,
                    reqs[i].idOffset,
                    &resolvedId,
                    reqs[i].requireCompleteProof ? &mapId : nullptr);
                out[i].resolvedId = resolvedId;
                out[i].verifiedMap = mapId;
            }
            return out;
        };

        uintptr_t link = 0;
        std::array<uint32_t, 2> linkIds{};
        bool linkIdRead = false;
        bool parentHeroChecked = false;
        uintptr_t parentHeroBase = 0;
        uint64_t parentHeroId = 0;
        bool parentHeroIdRead = false;
        bool transportOrDecryptFailure = false;
        bool parentHeroCleanNonHero = false;
        bool ambiguousMapFailed = false;
        const bool isCnLayout = !activeOffsets.capacityBoundRegistry &&
            activeOffsets.componentTransform == offset::ComponentTransformMode::Cn155284;
        const auto headerHasType = [](const EntityHeaderSnapshot& hdr, uint32_t type) {
            uint64_t bits = 0;
            return hdr.ReadParentOffset(0x110 + 8ull * (type >> 6), bits) &&
                (bits & (1ull << (type & 63))) != 0;
        };

        const bool headerHasLink = headerHasType(header, activeOffsets.typeLink);

        if (useCnWave) {
            const CnComponentSlotPlan linkPlan = PlanCnComponentSlot(parent, activeOffsets.typeLink, header);
            if (headerHasLink && !linkPlan.present)
                transportOrDecryptFailure = true;
            if (currentMapPositiveOnly && !linkPlan.present) {
                trace(8);
                return result;
            }
            const CnComponentSlotPlan parentHeroPlan = PlanCnComponentSlot(parent, activeOffsets.typeHeroId, header);
            if (!parentHeroPlan.present)
                parentHeroCleanNonHero = true;
            uint8_t keyA = 0, keyB = 0;
            uint64_t keyNow = 0, keyEpoch = 0;
            bool hasKeys = HasFreshCn155284ComponentKeys(keyA, keyB, keyNow, keyEpoch);

            uintptr_t linkCandidate = 0;
            uintptr_t parentHeroCandidate = 0;
            if (linkPlan.present || parentHeroPlan.present) {
                // Parent Wave A: refresh CN component keys (if stale) + read link/hero table entries in 1 wave.
                DWORD keyABytes = 0, keyBBytes = 0, linkRawBytes = 0, heroRawBytes = 0;
                uint64_t linkRaw = 0, heroRaw = 0;
                bool prepOk = true;
                bool anyQueued = false;
                if (!hasKeys) {
                    const bool pA = mem.AddScatterReadRequest(waveScatter, SDK->dwGameBase + activeOffsets.componentKeyByteRva, &keyA, sizeof(keyA), &keyABytes);
                    const bool pB = mem.AddScatterReadRequest(waveScatter, SDK->dwGameBase + activeOffsets.componentKeySecondByteRva, &keyB, sizeof(keyB), &keyBBytes);
                    prepOk = prepOk && pA && pB;
                    anyQueued = anyQueued || pA || pB;
                }
                if (linkPlan.present) {
                    const bool pL = mem.AddScatterReadRequest(waveScatter, linkPlan.entryAddress, &linkRaw, sizeof(linkRaw), &linkRawBytes);
                    prepOk = prepOk && pL;
                    anyQueued = anyQueued || pL;
                }
                if (parentHeroPlan.present) {
                    const bool pH = mem.AddScatterReadRequest(waveScatter, parentHeroPlan.entryAddress, &heroRaw, sizeof(heroRaw), &heroRawBytes);
                    prepOk = prepOk && pH;
                    anyQueued = anyQueued || pH;
                }
                if (!prepOk || (anyQueued && !runWave(true))) {
                    if (!prepOk) runWave(false);
                    if (!hasKeys) {
                        InvalidateCn155284ComponentKeys();
                        Diagnostics::RecordDecryptFailure();
                    }
                    if (currentMapPositiveOnly) trace(8);
                    return result;
                }
                if (!hasKeys) {
                    if (keyABytes == sizeof(keyA) && keyBBytes == sizeof(keyB)) {
                        StoreCn155284ComponentKeys(keyNow, keyEpoch, keyA, keyB);
                        hasKeys = true;
                    } else {
                        InvalidateCn155284ComponentKeys();
                        Diagnostics::RecordDecryptFailure();
                    }
                }
                if (hasKeys) {
                    if (linkPlan.present) {
                        if (linkRawBytes != sizeof(linkRaw)) {
                            transportOrDecryptFailure = true;
                        } else if (!linkRaw) {
                            Diagnostics::RecordDecryptFailure();
                        } else {
                            const uintptr_t dec = static_cast<uintptr_t>(linkPlan.presentMask & Cn155284::Component(linkRaw, keyA, keyB));
                            if (dec >= 0x10000 && dec < 0x0000800000000000ULL) {
                                linkCandidate = dec;
                            } else {
                                transportOrDecryptFailure = true;
                                Diagnostics::RecordDecryptFailure();
                            }
                        }
                    }
                    if (parentHeroPlan.present) {
                        if (heroRawBytes != sizeof(heroRaw)) {
                            transportOrDecryptFailure = true;
                        } else if (!heroRaw) {
                            parentHeroCleanNonHero = true;
                            Diagnostics::RecordDecryptFailure();
                        } else {
                            const uintptr_t dec = static_cast<uintptr_t>(parentHeroPlan.presentMask & Cn155284::Component(heroRaw, keyA, keyB));
                            if (dec >= 0x10000 && dec < 0x0000800000000000ULL) {
                                parentHeroCandidate = dec;
                            } else {
                                transportOrDecryptFailure = true;
                                Diagnostics::RecordDecryptFailure();
                            }
                        }
                    }
                } else {
                    transportOrDecryptFailure = true;
                }
            }

            // Parent Wave B: verify type bytes + read linkIds + read parent heroId in 1 wave.
            if (linkCandidate || parentHeroCandidate) {
                uint8_t linkType = 0, heroType = 0;
                DWORD linkTypeBytes = 0, linkIdsBytes = 0, heroTypeBytes = 0, heroIdBytes = 0;
                bool prepOk = true;
                bool anyQueued = false;
                if (linkCandidate) {
                    const bool p1 = mem.AddScatterReadRequest(waveScatter, linkCandidate + 0x10, &linkType, sizeof(linkType), &linkTypeBytes);
                    const bool p2 = mem.AddScatterReadRequest(waveScatter, linkCandidate + offset::Link_TargetId, linkIds.data(), sizeof(linkIds), &linkIdsBytes);
                    prepOk = prepOk && p1 && p2;
                    anyQueued = anyQueued || p1 || p2;
                }
                if (parentHeroCandidate) {
                    const bool p3 = mem.AddScatterReadRequest(waveScatter, parentHeroCandidate + 0x10, &heroType, sizeof(heroType), &heroTypeBytes);
                    const bool p4 = mem.AddScatterReadRequest(waveScatter, parentHeroCandidate + 0xD0, &parentHeroId, sizeof(parentHeroId), &heroIdBytes);
                    prepOk = prepOk && p3 && p4;
                    anyQueued = anyQueued || p3 || p4;
                }
                if (!prepOk || (anyQueued && !runWave(true))) {
                    if (!prepOk) runWave(false);
                    if (currentMapPositiveOnly) trace(8);
                    return result;
                }
                if (linkCandidate) {
                    if (linkTypeBytes == sizeof(linkType) && linkType == activeOffsets.typeLink) {
                        link = linkCandidate;
                        linkIdRead = (linkIdsBytes == sizeof(linkIds));
                        if (!linkIdRead)
                            transportOrDecryptFailure = true;
                    } else {
                        linkIds = {};
                        transportOrDecryptFailure = true;
                        Diagnostics::RecordDecryptFailure();
                    }
                }
                if (parentHeroCandidate) {
                    if (heroTypeBytes == sizeof(heroType) && heroType == activeOffsets.typeHeroId) {
                        parentHeroBase = parentHeroCandidate;
                        parentHeroIdRead = (heroIdBytes == sizeof(parentHeroId));
                        if (!parentHeroIdRead)
                            transportOrDecryptFailure = true;
                        else if (!GameData::IsKnownHeroId(parentHeroId))
                            parentHeroCleanNonHero = true;
                    } else {
                        parentHeroId = 0;
                        transportOrDecryptFailure = true;
                        Diagnostics::RecordDecryptFailure();
                    }
                }
            }
            parentHeroChecked = true;
        } else {
            link = DecryptComponent(parent, activeOffsets.typeLink, &header, true);
            if (activeOffsets.capacityBoundRegistry) {
                result = ProbeRegionalActor(parent, topology, header, match, link, discoveryTick, discoveries);
                if (bindingMembers) *bindingMembers = result;
                // Native map evidence admits the current body, not complete roster
                // enumeration or deletion authority over other bodies.
                if (bindingComplete) *bindingComplete =
                    activeOffsets.contextMap == offset::ContextMapMode::Plain && result.size() == 1;
                return result;
            }
            if (!link && headerHasLink) {
                bool cleanNullLink = false;
                if (isCnLayout) {
                    const auto linkPlan = PlanCnComponentSlot(parent, activeOffsets.typeLink, header);
                    uint64_t rawLink = 0;
                    if (linkPlan.present && SDK->read_range(linkPlan.entryAddress, &rawLink, sizeof(rawLink)) && rawLink == 0)
                        cleanNullLink = true;
                }
                if (!cleanNullLink)
                    transportOrDecryptFailure = true;
            }
            linkIdRead = link && SDK->read_range(link + offset::Link_TargetId, linkIds.data(), sizeof(linkIds));
            if (link && !linkIdRead)
                transportOrDecryptFailure = true;
            if (unresolvedKind && !headerHasType(header, activeOffsets.typeHeroId))
                parentHeroCleanNonHero = true;
        }

        std::vector<uint64_t> components;
        const bool targetIdRead = linkIdRead;
        const uint32_t targetId = linkIds[0], linkId = linkIds[1];
        if(currentMapPositiveOnly && (!linkIdRead || (!targetId && !linkId))){trace(8);return result;}
        // Explicit LinkedActor vs NoLinkActor separation: a self-contained NoLinkActor
        // (link == 0) has no link component to read, so linkIdRead == false must not
        // poison bindingsComplete and block roster_complete = 1 when topology is complete.
        // However, if the entity header HAS typeLink and reading/decrypting Link failed,
        // NEVER classify link == 0 as NoLinkActor with bindingComplete = true.
        const bool linkReadOrDecryptFailed = headerHasLink && link == 0 && transportOrDecryptFailure;
        const bool noLinkSelfActor = (link == 0) && !currentMapPositiveOnly && !linkReadOrDecryptFailed;
        bool allBindingsResolved = noLinkSelfActor
            ? topology.member_ids_complete
            : ((currentController || topology.member_ids_complete) && linkIdRead && targetIdRead);
        std::vector<std::pair<uint64_t,uint64_t>> contextBindings;
        std::vector<std::pair<uint64_t,CnLinkTargetMapIdentity>> positiveMapGuards;

        if (linkIdRead && targetIdRead) {
            struct LinkOffsetEntry {
                uint64_t idOffset = 0;
                uint32_t id = 0;
                bool resolvedByMap = false;
                std::vector<uint64_t> matches;
            };
            std::vector<LinkOffsetEntry> activeOffsetsToResolve;
            for (uint64_t idOffset : { static_cast<uint64_t>(offset::Link_TargetId), static_cast<uint64_t>(offset::Link_UniqueId) }) {
                const auto id = idOffset == static_cast<uint64_t>(offset::Link_TargetId) ? targetId : linkId;
                if (!id || (idOffset == static_cast<uint64_t>(offset::Link_UniqueId) && id == targetId))
                    continue;
                activeOffsetsToResolve.push_back({ idOffset, id, false, {} });
            }

            // Initial context-map batch (handles 1 or 2 bindings in 5 waves instead of 6*N serial RTs).
            if ((currentController || currentMapPositiveOnly) && !activeOffsetsToResolve.empty()) {
                std::vector<CnLinkTargetMapBatchRequest> initReqs;
                initReqs.reserve(activeOffsetsToResolve.size());
                for (const auto& entry : activeOffsetsToResolve)
                    initReqs.push_back({ link, entry.idOffset, currentMapPositiveOnly });
                const auto initResults = resolveMapBatch(initReqs);
                for (size_t i = 0; i < activeOffsetsToResolve.size(); ++i) {
                    auto& entry = activeOffsetsToResolve[i];
                    const auto& res = initResults[i];
                    if (res.resolvedId == entry.id && IsPlausibleUserPointer(res.target)) {
                        if (std::find(components.begin(), components.end(), res.target) == components.end())
                            components.push_back(res.target);
                        contextBindings.emplace_back(entry.idOffset, res.target);
                        if (currentMapPositiveOnly)
                            positiveMapGuards.emplace_back(entry.idOffset, res.verifiedMap);
                        entry.resolvedByMap = true;
                        continue;
                    }
                    // A failed map lookup alone does not establish an absent context.
                    // A complete fresh header sweep may prove a unique UID binding;
                    // partial/stale indexes never gain this fallback.
                    if (currentMapPositiveOnly && !topology.member_ids_complete) {
                        trace(8);
                        return {};
                    }
                }
            }

            // UID-index lookup & batched disambiguation for any ambiguous UIDs.
            std::vector<size_t> ambiguousIndices;
            std::vector<CnLinkTargetMapBatchRequest> ambiguousReqs;
            for (size_t i = 0; i < activeOffsetsToResolve.size(); ++i) {
                auto& entry = activeOffsetsToResolve[i];
                if (entry.resolvedByMap)
                    continue;
                for (const auto& [component, memberId] : topology.member_ids) {
                    if (memberId == entry.id && component != parent)
                        entry.matches.push_back(component);
                }
                // A UID can exist in more than one world/context. Only this controller's
                // map identifies which address it owns; equal UIDs are not a replacement proof.
                if (entry.matches.size() > 1) {
                    ambiguousIndices.push_back(i);
                    ambiguousReqs.push_back({ link, entry.idOffset, false });
                } else if (!entry.matches.empty()) {
                    components.push_back(entry.matches.front());
                }
            }
            if (!ambiguousReqs.empty()) {
                const auto ambiguousResults = resolveMapBatch(ambiguousReqs);
                for (size_t k = 0; k < ambiguousIndices.size(); ++k) {
                    const auto& entry = activeOffsetsToResolve[ambiguousIndices[k]];
                    const auto& res = ambiguousResults[k];
                    if (res.resolvedId == entry.id &&
                        std::find(entry.matches.begin(), entry.matches.end(), res.target) != entry.matches.end()) {
                        components.push_back(res.target);
                        contextBindings.emplace_back(entry.idOffset, res.target);
                    } else {
                        allBindingsResolved = false;
                        ambiguousMapFailed = true;
                        if (currentMapPositiveOnly) {
                            trace(8);
                            return {};
                        }
                    }
                }
            }
        }

        if (link && !currentMapPositiveOnly) {
            std::vector<std::pair<uint64_t, uint32_t>> fallbackOffsets;
            std::vector<CnLinkTargetMapBatchRequest> fallbackReqs;
            for (uint64_t idOffset : { static_cast<uint64_t>(offset::Link_TargetId), static_cast<uint64_t>(offset::Link_UniqueId) }) {
                const auto id = idOffset == static_cast<uint64_t>(offset::Link_TargetId) ? targetId : linkId;
                if (linkIdRead && !id)
                    continue;
                if (linkIdRead && idOffset == static_cast<uint64_t>(offset::Link_UniqueId) && id == targetId &&
                    std::any_of(contextBindings.begin(), contextBindings.end(), [&](const auto& pair) {
                        return pair.first == static_cast<uint64_t>(offset::Link_TargetId);
                    }))
                    continue;
                if (std::any_of(contextBindings.begin(), contextBindings.end(), [&](const auto& pair) { return pair.first == idOffset; }))
                    continue;
                if (id && std::any_of(topology.member_ids.begin(), topology.member_ids.end(), [&](const auto& pair) { return pair.second == id; }))
                    continue;
                fallbackOffsets.emplace_back(idOffset, id);
                fallbackReqs.push_back({ link, idOffset, false });
            }
            if (!fallbackReqs.empty()) {
                const auto fallbackResults = resolveMapBatch(fallbackReqs);
                for (size_t i = 0; i < fallbackOffsets.size(); ++i) {
                    const uint32_t id = fallbackOffsets[i].second;
                    const uint64_t target = fallbackResults[i].target;
                    if (id && !IsPlausibleUserPointer(target) &&
                        std::none_of(topology.member_ids.begin(), topology.member_ids.end(), [&](const auto& pair) { return pair.second == id; }))
                        allBindingsResolved = false;
                    if (IsPlausibleUserPointer(target) &&
                        std::find(components.begin(), components.end(), target) == components.end())
                        components.push_back(target);
                }
            }
        }

        auto resolvedComponents=components;
        if(currentMapPositiveOnly && components.empty()){trace(8);return result;}
        struct RecoveredBinding {
            uint64_t idOffset=0,target=0;
            CnLinkTargetMapIdentity map;
        };
        std::vector<RecoveredBinding> recoveredBindings;
        if(components.empty())components.push_back(parent);

        struct CandidateState {
            uint64_t component = 0;
            EntityHeaderSnapshot header{};
            bool needsHeaderRead = false;
            bool matchedById = false;
            uint32_t expectedId = 0;
            bool recoveredRemote = false;
            bool alive = true;
            bool cleanNonActor = false;
            uintptr_t heroBase = 0;
            hero_compo_t hero{};
            uint32_t actorMatch = 0;
            bool hotBody = false;
            bool needsCompHero = false;
            bool needsLifeCheck = false;
            CnComponentSlotPlan heroPlan{};
            CnComponentSlotPlan hpPlan{};
            CnComponentSlotPlan velPlan{};
            uint64_t heroRaw = 0, hpRaw = 0, velRaw = 0;
            DWORD heroRawBytes = 0, hpRawBytes = 0, velRawBytes = 0;
            uintptr_t heroCand = 0, hpCand = 0, velCand = 0;
            uint8_t heroType = 0, hpType = 0, velType = 0;
            DWORD heroTypeBytes = 0, hpTypeBytes = 0, velTypeBytes = 0;
            DWORD heroIdBytes = 0, hpBytes = 0, locBytes = 0;
            health_compo_t hp{};
            XMFLOAT3 location{};
            uint32_t currentId = 0, currentComponentId = 0;
            DWORD idBytes = 0, compIdBytes = 0;
        };

        std::vector<CandidateState> candidates;
        candidates.reserve(components.size());
        bool anyHeaderNeeded = false;
        for (auto component : components) {
            CandidateState c{};
            c.component = component;
            if (component == parent) {
                c.header = header;
            } else if (headers && headers->contains(component)) {
                c.header = headers->at(component);
            } else {
                c.needsHeaderRead = true;
                anyHeaderNeeded = true;
            }
            candidates.push_back(std::move(c));
        }

        // Candidate Header Wave: fetch all uncached candidate headers in 1 scatter wave.
        if (anyHeaderNeeded && RuntimeWorkersRunning() && ProcessConnection::IsConnected()) {
            if (useWave && !scatterPoisoned) {
                std::vector<std::array<uint8_t, kEntityHeaderSnapshotSize>> hdrBufs(candidates.size());
                std::vector<DWORD> hdrBytes(candidates.size(), 0);
                bool prepOk = true;
                bool anyQueued = false;
                for (size_t i = 0; i < candidates.size(); ++i) {
                    if (!candidates[i].needsHeaderRead)
                        continue;
                    const bool p = mem.AddScatterReadRequest(
                        waveScatter,
                        candidates[i].component + kEntityHeaderSnapshotOffset,
                        hdrBufs[i].data(),
                        hdrBufs[i].size(),
                        &hdrBytes[i]);
                    prepOk = prepOk && p;
                    anyQueued = anyQueued || p;
                }
                if (prepOk && anyQueued && runWave(true)) {
                    for (size_t i = 0; i < candidates.size(); ++i) {
                        if (!candidates[i].needsHeaderRead)
                            continue;
                        if (hdrBytes[i] == kEntityHeaderSnapshotSize) {
                            candidates[i].header.parent = candidates[i].component;
                            candidates[i].header.valid = true;
                            candidates[i].header.range.Assign(
                                candidates[i].component + kEntityHeaderSnapshotOffset,
                                hdrBufs[i].data(),
                                hdrBufs[i].size());
                        }
                    }
                } else if (!prepOk) {
                    runWave(false);
                }
            } else {
                for (auto& c : candidates) {
                    if (!RuntimeWorkersRunning() || !ProcessConnection::IsConnected())
                        break;
                    if (c.needsHeaderRead)
                        c.header.Read(c.component);
                }
            }
        }

        // Identity validation & optional stale-unique-index remote recovery.
        for (auto& c : candidates) {
            if (!RuntimeWorkersRunning() || !ProcessConnection::IsConnected()) {
                c.alive = false;
                continue;
            }
            const auto matchedId = std::find_if(topology.member_ids.begin(), topology.member_ids.end(), [&](const auto& pair) {
                return pair.first == c.component && pair.second && (pair.second == linkId || pair.second == targetId);
            });
            const auto context = std::find_if(contextBindings.begin(), contextBindings.end(), [&](const auto& pair) {
                return pair.second == c.component;
            });
            c.matchedById = context != contextBindings.end() || matchedId != topology.member_ids.end();
            c.expectedId = context != contextBindings.end()
                ? (context->first == offset::Link_TargetId ? targetId : linkId)
                : (c.matchedById ? matchedId->second : 0);
            uint32_t componentMatch = 0;
            if (!c.header.valid || (c.matchedById &&
                (!c.header.ReadParentOffset(offset::Entity_MatchId, componentMatch) || componentMatch != c.expectedId))) {
                // Only repair a failed unique index entry. Stable and ambiguous remote
                // bindings retain their existing path; this is not local-controller admission.
                const bool uniqueStaleIndex = !currentController && c.component != parent &&
                    context == contextBindings.end() && matchedId != topology.member_ids.end() &&
                    std::count_if(topology.member_ids.begin(), topology.member_ids.end(),
                        [&](const auto& pair) { return pair.second == c.expectedId && pair.first != parent; }) == 1;
                if (uniqueStaleIndex) {
                    const uint64_t idOffset = c.expectedId == targetId ? offset::Link_TargetId : offset::Link_UniqueId;
                    const auto recResults = resolveMapBatch({ { link, idOffset, true } });
                    const uint64_t replacement = recResults[0].target;
                    const uint32_t resolvedId = recResults[0].resolvedId;
                    const CnLinkTargetMapIdentity currentMap = recResults[0].verifiedMap;
                    uint32_t replacementMatch = 0;
                    EntityHeaderSnapshot replacementHeader{};
                    if (resolvedId == c.expectedId && IsPlausibleUserPointer(replacement) &&
                        replacement != c.component && replacement != parent && replacementHeader.Read(replacement) &&
                        replacementHeader.ReadParentOffset(offset::Entity_MatchId, replacementMatch) && replacementMatch == c.expectedId) {
                        for (auto& resolved : resolvedComponents)
                            if (resolved == c.component) resolved = replacement;
                        c.component = replacement;
                        c.header = std::move(replacementHeader);
                        contextBindings.emplace_back(idOffset, replacement);
                        recoveredBindings.push_back({ idOffset, replacement, currentMap });
                        c.recoveredRemote = true;
                    }
                }
                if (!c.recoveredRemote) {
                    transportOrDecryptFailure = true;
                    trace(c.header.valid ? 8 : 3, c.component);
                    c.alive = false;
                    continue;
                }
            }
        }

        // Hero + Health + Velocity verification waves.
        if (useCnWave && !scatterPoisoned) {
            bool anyWave4A = false;
            for (auto& c : candidates) {
                if (!c.alive)
                    continue;
                if (!RuntimeWorkersRunning() || !ProcessConnection::IsConnected()) {
                    c.alive = false;
                    continue;
                }
                if (parentHeroChecked && parentHeroBase != 0) {
                    c.heroBase = parentHeroBase;
                    if (!parentHeroIdRead || !GameData::IsKnownHeroId(parentHeroId)) {
                        if (!parentHeroIdRead)
                            transportOrDecryptFailure = true;
                        else
                            c.cleanNonActor = true;
                        trace(6, c.component);
                        c.alive = false;
                        continue;
                    }
                    c.hero.heroid = parentHeroId;
                    if (!c.header.ReadParentOffset(offset::Entity_MatchId, c.actorMatch)) {
                        transportOrDecryptFailure = true;
                        trace(2, c.component);
                        c.alive = false;
                        continue;
                    }
                    c.hotBody = !c.recoveredRemote && !currentMapPositiveOnly && hotActors &&
                        std::any_of(hotActors->begin(), hotActors->end(), [&](const auto& actor) {
                            return actor.address == c.component && actor.link == parent &&
                                actor.match == c.actorMatch && actor.hero == c.hero.heroid;
                        });
                    if (!c.hotBody) {
                        c.needsLifeCheck = true;
                        c.hpPlan = PlanCnComponentSlot(c.component, activeOffsets.typeHealth, c.header);
                        c.velPlan = PlanCnComponentSlot(c.component, TYPE_VELOCITY, c.header);
                        if (!c.hpPlan.present || !c.velPlan.present) {
                            c.cleanNonActor = true;
                            trace(4, c.component);
                            c.alive = false;
                            continue;
                        }
                        anyWave4A = true;
                    }
                } else {
                    c.needsCompHero = true;
                    c.heroPlan = PlanCnComponentSlot(c.component, activeOffsets.typeHeroId, c.header);
                    if (!c.heroPlan.present) {
                        c.cleanNonActor = true;
                        trace(6, c.component);
                        c.alive = false;
                        continue;
                    }
                    anyWave4A = true;
                    uint32_t preMatch = 0;
                    const bool hasPreMatch = c.header.ReadParentOffset(offset::Entity_MatchId, preMatch);
                    const bool mightBeHot = hasPreMatch && !c.recoveredRemote && !currentMapPositiveOnly && hotActors &&
                        std::any_of(hotActors->begin(), hotActors->end(), [&](const auto& actor) {
                            return actor.address == c.component && actor.link == parent && actor.match == preMatch;
                        });
                    if (!mightBeHot) {
                        c.needsLifeCheck = true;
                        c.hpPlan = PlanCnComponentSlot(c.component, activeOffsets.typeHealth, c.header);
                        c.velPlan = PlanCnComponentSlot(c.component, TYPE_VELOCITY, c.header);
                    }
                }
            }

            if (anyWave4A) {
                uint8_t keyA = 0, keyB = 0;
                uint64_t keyNow = 0, keyEpoch = 0;
                bool hasKeys = HasFreshCn155284ComponentKeys(keyA, keyB, keyNow, keyEpoch);
                DWORD keyABytes = 0, keyBBytes = 0;
                bool prepOk = true;
                bool anyQueued = false;
                if (!hasKeys) {
                    const bool pA = mem.AddScatterReadRequest(waveScatter, SDK->dwGameBase + activeOffsets.componentKeyByteRva, &keyA, sizeof(keyA), &keyABytes);
                    const bool pB = mem.AddScatterReadRequest(waveScatter, SDK->dwGameBase + activeOffsets.componentKeySecondByteRva, &keyB, sizeof(keyB), &keyBBytes);
                    prepOk = prepOk && pA && pB;
                    anyQueued = anyQueued || pA || pB;
                }
                for (auto& c : candidates) {
                    if (!c.alive) continue;
                    if (c.needsCompHero && c.heroPlan.present) {
                        const bool p = mem.AddScatterReadRequest(waveScatter, c.heroPlan.entryAddress, &c.heroRaw, sizeof(c.heroRaw), &c.heroRawBytes);
                        prepOk = prepOk && p;
                        anyQueued = anyQueued || p;
                    }
                    if (c.needsLifeCheck && c.hpPlan.present && c.velPlan.present) {
                        const bool pHp = mem.AddScatterReadRequest(waveScatter, c.hpPlan.entryAddress, &c.hpRaw, sizeof(c.hpRaw), &c.hpRawBytes);
                        const bool pVel = mem.AddScatterReadRequest(waveScatter, c.velPlan.entryAddress, &c.velRaw, sizeof(c.velRaw), &c.velRawBytes);
                        prepOk = prepOk && pHp && pVel;
                        anyQueued = anyQueued || pHp || pVel;
                    }
                }
                if (!prepOk || (anyQueued && !runWave(true))) {
                    if (!prepOk) runWave(false);
                    transportOrDecryptFailure = true;
                    if (!hasKeys) {
                        InvalidateCn155284ComponentKeys();
                        Diagnostics::RecordDecryptFailure();
                    }
                    for (auto& c : candidates) {
                        if (!c.alive || (!c.needsCompHero && !c.needsLifeCheck)) continue;
                        trace(c.needsCompHero ? 6 : 4, c.component);
                        c.alive = false;
                    }
                } else {
                    if (!hasKeys) {
                        if (keyABytes == sizeof(keyA) && keyBBytes == sizeof(keyB)) {
                            StoreCn155284ComponentKeys(keyNow, keyEpoch, keyA, keyB);
                            hasKeys = true;
                        } else {
                            transportOrDecryptFailure = true;
                            InvalidateCn155284ComponentKeys();
                            Diagnostics::RecordDecryptFailure();
                        }
                    }

                    // Decode pointers and build Wave 4B (type bytes + heroId + hp + location in 1 wave).
                    bool prepB = true;
                    bool anyQueuedB = false;
                    if (hasKeys) {
                        for (auto& c : candidates) {
                            if (!c.alive) continue;
                            if (c.needsCompHero) {
                                if (c.heroRawBytes == sizeof(c.heroRaw) && c.heroRaw) {
                                    const uintptr_t dec = static_cast<uintptr_t>(c.heroPlan.presentMask & Cn155284::Component(c.heroRaw, keyA, keyB));
                                    if (dec >= 0x10000 && dec < 0x0000800000000000ULL) {
                                        c.heroCand = dec;
                                        const bool p1 = mem.AddScatterReadRequest(waveScatter, dec + 0x10, &c.heroType, sizeof(c.heroType), &c.heroTypeBytes);
                                        const bool p2 = mem.AddScatterReadRequest(waveScatter, dec + 0xD0, &c.hero.heroid, sizeof(c.hero.heroid), &c.heroIdBytes);
                                        prepB = prepB && p1 && p2;
                                        anyQueuedB = anyQueuedB || p1 || p2;
                                    } else {
                                        Diagnostics::RecordDecryptFailure();
                                    }
                                } else if (c.heroRawBytes == sizeof(c.heroRaw) && !c.heroRaw) {
                                    Diagnostics::RecordDecryptFailure();
                                }
                            }
                            if (c.needsLifeCheck && c.hpPlan.present && c.velPlan.present) {
                                if (c.hpRawBytes == sizeof(c.hpRaw) && c.hpRaw) {
                                    const uintptr_t decHp = static_cast<uintptr_t>(c.hpPlan.presentMask & Cn155284::Component(c.hpRaw, keyA, keyB));
                                    if (decHp >= 0x10000 && decHp < 0x0000800000000000ULL)
                                        c.hpCand = decHp;
                                    else
                                        Diagnostics::RecordDecryptFailure();
                                } else if (c.hpRawBytes == sizeof(c.hpRaw) && !c.hpRaw) {
                                    Diagnostics::RecordDecryptFailure();
                                }
                                if (c.velRawBytes == sizeof(c.velRaw) && c.velRaw) {
                                    const uintptr_t decVel = static_cast<uintptr_t>(c.velPlan.presentMask & Cn155284::Component(c.velRaw, keyA, keyB));
                                    if (decVel >= 0x10000 && decVel < 0x0000800000000000ULL)
                                        c.velCand = decVel;
                                    else
                                        Diagnostics::RecordDecryptFailure();
                                } else if (c.velRawBytes == sizeof(c.velRaw) && !c.velRaw) {
                                    Diagnostics::RecordDecryptFailure();
                                }
                                if (c.hpCand && c.velCand) {
                                    const bool p1 = mem.AddScatterReadRequest(waveScatter, c.hpCand + 0x10, &c.hpType, sizeof(c.hpType), &c.hpTypeBytes);
                                    const bool p2 = mem.AddScatterReadRequest(waveScatter, c.hpCand, &c.hp, sizeof(c.hp), &c.hpBytes);
                                    const bool p3 = mem.AddScatterReadRequest(waveScatter, c.velCand + 0x10, &c.velType, sizeof(c.velType), &c.velTypeBytes);
                                    const bool p4 = mem.AddScatterReadRequest(waveScatter, c.velCand + 0x200, &c.location, sizeof(c.location), &c.locBytes);
                                    prepB = prepB && p1 && p2 && p3 && p4;
                                    anyQueuedB = anyQueuedB || p1 || p2 || p3 || p4;
                                }
                            }
                        }
                    }

                    bool waveBOk = false;
                    if (!prepB) {
                        runWave(false);
                    } else if (anyQueuedB) {
                        waveBOk = runWave(true);
                    } else {
                        waveBOk = hasKeys;
                    }

                    const auto validateLifePayload = [&](CandidateState& c) {
                        if (!c.hpPlan.present || !c.velPlan.present) {
                            c.cleanNonActor = true;
                            trace(4, c.component);
                            c.alive = false;
                            return;
                        }
                        const bool hpValid = c.hpCand != 0 && c.hpTypeBytes == sizeof(c.hpType) && c.hpType == activeOffsets.typeHealth;
                        const bool velValid = c.velCand != 0 && c.velTypeBytes == sizeof(c.velType) && c.velType == TYPE_VELOCITY;
                        if (!hpValid && c.hpCand) Diagnostics::RecordDecryptFailure();
                        if (!velValid && c.velCand) Diagnostics::RecordDecryptFailure();
                        if (!hpValid || !velValid) {
                            transportOrDecryptFailure = true;
                            trace(4, c.component);
                            c.alive = false;
                            return;
                        }
                        if (c.hpBytes != sizeof(c.hp) || c.locBytes != sizeof(c.location)) {
                            transportOrDecryptFailure = true;
                            trace(5, c.component);
                            c.alive = false;
                            return;
                        }
                        HealthPayloadSummary health{};
                        if (!TryParseHealthPayload(c.hp, health) ||
                            ((!discoveries || c.recoveredRemote || currentMapPositiveOnly) && (health.total <= 0 || health.isDead))) {
                            transportOrDecryptFailure = true;
                            trace(6, c.component);
                            c.alive = false;
                            return;
                        }
                        if (!std::isfinite(c.location.x) || !std::isfinite(c.location.y) || !std::isfinite(c.location.z) ||
                            std::fabs(c.location.x) > 100000 || std::fabs(c.location.y) > 100000 || std::fabs(c.location.z) > 100000) {
                            transportOrDecryptFailure = true;
                            trace(7, c.component);
                            c.alive = false;
                            return;
                        }
                    };

                    std::vector<size_t> lateLifeIndices;
                    for (size_t ci = 0; ci < candidates.size(); ++ci) {
                        auto& c = candidates[ci];
                        if (!c.alive || (!c.needsCompHero && !c.needsLifeCheck)) continue;
                        if (!waveBOk) {
                            transportOrDecryptFailure = true;
                            trace(c.needsCompHero ? 6 : 5, c.component);
                            c.alive = false;
                            continue;
                        }
                        if (c.needsCompHero) {
                            if (c.heroRawBytes == sizeof(c.heroRaw) && c.heroRaw == 0) {
                                c.cleanNonActor = true;
                                trace(6, c.component);
                                c.alive = false;
                                continue;
                            }
                            if (!c.heroCand || c.heroTypeBytes != sizeof(c.heroType) || c.heroType != activeOffsets.typeHeroId) {
                                if (c.heroCand) Diagnostics::RecordDecryptFailure();
                                transportOrDecryptFailure = true;
                                trace(6, c.component);
                                c.alive = false;
                                continue;
                            }
                            if (c.heroIdBytes != sizeof(c.hero.heroid)) {
                                transportOrDecryptFailure = true;
                                trace(6, c.component);
                                c.alive = false;
                                continue;
                            }
                            if (!GameData::IsKnownHeroId(c.hero.heroid)) {
                                c.cleanNonActor = true;
                                trace(6, c.component);
                                c.alive = false;
                                continue;
                            }
                            if (!c.header.ReadParentOffset(offset::Entity_MatchId, c.actorMatch)) {
                                transportOrDecryptFailure = true;
                                trace(2, c.component);
                                c.alive = false;
                                continue;
                            }
                            c.hotBody = !c.recoveredRemote && !currentMapPositiveOnly && hotActors &&
                                std::any_of(hotActors->begin(), hotActors->end(), [&](const auto& actor) {
                                    return actor.address == c.component && actor.link == parent &&
                                        actor.match == c.actorMatch && actor.hero == c.hero.heroid;
                                });
                            if (!c.hotBody && !c.needsLifeCheck) {
                                // Rare case: parentHeroBase == 0 and hotActors matched address+link+match,
                                // but hero changed -> defer to wave-batched life check (Wave 4C + 4D).
                                c.hpPlan = PlanCnComponentSlot(c.component, activeOffsets.typeHealth, c.header);
                                c.velPlan = PlanCnComponentSlot(c.component, TYPE_VELOCITY, c.header);
                                if (!c.hpPlan.present || !c.velPlan.present) {
                                    c.cleanNonActor = true;
                                    trace(4, c.component);
                                    c.alive = false;
                                    continue;
                                }
                                lateLifeIndices.push_back(ci);
                                continue;
                            }
                        }
                        if (!c.hotBody && c.needsLifeCheck) {
                            validateLifePayload(c);
                        }
                    }

                    // Wave 4C + 4D: wave-batched health + velocity check for rare hero-switched candidates.
                    if (!lateLifeIndices.empty() && !scatterPoisoned) {
                        bool prepC = true;
                        bool anyQueuedC = false;
                        for (size_t ci : lateLifeIndices) {
                            auto& c = candidates[ci];
                            if (!c.alive) continue;
                            const bool pHp = mem.AddScatterReadRequest(waveScatter, c.hpPlan.entryAddress, &c.hpRaw, sizeof(c.hpRaw), &c.hpRawBytes);
                            const bool pVel = mem.AddScatterReadRequest(waveScatter, c.velPlan.entryAddress, &c.velRaw, sizeof(c.velRaw), &c.velRawBytes);
                            prepC = prepC && pHp && pVel;
                            anyQueuedC = anyQueuedC || pHp || pVel;
                        }
                        if (!prepC || (anyQueuedC && !runWave(true))) {
                            if (!prepC) runWave(false);
                            transportOrDecryptFailure = true;
                            for (size_t ci : lateLifeIndices) {
                                auto& c = candidates[ci];
                                if (!c.alive) continue;
                                trace(4, c.component);
                                c.alive = false;
                            }
                        } else {
                            bool prepD = true;
                            bool anyQueuedD = false;
                            for (size_t ci : lateLifeIndices) {
                                auto& c = candidates[ci];
                                if (!c.alive) continue;
                                if (c.hpRawBytes == sizeof(c.hpRaw) && c.hpRaw) {
                                    const uintptr_t decHp = static_cast<uintptr_t>(c.hpPlan.presentMask & Cn155284::Component(c.hpRaw, keyA, keyB));
                                    if (decHp >= 0x10000 && decHp < 0x0000800000000000ULL)
                                        c.hpCand = decHp;
                                    else
                                        Diagnostics::RecordDecryptFailure();
                                } else if (c.hpRawBytes == sizeof(c.hpRaw) && !c.hpRaw) {
                                    Diagnostics::RecordDecryptFailure();
                                }
                                if (c.velRawBytes == sizeof(c.velRaw) && c.velRaw) {
                                    const uintptr_t decVel = static_cast<uintptr_t>(c.velPlan.presentMask & Cn155284::Component(c.velRaw, keyA, keyB));
                                    if (decVel >= 0x10000 && decVel < 0x0000800000000000ULL)
                                        c.velCand = decVel;
                                    else
                                        Diagnostics::RecordDecryptFailure();
                                } else if (c.velRawBytes == sizeof(c.velRaw) && !c.velRaw) {
                                    Diagnostics::RecordDecryptFailure();
                                }
                                if (c.hpCand && c.velCand) {
                                    const bool p1 = mem.AddScatterReadRequest(waveScatter, c.hpCand + 0x10, &c.hpType, sizeof(c.hpType), &c.hpTypeBytes);
                                    const bool p2 = mem.AddScatterReadRequest(waveScatter, c.hpCand, &c.hp, sizeof(c.hp), &c.hpBytes);
                                    const bool p3 = mem.AddScatterReadRequest(waveScatter, c.velCand + 0x10, &c.velType, sizeof(c.velType), &c.velTypeBytes);
                                    const bool p4 = mem.AddScatterReadRequest(waveScatter, c.velCand + 0x200, &c.location, sizeof(c.location), &c.locBytes);
                                    prepD = prepD && p1 && p2 && p3 && p4;
                                    anyQueuedD = anyQueuedD || p1 || p2 || p3 || p4;
                                }
                            }
                            bool waveDOk = false;
                            if (!prepD) {
                                runWave(false);
                            } else if (anyQueuedD) {
                                waveDOk = runWave(true);
                            } else {
                                waveDOk = true;
                            }
                            for (size_t ci : lateLifeIndices) {
                                auto& c = candidates[ci];
                                if (!c.alive) continue;
                                if (!waveDOk) {
                                    transportOrDecryptFailure = true;
                                    trace(5, c.component);
                                    c.alive = false;
                                    continue;
                                }
                                validateLifePayload(c);
                            }
                        }
                    }
                }
            }
        } else {
            const auto cleanMissingOrNullHero = [&](uint64_t entityParent, const EntityHeaderSnapshot& entityHeader) -> bool {
                if (!headerHasType(entityHeader, activeOffsets.typeHeroId))
                    return true;
                if (isCnLayout) {
                    const auto plan = PlanCnComponentSlot(entityParent, activeOffsets.typeHeroId, entityHeader);
                    if (!plan.present)
                        return true;
                    uint64_t rawSlot = 0;
                    if (SDK->read_range(plan.entryAddress, &rawSlot, sizeof(rawSlot)) && rawSlot == 0)
                        return true;
                }
                return false;
            };
            const auto parentHero = DecryptComponent(parent, activeOffsets.typeHeroId, &header, true);
            uint64_t nonWaveParentHeroId = 0;
            bool nonWaveParentHeroRead = false;
            if (parentHero) {
                nonWaveParentHeroRead = SDK->read_range(parentHero + 0xD0, &nonWaveParentHeroId, sizeof(nonWaveParentHeroId));
                if (!nonWaveParentHeroRead)
                    transportOrDecryptFailure = true;
                else if (!GameData::IsKnownHeroId(nonWaveParentHeroId))
                    parentHeroCleanNonHero = true;
            } else if (unresolvedKind) {
                if (cleanMissingOrNullHero(parent, header))
                    parentHeroCleanNonHero = true;
                else
                    transportOrDecryptFailure = true;
            }
            for (auto& c : candidates) {
                if (!c.alive) continue;
                if (!RuntimeWorkersRunning() || !ProcessConnection::IsConnected()) {
                    c.alive = false;
                    continue;
                }
                if (parentHero) {
                    if (!nonWaveParentHeroRead) {
                        trace(6, c.component);
                        c.alive = false;
                        continue;
                    }
                    c.hero.heroid = nonWaveParentHeroId;
                    if (!GameData::IsKnownHeroId(c.hero.heroid)) {
                        c.cleanNonActor = true;
                        trace(6, c.component);
                        c.alive = false;
                        continue;
                    }
                } else {
                    const auto heroBase = c.component == parent
                        ? uintptr_t{ 0 }
                        : DecryptComponent(c.component, activeOffsets.typeHeroId, &c.header, true);
                    if (!heroBase) {
                        if (unresolvedKind) {
                            if (c.component == parent || cleanMissingOrNullHero(c.component, c.header))
                                c.cleanNonActor = true;
                            else
                                transportOrDecryptFailure = true;
                        }
                        trace(6, c.component);
                        c.alive = false;
                        continue;
                    }
                    if (!SDK->read_range(heroBase + 0xD0, &c.hero.heroid, sizeof(c.hero.heroid))) {
                        transportOrDecryptFailure = true;
                        trace(6, c.component);
                        c.alive = false;
                        continue;
                    }
                    if (!GameData::IsKnownHeroId(c.hero.heroid)) {
                        c.cleanNonActor = true;
                        trace(6, c.component);
                        c.alive = false;
                        continue;
                    }
                }
                if (!c.header.ReadParentOffset(offset::Entity_MatchId, c.actorMatch)) {
                    transportOrDecryptFailure = true;
                    trace(2, c.component);
                    c.alive = false;
                    continue;
                }
                c.hotBody = !c.recoveredRemote && !currentMapPositiveOnly && hotActors &&
                    std::any_of(hotActors->begin(), hotActors->end(), [&](const auto& actor) {
                        return actor.address == c.component && actor.link == parent &&
                            actor.match == c.actorMatch && actor.hero == c.hero.heroid;
                    });
                if (!c.hotBody) {
                    if (!headerHasType(c.header, activeOffsets.typeHealth) || !headerHasType(c.header, TYPE_VELOCITY)) {
                        c.cleanNonActor = true;
                        trace(4, c.component);
                        c.alive = false;
                        continue;
                    }
                    const auto hpBase = DecryptComponent(c.component, activeOffsets.typeHealth, &c.header, true);
                    const auto velocityBase = DecryptComponent(c.component, TYPE_VELOCITY, &c.header, true);
                    if (!hpBase || !velocityBase) {
                        transportOrDecryptFailure = true;
                        trace(4, c.component);
                        c.alive = false;
                        continue;
                    }
                    bool readOk = false;
                    if (useWave && !scatterPoisoned) {
                        DWORD hpB = 0, locB = 0;
                        const bool pHp = mem.AddScatterReadRequest(waveScatter, hpBase, &c.hp, sizeof(c.hp), &hpB);
                        const bool pLoc = mem.AddScatterReadRequest(waveScatter, velocityBase + 0x200, &c.location, sizeof(c.location), &locB);
                        readOk = runWave(pHp && pLoc) && hpB == sizeof(c.hp) && locB == sizeof(c.location);
                    } else {
                        readOk = SDK->read_range(hpBase, &c.hp, sizeof(c.hp)) &&
                            SDK->read_range(velocityBase + 0x200, &c.location, sizeof(c.location));
                    }
                    if (!readOk) {
                        transportOrDecryptFailure = true;
                        trace(5, c.component);
                        c.alive = false;
                        continue;
                    }
                    HealthPayloadSummary health{};
                    if (!TryParseHealthPayload(c.hp, health) ||
                        ((!discoveries || c.recoveredRemote || currentMapPositiveOnly) && (health.total <= 0 || health.isDead))) {
                        transportOrDecryptFailure = true;
                        trace(6, c.component);
                        c.alive = false;
                        continue;
                    }
                    if (!std::isfinite(c.location.x) || !std::isfinite(c.location.y) || !std::isfinite(c.location.z) ||
                        std::fabs(c.location.x) > 100000 || std::fabs(c.location.y) > 100000 || std::fabs(c.location.z) > 100000) {
                        transportOrDecryptFailure = true;
                        trace(7, c.component);
                        c.alive = false;
                        continue;
                    }
                }
            }
        }

        // Recheck + final linkIds stability wave (1 single scatter wave across all surviving candidates).
        bool anyAlive = std::any_of(candidates.begin(), candidates.end(), [](const auto& c) { return c.alive; });
        std::array<uint32_t, 2> batchedCurrentLinkIds{};
        bool batchedLinkIdsAttempted = false;
        bool batchedLinkIdsRead = false;
        if (anyAlive && RuntimeWorkersRunning() && ProcessConnection::IsConnected()) {
            if (useWave && !scatterPoisoned) {
                uint32_t currentMatch = 0;
                DWORD matchBytes = 0, linkIdsBytes = 0;
                bool prepOk = mem.AddScatterReadRequest(waveScatter, parent + offset::Entity_MatchId, &currentMatch, sizeof(currentMatch), &matchBytes);
                if (link && linkIdRead && targetIdRead) {
                    batchedLinkIdsAttempted = true;
                    prepOk = prepOk && mem.AddScatterReadRequest(waveScatter, link + offset::Link_TargetId, batchedCurrentLinkIds.data(), sizeof(batchedCurrentLinkIds), &linkIdsBytes);
                }
                const bool reuseBatchedUniqueId = batchedLinkIdsAttempted &&
                    (offset::Link_UniqueId == offset::Link_TargetId + sizeof(uint32_t));
                for (auto& c : candidates) {
                    if (!c.alive || !c.matchedById)
                        continue;
                    if (!reuseBatchedUniqueId)
                        prepOk = prepOk && mem.AddScatterReadRequest(waveScatter, link + offset::Link_UniqueId, &c.currentId, sizeof(c.currentId), &c.idBytes);
                    prepOk = prepOk && mem.AddScatterReadRequest(waveScatter, c.component + offset::Entity_MatchId, &c.currentComponentId, sizeof(c.currentComponentId), &c.compIdBytes);
                }
                const bool execOk = runWave(prepOk);
                batchedLinkIdsRead = execOk && batchedLinkIdsAttempted && linkIdsBytes == sizeof(batchedCurrentLinkIds);
                const bool parentMatchOk = execOk && matchBytes == sizeof(currentMatch) && currentMatch == match;
                for (auto& c : candidates) {
                    if (!c.alive)
                        continue;
                    const bool uniqueIdOk = reuseBatchedUniqueId
                        ? (batchedLinkIdsRead && batchedCurrentLinkIds[1] == linkId)
                        : (c.idBytes == sizeof(c.currentId) && c.currentId == linkId);
                    const bool recheckOk = parentMatchOk &&
                        (!c.matchedById || (uniqueIdOk &&
                                            c.compIdBytes == sizeof(c.currentComponentId) && c.currentComponentId == c.expectedId));
                    if (!recheckOk) {
                        trace(8, c.component);
                        continue;
                    }
                    result.emplace_back(c.component, parent);
                    if (discoveries) {
                        uint32_t actorMatch = 0;
                        if (!c.header.ReadParentOffset(offset::Entity_MatchId, actorMatch)) {
                            result.pop_back();
                            continue;
                        }
                        if (c.component != parent && actorMatch != linkId && actorMatch != targetId) {
                            result.pop_back();
                            continue;
                        }
                        discoveries->push_back({ c.component, parent, c.hero.heroid, actorMatch, discoveryTick, currentMapPositiveOnly });
                    }
                    trace(0, c.component);
                }
            } else {
                for (auto& c : candidates) {
                    if (!c.alive)
                        continue;
                    uint32_t currentMatch = 0, currentId = 0, currentComponentId = 0;
                    const bool recheckOk = SDK->read_range(parent + offset::Entity_MatchId, &currentMatch, sizeof(currentMatch)) &&
                        currentMatch == match &&
                        (!c.matchedById || (SDK->read_range(link + offset::Link_UniqueId, &currentId, sizeof(currentId)) && currentId == linkId &&
                                            SDK->read_range(c.component + offset::Entity_MatchId, &currentComponentId, sizeof(currentComponentId)) && currentComponentId == c.expectedId));
                    if (!recheckOk) {
                        trace(8, c.component);
                        continue;
                    }
                    result.emplace_back(c.component, parent);
                    if (discoveries) {
                        uint32_t actorMatch = 0;
                        if (!c.header.ReadParentOffset(offset::Entity_MatchId, actorMatch)) {
                            result.pop_back();
                            continue;
                        }
                        if (c.component != parent && actorMatch != linkId && actorMatch != targetId) {
                            result.pop_back();
                            continue;
                        }
                        discoveries->push_back({ c.component, parent, c.hero.heroid, actorMatch, discoveryTick, currentMapPositiveOnly });
                    }
                    trace(0, c.component);
                }
            }
        }

        if(!result.empty()) {
            allBindingsResolved=allBindingsResolved && std::all_of(resolvedComponents.begin(),resolvedComponents.end(),[&](auto component) {
                return std::find(result.begin(),result.end(),std::make_pair(component,parent))!=result.end();
            });
            if(!AcceptEntityRegistry(topology.registry)){if(discoveries)discoveries->clear();return {};}
            bool idsStable = true;
            if (link) {
                if (batchedLinkIdsAttempted) {
                    idsStable = linkIdRead && targetIdRead && batchedLinkIdsRead && batchedCurrentLinkIds == linkIds;
                } else {
                    std::array<uint32_t, 2> currentLinkIds{};
                    idsStable = linkIdRead && targetIdRead &&
                        SDK->read_range(link + offset::Link_TargetId, currentLinkIds.data(), sizeof(currentLinkIds)) &&
                        currentLinkIds == linkIds;
                }
                if (!idsStable) { if (discoveries) discoveries->clear(); trace(8); return {}; }
            } else if (!noLinkSelfActor) {
                idsStable = false;
            }
            if (!contextBindings.empty()) {
                if (waveScatter && !scatterPoisoned && (g_probeWaveScatterEnabled || contextBindings.size() > 1)) {
                    std::vector<CnLinkTargetMapBatchRequest> mapReqs;
                    mapReqs.reserve(contextBindings.size());
                    for (const auto& [idOffset, target] : contextBindings) {
                        const bool needProof =
                            std::any_of(recoveredBindings.begin(), recoveredBindings.end(),
                                [&](const auto& b) { return b.idOffset == idOffset && b.target == target; }) ||
                            std::any_of(positiveMapGuards.begin(), positiveMapGuards.end(),
                                [&](const auto& g) { return g.first == idOffset; });
                        mapReqs.push_back({ link, idOffset, needProof });
                    }
                    bool poisoned = false;
                    const auto mapResults = ResolveCnLinkTargetMapBatch(mapReqs, waveScatter, false, &poisoned);
                    if (poisoned) {
                        scatterPoisoned = true;
                        if (waveScatterPoisoned)
                            *waveScatterPoisoned = true;
                    }
                    for (size_t idx = 0; idx < contextBindings.size(); ++idx) {
                        const auto& [idOffset, target] = contextBindings[idx];
                        const auto recovered = std::find_if(recoveredBindings.begin(), recoveredBindings.end(),
                            [&](const auto& binding) { return binding.idOffset == idOffset && binding.target == target; });
                        const auto positiveGuard = std::find_if(positiveMapGuards.begin(), positiveMapGuards.end(),
                            [&](const auto& guard) { return guard.first == idOffset; });
                        const auto& res = mapResults[idx];
                        if (res.target != target ||
                            (recovered != recoveredBindings.end() && res.verifiedMap != recovered->map) ||
                            (positiveGuard != positiveMapGuards.end() && res.verifiedMap != positiveGuard->second)) {
                            if (discoveries) discoveries->clear();
                            trace(8);
                            return {};
                        }
                    }
                } else {
                    for (const auto& [idOffset, target] : contextBindings) {
                        const auto recovered = std::find_if(recoveredBindings.begin(), recoveredBindings.end(), [&](const auto& binding) {
                            return binding.idOffset == idOffset && binding.target == target;
                        });
                        CnLinkTargetMapIdentity currentMap;
                        const auto positiveGuard = std::find_if(positiveMapGuards.begin(), positiveMapGuards.end(),
                            [&](const auto& guard) { return guard.first == idOffset; });
                        const auto currentTarget = ResolveCnLinkTargetMapBase(link, idOffset, nullptr,
                            recovered != recoveredBindings.end() || positiveGuard != positiveMapGuards.end() ? &currentMap : nullptr);
                        if (currentTarget != target || (recovered != recoveredBindings.end() && currentMap != recovered->map) ||
                            (positiveGuard != positiveMapGuards.end() && currentMap != positiveGuard->second)) {
                            if (discoveries) discoveries->clear();
                            trace(8);
                            return {};
                        }
                    }
                }
            }
            allBindingsResolved=allBindingsResolved && idsStable && !transportOrDecryptFailure;
            if(currentMapPositiveOnly)allBindingsResolved=false;
            if(allBindingsResolved && bindingMembers) {
                if (noLinkSelfActor)
                    bindingMembers->emplace_back(parent, parent);
                else
                    for (auto component : resolvedComponents) bindingMembers->emplace_back(component, parent);
            }
            if(bindingComplete)*bindingComplete=allBindingsResolved;
        } else if (unresolvedKind && !currentMapPositiveOnly &&
            !transportOrDecryptFailure && !ambiguousMapFailed &&
            !candidates.empty() && AcceptEntityRegistry(topology.registry)) {
            const bool allCleanNonActors = std::all_of(candidates.begin(), candidates.end(),
                [](const CandidateState& c) { return c.cleanNonActor; });
            if (allCleanNonActors && (allBindingsResolved || (topology.member_ids_complete && parentHeroCleanNonHero))) {
                *unresolvedKind = PriorityProbeUnresolvedKind::DefinitiveNonActor;
            }
        }
        return result;
    }

    enum class EntityScanPhase { Slots, Headers, Pairing };
    struct EntityScanProgress {
        EntityScanPhase phase;
        const std::vector<std::pair<uint64_t,uint64_t>>* pairs;
    };
    struct EntityScanTask {
        struct promise_type {
            std::vector<std::pair<uint64_t,uint64_t>> result;
            EntityScanProgress progress{EntityScanPhase::Slots,nullptr};
            std::exception_ptr failure;
            EntityScanTask get_return_object() { return EntityScanTask{std::coroutine_handle<promise_type>::from_promise(*this)}; }
            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_always final_suspend() noexcept { return {}; }
            std::suspend_always yield_value(EntityScanProgress value) noexcept { progress=value; return {}; }
            void return_value(std::vector<std::pair<uint64_t,uint64_t>> value) { result=std::move(value); progress.pairs=&result; }
            void unhandled_exception() noexcept { failure=std::current_exception(); }
        };
        std::coroutine_handle<promise_type> handle;
        explicit EntityScanTask(std::coroutine_handle<promise_type> value):handle(value) {}
        EntityScanTask(EntityScanTask&& other) noexcept:handle(std::exchange(other.handle,{})) {}
        EntityScanTask(const EntityScanTask&)=delete;
        ~EntityScanTask() { if(handle)handle.destroy(); }
        bool Done() const { return handle.done(); }
        void Resume() { if(!Done())handle.resume(); if(handle.promise().failure)std::rethrow_exception(handle.promise().failure); }
        const auto& Pairs() const { return handle.promise().progress.pairs ? *handle.promise().progress.pairs : handle.promise().result; }
    };
    // Shared by full scan and priority binding refresh on the acquisition thread.
    inline EntityTopologySample entityScanTopology;
    inline std::unordered_map<uint64_t,EntityHeaderSnapshot> entityScanHeaders;

    inline EntityScanTask BeginEntityScan() {
        entityScanComplete=false;
        entityScanReadComplete=false;
        entityScanStatus=EntityRegistry::ScanStatus::Unavailable;
        entityScanRegistry={};
        SDK->BeginFrame(Diagnostics::SdkFrameSource::Scan);
        const Diagnostics::ScopedDmaCallsite scannerCallsite(Diagnostics::DmaCallsite::EntityScan);
        (void)scannerCallsite;

        std::vector<std::pair<uint64_t, uint64_t>> result;

        struct Entity {
            uint64_t entity;
            uint64_t pad;
        };

        struct EntityScanRecord {
            uint64_t entity = 0;
            uint32_t unique_id = 0;
            uint64_t ptr = 0;
            uint64_t entity_id = 0;
            EntityHeaderSnapshot header{};
        };

        const Diagnostics::DmaReadStats scanDmaStart = Diagnostics::SnapshotDmaReadStats();
        const bool scanDmaRangeDiagEnabled =
            EntityScannerEnvFlagEnabled("UN_DMA_SCAN_DMA_RANGE_DIAG");
        const size_t scanDmaRangeStart =
            scanDmaRangeDiagEnabled ? Diagnostics::DmaSampleCursor() : 0;
        Diagnostics::EntityScanDetailStats scanDetail{};
        scanDetail.scanDmaRangeDiagEnabled = scanDmaRangeDiagEnabled;
        auto elapsedMs = [](std::chrono::steady_clock::time_point startedAt) {
            return static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - startedAt).count()) / 1000.0;
        };
        auto finishScanDetail = [&]() {
            scanDetail.registryGeneration=entityScanRegistry.generation;
            scanDetail.registryCapacity=entityScanRegistry.capacity;
            scanDetail.plannedBytes=entityScanRegistry.Bytes();
            scanDetail.registryScanStatus=static_cast<uint8_t>(entityScanStatus);
            const auto finalizeStartedAt = std::chrono::steady_clock::now();
            const Diagnostics::DmaReadStats scanDmaEnd = Diagnostics::SnapshotDmaReadStats();
            scanDetail.scanDmaReadsDelta = scanDmaEnd.total >= scanDmaStart.total
                ? scanDmaEnd.total - scanDmaStart.total
                : 0;
            scanDetail.scanDmaFailDelta = scanDmaEnd.failed >= scanDmaStart.failed
                ? scanDmaEnd.failed - scanDmaStart.failed
                : 0;
            if (scanDmaRangeDiagEnabled) {
                const Diagnostics::DmaWindowStats scanDmaRange =
                    Diagnostics::GetDmaRangeStats(
                        scanDmaRangeStart,
                        Diagnostics::DmaSampleCursor());
                scanDetail.scanDmaRangeReads = scanDmaRange.totalReads;
                scanDetail.scanDmaRangeFailed = scanDmaRange.failedReads;
                scanDetail.scanDmaRangeMaxLatencyUs = scanDmaRange.maxLatencyUs;
                uint64_t maxCallsiteLatencyUs = 0;
                int maxCallsite = 0;
                for (int i = 0; i < static_cast<int>(Diagnostics::DmaCallsite::Count); ++i) {
                    if (scanDmaRange.perCallsiteMaxUs[i] > maxCallsiteLatencyUs) {
                        maxCallsiteLatencyUs = scanDmaRange.perCallsiteMaxUs[i];
                        maxCallsite = i;
                    }
                }
                scanDetail.scanDmaRangeMaxCallsite =
                    static_cast<uint8_t>(maxCallsite);
                auto isScannerCallsite = [](Diagnostics::DmaCallsite callsite) {
                    switch (callsite) {
                    case Diagnostics::DmaCallsite::EntityScan:
                    case Diagnostics::DmaCallsite::EntityScanRoot:
                    case Diagnostics::DmaCallsite::EntityScanListRead:
                    case Diagnostics::DmaCallsite::EntityScanRecordBuild:
                    case Diagnostics::DmaCallsite::EntityScanRecordMatchId:
                    case Diagnostics::DmaCallsite::EntityScanRecordHeader:
                    case Diagnostics::DmaCallsite::EntityScanRecordPoolPtr:
                    case Diagnostics::DmaCallsite::EntityScanRecordPoolId:
                    case Diagnostics::DmaCallsite::EntityScanMatchLink:
                    case Diagnostics::DmaCallsite::EntityScanTargetMap:
                    case Diagnostics::DmaCallsite::EntityScanMapCandidate:
                    case Diagnostics::DmaCallsite::EntityScanLinkTargetResolve:
                    case Diagnostics::DmaCallsite::EntityScanSelfValidation:
                    case Diagnostics::DmaCallsite::EntityScanComponentValidation:
                        return true;
                    default:
                        return false;
                    }
                };
                for (int i = 0; i < static_cast<int>(Diagnostics::DmaCallsite::Count); ++i) {
                    const uint64_t reads = scanDmaRange.perCallsiteReads[i];
                    if (reads == 0)
                        continue;
                    const uint64_t maxUs = scanDmaRange.perCallsiteMaxUs[i];
                    const auto callsite = static_cast<Diagnostics::DmaCallsite>(i);
                    if (isScannerCallsite(callsite)) {
                        scanDetail.scanDmaRangeScannerReads += reads;
                        if (maxUs > scanDetail.scanDmaRangeScannerMaxLatencyUs) {
                            scanDetail.scanDmaRangeScannerMaxLatencyUs = maxUs;
                            scanDetail.scanDmaRangeScannerMaxCallsite =
                                static_cast<uint8_t>(i);
                        }
                    } else {
                        scanDetail.scanDmaRangeForeignReads += reads;
                        if (maxUs > scanDetail.scanDmaRangeForeignMaxLatencyUs) {
                            scanDetail.scanDmaRangeForeignMaxLatencyUs = maxUs;
                            scanDetail.scanDmaRangeForeignMaxCallsite =
                                static_cast<uint8_t>(i);
                        }
                    }
                }
                auto callsiteMax = [&](Diagnostics::DmaCallsite callsite) {
                    return scanDmaRange.perCallsiteMaxUs[static_cast<int>(callsite)];
                };
                scanDetail.scanDmaRangeRootMaxUs =
                    callsiteMax(Diagnostics::DmaCallsite::EntityScanRoot);
                scanDetail.scanDmaRangeListReadMaxUs =
                    callsiteMax(Diagnostics::DmaCallsite::EntityScanListRead);
                scanDetail.scanDmaRangeRecordHeaderMaxUs =
                    callsiteMax(Diagnostics::DmaCallsite::EntityScanRecordHeader);
                scanDetail.scanDmaRangeRecordPoolIdMaxUs =
                    callsiteMax(Diagnostics::DmaCallsite::EntityScanRecordPoolId);
                scanDetail.scanDmaRangeTargetMapMaxUs =
                    callsiteMax(Diagnostics::DmaCallsite::EntityScanTargetMap);
                scanDetail.scanDmaRangeComponentValidationMaxUs =
                    callsiteMax(Diagnostics::DmaCallsite::EntityScanComponentValidation);
                scanDetail.scanDmaRangeViewMatrixMaxUs =
                    callsiteMax(Diagnostics::DmaCallsite::ViewMatrix);
            }
            scanDetail.scanFinalizeMs += elapsedMs(finalizeStartedAt);
            Diagnostics::SetEntityScanDetailStats(scanDetail);
        };

        auto isDynamicEntityId = [](uint64_t entity_id) {
            return V2::WorldObjects::ObjectIndex(entity_id) >= 0;
        };

        const bool cnProfile = offset::UsesProtectedEntityLayout();
        const DWORD rootCacheTtlMs =
            cnProfile && !DynamicEntityRegistryEnabled() ? EntityScannerEntityListRootCacheTtlMs() : 0;
        const bool rootCacheEnabled = cnProfile && rootCacheTtlMs > 0;
        scanDetail.cnEntityListRootCacheEnabled = rootCacheEnabled;
        scanDetail.cnEntityListRootCacheTtlMs = rootCacheTtlMs;
        const DWORD staleMetadataMs =
            cnProfile ? EntityScannerStaleMetadataMs() : 0;
        scanDetail.cnScannerStaleMetadataMs = staleMetadataMs;
        const bool staleMetadataOnly =
            cnProfile &&
            staleMetadataMs > 0 &&
            EntityScannerEnvFlagEnabled("UN_DMA_CN_SCANNER_STALE_METADATA_ONLY");
        scanDetail.cnScannerStaleMetadataOnlyEnabled = staleMetadataOnly;

        static uint64_t cachedCnEntityListRootModuleBase = 0;
        static uint64_t cachedCnEntityListRootPointer = 0;
        static DWORD cachedCnEntityListRootTick = 0;
        static uint64_t cachedRootEpoch = 0;
        EntityRegistryScanGuard registryGuard;
        entityScanRegistry=registryGuard.snapshot;
        if(!registryGuard.valid){finishScanDetail();co_return result;}
        const uint64_t scanEpoch = registryGuard.dynamic?registryGuard.snapshot.generation:ProcessConnection::ConnectionEpoch();
        if (cachedRootEpoch != scanEpoch || (rootCacheEnabled &&
            cachedCnEntityListRootModuleBase != 0 &&
            cachedCnEntityListRootModuleBase != SDK->dwGameBase)) {
            cachedRootEpoch = scanEpoch;
            cachedCnEntityListRootModuleBase = 0;
            cachedCnEntityListRootPointer = 0;
            cachedCnEntityListRootTick = 0;
        }

        const auto rootPhaseStartedAt = std::chrono::steady_clock::now();
        uint64_t entity_list = 0;
        const DWORD rootCacheNowTick = static_cast<DWORD>(EntityRegistryNowMs());
        const bool rootCacheContextMatch =
            rootCacheEnabled &&
            cachedCnEntityListRootModuleBase == SDK->dwGameBase &&
            cachedCnEntityListRootPointer != 0;
        const DWORD rootCacheAgeMs =
            rootCacheContextMatch ?
                (rootCacheNowTick - cachedCnEntityListRootTick) : 0;
        if(registryGuard.dynamic) {
            entity_list=registryGuard.snapshot.root;
        } else if (rootCacheContextMatch &&
            rootCacheAgeMs <= rootCacheTtlMs) {
            entity_list = cachedCnEntityListRootPointer;
            scanDetail.cnEntityListRootCacheHitCount = 1;
        } else if (
            rootCacheContextMatch &&
            staleMetadataMs > 0 &&
            (staleMetadataOnly ||
             static_cast<uint64_t>(rootCacheAgeMs) <=
                static_cast<uint64_t>(rootCacheTtlMs) + staleMetadataMs)) {
            entity_list = cachedCnEntityListRootPointer;
            scanDetail.cnEntityListRootCacheExpiredCount = 1;
            scanDetail.cnEntityListRootCacheStaleHitCount = 1;
        } else {
            if (rootCacheContextMatch) {
                scanDetail.cnEntityListRootCacheExpiredCount = 1;
            }
            scanDetail.cnEntityListRootCacheReadCount = 1;
            const Diagnostics::ScopedDmaCallsite rootCallsite(
                Diagnostics::DmaCallsite::EntityScanRoot);
            entity_list = SDK->RPM<uint64_t>(
                SDK->dwGameBase + offset::Active().Address_entity_base);
            if (rootCacheEnabled && entity_list != 0) {
                cachedCnEntityListRootModuleBase = SDK->dwGameBase;
                cachedCnEntityListRootPointer = entity_list;
                cachedCnEntityListRootTick = rootCacheNowTick;
                scanDetail.cnEntityListRootCacheStoreCount = 1;
            }
        }
        scanDetail.entityList = entity_list;
        if (!entity_list) {
            scanDetail.scanRootMs += elapsedMs(rootPhaseStartedAt);
            finishScanDetail();
            Diagnostics::Trace("Entity scan skipped: entity list pointer is null.");
            co_return result;
        }

        std::vector<uint64_t> candidateParents;
        struct RecordMetadata { uint64_t plus8=0, poolId=0; uint32_t wrapperId=0; bool poolRead=false; };
        std::unordered_map<uint64_t,RecordMetadata> recordMetadata;
        std::vector<EntityScanRecord> records{};
        std::unordered_map<uint32_t, std::vector<uint64_t>> match_index{};
        std::unordered_set<uint64_t> seen_entities{};
        records.reserve(4096);
        match_index.reserve(4096);
        seen_entities.reserve(4096);

        const size_t kEntityListReadSize =
            registryGuard.dynamic ? registryGuard.snapshot.Bytes() : (cnProfile ? 0x40000 : 0x1E000);
        const size_t kEntityListChunkSize =
            cnProfile ? EntityScannerCnEntityListChunkSize() : 0x1000;
        scanDetail.cnEntityListChunkSize = cnProfile ? kEntityListChunkSize : 0;
        constexpr size_t kEntityListFallbackChunkSize = 0x1000;
        const bool lightScanRequested = EntityLightScanRequested();
        const bool lightScan = lightScanRequested;
        const size_t lightScanMaxCandidates =
            lightScan ? EntityLightScanMaxCandidates() : 0;
        const bool cnMapCandidateCacheEnabled = cnProfile;
        scanDetail.cnMapCandidateCacheEnabled = cnMapCandidateCacheEnabled;
        const bool cnMapDiagEnabled =
            cnProfile && EntityScannerEnvFlagEnabled("UN_DMA_CN_MAP_DIAG");
        scanDetail.cnMapDiagEnabled = cnMapDiagEnabled;
        const bool cnRecordMatchIdFromHeaderEnabled = cnProfile;
        scanDetail.cnRecordMatchIdFromHeaderEnabled = cnRecordMatchIdFromHeaderEnabled;
        static uint64_t cachedModuleBase = 0;
        static uint64_t cachedModuleSize = 0;
        static uint64_t cachedModuleEpoch = 0;
        if (cnProfile && (cachedModuleEpoch != scanEpoch || cachedModuleBase != SDK->dwGameBase)) {
            cachedModuleEpoch = scanEpoch;
            cachedModuleBase = SDK->dwGameBase;
            cachedModuleSize = static_cast<uint64_t>(mem.GetBaseSize("Overwatch.exe"));
        }
        const uint64_t moduleSize = cnProfile ? cachedModuleSize : 0;
        scanDetail.scanRootMs += elapsedMs(rootPhaseStartedAt);

        struct PersistentCnRecordSnapshotCacheEntry {
            uint64_t entityList = 0;
            uint64_t moduleBase = 0;
            uint64_t moduleSize = 0;
            DWORD updateTick = 0;
            EntityScanRecord record{};
        };
        static std::unordered_map<uint64_t, PersistentCnRecordSnapshotCacheEntry>
            persistentCnRecordSnapshotCache{};
        static uint64_t persistentCnRecordSnapshotEpoch = 0;
        if (persistentCnRecordSnapshotEpoch != scanEpoch) {
            persistentCnRecordSnapshotCache.clear();
            persistentCnRecordSnapshotEpoch = scanEpoch;
        }
        static uint64_t persistentCnRecordSnapshotEntityList = 0;
        static uint64_t persistentCnRecordSnapshotModuleBase = 0;
        static uint64_t persistentCnRecordSnapshotModuleSize = 0;
        const DWORD recordSnapshotCacheTtlMs =
            cnProfile ? EntityScannerRecordSnapshotCacheTtlMs() : 0;
        const bool recordSnapshotCacheEnabled =
            cnProfile && recordSnapshotCacheTtlMs > 0;
        const size_t recordSnapshotCacheRefreshBudget =
            recordSnapshotCacheEnabled ? EntityScannerRecordSnapshotCacheRefreshBudget() : 0;
        const bool recordSnapshotCacheRefreshBudgetEnabled =
            recordSnapshotCacheRefreshBudget > 0;
        size_t recordSnapshotCacheRefreshCount = 0;
        scanDetail.cnRecordSnapshotCacheEnabled = recordSnapshotCacheEnabled;
        scanDetail.cnRecordSnapshotCacheTtlMs = recordSnapshotCacheTtlMs;
        scanDetail.cnRecordSnapshotCacheRefreshBudget =
            recordSnapshotCacheRefreshBudget;
        if (recordSnapshotCacheEnabled &&
            (persistentCnRecordSnapshotEntityList != entity_list ||
             persistentCnRecordSnapshotModuleBase != SDK->dwGameBase ||
             persistentCnRecordSnapshotModuleSize != moduleSize)) {
            persistentCnRecordSnapshotCache.clear();
            persistentCnRecordSnapshotEntityList = entity_list;
            persistentCnRecordSnapshotModuleBase = SDK->dwGameBase;
            persistentCnRecordSnapshotModuleSize = moduleSize;
        }
        if (recordSnapshotCacheEnabled &&
            persistentCnRecordSnapshotCache.size() > 8192) {
            persistentCnRecordSnapshotCache.clear();
        }

        static std::unordered_map<uint64_t, DWORD>
            persistentCnEntityListReadNegativeCache{};
        static uint64_t persistentCnEntityListReadNegativeCacheEpoch = 0;
        if (persistentCnEntityListReadNegativeCacheEpoch != scanEpoch) {
            persistentCnEntityListReadNegativeCache.clear();
            persistentCnEntityListReadNegativeCacheEpoch = scanEpoch;
        }
        static uint64_t persistentCnEntityListReadNegativeCacheEntityList = 0;
        static uint64_t persistentCnEntityListReadNegativeCacheModuleBase = 0;
        const DWORD listReadNegativeCacheTtlMs =
            cnProfile ? EntityScannerEntityListReadNegativeCacheTtlMs() : 0;
        const bool listReadNegativeCacheEnabled =
            cnProfile && listReadNegativeCacheTtlMs > 0;
        scanDetail.cnEntityListReadNegativeCacheEnabled =
            listReadNegativeCacheEnabled;
        scanDetail.cnEntityListReadNegativeCacheTtlMs =
            listReadNegativeCacheTtlMs;
        if (listReadNegativeCacheEnabled &&
            (persistentCnEntityListReadNegativeCacheEntityList != entity_list ||
             persistentCnEntityListReadNegativeCacheModuleBase != SDK->dwGameBase)) {
            persistentCnEntityListReadNegativeCache.clear();
            persistentCnEntityListReadNegativeCacheEntityList = entity_list;
            persistentCnEntityListReadNegativeCacheModuleBase = SDK->dwGameBase;
        }
        if (listReadNegativeCacheEnabled &&
            persistentCnEntityListReadNegativeCache.size() > 512) {
            persistentCnEntityListReadNegativeCache.clear();
        }
        auto entityListReadNegativeCacheKey = [](size_t offset, size_t size) {
            return static_cast<uint64_t>(offset) ^
                (static_cast<uint64_t>(size) << 32);
        };
        auto shouldSkipEntityListRead = [&](size_t offset, size_t size) {
            if (!listReadNegativeCacheEnabled)
                return false;
            ++scanDetail.cnEntityListReadNegativeCacheLookupCount;
            const uint64_t key = entityListReadNegativeCacheKey(offset, size);
            auto it = persistentCnEntityListReadNegativeCache.find(key);
            if (it == persistentCnEntityListReadNegativeCache.end())
                return false;
            const DWORD now = static_cast<DWORD>(EntityRegistryNowMs());
            const DWORD ageMs = now - it->second;
            const bool fresh = ageMs <= listReadNegativeCacheTtlMs;
            const bool staleAllowed =
                !fresh &&
                staleMetadataMs > 0 &&
                (staleMetadataOnly ||
                 static_cast<uint64_t>(ageMs) <=
                    static_cast<uint64_t>(listReadNegativeCacheTtlMs) +
                        staleMetadataMs);
            if (fresh || staleAllowed) {
                ++scanDetail.cnEntityListReadNegativeCacheHitCount;
                if (staleAllowed)
                    ++scanDetail.cnEntityListReadNegativeCacheStaleHitCount;
                ++scanDetail.listReadSkippedCount;
                return true;
            }
            ++scanDetail.cnEntityListReadNegativeCacheExpiredCount;
            persistentCnEntityListReadNegativeCache.erase(it);
            return false;
        };
        auto rememberEntityListReadFailure = [&](size_t offset, size_t size) {
            if (!listReadNegativeCacheEnabled)
                return;
            persistentCnEntityListReadNegativeCache.insert_or_assign(
                entityListReadNegativeCacheKey(offset, size),
                static_cast<DWORD>(EntityRegistryNowMs()));
            ++scanDetail.cnEntityListReadNegativeCacheStoreCount;
        };

        struct PersistentCnEntityListReadCacheEntry {
            DWORD updateTick = 0;
            std::vector<uint8_t> bytes{};
        };
        static std::unordered_map<uint64_t, PersistentCnEntityListReadCacheEntry>
            persistentCnEntityListReadCache{};
        static uint64_t persistentCnEntityListReadCacheEpoch = 0;
        if (persistentCnEntityListReadCacheEpoch != scanEpoch) {
            persistentCnEntityListReadCache.clear();
            persistentCnEntityListReadCacheEpoch = scanEpoch;
        }
        static uint64_t persistentCnEntityListReadCacheEntityList = 0;
        static uint64_t persistentCnEntityListReadCacheModuleBase = 0;
        const DWORD listReadCacheTtlMs =
            cnProfile ? EntityScannerEntityListReadCacheTtlMs() : 0;
        const bool listReadCacheEnabled =
            cnProfile && listReadCacheTtlMs > 0;
        scanDetail.cnEntityListReadCacheEnabled = listReadCacheEnabled;
        scanDetail.cnEntityListReadCacheTtlMs = listReadCacheTtlMs;
        if (listReadCacheEnabled &&
            (persistentCnEntityListReadCacheEntityList != entity_list ||
             persistentCnEntityListReadCacheModuleBase != SDK->dwGameBase)) {
            persistentCnEntityListReadCache.clear();
            persistentCnEntityListReadCacheEntityList = entity_list;
            persistentCnEntityListReadCacheModuleBase = SDK->dwGameBase;
        }
        if (listReadCacheEnabled &&
            persistentCnEntityListReadCache.size() > 512) {
            persistentCnEntityListReadCache.clear();
        }
        auto loadEntityListReadCache =
            [&](size_t offset, size_t size, std::vector<uint8_t>& destination) {
                if (!listReadCacheEnabled)
                    return false;
                ++scanDetail.cnEntityListReadCacheLookupCount;
                const uint64_t key = entityListReadNegativeCacheKey(offset, size);
                auto it = persistentCnEntityListReadCache.find(key);
                if (it == persistentCnEntityListReadCache.end())
                    return false;

                const DWORD now = static_cast<DWORD>(EntityRegistryNowMs());
                PersistentCnEntityListReadCacheEntry& entry = it->second;
                const DWORD ageMs = now - entry.updateTick;
                if (entry.bytes.size() != size) {
                    ++scanDetail.cnEntityListReadCacheExpiredCount;
                    persistentCnEntityListReadCache.erase(it);
                    return false;
                }

                if (ageMs > listReadCacheTtlMs) {
                    ++scanDetail.cnEntityListReadCacheExpiredCount;
                    if (staleMetadataMs == 0 ||
                        (!staleMetadataOnly &&
                         static_cast<uint64_t>(ageMs) >
                            static_cast<uint64_t>(listReadCacheTtlMs) +
                                staleMetadataMs)) {
                        persistentCnEntityListReadCache.erase(it);
                        return false;
                    }

                    std::memcpy(destination.data(), entry.bytes.data(), size);
                    ++scanDetail.cnEntityListReadCacheHitCount;
                    ++scanDetail.cnEntityListReadCacheStaleHitCount;
                    return true;
                }

                std::memcpy(destination.data(), entry.bytes.data(), size);
                ++scanDetail.cnEntityListReadCacheHitCount;
                return true;
            };
        auto rememberEntityListReadSuccess =
            [&](size_t offset, size_t size, const std::vector<uint8_t>& source) {
                if (!listReadCacheEnabled)
                    return;
                PersistentCnEntityListReadCacheEntry entry{};
                entry.updateTick = static_cast<DWORD>(EntityRegistryNowMs());
                entry.bytes.assign(source.begin(), source.begin() + size);
                persistentCnEntityListReadCache.insert_or_assign(
                    entityListReadNegativeCacheKey(offset, size),
                    std::move(entry));
                ++scanDetail.cnEntityListReadCacheStoreCount;
            };

        auto addRecord = [&](uint64_t possible_common) {
            const auto recordBuildStartedAt = std::chrono::steady_clock::now();
            const Diagnostics::ScopedDmaCallsite recordBuildCallsite(
                Diagnostics::DmaCallsite::EntityScanRecordBuild);
            ++scanDetail.recordAddAttemptCount;
            auto finishRecordBuild = [&]() {
                scanDetail.scanRecordBuildMs += elapsedMs(recordBuildStartedAt);
            };
            if (!possible_common)
            {
                finishRecordBuild();
                return;
            }
            if (cnProfile &&
                !IsLikelyCnEntityParent(
                    possible_common,
                    entity_list,
                    kEntityListReadSize,
                    SDK->dwGameBase,
                    moduleSize)) {
                finishRecordBuild();
                return;
            }

            if (recordSnapshotCacheEnabled) {
                ++scanDetail.cnRecordSnapshotCacheLookupCount;
                const DWORD now = static_cast<DWORD>(EntityRegistryNowMs());
                auto snapshotIt = persistentCnRecordSnapshotCache.find(possible_common);
                if (snapshotIt != persistentCnRecordSnapshotCache.end()) {
                    const PersistentCnRecordSnapshotCacheEntry& entry =
                        snapshotIt->second;
                    const bool sameContext =
                        entry.entityList == entity_list &&
                        entry.moduleBase == SDK->dwGameBase &&
                        entry.moduleSize == moduleSize;
                    const DWORD ageMs = now - entry.updateTick;
                    const bool fresh = sameContext && ageMs <= recordSnapshotCacheTtlMs;
                    const bool staleMetadataAllowed =
                        sameContext &&
                        !fresh &&
                        staleMetadataMs > 0 &&
                        (staleMetadataOnly || ageMs <= staleMetadataMs);
                    const bool staleAllowed =
                        staleMetadataAllowed ||
                        (sameContext &&
                         !fresh &&
                         recordSnapshotCacheRefreshBudgetEnabled &&
                         recordSnapshotCacheRefreshCount >= recordSnapshotCacheRefreshBudget);
                    if (fresh || staleAllowed) {
                        EntityScanRecord cached = entry.record;
                        if (cached.entity == possible_common && cached.unique_id != 0) {
                            if (!seen_entities.insert(possible_common).second) {
                                ++scanDetail.recordDuplicateCount;
                                finishRecordBuild();
                                return;
                            }
                            ++scanDetail.cnRecordSnapshotCacheHitCount;
                            if (staleAllowed)
                                ++scanDetail.cnRecordSnapshotCacheStaleHitCount;
                            match_index[cached.unique_id].push_back(cached.entity);
                            records.push_back(std::move(cached));
                            finishRecordBuild();
                            return;
                        }
                    }
                    if (sameContext)
                        ++scanDetail.cnRecordSnapshotCacheExpiredCount;
                    persistentCnRecordSnapshotCache.erase(snapshotIt);
                }
            }
            if (recordSnapshotCacheEnabled) {
                ++recordSnapshotCacheRefreshCount;
                ++scanDetail.cnRecordSnapshotCacheRefreshCount;
            }

            EntityScanRecord record{};
            record.entity = possible_common;
            bool recordHeaderLoaded = false;
            auto readRecordHeader = [&]() {
                if (recordHeaderLoaded)
                    return;
                ++scanDetail.recordHeaderReadCount;
                {
                    const Diagnostics::ScopedDmaCallsite headerCallsite(
                        Diagnostics::DmaCallsite::EntityScanRecordHeader);
                    const auto loaded=entityScanHeaders.find(possible_common);
                    if(loaded!=entityScanHeaders.end())record.header=loaded->second;
                    else record.header.Read(possible_common);
                }
                if (!record.header.valid)
                    ++scanDetail.recordHeaderFailCount;
                recordHeaderLoaded = true;
            };

            bool headerMatchIdObserved = false;
            bool headerHasUniqueId = false;
            uint32_t headerUniqueId = 0;
            auto observeHeaderMatchId = [&](uint32_t compareValue, bool haveCompareValue) {
                if (headerMatchIdObserved)
                    return;
                headerHasUniqueId =
                    record.header.ReadParentOffset(offset::Entity_MatchId, headerUniqueId);
                if (headerHasUniqueId) {
                    ++scanDetail.recordMatchIdHeaderHitCount;
                    if (haveCompareValue) {
                        if (headerUniqueId == compareValue)
                            ++scanDetail.recordMatchIdHeaderMatchCount;
                        else
                            ++scanDetail.recordMatchIdHeaderMismatchCount;
                    }
                } else {
                    ++scanDetail.recordMatchIdHeaderMissCount;
                }
                headerMatchIdObserved = true;
            };

            uint32_t cnUniqueId = 0;
            const auto objectMetadata = recordMetadata.find(possible_common);
            const bool knownObject = objectMetadata != recordMetadata.end() && objectMetadata->second.poolRead &&
                isDynamicEntityId(objectMetadata->second.poolId);
            if (cnProfile && cnRecordMatchIdFromHeaderEnabled) {
                readRecordHeader();
                observeHeaderMatchId(0, false);
                if (headerHasUniqueId && headerUniqueId != 0) {
                    cnUniqueId = headerUniqueId;
                    ++scanDetail.recordMatchIdHeaderUseCount;
                } else {
                    ++scanDetail.recordRemoteFallbackReadCount;
                    ++scanDetail.recordMatchIdDirectReadCount;
                    {
                        const Diagnostics::ScopedDmaCallsite matchIdCallsite(
                            Diagnostics::DmaCallsite::EntityScanRecordMatchId);
                        cnUniqueId = SDK->RPM<uint32_t>(
                            possible_common + offset::Entity_MatchId);
                    }
                    if (cnUniqueId == 0 && !knownObject) {
                        ++scanDetail.recordMatchIdDirectZeroCount;
                        finishRecordBuild();
                        return;
                    }
                }
            } else if (cnProfile) {
                ++scanDetail.recordRemoteFallbackReadCount;
                ++scanDetail.recordMatchIdDirectReadCount;
                {
                    const Diagnostics::ScopedDmaCallsite matchIdCallsite(
                        Diagnostics::DmaCallsite::EntityScanRecordMatchId);
                    cnUniqueId = SDK->RPM<uint32_t>(
                        possible_common + offset::Entity_MatchId);
                }
                if (cnUniqueId == 0 && !knownObject) {
                    ++scanDetail.recordMatchIdDirectZeroCount;
                    finishRecordBuild();
                    return;
                }
            }

            if (!seen_entities.insert(possible_common).second) {
                ++scanDetail.recordDuplicateCount;
                finishRecordBuild();
                return;
            }

            readRecordHeader();
            observeHeaderMatchId(cnUniqueId, cnProfile && cnUniqueId != 0);

            if (cnProfile) {
                record.unique_id = cnUniqueId;
            } else if (headerHasUniqueId) {
                record.unique_id = headerUniqueId;
            } else {
                ++scanDetail.recordRemoteFallbackReadCount;
                ++scanDetail.recordMatchIdDirectReadCount;
                const Diagnostics::ScopedDmaCallsite matchIdCallsite(
                    Diagnostics::DmaCallsite::EntityScanRecordMatchId);
                record.unique_id = SDK->RPM<uint32_t>(possible_common + offset::Entity_MatchId);
                if (record.unique_id == 0)
                    ++scanDetail.recordMatchIdDirectZeroCount;
            }

            uint64_t ptr_value = 0;
            if (!record.header.ReadParentOffset(offset::Entity_PoolPtr, ptr_value)) {
                ++scanDetail.recordRemoteFallbackReadCount;
                const Diagnostics::ScopedDmaCallsite poolPtrCallsite(
                    Diagnostics::DmaCallsite::EntityScanRecordPoolPtr);
                ptr_value = SDK->RPM<uint64_t>(possible_common + offset::Entity_PoolPtr);
            }

            record.ptr = ptr_value & 0xFFFFFFFFFFFFFFC0;
            if (record.ptr && record.ptr < 0xFFFFFFFFFFFFFFEF) {
                ++scanDetail.poolIdReadCount;
                const Diagnostics::ScopedDmaCallsite poolIdCallsite(
                    Diagnostics::DmaCallsite::EntityScanRecordPoolId);
                const auto metadata=recordMetadata.find(possible_common);
                record.entity_id = metadata!=recordMetadata.end() && metadata->second.poolRead ? metadata->second.poolId :
                    SDK->RPM<uint64_t>(record.ptr + offset::Pool_PoolId);
            }

            if (record.unique_id != 0)
                match_index[record.unique_id].push_back(record.entity);
            if (recordSnapshotCacheEnabled && record.unique_id != 0) {
                PersistentCnRecordSnapshotCacheEntry entry{};
                entry.entityList = entity_list;
                entry.moduleBase = SDK->dwGameBase;
                entry.moduleSize = moduleSize;
                entry.updateTick = static_cast<DWORD>(EntityRegistryNowMs());
                entry.record = record;
                persistentCnRecordSnapshotCache.insert_or_assign(
                    record.entity,
                    entry);
                ++scanDetail.cnRecordSnapshotCacheStoreCount;
            }
            records.push_back(std::move(record));
            finishRecordBuild();
        };

        std::vector<uint8_t> entity_chunk(kEntityListChunkSize);
        size_t readable_bytes = 0;
        size_t readable_chunks = 0;
        size_t slots_scanned = 0;
        size_t nonzero_slots = 0;
        size_t plausible_slots = 0;
        auto processEntityChunk = [&](const std::vector<uint8_t>& chunk,
                                      size_t chunk_size) {
            for (size_t slot_offset = 0; slot_offset + sizeof(Entity) <= chunk_size;
                 slot_offset += sizeof(Entity)) {
                if (!RuntimeWorkersRunning() || !ProcessConnection::IsConnected() || !registryGuard.Check())
                    break;
                const auto slotWalkStartedAt = std::chrono::steady_clock::now();
                uint64_t possible_common = 0;
                std::memcpy(&possible_common, chunk.data() + slot_offset, sizeof(possible_common));
                ++slots_scanned;
                if (possible_common)
                    ++nonzero_slots;
                if (IsPlausibleUserPointer(possible_common))
                    ++plausible_slots;
                scanDetail.scanSlotWalkMs += elapsedMs(slotWalkStartedAt);
                if(IsPlausibleUserPointer(possible_common) && (!cnProfile || IsLikelyCnEntityParent(
                    possible_common,entity_list,kEntityListReadSize,SDK->dwGameBase,moduleSize)))
                    candidateParents.push_back(possible_common);
            }
        };

        for (size_t offset = 0; offset < kEntityListReadSize; offset += kEntityListChunkSize) {
            co_yield EntityScanProgress{EntityScanPhase::Slots, &result};
            if (!RuntimeWorkersRunning() || !ProcessConnection::IsConnected() || !registryGuard.Check())
                break;
            const size_t remaining = kEntityListReadSize - offset;
            const size_t chunk_size =
                registryGuard.dynamic?EntityRegistry::ChunkBytes(registryGuard.snapshot,offset,kEntityListChunkSize):
                (remaining < kEntityListChunkSize ? remaining : kEntityListChunkSize);
            bool chunkRead = !registryGuard.dynamic && loadEntityListReadCache(offset, chunk_size, entity_chunk);
            const bool chunkSkipped = !registryGuard.dynamic && !chunkRead && shouldSkipEntityListRead(offset, chunk_size);
            if (!chunkSkipped) {
                if (!chunkRead) {
                    ++scanDetail.listReadCount;
                    const auto listReadStartedAt = std::chrono::steady_clock::now();
                    {
                        const Diagnostics::ScopedDmaCallsite listReadCallsite(
                            Diagnostics::DmaCallsite::EntityScanListRead);
                        chunkRead = mem.ReadFreshExact(entity_list + offset, entity_chunk.data(), chunk_size);
                    }
                    scanDetail.scanListReadMs += elapsedMs(listReadStartedAt);
                    if (chunkRead)
                        rememberEntityListReadSuccess(offset, chunk_size, entity_chunk);
                }
            }
            if (chunkRead) {
                readable_bytes += chunk_size;
                ++readable_chunks;
                processEntityChunk(entity_chunk, chunk_size);
                continue;
            }
            if (!chunkSkipped) {
                ++scanDetail.listReadFailCount;
                rememberEntityListReadFailure(offset, chunk_size);
            }

            if (!cnProfile || kEntityListFallbackChunkSize >= chunk_size)
                continue;

            for (size_t sub_offset = 0; sub_offset < chunk_size;
                 sub_offset += kEntityListFallbackChunkSize) {
                if (!RuntimeWorkersRunning() || !ProcessConnection::IsConnected() || !registryGuard.Check())
                    break;
                const size_t sub_remaining = chunk_size - sub_offset;
                const size_t sub_size =
                    sub_remaining < kEntityListFallbackChunkSize
                        ? sub_remaining
                        : kEntityListFallbackChunkSize;
                const size_t absoluteSubOffset = offset + sub_offset;
                bool subChunkRead =
                    !registryGuard.dynamic && loadEntityListReadCache(absoluteSubOffset, sub_size, entity_chunk);
                const bool subChunkSkipped = subChunkRead || registryGuard.dynamic
                    ? false
                    : shouldSkipEntityListRead(absoluteSubOffset, sub_size);
                if (!subChunkSkipped) {
                    if (!subChunkRead) {
                        ++scanDetail.listReadCount;
                        ++scanDetail.listFallbackReadCount;
                        const auto subListReadStartedAt = std::chrono::steady_clock::now();
                        {
                            const Diagnostics::ScopedDmaCallsite listReadCallsite(
                                Diagnostics::DmaCallsite::EntityScanListRead);
                            subChunkRead = mem.ReadFreshExact(entity_list + absoluteSubOffset,
                                    entity_chunk.data(),
                                    sub_size);
                        }
                        scanDetail.scanListReadMs += elapsedMs(subListReadStartedAt);
                        if (subChunkRead)
                            rememberEntityListReadSuccess(
                                absoluteSubOffset,
                                sub_size,
                                entity_chunk);
                    }
                }
                if (!subChunkRead) {
                    if (!subChunkSkipped) {
                        ++scanDetail.listReadFailCount;
                        rememberEntityListReadFailure(absoluteSubOffset, sub_size);
                    }
                    continue;
                }
                readable_bytes += sub_size;
                ++readable_chunks;
                processEntityChunk(entity_chunk, sub_size);
            }
        }

        entityScanHeaders.clear();
        struct ScanHandle {
            VMMDLL_SCATTER_HANDLE value=mem.CreateScatterHandle();
            ~ScanHandle(){if(value)mem.CloseScatterHandle(value);}
        } scanHandle;
        struct HeaderRead {
            std::array<uint8_t,0x150> data{};
            uint32_t wrapperId=0;
            uint64_t poolId=0;
            DWORD bytes=0,wrapperBytes=0,poolBytes=0;
        };
        // Match PURE's 64-entity header batch to cut full-scan DMA scatter round trips by 4x.
        std::array<HeaderRead,64> headers{};
        bool recordReadsComplete=true;
        for(size_t first=0;first<candidateParents.size();first+=headers.size()) {
            co_yield EntityScanProgress{EntityScanPhase::Headers, &result};
            if(!registryGuard.Check() || !scanHandle.value) break;
            headers={};
            const size_t count=(std::min)(headers.size(),candidateParents.size()-first);
            for(size_t i=0;i<count;++i) {
                const auto parent=candidateParents[first+i];auto& read=headers[i];
                mem.AddScatterReadRequest(scanHandle.value,parent,read.data.data(),read.data.size(),&read.bytes);
                mem.AddScatterReadRequest(scanHandle.value,parent+0x2F0,&read.wrapperId,sizeof(read.wrapperId),&read.wrapperBytes);
            }
            const bool readOk=mem.ExecuteReadScatter(scanHandle.value);
            for(size_t i=0;i<count;++i) {
                auto& read=headers[i];const auto parent=candidateParents[first+i];
                if(!readOk || read.bytes!=read.data.size()) { recordReadsComplete=false;continue; }
                EntityHeaderSnapshot snapshot;snapshot.parent=parent;snapshot.valid=true;
                snapshot.range.Assign(parent,read.data.data(),read.data.size());
                uint64_t pool=0;snapshot.ReadParentOffset(offset::Entity_PoolPtr,pool);pool&=~0x3Full;
                if(IsPlausibleUserPointer(pool))mem.AddScatterReadRequest(scanHandle.value,pool+offset::Pool_PoolId,
                    &read.poolId,sizeof(read.poolId),&read.poolBytes);
                entityScanHeaders.insert_or_assign(parent,std::move(snapshot));
            }
            const bool poolOk=mem.ExecuteReadScatter(scanHandle.value);
            for(size_t i=0;i<count;++i) {
                const auto parent=candidateParents[first+i];auto& read=headers[i];
                auto& meta=recordMetadata[parent];
                std::memcpy(&meta.plus8,read.data.data()+8,sizeof(meta.plus8));
                meta.wrapperId=read.wrapperId;

                meta.poolId=read.poolId;meta.poolRead=poolOk && read.poolBytes==sizeof(read.poolId);
                if(entityScanHeaders.contains(parent))addRecord(parent);
            }
        }
        entityScanTopology={};
        entityScanTopology.registry=registryGuard.snapshot;
        entityScanTopology.root=entity_list;
        entityScanTopology.complete=readable_bytes==kEntityListReadSize;
        entityScanTopology.member_ids_complete=entityScanTopology.complete && recordReadsComplete;
        entityScanTopology.members=candidateParents;
        for(const auto& record:records) {
            if(record.unique_id)entityScanTopology.member_ids.emplace_back(record.entity,record.unique_id);
            if(record.header.valid)entityScanHeaders.emplace(record.entity,record.header);
            else entityScanTopology.member_ids_complete=false;
        }
        std::sort(entityScanTopology.members.begin(),entityScanTopology.members.end());
        entityScanTopology.members.erase(std::unique(entityScanTopology.members.begin(),entityScanTopology.members.end()),entityScanTopology.members.end());
        entityScanComplete = recordReadsComplete && readable_bytes == kEntityListReadSize && registryGuard.Check(true);
        entityScanReadComplete=entityScanComplete;
        scanDetail.readableBytes = readable_bytes;
        scanDetail.readableChunks = readable_chunks;
        scanDetail.slotsScanned = slots_scanned;
        scanDetail.nonZeroSlots = nonzero_slots;
        scanDetail.plausibleSlots = plausible_slots;
        scanDetail.records = records.size();
        scanDetail.matchIds = match_index.size();
        scanDetail.lightScanRequested = lightScanRequested;
        scanDetail.lightScanEnabled = lightScan;
        scanDetail.lightScanPairCap = lightScanMaxCandidates;

        if (records.empty()) {
            if(nonzero_slots!=0)entityScanComplete=false;
            entityScanStatus=!registryGuard.valid?EntityRegistry::ScanStatus::Invalidated:
                (entityScanComplete?EntityRegistry::ScanStatus::Complete:EntityRegistry::ScanStatus::Partial);
            Diagnostics::Trace("Entity scan skipped: no records from list=0x%llX chunks=%zu bytes=%zu.",
                static_cast<unsigned long long>(entity_list),
                readable_chunks,
                readable_bytes);
            finishScanDetail();
            co_return result;
        }

        auto addPair = [&](uint64_t component_parent,
                           uint64_t link_parent,
                           bool lightCandidate = false) {
            ++scanDetail.addPairAttemptCount;
            if (!component_parent || !link_parent)
                return false;
            if (lightCandidate &&
                lightScanMaxCandidates != 0 &&
                result.size() >= lightScanMaxCandidates) {
                ++scanDetail.lightScanCapHits;
                return false;
            }
            const auto pair = std::make_pair(component_parent, link_parent);
            if (std::find(result.begin(), result.end(), pair) != result.end()) {
                ++scanDetail.addPairDuplicateCount;
                return false;
            }
            result.push_back(pair);
            if (lightCandidate)
                ++scanDetail.lightScanUnvalidatedPairs;
            return true;
        };

        auto hasPlayableComponents = [&](uint64_t component_parent) {
            ++scanDetail.playableValidationAttemptCount;
            if (!component_parent)
                return false;
            const uint64_t health =
                DecryptComponent(component_parent, offset::Active().typeHealth);
            if (!health)
                return false;
            const uint64_t velocity =
                DecryptComponent(component_parent, TYPE_VELOCITY);
            if (!velocity)
                return false;
            ++scanDetail.playableValidationSuccessCount;
            return true;
        };

        auto hasCnPlayableComponents = [&](const EntityScanRecord& current) {
            const auto validationStartedAt = std::chrono::steady_clock::now();
            const Diagnostics::ScopedDmaCallsite validationCallsite(
                Diagnostics::DmaCallsite::EntityScanSelfValidation);
            ++scanDetail.playableValidationAttemptCount;
            auto finishValidation = [&]() {
                scanDetail.scanCnSelfValidationMs += elapsedMs(validationStartedAt);
            };
            if (!current.entity)
            {
                finishValidation();
                return false;
            }
            const EntityHeaderSnapshot* snapshot =
                current.header.valid ? &current.header : nullptr;
            auto rememberReject = [&](int reason,
                                      uint64_t healthBase,
                                      uint64_t heroBase,
                                      uint64_t heroId,
                                      uint64_t velocityBase,
                                      uint64_t boneBase,
                                      float healthValue,
                                      float healthMaxValue) {
                if (scanDetail.sampleRejectParent != 0)
                    return;
                scanDetail.sampleRejectReason = reason;
                scanDetail.sampleRejectParent = current.entity;
                scanDetail.sampleRejectMatchId = current.unique_id;
                scanDetail.sampleRejectHealthBase = healthBase;
                scanDetail.sampleRejectHeroBase = heroBase;
                scanDetail.sampleRejectHeroId = heroId;
                scanDetail.sampleRejectVelocityBase = velocityBase;
                scanDetail.sampleRejectBoneBase = boneBase;
                scanDetail.sampleRejectHealthCm = std::isfinite(healthValue)
                    ? static_cast<int>(healthValue * 100.0f)
                    : 0;
                scanDetail.sampleRejectHealthMaxCm = std::isfinite(healthMaxValue)
                    ? static_cast<int>(healthMaxValue * 100.0f)
                    : 0;
            };

            const uint64_t health =
                DecryptComponent(current.entity, offset::Active().typeHealth, snapshot);
            if (IsPlausibleUserPointer(health))
                ++scanDetail.selfHealthBase;
            if (!IsPlausibleUserPointer(health)) {
                rememberReject(1, health, 0, 0, 0, 0, 0.0f, 0.0f);
                finishValidation();
                return false;
            }

            health_compo_t health_compo{};
            if (!SDK->read_range(health, &health_compo, sizeof(health_compo))) {
                rememberReject(2, health, 0, 0, 0, 0, 0.0f, 0.0f);
                finishValidation();
                return false;
            }
            ++scanDetail.selfHealthRead;

            HealthPayloadSummary parsedHealth{};
            const bool validHealth = TryParseHealthPayload(health_compo, parsedHealth);
            const float totalHealth = parsedHealth.total;
            const float totalHealthMax = parsedHealth.total_max;
            if (!validHealth || parsedHealth.isDead || totalHealth <= 0.0f) {
                rememberReject(3, health, 0, 0, 0, 0, totalHealth, totalHealthMax);
                finishValidation();
                return false;
            }
            ++scanDetail.selfHealthPlausible;

            const uint64_t hero =
                DecryptComponent(current.entity, offset::Active().typeHeroId, snapshot);
            if (IsPlausibleUserPointer(hero))
                ++scanDetail.selfHeroBase;
            if (!IsPlausibleUserPointer(hero)) {
                rememberReject(4, health, hero, 0, 0, 0, totalHealth, totalHealthMax);
                finishValidation();
                return false;
            }
            hero_compo_t hero_compo{};
            if (!SDK->read_range(hero+0xD0, &hero_compo.heroid, sizeof(hero_compo.heroid))) {
                rememberReject(5, health, hero, 0, 0, 0, totalHealth, totalHealthMax);
                finishValidation();
                return false;
            }
            ++scanDetail.selfHeroRead;
            if (!GameData::IsKnownHeroId(hero_compo.heroid)) {
                rememberReject(6, health, hero, hero_compo.heroid, 0, 0, totalHealth, totalHealthMax);
                finishValidation();
                return false;
            }
            ++scanDetail.selfHeroKnown;

            const uint64_t velocity =
                DecryptComponent(current.entity, TYPE_VELOCITY, snapshot);
            const uint64_t bone =
                DecryptComponent(current.entity, TYPE_BONE, snapshot);
            if (IsPlausibleUserPointer(velocity))
                ++scanDetail.selfVelocityBase;
            if (IsPlausibleUserPointer(bone))
                ++scanDetail.selfBoneBase;
            if (!IsPlausibleUserPointer(velocity) && !IsPlausibleUserPointer(bone)) {
                rememberReject(7, health, hero, hero_compo.heroid, velocity, bone, totalHealth, totalHealthMax);
                finishValidation();
                return false;
            }
            ++scanDetail.selfPlayable;
            ++scanDetail.playableValidationSuccessCount;
            finishValidation();
            return true;
        };

        struct PersistentCnComponentNegativeCacheEntry {
            uint64_t entityList = 0;
            uint64_t moduleBase = 0;
            uint64_t moduleSize = 0;
            DWORD updateTick = 0;
        };
        static std::unordered_map<uint64_t, PersistentCnComponentNegativeCacheEntry>
            persistentCnComponentNegativeCache{};
        static uint64_t persistentCnComponentNegativeEpoch = 0;
        if (persistentCnComponentNegativeEpoch != scanEpoch) {
            persistentCnComponentNegativeCache.clear();
            persistentCnComponentNegativeEpoch = scanEpoch;
        }
        static uint64_t persistentCnComponentNegativeEntityList = 0;
        static uint64_t persistentCnComponentNegativeModuleBase = 0;
        static uint64_t persistentCnComponentNegativeModuleSize = 0;
        const DWORD componentNegativeCacheTtlMs =
            cnProfile ? EntityScannerComponentNegativeCacheTtlMs() : 0;
        const bool componentNegativeCacheEnabled =
            cnProfile && componentNegativeCacheTtlMs > 0;
        const size_t componentNegativeCacheRefreshBudget =
            componentNegativeCacheEnabled ? EntityScannerComponentNegativeCacheRefreshBudget() : 0;
        const bool componentNegativeCacheRefreshBudgetEnabled =
            componentNegativeCacheRefreshBudget > 0;
        size_t componentNegativeCacheRefreshCount = 0;
        scanDetail.cnComponentNegativeCacheEnabled = componentNegativeCacheEnabled;
        scanDetail.cnComponentNegativeCacheTtlMs = componentNegativeCacheTtlMs;
        scanDetail.cnComponentNegativeCacheRefreshBudget =
            componentNegativeCacheRefreshBudget;
        if (componentNegativeCacheEnabled &&
            (persistentCnComponentNegativeEntityList != entity_list ||
             persistentCnComponentNegativeModuleBase != SDK->dwGameBase ||
             persistentCnComponentNegativeModuleSize != moduleSize)) {
            persistentCnComponentNegativeCache.clear();
            persistentCnComponentNegativeEntityList = entity_list;
            persistentCnComponentNegativeModuleBase = SDK->dwGameBase;
            persistentCnComponentNegativeModuleSize = moduleSize;
        }
        if (componentNegativeCacheEnabled &&
            persistentCnComponentNegativeCache.size() > 8192) {
            persistentCnComponentNegativeCache.clear();
        }

        auto hasCnComponentOnlyPlayable = [&](const EntityScanRecord& source,
                                                uint64_t component_parent) {
            const auto validationStartedAt = std::chrono::steady_clock::now();
            const Diagnostics::ScopedDmaCallsite validationCallsite(
                Diagnostics::DmaCallsite::EntityScanComponentValidation);
            ++scanDetail.componentOnlyValidationAttemptCount;
            auto finishValidation = [&]() {
                scanDetail.scanComponentOnlyValidationMs += elapsedMs(validationStartedAt);
            };
            if (!component_parent)
            {
                finishValidation();
                return false;
            }

            if (componentNegativeCacheEnabled) {
                ++scanDetail.cnComponentNegativeCacheLookupCount;
                const DWORD now = static_cast<DWORD>(EntityRegistryNowMs());
                auto negativeIt = persistentCnComponentNegativeCache.find(component_parent);
                if (negativeIt != persistentCnComponentNegativeCache.end()) {
                    const PersistentCnComponentNegativeCacheEntry& entry =
                        negativeIt->second;
                    const bool sameContext =
                        entry.entityList == entity_list &&
                        entry.moduleBase == SDK->dwGameBase &&
                        entry.moduleSize == moduleSize;
                    const DWORD ageMs = now - entry.updateTick;
                    const bool fresh = sameContext && ageMs <= componentNegativeCacheTtlMs;
                    const bool staleMetadataAllowed =
                        sameContext &&
                        !fresh &&
                        staleMetadataMs > 0 &&
                        (staleMetadataOnly || ageMs <= staleMetadataMs);
                    const bool staleAllowed =
                        staleMetadataAllowed ||
                        (sameContext &&
                         !fresh &&
                         componentNegativeCacheRefreshBudgetEnabled &&
                         componentNegativeCacheRefreshCount >= componentNegativeCacheRefreshBudget);
                    if (fresh || staleAllowed) {
                        ++scanDetail.cnComponentNegativeCacheHitCount;
                        if (staleAllowed)
                            ++scanDetail.cnComponentNegativeCacheStaleHitCount;
                        finishValidation();
                        return false;
                    }
                    if (sameContext)
                        ++scanDetail.cnComponentNegativeCacheExpiredCount;
                    persistentCnComponentNegativeCache.erase(negativeIt);
                }
            }
            if (componentNegativeCacheEnabled) {
                ++componentNegativeCacheRefreshCount;
                ++scanDetail.cnComponentNegativeCacheRefreshCount;
            }

            EntityHeaderSnapshot componentHeader{};
            const EntityHeaderSnapshot* snapshot =
                entityScanHeaders.contains(component_parent) ? &entityScanHeaders.at(component_parent) :
                (componentHeader.Read(component_parent) ? &componentHeader : nullptr);
            auto rememberNegative = [&]() {
                if (!componentNegativeCacheEnabled)
                    return;
                PersistentCnComponentNegativeCacheEntry entry{};
                entry.entityList = entity_list;
                entry.moduleBase = SDK->dwGameBase;
                entry.moduleSize = moduleSize;
                entry.updateTick = static_cast<DWORD>(EntityRegistryNowMs());
                persistentCnComponentNegativeCache.insert_or_assign(
                    component_parent,
                    entry);
                ++scanDetail.cnComponentNegativeCacheStoreCount;
            };
            auto failValidation = [&]() {
                rememberNegative();
                finishValidation();
                return false;
            };
            auto rememberReject = [&](int reason,
                                      uint64_t healthBase,
                                      uint64_t velocityBase,
                                      uint64_t boneBase,
                                      float healthValue,
                                      float healthMaxValue) {
                if (scanDetail.sampleRejectParent != 0)
                    return;
                scanDetail.sampleRejectReason = reason;
                scanDetail.sampleRejectParent = component_parent;
                scanDetail.sampleRejectMatchId = source.unique_id;
                scanDetail.sampleRejectHealthBase = healthBase;
                scanDetail.sampleRejectVelocityBase = velocityBase;
                scanDetail.sampleRejectBoneBase = boneBase;
                scanDetail.sampleRejectHealthCm = std::isfinite(healthValue)
                    ? static_cast<int>(healthValue * 100.0f)
                    : 0;
                scanDetail.sampleRejectHealthMaxCm = std::isfinite(healthMaxValue)
                    ? static_cast<int>(healthMaxValue * 100.0f)
                    : 0;
            };

            const uint64_t health =
                DecryptComponent(component_parent, offset::Active().typeHealth, snapshot);
            if (IsPlausibleUserPointer(health))
                ++scanDetail.selfHealthBase;
            if (!IsPlausibleUserPointer(health)) {
                rememberReject(11, health, 0, 0, 0.0f, 0.0f);
                return failValidation();
            }

            health_compo_t health_compo{};
            if (!SDK->read_range(health, &health_compo, sizeof(health_compo))) {
                rememberReject(12, health, 0, 0, 0.0f, 0.0f);
                return failValidation();
            }
            ++scanDetail.selfHealthRead;

            HealthPayloadSummary parsedHealth{};
            const bool validHealth = TryParseHealthPayload(health_compo, parsedHealth);
            const float totalHealth = parsedHealth.total;
            const float totalHealthMax = parsedHealth.total_max;
            const bool plausibleHealthLayout = validHealth && !parsedHealth.isDead && totalHealth > 0.0f;
            if (!plausibleHealthLayout) {
                rememberReject(13, health, 0, 0, totalHealth, totalHealthMax);
                return failValidation();
            }
            ++scanDetail.selfHealthPlausible;

            const bool trainingBotSized =
                std::fabs(totalHealthMax - 200.0f) <= 5.0f ||
                std::fabs(totalHealthMax - 500.0f) <= 10.0f;
            if (!trainingBotSized) {
                rememberReject(14, health, 0, 0, totalHealth, totalHealthMax);
                return failValidation();
            }

            const uint64_t velocity =
                DecryptComponent(component_parent, TYPE_VELOCITY, snapshot);
            const uint64_t team =
                DecryptComponent(component_parent, offset::Active().typeTeam, snapshot);
            const uint64_t bone =
                DecryptComponent(component_parent, TYPE_BONE, snapshot);
            if (IsPlausibleUserPointer(velocity))
                ++scanDetail.selfVelocityBase;
            if (IsPlausibleUserPointer(bone))
                ++scanDetail.selfBoneBase;
            if (!IsPlausibleUserPointer(velocity)) {
                rememberReject(15, health, velocity, bone, totalHealth, totalHealthMax);
                return failValidation();
            }

            velocity_compo_t velocity_compo{};
            if (!SDK->read_range(velocity+0x200, &velocity_compo.location, sizeof(velocity_compo.location))) {
                rememberReject(16, health, velocity, bone, totalHealth, totalHealthMax);
                return failValidation();
            }
            const XMFLOAT3 location = velocity_compo.location;
            const bool finiteLocation =
                std::isfinite(location.x) &&
                std::isfinite(location.y) &&
                std::isfinite(location.z) &&
                std::fabs(location.x) < 100000.0f &&
                std::fabs(location.y) < 100000.0f &&
                std::fabs(location.z) < 100000.0f &&
                (std::fabs(location.x) > 0.001f ||
                 std::fabs(location.y) > 0.001f ||
                 std::fabs(location.z) > 0.001f);
            if (!finiteLocation) {
                rememberReject(17, health, velocity, bone, totalHealth, totalHealthMax);
                return failValidation();
            }

            bool teamOk = false;
            if (IsPlausibleUserPointer(team)) {
                const uint32_t teamBits =
                    SDK->RPM<uint32_t>(team + 0x58) & 0x0F800000u;
                teamOk = teamBits != 0;
            }
            if (!teamOk && !IsPlausibleUserPointer(bone)) {
                rememberReject(18, health, velocity, bone, totalHealth, totalHealthMax);
                return failValidation();
            }

            if (componentNegativeCacheEnabled)
                persistentCnComponentNegativeCache.erase(component_parent);
            ++scanDetail.selfPlayable;
            ++scanDetail.componentOnlyValidationSuccessCount;
            finishValidation();
            return true;
        };

        auto hasCnPlausibleHealth = [&](uint64_t component_parent) {
            if (!component_parent)
                return false;

            EntityHeaderSnapshot componentHeader{};
            const EntityHeaderSnapshot* snapshot =
                entityScanHeaders.contains(component_parent) ? &entityScanHeaders.at(component_parent) :
                (componentHeader.Read(component_parent) ? &componentHeader : nullptr);
            const uint64_t health =
                DecryptComponent(component_parent, offset::Active().typeHealth, snapshot);
            if (!IsPlausibleUserPointer(health))
                return false;

            health_compo_t health_compo{};
            if (!SDK->read_range(health, &health_compo, sizeof(health_compo)))
                return false;

            HealthPayloadSummary parsedHealth{};
            return TryParseHealthPayload(health_compo, parsedHealth) && !parsedHealth.isDead && parsedHealth.total > 0.0f;
        };

        auto hasCnKnownHero = [&](uint64_t link_parent) {
            if (!link_parent)
                return false;

            EntityHeaderSnapshot linkHeader{};
            const EntityHeaderSnapshot* snapshot =
                entityScanHeaders.contains(link_parent) ? &entityScanHeaders.at(link_parent) :
                (linkHeader.Read(link_parent) ? &linkHeader : nullptr);
            const uint64_t hero =
                DecryptComponent(link_parent, offset::Active().typeHeroId, snapshot);
            if (!IsPlausibleUserPointer(hero))
                return false;

            hero_compo_t hero_compo{};
            return SDK->read_range(hero+0xD0, &hero_compo.heroid, sizeof(hero_compo.heroid)) &&
                GameData::IsKnownHeroId(hero_compo.heroid);
        };

        auto addPlayableMatches = [&](uint32_t match_id,
                                      uint64_t link_parent,
                                      bool trustMatchOnly = false) {
            if (match_id == 0)
                return false;
            ++scanDetail.matchLookupCount;
            const auto match = match_index.find(match_id);
            if (match == match_index.end())
                return false;
            ++scanDetail.matchLookupHitCount;

            bool added = false;
            for (uint64_t component_parent : match->second) {
                if (component_parent == link_parent)
                    continue;
                if (!trustMatchOnly && !hasPlayableComponents(component_parent))
                    continue;
                added = addPair(component_parent, link_parent, trustMatchOnly) || added;
            }
            return added;
        };

        std::unordered_map<uint64_t, std::vector<uint64_t>> cnMapCandidateCache{};
        if (cnMapCandidateCacheEnabled)
            cnMapCandidateCache.reserve(records.size());
        struct PersistentCnMapCandidateCacheEntry {
            uint64_t entityList = 0;
            uint64_t moduleBase = 0;
            uint64_t moduleSize = 0;
            DWORD updateTick = 0;
            std::vector<uint64_t> candidates{};
        };
        static std::unordered_map<uint64_t, PersistentCnMapCandidateCacheEntry>
            persistentCnMapCandidateCache{};
        static uint64_t persistentCnMapCandidateEpoch = 0;
        if (persistentCnMapCandidateEpoch != scanEpoch) {
            persistentCnMapCandidateCache.clear();
            persistentCnMapCandidateEpoch = scanEpoch;
        }
        static uint64_t persistentCnMapCandidateEntityList = 0;
        static uint64_t persistentCnMapCandidateModuleBase = 0;
        static uint64_t persistentCnMapCandidateModuleSize = 0;
        const DWORD persistentMapCacheTtlMs =
            cnMapCandidateCacheEnabled ? EntityScannerPersistentMapCacheTtlMs() : 0;
        const bool persistentMapCacheEnabled =
            cnProfile && cnMapCandidateCacheEnabled && persistentMapCacheTtlMs > 0;
        const size_t persistentMapCacheRefreshBudget =
            persistentMapCacheEnabled ? EntityScannerPersistentMapCacheRefreshBudget() : 0;
        const bool persistentMapCacheRefreshBudgetEnabled =
            persistentMapCacheRefreshBudget > 0;
        size_t persistentMapCacheRefreshCount = 0;
        scanDetail.cnMapCandidatePersistentCacheEnabled = persistentMapCacheEnabled;
        scanDetail.cnMapCandidatePersistentCacheTtlMs = persistentMapCacheTtlMs;
        scanDetail.cnMapCandidatePersistentCacheRefreshBudget =
            persistentMapCacheRefreshBudget;
        if (persistentMapCacheEnabled &&
            (persistentCnMapCandidateEntityList != entity_list ||
             persistentCnMapCandidateModuleBase != SDK->dwGameBase ||
             persistentCnMapCandidateModuleSize != moduleSize)) {
            persistentCnMapCandidateCache.clear();
            persistentCnMapCandidateEntityList = entity_list;
            persistentCnMapCandidateModuleBase = SDK->dwGameBase;
            persistentCnMapCandidateModuleSize = moduleSize;
        }
        if (persistentMapCacheEnabled &&
            persistentCnMapCandidateCache.size() > 4096) {
            persistentCnMapCandidateCache.clear();
        }
        std::unordered_set<uint64_t> cnMapDiagParents{};
        if (cnMapDiagEnabled)
            cnMapDiagParents.reserve(records.size());

        auto collectCnMapBaseCandidatesForParent =
            [&](uint64_t parent) -> std::vector<uint64_t> {
                if (cnMapDiagEnabled) {
                    ++scanDetail.cnMapCandidateParentLookupCount;
                    if (cnMapDiagParents.insert(parent).second)
                        ++scanDetail.cnMapCandidateUniqueParentCount;
                    else
                        ++scanDetail.cnMapCandidateDuplicateParentCount;
                }

                if(const auto metadata=recordMetadata.find(parent);metadata!=recordMetadata.end()) {
                    const auto found=cnMapCandidateCache.find(parent);
                    if(found!=cnMapCandidateCache.end())return found->second;
                    std::vector<uint64_t> candidates;
                    AddCnMapBaseCandidate(candidates,parent,entity_list,kEntityListReadSize,SDK->dwGameBase,moduleSize);
                    AddCnMapBaseCandidate(candidates,metadata->second.plus8,entity_list,kEntityListReadSize,SDK->dwGameBase,moduleSize);
                    if(metadata->second.wrapperId)AddCnMapBaseCandidate(candidates,ResolveCnWrapperMapBase(parent),
                        entity_list,kEntityListReadSize,SDK->dwGameBase,moduleSize);
                    cnMapCandidateCache.emplace(parent,candidates);return candidates;
                }
                if (!cnMapCandidateCacheEnabled) {
                    return CollectCnMapBaseCandidates(
                        parent,
                        entity_list,
                        kEntityListReadSize,
                        SDK->dwGameBase,
                        moduleSize,
                        &scanDetail,
                        cnMapDiagEnabled);
                }

                ++scanDetail.cnMapCandidateCacheLookupCount;
                const auto cachedIt = cnMapCandidateCache.find(parent);
                if (cachedIt != cnMapCandidateCache.end()) {
                    ++scanDetail.cnMapCandidateCacheHitCount;
                    return cachedIt->second;
                }

                ++scanDetail.cnMapCandidateCacheMissCount;
                if (persistentMapCacheEnabled) {
                    ++scanDetail.cnMapCandidatePersistentCacheLookupCount;
                    const DWORD now = static_cast<DWORD>(EntityRegistryNowMs());
                    auto persistentIt = persistentCnMapCandidateCache.find(parent);
                    if (persistentIt != persistentCnMapCandidateCache.end()) {
                        const PersistentCnMapCandidateCacheEntry& entry =
                            persistentIt->second;
                        const bool sameContext =
                            entry.entityList == entity_list &&
                            entry.moduleBase == SDK->dwGameBase &&
                            entry.moduleSize == moduleSize;
                        const DWORD ageMs = now - entry.updateTick;
                        if (sameContext && ageMs <= persistentMapCacheTtlMs) {
                            ++scanDetail.cnMapCandidatePersistentCacheHitCount;
                            cnMapCandidateCache.emplace(parent, entry.candidates);
                            return entry.candidates;
                        }
                        if (sameContext &&
                            staleMetadataMs > 0 &&
                            (staleMetadataOnly || ageMs <= staleMetadataMs) &&
                            !entry.candidates.empty()) {
                            ++scanDetail.cnMapCandidatePersistentCacheStaleHitCount;
                            cnMapCandidateCache.emplace(parent, entry.candidates);
                            return entry.candidates;
                        }
                        if (sameContext)
                            ++scanDetail.cnMapCandidatePersistentCacheExpiredCount;
                        if (sameContext &&
                            persistentMapCacheRefreshBudgetEnabled &&
                            persistentMapCacheRefreshCount >= persistentMapCacheRefreshBudget &&
                            !entry.candidates.empty()) {
                            ++scanDetail.cnMapCandidatePersistentCacheStaleHitCount;
                            cnMapCandidateCache.emplace(parent, entry.candidates);
                            return entry.candidates;
                        }
                        persistentCnMapCandidateCache.erase(persistentIt);
                    }
                    ++scanDetail.cnMapCandidatePersistentCacheMissCount;
                }
                if (persistentMapCacheEnabled) {
                    ++persistentMapCacheRefreshCount;
                    ++scanDetail.cnMapCandidatePersistentCacheRefreshCount;
                }
                std::vector<uint64_t> candidates = CollectCnMapBaseCandidates(
                    parent,
                    entity_list,
                    kEntityListReadSize,
                    SDK->dwGameBase,
                    moduleSize,
                    &scanDetail,
                    cnMapDiagEnabled);
                cnMapCandidateCache.emplace(parent, candidates);
                if (persistentMapCacheEnabled) {
                    PersistentCnMapCandidateCacheEntry entry{};
                    entry.entityList = entity_list;
                    entry.moduleBase = SDK->dwGameBase;
                    entry.moduleSize = moduleSize;
                    entry.updateTick = static_cast<DWORD>(EntityRegistryNowMs());
                    entry.candidates = candidates;
                    persistentCnMapCandidateCache.insert_or_assign(parent, std::move(entry));
                    ++scanDetail.cnMapCandidatePersistentCacheStoreCount;
                }
                return candidates;
            };

        const bool worldProfile = !offset::UsesProtectedEntityLayout();
        std::unordered_set<uint64_t> cnLinkTargetPairs{};

        struct PersistentCnLinkDecryptNegativeCacheEntry {
            uint64_t entityList = 0;
            uint64_t moduleBase = 0;
            uint64_t moduleSize = 0;
            DWORD updateTick = 0;
        };
        static std::unordered_map<uint64_t, PersistentCnLinkDecryptNegativeCacheEntry>
            persistentCnLinkDecryptNegativeCache{};
        static uint64_t persistentCnLinkDecryptNegativeEpoch = 0;
        if (persistentCnLinkDecryptNegativeEpoch != scanEpoch) {
            persistentCnLinkDecryptNegativeCache.clear();
            persistentCnLinkDecryptNegativeEpoch = scanEpoch;
        }
        static uint64_t persistentCnLinkDecryptNegativeEntityList = 0;
        static uint64_t persistentCnLinkDecryptNegativeModuleBase = 0;
        static uint64_t persistentCnLinkDecryptNegativeModuleSize = 0;
        const DWORD linkDecryptNegativeCacheTtlMs =
            cnProfile ? EntityScannerLinkDecryptNegativeCacheTtlMs() : 0;
        const bool linkDecryptNegativeCacheEnabled =
            cnProfile && linkDecryptNegativeCacheTtlMs > 0;
        scanDetail.cnLinkDecryptNegativeCacheEnabled =
            linkDecryptNegativeCacheEnabled;
        scanDetail.cnLinkDecryptNegativeCacheTtlMs =
            linkDecryptNegativeCacheTtlMs;
        if (linkDecryptNegativeCacheEnabled &&
            (persistentCnLinkDecryptNegativeEntityList != entity_list ||
             persistentCnLinkDecryptNegativeModuleBase != SDK->dwGameBase ||
             persistentCnLinkDecryptNegativeModuleSize != moduleSize)) {
            persistentCnLinkDecryptNegativeCache.clear();
            persistentCnLinkDecryptNegativeEntityList = entity_list;
            persistentCnLinkDecryptNegativeModuleBase = SDK->dwGameBase;
            persistentCnLinkDecryptNegativeModuleSize = moduleSize;
        }
        if (linkDecryptNegativeCacheEnabled &&
            persistentCnLinkDecryptNegativeCache.size() > 8192) {
            persistentCnLinkDecryptNegativeCache.clear();
        }
        auto linkDecryptNegativeCacheHit = [&](uint64_t parent) {
            if (!linkDecryptNegativeCacheEnabled || !parent)
                return false;
            ++scanDetail.cnLinkDecryptNegativeCacheLookupCount;
            const DWORD now = static_cast<DWORD>(EntityRegistryNowMs());
            auto negativeIt = persistentCnLinkDecryptNegativeCache.find(parent);
            if (negativeIt == persistentCnLinkDecryptNegativeCache.end())
                return false;

            const PersistentCnLinkDecryptNegativeCacheEntry& entry =
                negativeIt->second;
            const bool sameContext =
                entry.entityList == entity_list &&
                entry.moduleBase == SDK->dwGameBase &&
                entry.moduleSize == moduleSize;
            const DWORD ageMs = now - entry.updateTick;
            const bool fresh = sameContext && ageMs <= linkDecryptNegativeCacheTtlMs;
            const bool staleAllowed =
                sameContext &&
                !fresh &&
                staleMetadataMs > 0 &&
                (staleMetadataOnly || ageMs <= staleMetadataMs);
            if (fresh || staleAllowed) {
                ++scanDetail.cnLinkDecryptNegativeCacheHitCount;
                if (staleAllowed)
                    ++scanDetail.cnLinkDecryptNegativeCacheStaleHitCount;
                return true;
            }

            if (sameContext)
                ++scanDetail.cnLinkDecryptNegativeCacheExpiredCount;
            persistentCnLinkDecryptNegativeCache.erase(negativeIt);
            return false;
        };
        auto storeLinkDecryptNegativeCache = [&](uint64_t parent) {
            if (!linkDecryptNegativeCacheEnabled || !parent)
                return;
            PersistentCnLinkDecryptNegativeCacheEntry entry{};
            entry.entityList = entity_list;
            entry.moduleBase = SDK->dwGameBase;
            entry.moduleSize = moduleSize;
            entry.updateTick = static_cast<DWORD>(EntityRegistryNowMs());
            persistentCnLinkDecryptNegativeCache.insert_or_assign(parent, entry);
            ++scanDetail.cnLinkDecryptNegativeCacheStoreCount;
        };
        auto decryptCnLink = [&](uint64_t parent,
                                   const EntityHeaderSnapshot* snapshot) {
            if (linkDecryptNegativeCacheHit(parent))
                return static_cast<uint64_t>(0);
            ++scanDetail.linkDecryptAttemptCount;
            const uint64_t link = DecryptComponent(parent, offset::Active().typeLink, snapshot);
            if (!link)
                storeLinkDecryptNegativeCache(parent);
            return link;
        };

        for (const EntityScanRecord& current : records) {
            co_yield EntityScanProgress{EntityScanPhase::Pairing, &result};
            if (!RuntimeWorkersRunning() || !ProcessConnection::IsConnected() || !registryGuard.Check())
                break;
            const auto matchLinkStartedAt = std::chrono::steady_clock::now();
            const Diagnostics::ScopedDmaCallsite matchLinkCallsite(
                Diagnostics::DmaCallsite::EntityScanMatchLink);
            uint64_t cur_entity = current.entity;
            if (!cur_entity) {
                scanDetail.scanMatchLinkMs += elapsedMs(matchLinkStartedAt);
                continue;
            }

            if (worldProfile && lightScan) {
                bool added = false;
                if (current.unique_id != 0)
                    added = addPlayableMatches(current.unique_id + 1, cur_entity, true);
                if (!added && current.unique_id != 0)
                    added = addPlayableMatches(current.unique_id, cur_entity, true);
                if (!added &&
                    current.unique_id != 0 &&
                    (current.unique_id & 0x80000000u) == 0) {
                    addPlayableMatches(current.unique_id | 0x80000000u, cur_entity, true);
                }
                scanDetail.scanMatchLinkMs += elapsedMs(matchLinkStartedAt);
                continue;
            }

            uint64_t common_linker = decryptCnLink(
                cur_entity,
                current.header.valid ? &current.header : nullptr);
            if (!common_linker) {
                if (worldProfile) {
                    Diagnostics::RecordInvalidEntity();
                    scanDetail.scanMatchLinkMs += elapsedMs(matchLinkStartedAt);
                    continue;
                }
            } else {
                ++scanDetail.linkDecryptSuccessCount;
                ++scanDetail.linkPresent;
            }

            if (worldProfile) {
                uint32_t unique_id = SDK->RPM<uint32_t>(common_linker + offset::Link_UniqueId);
                if (unique_id != 0)
                    ++scanDetail.linkUidNonZero;
                bool added = false;
                if (current.unique_id != 0)
                    added = addPlayableMatches(current.unique_id + 1, cur_entity);
                if (!added && unique_id != 0)
                    added = addPlayableMatches(unique_id, cur_entity);
                if (!added && unique_id != 0 && (unique_id & 0x80000000u) == 0)
                    addPlayableMatches(unique_id | 0x80000000u, cur_entity);
                scanDetail.scanMatchLinkMs += elapsedMs(matchLinkStartedAt);
                continue;
            }

            bool processedCnLink = false;
            auto processCnLinkParent = [&](uint64_t link_parent,
                                             const EntityHeaderSnapshot* snapshot) {
                if (!link_parent)
                    return;

                const uint64_t linkBase =
                    link_parent == cur_entity && common_linker
                        ? common_linker
                        : decryptCnLink(link_parent, snapshot);
                if (!linkBase)
                    return;
                if (!(link_parent == cur_entity && common_linker))
                    ++scanDetail.linkDecryptSuccessCount;

                processedCnLink = true;
                ++scanDetail.linkPresent;

                const uint32_t unique_id =
                    SDK->RPM<uint32_t>(linkBase + offset::Link_UniqueId);
                if (unique_id != 0) {
                    ++scanDetail.linkUidNonZero;
                    ++scanDetail.matchLookupCount;
                    const auto match = match_index.find(unique_id);
                    if (match != match_index.end()) {
                        ++scanDetail.matchLookupHitCount;
                        ++scanDetail.linkMatched;
                        scanDetail.cnBucketEntryScanCount += match->second.size();
                        for (uint64_t component_parent : match->second) {
                            if (addPair(component_parent, link_parent, lightScan))
                                ++scanDetail.linkPairs;
                        }
                    }
                }

                const bool linkHeroKnown = !lightScan && hasCnKnownHero(link_parent);
                for (uint64_t idOffset : { offset::Link_TargetId, offset::Link_UniqueId }) {
                    uint32_t resolvedId = 0;
                    uint64_t targetMapBase = 0;
                    {
                        const auto targetMapStartedAt = std::chrono::steady_clock::now();
                        const Diagnostics::ScopedDmaCallsite targetResolveCallsite(
                            Diagnostics::DmaCallsite::EntityScanLinkTargetResolve);
                        ++scanDetail.cnTargetMapAttemptCount;
                        targetMapBase =
                            ResolveCnLinkTargetMapBase(linkBase, idOffset, &resolvedId);
                        scanDetail.scanCnTargetMapMs += elapsedMs(targetMapStartedAt);
                    }
                    if (!IsLikelyCnEntityParent(
                            targetMapBase,
                            entity_list,
                            kEntityListReadSize,
                            SDK->dwGameBase,
                            moduleSize)) {
                        continue;
                    }
                    ++scanDetail.cnTargetMapSuccessCount;

                    const uint64_t pairKey =
                        (link_parent >> 4) ^ (targetMapBase << 17) ^ resolvedId;
                    if (!cnLinkTargetPairs.insert(pairKey).second)
                        continue;

                    if (!lightScan && !linkHeroKnown && !hasCnPlausibleHealth(targetMapBase))
                        continue;

                    if (addPair(targetMapBase, link_parent, lightScan)) {
                        ++scanDetail.linkMatched;
                        ++scanDetail.linkPairs;
                    }
                }
            };

            if (common_linker)
                processCnLinkParent(
                    cur_entity,
                    current.header.valid ? &current.header : nullptr);

            {
                std::vector<uint64_t> linkParentCandidates{};
                {
                    const auto targetMapStartedAt = std::chrono::steady_clock::now();
                    const Diagnostics::ScopedDmaCallsite mapCandidateCallsite(
                        Diagnostics::DmaCallsite::EntityScanMapCandidate);
                    linkParentCandidates =
                        collectCnMapBaseCandidatesForParent(cur_entity);
                    scanDetail.scanCnTargetMapMs += elapsedMs(targetMapStartedAt);
                }
                scanDetail.cnMapCandidateCount += linkParentCandidates.size();
                for (uint64_t link_parent : linkParentCandidates) {
                    if (link_parent == cur_entity)
                        continue;
                    if (linkDecryptNegativeCacheHit(link_parent))
                        continue;

                    EntityHeaderSnapshot linkHeader{};
                    const EntityHeaderSnapshot* snapshot =
                        entityScanHeaders.contains(link_parent) ? &entityScanHeaders.at(link_parent) :
                (linkHeader.Read(link_parent) ? &linkHeader : nullptr);
                    processCnLinkParent(link_parent, snapshot);
                }
            }

            if (!processedCnLink)
                Diagnostics::RecordInvalidEntity();
            scanDetail.scanMatchLinkMs += elapsedMs(matchLinkStartedAt);
        }

        if (offset::UsesProtectedEntityLayout() && !lightScan) {
            for (const EntityScanRecord& current : records) {
                co_yield EntityScanProgress{EntityScanPhase::Pairing, &result};
                if (!RuntimeWorkersRunning() || !ProcessConnection::IsConnected() || !registryGuard.Check())
                    break;
                if (hasCnPlayableComponents(current))
                    addPair(current.entity, current.entity);
                std::vector<uint64_t> componentCandidates{};
                {
                    const auto targetMapStartedAt = std::chrono::steady_clock::now();
                    const Diagnostics::ScopedDmaCallsite mapCandidateCallsite(
                        Diagnostics::DmaCallsite::EntityScanMapCandidate);
                    componentCandidates =
                        collectCnMapBaseCandidatesForParent(current.entity);
                    scanDetail.scanCnTargetMapMs += elapsedMs(targetMapStartedAt);
                }
                scanDetail.cnMapCandidateCount += componentCandidates.size();
                for (uint64_t component_parent : componentCandidates) {
                    if (hasCnComponentOnlyPlayable(current, component_parent))
                        addPair(component_parent, component_parent);
                }
            }
        }

        const auto dynamicPairStartedAt = std::chrono::steady_clock::now();
        for (const EntityScanRecord& current : records) {
            co_yield EntityScanProgress{EntityScanPhase::Pairing, &result};
            if (!RuntimeWorkersRunning() || !ProcessConnection::IsConnected() || !registryGuard.Check())
                break;
            if (isDynamicEntityId(current.entity_id)) {
                addPair(current.entity, current.entity);
                ++scanDetail.dynamicPairs;
            }
        }
        scanDetail.scanDynamicPairMs += elapsedMs(dynamicPairStartedAt);

        if(!records.empty() && result.empty())entityScanComplete=false;
        if(!registryGuard.Check(true)) {
            result.clear();entityScanComplete=false;entityScanStatus=EntityRegistry::ScanStatus::Invalidated;
        } else entityScanStatus=entityScanComplete?EntityRegistry::ScanStatus::Complete:EntityRegistry::ScanStatus::Partial;
        scanDetail.totalPairs = result.size();
        finishScanDetail();

        Diagnostics::Trace("Entity scan list=0x%llX bytes=%zu records=%zu pairs=%zu.",
            static_cast<unsigned long long>(entity_list),
            readable_bytes,
            records.size(),
            result.size());
        co_return result;
    }

    inline std::vector<std::pair<uint64_t,uint64_t>> get_ow_entities() {
        auto scan=BeginEntityScan();
        while(!scan.Done())scan.Resume();
        return scan.Pairs();
    }

    inline std::string GetHeroEngNames(uint64_t HeroID, uint64_t LinkBase) {
        if (HeroID == eHero::HERO_DVA) {
            return SDK->RPM<uint16_t>(LinkBase + 0xD4) != SDK->RPM<uint16_t>(LinkBase + 0xD8)
                ? "D.Va"
                : "Hana";
        }

        if (const char* name = GameData::HeroName(HeroID); name[0] != '\0')
            return name;

        switch (HeroID) {
            case eHero::TOBTERT: return "Tob";
            case eHero::SYMTERT: return "Sym";
            case eHero::Bob:     return "Bob";
            default:
                if (GameData::HasHeroIdPrefix(HeroID)) {
                    char fallback[16] = {};
                    std::snprintf(fallback, sizeof(fallback), "Hero_%04llX",
                        static_cast<unsigned long long>(HeroID & 0xFFFFull));
                    return fallback;
                }
                if (HeroID != 0) {
                    return GameData::UnknownHeroFallbackName(HeroID);
                }
                return "Unknown";
        }
    }

    inline bool IsSkillActive(uint64_t base, uint16_t index, uint16_t id, bool* readable = nullptr) {
        if (readable) *readable = false;
        if (id == 0) return false;
        uint64_t skillList = 0;

        if (index == 0) {
            skillList = base + 0x570;
        } else {
            uint32_t count = SDK->RPM<uint32_t>(base + 0x378);
            if (count == 0 || count >= 0xFF) return false;
            uint64_t entry = SDK->RPM<uint64_t>(base + 0x370);
            if (!entry) return false;
            for (uint32_t i = 0; i < count; i++, entry += 0x10) {
                if (SDK->RPM<uint16_t>(entry + 0x8) == index) {
                    uint64_t listStruct = SDK->RPM<uint64_t>(entry);
                    if (!listStruct) return false;
                    skillList = SDK->RPM<uint64_t>(listStruct + 0xA8);
                    break;
                }
            }
        }
        if (!skillList) return false;

        const auto result = SkillRead::FindInBucket(skillList + 0x20 * ((id & 0xF) + 1), id,
            [](uint64_t address, void* bytes, size_t size) { return SDK->read_range(address, bytes, size); });
        if (result.status != SkillRead::Status::Found || !SkillRead::UserRange(result.address, 0x49)) return false;
        uint8_t state = 0;
        const bool ok = SDK->read_range(result.address + 0x48, &state, sizeof(state)) && state <= 1;
        if (readable) *readable = ok;
        return ok && state == 1;
    }

    inline uintptr_t SkillStructCheck(uint64_t a1, uint16_t a2) {
        const uint32_t count = SDK->RPM<uint32_t>(a1 + 0x2A8);
        if (count == 0 || count >= 0xFF) return 0;

        uint64_t entry = SDK->RPM<uint64_t>(a1 + 0x2A0);
        if (!entry) return 0;

        for (uint32_t scanned = 0; scanned < count; ++scanned, entry += 16) {
            if (SDK->RPM<uint16_t>(entry + 8) == a2)
                return SDK->RPM<uint64_t>(entry);
        }
        return 0;
    }

    inline uint64_t FnSkillStruct(__m128* a1, uint16_t* a2) {
        uint16_t* v3 = a2;
        if (!a2[1]) return 0;
        uint16_t v4 = *a2;
        uint64_t v5 = a1->m128_i64[1];
        uint64_t v6 = 0;
        if (!v4) {
            v6 = v5 + 0x4A0;
            goto LABEL_6;
        }
        {
            uint64_t v7 = SkillStructCheck(v5, v4);
            if (!v7) return 0;
            v6 = SDK->RPM<uint64_t>(v7 + 0xB8);
        }
    LABEL_6:
        const auto result = SkillRead::FindInBucket(v6 + 32 * ((v3[1] & 0xF) + 1), v3[1],
            [](uint64_t address, void* bytes, size_t size) { return SDK->read_range(address, bytes, size); });
        return result.status == SkillRead::Status::Found ? result.address : 0;
    }

    inline bool IsSkillActivate1(uint64_t base, uint16_t skillIdx, uint16_t skillIdx2) {
        __m128 skillStruct{};
        uint16_t skillId[15] = { skillIdx, skillIdx2 };
        skillStruct.m128_u64[1] = base + 0xD0;
        uint64_t skill = FnSkillStruct(&skillStruct, skillId);
        if (!SkillRead::UserRange(skill, 0x49)) return false;
        uint8_t state = 0;
        return SDK->read_range(skill + 0x48, &state, sizeof(state)) && state == 1;
    }

    inline float GetSkill(uint64_t base, uint16_t skillIdx, uint16_t skillIdx2, uint16_t offset) {
        __m128 skillStruct{};
        uint16_t skillId[15] = { skillIdx, skillIdx2 };
        skillStruct.m128_u64[1] = base + 0xD0;
        uint64_t skill = FnSkillStruct(&skillStruct, skillId);
        if (!skill) return 0.f;
        return SDK->RPM<float>(skill + offset);
    }

    // Formal V2 uses active-edge timers. The proven per-hero direct-CD reader is
    // preserved, in full and intentionally non-executable, in DirectCooldownResearch.hpp.
    // Do not restore the old fixed-index CD helpers or the ambiguous 1.f ready sentinel.

    inline bool TryReadUlt(uint64_t base, uint16_t skillIdx, uint16_t skillIdx2, float& ultimate) {
        __m128 skillStruct{};
        uint16_t skillId[15] = { skillIdx, skillIdx2 };
        skillStruct.m128_u64[1] = base + 0xD0;
        uint64_t skill = FnSkillStruct(&skillStruct, skillId);
        if (!skill)
            return false;
        return SDK->read_range(skill + 0x60, &ultimate, sizeof(ultimate));
    }

    inline float readult(uint64_t base, uint16_t skillIdx, uint16_t skillIdx2) {
        float ultimate = 0.0f;
        return TryReadUlt(base, skillIdx, skillIdx2, ultimate) ? ultimate : 0.0f;
    }

    inline int readammo(uint64_t base, uint16_t skillIdx, uint16_t skillIdx2) {
        __m128 skillStruct{};
        uint16_t skillId[15] = { skillIdx, skillIdx2 };
        skillStruct.m128_u64[1] = base + 0xD0;
        const uint64_t skill = FnSkillStruct(&skillStruct, skillId);
        if (!skill) return -1;

        auto roundedAmmo = [](float value) -> int {
            if (!std::isfinite(value) || value < 0.0f || value > 300.0f)
                return -1;
            if (value > 0.0f && value < 0.01f)
                return -1;
            return static_cast<int>(std::lround(value));
        };

        int zeroCandidate = -1;
        auto chooseAmmo = [&zeroCandidate](int value) -> int {
            if (value > 0)
                return value;
            if (value == 0)
                zeroCandidate = 0;
            return -1;
        };

        int ammo = chooseAmmo(roundedAmmo(SDK->RPM<float>(skill + 0x60)));
        if (ammo > 0)
            return ammo;

        ammo = chooseAmmo(roundedAmmo(SDK->RPM<float>(skill + 0x50)));
        if (ammo > 0)
            return ammo;

        const int rawAmmo50 = SDK->RPM<int>(skill + 0x50);
        if (rawAmmo50 >= 0 && rawAmmo50 <= 300) {
            ammo = chooseAmmo(rawAmmo50);
            if (ammo > 0)
                return ammo;
        }

        const int rawAmmo60 = SDK->RPM<int>(skill + 0x60);
        if (rawAmmo60 >= 0 && rawAmmo60 <= 300) {
            ammo = chooseAmmo(rawAmmo60);
            if (ammo > 0)
                return ammo;
        }

        return zeroCandidate;
    }

} // namespace OW
