// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/webcrypto/webcrypto_impl.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>  //cb

#include <memory>
#include <utility>

#include "base/bind.h"
#include "base/check_op.h"
#include "base/lazy_instance.h"
#include "base/location.h"
#include "base/macros.h"
#include "base/single_thread_task_runner.h"
#include "base/task_runner.h"
#include "base/threading/thread.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/trace_event/trace_event.h"
#include "components/webcrypto/algorithm_dispatch.h"
#include "components/webcrypto/crypto_data.h"
#include "components/webcrypto/generate_key_result.h"
#include "components/webcrypto/status.h"
#include "third_party/blink/public/platform/web_crypto_key_algorithm.h"
#include "third_party/blink/public/platform/web_string.h"

namespace webcrypto {

using webcrypto::Status;

namespace {

// ---------------------
// Threading
// ---------------------
//
// WebCrypto operations can be slow. For instance generating an RSA key can
// take seconds.
//
// The strategy used here is to run a worker pool for all WebCrypto operations
// (except structured cloning). This same pool is also used by requests started
// from Blink Web Workers.
//
// A few notes to keep in mind:
//
// * PostTaskAndReply() is not used because of how it handles failures -- it
//   leaks the callback when failing to post back to the origin thread.
//
//   This is a problem since WebCrypto may be called from WebWorker threads,
//   which may be aborted at any time. Leaking would be undesirable, and
//   reachable in practice.
//
// * blink::WebArrayBuffer is NOT threadsafe, and should therefore be allocated
//   only on the target Blink thread.
//
//   TODO(eroman): Is there any way around this? Copying the result between
//                 threads is silly.
//
// * WebCryptoAlgorithm and WebCryptoKey are threadsafe, by virtue of being
//   immutable. Internally asymmetric WebCryptoKeys wrap BoringSSL's EVP_PKEY.
//   These are safe to use for BoringSSL operations across threads, provided
//   the internals of the EVP_PKEY are not mutated (they never should be
//   following ImportKey()).
//
// * blink::WebCryptoResult is not threadsafe and should only be operated on
//   the target Blink thread. HOWEVER, it is safe to delete it from any thread.
//   This can happen if by the time the operation has completed in the crypto
//   worker pool, the Blink worker thread that initiated the request is gone.
//   Posting back to the origin thread will fail, and the WebCryptoResult will
//   be deleted while running in the crypto worker pool.
class CryptoThreadPool {
 public:
  CryptoThreadPool() : worker_thread_("WebCrypto") {
    base::Thread::Options options;
    options.joinable = false;
    worker_thread_.StartWithOptions(std::move(options));
  }

  static bool PostTask(const base::Location& from_here, base::OnceClosure task);

 private:
  // TODO(gab): the pool is currently using a single non-joinable thread to
  // mimic the old behavior of using a CONTINUE_ON_SHUTDOWN SequencedTaskRunner
  // on a single-threaded SequencedWorkerPool, but we'd like to consider using
  // the ThreadPool here and allowing multiple threads (SEQUENCED or even
  // PARALLEL ExecutionMode: http://crbug.com/623700).
  base::Thread worker_thread_;

  DISALLOW_COPY_AND_ASSIGN(CryptoThreadPool);
};

base::LazyInstance<CryptoThreadPool>::Leaky crypto_thread_pool =
    LAZY_INSTANCE_INITIALIZER;

bool CryptoThreadPool::PostTask(const base::Location& from_here,
                                base::OnceClosure task) {
  return crypto_thread_pool.Get().worker_thread_.task_runner()->PostTask(
      from_here, std::move(task));
}

void CompleteWithThreadPoolError(blink::WebCryptoResult* result) {
  result->CompleteWithError(blink::kWebCryptoErrorTypeOperation,
                            "Failed posting to crypto worker pool");
}

void CompleteWithError(const Status& status, blink::WebCryptoResult* result) {
  DCHECK(status.IsError());

  result->CompleteWithError(status.error_type(),
                            blink::WebString::FromUTF8(status.error_details()));
}

void CompleteWithBufferOrError(const Status& status,
                               const std::vector<uint8_t>& buffer,
                               blink::WebCryptoResult* result) {
  if (status.IsError()) {
    CompleteWithError(status, result);
  } else {
    if (buffer.size() > UINT_MAX) {
      // WebArrayBuffers have a smaller range than std::vector<>, so
      // theoretically this could overflow.
      CompleteWithError(Status::ErrorUnexpected(), result);
    } else {
      result->CompleteWithBuffer(buffer.data(),
                                 static_cast<unsigned int>(buffer.size()));
    }
  }
}

void CompleteWithKeyOrError(const Status& status,
                            const blink::WebCryptoKey& key,
                            blink::WebCryptoResult* result) {
  if (status.IsError()) {
    CompleteWithError(status, result);
  } else {
    result->CompleteWithKey(key);
  }
}

// --------------------------------------------------------------------
// State
// --------------------------------------------------------------------
//
// Explicit state classes are used rather than base::Bind{Once,Repeating}. This
// is done both for clarity, but also to avoid extraneous allocations for things
// like passing buffers and result objects between threads.
//
// BaseState is the base class common to all of the async operations, and
// keeps track of the thread to complete on, the error state, and the
// callback into Blink.
//
// Ownership of the State object is passed between the crypto thread and the
// Blink thread. Under normal completion it is destroyed on the Blink thread.
// However it may also be destroyed on the crypto thread if the Blink thread
// has vanished (which can happen for Blink web worker threads).

struct BaseState {
  BaseState(const blink::WebCryptoResult& result,
            scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : origin_thread(task_runner), result(result) {}

  bool cancelled() { return result.Cancelled(); }

  scoped_refptr<base::TaskRunner> origin_thread;

  webcrypto::Status status;
  blink::WebCryptoResult result;

 protected:
  // Since there is no virtual destructor, must not delete directly as a
  // BaseState.
  ~BaseState() {}
};

struct EncryptState : public BaseState {
  EncryptState(const blink::WebCryptoAlgorithm& algorithm,
               const blink::WebCryptoKey& key,
               blink::WebVector<unsigned char> data,
               const blink::WebCryptoResult& result,
               scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : BaseState(result, std::move(task_runner)),
        algorithm(algorithm),
        key(key),
        data(std::move(data)) {}

  const blink::WebCryptoAlgorithm algorithm;
  const blink::WebCryptoKey key;
  const blink::WebVector<unsigned char> data;

  std::vector<uint8_t> buffer;
};

typedef EncryptState DecryptState;
typedef EncryptState DigestState;

struct GenerateKeyState : public BaseState {
  GenerateKeyState(const blink::WebCryptoAlgorithm& algorithm,
                   bool extractable,
                   blink::WebCryptoKeyUsageMask usages,
                   const blink::WebCryptoResult& result,
                   scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : BaseState(result, std::move(task_runner)),
        algorithm(algorithm),
        extractable(extractable),
        usages(usages) {}

  const blink::WebCryptoAlgorithm algorithm;
  const bool extractable;
  const blink::WebCryptoKeyUsageMask usages;

  webcrypto::GenerateKeyResult generate_key_result;
};

struct ImportKeyState : public BaseState {
  ImportKeyState(blink::WebCryptoKeyFormat format,
                 blink::WebVector<unsigned char> key_data,
                 const blink::WebCryptoAlgorithm& algorithm,
                 bool extractable,
                 blink::WebCryptoKeyUsageMask usages,
                 const blink::WebCryptoResult& result,
                 scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : BaseState(result, std::move(task_runner)),
        format(format),
        key_data(std::move(key_data)),
        algorithm(algorithm),
        extractable(extractable),
        usages(usages) {}

  const blink::WebCryptoKeyFormat format;
  const blink::WebVector<unsigned char> key_data;
  const blink::WebCryptoAlgorithm algorithm;
  const bool extractable;
  const blink::WebCryptoKeyUsageMask usages;

  blink::WebCryptoKey key;
};

struct ExportKeyState : public BaseState {
  ExportKeyState(blink::WebCryptoKeyFormat format,
                 const blink::WebCryptoKey& key,
                 const blink::WebCryptoResult& result,
                 scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : BaseState(result, std::move(task_runner)), format(format), key(key) {}

  const blink::WebCryptoKeyFormat format;
  const blink::WebCryptoKey key;

  std::vector<uint8_t> buffer;
};

typedef EncryptState SignState;

struct VerifySignatureState : public BaseState {
  VerifySignatureState(const blink::WebCryptoAlgorithm& algorithm,
                       const blink::WebCryptoKey& key,
                       blink::WebVector<unsigned char> signature,
                       blink::WebVector<unsigned char> data,
                       const blink::WebCryptoResult& result,
                       scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : BaseState(result, std::move(task_runner)),
        algorithm(algorithm),
        key(key),
        signature(std::move(signature)),
        data(std::move(data)),
        verify_result(false) {}

  const blink::WebCryptoAlgorithm algorithm;
  const blink::WebCryptoKey key;
  blink::WebVector<unsigned char> signature;
  blink::WebVector<unsigned char> data;

  bool verify_result;
};

struct WrapKeyState : public BaseState {
  WrapKeyState(blink::WebCryptoKeyFormat format,
               const blink::WebCryptoKey& key,
               const blink::WebCryptoKey& wrapping_key,
               const blink::WebCryptoAlgorithm& wrap_algorithm,
               const blink::WebCryptoResult& result,
               scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : BaseState(result, std::move(task_runner)),
        format(format),
        key(key),
        wrapping_key(wrapping_key),
        wrap_algorithm(wrap_algorithm) {}

  const blink::WebCryptoKeyFormat format;
  const blink::WebCryptoKey key;
  const blink::WebCryptoKey wrapping_key;
  const blink::WebCryptoAlgorithm wrap_algorithm;

  std::vector<uint8_t> buffer;
};

struct UnwrapKeyState : public BaseState {
  UnwrapKeyState(blink::WebCryptoKeyFormat format,
                 blink::WebVector<unsigned char> wrapped_key,
                 const blink::WebCryptoKey& wrapping_key,
                 const blink::WebCryptoAlgorithm& unwrap_algorithm,
                 const blink::WebCryptoAlgorithm& unwrapped_key_algorithm,
                 bool extractable,
                 blink::WebCryptoKeyUsageMask usages,
                 const blink::WebCryptoResult& result,
                 scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : BaseState(result, std::move(task_runner)),
        format(format),
        wrapped_key(std::move(wrapped_key)),
        wrapping_key(wrapping_key),
        unwrap_algorithm(unwrap_algorithm),
        unwrapped_key_algorithm(unwrapped_key_algorithm),
        extractable(extractable),
        usages(usages) {}

  const blink::WebCryptoKeyFormat format;
  blink::WebVector<unsigned char> wrapped_key;
  const blink::WebCryptoKey wrapping_key;
  const blink::WebCryptoAlgorithm unwrap_algorithm;
  const blink::WebCryptoAlgorithm unwrapped_key_algorithm;
  const bool extractable;
  const blink::WebCryptoKeyUsageMask usages;

  blink::WebCryptoKey unwrapped_key;
};

struct DeriveBitsState : public BaseState {
  DeriveBitsState(const blink::WebCryptoAlgorithm& algorithm,
                  const blink::WebCryptoKey& base_key,
                  unsigned int length_bits,
                  const blink::WebCryptoResult& result,
                  scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : BaseState(result, std::move(task_runner)),
        algorithm(algorithm),
        base_key(base_key),
        length_bits(length_bits) {}

  const blink::WebCryptoAlgorithm algorithm;
  const blink::WebCryptoKey base_key;
  const unsigned int length_bits;

  std::vector<uint8_t> derived_bytes;
};

struct DeriveKeyState : public BaseState {
  DeriveKeyState(const blink::WebCryptoAlgorithm& algorithm,
                 const blink::WebCryptoKey& base_key,
                 const blink::WebCryptoAlgorithm& import_algorithm,
                 const blink::WebCryptoAlgorithm& key_length_algorithm,
                 bool extractable,
                 blink::WebCryptoKeyUsageMask usages,
                 const blink::WebCryptoResult& result,
                 scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : BaseState(result, std::move(task_runner)),
        algorithm(algorithm),
        base_key(base_key),
        import_algorithm(import_algorithm),
        key_length_algorithm(key_length_algorithm),
        extractable(extractable),
        usages(usages) {}

  const blink::WebCryptoAlgorithm algorithm;
  const blink::WebCryptoKey base_key;
  const blink::WebCryptoAlgorithm import_algorithm;
  const blink::WebCryptoAlgorithm key_length_algorithm;
  bool extractable;
  blink::WebCryptoKeyUsageMask usages;

  blink::WebCryptoKey derived_key;
};

// -----------------------------------------------------------------------------------------

struct DigestTestState : public BaseState {
  DigestTestState(const blink::WebCryptoAlgorithm& algorithm,
               const blink::WebCryptoKey& key,
               blink::WebVector<unsigned char> data,
               const blink::WebCryptoResult& result,
               scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : BaseState(result, std::move(task_runner)),
        algorithm(algorithm),
        key(key),
        data(std::move(data)) {}

  const blink::WebCryptoAlgorithm algorithm;
  const blink::WebCryptoKey key;
  const blink::WebVector<unsigned char> data;

  std::vector<uint8_t> buffer;
};

struct TPMInitState : public BaseState {
  TPMInitState(const blink::WebCryptoResult& result,
                 scoped_refptr<base::SingleThreadTaskRunner> task_runner)
        : BaseState(result, std::move(task_runner)) {}
};

struct TPMGetRandomState : public BaseState {
  TPMGetRandomState(const blink::WebCryptoResult& result,
                 scoped_refptr<base::SingleThreadTaskRunner> task_runner)
        : BaseState(result, std::move(task_runner)) {}

        std::vector<uint8_t> buffer;
};

struct TPMCreatePrimaryState : public BaseState {
  TPMCreatePrimaryState(const blink::WebCryptoResult& result,
                 scoped_refptr<base::SingleThreadTaskRunner> task_runner)
        : BaseState(result, std::move(task_runner)) {}
};

struct TPMCreateState : public BaseState {
  TPMCreateState(const blink::WebCryptoResult& result,
                 scoped_refptr<base::SingleThreadTaskRunner> task_runner)
        : BaseState(result, std::move(task_runner)) {}
};

struct TPMEncryptState : public BaseState {
  TPMEncryptState(const blink::WebCryptoResult& result,
                 scoped_refptr<base::SingleThreadTaskRunner> task_runner)
        : BaseState(result, std::move(task_runner)) {}
};

struct TPMDecryptState : public BaseState {
  TPMDecryptState(const blink::WebCryptoResult& result,
                 scoped_refptr<base::SingleThreadTaskRunner> task_runner)
        : BaseState(result, std::move(task_runner)) {}
};

struct TPMSignState : public BaseState {
  TPMSignState(const blink::WebCryptoResult& result,
                 scoped_refptr<base::SingleThreadTaskRunner> task_runner)
        : BaseState(result, std::move(task_runner)) {}
};

struct TPMVerifyState : public BaseState {
  TPMVerifyState(const blink::WebCryptoResult& result,
                 scoped_refptr<base::SingleThreadTaskRunner> task_runner)
        : BaseState(result, std::move(task_runner)) {}
};

struct TPMFlushContextState : public BaseState {
  TPMFlushContextState(const blink::WebCryptoResult& result,
                 scoped_refptr<base::SingleThreadTaskRunner> task_runner)
        : BaseState(result, std::move(task_runner)) {}
};

// --------------------------------------------------------------------
// Wrapper functions
// --------------------------------------------------------------------
//
// * The methods named Do*() run on the crypto thread.
// * The methods named Do*Reply() run on the target Blink thread

void DoEncryptReply(std::unique_ptr<EncryptState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoEncryptReply");
  CompleteWithBufferOrError(state->status, state->buffer, &state->result);
}

void DoEncrypt(std::unique_ptr<EncryptState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoEncrypt");
  EncryptState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status =
      webcrypto::Encrypt(state->algorithm, state->key,
                         webcrypto::CryptoData(state->data), &state->buffer);
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoEncryptReply, std::move(passed_state)));
}

void DoDecryptReply(std::unique_ptr<DecryptState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoDecryptReply");
  CompleteWithBufferOrError(state->status, state->buffer, &state->result);
}

void DoDecrypt(std::unique_ptr<DecryptState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoDecrypt");
  DecryptState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status =
      webcrypto::Decrypt(state->algorithm, state->key,
                         webcrypto::CryptoData(state->data), &state->buffer);
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoDecryptReply, std::move(passed_state)));
}

void DoDigestReply(std::unique_ptr<DigestState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoDigestReply");
  CompleteWithBufferOrError(state->status, state->buffer, &state->result);
}

void DoDigest(std::unique_ptr<DigestState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoDigest");
  DigestState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status = webcrypto::Digest(
      state->algorithm, webcrypto::CryptoData(state->data), &state->buffer);
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoDigestReply, std::move(passed_state)));
}

void DoGenerateKeyReply(std::unique_ptr<GenerateKeyState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoGenerateKeyReply");
  if (state->status.IsError()) {
    CompleteWithError(state->status, &state->result);
  } else {
    state->generate_key_result.Complete(&state->result);
  }
}

void DoGenerateKey(std::unique_ptr<GenerateKeyState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoGenerateKey");
  GenerateKeyState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status =
      webcrypto::GenerateKey(state->algorithm, state->extractable,
                             state->usages, &state->generate_key_result);
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoGenerateKeyReply, std::move(passed_state)));
}

void DoImportKeyReply(std::unique_ptr<ImportKeyState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoImportKeyReply");
  CompleteWithKeyOrError(state->status, state->key, &state->result);
}

void DoImportKey(std::unique_ptr<ImportKeyState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoImportKey");
  ImportKeyState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status = webcrypto::ImportKey(
      state->format, webcrypto::CryptoData(state->key_data), state->algorithm,
      state->extractable, state->usages, &state->key);
  if (state->status.IsSuccess()) {
    DCHECK(state->key.Handle());
    DCHECK(!state->key.Algorithm().IsNull());
    DCHECK_EQ(state->extractable, state->key.Extractable());
  }

  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoImportKeyReply, std::move(passed_state)));
}

void DoExportKeyReply(std::unique_ptr<ExportKeyState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoExportKeyReply");
  if (state->format != blink::kWebCryptoKeyFormatJwk) {
    CompleteWithBufferOrError(state->status, state->buffer, &state->result);
    return;
  }

  if (state->status.IsError()) {
    CompleteWithError(state->status, &state->result);
  } else {
    state->result.CompleteWithJson(
        reinterpret_cast<const char*>(state->buffer.data()),
        static_cast<unsigned int>(state->buffer.size()));
  }
}

void DoExportKey(std::unique_ptr<ExportKeyState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoExportKey");
  ExportKeyState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status =
      webcrypto::ExportKey(state->format, state->key, &state->buffer);
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoExportKeyReply, std::move(passed_state)));
}

void DoSignReply(std::unique_ptr<SignState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoSignReply");
  CompleteWithBufferOrError(state->status, state->buffer, &state->result);
}

void DoSign(std::unique_ptr<SignState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoSign");
  SignState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status =
      webcrypto::Sign(state->algorithm, state->key,
                      webcrypto::CryptoData(state->data), &state->buffer);

  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoSignReply, std::move(passed_state)));
}

void DoVerifyReply(std::unique_ptr<VerifySignatureState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoVerifyReply");
  if (state->status.IsError()) {
    CompleteWithError(state->status, &state->result);
  } else {
    state->result.CompleteWithBoolean(state->verify_result);
  }
}

void DoVerify(std::unique_ptr<VerifySignatureState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoVerify");
  VerifySignatureState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status = webcrypto::Verify(
      state->algorithm, state->key, webcrypto::CryptoData(state->signature),
      webcrypto::CryptoData(state->data), &state->verify_result);

  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoVerifyReply, std::move(passed_state)));
}

void DoWrapKeyReply(std::unique_ptr<WrapKeyState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoWrapKeyReply");
  CompleteWithBufferOrError(state->status, state->buffer, &state->result);
}

void DoWrapKey(std::unique_ptr<WrapKeyState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoWrapKey");
  WrapKeyState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status =
      webcrypto::WrapKey(state->format, state->key, state->wrapping_key,
                         state->wrap_algorithm, &state->buffer);

  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoWrapKeyReply, std::move(passed_state)));
}

void DoUnwrapKeyReply(std::unique_ptr<UnwrapKeyState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoUnwrapKeyReply");
  CompleteWithKeyOrError(state->status, state->unwrapped_key, &state->result);
}

void DoUnwrapKey(std::unique_ptr<UnwrapKeyState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoUnwrapKey");
  UnwrapKeyState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status = webcrypto::UnwrapKey(
      state->format, webcrypto::CryptoData(state->wrapped_key),
      state->wrapping_key, state->unwrap_algorithm,
      state->unwrapped_key_algorithm, state->extractable, state->usages,
      &state->unwrapped_key);

  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoUnwrapKeyReply, std::move(passed_state)));
}

void DoDeriveBitsReply(std::unique_ptr<DeriveBitsState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoDeriveBitsReply");
  CompleteWithBufferOrError(state->status, state->derived_bytes,
                            &state->result);
}

void DoDeriveBits(std::unique_ptr<DeriveBitsState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoDeriveBits");
  DeriveBitsState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status =
      webcrypto::DeriveBits(state->algorithm, state->base_key,
                            state->length_bits, &state->derived_bytes);
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoDeriveBitsReply, std::move(passed_state)));
}

void DoDeriveKeyReply(std::unique_ptr<DeriveKeyState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoDeriveKeyReply");
  CompleteWithKeyOrError(state->status, state->derived_key, &state->result);
}

void DoDeriveKey(std::unique_ptr<DeriveKeyState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoDeriveKey");
  DeriveKeyState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status = webcrypto::DeriveKey(
      state->algorithm, state->base_key, state->import_algorithm,
      state->key_length_algorithm, state->extractable, state->usages,
      &state->derived_key);
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoDeriveKeyReply, std::move(passed_state)));
}

// ----------------------------------------------------------------------------------------------

void DoDigestTestReply(std::unique_ptr<DigestTestState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoDigestTestReply");
  CompleteWithBufferOrError(state->status, state->buffer, &state->result);
}

void DoDigestTest(std::unique_ptr<DigestTestState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoDigestTest");
  DigestTestState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status = webcrypto::DigestTest(
      state->algorithm, webcrypto::CryptoData(state->data), &state->buffer);
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoDigestTestReply, std::move(passed_state)));
}

void DoTPMInitReply(std::unique_ptr<TPMInitState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoTPMInitReply");
  //CompleteWithBufferOrError(state->status, &state->result);
}

void DoTPMInit(std::unique_ptr<TPMInitState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoTPMInit");
  TPMInitState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status =
      webcrypto::TPMInit();
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoTPMInitReply, std::move(passed_state)));
}

void DoTPMGetRandomReply(std::unique_ptr<TPMGetRandomState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoTPMGetRandomReply");
  CompleteWithBufferOrError(state->status, state->buffer, &state->result);
}

void DoTPMGetRandom(std::unique_ptr<TPMGetRandomState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoTPMGetRandom");
  TPMGetRandomState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status =
      webcrypto::TPMGetRandom(&state->buffer);
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoTPMGetRandomReply, std::move(passed_state)));
}

void DoTPMCreatePrimaryReply(std::unique_ptr<TPMCreatePrimaryState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoTPMCreatePrimaryReply");
}

void DoTPMCreatePrimary(std::unique_ptr<TPMCreatePrimaryState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoTPMCreatePrimary");
  TPMCreatePrimaryState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status =
      webcrypto::TPMCreatePrimary();
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoTPMCreatePrimaryReply, std::move(passed_state)));
}

void DoTPMCreateReply(std::unique_ptr<TPMCreateState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoTPMCreateReply");
}

void DoTPMCreate(std::unique_ptr<TPMCreateState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoTPMCreate");
  TPMCreateState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status =
      webcrypto::TPMCreate();
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoTPMCreateReply, std::move(passed_state)));
}

void DoTPMEncryptReply(std::unique_ptr<TPMEncryptState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoTPMEncryptReply");
}

void DoTPMEncrypt(std::unique_ptr<TPMEncryptState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoTPMEncrypt");
  TPMEncryptState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status =
      webcrypto::TPMEncrypt();
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoTPMEncryptReply, std::move(passed_state)));
}

void DoTPMDecryptReply(std::unique_ptr<TPMDecryptState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoTPMDecryptReply");
}

void DoTPMDecrypt(std::unique_ptr<TPMDecryptState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoTPMDecrypt");
  TPMDecryptState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status =
      webcrypto::TPMDecrypt();
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoTPMDecryptReply, std::move(passed_state)));
}

void DoTPMSignReply(std::unique_ptr<TPMSignState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoTPMSignReply");
}

void DoTPMSign(std::unique_ptr<TPMSignState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoTPMSign");
  TPMSignState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status =
      webcrypto::TPMSign();
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoTPMSignReply, std::move(passed_state)));
}

void DoTPMVerifyReply(std::unique_ptr<TPMVerifyState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoTPMVerifyReply");
}

void DoTPMVerify(std::unique_ptr<TPMVerifyState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoTPMVerify");
  TPMVerifyState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status =
      webcrypto::TPMVerify();
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoTPMVerifyReply, std::move(passed_state)));
}

void DoTPMFlushContextReply(std::unique_ptr<TPMFlushContextState> state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"),
               "DoTPMFlushContextReply");
}

void DoTPMFlushContext(std::unique_ptr<TPMFlushContextState> passed_state) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "DoTPMFlushContext");
  TPMFlushContextState* state = passed_state.get();
  if (state->cancelled())
    return;
  state->status =
      webcrypto::TPMFlushContext();
  state->origin_thread->PostTask(
      FROM_HERE, base::BindOnce(DoTPMFlushContextReply, std::move(passed_state)));
}

}  // namespace

WebCryptoImpl::WebCryptoImpl() {
}

WebCryptoImpl::~WebCryptoImpl() {
}

void WebCryptoImpl::Encrypt(
    const blink::WebCryptoAlgorithm& algorithm,
    const blink::WebCryptoKey& key,
    blink::WebVector<unsigned char> data,
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  DCHECK(!algorithm.IsNull());

  std::unique_ptr<EncryptState> state(new EncryptState(
      algorithm, key, std::move(data), result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoEncrypt, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::Decrypt(
    const blink::WebCryptoAlgorithm& algorithm,
    const blink::WebCryptoKey& key,
    blink::WebVector<unsigned char> data,
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  DCHECK(!algorithm.IsNull());
  
  std::unique_ptr<DecryptState> state(new DecryptState(
      algorithm, key, std::move(data), result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoDecrypt, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::Digest(
    const blink::WebCryptoAlgorithm& algorithm,
    blink::WebVector<unsigned char> data,
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  DCHECK(!algorithm.IsNull());

  std::unique_ptr<DigestState> state(
      new DigestState(algorithm, blink::WebCryptoKey::CreateNull(),
                      std::move(data), result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(FROM_HERE,
                                  base::BindOnce(DoDigest, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::GenerateKey(
    const blink::WebCryptoAlgorithm& algorithm,
    bool extractable,
    blink::WebCryptoKeyUsageMask usages,
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  DCHECK(!algorithm.IsNull());

  std::unique_ptr<GenerateKeyState> state(new GenerateKeyState(
      algorithm, extractable, usages, result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoGenerateKey, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::ImportKey(
    blink::WebCryptoKeyFormat format,
    blink::WebVector<unsigned char> key_data,
    const blink::WebCryptoAlgorithm& algorithm,
    bool extractable,
    blink::WebCryptoKeyUsageMask usages,
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  std::unique_ptr<ImportKeyState> state(
      new ImportKeyState(format, std::move(key_data), algorithm, extractable,
                         usages, result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoImportKey, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::ExportKey(
    blink::WebCryptoKeyFormat format,
    const blink::WebCryptoKey& key,
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  std::unique_ptr<ExportKeyState> state(
      new ExportKeyState(format, key, result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoExportKey, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::Sign(
    const blink::WebCryptoAlgorithm& algorithm,
    const blink::WebCryptoKey& key,
    blink::WebVector<unsigned char> data,
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  std::unique_ptr<SignState> state(new SignState(
      algorithm, key, std::move(data), result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(FROM_HERE,
                                  base::BindOnce(DoSign, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::VerifySignature(
    const blink::WebCryptoAlgorithm& algorithm,
    const blink::WebCryptoKey& key,
    blink::WebVector<unsigned char> signature,
    blink::WebVector<unsigned char> data,
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  std::unique_ptr<VerifySignatureState> state(new VerifySignatureState(
      algorithm, key, std::move(signature), std::move(data), result,
      std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(FROM_HERE,
                                  base::BindOnce(DoVerify, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::WrapKey(
    blink::WebCryptoKeyFormat format,
    const blink::WebCryptoKey& key,
    const blink::WebCryptoKey& wrapping_key,
    const blink::WebCryptoAlgorithm& wrap_algorithm,
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  std::unique_ptr<WrapKeyState> state(
      new WrapKeyState(format, key, wrapping_key, wrap_algorithm, result,
                       std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoWrapKey, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::UnwrapKey(
    blink::WebCryptoKeyFormat format,
    blink::WebVector<unsigned char> wrapped_key,
    const blink::WebCryptoKey& wrapping_key,
    const blink::WebCryptoAlgorithm& unwrap_algorithm,
    const blink::WebCryptoAlgorithm& unwrapped_key_algorithm,
    bool extractable,
    blink::WebCryptoKeyUsageMask usages,
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  std::unique_ptr<UnwrapKeyState> state(
      new UnwrapKeyState(format, std::move(wrapped_key), wrapping_key,
                         unwrap_algorithm, unwrapped_key_algorithm, extractable,
                         usages, result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoUnwrapKey, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::DeriveBits(
    const blink::WebCryptoAlgorithm& algorithm,
    const blink::WebCryptoKey& base_key,
    unsigned int length_bits,
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  std::unique_ptr<DeriveBitsState> state(new DeriveBitsState(
      algorithm, base_key, length_bits, result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoDeriveBits, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::DeriveKey(
    const blink::WebCryptoAlgorithm& algorithm,
    const blink::WebCryptoKey& base_key,
    const blink::WebCryptoAlgorithm& import_algorithm,
    const blink::WebCryptoAlgorithm& key_length_algorithm,
    bool extractable,
    blink::WebCryptoKeyUsageMask usages,
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  std::unique_ptr<DeriveKeyState> state(new DeriveKeyState(
      algorithm, base_key, import_algorithm, key_length_algorithm, extractable,
      usages, result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoDeriveKey, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

bool WebCryptoImpl::DeserializeKeyForClone(
    const blink::WebCryptoKeyAlgorithm& algorithm,
    blink::WebCryptoKeyType type,
    bool extractable,
    blink::WebCryptoKeyUsageMask usages,
    const unsigned char* key_data,
    unsigned key_data_size,
    blink::WebCryptoKey& key) {
  return webcrypto::DeserializeKeyForClone(
      algorithm, type, extractable, usages,
      webcrypto::CryptoData(key_data, key_data_size), &key);
}

bool WebCryptoImpl::SerializeKeyForClone(
    const blink::WebCryptoKey& key,
    blink::WebVector<unsigned char>& key_data) {
  return webcrypto::SerializeKeyForClone(key, &key_data);
}

// --------------------------------------------------------------------------------------------------------

void WebCryptoImpl::DigestTest(
    const blink::WebCryptoAlgorithm& algorithm,
    blink::WebVector<unsigned char> data,
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  DCHECK(!algorithm.IsNull());

  std::unique_ptr<DigestTestState> state(
      new DigestTestState(algorithm, blink::WebCryptoKey::CreateNull(),
                      std::move(data), result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(FROM_HERE,
                                  base::BindOnce(DoDigestTest, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::tpm(
  const blink::WebCryptoAlgorithm& algorithm,
  bool extractable,
  blink::WebCryptoKeyUsageMask usages,
  blink::WebCryptoResult result,
  scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  DCHECK(!algorithm.IsNull());

  std::unique_ptr<GenerateKeyState> state(new GenerateKeyState(
      algorithm, extractable, usages, result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoGenerateKey, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::TPMInit(
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  std::unique_ptr<TPMInitState> state(
      new TPMInitState(result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoTPMInit, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::TPMGetRandom(
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
    printf("web_crypto_impl.cc - TpmGetRandom");
  std::unique_ptr<TPMGetRandomState> state(
      new TPMGetRandomState(result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoTPMGetRandom, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::TPMCreatePrimary(
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  std::unique_ptr<TPMCreatePrimaryState> state(
      new TPMCreatePrimaryState(result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoTPMCreatePrimary, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::TPMCreate(
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  std::unique_ptr<TPMCreateState> state(
      new TPMCreateState(result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoTPMCreate, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::TPMEncrypt(
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  std::unique_ptr<TPMEncryptState> state(
      new TPMEncryptState(result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoTPMEncrypt, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::TPMDecrypt(
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  std::unique_ptr<TPMDecryptState> state(
      new TPMDecryptState(result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoTPMDecrypt, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::TPMSign(
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  std::unique_ptr<TPMSignState> state(
      new TPMSignState(result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoTPMSign, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::TPMVerify(
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  std::unique_ptr<TPMVerifyState> state(
      new TPMVerifyState(result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoTPMVerify, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::TPMFlushContext(
    blink::WebCryptoResult result,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  std::unique_ptr<TPMFlushContextState> state(
      new TPMFlushContextState(result, std::move(task_runner)));
  if (!CryptoThreadPool::PostTask(
          FROM_HERE, base::BindOnce(DoTPMFlushContext, std::move(state)))) {
    CompleteWithThreadPoolError(&result);
  }
}

void WebCryptoImpl::tpmInitCreateSignVerify() {}

}  // namespace webcrypto
