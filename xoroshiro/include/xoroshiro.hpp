/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <limits>

/**
 * Header-only xoroshiro128++ pseudo-random number generator.
 *
 * Reference: https://prng.di.unimi.it/xoroshiro128plusplus.c
 * (David Blackman and Sebastiano Vigna)
 *
 * @warning This is a fast statistical PRNG, **not** a cryptographically
 * secure one: observing a few consecutive outputs is enough to recover the
 * internal 128-bit state, after which all past and future outputs become
 * predictable. Do not use it for tokens, nonces, keys or anything else
 * security-relevant.
 *
 * @warning Instances are **not thread-safe**: the 128-bit state is updated
 * non-atomically, so sharing an instance between tasks without external
 * synchronization is a data race. Give each task its own instance instead.
 *
 * @warning This header requires C++20 or later (it uses std::rotl from
 * <bit>). Make sure the consuming project sets CMAKE_CXX_STANDARD to 20 or
 * higher.
 */
class xoroshiro128pp
{
    static_assert(__cplusplus >= 202002L, "xoroshiro128pp requires C++20 or later");

  public:
    using result_type = uint64_t;

    explicit constexpr xoroshiro128pp(uint64_t seed_value = 0) noexcept
    {
        seed(seed_value);
    }

    constexpr xoroshiro128pp(uint32_t seed_low, uint32_t seed_high) noexcept
    {
        seed(seed_low, seed_high);
    }

    constexpr void seed(uint64_t seed_value) noexcept
    {
        uint64_t x = seed_value;
        x += GOLDEN_GAMMA;
        state_[0] = splitmix64(x);
        x += GOLDEN_GAMMA;
        state_[1] = splitmix64(x);
    }

    /**
     * Seed from two 32-bit values: @p seed_low becomes the low 32 bits and
     * @p seed_high the high 32 bits of the combined 64-bit seed.
     */
    constexpr void seed(uint32_t seed_low, uint32_t seed_high) noexcept
    {
        seed(seed_low | (static_cast<uint64_t>(seed_high) << 32));
    }

    static constexpr result_type min() noexcept
    {
        return std::numeric_limits<result_type>::min();
    }
    static constexpr result_type max() noexcept
    {
        return std::numeric_limits<result_type>::max();
    }

    [[nodiscard]] constexpr result_type operator()() noexcept
    {
        const uint64_t s0 = state_[0];
        uint64_t s1 = state_[1];
        const uint64_t result = std::rotl(s0 + s1, 17) + s0;

        s1 ^= s0;
        state_[0] = std::rotl(s0, 49) ^ s1 ^ (s1 << 21);
        state_[1] = std::rotl(s1, 28);

        return result;
    }

  private:
    std::array<uint64_t, 2> state_{};

    static constexpr uint64_t GOLDEN_GAMMA = 0x9e3779b97f4a7c15ULL;

    /**
     * SplitMix64 output function (https://prng.di.unimi.it/splitmix64.c).
     * The caller owns the counter: mirroring the reference implementation,
     * it must be advanced by GOLDEN_GAMMA before every call.
     */
    [[nodiscard]] static constexpr uint64_t splitmix64(uint64_t x) noexcept
    {
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
};
