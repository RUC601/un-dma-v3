#pragma once

#include <algorithm>
#include <cstdint>

#include "Game/TargetSolutionTypes.hpp"
#include "Game/TriggerBoneSelection.hpp"

namespace OW::Config {

inline constexpr int kLegacyAimBoneChest = 0;
inline constexpr int kLegacyAimBoneHead = 1;
inline constexpr int kLegacyAimBoneNeck = 2;

inline constexpr int CountBoneMaskBits(SkeletonBoneMask mask) noexcept
{
    int bits = 0;
    mask &= kSkeletonBoneAllMask;
    while (mask != 0) {
        bits += static_cast<int>(mask & 1u);
        mask >>= 1u;
    }
    return bits;
}

// Migrates legacy (aimBoneMask, boneSelectionMode, preferredBone) into AimPriority (§7.2).
// boneSelectionMode: 0=Only, 1=Prefer, 2=Closest.
// preferredBone: 0=Chest, 1=Head, 2=Neck.
inline constexpr AimPriority DeriveAimPriorityFromLegacy(SkeletonBoneMask rawMask,
                                                         int boneSelectionMode,
                                                         int preferredBone) noexcept
{
    const SkeletonBoneMask mask = NormalizeSkeletonBoneMask(rawMask);
    const int mode = std::clamp(boneSelectionMode, 0, 2);

    if (mode == 1) {
        if (preferredBone == kLegacyAimBoneHead)
            return AimPriority::HeadThenBody;
        if (preferredBone == kLegacyAimBoneNeck)
            return AimPriority::NeckSpread;
        return AimPriority::BodyCenter;
    }

    if (mask == kSkeletonBoneHead)
        return AimPriority::HeadOnly;

    if (mode == 0) {
        if ((mask & kSkeletonBoneHead) != 0 &&
            (mask & (kSkeletonBoneNeck | kSkeletonBoneChest | kSkeletonBonePelvis)) != 0) {
            return AimPriority::HeadThenBody;
        }
        if (mask == kSkeletonBoneNeck || mask == (kSkeletonBoneNeck | kSkeletonBoneChest))
            return AimPriority::NeckSpread;
        if ((mask & ~(kSkeletonBoneChest | kSkeletonBonePelvis)) == 0)
            return AimPriority::BodyCenter;
        return AimPriority::NearestHitbox;
    }

    // mode == 2 (Closest): preserve closest-in-mask for multi-bone selections,
    // and BodyCenter for pure Chest/Pelvis masks.
    if ((mask & ~(kSkeletonBoneChest | kSkeletonBonePelvis)) == 0)
        return AimPriority::BodyCenter;
    if (mask == kSkeletonBoneNeck)
        return AimPriority::NeckSpread;
    return AimPriority::NearestHitbox;
}

// Migrates legacy (aimBoneMask, boneSelectionMode) into HitAcceptance (§7.2).
// Multi-bone masks preserve legacy InAllowedAimHitWindow acceptance across the mask;
// single-bone masks keep strict SelectedOnly acceptance.
inline constexpr HitAcceptance DeriveHitAcceptanceFromLegacy(SkeletonBoneMask rawMask,
                                                             int boneSelectionMode) noexcept
{
    (void)boneSelectionMode;
    const SkeletonBoneMask mask = NormalizeSkeletonBoneMask(rawMask);
    return CountBoneMaskBits(mask) > 1
        ? HitAcceptance::AnyValidHitbox
        : HitAcceptance::SelectedOnly;
}

// Unified loader migration: explicit saved values win (clamped to valid enum ranges);
// missing keys migrate deterministically from legacy bone fields.
inline constexpr void MigrateAimTargetFields(bool hasAimPriority,
                                             int savedAimPriority,
                                             bool hasHitAcceptance,
                                             int savedHitAcceptance,
                                             SkeletonBoneMask rawMask,
                                             int boneSelectionMode,
                                             int preferredBone,
                                             AimPriority& outPriority,
                                             HitAcceptance& outAcceptance) noexcept
{
    outPriority = hasAimPriority
        ? ClampAimPriority(savedAimPriority)
        : DeriveAimPriorityFromLegacy(rawMask, boneSelectionMode, preferredBone);
    outAcceptance = hasHitAcceptance
        ? ClampHitAcceptance(savedHitAcceptance)
        : DeriveHitAcceptanceFromLegacy(rawMask, boneSelectionMode);
}

// Shared Aim & Trigger slot-rule builder so ApplyLocalHeroPreset and ApplyLocalHeroTriggerPreset
// can never drop trailing Phase 1-3 fields (aimPriority, hitAcceptance, barrierPolicy, roleUtilityMode).
// Sombra stealth filtering honors BOTH the user-facing UI visibility toggle (uiIgnoreInvisibleValue,
// per V2 commit bbda8778) AND an explicit legacy/JSON preset.ignoreSombraStealth flag.
template <typename RulesT, typename PresetT>
inline void PopulateSlotTargetRulesFromPreset(RulesT& outRules,
                                              const PresetT& preset,
                                              bool uiIgnoreInvisibleValue) noexcept
{
    outRules = RulesT{};
    outRules.active = true;
    outRules.ignoreSombraStealth = uiIgnoreInvisibleValue || preset.ignoreSombraStealth;
    outRules.requireTargetable = preset.requireTargetable;
    outRules.allyHealthMaxPercent = preset.allyHealthMaxPercent;
    outRules.maxTargetSpeedMps = preset.maxTargetSpeedMps;
    outRules.allyBoneMask = preset.allyBoneMask;
    outRules.predictionAlgorithm = preset.predictionAlgorithm;
    outRules.boneSelectionMode = preset.boneSelectionMode;
    outRules.preferredBone = preset.preferredBone;
    outRules.aimPriority = static_cast<int>(ClampAimPriority(preset.aimPriority));
    outRules.hitAcceptance = static_cast<int>(ClampHitAcceptance(preset.hitAcceptance));
    outRules.barrierPolicy = static_cast<int>(ClampBarrierPolicy(preset.barrierPolicy));
    outRules.roleUtilityMode = static_cast<int>(ClampRoleUtilityMode(preset.roleUtilityMode));
}

} // namespace OW::Config
