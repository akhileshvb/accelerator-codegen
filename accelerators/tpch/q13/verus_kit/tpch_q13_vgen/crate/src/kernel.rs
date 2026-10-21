#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::sqlfns::like_match;
use verus_prooflib::like_match_exec;
use std::sync::Arc;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::aggregate::*;
use crate::facts::*;
use crate::recs::*;
use crate::dense::*;
use crate::general::*;

verus! {

pub open spec fn customer_count(db: Db, key: int) -> int {
    sum(db.customer, |row: CustomerRow| ind(row.c_custkey == key))
}

/// Orders of customer `key` whose comment does NOT match the LIKE pattern `p`.
pub open spec fn order_count(db: Db, p: Seq<u8>, key: int) -> int {
    sum(db.orders, |row: OrdersRow| ind(c_orders_on_orders(
        db, params(p), CustomerRow { c_custkey: key }, row)))
}

pub open spec fn histogram_count(db: Db, p: Seq<u8>, bucket: int) -> int {
    sum(customer_keys(db), |key: int| contribution(db, p, key, bucket))
}

/// The histogram of the 16 tracked patterns, 1024 buckets each, laid end to end.
pub open spec fn hist_at(h: Seq<i64>, j: int, b: int) -> int {
    if 0 <= j < 16 && 0 <= b < 1024 && j * 1024 + b < h.len() { h[j * 1024 + b] as int } else { 0 }
}

/// The accelerator state, for the 16 TPC-H '%word1%word2%' patterns at once: one 17-field record
/// per customer key (multiplicity, then per pattern the customer's non-matching order count),
/// in copy-on-write pages; one 1024-bucket histogram per pattern.  A read whose pattern is not
/// tracked declines.
pub struct Fast {
    recs: Recs,
    hist: Arc<Vec<i64>>,
    pats: Arc<Vec<Vec<u8>>>,
    db: Ghost<Db>,
}

impl Fast {
    pub closed spec fn db(self) -> Db { self.db@ }

    pub closed spec fn pat(self, j: int) -> Seq<u8> { (*self.pats)@[j]@ }

    closed spec fn ocnt(self, key: int, j: int) -> int { self.recs.cell(key, 1 + j) }

    #[verifier::type_invariant]
    spec fn wf(self) -> bool {
        &&& self.recs.wf()
        &&& (*self.pats)@.len() == 16
        &&& (*self.hist)@.len() == 16384
        &&& forall|key: int| #[trigger] self.recs.cell(key, 0) == customer_count(self.db@, key)
        &&& forall|key: int, j: int| 0 <= j < 16 ==> #[trigger] self.ocnt(key, j)
            == order_count(self.db@, (*self.pats)@[j]@, key)
        &&& forall|j: int, b: int| 0 <= j < 16 ==> #[trigger] hist_at((*self.hist)@, j, b)
            == histogram_count(self.db@, (*self.pats)@[j]@, b)
    }
}

fn bytes_eq(a: &[u8], b: &[u8]) -> (r: bool)
    ensures r == (a@ == b@),
{
    if a.len() != b.len() { return false; }
    let mut i = 0usize;
    while i < a.len()
        invariant i <= a.len(), a.len() == b.len(), a@.len() == b@.len(),
            forall|k: int| 0 <= k < i ==> a@[k] == b@[k],
        decreases a.len() - i,
    {
        if a[i] != b[i] { return false; }
        i += 1;
    }
    proof { assert(a@ =~= b@); }
    true
}
/// The i-th first word (special, pending, unusual, express).
fn first_word(i: usize) -> Vec<u8> {
    if i == 0 { vec![b's', b'p', b'e', b'c', b'i', b'a', b'l'] }
    else if i == 1 { vec![b'p', b'e', b'n', b'd', b'i', b'n', b'g'] }
    else if i == 2 { vec![b'u', b'n', b'u', b's', b'u', b'a', b'l'] }
    else { vec![b'e', b'x', b'p', b'r', b'e', b's', b's'] }
}

/// The i-th second word (packages, requests, accounts, deposits).
fn second_word(i: usize) -> Vec<u8> {
    if i == 0 { vec![b'p', b'a', b'c', b'k', b'a', b'g', b'e', b's'] }
    else if i == 1 { vec![b'r', b'e', b'q', b'u', b'e', b's', b't', b's'] }
    else if i == 2 { vec![b'a', b'c', b'c', b'o', b'u', b'n', b't', b's'] }
    else { vec![b'd', b'e', b'p', b'o', b's', b'i', b't', b's'] }
}

/// The j-th tracked pattern: '%' || first_word(j / 4) || '%' || second_word(j % 4) || '%'.
/// Which patterns are tracked affects only which reads decline, not correctness.
fn tracked_pattern(j: usize) -> Vec<u8>
    requires j < 16,
{
    let mut v: Vec<u8> = Vec::new();
    v.push(b'%');
    let mut w = first_word(j / 4);
    v.append(&mut w);
    v.push(b'%');
    let mut w2 = second_word(j % 4);
    v.append(&mut w2);
    v.push(b'%');
    v
}
fn tracked_patterns() -> (r: Vec<Vec<u8>>)
    ensures r@.len() == 16,
{
    let mut v: Vec<Vec<u8>> = Vec::new();
    let mut j = 0usize;
    while j < 16
        invariant j <= 16, v@.len() == j as int,
        decreases 16 - j,
    {
        v.push(tracked_pattern(j));
        j += 1;
    }
    v
}

/// Σ over the first n scanned orders of [custkey = key and the comment does not match p].
pub open spec fn ocp_f(p: Seq<u8>, key: int) -> spec_fn((int, Seq<u8>)) -> int {
    |row: (int, Seq<u8>)| ind(row.0 == key && !like_match(row.1, p))
}

pub open spec fn ocp(rows: Seq<(int, Seq<u8>)>, p: Seq<u8>, key: int, n: int) -> int {
    sum(rows.take(n), ocp_f(p, key))
}

/// Σ over the first n scanned customers of [custkey = key].
pub open spec fn ccp_f(key: int) -> spec_fn(CustomerRow) -> int {
    |row: CustomerRow| ind(row.c_custkey == key)
}

pub open spec fn ccp(rows: Seq<CustomerRow>, key: int, n: int) -> int {
    sum(rows.take(n), ccp_f(key))
}

pub proof fn lemma_ocp_step(rows: Seq<(int, Seq<u8>)>, p: Seq<u8>, key: int, n: int)
    requires 0 <= n < rows.len(),
    ensures ocp(rows, p, key, n + 1) == ocp(rows, p, key, n) + ind(rows[n].0 == key && !like_match(rows[n].1, p)),
{
    lemma_sum_take(rows, n, ocp_f(p, key));
}

pub proof fn lemma_ccp_step(rows: Seq<CustomerRow>, key: int, n: int)
    requires 0 <= n < rows.len(),
    ensures ccp(rows, key, n + 1) == ccp(rows, key, n) + ind(rows[n].c_custkey == key),
{
    lemma_sum_take(rows, n, ccp_f(key));
}

fn read_rec(recs: &Recs, key: i64) -> (v: Vec<u16>)
    requires recs.wf(), key_ok(key as int),
    ensures v@.len() == 17, forall|f: int| 0 <= f < 17 ==> (#[trigger] v@[f]) as int == recs.cell(key as int, f),
{
    let mut v: Vec<u16> = Vec::new();
    let mut f = 0usize;
    while f < 17
        invariant f <= 17, v@.len() == f as int, recs.wf(), key_ok(key as int),
            forall|g: int| 0 <= g < f ==> (#[trigger] v@[g]) as int == recs.cell(key as int, g),
        decreases 17 - f,
    {
        v.push(recs.get(key, f));
        f += 1;
    }
    v
}

pub fn fast_build(
    customers: &[CustomerScan],
    orders: &Vec<OrderScan>,
    Ghost(db): Ghost<Db>,
) -> (result: Option<Fast>)
    requires scan_customers_ok(db, customers@),
        scan_orders_ok(db, orders@),
    ensures result matches Some(state) ==> state.db() == db,
{
    // the key range of the dense window; every key must be a SQL INTEGER
    let mut kmax = 0usize;
    let mut i = 0usize;
    while i < customers.len()
        invariant i <= customers.len(), kmax <= 67108864,
            forall|x: int| 0 <= x < i ==> key_ok((#[trigger] customers@[x]).c_custkey as int),
        decreases customers.len() - i,
    {
        let key = customers[i].c_custkey;
        if key >= 0 && key < 67108864 && (key as usize) + 1 > kmax { kmax = key as usize + 1; }
        i += 1;
    }
    i = 0;
    while i < orders.len()
        invariant i <= orders.len(), kmax <= 67108864,
            forall|x: int| 0 <= x < customers.len() ==> key_ok((#[trigger] customers@[x]).c_custkey as int),
            forall|x: int| 0 <= x < i ==> key_ok((#[trigger] orders@[x]).o_custkey as int),
        decreases orders.len() - i,
    {
        let key = orders[i].o_custkey;
        if key >= 0 && key < 67108864 && (key as usize) + 1 > kmax { kmax = key as usize + 1; }
        i += 1;
    }
    let pats = tracked_patterns();
    let ghost cview = customer_scan_seq_view(customers@);
    let ghost view = order_scan_seq_view(orders@);
    let mut flat: Vec<u16> = vec![0u16; kmax * 17];
    // dense customer multiplicities
    let mut index = 0usize;
    while index < customers.len()
        invariant index <= customers.len(), flat@.len() == kmax * 17, kmax <= 67108864,
            cview == customer_scan_seq_view(customers@),
            forall|key: int| 0 <= key < kmax ==> #[trigger] flat_at(flat@, kmax as int, key, 0)
                == ccp(cview, key, index as int),
            forall|key: int, f: int| 1 <= f < 17 ==> #[trigger] flat_at(flat@, kmax as int, key, f) == 0,
        decreases customers.len() - index,
    {
        let key64 = customers[index].c_custkey;
        let ghost old = flat@;
        if key64 >= 0 && key64 < kmax as i64 {
            let key = key64 as usize;
            let idx = key * 17;
            proof { lemma_flat_in_range(flat@, kmax as int, key as int, 0); }
            let v = flat[idx];
            if v == 65535 { return None; }
            flat.set(idx, v + 1);
            proof {
                lemma_flat_set(old, kmax as int, key as int, 0, (v + 1) as u16);
                assert(flat_at(old, kmax as int, key as int, 0) == v as int);
            }
        }
        proof {
            assert(cview[index as int] == customer_scan_view(customers@[index as int]));
            assert(customer_scan_view(customers@[index as int]).c_custkey == key64 as int);
            assert forall|k2: int| 0 <= k2 < kmax implies #[trigger] flat_at(flat@, kmax as int, k2, 0)
                == ccp(cview, k2, index as int + 1) by {
                lemma_ccp_step(cview, k2, index as int);
            }
        }
        index += 1;
    }
    // dense per-pattern non-matching order counts
    index = 0;
    while index < orders.len()
        invariant index <= orders.len(), flat@.len() == kmax * 17, kmax <= 67108864,
            pats@.len() == 16, view == order_scan_seq_view(orders@),
            forall|key: int| 0 <= key < kmax ==> #[trigger] flat_at(flat@, kmax as int, key, 0)
                == ccp(cview, key, customers@.len() as int),
            forall|key: int, jj: int| 0 <= key < kmax && 0 <= jj < 16 ==> #[trigger] flat_at(flat@, kmax as int, key, 1 + jj)
                == ocp(view, pats@[jj]@, key, index as int),
            forall|key: int, f: int| !(0 <= key < kmax) ==> #[trigger] flat_at(flat@, kmax as int, key, f) == 0,
        decreases orders.len() - index,
    {
        let key64 = orders[index].o_custkey;
        if key64 >= 0 && key64 < kmax as i64 {
            let key = key64 as usize;
            proof { assert(key as int == orders@[index as int].o_custkey as int); }
            let mut j = 0usize;
            while j < 16
                invariant j <= 16, index < orders.len(), flat@.len() == kmax * 17, kmax <= 67108864,
                    pats@.len() == 16, view == order_scan_seq_view(orders@),
                    key as int == orders@[index as int].o_custkey, key < kmax,
                    forall|key: int| 0 <= key < kmax ==> #[trigger] flat_at(flat@, kmax as int, key, 0)
                        == ccp(cview, key, customers@.len() as int),
                    forall|key: int, jj: int| 0 <= key < kmax && 0 <= jj < 16 ==> #[trigger] flat_at(flat@, kmax as int, key, 1 + jj)
                        == ocp(view, pats@[jj]@, key, if jj < j as int { index as int + 1 } else { index as int }),
                    forall|key: int, f: int| !(0 <= key < kmax) ==> #[trigger] flat_at(flat@, kmax as int, key, f) == 0,
                decreases 16 - j,
            {
                let hit = like_match_exec(orders[index].o_comment.as_slice(), pats[j].as_slice());
                let ghost old = flat@;
                if !hit {
                    let idx = key * 17 + 1 + j;
                    proof { lemma_flat_in_range(flat@, kmax as int, key as int, 1 + j as int); }
                    let v = flat[idx];
                    if v == 65535 { return None; }
                    flat.set(idx, v + 1);
                    proof { lemma_flat_set(old, kmax as int, key as int, 1 + j as int, (v + 1) as u16); }
                }
                proof {
                    assert(view[index as int] == order_scan_view(orders@[index as int]));
                    assert forall|k2: int, jj: int| 0 <= k2 < kmax && 0 <= jj < 16 implies #[trigger] flat_at(flat@, kmax as int, k2, 1 + jj)
                        == ocp(view, pats@[jj]@, k2, if jj < (j + 1) as int { index as int + 1 } else { index as int }) by {
                        if jj == j as int {
                            lemma_ocp_step(view, pats@[jj]@, k2, index as int);
                            assert(view[index as int].0 == key as int);
                            assert(view[index as int].1 == orders@[index as int].o_comment@);
                            assert(hit == like_match(view[index as int].1, pats@[jj]@));
                            if !hit {
                                assert(flat_at(old, kmax as int, key as int, 1 + jj) == ocp(view, pats@[jj]@, key as int, index as int));
                            }
                        }
                    }
                }
                j += 1;
            }
        } else {
            proof {
                assert(view[index as int] == order_scan_view(orders@[index as int]));
                assert forall|k2: int, jj: int| 0 <= k2 < kmax && 0 <= jj < 16 implies #[trigger] flat_at(flat@, kmax as int, k2, 1 + jj)
                    == ocp(view, pats@[jj]@, k2, index as int + 1) by {
                    lemma_ocp_step(view, pats@[jj]@, k2, index as int);
                    assert(view[index as int].0 == key64 as int);
                }
            }
        }
        index += 1;
    }
    proof {
        assert(cview.take(customers@.len() as int) =~= cview);
        assert(view.take(orders@.len() as int) =~= view);
    }
    let mut recs = Recs::from_flat(&flat, kmax);
    proof {
        assert forall|key: int| 0 <= key < kmax implies #[trigger] recs.cell(key, 0) == ccp(cview, key, customers@.len() as int) by {
            lemma_flat_in_range(flat@, kmax as int, key, 0);
            assert(flat_at(flat@, kmax as int, key, 0) == ccp(cview, key, customers@.len() as int));
        }
        assert forall|key: int, jj: int| 0 <= key < kmax && 0 <= jj < 16 implies #[trigger] recs.cell(key, 1 + jj)
            == ocp(view, pats@[jj]@, key, orders@.len() as int) by {
            lemma_flat_in_range(flat@, kmax as int, key, 1 + jj);
            assert(flat_at(flat@, kmax as int, key, 1 + jj) == ocp(view, pats@[jj]@, key, orders@.len() as int));
        }
        assert forall|key: int, f: int| !(0 <= key < kmax) implies #[trigger] recs.cell(key, f) == 0 by {}
    }
    // customers outside the dense window
    index = 0;
    while index < customers.len()
        invariant index <= customers.len(), recs.wf(), kmax <= 67108864, cview == customer_scan_seq_view(customers@),
            forall|x: int| 0 <= x < customers.len() ==> key_ok((#[trigger] customers@[x]).c_custkey as int),
            forall|key: int| #[trigger] recs.cell(key, 0)
                == if 0 <= key < kmax { ccp(cview, key, customers@.len() as int) } else { ccp(cview, key, index as int) },
            forall|key: int, jj: int| 0 <= jj < 16 ==> #[trigger] recs.cell(key, 1 + jj)
                == if 0 <= key < kmax { ocp(view, pats@[jj]@, key, orders@.len() as int) } else { 0 },
            pats@.len() == 16,
        decreases customers.len() - index,
    {
        let key64 = customers[index].c_custkey;
        proof { assert(key_ok(customers@[index as int].c_custkey as int)); }
        if !(key64 >= 0 && key64 < kmax as i64) {
            let mut rec = read_rec(&recs, key64);
            let c = rec[0];
            if c == 65535 { return None; }
            rec.set(0, c + 1);
            if !recs.put(key64, rec.as_slice()) { return None; }
        }
        proof {
            assert(cview[index as int] == customer_scan_view(customers@[index as int]));
            assert forall|k2: int| #[trigger] recs.cell(k2, 0)
                == if 0 <= k2 < kmax { ccp(cview, k2, customers@.len() as int) } else { ccp(cview, k2, index as int + 1) } by {
                lemma_ccp_step(cview, k2, index as int);
            }
        }
        index += 1;
    }
    // orders outside the dense window
    index = 0;
    while index < orders.len()
        invariant index <= orders.len(), recs.wf(), kmax <= 67108864, pats@.len() == 16,
            view == order_scan_seq_view(orders@),
            forall|x: int| 0 <= x < orders.len() ==> key_ok((#[trigger] orders@[x]).o_custkey as int),
            forall|key: int| #[trigger] recs.cell(key, 0) == ccp(cview, key, customers@.len() as int),
            forall|key: int, jj: int| 0 <= jj < 16 ==> #[trigger] recs.cell(key, 1 + jj)
                == if 0 <= key < kmax { ocp(view, pats@[jj]@, key, orders@.len() as int) } else { ocp(view, pats@[jj]@, key, index as int) },
        decreases orders.len() - index,
    {
        let key64 = orders[index].o_custkey;
        proof { assert(key_ok(orders@[index as int].o_custkey as int)); }
        if !(key64 >= 0 && key64 < kmax as i64) {
            let ghost kk: int = key64 as int;
            let mut rec = read_rec(&recs, key64);
            let ghost rec0 = rec@;
            proof {
                assert forall|f: int| 0 <= f < 17 implies rec0[f] as int == recs.cell(kk, f) by {
                    assert(rec@[f] == rec0[f]);
                }
            }
            let mut j = 0usize;
            while j < 16
                invariant j <= 16, index < orders.len(), rec@.len() == 17, pats@.len() == 16,
                    view == order_scan_seq_view(orders@),
                    key64 == orders@[index as int].o_custkey, kk == key64 as int,
                    forall|f: int| 0 <= f < 17 ==> rec0[f] as int == recs.cell(kk, f),
                    rec@[0] == rec0[0],
                    forall|jj: int| 0 <= jj < 16 ==> #[trigger] rec@[1 + jj] as int
                        == ocp(view, pats@[jj]@, kk, if jj < j as int { index as int + 1 } else { index as int }),
                    !(0 <= kk && (kk) < kmax),
                decreases 16 - j,
            {
                let hit = like_match_exec(orders[index].o_comment.as_slice(), pats[j].as_slice());
                let ghost old = rec@;
                proof { assert(old[1 + j as int] as int == ocp(view, pats@[j as int]@, kk, index as int)); }
                if !hit {
                    let v = rec[1 + j];
                    if v == 65535 { return None; }
                    rec.set(1 + j, v + 1);
                }
                proof {
                    assert(view[index as int] == order_scan_view(orders@[index as int]));
                    assert forall|jj: int| 0 <= jj < 16 implies #[trigger] rec@[1 + jj] as int
                        == ocp(view, pats@[jj]@, kk, if jj < (j + 1) as int { index as int + 1 } else { index as int }) by {
                        assert(rec@.len() == 17);
                        assert(old[1 + jj] as int
                            == ocp(view, pats@[jj]@, kk, if jj < j as int { index as int + 1 } else { index as int }));
                        if jj != j as int {
                            assert(rec@[1 + jj] == old[1 + jj]);
                            assert(rec@[1 + jj] as int
                                == ocp(view, pats@[jj]@, kk, if jj < (j + 1) as int { index as int + 1 } else { index as int }));
                        } else {
                            lemma_ocp_step(view, pats@[jj]@, kk, index as int);
                            assert(view[index as int].0 == kk);
                            if !hit {
                                assert(rec@ == old.update(1 + j as int, (old[1 + j as int] + 1) as u16));
                                assert(rec@[1 + jj] as int == old[1 + jj] as int + 1);
                                assert(ocp(view, pats@[jj]@, kk, index as int + 1) == ocp(view, pats@[jj]@, kk, index as int) + 1);
                            } else {
                                assert(rec@ == old);
                                assert(ocp(view, pats@[jj]@, kk, index as int + 1) == ocp(view, pats@[jj]@, kk, index as int));
                            }
                        }
                    }
                }
                j += 1;
            }
            if !recs.put(key64, rec.as_slice()) { return None; }
        }
        proof {
            assert(view[index as int] == order_scan_view(orders@[index as int]));
            assert forall|k2: int, jj: int| 0 <= jj < 16 implies #[trigger] recs.cell(k2, 1 + jj)
                == if 0 <= k2 < kmax { ocp(view, pats@[jj]@, k2, orders@.len() as int) } else { ocp(view, pats@[jj]@, k2, index as int + 1) } by {
                lemma_ocp_step(view, pats@[jj]@, k2, index as int);
            }
        }
        index += 1;
    }
    proof {
        assert forall|key: int| #[trigger] recs.cell(key, 0) == customer_count(db, key) by {
            let weight = |row: CustomerRow| ind(row.c_custkey == key);
            assert(sum(cview, weight) == sum(db.customer, weight));
        }
        assert forall|key: int, jj: int| 0 <= jj < 16 implies #[trigger] recs.cell(key, 1 + jj)
            == order_count(db, pats@[jj]@, key) by {
            let p = pats@[jj]@;
            if !(0 <= key < kmax) { assert(ocp(view, p, key, orders@.len() as int) == ocp(view, p, key, orders@.len() as int)); }
            let weight = |row: (int, Seq<u8>)| ind(row.0 == key && !like_match(row.1, p));
            let projected = |row: OrdersRow| if true { weight((row.o_custkey, row.o_comment)) } else { 0 };
            assert(sum(order_scan_seq_view(orders@), weight) == sum(db.orders, projected));
            lemma_sum_ext_all(db.orders, projected, |row: OrdersRow|
                ind(c_orders_on_orders(db, params(p), CustomerRow { c_custkey: key }, row)));
        }
    }
    // the histograms: one contribution per distinct customer key
    let mut hist: Vec<i64> = vec![0i64; 16384];
    let mut seen = Sparse::new();
    index = 0;
    while index < customers.len()
        invariant index <= customers.len(), recs.wf(), pats@.len() == 16, hist@.len() == 16384, seen.wf(),
            cview == customer_scan_seq_view(customers@),
            forall|x: int| 0 <= x < customers.len() ==> key_ok((#[trigger] customers@[x]).c_custkey as int),
            forall|key: int| #[trigger] recs.cell(key, 0) == customer_count(db, key),
            forall|key: int, jj: int| 0 <= jj < 16 ==> #[trigger] recs.cell(key, 1 + jj) == order_count(db, pats@[jj]@, key),
            forall|key: int| #[trigger] seen.contains(key) <==> exists|x: int| 0 <= x < index && customers@[x].c_custkey as int == key,
            forall|jj: int, b: int| 0 <= jj < 16 ==> #[trigger] hist_at(hist@, jj, b)
                == sum(seen.keys(), |key: i128| contribution(db, pats@[jj]@, key as int, b)),
        decreases customers.len() - index,
    {
        let key64 = customers[index].c_custkey;
        let key = key64 as i128;
        let ghost seen_before = seen;
        if !seen.has(key) {
            seen.set(key, 1);
            let mut j = 0usize;
            while j < 16
                invariant j <= 16, index < customers.len(), recs.wf(), pats@.len() == 16, hist@.len() == 16384,
                    key_ok(key64 as int), key == key64 as i128,
                    forall|key: int| #[trigger] recs.cell(key, 0) == customer_count(db, key),
                    forall|key: int, jj: int| 0 <= jj < 16 ==> #[trigger] recs.cell(key, 1 + jj) == order_count(db, pats@[jj]@, key),
                    seen.keys() == seen_before.keys().push(key),
                    forall|jj: int, b: int| 0 <= jj < 16 ==> #[trigger] hist_at(hist@, jj, b)
                        == sum(if jj < j as int { seen.keys() } else { seen_before.keys() },
                            |key: i128| contribution(db, pats@[jj]@, key as int, b)),
                decreases 16 - j,
            {
                let cc = recs.get(key64, 0);
                let oc = recs.get(key64, 1 + j);
                let ghost old = hist@;
                let ghost bk: int = cc as int * oc as int;
                if cc > 0 {
                    proof {
                        assert((cc as u64) * (oc as u64) <= 4294836225u64) by (nonlinear_arith)
                            requires cc as u64 <= 65535u64, oc as u64 <= 65535u64;
                    }
                    let bucket64 = (cc as u64) * (oc as u64);
                    if bucket64 >= 1024 { return None; }
                    let bucket = bucket64 as usize;
                    let idx = j * 1024 + bucket;
                    let v = hist[idx];
                    if v == i64::MAX { return None; }
                    hist.set(idx, v + 1);
                    proof {
                        lemma_hist_set(old, j as int, bucket as int, (v + 1) as i64);
                        assert(bucket as int == bk);
                        assert(hist_at(old, j as int, bucket as int) == v as int);
                    }
                }
                proof {
                    assert forall|jj: int, b: int| 0 <= jj < 16 implies #[trigger] hist_at(hist@, jj, b)
                        == sum(if jj < (j + 1) as int { seen.keys() } else { seen_before.keys() },
                            |key: i128| contribution(db, pats@[jj]@, key as int, b)) by {
                        if jj == j as int {
                            lemma_sum_push(seen_before.keys(), key,
                                |k2: i128| contribution(db, pats@[jj]@, k2 as int, b));
                            assert(customer_count(db, key64 as int) == cc as int);
                            assert(order_count(db, pats@[jj]@, key64 as int) == oc as int);
                            assert(contribution(db, pats@[jj]@, key64 as int, b) == ind(cc > 0 && bk == b));
                        }
                    }
                }
                j += 1;
            }
        }
        proof {
            assert forall|k2: int| #[trigger] seen.contains(k2) <==> exists|x: int| 0 <= x < index + 1 && customers@[x].c_custkey as int == k2 by {
                if !seen_before.contains(key as int) {
                    assert(seen.contains(k2) == (seen_before.contains(k2) || k2 == key as int));
                }
                if seen.contains(k2) && k2 == key as int { assert(customers@[index as int].c_custkey as int == k2); }
            }
            assert forall|jj: int, b: int| 0 <= jj < 16 implies #[trigger] hist_at(hist@, jj, b)
                == sum(seen.keys(), |key: i128| contribution(db, pats@[jj]@, key as int, b)) by {
                if seen_before.contains(key as int) {
                    assert(seen.keys() == seen_before.keys());
                }
            }
        }
        index += 1;
    }
    proof {
        seen.lemma_keys();
        assert forall|key: int| customer_count(db, key) > 0 <==> has_key(seen.keys(), key) by {
            seen.lemma_support(key);
            let weight = |row: CustomerRow| ind(row.c_custkey == key);
            assert(sum(cview, weight) == sum(db.customer, weight));
            lemma_indicator(cview, weight);
            if customer_count(db, key) > 0 {
                let x = choose|x: int| 0 <= x < cview.len() && #[trigger] weight(cview[x]) > 0;
                assert(cview[x] == customer_scan_view(customers@[x]));
                assert(customers@[x].c_custkey as int == key);
            }
            if seen.contains(key) {
                let x = choose|x: int| 0 <= x < index && customers@[x].c_custkey as int == key;
                assert(cview[x] == customer_scan_view(customers@[x]));
                assert(weight(cview[x]) > 0);
            }
        }
        assert forall|jj: int, b: int| 0 <= jj < 16 implies #[trigger] hist_at(hist@, jj, b)
            == histogram_count(db, pats@[jj]@, b) by {
            lemma_seen_hist(db, pats@[jj]@, seen.keys(), b);
        }
    }
    let hist_a = Arc::new(hist);
    let pats_a = Arc::new(pats);
    Some(Fast { recs, hist: hist_a, pats: pats_a, db: Ghost(db) })
}

pub fn fast_execute_j(state: &Fast, j: usize) -> (result: Option<Vec<(i64, i64)>>)
    requires j < 16,
    ensures result matches Some(out) ==>
        query_result(state.db(), params(state.pat(j as int)), out_view(out@)),
{
    proof { use_type_invariant(state); }
    let ghost p = state.pat(j as int);
    let ghost db = state.db();
    let mut rows: Vec<(i64, i64)> = Vec::new();
    let mut bucket = 0usize;
    while bucket < 1024
        invariant state.wf(), j < 16, p == state.pat(j as int), db == state.db(), bucket <= 1024,
            forall|position: int| 0 <= position < rows@.len() ==> {
                &&& (#[trigger] rows@[position]).1 > 0
                &&& rows@[position].1 as int == histogram_count(db, p, rows@[position].0 as int)
                &&& 0 <= rows@[position].0 < bucket
            },
            forall|first: int, second: int| #![trigger rows@[first], rows@[second]]
                0 <= first < second < rows@.len() ==> rows@[first].0 != rows@[second].0,
            forall|b: int| 0 <= b < bucket && histogram_count(db, p, b) > 0 ==>
                exists|position: int| 0 <= position < rows@.len() && (#[trigger] rows@[position]).0 as int == b,
        decreases 1024 - bucket,
    {
        let frequency = state.hist[j * 1024 + bucket];
        let ghost previous = rows@;
        proof {
            assert(hist_at((*state.hist)@, j as int, bucket as int) == histogram_count(db, p, bucket as int));
        }
        if frequency > 0 {
            rows.push((bucket as i64, frequency));
        }
        proof {
            assert forall|position: int| 0 <= position < rows@.len() implies {
                &&& (#[trigger] rows@[position]).1 > 0
                &&& rows@[position].1 as int == histogram_count(db, p, rows@[position].0 as int)
                &&& 0 <= rows@[position].0 < bucket + 1
            } by {
                if position < previous.len() { assert(rows@[position] == previous[position]); }
            }
            assert forall|first: int, second: int| #![trigger rows@[first], rows@[second]]
                0 <= first < second < rows@.len() implies rows@[first].0 != rows@[second].0 by {
                if second < previous.len() {
                    assert(rows@[first] == previous[first] && rows@[second] == previous[second]);
                } else {
                    assert(rows@[first] == previous[first]);
                }
            }
            assert forall|b: int| 0 <= b < bucket + 1 && histogram_count(db, p, b) > 0 implies
                exists|position: int| 0 <= position < rows@.len() && (#[trigger] rows@[position]).0 as int == b by {
                if b < bucket as int {
                    let position = choose|position: int| 0 <= position < previous.len() && (#[trigger] previous[position]).0 as int == b;
                    assert(rows@[position] == previous[position]);
                } else {
                    assert(rows@[rows@.len() - 1].0 as int == b);
                }
            }
        }
        bucket += 1;
    }
    proof {
        assert forall|b: int| #[trigger] histogram_count(db, p, b) > 0 implies
            exists|position: int| 0 <= position < rows@.len() && (#[trigger] rows@[position]).0 as int == b by {
            assert(hist_at((*state.hist)@, j as int, b) == histogram_count(db, p, b));
        }
    }
    let ghost unsorted = rows@;
    crate::output::sort(&mut rows);
    proof {
        assert forall|row: (i64, i64)| #[trigger] rows@.contains(row) == unsorted.contains(row) by {
            lemma_bag_contains(rows@, row);
            lemma_bag_contains(unsorted, row);
        }
        assert forall|position: int| 0 <= position < rows@.len() implies {
            &&& (#[trigger] rows@[position]).1 > 0
            &&& rows@[position].1 as int == histogram_count(db, p, rows@[position].0 as int)
        } by {
            assert(rows@.contains(rows@[position]));
            let original = choose|original: int| 0 <= original < unsorted.len() && unsorted[original] == rows@[position];
        }
        assert forall|bucket: int| #[trigger] histogram_count(db, p, bucket) > 0 implies
            exists|position: int| 0 <= position < rows@.len() && (#[trigger] rows@[position]).0 as int == bucket by {
            let original = choose|original: int| 0 <= original < unsorted.len() && (#[trigger] unsorted[original]).0 as int == bucket;
            assert(unsorted.contains(unsorted[original]));
            assert(rows@.contains(unsorted[original]));
            let position = choose|position: int| 0 <= position < rows@.len() && rows@[position] == unsorted[original];
        }
        lemma_execute_facts(db, p, rows@);
    }
    Some(rows)
}
pub fn fast_execute(state: &Fast, p1: &[u8]) -> (result: Option<Vec<(i64, i64)>>)
    ensures result matches Some(out) ==> query_result(state.db(), params(p1@), out_view(out@)),
{
    proof { use_type_invariant(state); }
    let mut j = 0usize;
    while j < 16
        invariant j <= 16, state.wf(), (*state.pats)@.len() == 16,
        decreases 16 - j,
    {
        if bytes_eq(p1, state.pats[j].as_slice()) {
            return fast_execute_j(state, j);
        }
        j += 1;
    }
    None
}

/// Σ over the first n order deltas of sign * [custkey = key and the comment does not match p].
pub open spec fn och(rows: Seq<OrderDelta>, p: Seq<u8>, key: int, n: int) -> int {
    sum(rows.take(n), |change: OrderDelta|
        change.sign as int * ind(change.o_custkey as int == key && !like_match(change.o_comment@, p)))
}

/// What field f of the record of `key` must hold in database `db`.
pub open spec fn tgt(pats: Seq<Vec<u8>>, db: Db, key: int, f: int) -> int {
    if f == 0 { customer_count(db, key) } else { order_count(db, pats[f - 1]@, key) }
}

pub fn fast_apply(
    state: &Fast,
    customer_delta: &[CustomerDelta],
    orders_delta: &Vec<OrderDelta>,
    Ghost(orders_full): Ghost<Seq<(OrdersRow, int)>>,
    Ghost(db2): Ghost<Db>,
) -> (result: Result<Fast, u32>)
    requires orders_delta_ok(orders_delta@, orders_full),
        capture_ok(state.db(), db2, customer_delta_seq_view(customer_delta@), orders_full),
    ensures result matches Ok(next) ==> next.db() == db2,
{
    proof { use_type_invariant(state); }
    let ghost db = state.db();
    let mut customer_changes = Sparse::new();
    let mut order_changes = Sparse::new();
    let mut touched = Sparse::new();
    let mut index = 0usize;
    while index < customer_delta.len()
        invariant index <= customer_delta.len(),
            customer_changes.wf(), touched.wf(),
            forall|key: int| #[trigger] customer_changes.at(key)
                == sum(customer_delta_seq_view(customer_delta@).take(index as int),
                    |change: (CustomerRow, int)| change.1 * ind(change.0.c_custkey == key)),
            forall|key: int| #[trigger] customer_changes.at(key) != 0 ==> touched.contains(key),
            forall|key: int| #[trigger] touched.contains(key) ==> key_ok(key),
        decreases customer_delta.len() - index,
    {
        let key64 = customer_delta[index].row.c_custkey;
        let key = key64 as i128;
        if !customer_changes.add(key, customer_delta[index].sign as i128) { return Err(1); }
        touched.set(key, 1);
        proof {
            assert forall|key: int| #[trigger] customer_changes.at(key)
                == sum(customer_delta_seq_view(customer_delta@).take(index as int + 1),
                    |change: (CustomerRow, int)| change.1 * ind(change.0.c_custkey == key)) by {
                lemma_sum_take(customer_delta_seq_view(customer_delta@), index as int,
                    |change: (CustomerRow, int)| change.1 * ind(change.0.c_custkey == key));
            }
        }
        index += 1;
    }
    proof {
        assert(customer_delta_seq_view(customer_delta@).take(customer_delta@.len() as int) =~= customer_delta_seq_view(customer_delta@));
    }
    index = 0;
    while index < orders_delta.len()
        invariant index <= orders_delta.len(), state.wf(),
            customer_changes.wf(), order_changes.wf(), touched.wf(),
            forall|key: int| #[trigger] customer_changes.at(key) == customer_change(customer_delta@, key),
            forall|key: int| #[trigger] customer_changes.at(key) != 0 ==> touched.contains(key),
            forall|key: int| #[trigger] touched.contains(key) ==> key_ok(key),
            forall|key: int, jj: int| 0 <= jj < 16 && #[trigger] order_changes.at(key * 16 + jj) != 0 ==> touched.contains(key),
            forall|key: int, jj: int| 0 <= jj < 16 ==> #[trigger] order_changes.at(key * 16 + jj)
                == och(orders_delta@, state.pat(jj), key, index as int),
        decreases orders_delta.len() - index,
    {
        let key64 = orders_delta[index].o_custkey;
        let key = key64 as i128;
        touched.set(key, 1);
        let mut j = 0usize;
        while j < 16
            invariant j <= 16, index < orders_delta.len(), state.wf(), (*state.pats)@.len() == 16,
                key as int == orders_delta@[index as int].o_custkey as int,
                customer_changes.wf(), order_changes.wf(), touched.wf(),
                forall|key: int| #[trigger] customer_changes.at(key) == customer_change(customer_delta@, key),
                forall|key: int| #[trigger] customer_changes.at(key) != 0 ==> touched.contains(key),
                forall|key: int| #[trigger] touched.contains(key) ==> key_ok(key) || key == key64 as int,
                touched.contains(key as int),
                forall|key: int, jj: int| 0 <= jj < 16 && #[trigger] order_changes.at(key * 16 + jj) != 0 ==> touched.contains(key),
                forall|key: int, jj: int| 0 <= jj < 16 ==> #[trigger] order_changes.at(key * 16 + jj)
                    == och(orders_delta@, state.pat(jj), key, if jj < j as int { index as int + 1 } else { index as int }),
            decreases 16 - j,
        {
            let hit = like_match_exec(orders_delta[index].o_comment.as_slice(), state.pats[j].as_slice());
            let ghost old = order_changes;
            if !hit {
                if !order_changes.add(key * 16 + j as i128, orders_delta[index].sign as i128) { return Err(1); }
            }
            proof {
                assert forall|k2: int, jj: int| 0 <= jj < 16 implies #[trigger] order_changes.at(k2 * 16 + jj)
                    == och(orders_delta@, state.pat(jj), k2, if jj < (j + 1) as int { index as int + 1 } else { index as int }) by {
                    if jj == j as int {
                        lemma_sum_take(orders_delta@, index as int, |change: OrderDelta|
                            change.sign as int * ind(change.o_custkey as int == k2 && !like_match(change.o_comment@, state.pat(jj))));
                        assert(hit == like_match(orders_delta@[index as int].o_comment@, state.pat(j as int)));
                    }
                }
            }
            j += 1;
        }
        proof {
            assert forall|key: int| #[trigger] touched.contains(key) implies key_ok(key) by {}
        }
        index += 1;
    }
    proof {
        assert(forall|key: int, jj: int| 0 <= jj < 16 ==> #[trigger] order_changes.at(key * 16 + jj)
            == och(orders_delta@, state.pat(jj), key, orders_delta@.len() as int));
        assert forall|key: int, jj: int| 0 <= jj < 16 implies
            #[trigger] order_changes.at(key * 16 + jj) == order_change(orders_delta@, state.pat(jj), key) by {
            assert(orders_delta@.take(orders_delta@.len() as int) =~= orders_delta@);
        }
        assert forall|key: int| #[trigger] customer_changes.at(key)
            == customer_count(db2, key) - customer_count(db, key) by {
            lemma_capture_counts(db, db2, state.pat(0), customer_delta@, orders_delta@, orders_full, key);
        }
        assert forall|key: int, jj: int| 0 <= jj < 16 implies #[trigger] order_changes.at(key * 16 + jj)
            == order_count(db2, state.pat(jj), key) - order_count(db, state.pat(jj), key) by {
            lemma_capture_counts(db, db2, state.pat(jj), customer_delta@, orders_delta@, orders_full, key);
        }
    }
    // the new version
    let mut recs = state.recs.dup();
    let mut hist: Vec<i64> = (*state.hist).clone();
    proof { assert(hist@ =~= (*state.hist)@); }
    proof { touched.lemma_keys(); }
    index = 0;
    let mut newrec: Vec<u16> = vec![0u16; 17];
    while index < touched.len()
        invariant index <= touched.keys().len(), state.wf(), db == state.db(), (*state.pats)@.len() == 16,
            recs.wf(), touched.wf(), newrec@.len() == 17, hist@.len() == 16384,
            customer_changes.wf(), order_changes.wf(),
            forall|key: int| #[trigger] customer_changes.at(key) == customer_count(db2, key) - customer_count(db, key),
            forall|key: int, jj: int| 0 <= jj < 16 ==> #[trigger] order_changes.at(key * 16 + jj)
                == order_count(db2, state.pat(jj), key) - order_count(db, state.pat(jj), key),
            forall|key: int| #[trigger] touched.contains(key) ==> key_ok(key),
            forall|key: int| #[trigger] customer_changes.at(key) != 0 ==> touched.contains(key),
            forall|key: int, jj: int| 0 <= jj < 16 && #[trigger] order_changes.at(key * 16 + jj) != 0 ==> touched.contains(key),
            forall|key: i128| #[trigger] touched.keys().contains(key) == touched.contains(key as int),
            no_dups(touched.keys()),
            forall|key: int, f: int| 0 <= f < 17 ==> #[trigger] recs.cell(key, f)
                == if has_key(touched.keys().take(index as int), key) { tgt((*state.pats)@, db2, key, f) }
                    else { state.recs.cell(key, f) },
            forall|jj: int, b: int| 0 <= jj < 16 ==> #[trigger] hist_at(hist@, jj, b)
                == histogram_count(db, state.pat(jj), b)
                    + sum(touched.keys().take(index as int), |key: i128|
                        contribution(db2, state.pat(jj), key as int, b) - contribution(db, state.pat(jj), key as int, b)),
        decreases touched.keys().len() - index,
    {
        let key = touched.key(index);
        proof { assert(touched.keys().contains(key)); assert(touched.contains(key as int)); }
        let k = key as i64;
        let cust_change = customer_changes.get(key);
        let old_cc = state.recs.get(k, 0) as i128;
        proof { use_type_invariant(state); }
        assert(state.db@ == db);
        assert(state.recs.cell(key as int, 0) == customer_count(state.db@, key as int));
        assert(state.recs.cell(key as int, 0) == customer_count(db, key as int));
        let new_cc = match old_cc.checked_add(cust_change) { Some(v) => v, None => { return Err(1); } };
        if new_cc < 0 || new_cc > 65535 { return Err(2); }
        let ghost recs0 = recs;
        let ghost hist0 = hist@;
        newrec.set(0, new_cc as u16);
        let mut j = 0usize;
        while j < 16
            invariant j <= 16, state.wf(), db == state.db(), (*state.pats)@.len() == 16, k == key as i64,
                key_ok(key as int),
                recs.wf(), newrec@.len() == 17, hist@.len() == 16384,
                customer_changes.wf(), order_changes.wf(),
                old_cc as int == customer_count(db, key as int), 0 <= old_cc <= 65535,
                new_cc as int == customer_count(db2, key as int), 0 <= new_cc <= 65535,
                newrec@[0] as int == new_cc as int,
                forall|key2: int, jj: int| 0 <= jj < 16 ==> #[trigger] order_changes.at(key2 * 16 + jj)
                    == order_count(db2, state.pat(jj), key2) - order_count(db, state.pat(jj), key2),
                forall|jj: int| 0 <= jj < j ==> #[trigger] newrec@[1 + jj] as int == order_count(db2, state.pat(jj), key as int),
                forall|jj: int, b: int| 0 <= jj < 16 ==> #[trigger] hist_at(hist0, jj, b)
                    == histogram_count(db, state.pat(jj), b)
                        + sum(touched.keys().take(index as int), |key: i128|
                            contribution(db2, state.pat(jj), key as int, b) - contribution(db, state.pat(jj), key as int, b)),
                forall|jj: int, b: int| 0 <= jj < 16 ==> #[trigger] hist_at(hist@, jj, b)
                    == hist_at(hist0, jj, b) + if jj < j as int {
                        contribution(db2, state.pat(jj), key as int, b) - contribution(db, state.pat(jj), key as int, b)
                    } else { 0 },
            decreases 16 - j,
        {
            let old_oc = state.recs.get(k, 1 + j) as i128;
            let och_j = order_changes.get(key * 16 + j as i128);
            proof { use_type_invariant(state); assert(state.ocnt(key as int, j as int) == order_count(db, state.pat(j as int), key as int)); }
            let new_oc = match old_oc.checked_add(och_j) { Some(v) => v, None => { return Err(1); } };
            if new_oc < 0 || new_oc > 65535 { return Err(2); }
            newrec.set(1 + j, new_oc as u16);
            let ghost h0 = hist@;
            let ghost p = state.pat(j as int);
            proof {
                assert(old_oc as int == order_count(db, p, key as int));
                assert(new_oc as int == order_count(db2, p, key as int));
            }
            let ghost ob_spec: int = old_cc as int * old_oc as int;
            let ghost nb_spec: int = new_cc as int * new_oc as int;
            if old_cc > 0 {
                proof { assert(old_cc * old_oc <= 4294836225i128) by (nonlinear_arith) requires 0 <= old_cc <= 65535, 0 <= old_oc <= 65535; }
                let ob = (old_cc * old_oc) as i128;
                if ob >= 1024 { return Err(1); }
                let idx = j * 1024 + ob as usize;
                let v = hist[idx];
                match v.checked_sub(1) {
                    None => { return Err(1); },
                    Some(nv) => { hist.set(idx, nv); },
                }
                proof {
                    lemma_hist_set(h0, j as int, ob as int, (h0[j as int * 1024 + ob as int] - 1) as i64);
                    assert(ob as int == ob_spec);
                }
            }
            let ghost h1 = hist@;
            proof {
                assert forall|jj: int, b: int| 0 <= jj < 16 implies #[trigger] hist_at(h1, jj, b)
                    == hist_at(h0, jj, b) - (if jj == j as int && old_cc > 0 && b == ob_spec { 1int } else { 0int }) by {}
            }
            if new_cc > 0 {
                proof { assert(new_cc * new_oc <= 4294836225i128) by (nonlinear_arith) requires 0 <= new_cc <= 65535, 0 <= new_oc <= 65535; }
                let nb = (new_cc * new_oc) as i128;
                if nb >= 1024 { return Err(1); }
                let idx = j * 1024 + nb as usize;
                let v = hist[idx];
                match v.checked_add(1) {
                    None => { return Err(1); },
                    Some(nv) => { hist.set(idx, nv); },
                }
                proof {
                    lemma_hist_set(h1, j as int, nb as int, (h1[j as int * 1024 + nb as int] + 1) as i64);
                    assert(nb as int == nb_spec);
                }
            }
            proof {
                assert forall|jj: int, b: int| 0 <= jj < 16 implies #[trigger] hist_at(hist@, jj, b)
                    == hist_at(h1, jj, b) + (if jj == j as int && new_cc > 0 && b == nb_spec { 1int } else { 0int }) by {}
                assert forall|jj: int, b: int| 0 <= jj < 16 implies #[trigger] hist_at(hist@, jj, b)
                    == hist_at(hist0, jj, b) + if jj < (j + 1) as int {
                        contribution(db2, state.pat(jj), key as int, b) - contribution(db, state.pat(jj), key as int, b)
                    } else { 0 } by {
                    if jj == j as int {
                        assert(contribution(db, p, key as int, b) == ind(old_cc > 0 && old_cc * old_oc == b));
                        assert(contribution(db2, p, key as int, b) == ind(new_cc > 0 && new_cc * new_oc == b));
                    }
                }
            }
            j += 1;
        }
        let ghost before_recs = recs;
        if !recs.put(k, newrec.as_slice()) { return Err(1); }
        proof {
            lemma_has_key_step(touched.keys(), index as int);
            assert forall|key2: int, f: int| 0 <= f < 17 implies #[trigger] recs.cell(key2, f)
                == if has_key(touched.keys().take(index as int + 1), key2) { tgt((*state.pats)@, db2, key2, f) }
                    else { state.recs.cell(key2, f) } by {
                if key2 == key as int && f > 0 {
                    assert(newrec@[1 + (f - 1)] as int == order_count(db2, state.pat(f - 1), key as int));
                }
            }
            assert forall|jj: int, b: int| 0 <= jj < 16 implies #[trigger] hist_at(hist@, jj, b)
                == histogram_count(db, state.pat(jj), b)
                    + sum(touched.keys().take(index as int + 1), |key: i128|
                        contribution(db2, state.pat(jj), key as int, b) - contribution(db, state.pat(jj), key as int, b)) by {
                lemma_sum_take(touched.keys(), index as int, |key: i128|
                    contribution(db2, state.pat(jj), key as int, b) - contribution(db, state.pat(jj), key as int, b));
            }
        }
        index += 1;
    }
    let hist_a = Arc::new(hist);
    let pats_a = state.pats.clone();
    proof {
        use_type_invariant(state);
        assert(*pats_a == *state.pats);
        assert(touched.keys().take(touched.keys().len() as int) =~= touched.keys());
        lemma_cast_keys(touched.keys());
        // keys outside the touched set keep their counts
        assert forall|key: int| !#[trigger] touched.contains(key) implies
            customer_count(db, key) == customer_count(db2, key) by {
            assert(customer_changes.at(key) == 0);
        }
        assert forall|key: int, jj: int| 0 <= jj < 16 && !touched.contains(key) implies
            #[trigger] order_count(db, state.pat(jj), key) == order_count(db2, state.pat(jj), key) by {
            assert(order_changes.at(key * 16 + jj) == 0);
        }
        assert forall|key: int| !#[trigger] cast_keys(touched.keys()).contains(key) implies !touched.contains(key) by {
            touched.lemma_support(key);
        }
        assert forall|key: int| #[trigger] recs.cell(key, 0) == customer_count(db2, key) by {
            touched.lemma_support(key);
            if !touched.contains(key) { assert(!has_key(touched.keys(), key)); }
        }
        assert forall|key: int, j: int| 0 <= j < 16 implies #[trigger] recs.cell(key, 1 + j)
            == order_count(db2, (*pats_a)@[j]@, key) by {
            touched.lemma_support(key);
            assert(state.ocnt(key, j) == order_count(db, state.pat(j), key));
            assert(recs.cell(key, 1 + j) == if has_key(touched.keys(), key) { tgt((*state.pats)@, db2, key, 1 + j) }
                else { state.recs.cell(key, 1 + j) });
            if !touched.contains(key) { assert(!has_key(touched.keys(), key)); }
        }
        assert forall|j: int, b: int| 0 <= j < 16 implies #[trigger] hist_at((*hist_a)@, j, b)
            == histogram_count(db2, (*pats_a)@[j]@, b) by {
            assert forall|key: int| !#[trigger] cast_keys(touched.keys()).contains(key) implies
                customer_count(db, key) == customer_count(db2, key)
                    && order_count(db, state.pat(j), key) == order_count(db2, state.pat(j), key) by {
                assert(!touched.contains(key));
            }
            lemma_apply_facts(db, db2, state.pat(j), touched.keys(), b);
        }
    }
    Ok(Fast { recs, hist: hist_a, pats: pats_a, db: Ghost(db2) })
}


pub proof fn lemma_sum_pointwise<A, B>(left: Seq<A>, right: Seq<B>, f: spec_fn(A) -> int, g: spec_fn(B) -> int)
    requires left.len() == right.len(), forall|index: int| 0 <= index < left.len() ==> f(#[trigger] left[index]) == g(right[index]),
    ensures sum(left, f) == sum(right, g),
    decreases left.len(),
{
    if left.len() > 0 {
        let rest_left = left.drop_last();
        let rest_right = right.drop_last();
        assert forall|index: int| 0 <= index < rest_left.len() implies f(#[trigger] rest_left[index]) == g(rest_right[index]) by {
            assert(rest_left[index] == left[index]);
            assert(rest_right[index] == right[index]);
        }
        lemma_sum_pointwise(rest_left, rest_right, f, g);
        assert(left =~= rest_left.push(left.last()));
        assert(right =~= rest_right.push(right.last()));
        lemma_sum_push(rest_left, left.last(), f);
        lemma_sum_push(rest_right, right.last(), g);
    }
}

pub proof fn lemma_scan_lens(db: Db, customers: Seq<CustomerScan>, orders: Seq<OrderScan>)
    requires scan_customers_ok(db, customers), scan_orders_ok(db, orders),
    ensures customers.len() == db.customer.len(), orders.len() == db.orders.len(),
{
    let one_customer = |row: CustomerRow| 1int;
    verus_prooflib::lemma_sum_ones(customer_scan_seq_view(customers));
    verus_prooflib::lemma_sum_ones(db.customer);
    assert(sum(customer_scan_seq_view(customers), one_customer) == sum(db.customer, one_customer));
    let one_order = |row: (int, Seq<u8>)| 1int;
    verus_prooflib::lemma_sum_ones(order_scan_seq_view(orders));
    verus_prooflib::lemma_sum_ones(db.orders);
    assert(sum(order_scan_seq_view(orders), one_order) == sum(db.orders, |row: OrdersRow| if true { one_order((row.o_custkey, row.o_comment)) } else { 0int }));
    lemma_sum_ext_all(db.orders, |row: OrdersRow| if true { one_order((row.o_custkey, row.o_comment)) } else { 0int }, |row: OrdersRow| 1int);
}

pub proof fn lemma_scan_log(db: Db, rows: Seq<OrderScan>, f: spec_fn((int, Seq<u8>)) -> int)
    requires scan_orders_ok(db, rows),
    ensures sum(order_scan_seq_view(rows), f) == sum(db.orders, |orders: OrdersRow| f((orders.o_custkey, orders.o_comment))),
{
    assert(sum(order_scan_seq_view(rows), f) == sum(db.orders, |orders: OrdersRow| if true { f((orders.o_custkey, orders.o_comment)) } else { 0 }));
    lemma_sum_ext_all(db.orders, |orders: OrdersRow| if true { f((orders.o_custkey, orders.o_comment)) } else { 0 },
        |orders: OrdersRow| f((orders.o_custkey, orders.o_comment)));
}

/// The tracked fast path (when it could be built) and the signed logs the general path replays.
pub struct State {
    fast: Option<Fast>,
    orders: Log<OrderEntry>,
    customers: Log<CustEntry>,
    appended: u64,
    db: Ghost<Db>,
}

pub closed spec fn history(s: State) -> nat { s.appended as nat }

impl State {
    pub closed spec fn db(self) -> Db { self.db@ }

    #[verifier::type_invariant]
    spec fn valid(self) -> bool {
        &&& self.fast matches Some(fast) ==> fast.db() == self.db@
        &&& unit_orders(self.orders.entries())
        &&& unit_customers(self.customers.entries())
        &&& self.appended <= 4611686018427387904
        &&& self.orders.entries().len() <= 1099511627776 + self.appended
        &&& self.customers.entries().len() <= 1099511627776 + self.appended
        &&& forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] order_log_sum(self.orders.entries(), f)
            == sum(self.db@.orders, |orders: OrdersRow| f((orders.o_custkey, orders.o_comment)))
        &&& forall|f: spec_fn(CustomerRow) -> int| #[trigger] customer_log_sum(self.customers.entries(), f)
            == sum(self.db@.customer, f)
    }
}

#[verifier::rlimit(40)]
pub fn build(
    customers: &[CustomerScan],
    orders: Vec<OrderScan>,
    Ghost(db): Ghost<Db>,
) -> (result: Option<State>)
    requires scan_customers_ok(db, customers@), scan_orders_ok(db, orders@),
    ensures result matches Some(state) ==> state.db() == db,
        result matches Some(state) ==> history(state) == 0,
        envelope(db) ==> result is Some,
{
    proof { lemma_scan_lens(db, customers@, orders@); }
    if customers.len() as u64 > 1099511627776u64 || orders.len() as u64 > 1099511627776u64 { return None; }
    let fast = fast_build(customers, &orders, Ghost(db));
    let mut customer_entries: Vec<CustEntry> = Vec::new();
    let mut index = 0usize;
    while index < customers.len()
        invariant index <= customers.len(), customer_entries@.len() == index,
            forall|k: int| 0 <= k < index ==> customer_entries@[k].custkey == customers@[k].c_custkey && customer_entries@[k].sign == 1,
        decreases customers.len() - index,
    {
        customer_entries.push(CustEntry { custkey: customers[index].c_custkey, sign: 1 });
        index += 1;
    }
    let ghost cview = customer_scan_seq_view(customers@);
    let ghost original = order_scan_seq_view(orders@);
    let ghost orders_original = orders@;
    let mut remaining = orders;
    let mut order_entries: Vec<OrderEntry> = Vec::new();
    proof {
        assert(order_scan_seq_view(remaining@) == original);
        assert(order_entries@ =~= Seq::<OrderEntry>::empty());
        assert forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] order_log_sum(order_entries@, f) + sum(order_scan_seq_view(remaining@), f)
            == sum(original, f) by {
            lemma_sum_empty(order_entries@, |entry: OrderEntry| entry.sign as int * f((entry.custkey as int, (*entry.comment)@)));
        }
    }
    while remaining.len() > 0
        invariant remaining.len() + order_entries.len() == orders_original.len(),
            forall|k: int| 0 <= k < order_entries@.len() ==> order_entries@[k].sign == 1,
            forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] order_log_sum(order_entries@, f) + sum(order_scan_seq_view(remaining@), f)
                == sum(original, f),
        decreases remaining.len(),
    {
        let ghost before_entries = order_entries@;
        let ghost before_remaining = remaining@;
        let row = remaining.pop().unwrap();
        let ghost viewed = (row.o_custkey as int, row.o_comment@);
        order_entries.push(OrderEntry { custkey: row.o_custkey, comment: Arc::new(row.o_comment), sign: 1 });
        proof {
            assert(order_scan_seq_view(before_remaining) =~= order_scan_seq_view(remaining@).push(viewed));
            assert forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] order_log_sum(order_entries@, f) + sum(order_scan_seq_view(remaining@), f)
                == sum(original, f) by {
                let weight = |entry: OrderEntry| entry.sign as int * f((entry.custkey as int, (*entry.comment)@));
                assert(order_log_sum(before_entries, f) + sum(order_scan_seq_view(before_remaining), f) == sum(original, f));
                lemma_sum_push(before_entries, order_entries@.last(), weight);
                assert(order_entries@ =~= before_entries.push(order_entries@.last()));
                lemma_sum_push(order_scan_seq_view(remaining@), viewed, f);
                assert(order_entries@.last().sign as int * f((order_entries@.last().custkey as int, (*order_entries@.last().comment)@)) == f(viewed));
            }
        }
    }
    proof {
        assert(order_scan_seq_view(remaining@) =~= Seq::<(int, Seq<u8>)>::empty());
    }
    let ghost final_customers = customer_entries@;
    let ghost final_orders = order_entries@;
    let orders_log = Log::empty().with_chunk(order_entries);
    let customers_log = Log::empty().with_chunk(customer_entries);
    proof {
        assert(orders_log.entries() =~= final_orders);
        assert(customers_log.entries() =~= final_customers);
        assert forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] order_log_sum(orders_log.entries(), f)
            == sum(db.orders, |orders: OrdersRow| f((orders.o_custkey, orders.o_comment))) by {
            assert(order_log_sum(final_orders, f) + sum(order_scan_seq_view(remaining@), f) == sum(original, f));
            lemma_sum_empty(order_scan_seq_view(remaining@), f);
            lemma_scan_log(db, orders_original, f);
            assert(orders_log.entries() == final_orders);
        }
        assert forall|f: spec_fn(CustomerRow) -> int| #[trigger] customer_log_sum(customers_log.entries(), f) == sum(db.customer, f) by {
            lemma_sum_pointwise(final_customers, cview,
                |entry: CustEntry| entry.sign as int * f(CustomerRow { c_custkey: entry.custkey as int }), f);
        }
        assert forall|k: int| 0 <= k < orders_log.entries().len() implies
            (#[trigger] orders_log.entries()[k]).sign == 1 || orders_log.entries()[k].sign == -1 by {}
        assert forall|k: int| 0 <= k < customers_log.entries().len() implies
            (#[trigger] customers_log.entries()[k]).sign == 1 || customers_log.entries()[k].sign == -1 by {}
        assert(final_orders.len() == orders_original.len());
        assert(final_customers.len() == customers@.len());
    }
    let state = State { fast, orders: orders_log, customers: customers_log, appended: 0, db: Ghost(db) };
    Some(state)
}


pub proof fn lemma_log_order_count(db: Db, entries: Seq<OrderEntry>, p: Seq<u8>, key: int)
    requires forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] order_log_sum(entries, f)
        == sum(db.orders, |orders: OrdersRow| f((orders.o_custkey, orders.o_comment))),
    ensures order_count(db, p, key) == sum(entries, order_weight(p, key)),
{
    let f = |row: (int, Seq<u8>)| ind(row.0 == key && !like_match(row.1, p));
    assert(order_log_sum(entries, f) == sum(db.orders, |orders: OrdersRow| f((orders.o_custkey, orders.o_comment))));
    lemma_sum_ext_all(entries, |entry: OrderEntry| entry.sign as int * f((entry.custkey as int, (*entry.comment)@)), order_weight(p, key));
    lemma_sum_ext_all(db.orders, |row: OrdersRow| ind(c_orders_on_orders(db, params(p), CustomerRow { c_custkey: key }, row)),
        |orders: OrdersRow| f((orders.o_custkey, orders.o_comment)));
}

pub proof fn lemma_log_customer_count(db: Db, entries: Seq<CustEntry>, key: int)
    requires forall|f: spec_fn(CustomerRow) -> int| #[trigger] customer_log_sum(entries, f) == sum(db.customer, f),
    ensures customer_count(db, key) == sum(entries, customer_weight(key)),
{
    let f = |row: CustomerRow| ind(row.c_custkey == key);
    assert(customer_log_sum(entries, f) == sum(db.customer, f));
    lemma_sum_ext_all(entries, |entry: CustEntry| entry.sign as int * f(CustomerRow { c_custkey: entry.custkey as int }), customer_weight(key));
}

pub fn execute(state: &State, p1: &[u8]) -> (result: Option<Vec<(i64, i64)>>)
    ensures result matches Some(out) ==> query_result(state.db(), params(p1@), out_view(out@)),
        envelope(state.db()) && answer_fits(state.db(), params(p1@)) ==> result is Some,
{
    proof { use_type_invariant(state); }
    if let Some(fast) = &state.fast {
        if let Some(rows) = fast_execute(fast, p1) { return Some(rows); }
    }
    proof {
        assert forall|key: int| #[trigger] order_count(state.db(), p1@, key) == sum(state.orders.entries(), order_weight(p1@, key)) by {
            lemma_log_order_count(state.db(), state.orders.entries(), p1@, key);
        }
        assert forall|key: int| #[trigger] customer_count(state.db(), key) == sum(state.customers.entries(), customer_weight(key)) by {
            lemma_log_customer_count(state.db(), state.customers.entries(), key);
        }
    }
    replay(&state.orders, &state.customers, p1, Ghost(state.db()))
}

#[verifier::rlimit(40)]
pub fn apply(
    state: &State,
    customer_delta: &[CustomerDelta],
    orders_delta: &Vec<OrderDelta>,
    Ghost(orders_full): Ghost<Seq<(OrdersRow, int)>>,
    Ghost(db2): Ghost<Db>,
) -> (result: Result<State, u32>)
    requires orders_delta_ok(orders_delta@, orders_full),
        capture_ok(state.db(), db2, customer_delta_seq_view(customer_delta@), orders_full),
    ensures result matches Ok(next) ==> next.db() == db2,
        result matches Ok(next) ==> history(next) <= history(*state) + customer_delta@.len() + orders_delta@.len(),
        envelope(state.db()) && envelope(db2) && history(*state) + customer_delta@.len() + orders_delta@.len() <= max_history() ==> result is Ok,
{
    proof { use_type_invariant(state); }
    let total = state.appended as u128 + customer_delta.len() as u128 + orders_delta.len() as u128;
    if total > 4611686018427387904 { return Err(1); }
    let mut customer_entries: Vec<CustEntry> = Vec::new();
    let mut index = 0usize;
    while index < customer_delta.len()
        invariant index <= customer_delta.len(), customer_entries@.len() == index,
            forall|k: int| 0 <= k < index ==> customer_entries@[k].custkey == customer_delta@[k].row.c_custkey
                && customer_entries@[k].sign == customer_delta@[k].sign,
        decreases customer_delta.len() - index,
    {
        customer_entries.push(CustEntry { custkey: customer_delta[index].row.c_custkey, sign: customer_delta[index].sign });
        index += 1;
    }
    let mut order_entries: Vec<OrderEntry> = Vec::new();
    index = 0;
    while index < orders_delta.len()
        invariant index <= orders_delta.len(), order_entries@.len() == index,
            forall|k: int| 0 <= k < index ==> order_entries@[k].custkey == orders_delta@[k].o_custkey
                && order_entries@[k].sign == orders_delta@[k].sign && (*order_entries@[k].comment)@ == orders_delta@[k].o_comment@,
        decreases orders_delta.len() - index,
    {
        order_entries.push(OrderEntry {
            custkey: orders_delta[index].o_custkey,
            comment: Arc::new(crate::general::copy_bytes(&orders_delta[index].o_comment)),
            sign: orders_delta[index].sign,
        });
        index += 1;
    }
    let fast = match &state.fast {
        Some(fast) => match fast_apply(fast, customer_delta, orders_delta, Ghost(orders_full), Ghost(db2)) {
            Ok(next) => Some(next),
            Err(_) => None,
        },
        None => None,
    };
    let ghost new_customers = customer_entries@;
    let ghost new_orders = order_entries@;
    let orders_log = state.orders.with_chunk(order_entries);
    let customers_log = state.customers.with_chunk(customer_entries);
    proof {
        let db = state.db();
        let cdelta = customer_delta_seq_view(customer_delta@);
        assert(orders_log.entries() == state.orders.entries() + new_orders);
        assert(customers_log.entries() == state.customers.entries() + new_customers);
        assert forall|k: int| 0 <= k < orders_log.entries().len() implies
            (#[trigger] orders_log.entries()[k]).sign == 1 || orders_log.entries()[k].sign == -1 by {
            if k < state.orders.entries().len() { assert(orders_log.entries()[k] == state.orders.entries()[k]); }
            else {
                let offset = k - state.orders.entries().len();
                assert(orders_log.entries()[k] == new_orders[offset]);
                assert(orders_delta@[offset].sign as int == orders_full[offset].1);
            }
        }
        assert forall|k: int| 0 <= k < customers_log.entries().len() implies
            (#[trigger] customers_log.entries()[k]).sign == 1 || customers_log.entries()[k].sign == -1 by {
            if k < state.customers.entries().len() { assert(customers_log.entries()[k] == state.customers.entries()[k]); }
            else {
                let offset = k - state.customers.entries().len();
                assert(customers_log.entries()[k] == new_customers[offset]);
                assert(cdelta[offset].1 == customer_delta@[offset].sign as int);
            }
        }
        assert forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] order_log_sum(orders_log.entries(), f)
            == sum(db2.orders, |orders: OrdersRow| f((orders.o_custkey, orders.o_comment))) by {
            let weight = |entry: OrderEntry| entry.sign as int * f((entry.custkey as int, (*entry.comment)@));
            lemma_sum_concat(state.orders.entries(), new_orders, weight);
            let g = |orders: OrdersRow| f((orders.o_custkey, orders.o_comment));
            assert(order_log_sum(state.orders.entries(), f) == sum(db.orders, g));
            assert(sum(db2.orders, g) == sum(db.orders, g) + sum(orders_full, |d: (OrdersRow, int)| d.1 * g(d.0)));
            assert(new_orders.len() == orders_full.len());
            assert forall|k: int| 0 <= k < new_orders.len() implies weight(#[trigger] new_orders[k]) == (|d: (OrdersRow, int)| d.1 * g(d.0))(orders_full[k]) by {
                assert(new_orders[k].sign as int == orders_full[k].1);
                assert(new_orders[k].custkey as int == orders_full[k].0.o_custkey);
                assert((*new_orders[k].comment)@ == orders_full[k].0.o_comment);
            }
            lemma_sum_pointwise(new_orders, orders_full, weight, |d: (OrdersRow, int)| d.1 * g(d.0));
        }
        assert forall|f: spec_fn(CustomerRow) -> int| #[trigger] customer_log_sum(customers_log.entries(), f) == sum(db2.customer, f) by {
            let weight = |entry: CustEntry| entry.sign as int * f(CustomerRow { c_custkey: entry.custkey as int });
            lemma_sum_concat(state.customers.entries(), new_customers, weight);
            assert(customer_log_sum(state.customers.entries(), f) == sum(db.customer, f));
            assert(sum(db2.customer, f) == sum(db.customer, f) + sum(cdelta, |d: (CustomerRow, int)| d.1 * f(d.0)));
            assert(new_customers.len() == cdelta.len());
            assert forall|k: int| 0 <= k < new_customers.len() implies weight(#[trigger] new_customers[k]) == (|d: (CustomerRow, int)| d.1 * f(d.0))(cdelta[k]) by {
                assert(new_customers[k].sign as int == cdelta[k].1);
                assert(new_customers[k].custkey as int == cdelta[k].0.c_custkey);
            }
            lemma_sum_pointwise(new_customers, cdelta, weight, |d: (CustomerRow, int)| d.1 * f(d.0));
        }
    }
    let next = State { fast, orders: orders_log, customers: customers_log, appended: total as u64, db: Ghost(db2) };
    Ok(next)
}

}
