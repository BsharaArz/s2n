/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

/* Target Functions: s2n_kem_recv_ciphertext s2n_kem_decapsulate SIKE_P434_r2_crypto_kem_dec */

#include "tests/s2n_test.h"
#include "tests/testlib/s2n_testlib.h"
#include "tls/s2n_kem.h"
#include "utils/s2n_safety.h"
#include "pq-crypto/s2n_pq.h"

#define KAT_FILE_NAME "../unit/kats/sike_r2.kat"

/* This fuzz test uses the first private key (count = 0) from tests/unit/kats/sike_r2.kat.
 * A valid ciphertext to provide to s2n_kem_recv_ciphertext (as it would have appeared on
 * the wire) was generated by taking the corresponding KAT ciphertext (count = 0) and
 * prepending SIKE_P434_R2_CIPHERTEXT_BYTES as two hex-encoded bytes. */
static struct s2n_kem_params kem_params = { .kem = &s2n_sike_p434_r2 };

int s2n_fuzz_init(int *argc, char **argv[]) {
    GUARD(s2n_kem_recv_ciphertext_fuzz_test_init(KAT_FILE_NAME, &kem_params));
    return S2N_SUCCESS;
}

int s2n_fuzz_test(const uint8_t *buf, size_t len) {
    /* Test the portable C code */
    GUARD_AS_POSIX(s2n_disable_sikep434r2_asm());
    GUARD(s2n_kem_recv_ciphertext_fuzz_test(buf, len, &kem_params));

    /* Test the assembly, if available; if not, don't bother testing the C again */
    GUARD_AS_POSIX(s2n_try_enable_sikep434r2_asm());
    if (s2n_sikep434r2_asm_is_enabled()) {
        GUARD(s2n_kem_recv_ciphertext_fuzz_test(buf, len, &kem_params));
    }

    return S2N_SUCCESS;
}

S2N_FUZZ_TARGET(s2n_fuzz_init, s2n_fuzz_test, NULL)
