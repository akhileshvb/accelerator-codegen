#![verifier::loop_isolation(false)]
// The general path: signed logs of every lineitem and supplier row, replayed on a read the
// bucket profiles cannot answer exactly.  Every sum is an exact integer.
use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::wide::Wide;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::log::*;
use crate::model::weight;
use crate::query_proofs::{window_weight, has_winner};
use crate::emission::{repeated, lemma_repeat_push, rows_ordered, lemma_output_push};
use crate::output::OutputRow;
use crate::values::copy_bytes as copy_vec;

verus! {

broadcast use vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties;

pub struct LineEntry { pub suppkey: i64, pub price: i64, pub discount: i64, pub shipdate: i32, pub sign: i64 }

pub struct SuppEntry { pub suppkey: i64, pub name: Arc<Vec<u8>>, pub address: Arc<Vec<u8>>, pub phone: Arc<Vec<u8>>, pub sign: i64 }

pub open spec fn line_row(entry: LineEntry) -> LineitemRow {
    LineitemRow { l_suppkey: entry.suppkey as int, l_extendedprice: entry.price as int,
        l_discount: entry.discount as int, l_shipdate: entry.shipdate as int }
}

pub open spec fn supp_row(entry: SuppEntry) -> SupplierRow {
    SupplierRow { s_suppkey: entry.suppkey as int, s_name: (*entry.name)@, s_address: (*entry.address)@, s_phone: (*entry.phone)@ }
}

pub open spec fn line_term(f: spec_fn(LineitemRow) -> int) -> spec_fn(LineEntry) -> int {
    |entry: LineEntry| entry.sign as int * f(line_row(entry))
}

pub open spec fn supp_term(f: spec_fn(SupplierRow) -> int) -> spec_fn(SuppEntry) -> int {
    |entry: SuppEntry| entry.sign as int * f(supp_row(entry))
}

pub open spec fn line_log_sum(entries: Seq<LineEntry>, f: spec_fn(LineitemRow) -> int) -> int {
    sum(entries, line_term(f))
}

pub open spec fn supp_log_sum(entries: Seq<SuppEntry>, f: spec_fn(SupplierRow) -> int) -> int {
    sum(entries, supp_term(f))
}

pub open spec fn unit_lines(entries: Seq<LineEntry>) -> bool {
    forall|index: int| 0 <= index < entries.len() ==> (#[trigger] entries[index]).sign == 1 || entries[index].sign == -1
}

pub open spec fn unit_supps(entries: Seq<SuppEntry>) -> bool {
    forall|index: int| 0 <= index < entries.len() ==> (#[trigger] entries[index]).sign == 1 || entries[index].sign == -1
}

pub proof fn lemma_chunk_unit_lines(chunks: Seq<Seq<LineEntry>>, index: int)
    requires 0 <= index < chunks.len(), unit_lines(flat(chunks)),
    ensures unit_lines(chunks[index]),
{
    assert forall|position: int| 0 <= position < chunks[index].len() implies
        (#[trigger] chunks[index][position]).sign == 1 || chunks[index][position].sign == -1 by {
        lemma_flat_contains(chunks, chunks[index][position]);
        assert(chunks[index].contains(chunks[index][position]));
        assert(flat(chunks).contains(chunks[index][position]));
        let found = choose|found: int| 0 <= found < flat(chunks).len() && flat(chunks)[found] == chunks[index][position];
    }
}

pub proof fn lemma_chunk_unit_supps(chunks: Seq<Seq<SuppEntry>>, index: int)
    requires 0 <= index < chunks.len(), unit_supps(flat(chunks)),
    ensures unit_supps(chunks[index]),
{
    assert forall|position: int| 0 <= position < chunks[index].len() implies
        (#[trigger] chunks[index][position]).sign == 1 || chunks[index][position].sign == -1 by {
        lemma_flat_contains(chunks, chunks[index][position]);
        assert(chunks[index].contains(chunks[index][position]));
        assert(flat(chunks).contains(chunks[index][position]));
        let found = choose|found: int| 0 <= found < flat(chunks).len() && flat(chunks)[found] == chunks[index][position];
    }
}

/// What the window sum of one supplier adds for a logged row.
pub open spec fn win_term(supplier: int, lower: int, upper: int, revenue: bool) -> spec_fn(LineEntry) -> int {
    |entry: LineEntry| entry.sign as int * window_weight(supplier, lower, upper, revenue)(line_row(entry))
}

/// A supplier's signed row count and exact revenue inside the window.
pub type Cell = (i128, Wide);

pub open spec fn cnt_at(map: Map<i64, Cell>, key: i64) -> int {
    if map.contains_key(key) { map[key].0 as int } else { 0 }
}

pub open spec fn rev_at(map: Map<i64, Cell>, key: i64) -> int {
    if map.contains_key(key) { map[key].1@ } else { 0 }
}

pub open spec fn cnt_within(map: Map<i64, Cell>, bound: int) -> bool {
    forall|key: i64| -bound <= #[trigger] cnt_at(map, key) <= bound
}

pub proof fn lemma_weight_fits(price: int, factor: int)
    requires -0x8000000000000000int <= price <= 0x7fffffffffffffffint, -0x7fffffffffffff9cint <= factor <= 0x8000000000000064int,
    ensures -0x7fffffffffffffffffffffffffffffffint <= price * factor <= 0x7fffffffffffffffffffffffffffffffint,
{
    assert(price * factor <= 0x8000000000000000int * 0x8000000000000064int) by (nonlinear_arith)
        requires -0x8000000000000000int <= price <= 0x7fffffffffffffffint, -0x7fffffffffffff9cint <= factor <= 0x8000000000000064int;
    assert(price * factor >= -(0x8000000000000000int * 0x8000000000000064int)) by (nonlinear_arith)
        requires -0x8000000000000000int <= price <= 0x7fffffffffffffffint, -0x7fffffffffffff9cint <= factor <= 0x8000000000000064int;
}


#[verifier::spinoff_prover]
#[verifier::rlimit(40)]
pub fn tally_chunk(cells: &mut HashMap<i64, Cell>, keys: &mut Vec<i64>, chunk: &Vec<LineEntry>, lower: i32, upper: i32, Ghost(done): Ghost<int>)
    requires cnt_within(old(cells)@, done), old(cells)@.dom() == old(keys)@.to_set(), old(keys)@.no_duplicates(),
        unit_lines(chunk@), 0 <= done, done + chunk@.len() < 0x10000000000000000000000000000000int,
    ensures cnt_within(final(cells)@, done + chunk@.len()), final(cells)@.dom() == final(keys)@.to_set(), final(keys)@.no_duplicates(),
        forall|key: i64| #[trigger] cnt_at(final(cells)@, key) == cnt_at(old(cells)@, key)
            + sum(chunk@, win_term(key as int, lower as int, upper as int, false)),
        forall|key: i64| #[trigger] rev_at(final(cells)@, key) == rev_at(old(cells)@, key)
            + sum(chunk@, win_term(key as int, lower as int, upper as int, true)),
        final(keys)@.len() <= old(keys)@.len() + chunk@.len(),
{
    let mut index = 0usize;
    proof {
        assert forall|key: i64| #[trigger] cnt_at(cells@, key) == cnt_at(old(cells)@, key) + sum(chunk@.take(0), win_term(key as int, lower as int, upper as int, false)) by {
            assert(chunk@.take(0) =~= Seq::<LineEntry>::empty());
            lemma_sum_empty(chunk@.take(0), win_term(key as int, lower as int, upper as int, false));
        }
        assert forall|key: i64| #[trigger] rev_at(cells@, key) == rev_at(old(cells)@, key) + sum(chunk@.take(0), win_term(key as int, lower as int, upper as int, true)) by {
            assert(chunk@.take(0) =~= Seq::<LineEntry>::empty());
            lemma_sum_empty(chunk@.take(0), win_term(key as int, lower as int, upper as int, true));
        }
    }
    while index < chunk.len()
        invariant index <= chunk.len(), unit_lines(chunk@), 0 <= done, done + chunk@.len() < 0x10000000000000000000000000000000int,
            cnt_within(cells@, done + index as int), cells@.dom() == keys@.to_set(), keys@.no_duplicates(),
            keys@.len() <= old(keys)@.len() + index,
            forall|key: i64| #[trigger] cnt_at(cells@, key) == cnt_at(old(cells)@, key)
                + sum(chunk@.take(index as int), win_term(key as int, lower as int, upper as int, false)),
            forall|key: i64| #[trigger] rev_at(cells@, key) == rev_at(old(cells)@, key)
                + sum(chunk@.take(index as int), win_term(key as int, lower as int, upper as int, true)),
        decreases chunk.len() - index,
    {
        let entry = &chunk[index];
        let ghost before = cells@;
        proof { assert(chunk@[index as int].sign == 1 || chunk@[index as int].sign == -1); keys@.to_set_ensures(); }
        let inside = lower <= entry.shipdate && entry.shipdate < upper;
        let ghost mut delta_rev = 0int;
        if inside {
            if !cells.contains_key(&entry.suppkey) {
                proof { verus_prooflib::lemma_unique_push(keys@, entry.suppkey); }
                keys.push(entry.suppkey);
            }
            let (count, revenue) = match cells.get(&entry.suppkey) { Some(cell) => (cell.0, cell.1.dup()), None => (0, Wide::zero()) };
            proof {
                assert(-(done + index as int) <= cnt_at(before, entry.suppkey) <= done + index as int);
                assert(count as int == cnt_at(before, entry.suppkey));
                assert(revenue@ == rev_at(before, entry.suppkey));
                lemma_weight_fits(entry.price as int, 100 - entry.discount as int);
            }
            let factor = 100i128 - entry.discount as i128;
            let product = entry.price as i128 * factor;
            let signed = if entry.sign == 1 { product } else { -product };
            cells.insert(entry.suppkey, (count + entry.sign as i128, revenue.add(&Wide::from_i128(signed))));
            proof {
                delta_rev = signed as int;
                assert(product as int == entry.price as int * (100 - entry.discount as int));
                assert(signed as int == entry.sign as int * (entry.price as int * (100 - entry.discount as int)));
            }
        }
        proof {
            keys@.to_set_ensures();
            assert(cells@.dom() =~= keys@.to_set());
            assert forall|key: i64| #[trigger] cnt_at(cells@, key) == cnt_at(old(cells)@, key)
                + sum(chunk@.take(index as int + 1), win_term(key as int, lower as int, upper as int, false)) by {
                lemma_sum_take(chunk@, index as int, win_term(key as int, lower as int, upper as int, false));
                let term = win_term(key as int, lower as int, upper as int, false)(chunk@[index as int]);
                assert(cnt_at(before, key) == cnt_at(old(cells)@, key) + sum(chunk@.take(index as int), win_term(key as int, lower as int, upper as int, false)));
                if inside && key == entry.suppkey {
                    assert(cnt_at(cells@, key) == cnt_at(before, key) + entry.sign as int);
                    assert(term == entry.sign as int * 1);
                } else {
                    assert(cnt_at(cells@, key) == cnt_at(before, key));
                    assert(term == 0);
                }
            }
            assert forall|key: i64| #[trigger] rev_at(cells@, key) == rev_at(old(cells)@, key)
                + sum(chunk@.take(index as int + 1), win_term(key as int, lower as int, upper as int, true)) by {
                lemma_sum_take(chunk@, index as int, win_term(key as int, lower as int, upper as int, true));
                let term = win_term(key as int, lower as int, upper as int, true)(chunk@[index as int]);
                assert(rev_at(before, key) == rev_at(old(cells)@, key) + sum(chunk@.take(index as int), win_term(key as int, lower as int, upper as int, true)));
                if inside && key == entry.suppkey {
                    assert(term == entry.sign as int * (entry.price as int * (100 - entry.discount as int)));
                    assert(rev_at(cells@, key) == rev_at(before, key) + delta_rev);
                } else {
                    assert(rev_at(cells@, key) == rev_at(before, key));
                    assert(term == 0);
                }
            }
            assert forall|key: i64| -(done + index as int + 1) <= #[trigger] cnt_at(cells@, key) <= done + index as int + 1 by {
                assert(-(done + index as int) <= cnt_at(before, key) <= done + index as int);
            }
        }
        index += 1;
    }
    proof { assert(chunk@.take(chunk@.len() as int) =~= chunk@); }
}


/// Per-supplier window counts and revenues of the whole lineitem log, with the suppliers seen.
#[verifier::spinoff_prover]
#[verifier::rlimit(40)]
pub fn tally_lines(log: &Log<LineEntry>, lower: i32, upper: i32) -> (result: (HashMap<i64, Cell>, Vec<i64>))
    requires unit_lines(log.entries()), log.entries().len() < 0x1000000000000000000000000int,
    ensures ({ let (cells, keys) = result;
        &&& forall|key: i64| #[trigger] cnt_at(cells@, key) == sum(log.entries(), win_term(key as int, lower as int, upper as int, false))
        &&& forall|key: i64| #[trigger] rev_at(cells@, key) == sum(log.entries(), win_term(key as int, lower as int, upper as int, true))
        &&& cnt_within(cells@, log.entries().len() as int)
        &&& cells@.dom() == keys@.to_set()
        &&& keys@.no_duplicates()
        &&& keys@.len() <= log.entries().len() }),
{
    let mut cells: HashMap<i64, Cell> = HashMap::new();
    let mut keys: Vec<i64> = Vec::new();
    let ghost views = log.views();
    let mut index = 0usize;
    proof {
        assert(views.take(0) =~= Seq::<Seq<LineEntry>>::empty());
        assert forall|key: i64| #[trigger] cnt_at(cells@, key) == sum(flat(views.take(0)), win_term(key as int, lower as int, upper as int, false)) by {
            lemma_sum_empty(flat(views.take(0)), win_term(key as int, lower as int, upper as int, false));
        }
        assert forall|key: i64| #[trigger] rev_at(cells@, key) == sum(flat(views.take(0)), win_term(key as int, lower as int, upper as int, true)) by {
            lemma_sum_empty(flat(views.take(0)), win_term(key as int, lower as int, upper as int, true));
        }
        keys@.to_set_ensures();
        assert(cells@.dom() =~= keys@.to_set());
        assert(flat(views.take(0)).len() == 0);
    }
    while index < log.chunks.len()
        invariant index <= log.chunks.len(), views == log.views(), views.len() == log.chunks.len(),
            unit_lines(log.entries()), log.entries().len() < 0x1000000000000000000000000int,
            cnt_within(cells@, flat(views.take(index as int)).len() as int),
            cells@.dom() == keys@.to_set(), keys@.no_duplicates(),
            keys@.len() <= flat(views.take(index as int)).len(),
            forall|key: i64| #[trigger] cnt_at(cells@, key) == sum(flat(views.take(index as int)), win_term(key as int, lower as int, upper as int, false)),
            forall|key: i64| #[trigger] rev_at(cells@, key) == sum(flat(views.take(index as int)), win_term(key as int, lower as int, upper as int, true)),
        decreases log.chunks.len() - index,
    {
        let chunk = &log.chunks[index];
        proof {
            lemma_chunk_unit_lines(views, index as int);
            lemma_flat_prefix_len(views, index as int + 1);
            lemma_flat_step(views, index as int);
            assert(views[index as int] == (**chunk)@);
        }
        tally_chunk(&mut cells, &mut keys, chunk, lower, upper, Ghost(flat(views.take(index as int)).len() as int));
        proof {
            let after = flat(views.take(index as int + 1));
            assert forall|key: i64| #[trigger] cnt_at(cells@, key) == sum(after, win_term(key as int, lower as int, upper as int, false)) by {
                lemma_sum_concat(flat(views.take(index as int)), views[index as int], win_term(key as int, lower as int, upper as int, false));
            }
            assert forall|key: i64| #[trigger] rev_at(cells@, key) == sum(after, win_term(key as int, lower as int, upper as int, true)) by {
                lemma_sum_concat(flat(views.take(index as int)), views[index as int], win_term(key as int, lower as int, upper as int, true));
            }
            assert(after.len() == flat(views.take(index as int)).len() + chunk@.len());
        }
        index += 1;
    }
    proof { assert(views.take(views.len() as int) =~= views); }
    (cells, keys)
}


/// One distinct (name, address, phone) of a supplier, with its signed row count.
pub struct Variant2 { pub name: Arc<Vec<u8>>, pub address: Arc<Vec<u8>>, pub phone: Arc<Vec<u8>>, pub count: i128 }

pub open spec fn same_triple(variant: Variant2, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>) -> bool {
    (*variant.name)@ == name && (*variant.address)@ == address && (*variant.phone)@ == phone
}

pub open spec fn triple_count(variant: Variant2, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>) -> int {
    if same_triple(variant, name, address, phone) { variant.count as int } else { 0 }
}

pub open spec fn triple_counts(name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>) -> spec_fn(Variant2) -> int {
    |variant: Variant2| if same_triple(variant, name, address, phone) { variant.count as int } else { 0 }
}

pub open spec fn vcount(variants: Seq<Variant2>, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>) -> int {
    sum(variants, triple_counts(name, address, phone))
}

pub open spec fn distinct_variants(variants: Seq<Variant2>) -> bool {
    forall|left: int, right: int| 0 <= left < right < variants.len() ==>
        !(#[trigger] same_triple(variants[left], (*variants[right].name)@, (*variants[right].address)@, (*variants[right].phone)@))
            || left == right
}

pub open spec fn variants_within(variants: Seq<Variant2>, bound: int) -> bool {
    forall|index: int| 0 <= index < variants.len() ==> -bound <= (#[trigger] variants[index]).count <= bound
}

pub open spec fn group_ok(variants: Seq<Variant2>, bound: int) -> bool {
    distinct_variants(variants) && variants_within(variants, bound)
}

pub open spec fn groups_ok(groups: Map<i64, Vec<Variant2>>, bound: int) -> bool {
    forall|key: i64| #[trigger] groups.contains_key(key) ==> group_ok(groups[key]@, bound)
}

pub open spec fn vc(groups: Map<i64, Vec<Variant2>>, key: i64, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>) -> int {
    if groups.contains_key(key) { vcount(groups[key]@, name, address, phone) } else { 0 }
}

pub open spec fn triple_weight(supplier: int, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>) -> spec_fn(SuppEntry) -> int {
    |entry: SuppEntry| entry.sign as int * ind(entry.suppkey as int == supplier && (*entry.name)@ == name
        && (*entry.address)@ == address && (*entry.phone)@ == phone)
}

pub fn bytes_eq(left: &[u8], right: &[u8]) -> (result: bool)
    ensures result == (left@ == right@),
{
    if left.len() != right.len() { return false; }
    let mut index = 0usize;
    while index < left.len()
        invariant index <= left.len(), left.len() == right.len(),
            forall|position: int| 0 <= position < index ==> left@[position] == right@[position],
        decreases left.len() - index,
    {
        if left[index] != right[index] { return false; }
        index += 1;
    }
    proof { assert(left@ =~= right@); }
    true
}

pub fn same_variant(variant: &Variant2, entry: &SuppEntry) -> (result: bool)
    ensures result == same_triple(*variant, (*entry.name)@, (*entry.address)@, (*entry.phone)@),
{
    bytes_eq(variant.name.as_slice(), entry.name.as_slice()) && bytes_eq(variant.address.as_slice(), entry.address.as_slice())
        && bytes_eq(variant.phone.as_slice(), entry.phone.as_slice())
}

/// Adds one logged supplier row to the variants of its supplier.
#[verifier::spinoff_prover]
#[verifier::rlimit(40)]
pub fn group_entry(variants: &mut Vec<Variant2>, entry: &SuppEntry, Ghost(bound): Ghost<int>)
    requires group_ok(old(variants)@, bound), entry.sign == 1 || entry.sign == -1, bound + 1 < 0x10000000000000000000000000000000int, 0 <= bound,
    ensures group_ok(final(variants)@, bound + 1),
        forall|name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| #[trigger] vcount(final(variants)@, name, address, phone)
            == vcount(old(variants)@, name, address, phone)
                + entry.sign as int * ind((*entry.name)@ == name && (*entry.address)@ == address && (*entry.phone)@ == phone),
{
    let ghost before = variants@;
    let mut found = false;
    let mut index = 0usize;
    while index < variants.len() && !found
        invariant index <= variants.len(), variants@ == before, group_ok(before, bound),
            found ==> index < variants.len() && same_triple(variants@[index as int], (*entry.name)@, (*entry.address)@, (*entry.phone)@),
            forall|position: int| 0 <= position < index ==> !same_triple(variants@[position], (*entry.name)@, (*entry.address)@, (*entry.phone)@),
        decreases if found { 0int } else { variants.len() - index + 1 },
    {
        if same_variant(&variants[index], entry) { found = true; } else { index += 1; }
    }
    if found {
        let updated = Variant2 {
            name: Arc::clone(&variants[index].name), address: Arc::clone(&variants[index].address),
            phone: Arc::clone(&variants[index].phone), count: variants[index].count + entry.sign as i128,
        };
        proof {
            assert(-bound <= before[index as int].count <= bound);
            assert(same_triple(updated, (*entry.name)@, (*entry.address)@, (*entry.phone)@));
        }
        variants.set(index, updated);
        proof {
            assert forall|name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| #[trigger] vcount(variants@, name, address, phone)
                == vcount(before, name, address, phone)
                    + entry.sign as int * ind((*entry.name)@ == name && (*entry.address)@ == address && (*entry.phone)@ == phone) by {
                lemma_sum_update(before, index as int, updated, triple_counts(name, address, phone));
                if (*entry.name)@ == name && (*entry.address)@ == address && (*entry.phone)@ == phone {
                    assert(triple_count(updated, name, address, phone) == updated.count as int);
                    assert(triple_count(before[index as int], name, address, phone) == before[index as int].count as int);
                } else {
                    assert(triple_count(updated, name, address, phone) == 0);
                    assert(triple_count(before[index as int], name, address, phone) == 0);
                }
            }
            assert forall|left: int, right: int| 0 <= left < right < variants@.len() implies
                !(#[trigger] same_triple(variants@[left], (*variants@[right].name)@, (*variants@[right].address)@, (*variants@[right].phone)@)) || left == right by {
                assert(same_triple(variants@[left], (*variants@[right].name)@, (*variants@[right].address)@, (*variants@[right].phone)@)
                    == same_triple(before[left], (*before[right].name)@, (*before[right].address)@, (*before[right].phone)@));
            }
            assert forall|position: int| 0 <= position < variants@.len() implies -(bound + 1) <= (#[trigger] variants@[position]).count <= bound + 1 by {
                if position != index as int { assert(variants@[position] == before[position]); }
            }
        }
    } else {
        let added = Variant2 {
            name: Arc::clone(&entry.name), address: Arc::clone(&entry.address), phone: Arc::clone(&entry.phone), count: entry.sign as i128,
        };
        variants.push(added);
        proof {
            assert(variants@ =~= before.push(added));
            assert forall|name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| #[trigger] vcount(variants@, name, address, phone)
                == vcount(before, name, address, phone)
                    + entry.sign as int * ind((*entry.name)@ == name && (*entry.address)@ == address && (*entry.phone)@ == phone) by {
                lemma_sum_push(before, added, triple_counts(name, address, phone));
            }
            assert forall|left: int, right: int| 0 <= left < right < variants@.len() implies
                !(#[trigger] same_triple(variants@[left], (*variants@[right].name)@, (*variants@[right].address)@, (*variants@[right].phone)@)) || left == right by {
                if right == before.len() as int {
                    assert(!same_triple(before[left], (*entry.name)@, (*entry.address)@, (*entry.phone)@));
                } else {
                    assert(variants@[left] == before[left]);
                    assert(variants@[right] == before[right]);
                }
            }
            assert forall|position: int| 0 <= position < variants@.len() implies -(bound + 1) <= (#[trigger] variants@[position]).count <= bound + 1 by {
                if position < before.len() { assert(variants@[position] == before[position]); }
            }
        }
    }
}


#[verifier::spinoff_prover]
#[verifier::rlimit(40)]
pub fn group_chunk(groups: &mut HashMap<i64, Vec<Variant2>>, chunk: &Vec<SuppEntry>, Ghost(done): Ghost<int>)
    requires groups_ok(old(groups)@, done), unit_supps(chunk@), 0 <= done, done + chunk@.len() < 0x10000000000000000000000000000000int,
    ensures groups_ok(final(groups)@, done + chunk@.len()), final(groups)@.dom() == old(groups)@.dom(),
        forall|key: i64, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| #[trigger] vc(final(groups)@, key, name, address, phone)
            == if old(groups)@.contains_key(key) { vc(old(groups)@, key, name, address, phone) + sum(chunk@, triple_weight(key as int, name, address, phone)) } else { 0 },
{
    let mut index = 0usize;
    proof {
        assert forall|key: i64, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| #[trigger] vc(groups@, key, name, address, phone)
            == if old(groups)@.contains_key(key) { vc(old(groups)@, key, name, address, phone) + sum(chunk@.take(0), triple_weight(key as int, name, address, phone)) } else { 0 } by {
            assert(chunk@.take(0) =~= Seq::<SuppEntry>::empty());
            lemma_sum_empty(chunk@.take(0), triple_weight(key as int, name, address, phone));
        }
    }
    while index < chunk.len()
        invariant index <= chunk.len(), unit_supps(chunk@), 0 <= done, done + chunk@.len() < 0x10000000000000000000000000000000int,
            groups_ok(groups@, done + index as int), groups@.dom() == old(groups)@.dom(),
            forall|key: i64, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| #[trigger] vc(groups@, key, name, address, phone)
                == if old(groups)@.contains_key(key) { vc(old(groups)@, key, name, address, phone) + sum(chunk@.take(index as int), triple_weight(key as int, name, address, phone)) } else { 0 },
        decreases chunk.len() - index,
    {
        let entry = &chunk[index];
        let ghost before = groups@;
        proof { assert(chunk@[index as int].sign == 1 || chunk@[index as int].sign == -1); }
        let ghost mut old_variants = Seq::<Variant2>::empty();
        let ghost mut new_variants = Seq::<Variant2>::empty();
        if groups.contains_key(&entry.suppkey) {
            let mut variants = groups.remove(&entry.suppkey).unwrap();
            proof { assert(group_ok(variants@, done + index as int)); old_variants = variants@; }
            group_entry(&mut variants, entry, Ghost(done + index as int));
            proof { new_variants = variants@; }
            groups.insert(entry.suppkey, variants);
            proof { assert(groups@[entry.suppkey]@ == new_variants); assert(old_variants == before[entry.suppkey]@); }
        }
        proof {
            assert(groups@.dom() =~= old(groups)@.dom());
            assert forall|key: i64, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| #[trigger] vc(groups@, key, name, address, phone)
                == if old(groups)@.contains_key(key) { vc(old(groups)@, key, name, address, phone) + sum(chunk@.take(index as int + 1), triple_weight(key as int, name, address, phone)) } else { 0 } by {
                lemma_sum_take(chunk@, index as int, triple_weight(key as int, name, address, phone));
                let term = triple_weight(key as int, name, address, phone)(chunk@[index as int]);
                assert(before.contains_key(key) == old(groups)@.contains_key(key));
                if before.contains_key(entry.suppkey) && key == entry.suppkey {
                    assert(vc(groups@, key, name, address, phone) == vcount(new_variants, name, address, phone));
                    assert(vc(before, key, name, address, phone) == vcount(old_variants, name, address, phone));
                    assert(vc(groups@, key, name, address, phone) == vc(before, key, name, address, phone)
                        + entry.sign as int * ind((*entry.name)@ == name && (*entry.address)@ == address && (*entry.phone)@ == phone));
                } else {
                    assert(vc(groups@, key, name, address, phone) == vc(before, key, name, address, phone));
                    if key == entry.suppkey { assert(!before.contains_key(key)); }
                }
                if key == entry.suppkey && (*entry.name)@ == name && (*entry.address)@ == address && (*entry.phone)@ == phone {
                    assert(term == entry.sign as int * 1);
                } else { assert(term == 0); }
            }
            assert forall|key: i64| groups@.contains_key(key) implies group_ok(groups@[key]@, done + index as int + 1) by {
                if key != entry.suppkey { assert(before.contains_key(key)); assert(groups@[key] == before[key]); }
            }
        }
        index += 1;
    }
    proof { assert(chunk@.take(chunk@.len() as int) =~= chunk@); }
}

/// The variants of the given suppliers over the whole supplier log.
#[verifier::spinoff_prover]
#[verifier::rlimit(40)]
pub fn group_log(log: &Log<SuppEntry>, groups: HashMap<i64, Vec<Variant2>>) -> (result: HashMap<i64, Vec<Variant2>>)
    requires unit_supps(log.entries()), log.entries().len() < 0x1000000000000000000000000int, groups_ok(groups@, 0),
        forall|key: i64| #[trigger] groups@.contains_key(key) ==> groups@[key]@.len() == 0,
    ensures groups_ok(result@, log.entries().len() as int), result@.dom() == groups@.dom(),
        forall|key: i64, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| #[trigger] vc(result@, key, name, address, phone)
            == if groups@.contains_key(key) { sum(log.entries(), triple_weight(key as int, name, address, phone)) } else { 0 },
{
    let mut groups = groups;
    let ghost start = groups@;
    let ghost views = log.views();
    let mut index = 0usize;
    proof {
        assert(views.take(0) =~= Seq::<Seq<SuppEntry>>::empty());
        assert forall|key: i64, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| #[trigger] vc(groups@, key, name, address, phone)
            == if start.contains_key(key) { sum(flat(views.take(0)), triple_weight(key as int, name, address, phone)) } else { 0 } by {
            lemma_sum_empty(flat(views.take(0)), triple_weight(key as int, name, address, phone));
            if start.contains_key(key) { lemma_sum_empty(start[key]@, triple_counts(name, address, phone)); }
        }
        assert(flat(views.take(0)).len() == 0);
    }
    while index < log.chunks.len()
        invariant index <= log.chunks.len(), views == log.views(), views.len() == log.chunks.len(),
            unit_supps(log.entries()), log.entries().len() < 0x1000000000000000000000000int,
            groups_ok(groups@, flat(views.take(index as int)).len() as int), groups@.dom() == start.dom(),
            forall|key: i64, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| #[trigger] vc(groups@, key, name, address, phone)
                == if start.contains_key(key) { sum(flat(views.take(index as int)), triple_weight(key as int, name, address, phone)) } else { 0 },
        decreases log.chunks.len() - index,
    {
        let chunk = &log.chunks[index];
        proof {
            lemma_chunk_unit_supps(views, index as int);
            lemma_flat_prefix_len(views, index as int + 1);
            lemma_flat_step(views, index as int);
            assert(views[index as int] == (**chunk)@);
        }
        group_chunk(&mut groups, chunk, Ghost(flat(views.take(index as int)).len() as int));
        proof {
            let after = flat(views.take(index as int + 1));
            assert forall|key: i64, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| #[trigger] vc(groups@, key, name, address, phone)
                == if start.contains_key(key) { sum(after, triple_weight(key as int, name, address, phone)) } else { 0 } by {
                lemma_sum_concat(flat(views.take(index as int)), views[index as int], triple_weight(key as int, name, address, phone));
            }
            assert(after.len() == flat(views.take(index as int)).len() + chunk@.len());
        }
        index += 1;
    }
    proof { assert(views.take(views.len() as int) =~= views); }
    groups
}


// ── The answer the logs define ──

pub open spec fn variant_out(supplier: int, variant: Variant2, revenue: int) -> OutRow {
    OutRow { s_suppkey: supplier, s_name: (*variant.name)@, s_address: (*variant.address)@, s_phone: (*variant.phone)@, total_revenue: revenue }
}

pub open spec fn variant_rows_x(supplier: int, variant: Variant2, revenue: int) -> Seq<OutRow> {
    crate::emission::repeated(variant_out(supplier, variant, revenue), variant.count as int)
}

pub open spec fn supplier_rows_x(groups: Map<i64, Vec<Variant2>>, supplier: i64, revenue: int) -> Seq<OutRow> {
    if groups.contains_key(supplier) {
        flat(groups[supplier]@.map_values(|variant: Variant2| variant_rows_x(supplier as int, variant, revenue)))
    } else { Seq::empty() }
}

pub open spec fn model_rows(groups: Map<i64, Vec<Variant2>>, winners: Seq<i64>, revenue: int) -> Seq<OutRow> {
    flat(winners.map_values(|supplier: i64| supplier_rows_x(groups, supplier, revenue)))
}

pub open spec fn db_triple_count(rows: Seq<SupplierRow>, supplier: int, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>) -> int {
    sum(rows, |row: SupplierRow| ind(row.s_suppkey == supplier && row.s_name == name && row.s_address == address && row.s_phone == phone))
}

pub open spec fn nonneg_variants(variants: Seq<Variant2>) -> bool {
    forall|index: int| 0 <= index < variants.len() ==> (#[trigger] variants[index]).count >= 0
}

pub proof fn lemma_variants_count(variants: Seq<Variant2>, supplier: int, revenue: int, output: OutRow)
    requires nonneg_variants(variants),
    ensures sum(flat(variants.map_values(|variant: Variant2| variant_rows_x(supplier, variant, revenue))), eqv(output))
        == if output.s_suppkey == supplier && output.total_revenue == revenue { vcount(variants, output.s_name, output.s_address, output.s_phone) } else { 0int },
{
    let mapper = |variant: Variant2| variant_rows_x(supplier, variant, revenue);
    lemma_sum_flat_map(variants, mapper, eqv(output));
    let per_variant = |variant: Variant2| sum(mapper(variant), eqv(output));
    let expected = |variant: Variant2| if output.s_suppkey == supplier && output.total_revenue == revenue {
        triple_count(variant, output.s_name, output.s_address, output.s_phone)
    } else { 0int };
    assert forall|index: int| 0 <= index < variants.len() implies per_variant(#[trigger] variants[index]) == expected(variants[index]) by {
        crate::result_proofs::lemma_repeat_count(variant_out(supplier, variants[index], revenue), variants[index].count as int, output);
        let variant = variants[index];
        if variant_out(supplier, variant, revenue) == output { assert(same_triple(variant, output.s_name, output.s_address, output.s_phone)); }
        if output.s_suppkey == supplier && output.total_revenue == revenue && same_triple(variant, output.s_name, output.s_address, output.s_phone) {
            assert(variant_out(supplier, variant, revenue) == output);
        }
    }
    lemma_sum_ext(variants, per_variant, expected);
    if !(output.s_suppkey == supplier && output.total_revenue == revenue) {
        assert(expected =~= (|variant: Variant2| 0int));
        lemma_sum_zero(variants, expected);
    } else {
        assert(expected =~= triple_counts(output.s_name, output.s_address, output.s_phone));
    }
}

pub proof fn lemma_model_count(db: Db, groups: Map<i64, Vec<Variant2>>, winners: Seq<i64>, revenue: int, output: OutRow)
    requires
        forall|left: int, right: int| #![trigger winners[left], winners[right]] 0 <= left < right < winners.len() ==> winners[left] < winners[right],
        forall|supplier: i64| #[trigger] groups.contains_key(supplier) <==> has_winner(winners, supplier as int),
        forall|supplier: i64| groups.contains_key(supplier) ==> nonneg_variants(#[trigger] groups[supplier]@),
        forall|supplier: i64, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| groups.contains_key(supplier) ==> #[trigger] vc(groups, supplier, name, address, phone)
            == db_triple_count(db.supplier, supplier as int, name, address, phone),
    ensures model_rows(groups, winners, revenue).to_multiset().count(output) as int
        == sum(db.supplier, crate::result_proofs::supplier_output_weight(winners, revenue, output)),
{
    let rows = model_rows(groups, winners, revenue);
    lemma_count_as_sum(rows, output);
    let mapper = |supplier: i64| supplier_rows_x(groups, supplier, revenue);
    lemma_sum_flat_map(winners, mapper, eqv(output));
    let per_supplier = |supplier: i64| sum(mapper(supplier), eqv(output));
    assert forall|supplier: i64| #[trigger] per_supplier(supplier) == if output.s_suppkey == supplier as int && output.total_revenue == revenue {
        vc(groups, supplier, output.s_name, output.s_address, output.s_phone)
    } else { 0int } by {
        if groups.contains_key(supplier) { lemma_variants_count(groups[supplier]@, supplier as int, revenue, output); }
        else if output.s_suppkey == supplier as int && output.total_revenue == revenue { lemma_sum_empty(Seq::<OutRow>::empty(), eqv(output)); }
        else { lemma_sum_empty(Seq::<OutRow>::empty(), eqv(output)); }
    }
    let expected = crate::result_proofs::supplier_output_weight(winners, revenue, output);
    if has_winner(winners, output.s_suppkey) && output.total_revenue == revenue {
        let position = choose|position: int| 0 <= position < winners.len() && (#[trigger] winners[position]) as int == output.s_suppkey;
        assert forall|other: int| 0 <= other < winners.len() && other != position implies per_supplier(#[trigger] winners[other]) == 0 by {
            if other < position { assert(winners[other] < winners[position]); }
            else { assert(winners[position] < winners[other]); }
        }
        lemma_sum_single_match(winners, per_supplier, position);
        let supplier = winners[position];
        let count = |row: SupplierRow| ind(row.s_suppkey == output.s_suppkey && row.s_name == output.s_name && row.s_address == output.s_address && row.s_phone == output.s_phone);
        assert forall|row: SupplierRow| #[trigger] expected(row) == count(row) by {
            if row.s_suppkey == output.s_suppkey && row.s_name == output.s_name && row.s_address == output.s_address && row.s_phone == output.s_phone {
                assert(crate::result_proofs::supplier_output(row, revenue) == output);
            }
        }
        lemma_sum_ext_all(db.supplier, expected, count);
        assert(vc(groups, supplier, output.s_name, output.s_address, output.s_phone)
            == db_triple_count(db.supplier, output.s_suppkey, output.s_name, output.s_address, output.s_phone));
    } else {
        assert forall|position: int| 0 <= position < winners.len() implies per_supplier(#[trigger] winners[position]) == 0 by {
            if output.total_revenue == revenue { assert(winners[position] as int != output.s_suppkey); }
        }
        lemma_sum_zero(winners, per_supplier);
        assert forall|row: SupplierRow| #[trigger] expected(row) == 0 by {
            if crate::result_proofs::supplier_output(row, revenue) == output { assert(row.s_suppkey == output.s_suppkey); }
        }
        lemma_sum_zero(db.supplier, expected);
    }
}

pub proof fn lemma_model_correct(db: Db, parameters: Params, totals: Seq<(i64, int)>, winners: Seq<i64>, revenue: int,
    groups: Map<i64, Vec<Variant2>>)
    requires crate::exact::groups_match_x(totals, db, parameters.p1, parameters.p2), crate::exact::max_ok(totals, Some(revenue), winners),
        forall|left: int, right: int| #![trigger winners[left], winners[right]] 0 <= left < right < winners.len() ==> winners[left] < winners[right],
        forall|supplier: i64| #[trigger] groups.contains_key(supplier) <==> has_winner(winners, supplier as int),
        forall|supplier: i64| groups.contains_key(supplier) ==> nonneg_variants(#[trigger] groups[supplier]@),
        forall|supplier: i64, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| groups.contains_key(supplier) ==> #[trigger] vc(groups, supplier, name, address, phone)
            == db_triple_count(db.supplier, supplier as int, name, address, phone),
    ensures query_result(db, parameters, model_rows(groups, winners, revenue)),
{
    let rows = model_rows(groups, winners, revenue);
    crate::exact::lemma_maximum_sql_x(db, parameters, totals, winners, revenue);
    assert(rows.to_multiset() =~= crate::gen_spec::rows(db, parameters).to_multiset()) by {
        broadcast use vstd::multiset::group_multiset_axioms;
        assert forall|output: OutRow| #[trigger] rows.to_multiset().count(output) == crate::gen_spec::rows(db, parameters).to_multiset().count(output) by {
            lemma_model_count(db, groups, winners, revenue, output);
            crate::exact::lemma_sql_count_x(db, parameters, totals, winners, revenue, output);
        }
    }
    lemma_model_ordered(groups, winners, revenue);
    crate::database_proofs::lemma_execute_ok(db, parameters, rows);
}

pub proof fn lemma_model_ordered(groups: Map<i64, Vec<Variant2>>, winners: Seq<i64>, revenue: int)
    requires forall|left: int, right: int| #![trigger winners[left], winners[right]] 0 <= left < right < winners.len() ==> winners[left] < winners[right],
    ensures forall|left: int, right: int| #![trigger model_rows(groups, winners, revenue)[left], model_rows(groups, winners, revenue)[right]]
        0 <= left < right < model_rows(groups, winners, revenue).len() ==>
            model_rows(groups, winners, revenue)[left].s_suppkey <= model_rows(groups, winners, revenue)[right].s_suppkey,
    decreases winners.len(),
{
    if winners.len() > 0 {
        let prefix = winners.drop_last();
        assert forall|left: int, right: int| #![trigger prefix[left], prefix[right]] 0 <= left < right < prefix.len() implies prefix[left] < prefix[right] by {
            assert(prefix[left] == winners[left]); assert(prefix[right] == winners[right]);
        }
        lemma_model_ordered(groups, prefix, revenue);
        let mapper = |supplier: i64| supplier_rows_x(groups, supplier, revenue);
        assert(winners.map_values(mapper).drop_last() =~= prefix.map_values(mapper));
        let earlier = model_rows(groups, prefix, revenue);
        let last = supplier_rows_x(groups, winners.last(), revenue);
        assert(model_rows(groups, winners, revenue) == earlier + last);
        lemma_supplier_rows_key(groups, winners.last(), revenue);
        assert forall|position: int| 0 <= position < earlier.len() implies (#[trigger] earlier[position]).s_suppkey <= winners.last() as int by {
            lemma_model_keys(groups, prefix, revenue, position);
            let index = choose|index: int| 0 <= index < prefix.len() && earlier[position].s_suppkey == prefix[index] as int;
            assert(winners[index] < winners.last());
        }
        assert forall|left: int, right: int| #![trigger (earlier + last)[left], (earlier + last)[right]] 0 <= left < right < (earlier + last).len() implies
            (earlier + last)[left].s_suppkey <= (earlier + last)[right].s_suppkey by {
            if right < earlier.len() { assert((earlier + last)[left] == earlier[left]); assert((earlier + last)[right] == earlier[right]); }
            else if left < earlier.len() { assert((earlier + last)[left] == earlier[left]); assert((earlier + last)[right] == last[right - earlier.len()]); }
            else { assert((earlier + last)[left] == last[left - earlier.len()]); assert((earlier + last)[right] == last[right - earlier.len()]); }
        }
    }
}

pub proof fn lemma_supplier_rows_key(groups: Map<i64, Vec<Variant2>>, supplier: i64, revenue: int)
    ensures forall|position: int| 0 <= position < supplier_rows_x(groups, supplier, revenue).len() ==>
        (#[trigger] supplier_rows_x(groups, supplier, revenue)[position]).s_suppkey == supplier as int,
{
    if groups.contains_key(supplier) {
        let variants = groups[supplier]@;
        let mapper = |variant: Variant2| variant_rows_x(supplier as int, variant, revenue);
        let all = flat(variants.map_values(mapper));
        assert forall|position: int| 0 <= position < all.len() implies (#[trigger] all[position]).s_suppkey == supplier as int by {
            lemma_flat_contains(variants.map_values(mapper), all[position]);
            assert(all.contains(all[position]));
            let index = choose|index: int| 0 <= index < variants.map_values(mapper).len() && variants.map_values(mapper)[index].contains(all[position]);
            assert(variants.map_values(mapper)[index] == variant_rows_x(supplier as int, variants[index], revenue));
            let inner = choose|inner: int| 0 <= inner < variant_rows_x(supplier as int, variants[index], revenue).len() && variant_rows_x(supplier as int, variants[index], revenue)[inner] == all[position];
        }
    }
}

pub proof fn lemma_model_keys(groups: Map<i64, Vec<Variant2>>, winners: Seq<i64>, revenue: int, position: int)
    requires 0 <= position < model_rows(groups, winners, revenue).len(),
    ensures exists|index: int| 0 <= index < winners.len() && model_rows(groups, winners, revenue)[position].s_suppkey == winners[index] as int,
{
    let mapper = |supplier: i64| supplier_rows_x(groups, supplier, revenue);
    let row = model_rows(groups, winners, revenue)[position];
    lemma_flat_contains(winners.map_values(mapper), row);
    assert(model_rows(groups, winners, revenue).contains(row));
    let index = choose|index: int| 0 <= index < winners.map_values(mapper).len() && winners.map_values(mapper)[index].contains(row);
    lemma_supplier_rows_key(groups, winners[index], revenue);
    let inner = choose|inner: int| 0 <= inner < supplier_rows_x(groups, winners[index], revenue).len() && supplier_rows_x(groups, winners[index], revenue)[inner] == row;
}


pub proof fn lemma_max_step_x(totals: Seq<(i64, int)>, maximum: Option<int>, winners: Seq<i64>, supplier: i64, revenue: int)
    requires crate::exact::max_ok(totals, maximum, winners),
    ensures crate::exact::max_ok(totals.push((supplier, revenue)),
        if maximum is None || revenue > maximum.unwrap() { Some(revenue) } else { maximum },
        if maximum is None || revenue > maximum.unwrap() { seq![supplier] }
        else if maximum == Some(revenue) { winners.push(supplier) } else { winners }),
{
    let after = totals.push((supplier, revenue));
    assert(after.drop_last() =~= totals);
    let larger = maximum is None || revenue > maximum.unwrap();
    let value = if larger { revenue } else { maximum.unwrap() };
    let new_winners = if larger { seq![supplier] } else if maximum == Some(revenue) { winners.push(supplier) } else { winners };
    assert forall|position: int| 0 <= position < after.len() implies (#[trigger] after[position]).1 <= value by {
        if position < totals.len() { assert(after[position] == totals[position]); }
    }
    assert(after[totals.len() as int].1 == revenue);
    if larger { assert(after[totals.len() as int].1 == value); }
    else {
        let witness = choose|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).1 == value;
        assert(after[witness].1 == value);
    }
    assert forall|other: int| #[trigger] has_winner(new_winners, other) <==>
        exists|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == other && after[position].1 == value by {
        if larger {
            assert(has_winner(new_winners, other) == (other == supplier as int)) by {
                if other == supplier as int { assert(new_winners[0] as int == other); }
            }
            if exists|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == other && after[position].1 == value {
                let position = choose|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == other && after[position].1 == value;
                if position < totals.len() {
                    assert(after[position] == totals[position]);
                    assert(totals[position].1 <= maximum.unwrap());
                    assert(false);
                } else { assert(other == supplier as int); }
            }
            if other == supplier as int { assert(after[totals.len() as int].0 as int == other && after[totals.len() as int].1 == value); }
        } else if maximum == Some(revenue) {
            if has_winner(new_winners, other) {
                let position = choose|position: int| 0 <= position < new_winners.len() && (#[trigger] new_winners[position]) as int == other;
                if position < winners.len() {
                    assert(has_winner(winners, other));
                    let index = choose|index: int| 0 <= index < totals.len() && (#[trigger] totals[index]).0 as int == other && totals[index].1 == value;
                    assert(after[index] == totals[index]);
                } else { assert(other == supplier as int); assert(after[totals.len() as int].0 as int == other); }
            }
            if exists|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == other && after[position].1 == value {
                let position = choose|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == other && after[position].1 == value;
                if position < totals.len() {
                    assert(after[position] == totals[position]);
                    assert(has_winner(winners, other));
                    let found = choose|found: int| 0 <= found < winners.len() && (#[trigger] winners[found]) as int == other;
                    assert(new_winners[found] == winners[found]);
                } else { assert(new_winners[winners.len() as int] as int == other); }
            }
        } else {
            if has_winner(winners, other) {
                let index = choose|index: int| 0 <= index < totals.len() && (#[trigger] totals[index]).0 as int == other && totals[index].1 == value;
                assert(after[index] == totals[index]);
            }
            if exists|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == other && after[position].1 == value {
                let position = choose|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == other && after[position].1 == value;
                if position < totals.len() { assert(after[position] == totals[position]); }
                else { assert(revenue == value); assert(false); }
            }
        }
    }
}


pub proof fn lemma_group_push_x(totals: Seq<(i64, int)>, pair: (i64, int), supplier: int)
    ensures crate::exact::has_group_x(totals.push(pair), supplier) <==> crate::exact::has_group_x(totals, supplier) || supplier == pair.0 as int,
{
    let after = totals.push(pair);
    if crate::exact::has_group_x(after, supplier) {
        let position = choose|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == supplier;
        if position < totals.len() { assert(totals[position] == after[position]); assert(crate::exact::has_group_x(totals, supplier)); }
        else { assert(position == totals.len()); }
    }
    if crate::exact::has_group_x(totals, supplier) {
        let position = choose|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).0 as int == supplier;
        assert(after[position] == totals[position]);
        assert(crate::exact::has_group_x(after, supplier));
    }
    if supplier == pair.0 as int { assert(after[totals.len() as int] == pair); assert(crate::exact::has_group_x(after, supplier)); }
}

pub open spec fn max_view(maximum: Option<Wide>) -> Option<int> {
    match maximum { Some(value) => Some(value@), None => None }
}

/// The suppliers with rows in the window, their exact revenues, and the maximum with its winners.
#[verifier::spinoff_prover]
#[verifier::rlimit(60)]
pub fn scan_totals(cells: &HashMap<i64, Cell>, keys: &Vec<i64>) -> (result: (Option<Wide>, Vec<i64>, Ghost<Seq<(i64, int)>>))
    requires cells@.dom() == keys@.to_set(), keys@.no_duplicates(),
    ensures ({ let (maximum, winners, totals) = result;
        &&& crate::exact::max_ok(totals@, max_view(maximum), winners@)
        &&& winners@.no_duplicates()
        &&& forall|position: int| 0 <= position < totals@.len() ==> {
            let pair = #[trigger] totals@[position];
            keys@.contains(pair.0) && cnt_at(cells@, pair.0) > 0 && pair.1 == rev_at(cells@, pair.0)
        }
        &&& forall|key: i64| #[trigger] crate::exact::has_group_x(totals@, key as int) <==> keys@.contains(key) && cnt_at(cells@, key) > 0 }),
{
    let mut maximum: Option<Wide> = None;
    let mut winners: Vec<i64> = Vec::new();
    let ghost mut totals = Seq::<(i64, int)>::empty();
    let mut index = 0usize;
    proof { keys@.to_set_ensures(); }
    while index < keys.len()
        invariant index <= keys.len(), cells@.dom() == keys@.to_set(), keys@.no_duplicates(),
            crate::exact::max_ok(totals, max_view(maximum), winners@), winners@.no_duplicates(),
            forall|position: int| 0 <= position < totals.len() ==> {
                let pair = #[trigger] totals[position];
                keys@.take(index as int).contains(pair.0) && cnt_at(cells@, pair.0) > 0 && pair.1 == rev_at(cells@, pair.0)
            },
            forall|key: i64| #[trigger] crate::exact::has_group_x(totals, key as int) <==> keys@.take(index as int).contains(key) && cnt_at(cells@, key) > 0,
            forall|position: int| 0 <= position < winners.len() ==> keys@.take(index as int).contains(#[trigger] winners@[position]),
        decreases keys.len() - index,
    {
        let key = keys[index];
        let ghost previous_totals = totals;
        let ghost previous_winners = winners@;
        proof { assert(keys@.contains(key)); assert(cells@.contains_key(key)); }
        let (count, revenue) = match cells.get(&key) { Some(cell) => (cell.0, cell.1.dup()), None => (0, Wide::zero()) };
        proof {
            assert(count as int == cnt_at(cells@, key));
            assert(revenue@ == rev_at(cells@, key));
            assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(key));
            verus_prooflib::lemma_push_membership(keys@.take(index as int), key);
        }
        if count > 0 {
            proof { lemma_max_step_x(totals, max_view(maximum), winners@, key, revenue@); }
            let larger = match &maximum { Some(value) => value.lt(&revenue), None => true };
            let mut equal = false;
            if larger {
                maximum = Some(revenue.dup());
                winners.clear();
                winners.push(key);
            } else if let Some(value) = &maximum {
                if revenue.eq(value) { winners.push(key); equal = true; }
            }
            proof {
                assert(winners@ == if larger { seq![key] } else if equal { previous_winners.push(key) } else { previous_winners });
                totals = previous_totals.push((key, revenue@));
                assert(!keys@.take(index as int).contains(key)) by {
                    let found = choose|found: int| 0 <= found < index && keys@[found] == key;
                }
            }
        }
        proof {
            assert forall|position: int| 0 <= position < winners.len() implies keys@.take(index as int + 1).contains(#[trigger] winners@[position]) by {
                if count > 0 {
                    assert(winners@[position] == key || previous_winners.contains(winners@[position]));
                    if winners@[position] != key { assert(keys@.take(index as int).contains(winners@[position])); }
                } else { assert(winners@ == previous_winners); }
            }
            assert forall|position: int| 0 <= position < totals.len() implies {
                let pair = #[trigger] totals[position];
                keys@.take(index as int + 1).contains(pair.0) && cnt_at(cells@, pair.0) > 0 && pair.1 == rev_at(cells@, pair.0)
            } by {
                if count > 0 && position == previous_totals.len() as int { assert(totals[position] == (key, revenue@)); }
                else if count > 0 { assert(totals[position] == previous_totals[position]); }
            }
            assert forall|other: i64| #[trigger] crate::exact::has_group_x(totals, other as int) <==> keys@.take(index as int + 1).contains(other) && cnt_at(cells@, other) > 0 by {
                if count > 0 {
                    assert(totals =~= previous_totals.push((key, revenue@)));
                    lemma_group_push_x(previous_totals, (key, revenue@), other as int);
                    assert(((other as int) == (key as int)) == (other == key));
                }
            }
        }
        index += 1;
    }
    proof { assert(keys@.take(keys@.len() as int) =~= keys@); }
    (maximum, winners, Ghost(totals))
}


/// The winners in ascending order.
#[verifier::spinoff_prover]
#[verifier::rlimit(40)]
pub fn sort_winners(winners: &Vec<i64>) -> (out: Vec<i64>)
    requires winners@.no_duplicates(),
    ensures forall|left: int, right: int| #![trigger out@[left], out@[right]] 0 <= left < right < out.len() ==> out@[left] < out@[right],
        forall|value: i64| #[trigger] out@.contains(value) <==> winners@.contains(value),
        out@.no_duplicates(),
{
    let mut wide: Vec<i128> = Vec::new();
    let mut index = 0usize;
    while index < winners.len()
        invariant index <= winners.len(), wide@.len() == index,
            forall|position: int| 0 <= position < index ==> wide@[position] == winners@[position] as i128,
        decreases winners.len() - index,
    {
        wide.push(winners[index] as i128);
        index += 1;
    }
    proof {
        assert(wide@.subrange(0, wide@.len() as int) =~= wide@);
        assert forall|left: int, right: int| 0 <= left < right < wide@.len() implies wide@[left] != wide@[right] by {
            assert(winners@[left] != winners@[right]);
        }
    }
    let ordered = crate::sorting::sort_quantities(&wide, 0, wide.len());
    let mut out: Vec<i64> = Vec::new();
    index = 0;
    proof { assert(wide@.subrange(0, wide@.len() as int) =~= wide@); }
    while index < ordered.len()
        invariant index <= ordered.len(), out@.len() == index,
            forall|value: i128| #[trigger] ordered@.contains(value) <==> wide@.contains(value),
            forall|position: int| 0 <= position < wide@.len() ==> i64::MIN <= #[trigger] wide@[position] <= i64::MAX,
            forall|position: int| 0 <= position < index ==> out@[position] as i128 == ordered@[position],
        decreases ordered.len() - index,
    {
        proof { assert(ordered@.contains(ordered@[index as int])); assert(wide@.contains(ordered@[index as int])); }
        out.push(ordered[index] as i64);
        index += 1;
    }
    proof {
        assert forall|left: int, right: int| #![trigger out@[left], out@[right]] 0 <= left < right < out.len() implies out@[left] < out@[right] by {
            assert(ordered@[left] <= ordered@[right]);
            assert(ordered@[left] != ordered@[right]) by { }
        }
        assert forall|value: i64| #[trigger] out@.contains(value) <==> winners@.contains(value) by {
            if out@.contains(value) {
                let position = choose|position: int| 0 <= position < out.len() && out@[position] == value;
                assert(ordered@.contains(ordered@[position]));
                assert(out@[position] as i128 == ordered@[position]);
                assert(ordered@[position] == value as i128);
                assert(wide@.contains(ordered@[position]));
                let found = choose|found: int| 0 <= found < wide@.len() && wide@[found] == value as i128;
                assert(winners@[found] == value);
            }
            if winners@.contains(value) {
                let found = choose|found: int| 0 <= found < winners.len() && winners@[found] == value;
                assert(wide@[found] == winners@[found] as i128);
                assert(wide@.contains(value as i128));
                assert(ordered@.contains(value as i128));
                let position = choose|position: int| 0 <= position < ordered.len() && ordered@[position] == value as i128;
                assert(out@[position] == value);
            }
        }
        assert forall|left: int, right: int| 0 <= left < right < out.len() implies out@[left] != out@[right] by {
            assert(out@[left] < out@[right]);
        }
    }
    out
}


pub proof fn lemma_inner_step_x(variants: Seq<Variant2>, position: int, supplier: int, revenue: int)
    requires 0 <= position < variants.len(),
    ensures flat(variants.take(position + 1).map_values(|variant: Variant2| variant_rows_x(supplier, variant, revenue)))
        == flat(variants.take(position).map_values(|variant: Variant2| variant_rows_x(supplier, variant, revenue)))
            + variant_rows_x(supplier, variants[position], revenue),
{
    let before = variants.take(position).map_values(|variant: Variant2| variant_rows_x(supplier, variant, revenue));
    let after = variants.take(position + 1).map_values(|variant: Variant2| variant_rows_x(supplier, variant, revenue));
    assert(after =~= before.push(variant_rows_x(supplier, variants[position], revenue)));
    assert(after.drop_last() =~= before);
}

pub proof fn lemma_winner_step_x(groups: Map<i64, Vec<Variant2>>, winners: Seq<i64>, position: int, revenue: int)
    requires 0 <= position < winners.len(),
    ensures model_rows(groups, winners.take(position + 1), revenue)
        == model_rows(groups, winners.take(position), revenue) + supplier_rows_x(groups, winners[position], revenue),
{
    let mapper = |supplier: i64| supplier_rows_x(groups, supplier, revenue);
    let before = winners.take(position).map_values(mapper);
    let after = winners.take(position + 1).map_values(mapper);
    assert(after =~= before.push(supplier_rows_x(groups, winners[position], revenue)));
    assert(after.drop_last() =~= before);
}

/// Emits the rows of one supplier's variants, each repeated as often as it occurs.
#[verifier::spinoff_prover]
#[verifier::rlimit(60)]
pub fn emit_variants(output: &mut Vec<OutputRow>, variants: &Vec<Variant2>, supplier: i64, revenue: i128)
    requires rows_ordered(out_view(old(output)@)),
        forall|position: int| 0 <= position < old(output).len() ==> (#[trigger] out_view(old(output)@)[position]).s_suppkey <= supplier as int,
        nonneg_variants(variants@),
    ensures out_view(final(output)@) == out_view(old(output)@)
            + flat(variants@.map_values(|variant: Variant2| variant_rows_x(supplier as int, variant, revenue as int))),
        rows_ordered(out_view(final(output)@)),
        forall|position: int| 0 <= position < final(output).len() ==> (#[trigger] out_view(final(output)@)[position]).s_suppkey <= supplier as int,
{
    let ghost initial = out_view(output@);
    let mut index = 0usize;
    proof { assert(variants@.take(0) =~= Seq::<Variant2>::empty()); }
    while index < variants.len()
        invariant index <= variants.len(), initial == out_view(old(output)@), nonneg_variants(variants@),
            rows_ordered(out_view(output@)),
            forall|position: int| 0 <= position < output.len() ==> (#[trigger] out_view(output@)[position]).s_suppkey <= supplier as int,
            out_view(output@) == initial + flat(variants@.take(index as int).map_values(|variant: Variant2| variant_rows_x(supplier as int, variant, revenue as int))),
        decreases variants.len() - index,
    {
        let variant = &variants[index];
        let ghost row = variant_out(supplier as int, *variant, revenue as int);
        let ghost before = out_view(output@);
        let mut emitted = 0u64;
        proof { assert(variant.count >= 0); }
        while (emitted as i128) < variant.count
            invariant emitted as int <= variant.count as int, 0 <= variant.count, out_view(output@) == before + repeated(row, emitted as int),
                row == variant_out(supplier as int, *variant, revenue as int),
                rows_ordered(out_view(output@)),
                forall|position: int| 0 <= position < output.len() ==> (#[trigger] out_view(output@)[position]).s_suppkey <= supplier as int,
            decreases variant.count - emitted as int,
        {
            let ghost previous = out_view(output@);
            output.push((supplier, copy_vec(&variant.name), copy_vec(&variant.address), copy_vec(&variant.phone), revenue));
            proof {
                lemma_repeat_push(row, emitted as int);
                assert(out_view(output@) =~= previous.push(row));
                lemma_output_push(previous, row);
                assert((before + repeated(row, emitted as int)).push(row) =~= before + repeated(row, emitted as int + 1));
            }
            emitted += 1;
        }
        proof {
            assert(emitted as int == variant.count as int);
            lemma_inner_step_x(variants@, index as int, supplier as int, revenue as int);
        }
        index += 1;
    }
    proof { assert(variants@.take(variants@.len() as int) =~= variants@); }
}

/// Emits the rows of every winner, in ascending supplier order.
#[verifier::spinoff_prover]
#[verifier::rlimit(60)]
pub fn emit_winners(groups: &HashMap<i64, Vec<Variant2>>, winners: &Vec<i64>, revenue: i128) -> (output: Vec<OutputRow>)
    requires forall|left: int, right: int| #![trigger winners@[left], winners@[right]] 0 <= left < right < winners.len() ==> winners@[left] < winners@[right],
        forall|supplier: i64| groups@.contains_key(supplier) ==> nonneg_variants(#[trigger] groups@[supplier]@),
        forall|supplier: i64| #[trigger] has_winner(winners@, supplier as int) ==> groups@.contains_key(supplier),
    ensures out_view(output@) == model_rows(groups@, winners@, revenue as int), rows_ordered(out_view(output@)),
{
    let mut output: Vec<OutputRow> = Vec::new();
    let mut position = 0usize;
    proof { assert(winners@.take(0) =~= Seq::<i64>::empty()); assert(out_view(output@) =~= Seq::<OutRow>::empty()); }
    while position < winners.len()
        invariant position <= winners.len(), out_view(output@) == model_rows(groups@, winners@.take(position as int), revenue as int),
            rows_ordered(out_view(output@)),
            forall|left: int, right: int| #![trigger winners@[left], winners@[right]] 0 <= left < right < winners.len() ==> winners@[left] < winners@[right],
            forall|supplier: i64| groups@.contains_key(supplier) ==> nonneg_variants(#[trigger] groups@[supplier]@),
            forall|supplier: i64| #[trigger] has_winner(winners@, supplier as int) ==> groups@.contains_key(supplier),
            position > 0 ==> forall|index: int| 0 <= index < output.len() ==> (#[trigger] out_view(output@)[index]).s_suppkey <= winners@[position as int - 1] as int,
        decreases winners.len() - position,
    {
        let supplier = winners[position];
        let ghost before = out_view(output@);
        proof {
            assert forall|index: int| 0 <= index < output.len() implies (#[trigger] out_view(output@)[index]).s_suppkey <= supplier as int by {
                if position > 0 { assert(winners@[position as int - 1] < supplier); }
                else { assert(before.len() == 0); }
            }
            assert(has_winner(winners@, supplier as int)) by { assert(winners@[position as int] == supplier); }
        }
        let variants = groups.get(&supplier).unwrap();
        emit_variants(&mut output, variants, supplier, revenue);
        proof {
            assert(out_view(output@) == before + supplier_rows_x(groups@, supplier, revenue as int));
            lemma_winner_step_x(groups@, winners@, position as int, revenue as int);
        }
        position += 1;
    }
    proof { assert(winners@.take(position as int) =~= winners@); }
    output
}


pub open spec fn winner_total(groups: Map<i64, Vec<Variant2>>, supplier: i64) -> int {
    if groups.contains_key(supplier) { sum(groups[supplier]@, |variant: Variant2| variant.count as int) } else { 0 }
}

pub open spec fn all_total(groups: Map<i64, Vec<Variant2>>, winners: Seq<i64>) -> int {
    sum(winners, |supplier: i64| winner_total(groups, supplier))
}

pub proof fn lemma_model_len(groups: Map<i64, Vec<Variant2>>, winners: Seq<i64>, revenue: int)
    requires forall|supplier: i64| groups.contains_key(supplier) ==> nonneg_variants(#[trigger] groups[supplier]@),
    ensures model_rows(groups, winners, revenue).len() == all_total(groups, winners),
{
    let one = |row: OutRow| 1int;
    let mapper = |supplier: i64| supplier_rows_x(groups, supplier, revenue);
    verus_prooflib::lemma_sum_ones(model_rows(groups, winners, revenue));
    lemma_sum_flat_map(winners, mapper, one);
    assert forall|supplier: i64| #[trigger] sum(mapper(supplier), one) == winner_total(groups, supplier) by {
        if groups.contains_key(supplier) {
            let variants = groups[supplier]@;
            let inner = |variant: Variant2| variant_rows_x(supplier as int, variant, revenue);
            lemma_sum_flat_map(variants, inner, one);
            assert forall|index: int| 0 <= index < variants.len() implies sum(inner(#[trigger] variants[index]), one) == variants[index].count as int by {
                verus_prooflib::lemma_sum_ones(inner(variants[index]));
                assert(inner(variants[index]).len() == variants[index].count as int);
            }
            lemma_sum_ext(variants, |variant: Variant2| sum(inner(variant), one), |variant: Variant2| variant.count as int);
        } else { lemma_sum_empty(Seq::<OutRow>::empty(), one); }
    }
    lemma_sum_ext_all(winners, |supplier: i64| sum(mapper(supplier), one), |supplier: i64| winner_total(groups, supplier));
}

pub proof fn lemma_model_revenue(groups: Map<i64, Vec<Variant2>>, winners: Seq<i64>, revenue: int, position: int)
    requires 0 <= position < model_rows(groups, winners, revenue).len(),
    ensures model_rows(groups, winners, revenue)[position].total_revenue == revenue,
{
    let mapper = |supplier: i64| supplier_rows_x(groups, supplier, revenue);
    let row = model_rows(groups, winners, revenue)[position];
    lemma_flat_contains(winners.map_values(mapper), row);
    assert(model_rows(groups, winners, revenue).contains(row));
    let index = choose|index: int| 0 <= index < winners.map_values(mapper).len() && winners.map_values(mapper)[index].contains(row);
    let supplier = winners[index];
    assert(winners.map_values(mapper)[index] == supplier_rows_x(groups, supplier, revenue));
    if groups.contains_key(supplier) {
        let variants = groups[supplier]@;
        let inner = |variant: Variant2| variant_rows_x(supplier as int, variant, revenue);
        lemma_flat_contains(variants.map_values(inner), row);
        let slot = choose|slot: int| 0 <= slot < variants.map_values(inner).len() && variants.map_values(inner)[slot].contains(row);
        assert(variants.map_values(inner)[slot] == variant_rows_x(supplier as int, variants[slot], revenue));
        let copy = choose|copy: int| 0 <= copy < variant_rows_x(supplier as int, variants[slot], revenue).len() && variant_rows_x(supplier as int, variants[slot], revenue)[copy] == row;
    } else { assert(supplier_rows_x(groups, supplier, revenue) =~= Seq::<OutRow>::empty()); assert(false); }
}

pub proof fn lemma_nonneg_distinct(variants: Seq<Variant2>)
    requires distinct_variants(variants),
        forall|name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| #[trigger] vcount(variants, name, address, phone) >= 0,
    ensures nonneg_variants(variants),
{
    assert forall|index: int| 0 <= index < variants.len() implies (#[trigger] variants[index]).count >= 0 by {
        let name = (*variants[index].name)@;
        let address = (*variants[index].address)@;
        let phone = (*variants[index].phone)@;
        let weight = triple_counts(name, address, phone);
        assert forall|other: int| 0 <= other < variants.len() && other != index implies weight(#[trigger] variants[other]) == 0 by {
            if other < index {
                assert(!same_triple(variants[other], (*variants[index].name)@, (*variants[index].address)@, (*variants[index].phone)@));
            } else {
                assert(!same_triple(variants[index], (*variants[other].name)@, (*variants[other].address)@, (*variants[other].phone)@) || index == other);
            }
        }
        lemma_sum_single_match(variants, weight, index);
        assert(vcount(variants, name, address, phone) == variants[index].count as int);
    }
}

pub proof fn lemma_window_out_of_range(entries: Seq<LineEntry>, supplier: int, lower: int, upper: int, revenue: bool)
    requires supplier < i64::MIN || supplier > i64::MAX,
    ensures sum(entries, win_term(supplier, lower, upper, revenue)) == 0,
{
    assert forall|index: int| 0 <= index < entries.len() implies win_term(supplier, lower, upper, revenue)(#[trigger] entries[index]) == 0 by {}
    lemma_sum_zero(entries, win_term(supplier, lower, upper, revenue));
}


/// The answer for [lower, upper) from the logs: tally per supplier, take the maximum, join the winners.
#[verifier::spinoff_prover]
#[verifier::rlimit(200)]
pub fn replay(lines: &Log<LineEntry>, supps: &Log<SuppEntry>, lower: i32, upper: i32, Ghost(db): Ghost<Db>)
    -> (result: Option<Vec<OutputRow>>)
    requires unit_lines(lines.entries()), unit_supps(supps.entries()),
        lines.entries().len() < 0x8000000000000000, supps.entries().len() < 0x8000000000000000,
        forall|supplier: int, revenue: bool| #[trigger] crate::query_proofs::window_sum(db.lineitem, supplier, lower as int, upper as int, revenue)
            == sum(lines.entries(), win_term(supplier, lower as int, upper as int, revenue)),
        forall|supplier: int, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| #[trigger] db_triple_count(db.supplier, supplier, name, address, phone)
            == sum(supps.entries(), triple_weight(supplier, name, address, phone)),
    ensures result matches Some(rows) ==> query_result(db, params(lower as int, upper as int), out_view(rows@)),
        answer_fits(db, params(lower as int, upper as int)) ==> result is Some,
{
    let ghost parameters = params(lower as int, upper as int);
    let (cells, keys) = tally_lines(lines, lower, upper);
    let (maximum, winners, totals) = scan_totals(&cells, &keys);
    proof {
        keys@.to_set_ensures();
        assert forall|position: int| 0 <= position < totals@.len() implies {
            let pair = #[trigger] totals@[position];
            crate::query_proofs::window_sum(db.lineitem, pair.0 as int, lower as int, upper as int, false) > 0 &&
            pair.1 == crate::query_proofs::window_sum(db.lineitem, pair.0 as int, lower as int, upper as int, true)
        } by {
            let pair = totals@[position];
            assert(pair.0 as int as i64 == pair.0);
        }
        assert forall|supplier: int| #[trigger] crate::exact::has_group_x(totals@, supplier) <==>
            crate::query_proofs::window_sum(db.lineitem, supplier, lower as int, upper as int, false) > 0 by {
            if i64::MIN <= supplier <= i64::MAX {
                let key = supplier as i64;
                assert(key as int == supplier);
                if cnt_at(cells@, key) > 0 { assert(cells@.contains_key(key)); assert(keys@.contains(key)); }
            } else {
                lemma_window_out_of_range(lines.entries(), supplier, lower as int, upper as int, false);
                assert(!crate::exact::has_group_x(totals@, supplier)) by {
                    if crate::exact::has_group_x(totals@, supplier) {
                        let position = choose|position: int| 0 <= position < totals@.len() && (#[trigger] totals@[position]).0 as int == supplier;
                    }
                }
            }
        }
        assert(crate::exact::groups_match_x(totals@, db, lower as int, upper as int));
    }
    let top = match maximum {
        Some(top) => top,
        None => {
            proof {
                assert forall|supplier: int| #[trigger] crate::query_proofs::window_sum(db.lineitem, supplier, lower as int, upper as int, false) == 0 by {
                    assert(!crate::exact::has_group_x(totals@, supplier)) by { assert(totals@.len() == 0); }
                    let weight = crate::query_proofs::window_weight(supplier, lower as int, upper as int, false);
                    assert forall|index: int| 0 <= index < db.lineitem.len() implies weight(#[trigger] db.lineitem[index]) >= 0 by {}
                    lemma_sum_nonneg(db.lineitem, weight);
                }
                crate::query_proofs::lemma_no_groups(db, parameters);
                assert(out_view(Seq::<OutputRow>::empty()) =~= Seq::<OutRow>::empty());
            }
            return Some(Vec::new());
        }
    };
    let ghost revenue = top@;
    let ordered = sort_winners(&winners);
    let mut groups: HashMap<i64, Vec<Variant2>> = HashMap::new();
    let mut index = 0usize;
    while index < ordered.len()
        invariant index <= ordered.len(), groups_ok(groups@, 0),
            forall|key: i64| #[trigger] groups@.contains_key(key) <==> ordered@.take(index as int).contains(key),
            forall|key: i64| #[trigger] groups@.contains_key(key) ==> groups@[key]@.len() == 0,
        decreases ordered.len() - index,
    {
        let ghost before = groups@;
        proof {
            assert(ordered@.take(index as int + 1) =~= ordered@.take(index as int).push(ordered@[index as int]));
            verus_prooflib::lemma_push_membership(ordered@.take(index as int), ordered@[index as int]);
        }
        groups.insert(ordered[index], Vec::new());
        proof {
            assert forall|key: i64| #[trigger] groups@.contains_key(key) <==> ordered@.take(index as int + 1).contains(key) by {
                assert(key == ordered@[index as int] || groups@.contains_key(key) == before.contains_key(key));
            }
            assert forall|key: i64| #[trigger] groups@.contains_key(key) implies groups@[key]@.len() == 0 by {
                if key != ordered@[index as int] { assert(before.contains_key(key)); assert(groups@[key] == before[key]); }
            }
            assert forall|key: i64| #[trigger] groups@.contains_key(key) implies group_ok(groups@[key]@, 0) by {
                if key != ordered@[index as int] { assert(before.contains_key(key)); assert(groups@[key] == before[key]); }
                else {
                    assert(groups@[key]@.len() == 0);
                }
            }
        }
        index += 1;
    }
    proof { assert(ordered@.take(ordered@.len() as int) =~= ordered@); }
    let ghost starting = groups@;
    let groups = group_log(supps, groups);
    proof {
        assert(crate::exact::max_ok(totals@, Some(revenue), ordered@)) by {
            assert forall|supplier: int| #[trigger] has_winner(ordered@, supplier) <==> has_winner(winners@, supplier) by {
                if has_winner(ordered@, supplier) {
                    let position = choose|position: int| 0 <= position < ordered.len() && (#[trigger] ordered@[position]) as int == supplier;
                    assert(ordered@.contains(ordered@[position]));
                    assert(winners@.contains(ordered@[position]));
                    let found = choose|found: int| 0 <= found < winners.len() && winners@[found] == ordered@[position];
                }
                if has_winner(winners@, supplier) {
                    let position = choose|position: int| 0 <= position < winners.len() && (#[trigger] winners@[position]) as int == supplier;
                    assert(winners@.contains(winners@[position]));
                    assert(ordered@.contains(winners@[position]));
                    let found = choose|found: int| 0 <= found < ordered.len() && ordered@[found] == winners@[position];
                }
            }
        }
        assert forall|supplier: i64| #[trigger] groups@.contains_key(supplier) <==> has_winner(ordered@, supplier as int) by {
            assert(groups@.dom() == starting.dom());
            assert(starting.contains_key(supplier) == ordered@.contains(supplier));
            if ordered@.contains(supplier) {
                let position = choose|position: int| 0 <= position < ordered.len() && ordered@[position] == supplier;
                assert(ordered@[position] as int == supplier as int);
            }
            if has_winner(ordered@, supplier as int) {
                let position = choose|position: int| 0 <= position < ordered.len() && (#[trigger] ordered@[position]) as int == supplier as int;
                assert(ordered@[position] == supplier);
            }
        }
        assert forall|supplier: i64, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| groups@.contains_key(supplier) implies
            #[trigger] vc(groups@, supplier, name, address, phone) == db_triple_count(db.supplier, supplier as int, name, address, phone) by {
            assert(starting.contains_key(supplier));
            assert(db_triple_count(db.supplier, supplier as int, name, address, phone) == sum(supps.entries(), triple_weight(supplier as int, name, address, phone)));
        }
        assert forall|supplier: i64| groups@.contains_key(supplier) implies nonneg_variants(#[trigger] groups@[supplier]@) by {
            assert(group_ok(groups@[supplier]@, supps.entries().len() as int));
            assert forall|name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| #[trigger] vcount(groups@[supplier]@, name, address, phone) >= 0 by {
                assert(vc(groups@, supplier, name, address, phone) == vcount(groups@[supplier]@, name, address, phone));
                assert(vc(groups@, supplier, name, address, phone) == db_triple_count(db.supplier, supplier as int, name, address, phone));
                let weight = |row: SupplierRow| ind(row.s_suppkey == supplier as int && row.s_name == name && row.s_address == address && row.s_phone == phone);
                assert forall|position: int| 0 <= position < db.supplier.len() implies weight(#[trigger] db.supplier[position]) >= 0 by {}
                lemma_sum_nonneg(db.supplier, weight);
            }
            lemma_nonneg_distinct(groups@[supplier]@);
        }
        lemma_model_correct(db, parameters, totals@, ordered@, revenue, groups@);
        lemma_model_len(groups@, ordered@, revenue);
    }
    let ghost model = model_rows(groups@, ordered@, revenue);
    let mut total = Wide::zero();
    let mut position = 0usize;
    proof { assert(ordered@.take(0) =~= Seq::<i64>::empty()); lemma_sum_empty(ordered@.take(0), |supplier: i64| winner_total(groups@, supplier)); }
    while position < ordered.len()
        invariant position <= ordered.len(), total@ == all_total(groups@, ordered@.take(position as int)),
            forall|supplier: i64| #[trigger] groups@.contains_key(supplier) <==> has_winner(ordered@, supplier as int),
        decreases ordered.len() - position,
    {
        let supplier = ordered[position];
        proof { assert(ordered@[position as int] as int == supplier as int); assert(has_winner(ordered@, supplier as int)); }
        let variants = groups.get(&supplier).unwrap();
        let mut inner = 0usize;
        let ghost start_total = total@;
        proof { assert(variants@.take(0) =~= Seq::<Variant2>::empty()); lemma_sum_empty(variants@.take(0), |variant: Variant2| variant.count as int); }
        while inner < variants.len()
            invariant inner <= variants.len(), total@ == start_total + sum(variants@.take(inner as int), |variant: Variant2| variant.count as int),
            decreases variants.len() - inner,
        {
            proof { lemma_sum_take(variants@, inner as int, |variant: Variant2| variant.count as int); }
            total = total.add(&Wide::from_i128(variants[inner].count));
            inner += 1;
        }
        proof {
            assert(variants@.take(variants@.len() as int) =~= variants@);
            assert(winner_total(groups@, supplier) == sum(variants@, |variant: Variant2| variant.count as int));
            lemma_sum_take(ordered@, position as int, |supplier: i64| winner_total(groups@, supplier));
        }
        position += 1;
    }
    proof { assert(ordered@.take(ordered@.len() as int) =~= ordered@); }
    if Wide::from_i128(1099511627776).lt(&total) {
        proof {
            assert(model.len() == total@);
            if answer_fits(db, parameters) { assert(query_result(db, parameters, model)); assert(model.len() <= max_rows()); }
        }
        return None;
    }
    if total.is_zero() {
        proof {
            assert(model.len() == 0);
            assert(out_view(Seq::<OutputRow>::empty()) =~= model);
        }
        return Some(Vec::new());
    }
    let wide_revenue = match top.to_i128() {
        Some(value) => value,
        None => {
            proof {
                assert(model.len() == total@);
                lemma_model_revenue(groups@, ordered@, revenue, 0);
                if answer_fits(db, parameters) { assert(query_result(db, parameters, model)); assert(out_fits(model[0])); }
            }
            return None;
        }
    };
    proof {
        assert forall|supplier: i64| groups@.contains_key(supplier) implies nonneg_variants(#[trigger] groups@[supplier]@) by { }
        assert forall|supplier: i64| #[trigger] has_winner(ordered@, supplier as int) implies groups@.contains_key(supplier) by { }
    }
    let rows = emit_winners(&groups, &ordered, wide_revenue);
    proof { assert(wide_revenue as int == revenue); }
    Some(rows)
}

}
