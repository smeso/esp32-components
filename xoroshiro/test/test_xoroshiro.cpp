/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <limits>
#include <random>
#include <type_traits>

#include <doctest/doctest.h>
#include "xoroshiro.hpp"

TEST_CASE("known-answer test matches the reference implementation")
{
    const struct
    {
        uint64_t seed;
        uint64_t next[5];
    } cases[] = {
        { 0,
          { 0x6f68e1e7e2646ee1ULL,
            0xbf971b7f454094adULL,
            0x48f2de556f30de38ULL,
            0x6ea7c59f89bbfc75ULL,
            0x765437c08f02e2f5ULL } },
        { 1,
          { 0x08260b0f1b52fcacULL,
            0x5d9320f71ce29ff1ULL,
            0x28197699ec67f190ULL,
            0x593b393b9d1e5795ULL,
            0x38d7e95386fef5e4ULL } },
        { 42,
          { 0xe88af6caef1d3c23ULL,
            0x54a303b2a5a54931ULL,
            0xf370812ccd646345ULL,
            0x345839c63f9abb35ULL,
            0x57c3b20e1a93eb7fULL } },
        { 12345,
          { 0xe08ec422beebbea0ULL,
            0xc5454d3ad5892bf0ULL,
            0x5223964c36832da0ULL,
            0x8ea7792a1152a13aULL,
            0x2a085815e39fccffULL } },
        { 67,
          { 0x5a0dfed79d58d9c7ULL,
            0xe93dce4ce539eaecULL,
            0x9092577f8f495f71ULL,
            0xd1ad9e6389a56be3ULL,
            0xa4b780eb03ef5911ULL } },
    };
    for (const auto &c : cases) {
        xoroshiro128pp rng(c.seed);
        for (uint64_t v : c.next) {
            CHECK(rng() == v);
        }
    }
}

TEST_CASE("min and max match the underlying type")
{
    CHECK(xoroshiro128pp::min() == std::numeric_limits<uint64_t>::min());
    CHECK(xoroshiro128pp::max() == std::numeric_limits<uint64_t>::max());
}

TEST_CASE("same seed produces the same sequence")
{
    xoroshiro128pp a(42);
    xoroshiro128pp b(42);
    for (int i = 0; i < 100; i++) {
        CHECK(a() == b());
    }
}

TEST_CASE("different seeds produce different sequences")
{
    xoroshiro128pp a(1);
    xoroshiro128pp b(2);
    bool different = false;
    for (int i = 0; i < 100; i++) {
        if (a() != b())
            different = true;
    }
    CHECK(different);
}

TEST_CASE("two-argument seed matches the combined 64-bit seed")
{
    xoroshiro128pp a(0x12345678, 0x9ABCDEF0);
    xoroshiro128pp b(0x9ABCDEF012345678ULL);
    for (int i = 0; i < 100; i++) {
        CHECK(a() == b());
    }
}

TEST_CASE("satisfies UniformRandomBitGenerator")
{
    static_assert(std::is_invocable_r_v<uint64_t, xoroshiro128pp>);
    static_assert(std::is_same_v<xoroshiro128pp::result_type, uint64_t>);
    std::uniform_int_distribution<uint64_t> dist;
    xoroshiro128pp rng(7);
    for (int i = 0; i < 100; i++) {
        (void) dist(rng);
    }
}

TEST_CASE("works at compile time")
{
    static_assert([] {
        xoroshiro128pp rng(42);
        return rng() == 0xe88af6caef1d3c23ULL;
    }());
    static_assert([] {
        xoroshiro128pp a(0x12345678, 0x9ABCDEF0);
        xoroshiro128pp b(0x9ABCDEF012345678ULL);
        return a() == b();
    }());
}
