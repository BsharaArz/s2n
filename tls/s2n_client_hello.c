/*
 * Copyright 2014 Amazon.com, Inc. or its affiliates. All Rights Reserved.
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

#include <time.h>
#include <stdint.h>

#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_alerts.h"
#include "tls/s2n_tls.h"

#include "stuffer/s2n_stuffer.h"

#include "utils/s2n_random.h"
#include "utils/s2n_safety.h"

/* From RFC5246 A.4 */
#define S2N_TLS_CLIENT_HELLO    1

/* Per http://www-archive.mozilla.org/projects/security/pki/nss/ssl/draft02.html C.2 
 */
#define S2N_SSL_CLIENT_HELLO    1

int s2n_client_hello_recv(struct s2n_connection *conn, const char **err)
{
    struct s2n_stuffer *in = &conn->handshake.io;
    uint8_t session_id[S2N_TLS_SESSION_ID_LEN];
    uint8_t session_id_len;
    uint8_t compression_methods;
    uint16_t extensions_size;
    uint16_t cipher_suites_length;
    uint8_t *cipher_suites;
    uint8_t client_protocol_version[S2N_TLS_PROTOCOL_VERSION_LEN];

    GUARD(s2n_stuffer_read_bytes(in, client_protocol_version, S2N_TLS_PROTOCOL_VERSION_LEN, err));
    GUARD(s2n_stuffer_read_bytes(in, conn->pending.client_random, S2N_TLS_RANDOM_DATA_LEN, err));
    GUARD(s2n_stuffer_read_uint8(in, &session_id_len, err));

    conn->client_protocol_version = (client_protocol_version[0] * 10) + client_protocol_version[1];
    if (conn->client_protocol_version < S2N_SSLv3 || conn->client_protocol_version > S2N_TLS12) {
        GUARD(s2n_queue_reader_unsupported_protocol_version_alert(conn, err));
        *err = "Unsupported SSL/TLS version";
        return -1;
    }

    if (session_id_len > 32) {
        *err = "Supplied session id is too long";
        return -1;
    }

    GUARD(s2n_stuffer_read_bytes(in, session_id, session_id_len, err));
    GUARD(s2n_stuffer_read_uint16(in, &cipher_suites_length, err));
    cipher_suites = s2n_stuffer_raw_read(in, cipher_suites_length, err);
    notnull_check(cipher_suites);

    if (cipher_suites_length % S2N_TLS_CIPHER_SUITE_LEN) {
        *err = "Invalid cipher suites length";
        return -1;
    }

    GUARD(s2n_set_cipher_as_tls_server(conn, cipher_suites, cipher_suites_length / 2, err));

    GUARD(s2n_stuffer_read_uint8(in, &compression_methods, err));
    GUARD(s2n_stuffer_skip_read(in, compression_methods, err));

    conn->server->chosen_cert_chain = conn->config->cert_and_key_pairs;
    conn->handshake.next_state = SERVER_HELLO;

    if (s2n_stuffer_data_available(in) < 2) {
        /* No extensions */
        return 0;
    }

    GUARD(s2n_stuffer_read_uint16(in, &extensions_size, err));

    if (extensions_size > s2n_stuffer_data_available(in)) {
        *err = "Extension data length exceeds fragment length";
        return -1;
    }

    struct s2n_blob extensions;
    extensions.size = extensions_size;
    extensions.data = s2n_stuffer_raw_read(in, extensions.size, err);

    GUARD(s2n_client_extensions_recv(conn, &extensions, err));

    return 0;
}

int s2n_client_hello_send(struct s2n_connection *conn, const char **err)
{
    uint32_t gmt_unix_time = time(NULL);
    struct s2n_stuffer *out = &conn->handshake.io;
    struct s2n_stuffer client_random;
    struct s2n_blob b;
    uint8_t session_id_len = 0;
    uint8_t client_protocol_version[S2N_TLS_PROTOCOL_VERSION_LEN];

    b.data = conn->pending.client_random;
    b.size = S2N_TLS_RANDOM_DATA_LEN;

    /* Create the client random data */
    GUARD(s2n_stuffer_init(&client_random, &b, err));
    GUARD(s2n_stuffer_write_uint32(&client_random, gmt_unix_time, err));
    GUARD(s2n_stuffer_write_random_data(&client_random, 28, err));

    client_protocol_version[0] = conn->client_protocol_version / 10;
    client_protocol_version[1] = conn->client_protocol_version % 10;

    GUARD(s2n_stuffer_write_bytes(out, client_protocol_version, S2N_TLS_PROTOCOL_VERSION_LEN, err));
    GUARD(s2n_stuffer_copy(&client_random, out, S2N_TLS_RANDOM_DATA_LEN, err));
    GUARD(s2n_stuffer_write_uint8(out, session_id_len, err));
    GUARD(s2n_stuffer_write_uint16(out, s2n_cipher_preferences_default->count * 2, err));
    GUARD(s2n_stuffer_write_bytes(out, s2n_cipher_preferences_default->wire_format, s2n_cipher_preferences_default->count * 2, err));

    /* Zero compression methods */
    GUARD(s2n_stuffer_write_uint8(out, 1, err));
    GUARD(s2n_stuffer_write_uint8(out, 0, err));

    /* Write the extensions */
    GUARD(s2n_client_extensions_send(conn, out, err));

    conn->handshake.next_state = SERVER_HELLO;

    return 0;
}

/* See http://www-archive.mozilla.org/projects/security/pki/nss/ssl/draft02.html 2.5 */
int s2n_sslv2_client_hello_recv(struct s2n_connection *conn, const char **err)
{
    struct s2n_stuffer *in = &conn->handshake.io;
    uint16_t session_id_length;
    uint16_t cipher_suites_length;
    uint16_t challenge_length;
    uint8_t *cipher_suites;

    if (conn->client_protocol_version < S2N_SSLv3 || conn->client_protocol_version > S2N_TLS12) {
        GUARD(s2n_queue_reader_unsupported_protocol_version_alert(conn, err));
        *err = "Unsupported SSL/TLS version";
        return -1;
    }

    /* We start 5 bytes into the record */
    GUARD(s2n_stuffer_read_uint16(in, &cipher_suites_length, err));

    if (cipher_suites_length % S2N_SSLv2_CIPHER_SUITE_LEN) {
        *err = "Invalid cipher suites length";
        return -1;
    }

    GUARD(s2n_stuffer_read_uint16(in, &session_id_length, err));

    if (session_id_length) {
        *err = "Invalid SSLv2 session id length";
        return -1;
    }

    GUARD(s2n_stuffer_read_uint16(in, &challenge_length, err));

    if (challenge_length > S2N_TLS_RANDOM_DATA_LEN) {
        *err = "Invalid SSLv2 challenge length";
        return -1;
    }

    cipher_suites = s2n_stuffer_raw_read(in, cipher_suites_length, err);
    notnull_check(cipher_suites);
    GUARD(s2n_set_cipher_as_sslv2_server(conn, cipher_suites, cipher_suites_length / S2N_SSLv2_CIPHER_SUITE_LEN, err));

    struct s2n_blob b;
    b.data = conn->pending.client_random;
    b.size = S2N_TLS_RANDOM_DATA_LEN;

    b.data += S2N_TLS_RANDOM_DATA_LEN - challenge_length;
    b.size -= S2N_TLS_RANDOM_DATA_LEN - challenge_length;

    GUARD(s2n_stuffer_read(in, &b, err));

    conn->server->chosen_cert_chain = conn->config->cert_and_key_pairs;
    conn->handshake.was_client_hello_sslv2 = 1;
    conn->handshake.next_state = SERVER_HELLO;

    return 0;
}