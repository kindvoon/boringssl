/* Copyright (c) 2020, Google Inc.
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

#include <openssl/trust_token.h>

#include <openssl/bn.h>
#include <openssl/bytestring.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/mem.h>
#include <openssl/nid.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "../ec_extra/internal.h"
#include "../fipsmodule/bn/internal.h"
#include "../fipsmodule/ec/internal.h"

#include "internal.h"


typedef struct {
  const EC_GROUP *group;
  EC_RAW_POINT h;
  // hash_t implements the H_t operation in PMBTokens. It returns on on success
  // and zero on error.
  int (*hash_t)(const EC_GROUP *group, EC_RAW_POINT *out,
                const uint8_t t[PMBTOKEN_NONCE_SIZE]);
  // hash_s implements the H_s operation in PMBTokens. It returns on on success
  // and zero on error.
  int (*hash_s)(const EC_GROUP *group, EC_RAW_POINT *out, const EC_RAW_POINT *t,
                const uint8_t s[PMBTOKEN_NONCE_SIZE]);
  // hash_c implements the H_c operation in PMBTokens. It returns on on success
  // and zero on error.
  int (*hash_c)(const EC_GROUP *group, EC_SCALAR *out, uint8_t *buf,
                size_t len);
} PMBTOKEN_METHOD;

static const uint8_t kDefaultAdditionalData[32] = {0};

static int mul_twice(const EC_GROUP *group, EC_RAW_POINT *out,
                     const EC_RAW_POINT *g, const EC_SCALAR *g_scalar,
                     const EC_RAW_POINT *p, const EC_SCALAR *p_scalar) {
  EC_RAW_POINT tmp1, tmp2;
  if (!ec_point_mul_scalar(group, &tmp1, g, g_scalar) ||
      !ec_point_mul_scalar(group, &tmp2, p, p_scalar)) {
    return 0;
  }

  group->meth->add(group, out, &tmp1, &tmp2);
  return 1;
}

static int mul_twice_base(const EC_GROUP *group, EC_RAW_POINT *out,
                          const EC_SCALAR *base_scalar, const EC_RAW_POINT *p,
                          const EC_SCALAR *p_scalar) {
  EC_RAW_POINT tmp1, tmp2;
  if (!ec_point_mul_scalar_base(group, &tmp1, base_scalar) ||
      !ec_point_mul_scalar(group, &tmp2, p, p_scalar)) {
    return 0;
  }

  group->meth->add(group, out, &tmp1, &tmp2);
  return 1;
}

// (v0;v1) = p_scalar*(G;p1) + q_scalar*(q0;q1) - r_scalar*(r0;r1)
static int mul_add_and_sub(const EC_GROUP *group, EC_RAW_POINT *out_v0,
                           EC_RAW_POINT *out_v1, const EC_RAW_POINT *p1,
                           const EC_SCALAR *p_scalar, const EC_RAW_POINT *q0,
                           const EC_RAW_POINT *q1, const EC_SCALAR *q_scalar,
                           const EC_RAW_POINT *r0, const EC_RAW_POINT *r1,
                           const EC_SCALAR *r_scalar) {
  EC_RAW_POINT tmp0, tmp1, v0, v1;
  if (!mul_twice_base(group, &v0, p_scalar, q0, q_scalar) ||
      !mul_twice(group, &v1, p1, p_scalar, q1, q_scalar) ||
      !ec_point_mul_scalar(group, &tmp0, r0, r_scalar) ||
      !ec_point_mul_scalar(group, &tmp1, r1, r_scalar)) {
    return 0;
  }
  ec_GFp_simple_invert(group, &tmp0);
  ec_GFp_simple_invert(group, &tmp1);
  group->meth->add(group, out_v0, &v0, &tmp0);
  group->meth->add(group, out_v1, &v1, &tmp1);
  return 1;
}

// generate_keypair generates a keypair for the PMBTokens construction.
// |out_x| and |out_y| are set to the secret half of the keypair, while
// |*out_pub| is set to the public half of the keypair. It returns one on
// success and zero on failure.
static int generate_keypair(const PMBTOKEN_METHOD *method, EC_SCALAR *out_x,
                            EC_SCALAR *out_y, EC_RAW_POINT *out_pub) {
  if (!ec_random_nonzero_scalar(method->group, out_x, kDefaultAdditionalData) ||
      !ec_random_nonzero_scalar(method->group, out_y, kDefaultAdditionalData) ||
      !mul_twice_base(method->group, out_pub, out_x, &method->h, out_y)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, ERR_R_MALLOC_FAILURE);
    return 0;
  }
  return 1;
}

static int point_to_cbb(CBB *out, const EC_GROUP *group,
                        const EC_RAW_POINT *point) {
  EC_AFFINE affine;
  if (!ec_jacobian_to_affine(group, &affine, point)) {
    return 0;
  }
  size_t len =
      ec_point_to_bytes(group, &affine, POINT_CONVERSION_UNCOMPRESSED, NULL, 0);
  if (len == 0) {
    return 0;
  }
  uint8_t *p;
  return CBB_add_space(out, &p, len) &&
         ec_point_to_bytes(group, &affine, POINT_CONVERSION_UNCOMPRESSED, p,
                           len) == len;
}

static int cbs_get_prefixed_point(CBS *cbs, const EC_GROUP *group,
                                  EC_RAW_POINT *out) {
  CBS child;
  EC_AFFINE affine;
  if (!CBS_get_u16_length_prefixed(cbs, &child) ||
      !ec_point_from_uncompressed(group, &affine, CBS_data(&child),
                                  CBS_len(&child))) {
    return 0;
  }
  ec_affine_to_jacobian(group, out, &affine);
  return 1;
}

void PMBTOKEN_PRETOKEN_free(PMBTOKEN_PRETOKEN *pretoken) {
  OPENSSL_free(pretoken);
}

static int pmbtoken_generate_key(const PMBTOKEN_METHOD *method,
                                 CBB *out_private, CBB *out_public) {
  const EC_GROUP *group = method->group;
  EC_RAW_POINT pub0, pub1, pubs;
  EC_SCALAR x0, y0, x1, y1, xs, ys;
  if (!generate_keypair(method, &x0, &y0, &pub0) ||
      !generate_keypair(method, &x1, &y1, &pub1) ||
      !generate_keypair(method, &xs, &ys, &pubs)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_KEYGEN_FAILURE);
    return 0;
  }

  const EC_SCALAR *scalars[] = {&x0, &y0, &x1, &y1, &xs, &ys};
  size_t scalar_len = BN_num_bytes(&group->order);
  for (size_t i = 0; i < OPENSSL_ARRAY_SIZE(scalars); i++) {
    uint8_t *buf;
    if (!CBB_add_space(out_private, &buf, scalar_len)) {
      OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_BUFFER_TOO_SMALL);
      return 0;
    }
    ec_scalar_to_bytes(group, buf, &scalar_len, scalars[i]);
  }

  // TODO(https://crbug.com/boringssl/331): When updating the key format, remove
  // the redundant length prefixes.
  CBB child;
  if (!CBB_add_u16_length_prefixed(out_public, &child) ||
      !point_to_cbb(&child, group, &pub0) ||
      !CBB_add_u16_length_prefixed(out_public, &child) ||
      !point_to_cbb(&child, group, &pub1) ||
      !CBB_add_u16_length_prefixed(out_public, &child) ||
      !point_to_cbb(&child, group, &pubs) ||
      !CBB_flush(out_public)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_BUFFER_TOO_SMALL);
    return 0;
  }

  return 1;
}

static int pmbtoken_client_key_from_bytes(const PMBTOKEN_METHOD *method,
                                          PMBTOKEN_CLIENT_KEY *key,
                                          const uint8_t *in, size_t len) {
  // TODO(https://crbug.com/boringssl/331): When updating the key format, remove
  // the redundant length prefixes.
  CBS cbs;
  CBS_init(&cbs, in, len);
  if (!cbs_get_prefixed_point(&cbs, method->group, &key->pub0) ||
      !cbs_get_prefixed_point(&cbs, method->group, &key->pub1) ||
      !cbs_get_prefixed_point(&cbs, method->group, &key->pubs) ||
      CBS_len(&cbs) != 0) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_DECODE_FAILURE);
    return 0;
  }

  return 1;
}

static int pmbtoken_issuer_key_from_bytes(const PMBTOKEN_METHOD *method,
                                          PMBTOKEN_ISSUER_KEY *key,
                                          const uint8_t *in, size_t len) {
  const EC_GROUP *group = method->group;
  CBS cbs, tmp;
  CBS_init(&cbs, in, len);
  size_t scalar_len = BN_num_bytes(&group->order);
  EC_SCALAR *scalars[] = {&key->x0, &key->y0, &key->x1,
                          &key->y1, &key->xs, &key->ys};
  for (size_t i = 0; i < OPENSSL_ARRAY_SIZE(scalars); i++) {
    if (!CBS_get_bytes(&cbs, &tmp, scalar_len) ||
        !ec_scalar_from_bytes(group, scalars[i], CBS_data(&tmp),
                              CBS_len(&tmp))) {
      OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_DECODE_FAILURE);
      return 0;
    }
  }

  // Recompute the public key.
  if (!mul_twice_base(group, &key->pubs, &key->xs, &method->h, &key->ys) ||
      !mul_twice_base(group, &key->pub0, &key->x0, &method->h, &key->y0) ||
      !mul_twice_base(group, &key->pub1, &key->x1, &method->h, &key->y1)) {
    return 0;
  }

  return 1;
}

static STACK_OF(PMBTOKEN_PRETOKEN) *
    pmbtoken_blind(const PMBTOKEN_METHOD *method, CBB *cbb, size_t count) {
  const EC_GROUP *group = method->group;
  STACK_OF(PMBTOKEN_PRETOKEN) *pretokens = sk_PMBTOKEN_PRETOKEN_new_null();
  if (pretokens == NULL) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, ERR_R_MALLOC_FAILURE);
    goto err;
  }

  for (size_t i = 0; i < count; i++) {
    // Insert |pretoken| into |pretokens| early to simplify error-handling.
    PMBTOKEN_PRETOKEN *pretoken = OPENSSL_malloc(sizeof(PMBTOKEN_PRETOKEN));
    if (pretoken == NULL ||
        !sk_PMBTOKEN_PRETOKEN_push(pretokens, pretoken)) {
      OPENSSL_PUT_ERROR(TRUST_TOKEN, ERR_R_MALLOC_FAILURE);
      PMBTOKEN_PRETOKEN_free(pretoken);
      goto err;
    }

    RAND_bytes(pretoken->t, sizeof(pretoken->t));

    // We sample |pretoken->r| in Montgomery form to simplify inverting.
    if (!ec_random_nonzero_scalar(group, &pretoken->r,
                                  kDefaultAdditionalData)) {
      OPENSSL_PUT_ERROR(TRUST_TOKEN, ERR_R_MALLOC_FAILURE);
      goto err;
    }

    EC_SCALAR rinv;
    ec_scalar_inv0_montgomery(group, &rinv, &pretoken->r);
    // Convert both out of Montgomery form.
    ec_scalar_from_montgomery(group, &pretoken->r, &pretoken->r);
    ec_scalar_from_montgomery(group, &rinv, &rinv);

    EC_RAW_POINT T;
    if (!method->hash_t(group, &T, pretoken->t) ||
        !ec_point_mul_scalar(group, &pretoken->Tp, &T, &rinv)) {
      goto err;
    }

    // TODO(https://crbug.com/boringssl/331): When updating the key format,
    // remove the redundant length prefixes.
    CBB child;
    if (!CBB_add_u16_length_prefixed(cbb, &child) ||
        !point_to_cbb(&child, group, &pretoken->Tp) ||
        !CBB_flush(cbb)) {
      goto err;
    }
  }

  return pretokens;

err:
  sk_PMBTOKEN_PRETOKEN_pop_free(pretokens, PMBTOKEN_PRETOKEN_free);
  return NULL;
}

static int scalar_to_cbb(CBB *out, const EC_GROUP *group,
                         const EC_SCALAR *scalar) {
  uint8_t *buf;
  size_t scalar_len = BN_num_bytes(&group->order);
  if (!CBB_add_space(out, &buf, scalar_len)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, ERR_R_MALLOC_FAILURE);
    return 0;
  }
  ec_scalar_to_bytes(group, buf, &scalar_len, scalar);
  return 1;
}

static int scalar_from_cbs(CBS *cbs, const EC_GROUP *group, EC_SCALAR *out) {
  size_t scalar_len = BN_num_bytes(&group->order);
  CBS tmp;
  if (!CBS_get_bytes(cbs, &tmp, scalar_len)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_DECODE_FAILURE);
    return 0;
  }

  ec_scalar_from_bytes(group, out, CBS_data(&tmp), CBS_len(&tmp));
  return 1;
}

static int hash_c_dleq(const PMBTOKEN_METHOD *method, EC_SCALAR *out,
                       const EC_RAW_POINT *X, const EC_RAW_POINT *T,
                       const EC_RAW_POINT *S, const EC_RAW_POINT *W,
                       const EC_RAW_POINT *K0, const EC_RAW_POINT *K1) {
  static const uint8_t kDLEQ2Label[] = "DLEQ2";

  int ok = 0;
  CBB cbb;
  CBB_zero(&cbb);
  uint8_t *buf = NULL;
  size_t len;
  if (!CBB_init(&cbb, 0) ||
      !CBB_add_bytes(&cbb, kDLEQ2Label, sizeof(kDLEQ2Label)) ||
      !point_to_cbb(&cbb, method->group, X) ||
      !point_to_cbb(&cbb, method->group, T) ||
      !point_to_cbb(&cbb, method->group, S) ||
      !point_to_cbb(&cbb, method->group, W) ||
      !point_to_cbb(&cbb, method->group, K0) ||
      !point_to_cbb(&cbb, method->group, K1) ||
      !CBB_finish(&cbb, &buf, &len) ||
      !method->hash_c(method->group, out, buf, len)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, ERR_R_MALLOC_FAILURE);
    goto err;
  }

  ok = 1;

err:
  CBB_cleanup(&cbb);
  OPENSSL_free(buf);
  return ok;
}

static int hash_c_dleqor(const PMBTOKEN_METHOD *method, EC_SCALAR *out,
                         const EC_RAW_POINT *X0, const EC_RAW_POINT *X1,
                         const EC_RAW_POINT *T, const EC_RAW_POINT *S,
                         const EC_RAW_POINT *W, const EC_RAW_POINT *K00,
                         const EC_RAW_POINT *K01, const EC_RAW_POINT *K10,
                         const EC_RAW_POINT *K11) {
  static const uint8_t kDLEQOR2Label[] = "DLEQOR2";

  int ok = 0;
  CBB cbb;
  CBB_zero(&cbb);
  uint8_t *buf = NULL;
  size_t len;
  if (!CBB_init(&cbb, 0) ||
      !CBB_add_bytes(&cbb, kDLEQOR2Label, sizeof(kDLEQOR2Label)) ||
      !point_to_cbb(&cbb, method->group, X0) ||
      !point_to_cbb(&cbb, method->group, X1) ||
      !point_to_cbb(&cbb, method->group, T) ||
      !point_to_cbb(&cbb, method->group, S) ||
      !point_to_cbb(&cbb, method->group, W) ||
      !point_to_cbb(&cbb, method->group, K00) ||
      !point_to_cbb(&cbb, method->group, K01) ||
      !point_to_cbb(&cbb, method->group, K10) ||
      !point_to_cbb(&cbb, method->group, K11) ||
      !CBB_finish(&cbb, &buf, &len) ||
      !method->hash_c(method->group, out, buf, len)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, ERR_R_MALLOC_FAILURE);
    goto err;
  }

  ok = 1;

err:
  CBB_cleanup(&cbb);
  OPENSSL_free(buf);
  return ok;
}

// The DLEQ2 and DLEQOR2 constructions are described in appendix B of
// https://eprint.iacr.org/2020/072/20200324:214215. DLEQ2 is an instance of
// DLEQOR2 with only one value (n=1).

static int dleq_generate(const PMBTOKEN_METHOD *method, CBB *cbb,
                         const PMBTOKEN_ISSUER_KEY *priv, const EC_RAW_POINT *T,
                         const EC_RAW_POINT *S, const EC_RAW_POINT *W,
                         const EC_RAW_POINT *Ws, uint8_t private_metadata) {
  const EC_GROUP *group = method->group;

  // We generate a DLEQ proof for the validity token and a DLEQOR2 proof for the
  // private metadata token. To allow amortizing Jacobian-to-affine conversions,
  // we compute Ki for both proofs first.

  // Setup the DLEQ proof.
  EC_SCALAR ks0, ks1;
  EC_RAW_POINT Ks0, Ks1;
  if (// ks0, ks1 <- Zp
      !ec_random_nonzero_scalar(group, &ks0, kDefaultAdditionalData) ||
      !ec_random_nonzero_scalar(group, &ks1, kDefaultAdditionalData) ||
      // Ks = ks0*(G;T) + ks1*(H;S)
      !mul_twice_base(group, &Ks0, &ks0, &method->h, &ks1) ||
      !mul_twice(group, &Ks1, T, &ks0, S, &ks1)) {
    return 0;
  }

  // Setup the DLEQOR proof. First, select values of xb, yb (keys corresponding
  // to the private metadata value) and pubo (public key corresponding to the
  // other value) in constant time.
  BN_ULONG mask = ((BN_ULONG)0) - (private_metadata & 1);
  EC_RAW_POINT pubo;
  EC_SCALAR xb, yb;
  ec_scalar_select(group, &xb, mask, &priv->x1, &priv->x0);
  ec_scalar_select(group, &yb, mask, &priv->y1, &priv->y0);
  ec_point_select(group, &pubo, mask, &priv->pub0, &priv->pub1);

  EC_SCALAR k0, k1, co, uo, vo;
  EC_RAW_POINT Kb0, Kb1, Ko0, Ko1;
  if (// k0, k1 <- Zp
      !ec_random_nonzero_scalar(group, &k0, kDefaultAdditionalData) ||
      !ec_random_nonzero_scalar(group, &k1, kDefaultAdditionalData) ||
      // Kb = k0*(G;T) + k1*(H;S)
      !mul_twice_base(group, &Kb0, &k0, &method->h, &k1) ||
      !mul_twice(group, &Kb1, T, &k0, S, &k1) ||
      // co, uo, vo <- Zp
      !ec_random_nonzero_scalar(group, &co, kDefaultAdditionalData) ||
      !ec_random_nonzero_scalar(group, &uo, kDefaultAdditionalData) ||
      !ec_random_nonzero_scalar(group, &vo, kDefaultAdditionalData) ||
      // Ko = uo*(G;T) + vo*(H;S) - co*(pubo;W)
      !mul_add_and_sub(group, &Ko0, &Ko1, T, &uo, &method->h, S, &vo, &pubo, W,
                       &co)) {
    return 0;
  }

  // Select the K corresponding to K0 and K1 in constant-time.
  EC_RAW_POINT K00, K01, K10, K11;
  ec_point_select(group, &K00, mask, &Ko0, &Kb0);
  ec_point_select(group, &K01, mask, &Ko1, &Kb1);
  ec_point_select(group, &K10, mask, &Kb0, &Ko0);
  ec_point_select(group, &K11, mask, &Kb1, &Ko1);

  // Compute c = Hc(...) for the two proofs.
  EC_SCALAR cs, c;
  if (!hash_c_dleq(method, &cs, &priv->pubs, T, S, Ws, &Ks0, &Ks1) ||
      !hash_c_dleqor(method, &c, &priv->pub0, &priv->pub1, T, S, W, &K00, &K01,
                     &K10, &K11)) {
    return 0;
  }

  // Compute cb, ub, and ub for the two proofs. In each of these products, only
  // one operand is in Montgomery form, so the product does not need to be
  // converted.

  EC_SCALAR cs_mont;
  ec_scalar_to_montgomery(group, &cs_mont, &cs);

  // us = ks0 + cs*xs
  EC_SCALAR us, vs;
  ec_scalar_mul_montgomery(group, &us, &priv->xs, &cs_mont);
  ec_scalar_add(group, &us, &ks0, &us);

  // vs = ks1 + cs*ys
  ec_scalar_mul_montgomery(group, &vs, &priv->ys, &cs_mont);
  ec_scalar_add(group, &vs, &ks1, &vs);

  // Store DLEQ2 proof in transcript.
  if (!scalar_to_cbb(cbb, group, &cs) ||
      !scalar_to_cbb(cbb, group, &us) ||
      !scalar_to_cbb(cbb, group, &vs)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, ERR_R_MALLOC_FAILURE);
    return 0;
  }

  // cb = c - co
  EC_SCALAR cb, ub, vb;
  ec_scalar_sub(group, &cb, &c, &co);

  EC_SCALAR cb_mont;
  ec_scalar_to_montgomery(group, &cb_mont, &cb);

  // ub = k0 + cb*xb
  ec_scalar_mul_montgomery(group, &ub, &xb, &cb_mont);
  ec_scalar_add(group, &ub, &k0, &ub);

  // vb = k1 + cb*yb
  ec_scalar_mul_montgomery(group, &vb, &yb, &cb_mont);
  ec_scalar_add(group, &vb, &k1, &vb);

  // Select c, u, v in constant-time.
  EC_SCALAR c0, c1, u0, u1, v0, v1;
  ec_scalar_select(group, &c0, mask, &co, &cb);
  ec_scalar_select(group, &u0, mask, &uo, &ub);
  ec_scalar_select(group, &v0, mask, &vo, &vb);
  ec_scalar_select(group, &c1, mask, &cb, &co);
  ec_scalar_select(group, &u1, mask, &ub, &uo);
  ec_scalar_select(group, &v1, mask, &vb, &vo);

  // Store DLEQOR2 proof in transcript.
  if (!scalar_to_cbb(cbb, group, &c0) ||
      !scalar_to_cbb(cbb, group, &c1) ||
      !scalar_to_cbb(cbb, group, &u0) ||
      !scalar_to_cbb(cbb, group, &u1) ||
      !scalar_to_cbb(cbb, group, &v0) ||
      !scalar_to_cbb(cbb, group, &v1)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, ERR_R_MALLOC_FAILURE);
    return 0;
  }

  return 1;
}

static int dleq_verify(const PMBTOKEN_METHOD *method, CBS *cbs,
                       const PMBTOKEN_CLIENT_KEY *pub, const EC_RAW_POINT *T,
                       const EC_RAW_POINT *S, const EC_RAW_POINT *W,
                       const EC_RAW_POINT *Ws) {
  const EC_GROUP *group = method->group;

  // We verify a DLEQ proof for the validity token and a DLEQOR2 proof for the
  // private metadata token. To allow amortizing Jacobian-to-affine conversions,
  // we compute Ki for both proofs first.

  // Decode the DLEQ proof.
  EC_SCALAR cs, us, vs;
  if (!scalar_from_cbs(cbs, group, &cs) ||
      !scalar_from_cbs(cbs, group, &us) ||
      !scalar_from_cbs(cbs, group, &vs)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_DECODE_FAILURE);
    return 0;
  }

  // Ks = us*(G;T) + vs*(H;S) - cs*(pubs;Ws)
  EC_RAW_POINT Ks0, Ks1;
  if (!mul_add_and_sub(group, &Ks0, &Ks1, T, &us, &method->h, S, &vs,
                       &pub->pubs, Ws, &cs)) {
    return 0;
  }

  // Decode the DLEQOR proof.
  EC_SCALAR c0, c1, u0, u1, v0, v1;
  if (!scalar_from_cbs(cbs, group, &c0) ||
      !scalar_from_cbs(cbs, group, &c1) ||
      !scalar_from_cbs(cbs, group, &u0) ||
      !scalar_from_cbs(cbs, group, &u1) ||
      !scalar_from_cbs(cbs, group, &v0) ||
      !scalar_from_cbs(cbs, group, &v1)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_DECODE_FAILURE);
    return 0;
  }

  EC_RAW_POINT K00, K01, K10, K11;
  if (// K0 = u0*(G;T) + v0*(H;S) - c0*(pub0;W)
      !mul_add_and_sub(group, &K00, &K01, T, &u0, &method->h, S, &v0,
                       &pub->pub0, W, &c0) ||
      // K1 = u1*(G;T) + v1*(H;S) - c1*(pub1;Ws)
      !mul_add_and_sub(group, &K10, &K11, T, &u1, &method->h, S, &v1,
                       &pub->pub1, W, &c1)) {
    return 0;
  }

  // Check the DLEQ proof.
  EC_SCALAR calculated;
  if (!hash_c_dleq(method, &calculated, &pub->pubs, T, S, Ws, &Ks0, &Ks1)) {
    return 0;
  }

  // cs == calculated
  if (!ec_scalar_equal_vartime(group, &cs, &calculated)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_INVALID_PROOF);
    return 0;
  }

  // Check the DLEQOR proof.
  if (!hash_c_dleqor(method, &calculated, &pub->pub0, &pub->pub1, T, S, W, &K00,
                     &K01, &K10, &K11)) {
    return 0;
  }

  // c0 + c1 == calculated
  EC_SCALAR c;
  ec_scalar_add(group, &c, &c0, &c1);
  if (!ec_scalar_equal_vartime(group, &c, &calculated)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_INVALID_PROOF);
    return 0;
  }

  return 1;
}

static int pmbtoken_sign(const PMBTOKEN_METHOD *method,
                         const PMBTOKEN_ISSUER_KEY *key, CBB *cbb, CBS *cbs,
                         size_t num_requested, size_t num_to_issue,
                         uint8_t private_metadata) {
  const EC_GROUP *group = method->group;
  if (num_requested < num_to_issue) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, ERR_R_INTERNAL_ERROR);
    return 0;
  }

  for (size_t i = 0; i < num_to_issue; i++) {
    EC_RAW_POINT Tp;
    if (!cbs_get_prefixed_point(cbs, group, &Tp)) {
      OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_DECODE_FAILURE);
      return 0;
    }

    EC_SCALAR xb, yb;
    BN_ULONG mask = ((BN_ULONG)0) - (private_metadata & 1);
    ec_scalar_select(group, &xb, mask, &key->x1, &key->x0);
    ec_scalar_select(group, &yb, mask, &key->y1, &key->y0);

    uint8_t s[PMBTOKEN_NONCE_SIZE];
    RAND_bytes(s, PMBTOKEN_NONCE_SIZE);
    EC_RAW_POINT Sp, Wp, Wsp;
    CBB child;
    if (!method->hash_s(group, &Sp, &Tp, s) ||
        !mul_twice(group, &Wp, &Tp, &xb, &Sp, &yb) ||
        !mul_twice(group, &Wsp, &Tp, &key->xs, &Sp, &key->ys) ||
        !CBB_add_bytes(cbb, s, PMBTOKEN_NONCE_SIZE) ||
        // TODO(https://crbug.com/boringssl/331): When updating the key format,
        // remove the redundant length prefixes.
        !CBB_add_u16_length_prefixed(cbb, &child) ||
        !point_to_cbb(&child, group, &Wp) ||
        !CBB_add_u16_length_prefixed(cbb, &child) ||
        !point_to_cbb(&child, group, &Wsp) ||
        !CBB_add_u16_length_prefixed(cbb, &child) ||
        !dleq_generate(method, &child, key, &Tp, &Sp, &Wp, &Wsp,
                       private_metadata) ||
        !CBB_flush(cbb)) {
      return 0;
    }
  }

  // Skip over any unused requests.
  size_t point_len = 1 + 2 * BN_num_bytes(&group->field);
  if (!CBS_skip(cbs, (2 + point_len) * (num_requested - num_to_issue))) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_DECODE_FAILURE);
    return 0;
  }

  return 1;
}

static STACK_OF(TRUST_TOKEN) *
    pmbtoken_unblind(const PMBTOKEN_METHOD *method,
                     const PMBTOKEN_CLIENT_KEY *key,
                     const STACK_OF(PMBTOKEN_PRETOKEN) * pretokens, CBS *cbs,
                     size_t count, uint32_t key_id) {
  const EC_GROUP *group = method->group;
  if (count > sk_PMBTOKEN_PRETOKEN_num(pretokens)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_DECODE_FAILURE);
    return NULL;
  }

  STACK_OF(TRUST_TOKEN) *ret = sk_TRUST_TOKEN_new_null();
  if (ret == NULL) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, ERR_R_MALLOC_FAILURE);
    return NULL;
  }

  for (size_t i = 0; i < count; i++) {
    const PMBTOKEN_PRETOKEN *pretoken =
        sk_PMBTOKEN_PRETOKEN_value(pretokens, i);

    uint8_t s[PMBTOKEN_NONCE_SIZE];
    EC_RAW_POINT Wp, Wsp;
    CBS proof;
    if (!CBS_copy_bytes(cbs, s, PMBTOKEN_NONCE_SIZE) ||
        !cbs_get_prefixed_point(cbs, group, &Wp) ||
        !cbs_get_prefixed_point(cbs, group, &Wsp) ||
        !CBS_get_u16_length_prefixed(cbs, &proof)) {
      OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_DECODE_FAILURE);
      goto err;
    }

    EC_RAW_POINT Sp;
    if (!method->hash_s(group, &Sp, &pretoken->Tp, s) ||
        !dleq_verify(method, &proof, key, &pretoken->Tp, &Sp, &Wp, &Wsp)) {
      goto err;
    }

    if (CBS_len(&proof) != 0) {
      OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_DECODE_FAILURE);
      goto err;
    }

    EC_RAW_POINT S, W, Ws;
    if (!ec_point_mul_scalar(group, &S, &Sp, &pretoken->r) ||
        !ec_point_mul_scalar(group, &W, &Wp, &pretoken->r) ||
        !ec_point_mul_scalar(group, &Ws, &Wsp, &pretoken->r)) {
      goto err;
    }

    // Serialize the token. Include |key_id| to avoid an extra copy in the layer
    // above.
    CBB token_cbb, child;
    size_t point_len = 1 + 2 * BN_num_bytes(&group->field);
    if (!CBB_init(&token_cbb, 4 + PMBTOKEN_NONCE_SIZE + 3 * (2 + point_len)) ||
        !CBB_add_u32(&token_cbb, key_id) ||
        !CBB_add_bytes(&token_cbb, pretoken->t, PMBTOKEN_NONCE_SIZE) ||
        // TODO(https://crbug.com/boringssl/331): When updating the key format,
        // remove the redundant length prefixes.
        !CBB_add_u16_length_prefixed(&token_cbb, &child) ||
        !point_to_cbb(&child, group, &S) ||
        !CBB_add_u16_length_prefixed(&token_cbb, &child) ||
        !point_to_cbb(&child, group, &W) ||
        !CBB_add_u16_length_prefixed(&token_cbb, &child) ||
        !point_to_cbb(&child, group, &Ws) ||
        !CBB_flush(&token_cbb)) {
      CBB_cleanup(&token_cbb);
      goto err;
    }

    TRUST_TOKEN *token =
        TRUST_TOKEN_new(CBB_data(&token_cbb), CBB_len(&token_cbb));
    CBB_cleanup(&token_cbb);
    if (token == NULL ||
        !sk_TRUST_TOKEN_push(ret, token)) {
      OPENSSL_PUT_ERROR(TRUST_TOKEN, ERR_R_MALLOC_FAILURE);
      TRUST_TOKEN_free(token);
      goto err;
    }
  }

  return ret;

err:
  sk_TRUST_TOKEN_pop_free(ret, TRUST_TOKEN_free);
  return NULL;
}

static int pmbtoken_read(const PMBTOKEN_METHOD *method,
                         const PMBTOKEN_ISSUER_KEY *key,
                         uint8_t out_nonce[PMBTOKEN_NONCE_SIZE],
                         uint8_t *out_private_metadata, const uint8_t *token,
                         size_t token_len) {
  const EC_GROUP *group = method->group;
  CBS cbs;
  CBS_init(&cbs, token, token_len);
  EC_RAW_POINT S, W, Ws;
  if (!CBS_copy_bytes(&cbs, out_nonce, PMBTOKEN_NONCE_SIZE) ||
      !cbs_get_prefixed_point(&cbs, group, &S) ||
      !cbs_get_prefixed_point(&cbs, group, &W) ||
      !cbs_get_prefixed_point(&cbs, group, &Ws) ||
      CBS_len(&cbs) != 0) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_INVALID_TOKEN);
    return 0;
  }


  EC_RAW_POINT T;
  if (!method->hash_t(group, &T, out_nonce)) {
    return 0;
  }

  EC_RAW_POINT calculated;
  // Check the validity of the token.
  if (!mul_twice(group, &calculated, &T, &key->xs, &S, &key->ys) ||
      !ec_GFp_simple_points_equal(group, &calculated, &Ws)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, TRUST_TOKEN_R_BAD_VALIDITY_CHECK);
    return 0;
  }

  EC_RAW_POINT W0, W1;
  if (!mul_twice(group, &W0, &T, &key->x0, &S, &key->y0) ||
      !mul_twice(group, &W1, &T, &key->x1, &S, &key->y1)) {
    return 0;
  }

  const int is_W0 = ec_GFp_simple_points_equal(group, &W0, &W);
  const int is_W1 = ec_GFp_simple_points_equal(group, &W1, &W);
  const int is_valid = is_W0 ^ is_W1;
  if (!is_valid) {
    // Invalid tokens will fail the validity check above.
    OPENSSL_PUT_ERROR(TRUST_TOKEN, ERR_R_INTERNAL_ERROR);
    return 0;
  }

  *out_private_metadata = is_W1;
  return 1;
}


// PMBTokens experiment v0.

static int pmbtoken_exp0_hash_t(const EC_GROUP *group, EC_RAW_POINT *out,
                                const uint8_t t[PMBTOKEN_NONCE_SIZE]) {
  const uint8_t kHashTLabel[] = "PMBTokensV0 HashT";
  return ec_hash_to_curve_p521_xmd_sha512_sswu_draft06(
      group, out, kHashTLabel, sizeof(kHashTLabel), t, PMBTOKEN_NONCE_SIZE);
}

static int pmbtoken_exp0_hash_s(const EC_GROUP *group, EC_RAW_POINT *out,
                                const EC_RAW_POINT *t,
                                const uint8_t s[PMBTOKEN_NONCE_SIZE]) {
  const uint8_t kHashSLabel[] = "PMBTokensV0 HashS";
  int ret = 0;
  CBB cbb;
  uint8_t *buf = NULL;
  size_t len;
  if (!CBB_init(&cbb, 0) || !point_to_cbb(&cbb, group, t) ||
      !CBB_add_bytes(&cbb, s, PMBTOKEN_NONCE_SIZE) ||
      !CBB_finish(&cbb, &buf, &len) ||
      !ec_hash_to_curve_p521_xmd_sha512_sswu_draft06(
          group, out, kHashSLabel, sizeof(kHashSLabel), buf, len)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, ERR_R_MALLOC_FAILURE);
    goto err;
  }

  ret = 1;

err:
  OPENSSL_free(buf);
  CBB_cleanup(&cbb);
  return ret;
}

static int pmbtoken_exp0_hash_c(const EC_GROUP *group, EC_SCALAR *out,
                                uint8_t *buf, size_t len) {
  const uint8_t kHashCLabel[] = "PMBTokensV0 HashC";
  return ec_hash_to_scalar_p521_xmd_sha512_draft06(
      group, out, kHashCLabel, sizeof(kHashCLabel), buf, len);
}

// H for PMBTokens v0 was generated with the following Python code.
/*
import hashlib

SEED_H = 'PrivacyPass H'

A = -3
B = 0x051953eb9618e1c9a1f929a21a0b68540eea2da725b99b315f3b8b489918ef109e156193951ec7e937b1652c0bd3bb1bf073573df883d2c34f1ef451fd46b503f00
P = 2**521 - 1

def get_y(x):
  y2 = (x**3 + A*x + B) % P
  y = pow(y2, (P+1)/4, P)
  if (y*y) % P != y2:
    raise ValueError("point not on curve")
  return y

def bit(h,i):
  return (ord(h[i/8]) >> (i%8)) & 1

b = 521
def decode_point(so):
  s = hashlib.sha256(so + '0').digest() + hashlib.sha256(so + '1').digest() + \
      hashlib.sha256(so + '2').digest()

  x = 0
  for i in range(0,b):
    x = x + (long(bit(s,i))<<i)
  if x >= P:
    raise ValueError("x out of range")
  y = get_y(x)
  if y & 1 != bit(s,b-1): y = P-y
  return (x, y)


def gen_point(seed):
  v = hashlib.sha256(seed).digest()
  it = 1
  while True:
    try:
      x,y = decode_point(v)
    except Exception, e:
      print e
      it += 1
      v = hashlib.sha256(v).digest()
      continue
    print "Found in %d iterations:" % it
    print "  x = %d" % x
    print "  y = %d" % y
    print " Encoded (hex): (%x, %x)" % (x, y)
    return (x, y)

if __name__ == "__main__":
  gen_point(SEED_H)
*/
static int pmbtoken_exp0_init_method(PMBTOKEN_METHOD *method) {
  method->group = EC_GROUP_new_by_curve_name(NID_secp521r1);
  if (method->group == NULL) {
    return 0;
  }

  method->hash_t = pmbtoken_exp0_hash_t;
  method->hash_c = pmbtoken_exp0_hash_c;
  method->hash_s = pmbtoken_exp0_hash_s;

  static const uint8_t kH[] = {
      0x04, 0x01, 0xf0, 0xa9, 0xf7, 0x9e, 0xbc, 0x12, 0x6c, 0xef, 0xd1, 0xab,
      0x29, 0x10, 0x03, 0x6f, 0x4e, 0xf5, 0xbd, 0xeb, 0x0f, 0x6b, 0xc0, 0x5c,
      0x0e, 0xce, 0xfe, 0x59, 0x45, 0xd1, 0x3e, 0x25, 0x33, 0x7e, 0x4c, 0xda,
      0x64, 0x53, 0x54, 0x4e, 0xf9, 0x76, 0x0d, 0x6d, 0xc5, 0x39, 0x2a, 0xd4,
      0xce, 0x84, 0x6e, 0x31, 0xc2, 0x86, 0x21, 0xf9, 0x5c, 0x98, 0xb9, 0x3d,
      0x01, 0x74, 0x9f, 0xc5, 0x1e, 0x47, 0x24, 0x00, 0x5c, 0x17, 0x62, 0x51,
      0x7d, 0x32, 0x5e, 0x29, 0xac, 0x52, 0x14, 0x75, 0x6f, 0x36, 0xd9, 0xc7,
      0xfa, 0xbb, 0xa9, 0x3b, 0x9d, 0x70, 0x49, 0x1e, 0xb4, 0x53, 0xbc, 0x55,
      0xea, 0xad, 0x8f, 0x26, 0x1d, 0xe0, 0xbc, 0xf3, 0x50, 0x5c, 0x7e, 0x66,
      0x41, 0xb5, 0x61, 0x70, 0x12, 0x72, 0xac, 0x6a, 0xb0, 0x6e, 0x78, 0x3d,
      0x17, 0x08, 0xe3, 0xdf, 0x3c, 0xff, 0xa6, 0xa0, 0xea, 0x96, 0x67, 0x92,
      0xcd,
  };

  EC_AFFINE h;
  if (!ec_point_from_uncompressed(method->group, &h, kH, sizeof(kH))) {
    return 0;
  }
  ec_affine_to_jacobian(method->group, &method->h, &h);
  return 1;
}

int pmbtoken_exp0_generate_key(CBB *out_private, CBB *out_public) {
  PMBTOKEN_METHOD method;
  if (!pmbtoken_exp0_init_method(&method)) {
    return 0;
  }

  return pmbtoken_generate_key(&method, out_private, out_public);
}

int pmbtoken_exp0_client_key_from_bytes(PMBTOKEN_CLIENT_KEY *key,
                                        const uint8_t *in, size_t len) {
  PMBTOKEN_METHOD method;
  if (!pmbtoken_exp0_init_method(&method)) {
    return 0;
  }
  return pmbtoken_client_key_from_bytes(&method, key, in, len);
}

int pmbtoken_exp0_issuer_key_from_bytes(PMBTOKEN_ISSUER_KEY *key,
                                        const uint8_t *in, size_t len) {
  PMBTOKEN_METHOD method;
  if (!pmbtoken_exp0_init_method(&method)) {
    return 0;
  }
  return pmbtoken_issuer_key_from_bytes(&method, key, in, len);
}

STACK_OF(PMBTOKEN_PRETOKEN) * pmbtoken_exp0_blind(CBB *cbb, size_t count) {
  PMBTOKEN_METHOD method;
  if (!pmbtoken_exp0_init_method(&method)) {
    return NULL;
  }
  return pmbtoken_blind(&method, cbb, count);
}

int pmbtoken_exp0_sign(const PMBTOKEN_ISSUER_KEY *key, CBB *cbb, CBS *cbs,
                       size_t num_requested, size_t num_to_issue,
                       uint8_t private_metadata) {
  PMBTOKEN_METHOD method;
  if (!pmbtoken_exp0_init_method(&method)) {
    return 0;
  }
  return pmbtoken_sign(&method, key, cbb, cbs, num_requested, num_to_issue,
                       private_metadata);
}

STACK_OF(TRUST_TOKEN) *
    pmbtoken_exp0_unblind(const PMBTOKEN_CLIENT_KEY *key,
                          const STACK_OF(PMBTOKEN_PRETOKEN) *pretokens,
                          CBS *cbs, size_t count, uint32_t key_id) {
  PMBTOKEN_METHOD method;
  if (!pmbtoken_exp0_init_method(&method)) {
    return NULL;
  }
  return pmbtoken_unblind(&method, key, pretokens, cbs, count, key_id);
}

int pmbtoken_exp0_read(const PMBTOKEN_ISSUER_KEY *key,
                       uint8_t out_nonce[PMBTOKEN_NONCE_SIZE],
                       uint8_t *out_private_metadata, const uint8_t *token,
                       size_t token_len) {
  PMBTOKEN_METHOD method;
  if (!pmbtoken_exp0_init_method(&method)) {
    return 0;
  }
  return pmbtoken_read(&method, key, out_nonce, out_private_metadata, token,
                       token_len);
}


// PMBTokens experiment v1.

static int pmbtoken_exp1_hash_t(const EC_GROUP *group, EC_RAW_POINT *out,
                                const uint8_t t[PMBTOKEN_NONCE_SIZE]) {
  const uint8_t kHashTLabel[] = "PMBTokens Experiment V1 HashT";
  return ec_hash_to_curve_p384_xmd_sha512_sswu_draft07(
      group, out, kHashTLabel, sizeof(kHashTLabel), t, PMBTOKEN_NONCE_SIZE);
}

static int pmbtoken_exp1_hash_s(const EC_GROUP *group, EC_RAW_POINT *out,
                                const EC_RAW_POINT *t,
                                const uint8_t s[PMBTOKEN_NONCE_SIZE]) {
  const uint8_t kHashSLabel[] = "PMBTokens Experiment V1 HashS";
  int ret = 0;
  CBB cbb;
  uint8_t *buf = NULL;
  size_t len;
  if (!CBB_init(&cbb, 0) ||
      !point_to_cbb(&cbb, group, t) ||
      !CBB_add_bytes(&cbb, s, PMBTOKEN_NONCE_SIZE) ||
      !CBB_finish(&cbb, &buf, &len) ||
      !ec_hash_to_curve_p384_xmd_sha512_sswu_draft07(
          group, out, kHashSLabel, sizeof(kHashSLabel), buf, len)) {
    OPENSSL_PUT_ERROR(TRUST_TOKEN, ERR_R_MALLOC_FAILURE);
    goto err;
  }

  ret = 1;

err:
  OPENSSL_free(buf);
  CBB_cleanup(&cbb);
  return ret;
}

static int pmbtoken_exp1_hash_c(const EC_GROUP *group, EC_SCALAR *out,
                                uint8_t *buf, size_t len) {
  const uint8_t kHashCLabel[] = "PMBTokens Experiment V1 HashC";
  return ec_hash_to_scalar_p384_xmd_sha512_draft07(
      group, out, kHashCLabel, sizeof(kHashCLabel), buf, len);
}

static int pmbtoken_exp1_init_method(PMBTOKEN_METHOD *method) {
  method->group = EC_GROUP_new_by_curve_name(NID_secp384r1);
  if (method->group == NULL) {
    return 0;
  }

  method->hash_t = pmbtoken_exp1_hash_t;
  method->hash_c = pmbtoken_exp1_hash_c;
  method->hash_s = pmbtoken_exp1_hash_s;

  // This is the output of |ec_hash_to_scalar_p384_xmd_sha512_draft07| with DST
  // "PMBTokens Experiment V1 HashH" and message "generator".
  static const uint8_t kH[] = {
      0x04, 0x82, 0xd5, 0x68, 0xf5, 0x39, 0xf6, 0x08, 0x19, 0xa1, 0x75,
      0x9f, 0x98, 0xb5, 0x10, 0xf5, 0x0b, 0x9d, 0x2b, 0xe1, 0x64, 0x4d,
      0x02, 0x76, 0x18, 0x11, 0xf8, 0x2f, 0xd3, 0x33, 0x25, 0x1f, 0x2c,
      0xb8, 0xf6, 0xf1, 0x9e, 0x93, 0x85, 0x79, 0xb3, 0xb7, 0x81, 0xa3,
      0xe6, 0x23, 0xc3, 0x1c, 0xff, 0x03, 0xd9, 0x40, 0x6c, 0xec, 0xe0,
      0x4d, 0xea, 0xdf, 0x9d, 0x94, 0xd1, 0x87, 0xab, 0x27, 0xf7, 0x4f,
      0x53, 0xea, 0xa3, 0x18, 0x72, 0xb9, 0xd1, 0x56, 0xa0, 0x4e, 0x81,
      0xaa, 0xeb, 0x1c, 0x22, 0x6d, 0x39, 0x1c, 0x5e, 0xb1, 0x27, 0xfc,
      0x87, 0xc3, 0x95, 0xd0, 0x13, 0xb7, 0x0b, 0x5c, 0xc7,
  };

  EC_AFFINE h;
  if (!ec_point_from_uncompressed(method->group, &h, kH, sizeof(kH))) {
    return 0;
  }
  ec_affine_to_jacobian(method->group, &method->h, &h);
  return 1;
}

int pmbtoken_exp1_generate_key(CBB *out_private, CBB *out_public) {
  PMBTOKEN_METHOD method;
  if (!pmbtoken_exp1_init_method(&method)) {
    return 0;
  }

  return pmbtoken_generate_key(&method, out_private, out_public);
}

int pmbtoken_exp1_client_key_from_bytes(PMBTOKEN_CLIENT_KEY *key,
                                        const uint8_t *in, size_t len) {
  PMBTOKEN_METHOD method;
  if (!pmbtoken_exp1_init_method(&method)) {
    return 0;
  }
  return pmbtoken_client_key_from_bytes(&method, key, in, len);
}

int pmbtoken_exp1_issuer_key_from_bytes(PMBTOKEN_ISSUER_KEY *key,
                                        const uint8_t *in, size_t len) {
  PMBTOKEN_METHOD method;
  if (!pmbtoken_exp1_init_method(&method)) {
    return 0;
  }
  return pmbtoken_issuer_key_from_bytes(&method, key, in, len);
}

STACK_OF(PMBTOKEN_PRETOKEN) * pmbtoken_exp1_blind(CBB *cbb, size_t count) {
  PMBTOKEN_METHOD method;
  if (!pmbtoken_exp1_init_method(&method)) {
    return NULL;
  }
  return pmbtoken_blind(&method, cbb, count);
}

int pmbtoken_exp1_sign(const PMBTOKEN_ISSUER_KEY *key, CBB *cbb, CBS *cbs,
                       size_t num_requested, size_t num_to_issue,
                       uint8_t private_metadata) {
  PMBTOKEN_METHOD method;
  if (!pmbtoken_exp1_init_method(&method)) {
    return 0;
  }
  return pmbtoken_sign(&method, key, cbb, cbs, num_requested, num_to_issue,
                       private_metadata);
}

STACK_OF(TRUST_TOKEN) *
    pmbtoken_exp1_unblind(const PMBTOKEN_CLIENT_KEY *key,
                          const STACK_OF(PMBTOKEN_PRETOKEN) *pretokens,
                          CBS *cbs, size_t count, uint32_t key_id) {
  PMBTOKEN_METHOD method;
  if (!pmbtoken_exp1_init_method(&method)) {
    return NULL;
  }
  return pmbtoken_unblind(&method, key, pretokens, cbs, count, key_id);
}

int pmbtoken_exp1_read(const PMBTOKEN_ISSUER_KEY *key,
                       uint8_t out_nonce[PMBTOKEN_NONCE_SIZE],
                       uint8_t *out_private_metadata, const uint8_t *token,
                       size_t token_len) {
  PMBTOKEN_METHOD method;
  if (!pmbtoken_exp1_init_method(&method)) {
    return 0;
  }
  return pmbtoken_read(&method, key, out_nonce, out_private_metadata, token,
                       token_len);
}

int pmbtoken_exp1_get_h_for_testing(uint8_t out[97]) {
  PMBTOKEN_METHOD method;
  if (!pmbtoken_exp1_init_method(&method)) {
    return 0;
  }
  EC_AFFINE h;
  return ec_jacobian_to_affine(method.group, &h, &method.h) &&
         ec_point_to_bytes(method.group, &h, POINT_CONVERSION_UNCOMPRESSED, out,
                           97) == 97;
}
