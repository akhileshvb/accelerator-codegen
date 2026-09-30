/* C interface of the Verus-verified stateless TPC-H Q1 kernel (src/verus/q1).
 *
 * The kernel is proved (Verus) against a hand-written reference specification
 * of the Q1 catalog SQL (src/verus/q1/src/spec.rs): after consuming batches
 * B1..Bk, q1v_finish returns exactly the Q1 groups of B1 ++ ... ++ Bk
 * (every non-empty (l_returnflag, l_linestatus) group, in ORDER BY order,
 * with exact sums).  The caller's obligation (trusted) is that the batches are
 * the rows of lineitem: every row with l_shipdate <= cutoff exactly once,
 * plus any number of rows with l_shipdate > cutoff; columns decoded exactly
 * (DECIMAL(15,2) as integer hundredths, dates as day numbers in the same
 * encoding as `cutoff`, CHAR(1) flags as their single byte).
 *
 * Sums: sum_qty, sum_base_price and sum_disc are at scale 2, sum_disc_price
 * at scale 4 and sum_charge at scale 6.  AVG columns are sum / count, which
 * the host formats per engine.
 *
 * q1v_consume / q1v_merge return 0 when the kernel refuses (a qualifying row
 * whose l_returnflag / l_linestatus are not both in 'A'..'Z', or one
 * with |quantity| or |extendedprice| >= 2^36 or |discount| or |tax| >= 2^7
 * hundredths, or more than 2^40 rows); the state is then poisoned and the
 * host must answer with SQL.  q1v_consume_simple is the unblocked kernel
 * (i128 sums updated per row) with the wider domain 2^46 / 2^16.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct q1v_state q1v_state;
typedef struct q1v_result q1v_result;

/* Layout of the kernel's #[repr(C)] OutGroup. */
typedef struct {
  __int128 sum_qty;
  __int128 sum_base_price;
  __int128 sum_disc_price;
  __int128 sum_charge;
  __int128 sum_disc;
  int64_t count;
  uint8_t returnflag;
  uint8_t linestatus;
} q1v_group;

q1v_state *q1v_new(int32_t cutoff);
void q1v_free(q1v_state *s);
uint8_t q1v_consume(q1v_state *s, size_t n, const int32_t *shipdate,
                    const int64_t *quantity, const int64_t *extendedprice,
                    const int64_t *discount, const int64_t *tax,
                    const uint8_t *returnflag, const uint8_t *linestatus);
uint8_t q1v_consume_simple(q1v_state *s, size_t n, const int32_t *shipdate,
                           const int64_t *quantity, const int64_t *extendedprice,
                           const int64_t *discount, const int64_t *tax,
                           const uint8_t *returnflag, const uint8_t *linestatus);
uint8_t q1v_merge(q1v_state *dst, const q1v_state *src);
q1v_result *q1v_finish(const q1v_state *s);
size_t q1v_result_len(const q1v_result *r);
const q1v_group *q1v_result_get(const q1v_result *r, size_t i);
void q1v_result_free(q1v_result *r);

#ifdef __cplusplus
}

static_assert(sizeof(q1v_group) == 96, "q1v_group must match the kernel's OutGroup");

#include <memory>
#include <stdexcept>
#include <vector>

namespace accelerator_codegen::verus_q1 {

/// Column buffers for one batch handed to the kernel.
struct Q1Batch {
  std::vector<int32_t> shipdate;
  std::vector<int64_t> quantity, extendedprice, discount, tax;
  std::vector<uint8_t> returnflag, linestatus;
  size_t n = 0;

  explicit Q1Batch(size_t cap = 2048) { Reserve(cap); }
  void Reserve(size_t cap) {
    shipdate.resize(cap);
    quantity.resize(cap);
    extendedprice.resize(cap);
    discount.resize(cap);
    tax.resize(cap);
    returnflag.resize(cap);
    linestatus.resize(cap);
  }
  size_t Capacity() const { return shipdate.size(); }
};

/// Owning handle around a kernel state.
class Q1Kernel {
public:
  explicit Q1Kernel(int32_t cutoff, bool simple = false)
      : state_(q1v_new(cutoff)), simple_(simple) {}
  ~Q1Kernel() { q1v_free(state_); }
  Q1Kernel(const Q1Kernel &) = delete;
  Q1Kernel &operator=(const Q1Kernel &) = delete;

  /// False: the kernel refused (out of its numeric domain).
  bool Consume(const Q1Batch &b) {
    if (b.n == 0) return ok_;
    auto *fn = simple_ ? q1v_consume_simple : q1v_consume;
    ok_ = ok_ && fn(state_, b.n, b.shipdate.data(), b.quantity.data(),
                    b.extendedprice.data(), b.discount.data(), b.tax.data(),
                    b.returnflag.data(), b.linestatus.data()) != 0;
    return ok_;
  }
  bool Merge(const Q1Kernel &other) {
    ok_ = ok_ && q1v_merge(state_, other.state_) != 0;
    return ok_;
  }
  bool Ok() const { return ok_; }

  /// The groups in ORDER BY order; empty optional-like (false) on refusal.
  bool Finish(std::vector<q1v_group> &out) const {
    out.clear();
    q1v_result *r = q1v_finish(state_);
    if (!r) return false;
    const size_t n = q1v_result_len(r);
    out.reserve(n);
    for (size_t i = 0; i < n; i++) out.push_back(*q1v_result_get(r, i));
    q1v_result_free(r);
    return true;
  }

private:
  q1v_state *state_;
  bool simple_ = false;
  bool ok_ = true;
};

} // namespace accelerator_codegen::verus_q1
#endif
