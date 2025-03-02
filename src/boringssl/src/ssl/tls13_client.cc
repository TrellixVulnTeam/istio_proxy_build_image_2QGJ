/* Copyright (c) 2016, Google Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include <openssl/ssl.h>

#include <assert.h>
#include <limits.h>
#include <string.h>

#include <utility>

#include <openssl/bytestring.h>
#include <openssl/digest.h>
#include <openssl/err.h>
#include <openssl/mem.h>
#include <openssl/stack.h>

#include "../crypto/internal.h"
#include "internal.h"


namespace bssl {

enum client_hs_state_t {
  state_read_hello_retry_request = 0,
  state_send_second_client_hello,
  state_read_server_hello,
  state_process_change_cipher_spec,
  state_read_encrypted_extensions,
  state_read_certificate_request,
  state_read_server_certificate,
  state_read_server_certificate_verify,
  state_read_server_finished,
  state_send_end_of_early_data,
  state_send_client_certificate,
  state_send_client_certificate_verify,
  state_complete_second_flight,
  state_done,
};

static const uint8_t kZeroes[EVP_MAX_MD_SIZE] = {0};

static enum ssl_hs_wait_t do_read_hello_retry_request(SSL_HANDSHAKE *hs) {
  SSL *const ssl = hs->ssl;
  SSLMessage msg;
  if (!ssl->method->get_message(ssl, &msg)) {
    return ssl_hs_read_message;
  }
  if (msg.type != SSL3_MT_HELLO_RETRY_REQUEST) {
    hs->tls13_state = state_read_server_hello;
    return ssl_hs_ok;
  }

  CBS body = msg.body, extensions;
  uint16_t server_version;
  if (!CBS_get_u16(&body, &server_version) ||
      !CBS_get_u16_length_prefixed(&body, &extensions) ||
      /* HelloRetryRequest may not be empty. */
      CBS_len(&extensions) == 0 ||
      CBS_len(&body) != 0) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_DECODE_ERROR);
    ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_DECODE_ERROR);
    return ssl_hs_error;
  }

  int have_cookie, have_key_share;
  CBS cookie, key_share;
  const SSL_EXTENSION_TYPE ext_types[] = {
      {TLSEXT_TYPE_key_share, &have_key_share, &key_share},
      {TLSEXT_TYPE_cookie, &have_cookie, &cookie},
  };

  uint8_t alert = SSL_AD_DECODE_ERROR;
  if (!ssl_parse_extensions(&extensions, &alert, ext_types,
                            OPENSSL_ARRAY_SIZE(ext_types),
                            0 /* reject unknown */)) {
    ssl3_send_alert(ssl, SSL3_AL_FATAL, alert);
    return ssl_hs_error;
  }

  if (have_cookie) {
    CBS cookie_value;
    if (!CBS_get_u16_length_prefixed(&cookie, &cookie_value) ||
        CBS_len(&cookie_value) == 0 ||
        CBS_len(&cookie) != 0) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_DECODE_ERROR);
      ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_DECODE_ERROR);
      return ssl_hs_error;
    }

    if (!CBS_stow(&cookie_value, &hs->cookie, &hs->cookie_len)) {
      return ssl_hs_error;
    }
  }

  if (have_key_share) {
    uint16_t group_id;
    if (!CBS_get_u16(&key_share, &group_id) || CBS_len(&key_share) != 0) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_DECODE_ERROR);
      ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_DECODE_ERROR);
      return ssl_hs_error;
    }

    /* The group must be supported. */
    const uint16_t *groups;
    size_t groups_len;
    tls1_get_grouplist(ssl, &groups, &groups_len);
    int found = 0;
    for (size_t i = 0; i < groups_len; i++) {
      if (groups[i] == group_id) {
        found = 1;
        break;
      }
    }

    if (!found) {
      ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_ILLEGAL_PARAMETER);
      OPENSSL_PUT_ERROR(SSL, SSL_R_WRONG_CURVE);
      return ssl_hs_error;
    }

    /* Check that the HelloRetryRequest does not request the key share that
     * was provided in the initial ClientHello. */
    if (hs->key_share->GroupID() == group_id) {
      ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_ILLEGAL_PARAMETER);
      OPENSSL_PUT_ERROR(SSL, SSL_R_WRONG_CURVE);
      return ssl_hs_error;
    }

    hs->key_share.reset();
    hs->retry_group = group_id;
  }

  if (!ssl_hash_message(hs, msg)) {
    return ssl_hs_error;
  }

  ssl->method->next_message(ssl);
  hs->received_hello_retry_request = 1;
  hs->tls13_state = state_send_second_client_hello;
  /* 0-RTT is rejected if we receive a HelloRetryRequest. */
  if (hs->in_early_data) {
    return ssl_hs_early_data_rejected;
  }
  return ssl_hs_ok;
}

static enum ssl_hs_wait_t do_send_second_client_hello(SSL_HANDSHAKE *hs) {
  SSL *const ssl = hs->ssl;
  /* Restore the null cipher. We may have switched due to 0-RTT. */
  bssl::UniquePtr<SSLAEADContext> null_ctx = SSLAEADContext::CreateNullCipher();
  if (!null_ctx ||
      !ssl->method->set_write_state(ssl, std::move(null_ctx)) ||
      !ssl_write_client_hello(hs)) {
    return ssl_hs_error;
  }

  hs->tls13_state = state_read_server_hello;
  return ssl_hs_flush;
}

static enum ssl_hs_wait_t do_read_server_hello(SSL_HANDSHAKE *hs) {
  SSL *const ssl = hs->ssl;
  SSLMessage msg;
  if (!ssl->method->get_message(ssl, &msg)) {
    return ssl_hs_read_message;
  }
  if (!ssl_check_message_type(ssl, msg, SSL3_MT_SERVER_HELLO)) {
    return ssl_hs_error;
  }

  CBS body = msg.body, server_random, session_id, extensions;
  uint16_t server_version;
  uint16_t cipher_suite;
  uint8_t compression_method;
  if (!CBS_get_u16(&body, &server_version) ||
      !CBS_get_bytes(&body, &server_random, SSL3_RANDOM_SIZE) ||
      (ssl->version == TLS1_3_EXPERIMENT_VERSION &&
       !CBS_get_u8_length_prefixed(&body, &session_id)) ||
      !CBS_get_u16(&body, &cipher_suite) ||
      (ssl->version == TLS1_3_EXPERIMENT_VERSION &&
       (!CBS_get_u8(&body, &compression_method) || compression_method != 0)) ||
      !CBS_get_u16_length_prefixed(&body, &extensions) ||
      CBS_len(&body) != 0) {
    ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_DECODE_ERROR);
    OPENSSL_PUT_ERROR(SSL, SSL_R_DECODE_ERROR);
    return ssl_hs_error;
  }

  uint16_t expected_version =
      ssl->version == TLS1_3_EXPERIMENT_VERSION ? TLS1_2_VERSION : ssl->version;
  if (server_version != expected_version) {
    ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_DECODE_ERROR);
    OPENSSL_PUT_ERROR(SSL, SSL_R_WRONG_VERSION_NUMBER);
    return ssl_hs_error;
  }

  assert(ssl->s3->have_version);
  OPENSSL_memcpy(ssl->s3->server_random, CBS_data(&server_random),
                 SSL3_RANDOM_SIZE);

  const SSL_CIPHER *cipher = SSL_get_cipher_by_value(cipher_suite);
  if (cipher == NULL) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_UNKNOWN_CIPHER_RETURNED);
    ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_ILLEGAL_PARAMETER);
    return ssl_hs_error;
  }

  /* Check if the cipher is a TLS 1.3 cipher. */
  if (SSL_CIPHER_get_min_version(cipher) > ssl3_protocol_version(ssl) ||
      SSL_CIPHER_get_max_version(cipher) < ssl3_protocol_version(ssl)) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_WRONG_CIPHER_RETURNED);
    ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_ILLEGAL_PARAMETER);
    return ssl_hs_error;
  }

  /* Parse out the extensions. */
  int have_key_share = 0, have_pre_shared_key = 0, have_supported_versions = 0;
  CBS key_share, pre_shared_key, supported_versions;
  const SSL_EXTENSION_TYPE ext_types[] = {
      {TLSEXT_TYPE_key_share, &have_key_share, &key_share},
      {TLSEXT_TYPE_pre_shared_key, &have_pre_shared_key, &pre_shared_key},
      {TLSEXT_TYPE_supported_versions, &have_supported_versions,
       &supported_versions},
  };

  uint8_t alert = SSL_AD_DECODE_ERROR;
  if (!ssl_parse_extensions(&extensions, &alert, ext_types,
                            OPENSSL_ARRAY_SIZE(ext_types),
                            0 /* reject unknown */)) {
    ssl3_send_alert(ssl, SSL3_AL_FATAL, alert);
    return ssl_hs_error;
  }

  /* supported_versions is parsed in handshake_client to select the experimental
   * TLS 1.3 version. */
  if (have_supported_versions && ssl->version != TLS1_3_EXPERIMENT_VERSION) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_UNEXPECTED_EXTENSION);
    ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_UNSUPPORTED_EXTENSION);
    return ssl_hs_error;
  }

  alert = SSL_AD_DECODE_ERROR;
  if (have_pre_shared_key) {
    if (ssl->session == NULL) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_UNEXPECTED_EXTENSION);
      ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_UNSUPPORTED_EXTENSION);
      return ssl_hs_error;
    }

    if (!ssl_ext_pre_shared_key_parse_serverhello(hs, &alert,
                                                  &pre_shared_key)) {
      ssl3_send_alert(ssl, SSL3_AL_FATAL, alert);
      return ssl_hs_error;
    }

    if (ssl->session->ssl_version != ssl->version) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_OLD_SESSION_VERSION_NOT_RETURNED);
      ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_ILLEGAL_PARAMETER);
      return ssl_hs_error;
    }

    if (ssl->session->cipher->algorithm_prf != cipher->algorithm_prf) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_OLD_SESSION_PRF_HASH_MISMATCH);
      ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_ILLEGAL_PARAMETER);
      return ssl_hs_error;
    }

    if (!ssl_session_is_context_valid(ssl, ssl->session)) {
      /* This is actually a client application bug. */
      OPENSSL_PUT_ERROR(SSL,
                        SSL_R_ATTEMPT_TO_REUSE_SESSION_IN_DIFFERENT_CONTEXT);
      ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_ILLEGAL_PARAMETER);
      return ssl_hs_error;
    }

    ssl->s3->session_reused = 1;
    /* Only authentication information carries over in TLS 1.3. */
    hs->new_session = SSL_SESSION_dup(ssl->session, SSL_SESSION_DUP_AUTH_ONLY);
    if (!hs->new_session) {
      ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_INTERNAL_ERROR);
      return ssl_hs_error;
    }
    ssl_set_session(ssl, NULL);

    /* Resumption incorporates fresh key material, so refresh the timeout. */
    ssl_session_renew_timeout(ssl, hs->new_session.get(),
                              ssl->session_ctx->session_psk_dhe_timeout);
  } else if (!ssl_get_new_session(hs, 0)) {
    ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_INTERNAL_ERROR);
    return ssl_hs_error;
  }

  hs->new_session->cipher = cipher;
  hs->new_cipher = cipher;

  /* The PRF hash is now known. Set up the key schedule. */
  if (!tls13_init_key_schedule(hs)) {
    return ssl_hs_error;
  }

  /* Incorporate the PSK into the running secret. */
  if (ssl->s3->session_reused) {
    if (!tls13_advance_key_schedule(hs, hs->new_session->master_key,
                                    hs->new_session->master_key_length)) {
      return ssl_hs_error;
    }
  } else if (!tls13_advance_key_schedule(hs, kZeroes, hs->hash_len)) {
    return ssl_hs_error;
  }

  if (!have_key_share) {
    /* We do not support psk_ke and thus always require a key share. */
    OPENSSL_PUT_ERROR(SSL, SSL_R_MISSING_KEY_SHARE);
    ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_MISSING_EXTENSION);
    return ssl_hs_error;
  }

  /* Resolve ECDHE and incorporate it into the secret. */
  uint8_t *dhe_secret;
  size_t dhe_secret_len;
  alert = SSL_AD_DECODE_ERROR;
  if (!ssl_ext_key_share_parse_serverhello(hs, &dhe_secret, &dhe_secret_len,
                                           &alert, &key_share)) {
    ssl3_send_alert(ssl, SSL3_AL_FATAL, alert);
    return ssl_hs_error;
  }

  if (!tls13_advance_key_schedule(hs, dhe_secret, dhe_secret_len)) {
    OPENSSL_free(dhe_secret);
    return ssl_hs_error;
  }
  OPENSSL_free(dhe_secret);

  if (!ssl_hash_message(hs, msg) ||
      !tls13_derive_handshake_secrets(hs)) {
    return ssl_hs_error;
  }

  ssl->method->next_message(ssl);
  hs->tls13_state = state_process_change_cipher_spec;
  return ssl->version == TLS1_3_EXPERIMENT_VERSION
             ? ssl_hs_read_change_cipher_spec
             : ssl_hs_ok;
}

static enum ssl_hs_wait_t do_process_change_cipher_spec(SSL_HANDSHAKE *hs) {
  SSL *const ssl = hs->ssl;
  if (!tls13_set_traffic_key(ssl, evp_aead_open, hs->server_handshake_secret,
                             hs->hash_len)) {
    return ssl_hs_error;
  }

  if (!hs->early_data_offered) {
    /* If not sending early data, set client traffic keys now so that alerts are
     * encrypted. */
    if ((ssl->version == TLS1_3_EXPERIMENT_VERSION &&
         !ssl3_add_change_cipher_spec(ssl)) ||
        !tls13_set_traffic_key(ssl, evp_aead_seal, hs->client_handshake_secret,
                               hs->hash_len)) {
      return ssl_hs_error;
    }
  }

  hs->tls13_state = state_read_encrypted_extensions;
  return ssl_hs_ok;
}

static enum ssl_hs_wait_t do_read_encrypted_extensions(SSL_HANDSHAKE *hs) {
  SSL *const ssl = hs->ssl;
  SSLMessage msg;
  if (!ssl->method->get_message(ssl, &msg)) {
    return ssl_hs_read_message;
  }
  if (!ssl_check_message_type(ssl, msg, SSL3_MT_ENCRYPTED_EXTENSIONS)) {
    return ssl_hs_error;
  }

  CBS body = msg.body;
  if (!ssl_parse_serverhello_tlsext(hs, &body)) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_PARSE_TLSEXT);
    return ssl_hs_error;
  }
  if (CBS_len(&body) != 0) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_DECODE_ERROR);
    ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_DECODE_ERROR);
    return ssl_hs_error;
  }

  /* Store the negotiated ALPN in the session. */
  if (ssl->s3->alpn_selected != NULL) {
    hs->new_session->early_alpn = (uint8_t *)BUF_memdup(
        ssl->s3->alpn_selected, ssl->s3->alpn_selected_len);
    if (hs->new_session->early_alpn == NULL) {
      ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_INTERNAL_ERROR);
      return ssl_hs_error;
    }
    hs->new_session->early_alpn_len = ssl->s3->alpn_selected_len;
  }

  if (ssl->early_data_accepted) {
    if (hs->early_session->cipher != hs->new_session->cipher ||
        hs->early_session->early_alpn_len != ssl->s3->alpn_selected_len ||
        OPENSSL_memcmp(hs->early_session->early_alpn, ssl->s3->alpn_selected,
                       ssl->s3->alpn_selected_len) != 0) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_ALPN_MISMATCH_ON_EARLY_DATA);
      return ssl_hs_error;
    }
    if (ssl->s3->tlsext_channel_id_valid || hs->received_custom_extension) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_UNEXPECTED_EXTENSION_ON_EARLY_DATA);
      return ssl_hs_error;
    }
  }

  if (!ssl_hash_message(hs, msg)) {
    return ssl_hs_error;
  }

  ssl->method->next_message(ssl);
  hs->tls13_state = state_read_certificate_request;
  if (hs->in_early_data && !ssl->early_data_accepted) {
    return ssl_hs_early_data_rejected;
  }
  return ssl_hs_ok;
}

static enum ssl_hs_wait_t do_read_certificate_request(SSL_HANDSHAKE *hs) {
  SSL *const ssl = hs->ssl;
  /* CertificateRequest may only be sent in non-resumption handshakes. */
  if (ssl->s3->session_reused) {
    hs->tls13_state = state_read_server_finished;
    return ssl_hs_ok;
  }

  SSLMessage msg;
  if (!ssl->method->get_message(ssl, &msg)) {
    return ssl_hs_read_message;
  }

  /* CertificateRequest is optional. */
  if (msg.type != SSL3_MT_CERTIFICATE_REQUEST) {
    hs->tls13_state = state_read_server_certificate;
    return ssl_hs_ok;
  }

  CBS body = msg.body, context, supported_signature_algorithms;
  if (!CBS_get_u8_length_prefixed(&body, &context) ||
      /* The request context is always empty during the handshake. */
      CBS_len(&context) != 0 ||
      !CBS_get_u16_length_prefixed(&body, &supported_signature_algorithms) ||
      CBS_len(&supported_signature_algorithms) == 0 ||
      !tls1_parse_peer_sigalgs(hs, &supported_signature_algorithms)) {
    ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_DECODE_ERROR);
    OPENSSL_PUT_ERROR(SSL, SSL_R_DECODE_ERROR);
    return ssl_hs_error;
  }

  uint8_t alert = SSL_AD_DECODE_ERROR;
  UniquePtr<STACK_OF(CRYPTO_BUFFER)> ca_names =
      ssl_parse_client_CA_list(ssl, &alert, &body);
  if (!ca_names) {
    ssl3_send_alert(ssl, SSL3_AL_FATAL, alert);
    return ssl_hs_error;
  }

  /* Ignore extensions. */
  CBS extensions;
  if (!CBS_get_u16_length_prefixed(&body, &extensions) ||
      CBS_len(&body) != 0) {
    ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_DECODE_ERROR);
    OPENSSL_PUT_ERROR(SSL, SSL_R_DECODE_ERROR);
    return ssl_hs_error;
  }

  hs->cert_request = 1;
  hs->ca_names = std::move(ca_names);
  ssl->ctx->x509_method->hs_flush_cached_ca_names(hs);

  if (!ssl_hash_message(hs, msg)) {
    return ssl_hs_error;
  }

  ssl->method->next_message(ssl);
  hs->tls13_state = state_read_server_certificate;
  return ssl_hs_ok;
}

static enum ssl_hs_wait_t do_read_server_certificate(SSL_HANDSHAKE *hs) {
  SSL *const ssl = hs->ssl;
  SSLMessage msg;
  if (!ssl->method->get_message(ssl, &msg)) {
    return ssl_hs_read_message;
  }
  if (!ssl_check_message_type(ssl, msg, SSL3_MT_CERTIFICATE) ||
      !tls13_process_certificate(hs, msg, 0 /* certificate required */) ||
      !ssl_hash_message(hs, msg)) {
    return ssl_hs_error;
  }

  ssl->method->next_message(ssl);
  hs->tls13_state = state_read_server_certificate_verify;
  return ssl_hs_ok;
}

static enum ssl_hs_wait_t do_read_server_certificate_verify(
    SSL_HANDSHAKE *hs) {
  SSL *const ssl = hs->ssl;
  SSLMessage msg;
  if (!ssl->method->get_message(ssl, &msg)) {
    return ssl_hs_read_message;
  }
  switch (ssl_verify_peer_cert(hs)) {
    case ssl_verify_ok:
      break;
    case ssl_verify_invalid:
      return ssl_hs_error;
    case ssl_verify_retry:
      hs->tls13_state = state_read_server_certificate_verify;
      return ssl_hs_certificate_verify;
  }

  if (!ssl_check_message_type(ssl, msg, SSL3_MT_CERTIFICATE_VERIFY) ||
      !tls13_process_certificate_verify(hs, msg) ||
      !ssl_hash_message(hs, msg)) {
    return ssl_hs_error;
  }

  ssl->method->next_message(ssl);
  hs->tls13_state = state_read_server_finished;
  return ssl_hs_ok;
}

static enum ssl_hs_wait_t do_read_server_finished(SSL_HANDSHAKE *hs) {
  SSL *const ssl = hs->ssl;
  SSLMessage msg;
  if (!ssl->method->get_message(ssl, &msg)) {
    return ssl_hs_read_message;
  }
  if (!ssl_check_message_type(ssl, msg, SSL3_MT_FINISHED) ||
      !tls13_process_finished(hs, msg, 0 /* don't use saved value */) ||
      !ssl_hash_message(hs, msg) ||
      /* Update the secret to the master secret and derive traffic keys. */
      !tls13_advance_key_schedule(hs, kZeroes, hs->hash_len) ||
      !tls13_derive_application_secrets(hs)) {
    return ssl_hs_error;
  }

  ssl->method->next_message(ssl);
  hs->tls13_state = state_send_end_of_early_data;
  return ssl_hs_ok;
}

static enum ssl_hs_wait_t do_send_end_of_early_data(SSL_HANDSHAKE *hs) {
  SSL *const ssl = hs->ssl;

  if (ssl->early_data_accepted) {
    hs->can_early_write = 0;
    if (!ssl->method->add_alert(ssl, SSL3_AL_WARNING,
                                TLS1_AD_END_OF_EARLY_DATA)) {
      return ssl_hs_error;
    }
  }

  if (hs->early_data_offered) {
    if ((ssl->version == TLS1_3_EXPERIMENT_VERSION &&
         !ssl3_add_change_cipher_spec(ssl)) ||
        !tls13_set_traffic_key(ssl, evp_aead_seal, hs->client_handshake_secret,
                               hs->hash_len)) {
      return ssl_hs_error;
    }
  }

  hs->tls13_state = state_send_client_certificate;
  return ssl_hs_ok;
}

static enum ssl_hs_wait_t do_send_client_certificate(SSL_HANDSHAKE *hs) {
  SSL *const ssl = hs->ssl;

  /* The peer didn't request a certificate. */
  if (!hs->cert_request) {
    hs->tls13_state = state_complete_second_flight;
    return ssl_hs_ok;
  }

  /* Call cert_cb to update the certificate. */
  if (ssl->cert->cert_cb != NULL) {
    int rv = ssl->cert->cert_cb(ssl, ssl->cert->cert_cb_arg);
    if (rv == 0) {
      ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_INTERNAL_ERROR);
      OPENSSL_PUT_ERROR(SSL, SSL_R_CERT_CB_ERROR);
      return ssl_hs_error;
    }
    if (rv < 0) {
      hs->tls13_state = state_send_client_certificate;
      return ssl_hs_x509_lookup;
    }
  }

  if (!ssl_on_certificate_selected(hs) ||
      !tls13_add_certificate(hs)) {
    return ssl_hs_error;
  }

  hs->tls13_state = state_send_client_certificate_verify;
  return ssl_hs_ok;
}

static enum ssl_hs_wait_t do_send_client_certificate_verify(SSL_HANDSHAKE *hs) {
  SSL *const ssl = hs->ssl;
  /* Don't send CertificateVerify if there is no certificate. */
  if (!ssl_has_certificate(ssl)) {
    hs->tls13_state = state_complete_second_flight;
    return ssl_hs_ok;
  }

  switch (tls13_add_certificate_verify(hs)) {
    case ssl_private_key_success:
      hs->tls13_state = state_complete_second_flight;
      return ssl_hs_ok;

    case ssl_private_key_retry:
      hs->tls13_state = state_send_client_certificate_verify;
      return ssl_hs_private_key_operation;

    case ssl_private_key_failure:
      return ssl_hs_error;
  }

  assert(0);
  return ssl_hs_error;
}

static enum ssl_hs_wait_t do_complete_second_flight(SSL_HANDSHAKE *hs) {
  SSL *const ssl = hs->ssl;

  /* Send a Channel ID assertion if necessary. */
  if (ssl->s3->tlsext_channel_id_valid) {
    if (!ssl_do_channel_id_callback(ssl)) {
      hs->tls13_state = state_complete_second_flight;
      return ssl_hs_error;
    }

    if (ssl->tlsext_channel_id_private == NULL) {
      return ssl_hs_channel_id_lookup;
    }

    ScopedCBB cbb;
    CBB body;
    if (!ssl->method->init_message(ssl, cbb.get(), &body, SSL3_MT_CHANNEL_ID) ||
        !tls1_write_channel_id(hs, &body) ||
        !ssl_add_message_cbb(ssl, cbb.get())) {
      return ssl_hs_error;
    }
  }

  /* Send a Finished message. */
  if (!tls13_add_finished(hs)) {
    return ssl_hs_error;
  }

  /* Derive the final keys and enable them. */
  if (!tls13_set_traffic_key(ssl, evp_aead_open, hs->server_traffic_secret_0,
                             hs->hash_len) ||
      !tls13_set_traffic_key(ssl, evp_aead_seal, hs->client_traffic_secret_0,
                             hs->hash_len) ||
      !tls13_derive_resumption_secret(hs)) {
    return ssl_hs_error;
  }

  hs->tls13_state = state_done;
  return ssl_hs_flush;
}

enum ssl_hs_wait_t tls13_client_handshake(SSL_HANDSHAKE *hs) {
  while (hs->tls13_state != state_done) {
    enum ssl_hs_wait_t ret = ssl_hs_error;
    enum client_hs_state_t state =
        static_cast<enum client_hs_state_t>(hs->tls13_state);
    switch (state) {
      case state_read_hello_retry_request:
        ret = do_read_hello_retry_request(hs);
        break;
      case state_send_second_client_hello:
        ret = do_send_second_client_hello(hs);
        break;
      case state_read_server_hello:
        ret = do_read_server_hello(hs);
        break;
      case state_process_change_cipher_spec:
        ret = do_process_change_cipher_spec(hs);
        break;
      case state_read_encrypted_extensions:
        ret = do_read_encrypted_extensions(hs);
        break;
      case state_read_certificate_request:
        ret = do_read_certificate_request(hs);
        break;
      case state_read_server_certificate:
        ret = do_read_server_certificate(hs);
        break;
      case state_read_server_certificate_verify:
        ret = do_read_server_certificate_verify(hs);
        break;
      case state_read_server_finished:
        ret = do_read_server_finished(hs);
        break;
      case state_send_end_of_early_data:
        ret = do_send_end_of_early_data(hs);
        break;
      case state_send_client_certificate:
        ret = do_send_client_certificate(hs);
        break;
      case state_send_client_certificate_verify:
        ret = do_send_client_certificate_verify(hs);
        break;
      case state_complete_second_flight:
        ret = do_complete_second_flight(hs);
        break;
      case state_done:
        ret = ssl_hs_ok;
        break;
    }

    if (hs->state != state) {
      ssl_do_info_callback(hs->ssl, SSL_CB_CONNECT_LOOP, 1);
    }

    if (ret != ssl_hs_ok) {
      return ret;
    }
  }

  return ssl_hs_ok;
}

const char *tls13_client_handshake_state(SSL_HANDSHAKE *hs) {
  enum client_hs_state_t state =
      static_cast<enum client_hs_state_t>(hs->tls13_state);
  switch (state) {
    case state_read_hello_retry_request:
      return "TLS 1.3 client read_hello_retry_request";
    case state_send_second_client_hello:
      return "TLS 1.3 client send_second_client_hello";
    case state_read_server_hello:
      return "TLS 1.3 client read_server_hello";
    case state_process_change_cipher_spec:
      return "TLS 1.3 client process_change_cipher_spec";
    case state_read_encrypted_extensions:
      return "TLS 1.3 client read_encrypted_extensions";
    case state_read_certificate_request:
      return "TLS 1.3 client read_certificate_request";
    case state_read_server_certificate:
      return "TLS 1.3 client read_server_certificate";
    case state_read_server_certificate_verify:
      return "TLS 1.3 client read_server_certificate_verify";
    case state_read_server_finished:
      return "TLS 1.3 client read_server_finished";
    case state_send_end_of_early_data:
      return "TLS 1.3 client send_end_of_early_data";
    case state_send_client_certificate:
      return "TLS 1.3 client send_client_certificate";
    case state_send_client_certificate_verify:
      return "TLS 1.3 client send_client_certificate_verify";
    case state_complete_second_flight:
      return "TLS 1.3 client complete_second_flight";
    case state_done:
      return "TLS 1.3 client done";
  }

  return "TLS 1.3 client unknown";
}

int tls13_process_new_session_ticket(SSL *ssl, const SSLMessage &msg) {
  UniquePtr<SSL_SESSION> session(SSL_SESSION_dup(ssl->s3->established_session,
                                                 SSL_SESSION_INCLUDE_NONAUTH));
  if (!session) {
    return 0;
  }

  ssl_session_rebase_time(ssl, session.get());

  uint32_t server_timeout;
  CBS body = msg.body, ticket, extensions;
  if (!CBS_get_u32(&body, &server_timeout) ||
      !CBS_get_u32(&body, &session->ticket_age_add) ||
      !CBS_get_u16_length_prefixed(&body, &ticket) ||
      !CBS_stow(&ticket, &session->tlsext_tick, &session->tlsext_ticklen) ||
      !CBS_get_u16_length_prefixed(&body, &extensions) ||
      CBS_len(&body) != 0) {
    ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_DECODE_ERROR);
    OPENSSL_PUT_ERROR(SSL, SSL_R_DECODE_ERROR);
    return 0;
  }

  /* Cap the renewable lifetime by the server advertised value. This avoids
   * wasting bandwidth on 0-RTT when we know the server will reject it. */
  if (session->timeout > server_timeout) {
    session->timeout = server_timeout;
  }

  /* Parse out the extensions. */
  int have_early_data_info = 0;
  CBS early_data_info;
  const SSL_EXTENSION_TYPE ext_types[] = {
      {TLSEXT_TYPE_ticket_early_data_info, &have_early_data_info,
       &early_data_info},
  };

  uint8_t alert = SSL_AD_DECODE_ERROR;
  if (!ssl_parse_extensions(&extensions, &alert, ext_types,
                            OPENSSL_ARRAY_SIZE(ext_types),
                            1 /* ignore unknown */)) {
    ssl3_send_alert(ssl, SSL3_AL_FATAL, alert);
    return 0;
  }

  if (have_early_data_info && ssl->cert->enable_early_data) {
    if (!CBS_get_u32(&early_data_info, &session->ticket_max_early_data) ||
        CBS_len(&early_data_info) != 0) {
      ssl3_send_alert(ssl, SSL3_AL_FATAL, SSL_AD_DECODE_ERROR);
      OPENSSL_PUT_ERROR(SSL, SSL_R_DECODE_ERROR);
      return 0;
    }
  }

  session->ticket_age_add_valid = 1;
  session->not_resumable = 0;

  if (ssl->ctx->new_session_cb != NULL &&
      ssl->ctx->new_session_cb(ssl, session.get())) {
    /* |new_session_cb|'s return value signals that it took ownership. */
    session.release();
  }

  return 1;
}

void ssl_clear_tls13_state(SSL_HANDSHAKE *hs) {
  hs->key_share.reset();

  OPENSSL_free(hs->key_share_bytes);
  hs->key_share_bytes = NULL;
  hs->key_share_bytes_len = 0;
}

}  // namespace bssl
