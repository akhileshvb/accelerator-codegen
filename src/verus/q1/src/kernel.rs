// Stateless Q1 kernel: a streaming group-by over lineitem batches.
//
// A `Q1State` carries (as ghost) the rows it has consumed so far.  Its
// invariant ties the executable accumulators to those rows: slot
// k = (rf - 'A') * 26 + (ls - 'A') holds, for the group (rf, ls), exactly the spec's group
// sums and COUNT(*) (dense direct-addressed slots, no hashing).  Every sum is
// an exact i128; per-row input bounds (checked, out-of-domain rows make the
// kernel refuse) plus a row-count bound prove that no sum overflows.
//
//   new(cutoff)            empty state
//   consume(batch)         rows := rows ++ batch        (or refuse)
//       blocked: each block of <= BLOCK rows is summed into i64 block-local
//       slots (narrow domain: |qty|, |price| < 2^36, |disc|, |tax| < 2^7
//       hundredths), then the touched slots are folded into the i128 slots
//   consume_simple(batch)  same contract, i128 slots updated per row
//       (wider domain: 2^46 / 2^16)
//   merge(other)           rows := rows ++ other.rows   (or refuse)
//   finish()               gen_spec::query_result for the consumed rows
use vstd::prelude::*;
use verus_common::bytes::*;
use verus_common::sums::*;
use verus_common::rat::*;
use crate::gen_spec::*;
use crate::bridge::*;

verus! {

/// Slots: one per (l_returnflag, l_linestatus) pair of upper-case ASCII
/// letters.  A qualifying row whose flags are not both in 'A'..'Z' makes the
/// kernel refuse (the host then answers with SQL).
pub const NSLOTS: usize = 676;

/// At most 2^40 rows per state.
pub const MAX_ROWS: u64 = 0x100_0000_0000;

/// |l_quantity|, |l_extendedprice| < 2^46 (in hundredths).
pub const VAL_LIM: i64 = 0x4000_0000_0000;

/// |l_discount|, |l_tax| < 2^16 (in hundredths).
pub const RATE_LIM: i64 = 0x1_0000;

// Per-row bounds on each summand, and the resulting bounds on a sum over n rows.
pub open spec fn b_val() -> int { 0x4000_0000_0000 }  // 2^46
pub open spec fn b_dp() -> int { 0x8000_0000_0000_0000 }  // 2^63
pub open spec fn b_ch() -> int { 0x1_0000_0000_0000_0000_0000 }  // 2^80
pub open spec fn b_rate() -> int { 0x1_0000 }  // 2^16

// Blocked consume: a narrower numeric domain so that one block's sums fit in
// i64 (the chunk-local accumulators of the C++ kernel, proved).
/// |l_quantity|, |l_extendedprice| < 2^36 (in hundredths).
pub const LOC_VAL_LIM: i64 = 0x10_0000_0000;

/// |l_discount|, |l_tax| < 2^7 (in hundredths).
pub const LOC_RATE_LIM: i64 = 0x80;

/// Rows per block.
pub const BLOCK: usize = 2048;

pub open spec fn s_val() -> int { 0x10_0000_0000 }  // 2^36
pub open spec fn s_dp() -> int { 0x1000_0000_0000 }  // 2^44
pub open spec fn s_ch() -> int { 0x10_0000_0000_0000 }  // 2^52
pub open spec fn s_rate() -> int { 0x80 }  // 2^7

#[derive(Clone, Copy)]
pub struct Acc {
    pub qty: i128,
    pub base: i128,
    pub disc_price: i128,
    pub charge: i128,
    pub disc: i128,
    pub count: i64,
}

/// A block-local accumulator (i64 sums over at most BLOCK rows).
#[derive(Clone, Copy)]
pub struct Loc {
    pub qty: i64,
    pub base: i64,
    pub disc_price: i64,
    pub charge: i64,
    pub disc: i64,
    pub count: i64,
}

pub open spec fn loc_zero() -> Loc {
    Loc { qty: 0, base: 0, disc_price: 0, charge: 0, disc: 0, count: 0 }
}

/// One output group, as it crosses the FFI boundary.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct OutGroup {
    pub sum_qty: i128,
    pub sum_base_price: i128,
    pub sum_disc_price: i128,
    pub sum_charge: i128,
    pub sum_disc: i128,
    pub count: i64,
    pub rf: u8,
    pub ls: u8,
}

/// An output group as the generated spec's output row: AVG columns are the
/// exact rationals sum / count (the host formats them).
pub open spec fn group_view(o: OutGroup) -> OutRow {
    OutRow {
        l_returnflag: seq![o.rf],
        l_linestatus: seq![o.ls],
        sum_qty: o.sum_qty as int,
        sum_base_price: o.sum_base_price as int,
        sum_disc_price: o.sum_disc_price as int,
        sum_charge: o.sum_charge as int,
        avg_qty: Rat { num: o.sum_qty as int, den: o.count as int * 100 },
        avg_price: Rat { num: o.sum_base_price as int, den: o.count as int * 100 },
        avg_disc: Rat { num: o.sum_disc as int, den: o.count as int * 100 },
        count_order: o.count as int,
    }
}

pub open spec fn out_view(s: Seq<OutGroup>) -> Seq<OutRow> {
    s.map_values(|o: OutGroup| group_view(o))
}

// ── Rows of a columnar batch ────────────────────────────────────────────────

pub open spec fn row_at(
    sd: Seq<i32>,
    q: Seq<i64>,
    p: Seq<i64>,
    d: Seq<i64>,
    t: Seq<i64>,
    rf: Seq<u8>,
    ls: Seq<u8>,
    i: int,
) -> LineitemRow {
    LineitemRow {
        l_shipdate: sd[i] as int,
        l_quantity: q[i] as int,
        l_extendedprice: p[i] as int,
        l_discount: d[i] as int,
        l_tax: t[i] as int,
        l_returnflag: seq![rf[i]],
        l_linestatus: seq![ls[i]],
    }
}

/// The batch as spec rows: row i is (sd[i], q[i], p[i], d[i], t[i], rf[i], ls[i]).
pub open spec fn batch_view(
    sd: Seq<i32>,
    q: Seq<i64>,
    p: Seq<i64>,
    d: Seq<i64>,
    t: Seq<i64>,
    rf: Seq<u8>,
    ls: Seq<u8>,
) -> Seq<LineitemRow> {
    Seq::new(sd.len(), |i: int| row_at(sd, q, p, d, t, rf, ls, i))
}

// ── Slots ───────────────────────────────────────────────────────────────────

pub open spec fn slot_rf(k: int) -> Seq<u8> {
    seq![(k / 26 + 65) as u8]
}

pub open spec fn slot_ls(k: int) -> Seq<u8> {
    seq![(k % 26 + 65) as u8]
}

pub open spec fn is_letter(b: u8) -> bool {
    65 <= b <= 90
}

/// The slot of a row with single-letter flags.
pub open spec fn key_of(l: LineitemRow) -> int {
    (l.l_returnflag[0] as int - 65) * 26 + (l.l_linestatus[0] as int - 65)
}

/// Every row has single-byte flags, and every row that passes the filter has
/// upper-case letter flags (the kernel refuses otherwise).
pub open spec fn row_keys_ok(l: LineitemRow, c: int) -> bool {
    &&& l.l_returnflag.len() == 1
    &&& l.l_linestatus.len() == 1
    &&& qualifies(l, c) ==> is_letter(l.l_returnflag[0]) && is_letter(l.l_linestatus[0])
}

pub open spec fn keys_ok(rows: Seq<LineitemRow>, c: int) -> bool {
    forall|i: int| 0 <= i < rows.len() ==> row_keys_ok(#[trigger] rows[i], c)
}

/// Slot k's accumulator is group (slot_rf(k), slot_ls(k)) over `rows`, and
/// its sums are bounded by n times the per-row bounds.
pub open spec fn slot_ok(rows: Seq<LineitemRow>, c: int, k: int, a: Acc, n: int) -> bool {
    &&& a.count as int == gsum(rows, c, slot_rf(k), slot_ls(k), f_one())
    &&& a.qty as int == gsum(rows, c, slot_rf(k), slot_ls(k), f_qty())
    &&& a.base as int == gsum(rows, c, slot_rf(k), slot_ls(k), f_base())
    &&& a.disc_price as int == gsum(rows, c, slot_rf(k), slot_ls(k), f_disc_price())
    &&& a.charge as int == gsum(rows, c, slot_rf(k), slot_ls(k), f_charge())
    &&& a.disc as int == gsum(rows, c, slot_rf(k), slot_ls(k), f_disc())
    &&& 0 <= a.count <= n
    &&& -n * b_val() <= a.qty <= n * b_val()
    &&& -n * b_val() <= a.base <= n * b_val()
    &&& -n * b_dp() <= a.disc_price <= n * b_dp()
    &&& -n * b_ch() <= a.charge <= n * b_ch()
    &&& -n * b_rate() <= a.disc <= n * b_rate()
}

/// Block-local slot k is group (slot_rf(k), slot_ls(k)) over the block's
/// rows `blk` (m of them), with the narrow per-row bounds.
pub open spec fn loc_ok(blk: Seq<LineitemRow>, c: int, k: int, a: Loc, m: int) -> bool {
    &&& a.count as int == gsum(blk, c, slot_rf(k), slot_ls(k), f_one())
    &&& a.qty as int == gsum(blk, c, slot_rf(k), slot_ls(k), f_qty())
    &&& a.base as int == gsum(blk, c, slot_rf(k), slot_ls(k), f_base())
    &&& a.disc_price as int == gsum(blk, c, slot_rf(k), slot_ls(k), f_disc_price())
    &&& a.charge as int == gsum(blk, c, slot_rf(k), slot_ls(k), f_charge())
    &&& a.disc as int == gsum(blk, c, slot_rf(k), slot_ls(k), f_disc())
    &&& 0 <= a.count <= m
    &&& -m * s_val() <= a.qty <= m * s_val()
    &&& -m * s_val() <= a.base <= m * s_val()
    &&& -m * s_dp() <= a.disc_price <= m * s_dp()
    &&& -m * s_ch() <= a.charge <= m * s_ch()
    &&& -m * s_rate() <= a.disc <= m * s_rate()
}

pub open spec fn not_in(ts: Seq<usize>, k: int) -> bool {
    forall|i: int| 0 <= i < ts.len() ==> ts[i] as int != k
}

pub open spec fn distinct_slots(ts: Seq<usize>) -> bool {
    &&& forall|i: int| 0 <= i < ts.len() ==> (#[trigger] ts[i]) < NSLOTS
    &&& forall|i: int, j: int| 0 <= i < j < ts.len() ==> #[trigger] ts[i] != #[trigger] ts[j]
}

pub open spec fn slot_group(k: int, a: Acc) -> OutRow {
    OutRow {
        l_returnflag: slot_rf(k),
        l_linestatus: slot_ls(k),
        sum_qty: a.qty as int,
        sum_base_price: a.base as int,
        sum_disc_price: a.disc_price as int,
        sum_charge: a.charge as int,
        avg_qty: Rat { num: a.qty as int, den: a.count as int * 100 },
        avg_price: Rat { num: a.base as int, den: a.count as int * 100 },
        avg_disc: Rat { num: a.disc as int, den: a.count as int * 100 },
        count_order: a.count as int,
    }
}

// ── Lemmas ──────────────────────────────────────────────────────────────────

pub proof fn lemma_gsum_empty(db: Seq<LineitemRow>, c: int, rf: Seq<u8>, ls: Seq<u8>, f: spec_fn(LineitemRow) -> int)
    requires
        db.len() == 0,
    ensures
        gsum(db, c, rf, ls, f) == 0,
{
    lemma_sum_empty(db, |l: LineitemRow| if in_group(l, c, rf, ls) { f(l) } else { 0 });
}

pub proof fn lemma_gsum_push(
    db: Seq<LineitemRow>,
    x: LineitemRow,
    c: int,
    rf: Seq<u8>,
    ls: Seq<u8>,
    f: spec_fn(LineitemRow) -> int,
)
    ensures
        gsum(db.push(x), c, rf, ls, f) == gsum(db, c, rf, ls, f) + (if in_group(x, c, rf, ls) {
            f(x)
        } else {
            0
        }),
{
    lemma_sum_push(db, x, |l: LineitemRow| if in_group(l, c, rf, ls) { f(l) } else { 0 });
}

pub proof fn lemma_gsum_concat(
    a: Seq<LineitemRow>,
    b: Seq<LineitemRow>,
    c: int,
    rf: Seq<u8>,
    ls: Seq<u8>,
    f: spec_fn(LineitemRow) -> int,
)
    ensures
        gsum(a + b, c, rf, ls, f) == gsum(a, c, rf, ls, f) + gsum(b, c, rf, ls, f),
{
    lemma_sum_concat(a, b, |l: LineitemRow| if in_group(l, c, rf, ls) { f(l) } else { 0 });
}

/// A row is in slot k's group iff it passes the filter and k is its slot.
pub proof fn lemma_slot_match(l: LineitemRow, c: int, k: int)
    requires
        0 <= k < NSLOTS,
        row_keys_ok(l, c),
    ensures
        in_group(l, c, slot_rf(k), slot_ls(k)) == (qualifies(l, c) && k == key_of(l)),
{
    if qualifies(l, c) {
        let x = l.l_returnflag[0];
        let y = l.l_linestatus[0];
        assert(l.l_returnflag =~= seq![x]);
        assert(l.l_linestatus =~= seq![y]);
        let kk = key_of(l);
        assert(0 <= k / 26 < 26);
        assert(0 <= k % 26 < 26);
        assert(k == 26 * (k / 26) + k % 26);
        if k == kk {
            assert(k / 26 == x as int - 65);
            assert(k % 26 == y as int - 65);
            assert(slot_rf(k) =~= seq![x]);
            assert(slot_ls(k) =~= seq![y]);
        } else {
            if slot_rf(k) == l.l_returnflag && slot_ls(k) == l.l_linestatus {
                assert(slot_rf(k)[0] == x);
                assert(slot_ls(k)[0] == y);
            }
        }
    }
}

pub proof fn lemma_mul_bound(a: int, b: int, ba: int, bb: int)
    requires
        0 < ba,
        0 < bb,
        -ba < a < ba,
        -bb < b < bb,
    ensures
        -(ba * bb) < a * b < ba * bb,
{
    if a >= 0 && b >= 0 {
        assert(a * b <= a * bb) by (nonlinear_arith)
            requires a >= 0, b < bb;
        assert(a * bb < ba * bb) by (nonlinear_arith)
            requires a < ba, bb > 0;
    } else if a < 0 && b < 0 {
        assert((-a) * (-b) <= (-a) * bb) by (nonlinear_arith)
            requires -a > 0, -b < bb;
        assert((-a) * bb < ba * bb) by (nonlinear_arith)
            requires -a < ba, bb > 0;
        assert(a * b == (-a) * (-b)) by (nonlinear_arith);
    } else if a >= 0 {
        assert(a * (-b) <= a * bb) by (nonlinear_arith)
            requires a >= 0, -b < bb;
        assert(a * bb < ba * bb) by (nonlinear_arith)
            requires a < ba, bb > 0;
        assert(a * b == -(a * (-b))) by (nonlinear_arith);
    } else {
        assert((-a) * b <= (-a) * bb) by (nonlinear_arith)
            requires -a > 0, b < bb;
        assert((-a) * bb < ba * bb) by (nonlinear_arith)
            requires -a < ba, bb > 0;
        assert(a * b == -((-a) * b)) by (nonlinear_arith);
    }
}

pub proof fn lemma_bytes_lt_single(x: u8, y: u8)
    ensures
        bytes_lt(seq![x], seq![y]) == (x < y),
{
    reveal_with_fuel(bytes_lt, 2);
    assert(seq![x].drop_first() =~= Seq::<u8>::empty());
    assert(seq![y].drop_first() =~= Seq::<u8>::empty());
}

/// Increasing slot index is increasing ORDER BY key.
pub proof fn lemma_slot_order(k1: int, k2: int, a1: Acc, a2: Acc)
    requires
        0 <= k1 < k2 < NSLOTS,
    ensures
        row_lt(slot_group(k1, a1), slot_group(k2, a2)),
{
    let x1 = (k1 / 26 + 65) as u8;
    let x2 = (k2 / 26 + 65) as u8;
    let y1 = (k1 % 26 + 65) as u8;
    let y2 = (k2 % 26 + 65) as u8;
    assert(k1 == 26 * (k1 / 26) + k1 % 26);
    assert(k2 == 26 * (k2 / 26) + k2 % 26);
    assert(0 <= k1 / 26 < 26 && 0 <= k2 / 26 < 26);
    lemma_bytes_lt_single(x1, x2);
    lemma_bytes_lt_single(y1, y2);
    if k1 / 26 == k2 / 26 {
        assert(slot_rf(k1) =~= slot_rf(k2));
    }
}

/// Pushing row l keeps every slot other than its own unchanged, and adds its
/// summands to its own slot (when it passes the filter).
pub proof fn lemma_push_slot(rows: Seq<LineitemRow>, l: LineitemRow, c: int, k: int)
    requires
        0 <= k < NSLOTS,
        row_keys_ok(l, c),
    ensures
        ({
            let hit = qualifies(l, c) && k == key_of(l);
            let r2 = rows.push(l);
            let rf = slot_rf(k);
            let ls = slot_ls(k);
            &&& gsum(r2, c, rf, ls, f_one()) == gsum(rows, c, rf, ls, f_one()) + (if hit { 1int } else { 0 })
            &&& gsum(r2, c, rf, ls, f_qty()) == gsum(rows, c, rf, ls, f_qty()) + (if hit { l.l_quantity } else { 0 })
            &&& gsum(r2, c, rf, ls, f_base()) == gsum(rows, c, rf, ls, f_base()) + (if hit { l.l_extendedprice } else { 0 })
            &&& gsum(r2, c, rf, ls, f_disc_price()) == gsum(rows, c, rf, ls, f_disc_price()) + (if hit { disc_price(l) } else { 0 })
            &&& gsum(r2, c, rf, ls, f_charge()) == gsum(rows, c, rf, ls, f_charge()) + (if hit { charge(l) } else { 0 })
            &&& gsum(r2, c, rf, ls, f_disc()) == gsum(rows, c, rf, ls, f_disc()) + (if hit { l.l_discount } else { 0 })
        }),
{
    lemma_slot_match(l, c, k);
    let rf = slot_rf(k);
    let ls = slot_ls(k);
    lemma_gsum_push(rows, l, c, rf, ls, f_one());
    lemma_gsum_push(rows, l, c, rf, ls, f_qty());
    lemma_gsum_push(rows, l, c, rf, ls, f_base());
    lemma_gsum_push(rows, l, c, rf, ls, f_disc_price());
    lemma_gsum_push(rows, l, c, rf, ls, f_charge());
    lemma_gsum_push(rows, l, c, rf, ls, f_disc());
}

// ── State ───────────────────────────────────────────────────────────────────

pub struct Q1State {
    pub cutoff: i32,
    pub slots: Vec<Acc>,
    pub loc: Vec<Loc>,
    pub touched: Vec<usize>,
    pub n: u64,
    pub rows: Ghost<Seq<LineitemRow>>,
}

impl Q1State {
    pub open spec fn inv(&self) -> bool {
        &&& self.slots@.len() == NSLOTS
        &&& self.loc@.len() == NSLOTS
        &&& self.touched@.len() == 0
        &&& forall|k: int| 0 <= k < NSLOTS ==> #[trigger] self.loc@[k] == loc_zero()
        &&& self.n as int == self.rows@.len()
        &&& self.n <= MAX_ROWS
        &&& keys_ok(self.rows@, self.cutoff as int)
        &&& forall|k: int|
            0 <= k < NSLOTS ==> slot_ok(self.rows@, self.cutoff as int, k, #[trigger] self.slots@[k], self.n as int)
    }

    pub fn new(cutoff: i32) -> (s: Q1State)
        ensures
            s.inv(),
            s.cutoff == cutoff,
            s.rows@ == Seq::<LineitemRow>::empty(),
    {
        let zero = Acc { qty: 0, base: 0, disc_price: 0, charge: 0, disc: 0, count: 0 };
        let mut slots: Vec<Acc> = Vec::with_capacity(NSLOTS);
        let mut k: usize = 0;
        while k < NSLOTS
            invariant
                k <= NSLOTS,
                slots@.len() == k,
                forall|j: int| 0 <= j < k ==> #[trigger] slots@[j] == zero,
            decreases NSLOTS - k,
        {
            slots.push(zero);
            k += 1;
        }
        let lz = Loc { qty: 0, base: 0, disc_price: 0, charge: 0, disc: 0, count: 0 };
        assert(lz == loc_zero());
        let mut loc: Vec<Loc> = Vec::with_capacity(NSLOTS);
        let mut k: usize = 0;
        while k < NSLOTS
            invariant
                k <= NSLOTS,
                loc@.len() == k,
                lz == loc_zero(),
                forall|j: int| 0 <= j < k ==> #[trigger] loc@[j] == loc_zero(),
            decreases NSLOTS - k,
        {
            loc.push(lz);
            k += 1;
        }
        let s = Q1State { cutoff, slots, loc, touched: Vec::with_capacity(NSLOTS), n: 0, rows: Ghost(Seq::empty()) };
        assert forall|k: int| 0 <= k < NSLOTS implies slot_ok(s.rows@, cutoff as int, k, #[trigger] s.slots@[k], 0) by {
            let rf = slot_rf(k);
            let ls = slot_ls(k);
            lemma_gsum_empty(s.rows@, cutoff as int, rf, ls, f_one());
            lemma_gsum_empty(s.rows@, cutoff as int, rf, ls, f_qty());
            lemma_gsum_empty(s.rows@, cutoff as int, rf, ls, f_base());
            lemma_gsum_empty(s.rows@, cutoff as int, rf, ls, f_disc_price());
            lemma_gsum_empty(s.rows@, cutoff as int, rf, ls, f_charge());
            lemma_gsum_empty(s.rows@, cutoff as int, rf, ls, f_disc());
        }
        s
    }

    /// Consume one columnar batch.  Returns false (state unusable) if a
    /// qualifying row is outside the kernel's numeric domain or the row
    /// budget would be exceeded; the host then answers with SQL.
    pub fn consume_simple(
        &mut self,
        sd: &[i32],
        q: &[i64],
        p: &[i64],
        d: &[i64],
        t: &[i64],
        rf: &[u8],
        ls: &[u8],
    ) -> (ok: bool)
        requires
            old(self).inv(),
            q@.len() == sd@.len(),
            p@.len() == sd@.len(),
            d@.len() == sd@.len(),
            t@.len() == sd@.len(),
            rf@.len() == sd@.len(),
            ls@.len() == sd@.len(),
        ensures
            ok ==> final(self).inv(),
            ok ==> final(self).cutoff == old(self).cutoff,
            ok ==> final(self).rows@ == old(self).rows@ + batch_view(sd@, q@, p@, d@, t@, rf@, ls@),
    {
        let len = sd.len();
        if len as u64 > MAX_ROWS - self.n {
            return false;
        }
        let cutoff = self.cutoff;
        let ghost rows0 = self.rows@;
        let ghost batch = batch_view(sd@, q@, p@, d@, t@, rf@, ls@);
        let mut i: usize = 0;
        while i < len
            invariant
                i <= len,
                len == sd@.len(),
                q@.len() == len,
                p@.len() == len,
                d@.len() == len,
                t@.len() == len,
                rf@.len() == len,
                ls@.len() == len,
                batch == batch_view(sd@, q@, p@, d@, t@, rf@, ls@),
                self.inv(),
                self.cutoff == cutoff,
                self.rows@ == rows0 + batch.take(i as int),
                self.n + (len - i) <= MAX_ROWS,
            decreases len - i,
        {
            let ghost l = row_at(sd@, q@, p@, d@, t@, rf@, ls@, i as int);
            let ghost r0 = self.rows@;
            let ghost n0 = self.n as int;
            let ghost s0 = self.slots@;
            let ghost kk = (rf@[i as int] as int - 65) * 26 + (ls@[i as int] as int - 65);
            let hit = sd[i] <= cutoff;
            if hit {
                let qv = q[i];
                let pv = p[i];
                let dv = d[i];
                let tv = t[i];
                if !(-VAL_LIM < qv && qv < VAL_LIM && -VAL_LIM < pv && pv < VAL_LIM && -RATE_LIM < dv
                    && dv < RATE_LIM && -RATE_LIM < tv && tv < RATE_LIM) {
                    return false;
                }
                let fa = rf[i];
                let fb = ls[i];
                if fa < 65 || fa > 90 || fb < 65 || fb > 90 {
                    return false;
                }
                let k = (fa - 65) as usize * 26 + (fb - 65) as usize;
                proof {
                    lemma_mul_bound(pv as int, 100 - dv as int, b_val(), 0x2_0000);
                }
                let dp: i128 = (pv as i128) * ((100 - dv) as i128);
                proof {
                    lemma_mul_bound(dp as int, 100 + tv as int, b_dp(), 0x2_0000);
                }
                let ch: i128 = dp * ((100 + tv) as i128);
                let a = self.slots[k];
                proof {
                    assert(slot_ok(r0, cutoff as int, k as int, s0[k as int], n0));
                }
                let na = Acc {
                    qty: a.qty + qv as i128,
                    base: a.base + pv as i128,
                    disc_price: a.disc_price + dp,
                    charge: a.charge + ch,
                    disc: a.disc + dv as i128,
                    count: a.count + 1,
                };
                self.slots.set(k, na);
            }
            proof {
                assert(l.l_returnflag.len() == 1);
                assert(l.l_linestatus.len() == 1);
                assert(l.l_returnflag[0] == rf@[i as int]);
                assert(l.l_linestatus[0] == ls@[i as int]);
                assert(qualifies(l, cutoff as int) == hit);
                assert(row_keys_ok(l, cutoff as int));
                assert(hit ==> key_of(l) == kk);
                let r1 = r0.push(l);
                let n1 = n0 + 1;
                assert forall|k: int| 0 <= k < NSLOTS implies slot_ok(r1, cutoff as int, k, #[trigger] self.slots@[k], n1) by {
                    lemma_push_slot(r0, l, cutoff as int, k);
                    assert(slot_ok(r0, cutoff as int, k, s0[k], n0));
                    if !(hit && k == kk) {
                        assert(self.slots@[k] == s0[k]);
                    } else {
                        assert(disc_price(l) == l.l_extendedprice * (100 - l.l_discount));
                        assert(charge(l) == disc_price(l) * (100 + l.l_tax));
                    }
                }
                assert forall|j: int| 0 <= j < r1.len() implies row_keys_ok(#[trigger] r1[j], cutoff as int) by {
                    if j < r0.len() {
                        assert(r1[j] == r0[j]);
                    }
                }
                assert(batch.take(i as int + 1) =~= batch.take(i as int).push(l));
                assert(rows0 + batch.take(i as int + 1) =~= r1);
            }
            self.rows = Ghost(self.rows@.push(l));
            self.n = self.n + 1;
            i += 1;
        }
        proof {
            assert(batch.take(len as int) =~= batch);
        }
        true
    }

    /// Consume one columnar batch, in blocks of at most BLOCK rows: each block
    /// is summed into i64 block-local slots (the narrow domain), then the
    /// touched slots are folded into the i128 slots.  Returns false (state
    /// unusable) if a qualifying row is outside the narrow domain or the row
    /// budget would be exceeded; the host then answers with SQL.
    pub fn consume(
        &mut self,
        sd: &[i32],
        q: &[i64],
        p: &[i64],
        d: &[i64],
        t: &[i64],
        rf: &[u8],
        ls: &[u8],
    ) -> (ok: bool)
        requires
            old(self).inv(),
            q@.len() == sd@.len(),
            p@.len() == sd@.len(),
            d@.len() == sd@.len(),
            t@.len() == sd@.len(),
            rf@.len() == sd@.len(),
            ls@.len() == sd@.len(),
        ensures
            ok ==> final(self).inv(),
            ok ==> final(self).cutoff == old(self).cutoff,
            ok ==> final(self).rows@ == old(self).rows@ + batch_view(sd@, q@, p@, d@, t@, rf@, ls@),
    {
        let len = sd.len();
        if len as u64 > MAX_ROWS - self.n {
            return false;
        }
        let ghost batch = batch_view(sd@, q@, p@, d@, t@, rf@, ls@);
        let ghost rows0 = self.rows@;
        let ghost cutoff = self.cutoff;
        let mut start: usize = 0;
        while start < len
            invariant
                start <= len,
                len == sd@.len(),
                q@.len() == len,
                p@.len() == len,
                d@.len() == len,
                t@.len() == len,
                rf@.len() == len,
                ls@.len() == len,
                batch == batch_view(sd@, q@, p@, d@, t@, rf@, ls@),
                self.inv(),
                self.cutoff == cutoff,
                self.rows@ == rows0 + batch.take(start as int),
                self.n + (len - start) <= MAX_ROWS,
            decreases len - start,
        {
            let end = if len - start > BLOCK { start + BLOCK } else { len };
            if !self.consume_block(sd, q, p, d, t, rf, ls, start, end, Ghost(batch)) {
                return false;
            }
            proof {
                assert(rows0 + batch.take(end as int) =~= rows0 + batch.take(start as int) + batch.subrange(
                    start as int,
                    end as int,
                ));
            }
            start = end;
        }
        proof {
            assert(batch.take(len as int) =~= batch);
        }
        true
    }

    /// Phase-1 postcondition: block-local slots hold the block's group sums;
    /// the touched list names exactly the non-zero local slots.
    pub open spec fn block_summed(&self, blk: Seq<LineitemRow>, m: int) -> bool {
        &&& self.loc@.len() == NSLOTS
        &&& self.slots@.len() == NSLOTS
        &&& distinct_slots(self.touched@)
        &&& forall|k: int|
            0 <= k < NSLOTS ==> loc_ok(blk, self.cutoff as int, k, #[trigger] self.loc@[k], m)
        &&& forall|k: int| 0 <= k < NSLOTS && not_in(self.touched@, k) ==> #[trigger] self.loc@[k] == loc_zero()
        &&& forall|j: int| 0 <= j < self.touched@.len() ==> self.loc@[#[trigger] self.touched@[j] as int].count > 0
    }

    /// The i128 slots and ghost rows as in `inv`, without the block-local part.
    pub open spec fn global_ok(&self) -> bool {
        &&& self.slots@.len() == NSLOTS
        &&& self.n as int == self.rows@.len()
        &&& self.n <= MAX_ROWS
        &&& keys_ok(self.rows@, self.cutoff as int)
        &&& forall|k: int|
            0 <= k < NSLOTS ==> slot_ok(self.rows@, self.cutoff as int, k, #[trigger] self.slots@[k], self.n as int)
    }

    fn consume_block(
        &mut self,
        sd: &[i32],
        q: &[i64],
        p: &[i64],
        d: &[i64],
        t: &[i64],
        rf: &[u8],
        ls: &[u8],
        start: usize,
        end: usize,
        batch: Ghost<Seq<LineitemRow>>,
    ) -> (ok: bool)
        requires
            old(self).inv(),
            q@.len() == sd@.len(),
            p@.len() == sd@.len(),
            d@.len() == sd@.len(),
            t@.len() == sd@.len(),
            rf@.len() == sd@.len(),
            ls@.len() == sd@.len(),
            batch@ == batch_view(sd@, q@, p@, d@, t@, rf@, ls@),
            start <= end <= sd@.len(),
            end - start <= BLOCK,
            old(self).n + (end - start) <= MAX_ROWS,
        ensures
            ok ==> final(self).inv(),
            ok ==> final(self).cutoff == old(self).cutoff,
            ok ==> final(self).rows@ == old(self).rows@ + batch@.subrange(start as int, end as int),
    {
        if !self.block_accumulate(sd, q, p, d, t, rf, ls, start, end, batch) {
            return false;
        }
        let ghost blk = batch@.subrange(start as int, end as int);
        self.block_fold(Ghost(blk), end - start);
        true
    }

    /// Phase 1: sum rows start..end into the block-local slots.
    fn block_accumulate(
        &mut self,
        sd: &[i32],
        q: &[i64],
        p: &[i64],
        d: &[i64],
        t: &[i64],
        rf: &[u8],
        ls: &[u8],
        start: usize,
        end: usize,
        batch: Ghost<Seq<LineitemRow>>,
    ) -> (ok: bool)
        requires
            old(self).inv(),
            q@.len() == sd@.len(),
            p@.len() == sd@.len(),
            d@.len() == sd@.len(),
            t@.len() == sd@.len(),
            rf@.len() == sd@.len(),
            ls@.len() == sd@.len(),
            batch@ == batch_view(sd@, q@, p@, d@, t@, rf@, ls@),
            start <= end <= sd@.len(),
            end - start <= BLOCK,
        ensures
            ok ==> final(self).block_summed(batch@.subrange(start as int, end as int), end - start),
            ok ==> keys_ok(batch@.subrange(start as int, end as int), old(self).cutoff as int),
            ok ==> final(self).global_ok(),
            ok ==> final(self).slots@ == old(self).slots@,
            ok ==> final(self).rows@ == old(self).rows@,
            ok ==> final(self).n == old(self).n,
            ok ==> final(self).cutoff == old(self).cutoff,
    {
        let cutoff = self.cutoff;
        let ghost c = cutoff as int;
        // Redundant for the proof (the precondition says so); it lets the
        // compiler drop the per-access bounds checks in the loop below.
        let len = sd.len();
        if q.len() != len || p.len() != len || d.len() != len || t.len() != len || rf.len() != len || ls.len() != len {
            return false;
        }
        proof {
            assert forall|k: int| 0 <= k < NSLOTS implies loc_ok(batch@.subrange(start as int, start as int), c, k, #[trigger] self.loc@[k], 0) by {
                let e = batch@.subrange(start as int, start as int);
                let rf = slot_rf(k);
                let ls = slot_ls(k);
                lemma_gsum_empty(e, c, rf, ls, f_one());
                lemma_gsum_empty(e, c, rf, ls, f_qty());
                lemma_gsum_empty(e, c, rf, ls, f_base());
                lemma_gsum_empty(e, c, rf, ls, f_disc_price());
                lemma_gsum_empty(e, c, rf, ls, f_charge());
                lemma_gsum_empty(e, c, rf, ls, f_disc());
            }
        }
        let mut i: usize = start;
        while i < end
            invariant
                start <= i <= end,
                end <= sd@.len(),
                end - start <= BLOCK,
                q@.len() == sd@.len(),
                p@.len() == sd@.len(),
                d@.len() == sd@.len(),
                t@.len() == sd@.len(),
                rf@.len() == sd@.len(),
                ls@.len() == sd@.len(),
                batch@ == batch_view(sd@, q@, p@, d@, t@, rf@, ls@),
                self.cutoff == cutoff,
                c == cutoff as int,
                self.global_ok(),
                self.slots@ == old(self).slots@,
                self.rows@ == old(self).rows@,
                self.n == old(self).n,
                self.block_summed(batch@.subrange(start as int, i as int), i - start),
                keys_ok(batch@.subrange(start as int, i as int), c),
            decreases end - i,
        {
            let ghost l = row_at(sd@, q@, p@, d@, t@, rf@, ls@, i as int);
            let ghost blk = batch@.subrange(start as int, i as int);
            let ghost lo = self.loc@;
            let ghost t0 = self.touched@;
            let ghost m = i - start;
            let ghost kk = (rf@[i as int] as int - 65) * 26 + (ls@[i as int] as int - 65);
            let hit = sd[i] <= cutoff;
            if hit {
                let qv = q[i];
                let pv = p[i];
                let dv = d[i];
                let tv = t[i];
                if !(-LOC_VAL_LIM < qv && qv < LOC_VAL_LIM && -LOC_VAL_LIM < pv && pv < LOC_VAL_LIM
                    && -LOC_RATE_LIM < dv && dv < LOC_RATE_LIM && -LOC_RATE_LIM < tv && tv < LOC_RATE_LIM) {
                    return false;
                }
                let fa = rf[i];
                let fb = ls[i];
                if fa < 65 || fa > 90 || fb < 65 || fb > 90 {
                    return false;
                }
                let k = (fa - 65) as usize * 26 + (fb - 65) as usize;
                proof {
                    lemma_mul_bound(pv as int, 100 - dv as int, s_val(), 0x100);
                }
                let dp: i64 = pv * (100 - dv);
                proof {
                    lemma_mul_bound(dp as int, 100 + tv as int, s_dp(), 0x100);
                }
                let ch: i64 = dp * (100 + tv);
                let a = self.loc[k];
                proof {
                    assert(loc_ok(blk, c, k as int, lo[k as int], m));
                }
                if a.count == 0 {
                    proof {
                        assert forall|j: int| 0 <= j < t0.len() implies t0[j] != k by {
                            assert(lo[t0[j] as int].count > 0);
                        }
                    }
                    self.touched.push(k);
                }
                let na = Loc {
                    qty: a.qty + qv,
                    base: a.base + pv,
                    disc_price: a.disc_price + dp,
                    charge: a.charge + ch,
                    disc: a.disc + dv,
                    count: a.count + 1,
                };
                self.loc.set(k, na);
            }
            proof {
                assert(l.l_returnflag.len() == 1);
                assert(l.l_linestatus.len() == 1);
                assert(l.l_returnflag[0] == rf@[i as int]);
                assert(l.l_linestatus[0] == ls@[i as int]);
                assert(qualifies(l, c) == hit);
                assert(row_keys_ok(l, c));
                assert(hit ==> key_of(l) == kk);
                let b1 = blk.push(l);
                assert(b1 =~= batch@.subrange(start as int, i as int + 1));
                assert forall|j: int| 0 <= j < b1.len() implies row_keys_ok(#[trigger] b1[j], c) by {
                    if j < blk.len() {
                        assert(b1[j] == blk[j]);
                    }
                }
                assert forall|k: int| 0 <= k < NSLOTS implies loc_ok(b1, c, k, #[trigger] self.loc@[k], m + 1) by {
                    lemma_push_slot(blk, l, c, k);
                    assert(loc_ok(blk, c, k, lo[k], m));
                    if !(hit && k == kk) {
                        assert(self.loc@[k] == lo[k]);
                    } else {
                        assert(disc_price(l) == l.l_extendedprice * (100 - l.l_discount));
                        assert(charge(l) == disc_price(l) * (100 + l.l_tax));
                    }
                }
                let t1 = self.touched@;
                assert(t1 == t0 || t1 == t0.push(kk as usize));
                assert forall|j: int| 0 <= j < t0.len() implies t1[j] == t0[j] by {}
                assert forall|k: int| 0 <= k < NSLOTS && not_in(t1, k) implies #[trigger] self.loc@[k] == loc_zero() by {
                    assert forall|j: int| 0 <= j < t0.len() implies t0[j] as int != k by {
                        assert(t1[j] == t0[j]);
                    }
                    if hit && k == kk {
                        if t1 == t0 {
                            assert(lo[k].count > 0);
                        } else {
                            assert(t1[t0.len() as int] as int == k);
                        }
                    }
                }
                assert forall|j: int| 0 <= j < t1.len() implies self.loc@[#[trigger] t1[j] as int].count > 0 by {
                    if j < t0.len() {
                        assert(t1[j] == t0[j]);
                        assert(lo[t0[j] as int].count > 0);
                    }
                }
                assert(distinct_slots(t1));
            }
            i += 1;
        }
        true
    }

    /// Phase 2: fold the touched block-local slots into the i128 slots.
    fn block_fold(&mut self, blk: Ghost<Seq<LineitemRow>>, m: usize)
        requires
            old(self).global_ok(),
            old(self).block_summed(blk@, m as int),
            m <= BLOCK,
            blk@.len() == m,
            keys_ok(blk@, old(self).cutoff as int),
            old(self).n + m <= MAX_ROWS,
        ensures
            final(self).inv(),
            final(self).cutoff == old(self).cutoff,
            final(self).rows@ == old(self).rows@ + blk@,
    {
        let lz = Loc { qty: 0, base: 0, disc_price: 0, charge: 0, disc: 0, count: 0 };
        let ghost c = self.cutoff as int;
        let ghost g0 = self.slots@;
        let ghost l0 = self.loc@;
        let ghost ts = self.touched@;
        let ghost r0 = self.rows@;
        let ghost n0 = self.n as int;
        let nt = self.touched.len();
        let mut j: usize = 0;
        while j < nt
            invariant
                j <= nt,
                nt == ts.len(),
                self.touched@ == ts,
                distinct_slots(ts),
                lz == loc_zero(),
                self.slots@.len() == NSLOTS,
                self.loc@.len() == NSLOTS,
                self.cutoff as int == c,
                self.rows@ == r0,
                self.n as int == n0,
                g0.len() == NSLOTS,
                l0.len() == NSLOTS,
                n0 + m <= MAX_ROWS,
                m <= BLOCK,
                forall|k: int| 0 <= k < NSLOTS ==> slot_ok(r0, c, k, #[trigger] g0[k], n0),
                forall|k: int| 0 <= k < NSLOTS ==> loc_ok(blk@, c, k, #[trigger] l0[k], m as int),
                forall|i: int|
                    0 <= i < j ==> {
                        let k = #[trigger] ts[i] as int;
                        let x = self.slots@[k];
                        &&& x.count == g0[k].count + l0[k].count
                        &&& x.qty == g0[k].qty + l0[k].qty
                        &&& x.base == g0[k].base + l0[k].base
                        &&& x.disc_price == g0[k].disc_price + l0[k].disc_price
                        &&& x.charge == g0[k].charge + l0[k].charge
                        &&& x.disc == g0[k].disc + l0[k].disc
                        &&& self.loc@[k] == loc_zero()
                    },
                forall|k: int|
                    0 <= k < NSLOTS && not_in(ts.take(j as int), k) ==> #[trigger] self.slots@[k] == g0[k]
                        && self.loc@[k] == l0[k],
            decreases nt - j,
        {
            let k = self.touched[j];
            let ghost sl = self.slots@;
            let ghost lc = self.loc@;
            proof {
                assert forall|kk: int| 0 <= kk < NSLOTS && not_in(ts.take(j as int), kk) implies #[trigger] sl[kk] == g0[kk]
                    && lc[kk] == l0[kk] by {
                    assert(self.slots@[kk] == sl[kk]);
                }
            }
            proof {
                assert forall|i: int| 0 <= i < j implies ts.take(j as int)[i] as int != k as int by {
                    assert(ts.take(j as int)[i] == ts[i]);
                }
                assert(not_in(ts.take(j as int), k as int));
                assert(slot_ok(r0, c, k as int, g0[k as int], n0));
                assert(loc_ok(blk@, c, k as int, l0[k as int], m as int));
            }
            let a = self.slots[k];
            let b = self.loc[k];
            let x = Acc {
                qty: a.qty + b.qty as i128,
                base: a.base + b.base as i128,
                disc_price: a.disc_price + b.disc_price as i128,
                charge: a.charge + b.charge as i128,
                disc: a.disc + b.disc as i128,
                count: a.count + b.count,
            };
            self.slots.set(k, x);
            self.loc.set(k, lz);
            proof {
                assert forall|i: int| 0 <= i < j + 1 implies {
                    let kk = #[trigger] ts[i] as int;
                    let y = self.slots@[kk];
                    &&& y.count == g0[kk].count + l0[kk].count
                    &&& y.qty == g0[kk].qty + l0[kk].qty
                    &&& y.base == g0[kk].base + l0[kk].base
                    &&& y.disc_price == g0[kk].disc_price + l0[kk].disc_price
                    &&& y.charge == g0[kk].charge + l0[kk].charge
                    &&& y.disc == g0[kk].disc + l0[kk].disc
                    &&& self.loc@[kk] == loc_zero()
                } by {
                    if i < j {
                        assert(ts[i] != ts[j as int]);
                        assert(self.slots@[ts[i] as int] == sl[ts[i] as int]);
                        assert(self.loc@[ts[i] as int] == lc[ts[i] as int]);
                    }
                }
                let s1 = ts.take(j as int + 1);
                assert forall|kk: int| 0 <= kk < NSLOTS && not_in(s1, kk) implies #[trigger] self.slots@[kk] == g0[kk]
                    && self.loc@[kk] == l0[kk] by {
                    assert(s1[j as int] == k);
                    assert(kk != k as int);
                    assert forall|i: int| 0 <= i < j implies ts.take(j as int)[i] as int != kk by {
                        assert(ts.take(j as int)[i] == s1[i]);
                    }
                    assert(not_in(ts.take(j as int), kk));
                    assert(sl[kk] == g0[kk] && lc[kk] == l0[kk]);
                }
            }
            j += 1;
        }
        self.touched.clear();
        proof {
            let r1 = r0 + blk@;
            let n1 = n0 + m;
            assert(ts.take(nt as int) =~= ts);
            assert forall|k: int| 0 <= k < NSLOTS implies slot_ok(r1, c, k, #[trigger] self.slots@[k], n1)
                && self.loc@[k] == loc_zero() by {
                assert(slot_ok(r0, c, k, g0[k], n0));
                assert(loc_ok(blk@, c, k, l0[k], m as int));
                if not_in(ts, k) {
                    assert(l0[k] == loc_zero());
                } else {
                    let i = choose|i: int| 0 <= i < ts.len() && ts[i] as int == k;
                    assert(ts[i] as int == k);
                }
                let rf = slot_rf(k);
                let ls = slot_ls(k);
                lemma_gsum_concat(r0, blk@, c, rf, ls, f_one());
                lemma_gsum_concat(r0, blk@, c, rf, ls, f_qty());
                lemma_gsum_concat(r0, blk@, c, rf, ls, f_base());
                lemma_gsum_concat(r0, blk@, c, rf, ls, f_disc_price());
                lemma_gsum_concat(r0, blk@, c, rf, ls, f_charge());
                lemma_gsum_concat(r0, blk@, c, rf, ls, f_disc());
            }
            assert forall|j: int| 0 <= j < r1.len() implies row_keys_ok(#[trigger] r1[j], c) by {
                if j < r0.len() {
                    assert(r1[j] == r0[j]);
                } else {
                    assert(r1[j] == blk@[j - r0.len()]);
                }
            }
        }
        self.rows = Ghost(self.rows@ + blk@);
        self.n = self.n + m as u64;
        proof {
            assert(self.slots@.len() == NSLOTS);
            assert(self.loc@.len() == NSLOTS);
            assert(self.touched@.len() == 0);
            assert forall|k: int| 0 <= k < NSLOTS implies #[trigger] self.loc@[k] == loc_zero() by {
                let _ = self.slots@[k];
            }
            assert(self.n as int == self.rows@.len());
            assert(self.n <= MAX_ROWS);
            assert(keys_ok(self.rows@, self.cutoff as int));
            assert(forall|k: int|
                0 <= k < NSLOTS ==> slot_ok(self.rows@, self.cutoff as int, k, #[trigger] self.slots@[k], self.n as int));
        }
    }

    /// Fold `other`'s rows into this state (per-thread states, merged at the end).
    pub fn merge(&mut self, other: &Q1State) -> (ok: bool)
        requires
            old(self).inv(),
            other.inv(),
            other.cutoff == old(self).cutoff,
        ensures
            ok ==> final(self).inv(),
            ok ==> final(self).cutoff == old(self).cutoff,
            ok ==> final(self).rows@ == old(self).rows@ + other.rows@,
    {
        if self.n > MAX_ROWS - other.n {
            return false;
        }
        let ghost s0 = self.slots@;
        let ghost ra = self.rows@;
        let ghost rb = other.rows@;
        let ghost na = self.n as int;
        let ghost nb = other.n as int;
        let ghost c = self.cutoff as int;
        let mut k: usize = 0;
        while k < NSLOTS
            invariant
                k <= NSLOTS,
                self.slots@.len() == NSLOTS,
                other.inv(),
                other.rows@ == rb,
                other.n as int == nb,
                other.cutoff as int == c,
                na + nb <= MAX_ROWS,
                s0.len() == NSLOTS,
                forall|j: int| 0 <= j < NSLOTS ==> slot_ok(ra, c, j, #[trigger] s0[j], na),
                forall|j: int| k <= j < NSLOTS ==> #[trigger] self.slots@[j] == s0[j],
                forall|j: int|
                    0 <= j < k ==> {
                        let x = #[trigger] self.slots@[j];
                        let a = s0[j];
                        let b = other.slots@[j];
                        &&& x.count == a.count + b.count
                        &&& x.qty == a.qty + b.qty
                        &&& x.base == a.base + b.base
                        &&& x.disc_price == a.disc_price + b.disc_price
                        &&& x.charge == a.charge + b.charge
                        &&& x.disc == a.disc + b.disc
                    },
            decreases NSLOTS - k,
        {
            let a = self.slots[k];
            let b = other.slots[k];
            proof {
                assert(slot_ok(ra, c, k as int, s0[k as int], na));
                assert(slot_ok(rb, c, k as int, other.slots@[k as int], nb));
            }
            let x = Acc {
                qty: a.qty + b.qty,
                base: a.base + b.base,
                disc_price: a.disc_price + b.disc_price,
                charge: a.charge + b.charge,
                disc: a.disc + b.disc,
                count: a.count + b.count,
            };
            self.slots.set(k, x);
            k += 1;
        }
        proof {
            let r = ra + rb;
            let n = na + nb;
            assert forall|k: int| 0 <= k < NSLOTS implies slot_ok(r, c, k, #[trigger] self.slots@[k], n) by {
                assert(slot_ok(ra, c, k, s0[k], na));
                assert(slot_ok(rb, c, k, other.slots@[k], nb));
                let rf = slot_rf(k);
                let ls = slot_ls(k);
                lemma_gsum_concat(ra, rb, c, rf, ls, f_one());
                lemma_gsum_concat(ra, rb, c, rf, ls, f_qty());
                lemma_gsum_concat(ra, rb, c, rf, ls, f_base());
                lemma_gsum_concat(ra, rb, c, rf, ls, f_disc_price());
                lemma_gsum_concat(ra, rb, c, rf, ls, f_charge());
                lemma_gsum_concat(ra, rb, c, rf, ls, f_disc());
            }
            assert forall|j: int| 0 <= j < r.len() implies row_keys_ok(#[trigger] r[j], c) by {
                if j < ra.len() {
                    assert(r[j] == ra[j]);
                } else {
                    assert(r[j] == rb[j - ra.len()]);
                }
            }
        }
        self.rows = Ghost(self.rows@ + other.rows@);
        self.n = self.n + other.n;
        true
    }

    /// The answer: non-empty groups in slot order, which is ORDER BY order.
    pub fn finish(&self) -> (out: Vec<OutGroup>)
        requires
            self.inv(),
        ensures
            // the answer of the generated spec, for the rows in any day
            // encoding e and $1 = d that give this cutoff (bridge.rs)
            forall|e: int, d: int| self.cutoff + e == 10561 - d ==>
                #[trigger] query_result(db_at(self.rows@, e), params_of(d), out_view(out@)),
    {
        let ghost rows = self.rows@;
        let ghost c = self.cutoff as int;
        let mut out: Vec<OutGroup> = Vec::new();
        let ghost mut idx: Seq<int> = Seq::empty();
        let mut k: usize = 0;
        while k < NSLOTS
            invariant
                self.inv(),
                rows == self.rows@,
                c == self.cutoff as int,
                k <= NSLOTS,
                out@.len() == idx.len(),
                forall|i: int|
                    0 <= i < idx.len() ==> 0 <= #[trigger] idx[i] < k && self.slots@[idx[i]].count > 0
                        && group_view(out@[i]) == slot_group(idx[i], self.slots@[idx[i]]),
                forall|i: int, j: int| 0 <= i < j < idx.len() ==> #[trigger] idx[i] < #[trigger] idx[j],
                forall|j: int|
                    0 <= j < k && (#[trigger] self.slots@[j]).count > 0 ==> exists|i: int|
                        0 <= i < idx.len() && idx[i] == j,
            decreases NSLOTS - k,
        {
            let a = self.slots[k];
            if a.count > 0 {
                let rfb = (k / 26 + 65) as u8;
                let lsb = (k % 26 + 65) as u8;
                let g = OutGroup {
                    sum_qty: a.qty,
                    sum_base_price: a.base,
                    sum_disc_price: a.disc_price,
                    sum_charge: a.charge,
                    sum_disc: a.disc,
                    count: a.count,
                    rf: rfb,
                    ls: lsb,
                };
                out.push(g);
                proof {
                    let i0 = idx.len() as int;
                    let idx0 = idx;
                    idx = idx.push(k as int);
                    assert(group_view(g).l_returnflag =~= slot_rf(k as int));
                    assert(group_view(g).l_linestatus =~= slot_ls(k as int));
                    assert(idx[i0] == k as int);
                    assert(group_view(out@[i0]) == slot_group(k as int, self.slots@[k as int]));
                    assert forall|j: int| 0 <= j < k + 1 && (#[trigger] self.slots@[j]).count > 0 implies exists|i: int|
                        0 <= i < idx.len() && idx[i] == j by {
                        if j == k {
                            assert(idx[i0] == j);
                        } else {
                            assert(0 <= j < k && self.slots@[j].count > 0);
                            let i1 = choose|i: int| 0 <= i < idx0.len() && idx0[i] == j;
                            assert(idx[i1] == idx0[i1]);
                        }
                    }
                }
            }
            k += 1;
        }
        proof {
            let ov = out_view(out@);
            assert forall|e: int, d: int| self.cutoff + e == 10561 - d implies
                #[trigger] query_result(db_at(rows, e), params_of(d), ov) by {
                let gd = db_at(rows, e);
                let pp = params_of(d);
                assert forall|i: int| 0 <= i < ov.len() implies row_ok(gd, pp, #[trigger] ov[i]) by {
                    assert(ov[i] == group_view(out@[i]));
                    assert(slot_ok(rows, c, idx[i], self.slots@[idx[i]], self.n as int));
                    let (rf, ls) = (ov[i].l_returnflag, ov[i].l_linestatus);
                    lemma_gcount_eq(rows, e, c, d, rf, ls);
                    lemma_gsum_eq(rows, e, c, d, rf, ls, f_qty(), |t: Tuple| t.lineitem.l_quantity);
                    lemma_gsum_eq(rows, e, c, d, rf, ls, f_base(), |t: Tuple| t.lineitem.l_extendedprice);
                    lemma_gsum_eq(rows, e, c, d, rf, ls, f_disc_price(), |t: Tuple| t.lineitem.l_extendedprice * (100 - t.lineitem.l_discount));
                    lemma_gsum_eq(rows, e, c, d, rf, ls, f_charge(), |t: Tuple| t.lineitem.l_extendedprice * (100 - t.lineitem.l_discount) * (100 + t.lineitem.l_tax));
                    lemma_gsum_eq(rows, e, c, d, rf, ls, f_disc(), |t: Tuple| t.lineitem.l_discount);
                }
                assert forall|i: int, j: int| 0 <= i < j < ov.len() implies
                    !same_group(#[trigger] ov[i], #[trigger] ov[j]) && !row_lt(ov[j], ov[i]) by {
                    assert(ov[i] == group_view(out@[i]));
                    assert(ov[j] == group_view(out@[j]));
                    assert(idx[i] < idx[j]);
                    lemma_slot_order(idx[i], idx[j], self.slots@[idx[i]], self.slots@[idx[j]]);
                    lemma_bytes_lt_irrefl(ov[i].l_returnflag);
                    lemma_bytes_lt_irrefl(ov[i].l_linestatus);
                    lemma_bytes_lt_asym(ov[i].l_returnflag, ov[j].l_returnflag);
                    lemma_bytes_lt_asym(ov[i].l_linestatus, ov[j].l_linestatus);
                }
                assert forall|rf: Seq<u8>, ls: Seq<u8>| #[trigger] group_count(gd, pp, rf, ls) > 0 implies exists|i: int|
                    0 <= i < ov.len() && (#[trigger] ov[i]).l_returnflag == rf && ov[i].l_linestatus == ls by {
                    lemma_gcount_eq(rows, e, c, d, rf, ls);
                    let f = |l: LineitemRow| if in_group(l, c, rf, ls) { f_one()(l) } else { 0 };
                    let w = lemma_sum_nonzero_witness(rows, f);
                    let l = rows[w];
                    assert(in_group(l, c, rf, ls));
                    assert(row_keys_ok(l, c));
                    let kk = key_of(l);
                    assert(0 <= kk < NSLOTS);
                    lemma_slot_match(l, c, kk);
                    assert(slot_rf(kk) =~= rf);
                    assert(slot_ls(kk) =~= ls);
                    assert(slot_ok(rows, c, kk, self.slots@[kk], self.n as int));
                    assert(self.slots@[kk].count > 0);
                    let i = choose|i: int| 0 <= i < idx.len() && idx[i] == kk;
                    assert(ov[i] == group_view(out@[i]));
                }
            }
        }
        out
    }
}

} // verus!
