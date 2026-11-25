#pragma once

// Ground-plane clipping for V3 ray work.
//
// This header owns the geometry only. It deliberately takes the ground height as an explicit
// parameter: the product has no verified ground-height offset yet (see V3_PROGRESS), so a
// caller must supply one. Do not substitute a guessed constant here.
//
// Convention: world Y grows upward (see Target.hpp's `launchUpBias`, added on +Y to bias a
// launch upward). "Below the ground" therefore means `point.Y < groundY`.
//
// Ballistic model: a projectile leaves `from` along `unitDirection` at `speed`, and gravity
// accelerates it along -Y. This is the standard drop model; it is NOT the target-motion model
// used by SolveBallisticIntercept() in BallisticSolver.hpp, which predicts where a falling
// target will be for a straight-line shot.

#include <cmath>

#include "Utils/Types.hpp"

namespace OW {

// Straight (hitscan) segment clipped against a horizontal ground plane.
struct GroundSegmentClip {
    bool clipped = false;
    Vector3 point{};
};

// Returns the first point of the camera -> aim segment that reaches `groundY`. When the whole
// segment stays at or above the plane nothing is clipped and `point` is unused.
inline GroundSegmentClip ClipSegmentToGround(const Vector3& from, const Vector3& to, float groundY)
{
    GroundSegmentClip result{};
    if (!std::isfinite(from.Y) || !std::isfinite(to.Y) || !std::isfinite(groundY))
        return result;
    if (to.Y >= groundY)
        return result;  // the aim stays on or above the ground

    if (from.Y < groundY) {
        // The origin is already under the plane; nothing about this segment is usable.
        result.clipped = true;
        result.point = from;
        return result;
    }

    const float span = to.Y - from.Y;  // negative
    if (!std::isfinite(span) || span >= -1e-6f) {
        // Degenerate vertical span with the endpoint below the plane.
        result.clipped = true;
        result.point = to;
        return result;
    }

    const float t = (groundY - from.Y) / span;
    if (!std::isfinite(t) || t <= 0.0f) {
        result.clipped = true;
        result.point = from;
        return result;
    }
    const float clamped = (t > 1.0f) ? 1.0f : t;
    result.clipped = true;
    result.point = Vector3(from.X + (to.X - from.X) * clamped,
                           from.Y + span * clamped,
                           from.Z + (to.Z - from.Z) * clamped);
    return result;
}

// Ballistic landing point on a horizontal ground plane.
struct GroundLanding {
    bool solved = false;
    Vector3 point{};
    float seconds = 0.0f;
};

// Solves y(t) = groundY for the descending branch of a dropped projectile.
// `gravity` is a positive magnitude; `unitDirection` must be normalized by the caller.
// Returns unsolved when the path never descends to the plane inside `maxSeconds`.
inline GroundLanding SolveGroundLanding(const Vector3& from, const Vector3& unitDirection,
                                        float speed, float gravity, float groundY,
                                        float maxSeconds = 5.0f)
{
    GroundLanding result{};
    if (!std::isfinite(from.Y) || !std::isfinite(groundY) ||
        !std::isfinite(unitDirection.X) || !std::isfinite(unitDirection.Y) ||
        !std::isfinite(unitDirection.Z) || !std::isfinite(speed) ||
        !std::isfinite(gravity) || !std::isfinite(maxSeconds))
        return result;
    if (speed <= 0.0f || gravity < 0.0f || maxSeconds <= 0.0f)
        return result;

    const double y0 = from.Y;
    const double g = gravity;
    const double vy = static_cast<double>(unitDirection.Y) * speed;
    const double drop = y0 - groundY;  // positive while the origin is above the ground

    if (drop < 0.0) {
        // Already under the plane: the origin is the only defensible landing.
        result.solved = true;
        result.point = from;
        result.seconds = 0.0f;
        return result;
    }

    double seconds = -1.0;
    if (g <= 1e-9) {
        if (vy < -1e-9)
            seconds = drop / -vy;  // straight line downwards
    } else {
        const double discriminant = vy * vy + 2.0 * g * drop;
        if (discriminant >= 0.0) {
            const double root = std::sqrt(discriminant);
            const double candidate = (vy + root) / g;  // vy - g*t == -root, i.e. descending
            if (candidate > 0.0)
                seconds = candidate;
        }
    }

    if (!(seconds >= 0.0) || seconds > static_cast<double>(maxSeconds))
        return result;

    result.solved = true;
    result.seconds = static_cast<float>(seconds);
    result.point = Vector3(static_cast<float>(from.X + static_cast<double>(unitDirection.X) * speed * seconds),
                           static_cast<float>(y0 + vy * seconds - 0.5 * g * seconds * seconds),
                           static_cast<float>(from.Z + static_cast<double>(unitDirection.Z) * speed * seconds));
    return result;
}

}  // namespace OW
