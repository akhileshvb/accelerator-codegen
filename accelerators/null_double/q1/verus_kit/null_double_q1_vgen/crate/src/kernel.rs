// A minimal kernel for exercising NULL-able and DOUBLE cells end to end: it keeps the
// table as a bag of rows and filters it on every execute.
#![allow(unused_imports)]
use vstd::prelude::*;
use vstd::multiset::Multiset;
use verus_common::bag::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::f64bits::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;

verus! {

pub struct State {
    rows: Vec<NumRt>,
    db: Ghost<Db>,
}

impl State {
    pub closed spec fn db(self) -> Db {
        self.db@
    }

    #[verifier::type_invariant]
    closed spec fn holds_db(self) -> bool {
        rows_v(self.rows@).to_multiset() =~= self.db@.num.to_multiset()
    }
}

pub open spec fn history(s: State) -> nat {
    0
}

// ── Rows as a bag ──

fn same_int(a: i64, a_null: u8, b: i64, b_null: u8) -> (r: bool)
    ensures
        r == ((if a_null != 0 { None } else { Some(a as int) }) == (if b_null != 0 { None } else { Some(b as int) })),
{
    if a_null != 0 || b_null != 0 { a_null != 0 && b_null != 0 } else { a == b }
}

fn finite(bits: u64) -> (r: bool)
    ensures
        r == f64_finite(bits),
{
    proof { lemma_pow2_consts(); }
    (bits / 4503599627370496) % 2048 != 2047
}

fn zero(bits: u64) -> (r: bool)
    ensures
        r == (f64_mag_bits(bits) == 0),
{
    proof { lemma_pow2_consts(); }
    bits % 9223372036854775808 == 0
}

fn same_double(a: u64, a_null: u8, b: u64, b_null: u8) -> (r: bool)
    ensures
        r == ((if a_null != 0 { None } else { Some(f64_val(a)) }) == (if b_null != 0 { None } else { Some(f64_val(b)) })),
{
    if a_null != 0 || b_null != 0 {
        a_null != 0 && b_null != 0
    } else {
        proof {
            lemma_f64_in_range(a);
            lemma_f64_in_range(b);
            if f64_finite(a) && f64_finite(b) { lemma_f64_val_eq(a, b); }
        }
        if finite(a) && finite(b) { a == b || (zero(a) && zero(b)) } else { !finite(a) && !finite(b) }
    }
}

impl BagRow for NumRt {
    type V = NumRow;

    open spec fn rv(&self) -> NumRow {
        num_rt_view(*self)
    }

    fn same(&self, o: &Self) -> (r: bool) {
        same_int(self.ddate, self.ddate__null, o.ddate, o.ddate__null)
            && same_int(self.qtrs, self.qtrs__null, o.qtrs, o.qtrs__null)
            && same_double(self.value, self.value__null, o.value, o.value__null)
    }

    fn dup(&self) -> (r: Self) {
        *self
    }
}

// ── The query as a filter ──

spec fn keep(x: NumRow) -> bool {
    x.qtrs is Some && x.qtrs->0 == 4
}

spec fn out_of(x: NumRow) -> OutRow {
    OutRow { ddate: x.ddate, qtrs: x.qtrs, value: x.value }
}

spec fn answer_of(num: Seq<NumRow>) -> Seq<OutRow> {
    rows(Db { num }, params())
}

spec fn kept(x: NumRow) -> Seq<OutRow> {
    if keep(x) { seq![out_of(x)] } else { Seq::empty() }
}

proof fn lemma_answer_push(num: Seq<NumRow>, x: NumRow)
    ensures
        answer_of(num.push(x)) =~= answer_of(num) + kept(x),
{
    let (db, db2) = (Db { num }, Db { num: num.push(x) });
    assert forall|y: NumRow| tuples_1(db2, params(), y) == tuples_1(db, params(), y) by {}
    assert(db2.num.map_values(|y: NumRow| tuples_1(db2, params(), y))
        =~= db.num.map_values(|y: NumRow| tuples_1(db, params(), y)).push(tuples_1(db, params(), x)));
    let m = db.num.map_values(|y: NumRow| tuples_1(db, params(), y));
    assert(flat(m.push(tuples_1(db, params(), x))) =~= flat(m) + tuples_1(db, params(), x)) by {
        assert(m.push(tuples_1(db, params(), x)).drop_last() =~= m);
    }
}

/// How often an output row occurs in the answer: the filter's weight summed over the table.
proof fn lemma_answer_count(num: Seq<NumRow>, o: OutRow)
    ensures
        answer_of(num).to_multiset().count(o) == sum(num, |x: NumRow| ind(keep(x) && out_of(x) == o)),
    decreases num.len(),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;

    if num.len() == 0 {
        assert(answer_of(num) =~= Seq::<OutRow>::empty());
    } else {
        let (init, x) = (num.drop_last(), num.last());
        lemma_answer_count(init, o);
        lemma_answer_push(init, x);
        assert(num =~= init.push(x));
        lemma_to_multiset_concat(answer_of(init), kept(x));
        if keep(x) {
            assert(kept(x) =~= seq![out_of(x)]);
            assert(kept(x).to_multiset() =~= Multiset::empty().insert(out_of(x)));
        } else {
            assert(kept(x).to_multiset() =~= Multiset::<OutRow>::empty());
        }
    }
}

/// The answer is a function of the table as a bag.
proof fn lemma_answer_bag(a: Seq<NumRow>, b: Seq<NumRow>)
    requires
        a.to_multiset() =~= b.to_multiset(),
    ensures
        answer_of(a).to_multiset() =~= answer_of(b).to_multiset(),
{
    broadcast use vstd::multiset::group_multiset_axioms;

    assert forall|o: OutRow| answer_of(a).to_multiset().count(o) == answer_of(b).to_multiset().count(o) by {
        lemma_answer_count(a, o);
        lemma_answer_count(b, o);
        lemma_sum_perm(a, b, |x: NumRow| ind(keep(x) && out_of(x) == o));
    }
}

// ── The entry points ──

pub fn build(
    num: &[NumRt],
    Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires
        scan_num_ok(db, num@),
    ensures
        r matches Some(s) ==> s.db() == db,
        r matches Some(s) ==> crate::kernel::history(s) == 0,
        envelope(db) ==> r is Some,
{
    let mut rows: Vec<NumRt> = Vec::new();
    let mut i = 0;
    while i < num.len()
        invariant
            i <= num@.len(),
            rows@ =~= num@.take(i as int),
        decreases num@.len() - i,
    {
        rows.push(num[i]);
        i += 1;
    }
    proof {
        assert(num@.take(i as int) =~= num@);
        assert(num_rt_seq_view(num@) =~= rows_v(rows@));
        lemma_sums_eq_multiset(num_rt_seq_view(num@), db.num);
    }
    Some(State { rows, db: Ghost(db) })
}

pub fn execute(
    s: &State,
) -> (r: Option<Vec<(Option<i64>, Option<i64>, Option<u64>)>>)
    ensures
        r matches Some(out) ==> query_result(s.db(), params(), out_view(out@)),
        envelope(s.db()) && answer_fits(s.db(), params()) ==> r is Some,
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
{
    proof { use_type_invariant(s); }
    let mut out: Vec<(Option<i64>, Option<i64>, Option<u64>)> = Vec::new();
    let mut i = 0;
    while i < s.rows.len()
        invariant
            i <= s.rows@.len(),
            s.holds_db(),
            out_view(out@) =~= answer_of(rows_v(s.rows@.take(i as int))),
            forall|j: int| 0 <= j < out.len() ==> out_fits(#[trigger] out_view(out@)[j]),
        decreases s.rows@.len() - i,
    {
        let row = &s.rows[i];
        proof {
            lemma_answer_push(rows_v(s.rows@.take(i as int)), row.rv());
            assert(s.rows@.take(i as int + 1) =~= s.rows@.take(i as int).push(*row));
            assert(rows_v(s.rows@.take(i as int + 1)) =~= rows_v(s.rows@.take(i as int)).push(row.rv()));
        }
        if row.qtrs__null == 0 && row.qtrs == 4 {
            if row.value__null == 0 && !finite(row.value) {
                proof {
                    lemma_f64_in_range(row.value);
                    broadcast use vstd::seq_lib::group_to_multiset_ensures;
                    assert(rows_v(s.rows@)[i as int] == row.rv());
                    assert(rows_v(s.rows@).contains(row.rv()));
                    assert(rows_v(s.rows@).to_multiset().count(row.rv()) > 0);
                    assert(s.db().num.contains(row.rv()));
                    let k = choose|k: int| 0 <= k < s.db().num.len() && s.db().num[k] == row.rv();
                    assert(!num_in_type(s.db().num[k]));
                }
                return None;
            }
            let ddate = if row.ddate__null == 0 { Some(row.ddate) } else { None };
            let value = if row.value__null == 0 { Some(row.value) } else { None };
            let ghost before = out@;
            out.push((ddate, Some(row.qtrs), value));
            proof {
                if row.value__null == 0 { lemma_f64_in_range(row.value); }
                assert(out_view(out@) =~= out_view(before).push(out_of(row.rv())));
                assert forall|j: int| 0 <= j < out.len() implies out_fits(#[trigger] out_view(out@)[j]) by {
                    if j < before.len() { assert(out_view(out@)[j] == out_view(before)[j]); }
                }
            }
        }
        i += 1;
    }
    proof {
        assert(s.rows@.take(i as int) =~= s.rows@);
        lemma_answer_bag(rows_v(s.rows@), s.db().num);
        assert(answer_of(s.db().num) =~= rows(s.db(), params()));
    }
    Some(out)
}

pub fn apply(
    s: &State,
    num_delta: &[NumDelta],
    Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires
        capture_ok(s.db(), db2, num_delta_seq_view(num_delta@)),
    ensures
        r matches Ok(s2) ==> s2.db() == db2,
        r matches Ok(s2) ==> crate::kernel::history(s2) <= crate::kernel::history(*s) + num_delta@.len(),
        envelope(s.db()) && envelope(db2) && crate::kernel::history(*s) + num_delta@.len() <= max_history() ==> r is Ok,
{
    proof { use_type_invariant(s); }
    let mut changes: Vec<(NumRt, i64)> = Vec::new();
    let mut i = 0;
    while i < num_delta.len()
        invariant
            i <= num_delta@.len(),
            dview(changes@) =~= num_delta_seq_view(num_delta@.take(i as int)),
        decreases num_delta@.len() - i,
    {
        let ghost before = changes@;
        changes.push((num_delta[i].row, num_delta[i].sign));
        proof {
            let d = num_delta@[i as int];
            assert(num_delta@.take(i as int + 1) =~= num_delta@.take(i as int).push(d));
            assert(dview(changes@) =~= dview(before).push((num_rt_view(d.row), d.sign as int)));
            assert(num_delta_seq_view(num_delta@.take(i as int + 1))
                =~= num_delta_seq_view(num_delta@.take(i as int)).push((num_rt_view(d.row), d.sign as int)));
        }
        i += 1;
    }
    proof {
        assert(num_delta@.take(i as int) =~= num_delta@);
        assert(unit_signs(dview(changes@)));
        lemma_bag_step_multiset(s.db().num, db2.num, dview(changes@));
    }
    match apply_bag(&s.rows, &changes) {
        Some(rows) => {
            proof { lemma_apply_bag_step(s.rows@, rows@, changes@, s.db().num, db2.num); }
            Ok(State { rows, db: Ghost(db2) })
        },
        None => {
            proof {
                broadcast use vstd::multiset::group_multiset_axioms;
                let x = choose|x: NumRow| #[trigger] minus_rows(dview(changes@)).to_multiset().count(x)
                    > rows_v(s.rows@).to_multiset().add(plus_rows(dview(changes@)).to_multiset()).count(x);
                assert(db2.num.to_multiset().add(minus_rows(dview(changes@)).to_multiset()).count(x)
                    == s.db().num.to_multiset().add(plus_rows(dview(changes@)).to_multiset()).count(x));
                assert(false);
            }
            Err(0)
        },
    }
}

} // verus!
