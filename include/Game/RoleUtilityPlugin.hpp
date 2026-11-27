#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "Game/GameData.hpp"
#include "Game/TargetSolutionTypes.hpp"

namespace OW::TargetPipeline {

inline constexpr uint64_t kHeroAnaId = GameData::MakeHeroId(0x13B);

// Normalized inputs passed to Stage 1 / Stage 2 role utility plugins (§4.2, Phase 3).
// NOTE: Stage 0 ProtectionFilterPasses remains strictly responsible for rejecting immortal
// or barrier-projected ENEMIES while NEVER rejecting ALLIES receiving healing (§1.2, §4.1).
struct RoleUtilityContext {
    uint64_t localHeroId = 0;
    int teamMode = 0;                 // 0=Enemies, 1=Allies, 2=All
    bool candidateIsEnemy = true;     // entity.Team (true=enemy, false=ally)
    uint64_t candidateHeroId = 0;
    float hp = 0.0f;
    float maxHp = 0.0f;
    float angleDeg = 0.0f;
    float effectiveFovDeg = 15.0f;
    float distance = 0.0f;
    float allyHealthMaxPercent = 100.0f;
    bool candidateImmortal = false;
    bool candidateBarrierProtected = false;
};

// Pluggable role utility plugin contract (§4.2, §9 Phase 3).
// Implementations must be deterministic, finite, and allocation-free on the hot path.
class IRoleUtilityPlugin {
public:
    virtual ~IRoleUtilityPlugin() = default;
    virtual const char* Name() const noexcept = 0;
    virtual bool AppliesTo(const RoleUtilityContext& ctx) const noexcept = 0;
    virtual float EvaluateUtility(const RoleUtilityContext& ctx) const noexcept = 0;
};

// Tuning parameters for the first built-in plugin: Smart Ana (§4.2).
// Balances ally low-HP crisis saving vs enemy one-shot execute (70-75 damage tier),
// while guaranteeing full/high-HP allies never steal crosshair focus from enemies.
struct SmartAnaConfig {
    // Ally healing crisis curve: alpha_heal * (1 - hpFrac)^gamma + beta_crit * I(hpFrac < critThresh)
    float healAlpha = 0.45f;
    float healGamma = 1.60f;
    float criticalLowFraction = 0.35f;
    float criticalLowBonus = 0.38f;
    // Frontline tank burst-save weight (maxHp >= 400 and below 50% HP)
    float tankMaxHpThreshold = 400.0f;
    float tankLowFraction = 0.50f;
    float tankLowBonus = 0.08f;
    // Suppress full/high-HP allies so they never pull focus away from active enemies
    float highHealthAllyFraction = 0.85f;
    float fullHealthAllyFraction = 0.99f;
    float fullHealthAllyPenalty = -0.50f;
    float highHealthAllyBasePenalty = -0.06f;
    float highHealthAllySlopePenalty = -0.18f;
    // Enemy execute & chip curve: alpha_kill * I(hp <= 75) + alpha_chip * (1 - hpFrac)
    float enemyKillHpThreshold = 75.0f;
    float enemyKillBonus = 0.42f;
    float enemyChipAlpha = 0.18f;
    // Bounded output clamp to preserve Stage 1 geometric/intent sanity
    float minUtility = -0.60f;
    float maxUtility = 0.90f;
};

class SmartAnaRoleUtilityPlugin final : public IRoleUtilityPlugin {
public:
    explicit constexpr SmartAnaRoleUtilityPlugin(SmartAnaConfig cfg = {}) noexcept
        : cfg_(cfg) {}

    const char* Name() const noexcept override { return "smart-ana"; }

    bool AppliesTo(const RoleUtilityContext& ctx) const noexcept override
    {
        return ctx.localHeroId == kHeroAnaId;
    }

    float EvaluateUtility(const RoleUtilityContext& ctx) const noexcept override
    {
        if (!std::isfinite(ctx.hp) || !std::isfinite(ctx.maxHp) || ctx.maxHp <= 0.0f)
            return 0.0f;

        const float clampedHp = (std::max)(0.0f, ctx.hp);
        const float hpFrac = std::clamp(clampedHp / ctx.maxHp, 0.0f, 1.0f);
        const float missingFrac = 1.0f - hpFrac;

        float score = 0.0f;
        if (!ctx.candidateIsEnemy) {
            // Ally healing evaluation: Stage 0 never filters allies on imort/barrprot,
            // so Smart Ana can prioritize saving bubbled/lamp-protected dying teammates.
            if (hpFrac >= cfg_.fullHealthAllyFraction) {
                score = cfg_.fullHealthAllyPenalty;
            } else if (hpFrac >= cfg_.highHealthAllyFraction) {
                const float span = (std::max)(1e-4f, cfg_.fullHealthAllyFraction - cfg_.highHealthAllyFraction);
                const float t = std::clamp((hpFrac - cfg_.highHealthAllyFraction) / span, 0.0f, 1.0f);
                score = cfg_.highHealthAllyBasePenalty + cfg_.highHealthAllySlopePenalty * t;
            } else {
                score = cfg_.healAlpha * std::pow(missingFrac, cfg_.healGamma);
                if (hpFrac < cfg_.criticalLowFraction) {
                    score += cfg_.criticalLowBonus;
                }
                if (ctx.maxHp >= cfg_.tankMaxHpThreshold && hpFrac < cfg_.tankLowFraction) {
                    score += cfg_.tankLowBonus;
                }
            }
        } else {
            // Enemy evaluation: one-shot lethal finish (<= 75 HP) + missing-HP chip pressure.
            if (clampedHp > 0.0f && clampedHp <= cfg_.enemyKillHpThreshold) {
                score += cfg_.enemyKillBonus;
            }
            score += cfg_.enemyChipAlpha * missingFrac;
        }

        if (!std::isfinite(score))
            return 0.0f;
        return std::clamp(score, cfg_.minUtility, cfg_.maxUtility);
    }

    const SmartAnaConfig& Config() const noexcept { return cfg_; }

private:
    SmartAnaConfig cfg_{};
};

// Fixed-capacity, allocation-free plugin registry for Stage 1 / Stage 2 role utility.
class RoleUtilityRegistry {
public:
    static constexpr size_t kMaxPlugins = 16;

    RoleUtilityRegistry() noexcept
    {
        Register(&smartAna_);
    }

    bool Register(const IRoleUtilityPlugin* plugin) noexcept
    {
        if (!plugin || count_ >= kMaxPlugins)
            return false;
        for (size_t i = 0; i < count_; ++i) {
            if (plugins_[i] == plugin)
                return false;
        }
        plugins_[count_++] = plugin;
        return true;
    }

    size_t Count() const noexcept { return count_; }

    float Evaluate(RoleUtilityMode mode,
                   const RoleUtilityContext& ctx,
                   const char** outPluginName = nullptr) const noexcept
    {
        if (outPluginName) *outPluginName = nullptr;
        if (mode == RoleUtilityMode::Off)
            return 0.0f;

        if (mode == RoleUtilityMode::SmartAna) {
            if (outPluginName) *outPluginName = smartAna_.Name();
            const float value = smartAna_.EvaluateUtility(ctx);
            return std::isfinite(value) ? value : 0.0f;
        }

        // Auto mode: evaluate plugins in registration order; last registered matching custom
        // plugin overrides earlier built-ins if multiple apply, or first match when distinct.
        // Searching in reverse allows custom overrides without mutating built-ins.
        for (size_t i = count_; i > 0; --i) {
            const IRoleUtilityPlugin* plugin = plugins_[i - 1];
            if (plugin && plugin->AppliesTo(ctx)) {
                if (outPluginName) *outPluginName = plugin->Name();
                const float value = plugin->EvaluateUtility(ctx);
                return std::isfinite(value) ? value : 0.0f;
            }
        }
        return 0.0f;
    }

    static const RoleUtilityRegistry& Default() noexcept
    {
        static const RoleUtilityRegistry registry{};
        return registry;
    }

private:
    SmartAnaRoleUtilityPlugin smartAna_{};
    std::array<const IRoleUtilityPlugin*, kMaxPlugins> plugins_{};
    size_t count_ = 0;
};

} // namespace OW::TargetPipeline
