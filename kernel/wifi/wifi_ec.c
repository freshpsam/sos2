/* hostap's arithmetic interface over the unmodified Mbed TLS P-256 backend. */
#include "utils/includes.h"
#include "utils/common.h"
#include "crypto/crypto.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ecp.h"
#include "wifi/port.h"
#include <limits.h>
#define MPI(p)    ((mbedtls_mpi *)(p))
#define CMPI(p)   ((const mbedtls_mpi *)(p))
#define POINT(p)  ((mbedtls_ecp_point *)(p))
#define CPOINT(p) ((const mbedtls_ecp_point *)(p))

struct crypto_ec {
    mbedtls_ecp_group group;
    mbedtls_mpi a;
};

struct crypto_bignum *crypto_bignum_init(void) {
    mbedtls_mpi *p = os_zalloc(sizeof(*p));
    if (p) mbedtls_mpi_init(p);
    return (void *)p;
}

struct crypto_bignum *crypto_bignum_init_set(const u8 *b, size_t n) {
    struct crypto_bignum *p = crypto_bignum_init();
    if (p && mbedtls_mpi_read_binary(MPI(p), b, n)) {
        crypto_bignum_deinit(p, 1);
        p = 0;
    }
    return p;
}

struct crypto_bignum *crypto_bignum_init_uint(unsigned value) {
    u8 b[4];
    WPA_PUT_BE32(b, value);
    return crypto_bignum_init_set(b, 4);
}

void crypto_bignum_deinit(struct crypto_bignum *p, int clear) {
    (void)clear;
    if (p) {
        mbedtls_mpi_free(MPI(p));
        os_free(p);
    }
}

int crypto_bignum_to_bin(const struct crypto_bignum *p, u8 *out, size_t capacity, size_t pad) {
    size_t n = mbedtls_mpi_size(CMPI(p));
    if (n < pad) n = pad;
    if (n > capacity || n > INT_MAX || mbedtls_mpi_write_binary(CMPI(p), out, n)) return -1;
    return (int)n;
}

int crypto_bignum_rand(struct crypto_bignum *r, const struct crypto_bignum *m) {
    return mbedtls_mpi_random(MPI(r), 0, CMPI(m), wifi_crypto_random, 0);
}

int crypto_bignum_add(const struct crypto_bignum *a, const struct crypto_bignum *b,
                      struct crypto_bignum *r) {
    return mbedtls_mpi_add_mpi(MPI(r), CMPI(a), CMPI(b));
}

int crypto_bignum_sub(const struct crypto_bignum *a, const struct crypto_bignum *b,
                      struct crypto_bignum *r) {
    return mbedtls_mpi_sub_mpi(MPI(r), CMPI(a), CMPI(b));
}

int crypto_bignum_div(const struct crypto_bignum *a, const struct crypto_bignum *b,
                      struct crypto_bignum *r) {
    return mbedtls_mpi_div_mpi(MPI(r), 0, CMPI(a), CMPI(b));
}

int crypto_bignum_mod(const struct crypto_bignum *a, const struct crypto_bignum *m,
                      struct crypto_bignum *r) {
    return mbedtls_mpi_mod_mpi(MPI(r), CMPI(a), CMPI(m));
}

int crypto_bignum_exptmod(const struct crypto_bignum *a, const struct crypto_bignum *b,
                          const struct crypto_bignum *m, struct crypto_bignum *r) {
    /* hostap permits output == exponent; Mbed TLS explicitly forbids it. */
    mbedtls_mpi t;
    mbedtls_mpi_init(&t);
    int result = mbedtls_mpi_exp_mod(&t, CMPI(a), CMPI(b), CMPI(m), 0);
    if (!result) mbedtls_mpi_swap(MPI(r), &t);
    mbedtls_mpi_free(&t);
    return result;
}

int crypto_bignum_inverse(const struct crypto_bignum *a, const struct crypto_bignum *m,
                          struct crypto_bignum *r) {
    return mbedtls_mpi_inv_mod(MPI(r), CMPI(a), CMPI(m));
}

int crypto_bignum_addmod(const struct crypto_bignum *a, const struct crypto_bignum *b,
                         const struct crypto_bignum *m, struct crypto_bignum *r) {
    int result = mbedtls_mpi_add_mpi(MPI(r), CMPI(a), CMPI(b));
    return result ? result : mbedtls_mpi_mod_mpi(MPI(r), MPI(r), CMPI(m));
}

int crypto_bignum_mulmod(const struct crypto_bignum *a, const struct crypto_bignum *b,
                         const struct crypto_bignum *m, struct crypto_bignum *r) {
    mbedtls_mpi t;
    mbedtls_mpi_init(&t);
    int result = mbedtls_mpi_mul_mpi(&t, CMPI(a), CMPI(b));
    if (!result) result = mbedtls_mpi_mod_mpi(MPI(r), &t, CMPI(m));
    mbedtls_mpi_free(&t);
    return result;
}

int crypto_bignum_sqrmod(const struct crypto_bignum *a, const struct crypto_bignum *m,
                         struct crypto_bignum *r) {
    return crypto_bignum_mulmod(a, a, m, r);
}

int crypto_bignum_rshift(const struct crypto_bignum *a, int bits, struct crypto_bignum *r) {
    if (bits < 0) return -1;
    int result = mbedtls_mpi_copy(MPI(r), CMPI(a));
    return result ? result : mbedtls_mpi_shift_r(MPI(r), bits);
}

int crypto_bignum_cmp(const struct crypto_bignum *a, const struct crypto_bignum *b) {
    return mbedtls_mpi_cmp_mpi(CMPI(a), CMPI(b));
}

int crypto_bignum_is_zero(const struct crypto_bignum *a) {
    return !mbedtls_mpi_cmp_int(CMPI(a), 0);
}

int crypto_bignum_is_one(const struct crypto_bignum *a) { return !mbedtls_mpi_cmp_int(CMPI(a), 1); }

int crypto_bignum_is_odd(const struct crypto_bignum *a) { return mbedtls_mpi_get_bit(CMPI(a), 0); }

int crypto_bignum_legendre(const struct crypto_bignum *a, const struct crypto_bignum *p) {
    mbedtls_mpi e, r;
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&r);
    int result = -2;
    if (mbedtls_mpi_sub_int(&e, CMPI(p), 1) || mbedtls_mpi_shift_r(&e, 1) ||
        mbedtls_mpi_exp_mod(&r, CMPI(a), &e, CMPI(p), 0))
        goto done;
    if (!mbedtls_mpi_cmp_int(&r, 0)) result = 0;
    else if (!mbedtls_mpi_cmp_int(&r, 1)) result = 1;
    else if (!mbedtls_mpi_sub_int(&e, CMPI(p), 1) && !mbedtls_mpi_cmp_mpi(&e, &r)) result = -1;
done:
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&r);
    return result;
}

struct crypto_ec *crypto_ec_init(int group) {
    if (group != 19) return 0; /* Explicit P-256 group policy; no weaker fallback. */
    struct crypto_ec *e = os_zalloc(sizeof(*e));
    if (!e) return 0;
    mbedtls_ecp_group_init(&e->group);
    mbedtls_mpi_init(&e->a);
    if (mbedtls_ecp_group_load(&e->group, MBEDTLS_ECP_DP_SECP256R1) ||
        mbedtls_mpi_sub_int(&e->a, &e->group.P, 3)) {
        crypto_ec_deinit(e);
        return 0;
    }
    return e;
}

void crypto_ec_deinit(struct crypto_ec *e) {
    if (e) {
        mbedtls_ecp_group_free(&e->group);
        mbedtls_mpi_free(&e->a);
        os_free(e);
    }
}

size_t crypto_ec_prime_len(struct crypto_ec *e) { return mbedtls_mpi_size(&e->group.P); }

size_t crypto_ec_prime_len_bits(struct crypto_ec *e) { return mbedtls_mpi_bitlen(&e->group.P); }

size_t crypto_ec_order_len(struct crypto_ec *e) { return mbedtls_mpi_size(&e->group.N); }

const struct crypto_bignum *crypto_ec_get_prime(struct crypto_ec *e) {
    return (const void *)&e->group.P;
}

const struct crypto_bignum *crypto_ec_get_order(struct crypto_ec *e) {
    return (const void *)&e->group.N;
}

const struct crypto_bignum *crypto_ec_get_a(struct crypto_ec *e) { return (const void *)&e->a; }

const struct crypto_bignum *crypto_ec_get_b(struct crypto_ec *e) {
    return (const void *)&e->group.B;
}

struct crypto_ec_point *crypto_ec_point_init(struct crypto_ec *e) {
    (void)e;
    mbedtls_ecp_point *p = os_zalloc(sizeof(*p));
    if (p) mbedtls_ecp_point_init(p);
    return (void *)p;
}

void crypto_ec_point_deinit(struct crypto_ec_point *p, int clear) {
    (void)clear;
    if (p) {
        mbedtls_ecp_point_free(POINT(p));
        os_free(p);
    }
}

int crypto_ec_point_to_bin(struct crypto_ec *e, const struct crypto_ec_point *p, u8 *x, u8 *y) {
    size_t n = crypto_ec_prime_len(e);
    if (x && mbedtls_mpi_write_binary(&CPOINT(p)->MBEDTLS_PRIVATE(X), x, n)) return -1;
    return y ? mbedtls_mpi_write_binary(&CPOINT(p)->MBEDTLS_PRIVATE(Y), y, n) : 0;
}

struct crypto_ec_point *crypto_ec_point_from_bin(struct crypto_ec *e, const u8 *data) {
    struct crypto_ec_point *p = crypto_ec_point_init(e);
    size_t n = crypto_ec_prime_len(e);
    if (!p) return 0;
    if (mbedtls_mpi_read_binary(&POINT(p)->MBEDTLS_PRIVATE(X), data, n) ||
        mbedtls_mpi_read_binary(&POINT(p)->MBEDTLS_PRIVATE(Y), data + n, n) ||
        mbedtls_mpi_lset(&POINT(p)->MBEDTLS_PRIVATE(Z), 1) ||
        mbedtls_ecp_check_pubkey(&e->group, POINT(p))) {
        crypto_ec_point_deinit(p, 1);
        return 0;
    }
    return p;
}

int crypto_ec_point_add(struct crypto_ec *e, const struct crypto_ec_point *a,
                        const struct crypto_ec_point *b, struct crypto_ec_point *r) {
    mbedtls_mpi one;
    mbedtls_mpi_init(&one);
    int result = mbedtls_mpi_lset(&one, 1);
    if (!result) result = mbedtls_ecp_muladd(&e->group, POINT(r), &one, CPOINT(a), &one, CPOINT(b));
    mbedtls_mpi_free(&one);
    return result;
}

int crypto_ec_point_mul(struct crypto_ec *e, const struct crypto_ec_point *p,
                        const struct crypto_bignum *b, struct crypto_ec_point *r) {
    return mbedtls_ecp_mul(&e->group, POINT(r), CMPI(b), CPOINT(p), wifi_crypto_random, 0);
}

int crypto_ec_point_invert(struct crypto_ec *e, struct crypto_ec_point *p) {
    return mbedtls_mpi_cmp_int(&POINT(p)->MBEDTLS_PRIVATE(Y), 0)
               ? mbedtls_mpi_sub_mpi(&POINT(p)->MBEDTLS_PRIVATE(Y), &e->group.P,
                                     &POINT(p)->MBEDTLS_PRIVATE(Y))
               : 0;
}

struct crypto_bignum *crypto_ec_point_compute_y_sqr(struct crypto_ec *e,
                                                    const struct crypto_bignum *x) {
    struct crypto_bignum *r = crypto_bignum_init(), *t = crypto_bignum_init();
    const struct crypto_bignum *p = crypto_ec_get_prime(e);
    if (!r || !t || crypto_bignum_sqrmod(x, p, r) || crypto_bignum_mulmod(x, r, p, r) ||
        crypto_bignum_mulmod(x, crypto_ec_get_a(e), p, t) || crypto_bignum_addmod(r, t, p, r) ||
        crypto_bignum_addmod(r, crypto_ec_get_b(e), p, r)) {
        crypto_bignum_deinit(r, 1);
        r = 0;
    }
    crypto_bignum_deinit(t, 1);
    return r;
}

int crypto_ec_point_is_at_infinity(struct crypto_ec *e, const struct crypto_ec_point *p) {
    (void)e;
    return mbedtls_ecp_is_zero((mbedtls_ecp_point *)p);
}

int crypto_ec_point_is_on_curve(struct crypto_ec *e, const struct crypto_ec_point *p) {
    return !mbedtls_ecp_check_pubkey(&e->group, CPOINT(p));
}

int crypto_ec_point_cmp(const struct crypto_ec *e, const struct crypto_ec_point *a,
                        const struct crypto_ec_point *b) {
    (void)e;
    return mbedtls_ecp_point_cmp(CPOINT(a), CPOINT(b));
}
