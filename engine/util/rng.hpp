// Mercury — deterministic pseudo-random number generation.
//
// Why not <random>?
// ----------------
// The engine must be *bit-reproducible*: the same seed and the same event
// stream must produce the same book, the same trades, and the same statistics,
// within a given compiler/platform configuration. Integer draws are portable;
// floating-point transforms can vary with libm. `std::mt19937_64` is specified
// by the standard, but `std::uniform_real_distribution` and friends are **not**,
// so their output differs between libstdc++, libc++, and MSVC. For a project
// whose entire scientific claim rests on being able to replay an adversarial
// regime and re-check it, that is unacceptable.
//
// So Mercury carries its own tiny, fully-specified generator and its own
// distribution transforms:
//
//   * `Rng` is a 128-bit-state PCG-style generator (xoshiro-like output) with
//     explicit seed mixing, so it is identical everywhere. It is fast (a few ns
//     per draw) which matters because the simulator draws millions of times.
//   * `split()` derives an independent child stream. This is what makes
//     *common random numbers* possible: policy A and policy B can be given
//     identical market shocks by deriving both from the same parent seed, and
//     parallel workers can be given disjoint streams without overlap.
//
// All integer routines are exact and unsigned-wrapping by construction, so
// there is no undefined behaviour in the generator itself.

#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace mercury {

/// A small, portable, deterministic pseudo-random number generator.
///
/// The state is two 64-bit words. Output uses the "xoshiro/PCG-style" fixed
/// permutation below; the exact constants are part of Mercury's reproducibility
/// contract and must never be changed without bumping the on-disk result format.
class Rng {
public:
    /// Construct from a 64-bit seed. Two generators seeded the same way produce
    /// the same stream on every platform.
    explicit Rng(std::uint64_t seed = 0) { seed_stream(seed); }

    /// Construct a raw generator from a full 128-bit state (used by `split`).
    Rng(std::uint64_t a, std::uint64_t b) noexcept : s0_(a), s1_(b) {
        if (s0_ == 0 && s1_ == 0) {
            // The all-zero state is a fixed point of the xorshift step; avoid it.
            s1_ = 0x9E3779B97F4A7C15ULL;
        }
    }

    /// Re-seed in place. Mixes the seed so that low-entropy seeds (0, 1, 2, ...)
    /// still produce well-separated streams.
    void seed_stream(std::uint64_t seed) noexcept {
        std::uint64_t z = seed + 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31);
        s0_ = z;
        s1_ = z ^ 0xD1B54A32D192ED03ULL;
        if (s0_ == 0 && s1_ == 0) {
            s1_ = 0x9E3779B97F4A7C15ULL;
        }
    }

    /// Next 64-bit value. Uses a two-word rotate/xor/add permutation; deterministic and
    /// platform-independent (all arithmetic is unsigned, hence well-defined).
    [[nodiscard]] std::uint64_t next_u64() noexcept {
        const std::uint64_t result = rotl(s0_ + s1_, 17) + s0_;
        const std::uint64_t t = s1_ ^ s0_;
        s0_ = rotl(s0_, 49) ^ t ^ (t << 21);
        s1_ = rotl(t, 28);
        return result;
    }

    /// Derive an independent child generator. The parent is advanced so that
    /// repeated splits give disjoint streams; children are reproduced exactly
    /// from the parent's seed.
    [[nodiscard]] Rng split() noexcept {
        const std::uint64_t a = next_u64();
        const std::uint64_t b = next_u64();
        return Rng(a, b);
    }

    // -- Uniform integer / float transforms ----------------------------------

    /// Uniform integer in [0, bound). `bound` must be non-zero.
    [[nodiscard]] std::uint64_t uniform_u64(std::uint64_t bound) noexcept {
        // Lemire's multiply-shift with rejection to remove modulo bias. We keep
        // the rejection loop because biased draws would surreptitiously bias the
        // event intensities the whole simulation is built on.
        if (bound == 0) {
            return next_u64();
        }
        std::uint64_t x = next_u64();
        __uint128_t m = static_cast<__uint128_t>(x) * static_cast<__uint128_t>(bound);
        std::uint64_t low = static_cast<std::uint64_t>(m);
        if (low < bound) {
            const std::uint64_t threshold = (~bound + 1) % bound;
            while (low < threshold) {
                x = next_u64();
                m = static_cast<__uint128_t>(x) * static_cast<__uint128_t>(bound);
                low = static_cast<std::uint64_t>(m);
            }
        }
        return static_cast<std::uint64_t>(m >> 64);
    }

    /// Uniform integer in [lo, hi], inclusive. Requires lo <= hi.
    [[nodiscard]] std::int64_t uniform_int(std::int64_t lo,
                                           std::int64_t hi) noexcept {
        if (hi <= lo) return lo;
        const std::uint64_t span =
            static_cast<std::uint64_t>(hi - lo) + 1ULL;
        return lo + static_cast<std::int64_t>(uniform_u64(span));
    }

    /// Uniform double in [0, 1). Uses 53 bits so the result is a representable
    /// dyadic rational, which keeps it identical across platforms.
    [[nodiscard]] double uniform_double() noexcept {
        return static_cast<double>(next_u64() >> 11) * 0x1.0p-53;
    }

    /// Uniform double in [lo, hi).
    [[nodiscard]] double uniform_real(double lo, double hi) noexcept {
        return lo + (hi - lo) * uniform_double();
    }

    /// Standard exponential with rate 1 (`-log(U)`); U is drawn in (0, 1] so the
    /// result is finite. Used for the inter-arrival times of a Poisson process.
    [[nodiscard]] double exponential_unit() noexcept {
        // Map to (0, 1]: 1 - u where u in [0, 1) gives (0, 1].
        const double u = 1.0 - uniform_double();
        return -std::log(u);
    }

    /// Exponential with the given `rate` (mean 1/rate). Requires rate > 0.
    [[nodiscard]] double exponential(double rate) noexcept {
        return exponential_unit() / rate;
    }

    /// Bernoulli draw with probability `p` in [0, 1].
    [[nodiscard]] bool bernoulli(double p) noexcept {
        if (p <= 0.0) return false;
        if (p >= 1.0) return true;
        return uniform_double() < p;
    }

    /// Roughly normal via the sum of 12 uniforms (Irwin–Hall); mean 0, variance
    /// 1. Cheap and sufficient for jitter; we do not rely on exact tail shape.
    [[nodiscard]] double normal() noexcept {
        double s = 0.0;
        for (int i = 0; i < 12; ++i) {
            s += uniform_double();
        }
        return s - 6.0;
    }

    [[nodiscard]] std::uint64_t state0() const noexcept { return s0_; }
    [[nodiscard]] std::uint64_t state1() const noexcept { return s1_; }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) noexcept {
        return (x << k) | (x >> (64 - k));
    }

    std::uint64_t s0_{0};
    std::uint64_t s1_{0};
};

}  // namespace mercury
