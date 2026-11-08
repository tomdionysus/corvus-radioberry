// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace radioberry {

// Incremental SHA-256 (FIPS 180-4). Used to identify gateware images, so the
// tools need no crypto library.
class Sha256 {
public:
    using Digest = std::array<std::uint8_t, 32>;

    Sha256();

    void update(std::span<const std::uint8_t> data);
    void update(std::string_view data);
    Digest finish();

    // Lower-case hexadecimal digest of a whole buffer.
    static std::string hex(std::span<const std::uint8_t> data);
    static std::string hex(std::string_view data);
    static std::string to_hex(const Digest& digest);

private:
    void compress(const std::uint8_t* block);

    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;
};

}  // namespace radioberry
