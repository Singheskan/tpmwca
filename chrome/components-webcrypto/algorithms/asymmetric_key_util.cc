// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/webcrypto/algorithms/asymmetric_key_util.h"

#include <stdint.h>
#include <utility>

#include "components/webcrypto/algorithms/util.h"
#include "components/webcrypto/blink_key_handle.h"
#include "components/webcrypto/crypto_data.h"
#include "components/webcrypto/generate_key_result.h"
#include "components/webcrypto/status.h"
#include "crypto/openssl_util.h"
#include "third_party/boringssl/src/include/openssl/bytestring.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/mem.h"

#include "third_party/blink/public/platform/web_crypto_key_algorithm.h"

namespace webcrypto {

namespace {

// Exports an EVP_PKEY public key to the SPKI format.
Status ExportPKeySpki(EVP_PKEY* key, std::vector<uint8_t>* buffer) {
  crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);

  uint8_t* der;
  size_t der_len;
  bssl::ScopedCBB cbb;
  if (!CBB_init(cbb.get(), 0) || !EVP_marshal_public_key(cbb.get(), key) ||
      !CBB_finish(cbb.get(), &der, &der_len)) {
    return Status::ErrorUnexpected();
  }
  buffer->assign(der, der + der_len);
  OPENSSL_free(der);
  return Status::Success();
}

// Exports an EVP_PKEY private key to the PKCS8 format.
Status ExportPKeyPkcs8(EVP_PKEY* key, std::vector<uint8_t>* buffer) {
  crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);

  // TODO(eroman): Use the OID specified by webcrypto spec.
  //               http://crbug.com/373545
  uint8_t* der;
  size_t der_len;
  bssl::ScopedCBB cbb;
  if (!CBB_init(cbb.get(), 0) || !EVP_marshal_private_key(cbb.get(), key) ||
      !CBB_finish(cbb.get(), &der, &der_len)) {
    return Status::ErrorUnexpected();
  }
  buffer->assign(der, der + der_len);
  OPENSSL_free(der);
  return Status::Success();
}

}  // namespace

Status CreateWebCryptoPublicKey(bssl::UniquePtr<EVP_PKEY> public_key,
                                const blink::WebCryptoKeyAlgorithm& algorithm,
                                bool extractable,
                                blink::WebCryptoKeyUsageMask usages,
                                blink::WebCryptoKey* key) {
  // Serialize the key at creation time so that if structured cloning is
  // requested it can be done synchronously from the Blink thread.
  std::vector<uint8_t> spki_data;
  // printf("%02x ", spki_data[0]);f
  Status status = ExportPKeySpki(public_key.get(), &spki_data);
  printf("%02x ", spki_data[0]);
  if (status.IsError())
    return status;

  blink::WebCryptoAlgorithmId id = algorithm.Id();
  if (id == 8) {
    printf("\n CreateWebCryptoPublicKey - RSA-OAEP");
    TPMInit();
    TPMCreatePrimary();
    // TPMCreate();
    // TPMSetRawKeyBytes(spki_data.data());
  } else if (id == 11) {
    printf("\n CreateWebCryptoPublicKey - RSA-PSS");
    TPMInit();
    TPMCreatePrimarySignVerify();
  }
  printf("%02x ", spki_data[0]);

  *key = blink::WebCryptoKey::Create(
      CreateAsymmetricKeyHandle(std::move(public_key), spki_data),
      blink::kWebCryptoKeyTypePublic, extractable, algorithm, usages);

  TPMWritePublicKey(&spki_data);
  printf("%02x ", spki_data[0]);
  return Status::Success();
}

Status CreateWebCryptoPrivateKey(bssl::UniquePtr<EVP_PKEY> private_key,
                                 const blink::WebCryptoKeyAlgorithm& algorithm,
                                 bool extractable,
                                 blink::WebCryptoKeyUsageMask usages,
                                 blink::WebCryptoKey* key) {
  // Serialize the key at creation time so that if structured cloning is
  // requested it can be done synchronously from the Blink thread.
  std::vector<uint8_t> pkcs8_data;
  Status status = ExportPKeyPkcs8(private_key.get(), &pkcs8_data);
  blink::WebCryptoAlgorithmId id = algorithm.Id();
  if (status.IsError())
    return status;

  if (id == 8) {
    printf("\n CreateWebCryptoPrivateKey - RSA-OAEP");
    // TPMCreate();
    // TPMSetRawKeyBytes(pkcs8_data.data());
    // printf("%02x ", pkcs8_data[0]);
  } else if (id == 11) {
    printf("\n CreateWebCryptoPrivateKey - RSA-PSS");
    // TPMCreateSignVerify();
    // TPMSetRawKeyBytes(pkcs8_data.data());
    // printf("%02x ", pkcs8_data[0]);
  }

  *key = blink::WebCryptoKey::Create(
      CreateAsymmetricKeyHandle(std::move(private_key), pkcs8_data),
      blink::kWebCryptoKeyTypePrivate, extractable, algorithm, usages);
  
  return Status::Success();
}

Status ImportUnverifiedPkeyFromSpki(const CryptoData& key_data,
                                    int expected_pkey_id,
                                    bssl::UniquePtr<EVP_PKEY>* out_pkey) {
  crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);

  CBS cbs;
  CBS_init(&cbs, key_data.bytes(), key_data.byte_length());
  bssl::UniquePtr<EVP_PKEY> pkey(EVP_parse_public_key(&cbs));
  if (!pkey || CBS_len(&cbs) != 0)
    return Status::DataError();

  if (EVP_PKEY_id(pkey.get()) != expected_pkey_id)
    return Status::DataError();  // Data did not define expected key type.

  *out_pkey = std::move(pkey);
  return Status::Success();
}

Status ImportUnverifiedPkeyFromPkcs8(const CryptoData& key_data,
                                     int expected_pkey_id,
                                     bssl::UniquePtr<EVP_PKEY>* out_pkey) {
  crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);

  CBS cbs;
  CBS_init(&cbs, key_data.bytes(), key_data.byte_length());
  bssl::UniquePtr<EVP_PKEY> pkey(EVP_parse_private_key(&cbs));
  if (!pkey || CBS_len(&cbs) != 0)
    return Status::DataError();

  if (EVP_PKEY_id(pkey.get()) != expected_pkey_id)
    return Status::DataError();  // Data did not define expected key type.

  *out_pkey = std::move(pkey);
  return Status::Success();
}

Status GetUsagesForGenerateAsymmetricKey(
    blink::WebCryptoKeyUsageMask combined_usages,
    blink::WebCryptoKeyUsageMask all_public_usages,
    blink::WebCryptoKeyUsageMask all_private_usages,
    blink::WebCryptoKeyUsageMask* public_usages,
    blink::WebCryptoKeyUsageMask* private_usages) {
  // Ensure that the combined usages is a subset of the total possible usages.
  Status status = CheckKeyCreationUsages(all_public_usages | all_private_usages,
                                         combined_usages);
  if (status.IsError())
    return status;

  *public_usages = combined_usages & all_public_usages;
  *private_usages = combined_usages & all_private_usages;

  // NOTE: empty private_usages is allowed at this layer. Such keys will be
  // rejected at a higher layer.

  return Status::Success();
}

}  // namespace webcrypto
