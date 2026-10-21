// Name classes with exact counts: the part names of each part key, and the nation rows.
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::storage::*;
use crate::values::*;
use verus_prooflib::wide::Wide;
use crate::bagsig::signed;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct NCell { pub k: i64, pub name: Vec<u8>, pub count: Wide }

pub open spec fn ncell_w(f: spec_fn((int, Seq<u8>)) -> int) -> spec_fn(NCell) -> int {
    |e: NCell| e.count@ * f((e.k as int, e.name@))
}

/// The weighted count of the cells: each cell's count times f of its (key, name).
pub open spec fn ncell_sum(rows: Seq<NCell>, f: spec_fn((int, Seq<u8>)) -> int) -> int {
    sum(rows, ncell_w(f))
}

pub fn copy_ncells(rows: &Vec<NCell>) -> (r: Vec<NCell>)
    ensures forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] ncell_sum(r@, f) == ncell_sum(rows@, f),
{
    let mut r: Vec<NCell> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@.len() == i,
            forall|j: int| 0 <= j < i ==> r@[j].k == rows@[j].k && r@[j].name@ == rows@[j].name@ && r@[j].count@ == rows@[j].count@,
        decreases rows.len() - i,
    {
        r.push(NCell { k: rows[i].k, name: copy_bytes(&rows[i].name), count: rows[i].count.dup() });
        i += 1;
    }
    proof {
        assert forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] ncell_sum(r@, f) == ncell_sum(rows@, f) by {
            lemma_ncell_ext(r@, rows@, f);
        }
    }
    r
}

proof fn lemma_ncell_ext(s: Seq<NCell>, t: Seq<NCell>, f: spec_fn((int, Seq<u8>)) -> int)
    requires s.len() == t.len(), forall|j: int| 0 <= j < s.len() ==> s[j].k == t[j].k && s[j].name@ == t[j].name@ && s[j].count@ == t[j].count@,
    ensures ncell_sum(s, f) == ncell_sum(t, f),
    decreases s.len(),
{
    if s.len() > 0 { lemma_ncell_ext(s.drop_last(), t.drop_last(), f); }
}

/// rows += d of (k, name).
pub fn bump_ncell(rows: &mut Vec<NCell>, k: i64, name: &Vec<u8>, d: &Wide)
    ensures forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] ncell_sum(final(rows)@, f)
        == ncell_sum(old(rows)@, f) + d@ * f((k as int, name@)),
{
    let ghost before = rows@;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), rows@ == before, before == old(rows)@,
        decreases rows.len() - index,
    {
        if rows[index].k == k && bytes_equal(&rows[index].name, name) {
            let entry = NCell { k, name: copy_bytes(name), count: rows[index].count.add(d) };
            let ghost replacement = entry;
            rows.set(index, entry);
            proof {
                assert forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] ncell_sum(rows@, f) == ncell_sum(before, f) + d@ * f((k as int, name@)) by {
                    lemma_sum_update(before, index as int, replacement, ncell_w(f));
                    let c = before[index as int].count@;
                    assert(before[index as int].k == k && before[index as int].name@ == name@);
                    assert((c + d@) * f((k as int, name@)) == c * f((k as int, name@)) + d@ * f((k as int, name@))) by (nonlinear_arith);
                }
            }
            return;
        }
        index += 1;
    }
    let entry = NCell { k, name: copy_bytes(name), count: d.dup() };
    let ghost appended = entry;
    rows.push(entry);
    proof {
        assert forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] ncell_sum(rows@, f) == ncell_sum(before, f) + d@ * f((k as int, name@)) by {
            lemma_sum_push(before, appended, ncell_w(f));
        }
    }
}

pub type NPages = Pages<Vec<NCell>>;
pub type NOv = Work<Vec<NCell>>;

pub open spec fn pn_at(m: NPages, key: i64, f: spec_fn((int, Seq<u8>)) -> int) -> int {
    match m.at(key) { Some(rows) => ncell_sum(rows@, f), None => 0int }
}

pub open spec fn pn_ov_at(base: NPages, ov: NOv, key: i64, f: spec_fn((int, Seq<u8>)) -> int) -> int {
    if ov.rows@.contains_key(key) { ncell_sum(ov.rows@[key]@, f) } else { pn_at(base, key, f) }
}

pub fn pn_empty() -> (r: NPages)
    ensures r.valid(), forall|k: i64| #[trigger] r.at(k).is_none(),
{
    let w: NOv = Work::new();
    Pages::from_work(w)
}

pub fn pn_add(base: &NPages, ov: &mut NOv, key: i64, name: &Vec<u8>, d: &Wide)
    requires base.valid(), old(ov).valid(),
    ensures final(ov).valid(),
        forall|k: i64, f: spec_fn((int, Seq<u8>)) -> int| #[trigger] pn_ov_at(*base, *final(ov), k, f)
            == pn_ov_at(*base, *old(ov), k, f) + if k == key { d@ * f((key as int, name@)) } else { 0 },
{
    let ghost before = *ov;
    let mut rows = match ov.take(key) {
        Some(rows) => rows,
        None => match base.get(key) { Some(rows) => copy_ncells(&rows), None => Vec::new() },
    };
    proof {
        if !before.rows@.contains_key(key) && base.at(key).is_none() {
            assert forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] ncell_sum(rows@, f) == pn_ov_at(*base, before, key, f) by {
                lemma_sum_empty(rows@, ncell_w(f));
            }
        }
    }
    bump_ncell(&mut rows, key, name, d);
    ov.put(key, rows);
}

pub fn pn_publish(base: &NPages, ov: NOv) -> (r: NPages)
    requires base.valid(), ov.valid(),
    ensures r.valid(), forall|k: i64, f: spec_fn((int, Seq<u8>)) -> int| #[trigger] pn_at(r, k, f) == pn_ov_at(*base, ov, k, f),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let mut next = base.share();
    let mut work = ov;
    let ghost initial = work;
    let mut index = 0;
    while index < work.keys.len()
        invariant index <= work.keys.len(), initial.valid(), work.valid(), next.valid(), work.keys@ == initial.keys@,
            forall|key: i64| #[trigger] work.rows@.contains_key(key)
                <==> initial.rows@.contains_key(key) && !initial.keys@.take(index as int).contains(key),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> work.rows@[key] == initial.rows@[key],
            forall|key: i64| #[trigger] next.at(key) == if initial.keys@.take(index as int).contains(key) && initial.rows@.contains_key(key) {
                Some(initial.rows@[key])
            } else { base.at(key) },
        decreases work.keys.len() - index,
    {
        let key = work.keys[index];
        proof { assert(!initial.keys@.take(index as int).contains(key)); }
        let rows = work.take(key);
        match rows { Some(rows) => next.put(key, Some(rows)), None => {} }
        proof {
            assert(initial.keys@.take(index as int + 1) =~= initial.keys@.take(index as int).push(key));
            verus_prooflib::lemma_push_membership(initial.keys@.take(index as int), key);
            assert forall|target: i64| #[trigger] next.at(target) == if initial.keys@.take(index as int + 1).contains(target) && initial.rows@.contains_key(target) {
                Some(initial.rows@[target])
            } else { base.at(target) } by {}
            assert forall|target: i64| #[trigger] work.rows@.contains_key(target)
                <==> initial.rows@.contains_key(target) && !initial.keys@.take(index as int + 1).contains(target) by {}
        }
        index += 1;
    }
    proof {
        assert(initial.keys@.take(index as int) =~= initial.keys@);
        assert forall|k: i64, f: spec_fn((int, Seq<u8>)) -> int| #[trigger] pn_at(next, k, f) == pn_ov_at(*base, initial, k, f) by {
            if initial.rows@.contains_key(k) { assert(initial.keys@.contains(k)); }
        }
    }
    next
}

pub open spec fn part_w(key: int, f: spec_fn((int, Seq<u8>)) -> int) -> spec_fn(PartRow) -> int {
    |r: PartRow| if r.p_partkey == key { f((key, r.p_name)) } else { 0int }
}

/// The names of the part keys: per key, the weighted count of its part rows.
pub open spec fn pnames_state(m: NPages, db: Db) -> bool {
    m.valid() && forall|k: i64, f: spec_fn((int, Seq<u8>)) -> int| #[trigger] pn_at(m, k, f) == sum(db.part, part_w(k as int, f))
}

pub fn ingest_parts(base: &NPages, ov: &mut NOv, chunk: &Vec<PartDelta>)
    requires base.valid(), old(ov).valid(),
    ensures final(ov).valid(),
        forall|k: i64, f: spec_fn((int, Seq<u8>)) -> int| #[trigger] pn_ov_at(*base, *final(ov), k, f)
            == pn_ov_at(*base, *old(ov), k, f) + sum(part_delta_seq_view(chunk@), signed(part_w(k as int, f))),
{
    let ghost start = *ov;
    let mut i = 0;
    proof {
        assert forall|k: i64, f: spec_fn((int, Seq<u8>)) -> int| #[trigger] pn_ov_at(*base, *ov, k, f) == pn_ov_at(*base, start, k, f) + sum(part_delta_seq_view(chunk@).take(0), signed(part_w(k as int, f))) by {
            lemma_sum_empty(part_delta_seq_view(chunk@).take(0), signed(part_w(k as int, f)));
        }
    }
    while i < chunk.len()
        invariant i <= chunk.len(), base.valid(), ov.valid(), start == *old(ov),
            forall|k: i64, f: spec_fn((int, Seq<u8>)) -> int| #[trigger] pn_ov_at(*base, *ov, k, f) == pn_ov_at(*base, start, k, f) + sum(part_delta_seq_view(chunk@).take(i as int), signed(part_w(k as int, f))),
        decreases chunk.len() - i,
    {
        let d = &chunk[i];
        let sgn = Wide::from_i128(d.sign as i128);
        pn_add(base, ov, d.row.p_partkey, &d.row.p_name, &sgn);
        proof {
            let view = part_delta_seq_view(chunk@);
            assert forall|k: i64, f: spec_fn((int, Seq<u8>)) -> int| #[trigger] pn_ov_at(*base, *ov, k, f) == pn_ov_at(*base, start, k, f) + sum(view.take(i as int + 1), signed(part_w(k as int, f))) by {
                lemma_sum_take(view, i as int, signed(part_w(k as int, f)));
                assert(view[i as int] == (part_in_view(d.row), d.sign as int));
                if k == d.row.p_partkey { assert(signed(part_w(k as int, f))(view[i as int]) == d.sign as int * f((k as int, d.row.p_name@))); }
            }
        }
        i += 1;
    }
    proof { assert(part_delta_seq_view(chunk@).take(chunk@.len() as int) =~= part_delta_seq_view(chunk@)); }
}

pub open spec fn nation_wt(f: spec_fn((int, Seq<u8>)) -> int) -> spec_fn(NationRow) -> int {
    |n: NationRow| f((n.n_nationkey, n.n_name))
}

pub open spec fn nations_state(v: Seq<NCell>, db: Db) -> bool {
    forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] ncell_sum(v, f) == sum(db.nation, nation_wt(f))
}

/// The nation rows after the deltas of a transaction.
pub fn ingest_nations(rows: &Vec<NCell>, chunk: &Vec<NationDelta>) -> (r: Vec<NCell>)
    ensures forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] ncell_sum(r@, f)
        == ncell_sum(rows@, f) + sum(nation_delta_seq_view(chunk@), signed(nation_wt(f))),
{
    let mut r = copy_ncells(rows);
    let mut i = 0;
    proof {
        assert forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] ncell_sum(r@, f) == ncell_sum(rows@, f) + sum(nation_delta_seq_view(chunk@).take(0), signed(nation_wt(f))) by {
            lemma_sum_empty(nation_delta_seq_view(chunk@).take(0), signed(nation_wt(f)));
        }
    }
    while i < chunk.len()
        invariant i <= chunk.len(),
            forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] ncell_sum(r@, f) == ncell_sum(rows@, f) + sum(nation_delta_seq_view(chunk@).take(i as int), signed(nation_wt(f))),
        decreases chunk.len() - i,
    {
        let d = &chunk[i];
        let sgn = Wide::from_i128(d.sign as i128);
        bump_ncell(&mut r, d.row.n_nationkey, &d.row.n_name, &sgn);
        proof {
            let view = nation_delta_seq_view(chunk@);
            assert forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] ncell_sum(r@, f) == ncell_sum(rows@, f) + sum(view.take(i as int + 1), signed(nation_wt(f))) by {
                lemma_sum_take(view, i as int, signed(nation_wt(f)));
                assert(view[i as int] == (nation_in_view(d.row), d.sign as int));
            }
        }
        i += 1;
    }
    proof { assert(nation_delta_seq_view(chunk@).take(chunk@.len() as int) =~= nation_delta_seq_view(chunk@)); }
    r
}

}
