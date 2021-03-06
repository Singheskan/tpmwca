// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/webcrypto/algorithm_implementation.h"

#include "components/webcrypto/blink_key_handle.h"
#include "components/webcrypto/status.h"

#include <stdio.h> //cb

namespace webcrypto {

AlgorithmImplementation::~AlgorithmImplementation() {
}

Status AlgorithmImplementation::Encrypt(
    const blink::WebCryptoAlgorithm& algorithm,
    const blink::WebCryptoKey& key,
    const CryptoData& data,
    std::vector<uint8_t>* buffer) const {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::Decrypt(
    const blink::WebCryptoAlgorithm& algorithm,
    const blink::WebCryptoKey& key,
    const CryptoData& data,
    std::vector<uint8_t>* buffer) const {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::Sign(const blink::WebCryptoAlgorithm& algorithm,
                                     const blink::WebCryptoKey& key,
                                     const CryptoData& data,
                                     std::vector<uint8_t>* buffer) const {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::Verify(
    const blink::WebCryptoAlgorithm& algorithm,
    const blink::WebCryptoKey& key,
    const CryptoData& signature,
    const CryptoData& data,
    bool* signature_match) const {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::Digest(
    const blink::WebCryptoAlgorithm& algorithm,
    const CryptoData& data,
    std::vector<uint8_t>* buffer) const {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::DigestTest(
    const blink::WebCryptoAlgorithm& algorithm,
    const CryptoData& data,
    std::vector<uint8_t>* buffer) const {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::TPMInit() {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::TPMGetRandom(
    std::vector<uint8_t>* buffer) {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::TPMCreatePrimary() {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::TPMCreate() {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::TPMEncrypt() {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::TPMDecrypt() {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::TPMSign() {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::TPMVerify() {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::TPMFlushContext() {
  return Status::ErrorUnsupported();
}


Status AlgorithmImplementation::GenerateKey(
    const blink::WebCryptoAlgorithm& algorithm,
    bool extractable,
    blink::WebCryptoKeyUsageMask usages,
    GenerateKeyResult* result) const {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::DeriveBits(
    const blink::WebCryptoAlgorithm& algorithm,
    const blink::WebCryptoKey& base_key,
    bool has_optional_length_bits,
    unsigned int optional_length_bits,
    std::vector<uint8_t>* derived_bytes) const {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::GetKeyLength(
    const blink::WebCryptoAlgorithm& key_length_algorithm,
    bool* has_length_bits,
    unsigned int* length_bits) const {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::ImportKey(
    blink::WebCryptoKeyFormat format,
    const CryptoData& key_data,
    const blink::WebCryptoAlgorithm& algorithm,
    bool extractable,
    blink::WebCryptoKeyUsageMask usages,
    blink::WebCryptoKey* key) const {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::ExportKey(blink::WebCryptoKeyFormat format,
                                          const blink::WebCryptoKey& key,
                                          std::vector<uint8_t>* buffer) const {
  return Status::ErrorUnsupported();
}

Status AlgorithmImplementation::SerializeKeyForClone(
    const blink::WebCryptoKey& key,
    blink::WebVector<uint8_t>* key_data) const {
  *key_data = GetSerializedKeyData(key);
  return Status::Success();
}

Status AlgorithmImplementation::DeserializeKeyForClone(
    const blink::WebCryptoKeyAlgorithm& algorithm,
    blink::WebCryptoKeyType type,
    bool extractable,
    blink::WebCryptoKeyUsageMask usages,
    const CryptoData& key_data,
    blink::WebCryptoKey* key) const {
  return Status::ErrorUnsupported();
}

}  // namespace webcrypto
