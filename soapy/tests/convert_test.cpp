// SPDX-License-Identifier: GPL-2.0-or-later
#include "convert.hpp"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace radioberry::soapy;

namespace {

// One 24-bit big-endian value.
void put24(std::uint8_t* p, std::int32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 16);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v);
}

std::int32_t get16(const std::uint8_t* p) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0] << 8 | p[1]));
}

// Two frames of two receivers: receiver r, frame f carries values
// (first, second) = (1000 * (f + 1) + r, -(1000 * (f + 1) + r)).
std::vector<std::uint8_t> two_by_two() {
    std::vector<std::uint8_t> in(2 * 2 * 6);
    for (int f = 0; f < 2; ++f) {
        for (int r = 0; r < 2; ++r) {
            const int v = 1000 * (f + 1) + r;
            std::uint8_t* p = in.data() + static_cast<std::size_t>(f * 12 + r * 6);
            put24(p, v);
            put24(p + 3, -v);
        }
    }
    return in;
}

}  // namespace

TEST_CASE("formats by name") {
    CHECK(parse_format("CF32") == Format::CF32);
    CHECK(parse_format("CS16") == Format::CS16);
    CHECK(parse_format("CS32") == Format::CS32);
    CHECK_FALSE(parse_format("CU8").has_value());
    CHECK(std::string(format_name(Format::CF32)) == "CF32");
    CHECK(std::string(format_name(Format::CS16)) == "CS16");
    CHECK(std::string(format_name(Format::CS32)) == "CS32");
    CHECK(sample_bytes(Format::CF32) == 8);
    CHECK(sample_bytes(Format::CS16) == 4);
    CHECK(sample_bytes(Format::CS32) == 8);
}

TEST_CASE("receive frames become channels with I and Q swapped") {
    const auto in = two_by_two();

    SECTION("CF32 at 1.0 full scale") {
        std::vector<std::complex<float>> a(3);
        std::vector<std::complex<float>> b(3);
        void* buffs[] = {a.data(), b.data()};
        rx_convert(in.data(), 2, 2, 2, Format::CF32, buffs, 1);
        CHECK(a[1].real() == Catch::Approx(-1000 / kRxFullScale));
        CHECK(a[1].imag() == Catch::Approx(1000 / kRxFullScale));
        CHECK(b[2].real() == Catch::Approx(-2001 / kRxFullScale));
        CHECK(b[2].imag() == Catch::Approx(2001 / kRxFullScale));
    }
    SECTION("CS32 holds the 24-bit values") {
        std::vector<std::int32_t> a(4);
        void* buffs[] = {a.data()};
        rx_convert(in.data(), 2, 2, 1, Format::CS32, buffs, 0);  // the first channel only
        CHECK(a == std::vector<std::int32_t>{-1000, 1000, -2000, 2000});
    }
    SECTION("CS16 keeps the top 16 bits") {
        std::array<std::uint8_t, 6> frame{};
        put24(frame.data(), 0x123456);
        put24(frame.data() + 3, -0x7fff00);
        std::vector<std::int16_t> a(2);
        void* buffs[] = {a.data()};
        rx_convert(frame.data(), 1, 1, 1, Format::CS16, buffs, 0);
        CHECK(a[0] == -0x7fff);
        CHECK(a[1] == 0x1234);
    }
}

TEST_CASE("transmit samples become the driver's layout, swapped and clipped") {
    std::array<std::uint8_t, 12> out{};

    SECTION("CF32") {
        const std::complex<float> in[] = {{0.0f, 0.0f}, {0.5f, -0.25f}, {2.0f, -2.0f}};
        CHECK(tx_convert(in, 1, 2, Format::CF32, out.data()) == 1);
        CHECK(get16(out.data()) == -8192);  // Q first
        CHECK(get16(out.data() + 2) == 16384);
        CHECK(get16(out.data() + 4) == -32768);
        CHECK(get16(out.data() + 6) == 32767);
    }
    SECTION("CS16") {
        const std::int16_t in[] = {100, -200, -32768, 32767};
        CHECK(tx_convert(in, 0, 2, Format::CS16, out.data()) == 0);
        CHECK(get16(out.data()) == -200);
        CHECK(get16(out.data() + 2) == 100);
        CHECK(get16(out.data() + 4) == 32767);
        CHECK(get16(out.data() + 6) == -32768);
    }
    SECTION("CS32 is refused") {
        const std::int32_t in[] = {0, 0};
        CHECK_THROWS_AS(tx_convert(in, 0, 1, Format::CS32, out.data()), std::invalid_argument);
    }
}

TEST_CASE("drive bytes and the gateware's gain codes") {
    CHECK(drive_code(0) == 0);
    CHECK(drive_code(4) == 1);
    CHECK(drive_code(128) == 20);
    CHECK(drive_code(150) == 23);
    CHECK(drive_code(255) == 39);
    // The highest byte not above the code asked for, within the ceiling.
    CHECK(drive_for_code(20, 255) == 131);
    CHECK(drive_code(drive_for_code(20, 255)) == 20);
    CHECK(drive_for_code(39, 150) == 150);
    CHECK(drive_for_code(0, 150) == 3);
    CHECK(drive_for_code(10, 0) == 0);
    CHECK(drive_for_code(39, 300) == 255);
}
