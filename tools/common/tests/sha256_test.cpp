// SPDX-License-Identifier: GPL-2.0-or-later
#include "radioberry/sha256.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using radioberry::Sha256;

TEST_CASE("SHA-256 matches the FIPS 180-4 test vectors", "[sha256]") {
    CHECK(Sha256::hex(std::string_view{}) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::hex(std::string_view{"abc"}) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(
        Sha256::hex(std::string_view{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"}) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("SHA-256 of one million 'a'", "[sha256]") {
    const std::string data(1000000, 'a');
    CHECK(Sha256::hex(data) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA-256 gives the same digest however the input is split", "[sha256]") {
    std::vector<std::uint8_t> data(1000);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }
    const std::string whole = Sha256::hex(data);

    for (const std::size_t chunk : {1UL, 3UL, 55UL, 56UL, 63UL, 64UL, 65UL, 200UL}) {
        Sha256 h;
        for (std::size_t pos = 0; pos < data.size(); pos += chunk) {
            const std::size_t n = std::min(chunk, data.size() - pos);
            h.update(std::span<const std::uint8_t>(data.data() + pos, n));
        }
        CHECK(Sha256::to_hex(h.finish()) == whole);
    }
}

TEST_CASE("SHA-256 padding boundaries", "[sha256]") {
    // 55, 56 and 64 bytes straddle the length field boundary.
    CHECK(Sha256::hex(std::string(55, 'x')).size() == 64);
    CHECK(Sha256::hex(std::string(56, 'x')) != Sha256::hex(std::string(55, 'x')));
    CHECK(Sha256::hex(std::string(64, 'a')) ==
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}
