/* C interface of the Verus-verified TPC-H Q21 kernel (src/verus/q21).
 *
 * The kernel is proved (Verus) against a hand-written reference specification
 * of the Q21 catalog SQL (src/verus/q21/src/spec.rs).  Its preconditions are
 * obligations on the caller:
 *   q21v_build   the four arrays are the scans described in build.rs
 *                (build_contract): all supplier rows, all nation rows, the
 *                o_orderkey of every orders row with o_orderstatus = 'F'
 *                (with duplicates), and every lineitem row whose order has an
 *                'F' row (each lineitem once).
 *   q21v_apply   od / ld / sd / nd are exactly one committed transaction's
 *                changes to orders / lineitem / supplier / nation (signed,
 *                projected rows), and scan_rows are the post-transaction
 *                lineitem rows of scan_keys (maint.rs scan_contract).
 * States are immutable; q21v_apply returns a new state.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct q21v_state q21v_state;
typedef struct q21v_result q21v_result;

typedef struct {
  int64_t orderkey;
  int64_t suppkey;
  int32_t commitdate;
  int32_t receiptdate;
} q21v_li_row;

typedef struct {
  q21v_li_row row;
  int64_t sign; /* +1 insert, -1 delete */
} q21v_li_delta;

typedef struct {
  int64_t orderkey;
  uint8_t status_is_f; /* o_orderstatus = 'F' */
  int64_t sign;
} q21v_ord_delta;

q21v_state *q21v_build(size_t n_sup, const int64_t *sup_sk, const int32_t *sup_nk,
                       const uint8_t *sup_names, const size_t *sup_off,
                       size_t n_nat, const int32_t *nat_nk,
                       const uint8_t *nat_names, const size_t *nat_off,
                       size_t n_f, const int64_t *fkeys,
                       size_t n_li, const q21v_li_row *lis);
void q21v_free(q21v_state *s);

size_t q21v_needs_scan(const q21v_state *s, size_t n_od, const q21v_ord_delta *od,
                       int64_t *out);
q21v_state *q21v_apply(const q21v_state *s, size_t n_od, const q21v_ord_delta *od,
                       size_t n_ld, const q21v_li_delta *ld,
                       size_t n_sd, const int64_t *sd_sk, const int32_t *sd_nk,
                       const uint8_t *sd_names, const size_t *sd_off,
                       const int64_t *sd_sign,
                       size_t n_nd, const int32_t *nd_nk, const uint8_t *nd_names,
                       const size_t *nd_off, const int64_t *nd_sign, size_t n_sk,
                       const int64_t *scan_keys, size_t n_sr,
                       const q21v_li_row *scan_rows, int32_t *err);

q21v_result *q21v_execute(const q21v_state *s, const uint8_t *nation, size_t len);
size_t q21v_result_len(const q21v_result *r);
const uint8_t *q21v_result_name(const q21v_result *r, size_t i, size_t *len);
int64_t q21v_result_count(const q21v_result *r, size_t i);
void q21v_result_free(q21v_result *r);

#ifdef __cplusplus
}
#endif
