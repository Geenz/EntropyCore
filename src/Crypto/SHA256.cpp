/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "SHA256.h"

#include <sodium.h>

#include <cstring>
#include <format>

namespace EntropyEngine::Core::Crypto
{

static_assert(sizeof(crypto_hash_sha256_state) <= 104, "crypto_hash_sha256_state exceeds opaque storage size");

static crypto_hash_sha256_state& stateRef(uint8_t* storage) {
    return *reinterpret_cast<crypto_hash_sha256_state*>(storage);
}

static const crypto_hash_sha256_state& stateRef(const uint8_t* storage) {
    return *reinterpret_cast<const crypto_hash_sha256_state*>(storage);
}

SHA256::SHA256() {
    crypto_hash_sha256_init(&stateRef(_stateStorage));
}

SHA256::SHA256(const void* data, size_t len) {
    crypto_hash_sha256_init(&stateRef(_stateStorage));
    if (data && len > 0) {
        crypto_hash_sha256_update(&stateRef(_stateStorage), static_cast<const unsigned char*>(data), len);
    }
}

SHA256::SHA256(std::string_view str) {
    crypto_hash_sha256_init(&stateRef(_stateStorage));
    if (!str.empty()) {
        crypto_hash_sha256_update(&stateRef(_stateStorage), reinterpret_cast<const unsigned char*>(str.data()),
                                  str.size());
    }
}

SHA256::~SHA256() = default;

SHA256& SHA256::update(const void* data, size_t len) {
    if (data && len > 0) {
        _finalized = false;
        crypto_hash_sha256_update(&stateRef(_stateStorage), static_cast<const unsigned char*>(data), len);
    }
    return *this;
}

SHA256& SHA256::update(std::string_view str) {
    return update(str.data(), str.size());
}

void SHA256::finalize() const {
    if (_finalized) return;

    // Copy state so we can finalize without consuming the original
    crypto_hash_sha256_state copy;
    std::memcpy(&copy, &stateRef(_stateStorage), sizeof(copy));
    crypto_hash_sha256_final(&copy, _digest.data());

    _finalized = true;
}

SHA256::Digest SHA256::getDigest() const {
    finalize();
    return _digest;
}

std::string SHA256::getHexDigest() const {
    finalize();

    std::string hex;
    hex.reserve(64);
    for (uint8_t byte : _digest) {
        hex += std::format("{:02x}", byte);
    }
    return hex;
}

}  // namespace EntropyEngine::Core::Crypto
