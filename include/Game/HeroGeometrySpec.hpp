#pragma once

#include <cstdint>
#include <optional>

#include "Game/AimArchitecture.hpp"

namespace OW {

struct HitboxSegment {
    Vector3 a{};
    Vector3 b{};
    float radius = 0.0f;
};

struct ProjectileTrajectory {
    Vector3 origin{};
    Vector3 initialVelocity{};
    Vector3 targetVelocity{};
    Vector3 acceleration{};
    float maxTimeSeconds = 0.0f;
};

bool ContinuousTrajectoryIntersectsHitbox(const ProjectileTrajectory& trajectory,
                                          const HitboxSegment& hitbox,
                                          float projectileRadius,
                                          float* hitTimeSeconds = nullptr);

const HeroGeometrySpec* ResolveHeroGeometrySpec(uint64_t heroId);
const BoneHitboxSpec* ResolveBoneHitboxSpec(uint64_t heroId, int boneId);
float ResolveBoneHitboxRadius(uint64_t heroId, int boneId, float fallbackRadius);
float EffectiveExpandedHitboxRadius(float shapeRadius,
                                    float projectileRadius,
                                    float hitboxScalePercent);
// Bounded reverse-lateral safety margin offset applied only to the desired
// steering aim point (never to actual Hitbox contact geometry).
// Returns a world-space offset opposite to the target's lateral motion,
// bounded within half of the scaled effective radius and within the lateral
// lead displacement. Returns zero for stationary or purely radial motion.
Vector3 ComputeReverseLateralAimOffset(const Vector3& camera,
                                       const Vector3& rawAimPoint,
                                       const Vector3& predictedAimPoint,
                                       const Vector3& targetVelocity,
                                       float effectiveHitRadius,
                                       float projectileRadius,
                                       float hitboxScalePercent);
float ResolveEffectiveHitWindow(uint64_t heroId,
                                int boneId,
                                const WeaponSpec* weapon,
                                float hitboxScalePercent,
                                float fallbackBoneRadius,
                                float projectileRadiusOverride = -1.0f);
float ResolveEffectiveHitWindow(const c_entity& entity,
                                int boneId,
                                const WeaponSpec* weapon,
                                float hitboxScalePercent,
                                float fallbackBoneRadius,
                                float projectileRadiusOverride = -1.0f);
int AimBoneToSkeletonBoneId(int aimBone);
bool IsCoreHitboxBoneId(int boneId);
Vector3 ResolveBoneHitboxOffsetLocal(uint64_t heroId, int boneId);
Vector3 ResolveBoneHitboxCenter(const c_entity& entity, int boneId, const Vector3& bonePoint);
bool TryResolveNativeHitboxCenter(const c_entity& entity, int boneId, Vector3& outPoint);
bool NativeHitboxSampleUsable(const c_entity& entity, uint32_t now);
// Maps canonical Structs.hpp BONE_* IDs to actual live OW2 native hitbox shape.boneId values
// (e.g. chest on bone=5 for real heroes / bone=83 for training bots, shoulders on 13/54, etc.).
inline bool NativeShapeMatchesBoneId(int shapeBoneId,
                                     int requestedBoneId,
                                     ModelShapes::CriticalState critical,
                                     bool requireExplicitCritical) noexcept
{
    if (requireExplicitCritical) {
        if (critical != ModelShapes::CriticalState::Critical)
            return false;
        if (requestedBoneId == BONE_HEAD)
            return true;
    }
    if (shapeBoneId == requestedBoneId)
        return true;
    switch (requestedBoneId) {
    case BONE_HEAD:
        return critical == ModelShapes::CriticalState::Critical ||
            shapeBoneId == 17 || shapeBoneId == 18 || shapeBoneId == 11 || shapeBoneId == 105;
    case BONE_NECK:
        return shapeBoneId == 16 || shapeBoneId == 5 || shapeBoneId == 83;
    case BONE_CHEST:
        return shapeBoneId == 2 || shapeBoneId == 5 || shapeBoneId == 4 ||
            shapeBoneId == 15 || shapeBoneId == 83 || shapeBoneId == 82 || shapeBoneId == 81;
    case BONE_PELVIS:
        return shapeBoneId == 3 || shapeBoneId == 2;
    case BONE_L_SHOULDER:
        return shapeBoneId == 49 || shapeBoneId == 13 || shapeBoneId == 80;
    case BONE_R_SHOULDER:
        return shapeBoneId == 54 || shapeBoneId == 53;
    case BONE_L_ELBOW:
        return shapeBoneId == 14;
    case BONE_R_ELBOW:
        return shapeBoneId == 51 || shapeBoneId == 55;
    case BONE_L_HAND:
        return shapeBoneId == 41 || shapeBoneId == 28 || shapeBoneId == 386;
    case BONE_R_HAND:
        return shapeBoneId == 71 || shapeBoneId == 58 || shapeBoneId == 387;
    case BONE_L_KNEE:
        return shapeBoneId == 89 || shapeBoneId == 85;
    case BONE_R_KNEE:
        return shapeBoneId == 99 || shapeBoneId == 95;
    case BONE_L_SHANK:
        return shapeBoneId == 87 || shapeBoneId == 89 || shapeBoneId == 90;
    case BONE_R_SHANK:
        return shapeBoneId == 97 || shapeBoneId == 99 || shapeBoneId == 100;
    case BONE_L_ANKLE:
        return shapeBoneId == 86 || shapeBoneId == 90;
    case BONE_R_ANKLE:
        return shapeBoneId == 96 || shapeBoneId == 100;
    default:
        return false;
    }
}
// Distance to the closest associated shape's axis divided by its expanded radius.
// A score of 1 is contact. An absent score means the caller should use its bone fallback.
std::optional<float> NativeHitWindowScore(const c_entity& entity, int boneId, uint32_t now,
    const Vector3& origin, const Vector3& direction, const Vector3& predictedDisplacement,
    float projectileRadius, float hitboxScalePercent, bool requireExplicitCritical = false);
std::optional<float> NativeHitWindowScore(const c_entity& entity, int boneId, uint32_t now,
    const Vector3& origin, const Vector3& direction, const NativeHitWindowInput& input,
    const Vector3& extraDisplacement = {}, bool requireExplicitCritical = false);

} // namespace OW
