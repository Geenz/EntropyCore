/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/**
 * @file SHA256.h
 * @brief SHA-256 hash utility wrapping libsodium
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace EntropyEngine::Core::Crypto
{

/**
 * @brief Incremental SHA-256 hash using libsodium.
 *
 * Supports one-shot construction and incremental update().
 * The header avoids including <sodium.h> by using opaque storage
 * for the hash state struct.
 */
class SHA256
{
public:
    using Digest = std::array<uint8_t, 32>;

    /// Default constructor: initializes empty hash state.
    SHA256();

    /// One-shot: hash a block of data.
    SHA256(const void* data, size_t len);

    /// One-shot: hash a string_view.
    explicit SHA256(std::string_view str);

    ~SHA256();

    SHA256(const SHA256&) = delete;
    SHA256& operator=(const SHA256&) = delete;

    /// Feed more data into the hash.
    SHA256& update(const void* data, size_t len);

    /// Feed a string_view into the hash.
    SHA256& update(std::string_view str);

    /// Return the 32-byte message digest.
    [[nodiscard]] Digest getDigest() const;

    /// Return the digest as a 64-character lowercase hex string.
    [[nodiscard]] std::string getHexDigest() const;

private:
    // Opaque storage for crypto_hash_sha256_state.
    // libsodium guarantees this struct is at most 104 bytes on all platforms.
    alignas(8) uint8_t _stateStorage[104];
    mutable bool _finalized = false;
    mutable Digest _digest{};

    void finalize() const;
};

}  // namespace EntropyEngine::Core::Crypto
