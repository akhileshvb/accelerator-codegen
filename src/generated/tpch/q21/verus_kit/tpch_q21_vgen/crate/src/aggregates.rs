use vstd::prelude::*;
use std::sync::Arc;
use crate::index::*;
use crate::gen_spec::*;
use verus_common::sums::*;

macro_rules! some_or_return {
    ($expression:expr) => {
        match $expression { Some(value) => value, None => return None }
    };
}
pub(crate) use some_or_return;

verus! {

pub type Counts = Index<i128>;
pub type Slices = Index<Counts>;

pub struct NameEntry {
    pub key: Key,
    pub references: i128,
}

pub type NamePool = Index<NameEntry>;

pub open spec fn name_references(db: Db, name: Seq<u8>) -> int {
    sum(db.supplier, |row: SupplierRow| if row.s_name == name { 1 } else { 0 })
        + sum(db.nation, |row: NationRow| if row.n_name == name { 1 } else { 0 })
}

pub open spec fn name_count(names: NamePool, key: KeyView) -> int {
    match names.at(key) { Some(entry) => entry.references as int, None => 0 }
}

pub open spec fn names_valid(names: NamePool) -> bool {
    &&& names.wf()
    &&& forall|position: int| 0 <= position < names.entries().len() ==> {
        let entry = (#[trigger] names.entries()[position]);
        entry.0 == entry.1.key.view() && entry.1.references > 0
    }
}

pub open spec fn names_ok(names: NamePool, db: Db) -> bool {
    &&& names_valid(names)
    &&& forall|key: KeyView| #![trigger name_count(names, key)] name_count(names, key) ==
        if key.0 == 0 { name_references(db, key.1) } else { 0 }
}

pub fn intern(names: &mut NamePool, candidate: Key, change: i128) -> (result: Option<Key>)
    requires names_valid(*old(names)),
    ensures
        result matches Some(key) ==> key.view() == candidate.view(),
        result.is_some() ==> names_valid(*final(names)),
        result.is_some() ==> forall|query: KeyView| #![trigger name_count(*final(names), query)]
            name_count(*final(names), query) == name_count(*old(names), query) + if query == candidate.view() { change as int } else { 0 },
{
    let ghost previous = *names;
    proof { names.model(); }
    let (key, old_references) = match names.get(&candidate) {
        Some(entry) => (entry.key.share(), entry.references),
        None => (candidate, 0),
    };
    let references = some_or_return!(old_references.checked_add(change));
    if references < 0 { return None; }
    if references == 0 { names.remove(&key); }
    else { names.put(key.share(), Arc::new(NameEntry { key: key.share(), references })); }
    proof {
        names.model();
        assert forall|position: int| #![trigger names.entries()[position]] 0 <= position < names.entries().len() implies {
            let entry = names.entries()[position];
            entry.0 == entry.1.key.view() && entry.1.references > 0
        } by {
            let query = names.entries()[position].0;
            if query != key.view() {
                let prior = choose|prior: int| #![auto] 0 <= prior < previous.entries().len() && previous.entries()[prior].0 == query;
                assert(previous.entries()[prior].1 == names.entries()[position].1);
            }
        }
    }
    Some(key)
}

pub fn canonical(names: &NamePool, previous: &NamePool, bytes: &Vec<u8>) -> (result: Key)
    requires names_valid(*names), names_valid(*previous),
    ensures result.view() == (0int, bytes@),
{
    proof { names.model(); previous.model(); }
    let candidate = Key::text(bytes.clone());
    if let Some(entry) = names.get(&candidate) { return entry.key.share(); }
    if let Some(entry) = previous.get(&candidate) { return entry.key.share(); }
    candidate
}

pub fn update_names(names: &mut NamePool, changes: &Counts) -> (result: Option<()>)
    requires names_valid(*old(names)),
    ensures
        result.is_some() ==> names_valid(*final(names)),
        result.is_some() ==> forall|query: KeyView| #![trigger name_count(*final(names), query)]
            name_count(*final(names), query) == name_count(*old(names), query) + count(*changes, query),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let ghost previous = *names;
    let rows = changes.rows();
    let mut position = 0usize;
    while position < rows.len()
        invariant
            position <= rows.len(), names_valid(*names),
            rows@.map_values(|row: (Key, Arc<i128>)| (row.0.view(), *row.1)) == changes.entries(), changes.wf(),
            forall|query: KeyView| #![trigger name_count(*names, query)] name_count(*names, query) == name_count(previous, query)
                + sum(rows@.take(position as int), count_row_weight(query)),
        decreases rows.len() - position,
    {
        let (key, change) = &rows[position];
        some_or_return!(intern(names, key.share(), **change));
        proof {
            assert forall|query: KeyView| #![trigger name_count(*names, query)] name_count(*names, query) == name_count(previous, query)
                + sum(rows@.take(position as int + 1), count_row_weight(query)) by {
                lemma_sum_take(rows@, position as int, count_row_weight(query));
            }
        }
        position += 1;
    }
    proof {
        assert(rows@.take(position as int) =~= rows@);
        assert forall|query: KeyView| #![trigger name_count(*names, query)] name_count(*names, query) == name_count(previous, query) + count(*changes, query) by {
            lemma_count_sum(changes, query);
            lemma_sum_map(rows@, |row: (Key, Arc<i128>)| (row.0.view(), *row.1), entry_weight(query), count_row_weight(query));
        }
    }
    Some(())
}

pub open spec fn count(index: Counts, key: KeyView) -> int {
    match index.at(key) { Some(value) => value as int, None => 0 }
}

pub open spec fn nested_count(index: Slices, outer: KeyView, inner: KeyView) -> int {
    match index.at(outer) { Some(slice) => count(slice, inner), None => 0 }
}

pub open spec fn positive(index: Counts) -> bool {
    &&& index.wf()
    &&& forall|position: int| #![trigger index.entries()[position]] 0 <= position < index.entries().len() ==> index.entries()[position].1 > 0
}

pub open spec fn positive_slices(index: Slices) -> bool {
    &&& index.wf()
    &&& forall|position: int| 0 <= position < index.entries().len() ==> {
        let slice = (#[trigger] index.entries()[position]).1;
        positive(slice) && slice.entries().len() > 0
    }
}

pub proof fn lemma_positive_at(index: &Counts, key: KeyView)
    requires positive(*index),
    ensures
        count(*index, key) >= 0,
        index.at(key) matches Some(value) ==> value > 0,
        (count(*index, key) == 0) == index.at(key).is_none(),
{
    index.model();
    if index.at(key).is_some() {
        let position = choose|position: int| #![auto] 0 <= position < index.entries().len() && index.entries()[position].0 == key;
        assert(index.entries()[position].1 > 0);
    }
}

pub open spec fn entry_weight(key: KeyView) -> spec_fn((KeyView, i128)) -> int {
    |row: (KeyView, i128)| if row.0 == key { row.1 as int } else { 0 }
}

pub open spec fn count_row_weight(key: KeyView) -> spec_fn((Key, Arc<i128>)) -> int {
    |row: (Key, Arc<i128>)| if row.0.view() == key { *row.1 as int } else { 0 }
}

pub proof fn lemma_count_sum(index: &Counts, key: KeyView)
    requires index.wf(),
    ensures count(*index, key) == sum(index.entries(), entry_weight(key)),
{
    index.model();
    if index.at(key).is_some() {
        let position = choose|position: int| #![auto] 0 <= position < index.entries().len() && index.entries()[position].0 == key;
        assert forall|other: int| #![trigger index.entries()[other]] 0 <= other < index.entries().len() && other != position implies entry_weight(key)(index.entries()[other]) == 0 by {
            lemma_key_order(key, key);
            if other < position { assert(key_lt(index.entries()[other].0, index.entries()[position].0)); }
            else { assert(key_lt(index.entries()[position].0, index.entries()[other].0)); }
        }
        lemma_sum_single_match(index.entries(), entry_weight(key), position);
    } else {
        lemma_sum_zero(index.entries(), entry_weight(key));
    }
}

pub open spec fn slice_entry_weight(outer: KeyView, inner: KeyView) -> spec_fn((KeyView, Counts)) -> int {
    |row: (KeyView, Counts)| if row.0 == outer { count(row.1, inner) } else { 0 }
}

pub proof fn lemma_nested_count_sum(index: &Slices, outer: KeyView, inner: KeyView)
    requires index.wf(),
    ensures nested_count(*index, outer, inner) == sum(index.entries(), slice_entry_weight(outer, inner)),
{
    index.model();
    if index.at(outer).is_some() {
        let position = choose|position: int| #![auto] 0 <= position < index.entries().len() && index.entries()[position].0 == outer;
        assert forall|other: int| #![trigger index.entries()[other]] 0 <= other < index.entries().len() && other != position implies slice_entry_weight(outer, inner)(index.entries()[other]) == 0 by {
            lemma_key_order(outer, outer);
            if other < position { assert(key_lt(index.entries()[other].0, index.entries()[position].0)); }
            else { assert(key_lt(index.entries()[position].0, index.entries()[other].0)); }
        }
        lemma_sum_single_match(index.entries(), slice_entry_weight(outer, inner), position);
    } else {
        lemma_sum_zero(index.entries(), slice_entry_weight(outer, inner));
    }
}

pub open spec fn nations_for(index: Slices, db: Db) -> bool {
    &&& positive_slices(index)
    &&& forall|outer: KeyView, inner: KeyView| #![trigger nested_count(index, outer, inner)] nested_count(index, outer, inner) ==
        if outer.0 == 0 && inner.1 == Seq::<u8>::empty() { nation_multiplicity(db, inner.0, outer.1) } else { 0 }
}

pub open spec fn waiting_for(index: Counts, db: Db) -> bool {
    &&& positive(index)
    &&& forall|key: KeyView| #![trigger count(index, key)] count(index, key) ==
        if key.1 == Seq::<u8>::empty() { waiting(db, key.0) } else { 0 }
}

pub open spec fn suppliers_for(index: Slices, db: Db) -> bool {
    &&& positive_slices(index)
    &&& forall|outer: KeyView, inner: KeyView| #![trigger nested_count(index, outer, inner)] nested_count(index, outer, inner) ==
        if outer.1 == Seq::<u8>::empty() { supplier_multiplicity(db, outer.0, inner.0, inner.1) } else { 0 }
}

pub open spec fn buckets_for(index: Slices, db: Db) -> bool {
    &&& positive_slices(index)
    &&& forall|outer: KeyView, inner: KeyView| #![trigger nested_count(index, outer, inner)] nested_count(index, outer, inner) ==
        if outer.1 == Seq::<u8>::empty() && inner.0 == 0 { bucket(db, outer.0, inner.1) } else { 0 }
}

pub open spec fn finished(db: Db, order: int) -> int {
    sum(db.orders, |row: OrdersRow| if row.o_orderkey == order && row.o_orderstatus == seq![b'F'] { 1 } else { 0 })
}

pub open spec fn waiting(db: Db, supplier: int) -> int {
    sum(db.lineitem, |row: LineitemRow| if row.l_suppkey == supplier
        && row.l_receiptdate > row.l_commitdate && exists_1(db, row) && !exists_2(db, row) {
        finished(db, row.l_orderkey)
    } else { 0 })
}

pub open spec fn supplier_multiplicity(db: Db, supplier: int, nation: int, name: Seq<u8>) -> int {
    sum(db.supplier, |row: SupplierRow| if row.s_suppkey == supplier && row.s_nationkey == nation && row.s_name == name { 1 } else { 0 })
}

pub open spec fn nation_multiplicity(db: Db, nation: int, name: Seq<u8>) -> int {
    sum(db.nation, |row: NationRow| if row.n_nationkey == nation && row.n_name == name { 1 } else { 0 })
}

pub open spec fn bucket(db: Db, nation: int, name: Seq<u8>) -> int {
    sum(db.supplier, |row: SupplierRow| if row.s_nationkey == nation && row.s_name == name { waiting(db, row.s_suppkey) } else { 0 })
}

pub fn amount(index: &Counts, key: &Key) -> (value: i128)
    ensures value as int == count(*index, key.view()),
{
    match index.get(key) { Some(value) => *value, None => 0 }
}

pub fn slice(index: &Slices, key: &Key) -> (result: Counts)
    ensures
        forall|inner: KeyView| #![trigger count(result, inner)] count(result, inner) == nested_count(*index, key.view(), inner),
        positive_slices(*index) ==> positive(result),
{
    match index.get(key) {
        Some(value) => {
            proof {
                if positive_slices(*index) {
                    index.model();
                    let position = choose|position: int| #![auto] 0 <= position < index.entries().len() && index.entries()[position].0 == key.view();
                    assert(index.entries()[position].1 == *value);
                }
            }
            value.share()
        },
        None => Counts::new(),
    }
}

pub fn adjust(index: &mut Counts, key: Key, delta: i128, nonnegative: bool) -> (result: Option<()>)
    ensures
        result.is_some() ==> forall|query: KeyView| #![trigger count(*final(index), query)]
            count(*final(index), query) == count(*old(index), query) + if query == key.view() { delta as int } else { 0 },
        result.is_some() && nonnegative && positive(*old(index)) ==> positive(*final(index)),
{
    let ghost previous = *index;
    let old = amount(index, &key);
    let next = some_or_return!(old.checked_add(delta));
    if nonnegative && next < 0 { return None; }
    if next == 0 { index.remove(&key); }
    else { index.put(key, Arc::new(next)); }
    proof {
        if nonnegative && positive(previous) {
            index.model();
            assert forall|position: int| #![trigger index.entries()[position]] 0 <= position < index.entries().len() implies index.entries()[position].1 > 0 by {
                let query = index.entries()[position].0;
                if query == key.view() {
                    assert(index.at(query).is_some());
                } else {
                    lemma_positive_at(&previous, query);
                }
            }
        }
    }
    Some(())
}

pub fn adjust_slice(index: &mut Slices, outer: Key, inner: Key, delta: i128, nonnegative: bool) -> (result: Option<()>)
    ensures
        result.is_some() ==> forall|query_outer: KeyView, query_inner: KeyView| #![trigger nested_count(*final(index), query_outer, query_inner)]
            nested_count(*final(index), query_outer, query_inner) == nested_count(*old(index), query_outer, query_inner)
                + if query_outer == outer.view() && query_inner == inner.view() { delta as int } else { 0 },
        result.is_some() && nonnegative && positive_slices(*old(index)) ==> positive_slices(*final(index)),
{
    let ghost previous = *index;
    let mut values = slice(index, &outer);
    some_or_return!(adjust(&mut values, inner, delta, nonnegative));
    let ghost updated_values = values;
    proof {
        if values.entries().len() == 0 {
            values.model();
            assert forall|query: KeyView| #![trigger count(values, query)] count(values, query) == 0 by {}
        }
    }
    if values.is_empty() { index.remove(&outer); }
    else { index.put(outer, Arc::new(values)); }
    proof {
        assert forall|query_outer: KeyView, query_inner: KeyView| #![trigger nested_count(*index, query_outer, query_inner)]
            nested_count(*index, query_outer, query_inner) == nested_count(previous, query_outer, query_inner)
                + if query_outer == outer.view() && query_inner == inner.view() { delta as int } else { 0 } by {
            if query_outer == outer.view() {
                assert(count(updated_values, query_inner) == nested_count(previous, outer.view(), query_inner)
                    + if query_inner == inner.view() { delta as int } else { 0 });
                if updated_values.entries().len() == 0 {
                    assert(count(updated_values, query_inner) == 0);
                }
            }
        }
        if nonnegative && positive_slices(previous) {
            previous.model();
            index.model();
            assert forall|position: int| #![trigger index.entries()[position]] 0 <= position < index.entries().len() implies {
                let value = index.entries()[position].1;
                positive(value) && value.entries().len() > 0
            } by {
                let query = index.entries()[position].0;
                if query != outer.view() {
                    let prior = choose|prior: int| #![auto] 0 <= prior < previous.entries().len() && previous.entries()[prior].0 == query;
                    assert(index.entries()[position].1 == previous.entries()[prior].1);
                } else {
                    assert(index.entries()[position].1 == updated_values);
                }
            }
        }
    }
    Some(())
}

pub open spec fn variant_row_weight(outer: KeyView, inner: KeyView, delta: int) -> spec_fn((Key, Arc<i128>)) -> int {
    |row: (Key, Arc<i128>)| if outer == (row.0.view().0, Seq::<u8>::empty()) && inner == (0int, row.0.view().1) { delta * (*row.1 as int) } else { 0 }
}

pub fn add_weighted_variants(target: &mut Slices, variants: &Counts, delta: i128) -> (result: Option<()>)
    ensures result.is_some() ==> forall|outer: KeyView, inner: KeyView| #![trigger nested_count(*final(target), outer, inner)]
        nested_count(*final(target), outer, inner) == nested_count(*old(target), outer, inner)
            + if outer.1 == Seq::<u8>::empty() && inner.0 == 0 { delta as int * count(*variants, (outer.0, inner.1)) } else { 0 },
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let ghost previous = *target;
    let rows = variants.rows();
    let mut position = 0usize;
    while position < rows.len()
        invariant
            position <= rows.len(), variants.wf(),
            rows@.map_values(|row: (Key, Arc<i128>)| (row.0.view(), *row.1)) == variants.entries(),
            forall|outer: KeyView, inner: KeyView| #![trigger nested_count(*target, outer, inner)]
                nested_count(*target, outer, inner) == nested_count(previous, outer, inner)
                    + sum(rows@.take(position as int), variant_row_weight(outer, inner, delta as int)),
        decreases rows.len() - position,
    {
        let (key, multiplicity) = &rows[position];
        let change = some_or_return!(delta.checked_mul(**multiplicity));
        some_or_return!(adjust_slice(target, Key::number(key.number), key.with_number(0), change, false));
        proof {
            assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(*target, outer, inner)]
                nested_count(*target, outer, inner) == nested_count(previous, outer, inner)
                    + sum(rows@.take(position as int + 1), variant_row_weight(outer, inner, delta as int)) by {
                lemma_sum_take(rows@, position as int, variant_row_weight(outer, inner, delta as int));
            }
        }
        position += 1;
    }
    proof {
        assert(rows@.take(position as int) =~= rows@);
        assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(*target, outer, inner)]
            nested_count(*target, outer, inner) == nested_count(previous, outer, inner)
                + if outer.1 == Seq::<u8>::empty() && inner.0 == 0 { delta as int * count(*variants, (outer.0, inner.1)) } else { 0 } by {
            if outer.1 == Seq::<u8>::empty() && inner.0 == 0 {
                let weight = count_row_weight((outer.0, inner.1));
                lemma_count_sum(variants, (outer.0, inner.1));
                lemma_sum_map(rows@, |row: (Key, Arc<i128>)| (row.0.view(), *row.1), entry_weight((outer.0, inner.1)), weight);
                lemma_sum_scale(rows@, weight, variant_row_weight(outer, inner, delta as int), delta as int);
            } else { lemma_sum_zero(rows@, variant_row_weight(outer, inner, delta as int)); }
        }
    }
    Some(())
}

pub open spec fn slice_row_weight(outer: KeyView, inner: KeyView) -> spec_fn((Key, Arc<Counts>)) -> int {
    |row: (Key, Arc<Counts>)| if row.0.view() == outer { count(*row.1, inner) } else { 0 }
}

pub fn apply_slice_changes(target: &mut Slices, changes: &Slices) -> (result: Option<()>)
    ensures
        result.is_some() ==> forall|outer: KeyView, inner: KeyView| #![trigger nested_count(*final(target), outer, inner)]
            nested_count(*final(target), outer, inner) == nested_count(*old(target), outer, inner) + nested_count(*changes, outer, inner),
        result.is_some() && positive_slices(*old(target)) ==> positive_slices(*final(target)),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let ghost previous = *target;
    let outer_rows = changes.rows();
    let mut outer_position = 0usize;
    while outer_position < outer_rows.len()
        invariant
            outer_position <= outer_rows.len(), changes.wf(),
            outer_rows@.map_values(|row: (Key, Arc<Counts>)| (row.0.view(), *row.1)) == changes.entries(),
            positive_slices(previous) ==> positive_slices(*target),
            forall|outer: KeyView, inner: KeyView| #![trigger nested_count(*target, outer, inner)]
                nested_count(*target, outer, inner) == nested_count(previous, outer, inner)
                    + sum(outer_rows@.take(outer_position as int), slice_row_weight(outer, inner)),
        decreases outer_rows.len() - outer_position,
    {
        let (outer, values) = &outer_rows[outer_position];
        let inner_rows = values.rows();
        let ghost before_inner = *target;
        let mut inner_position = 0usize;
        while inner_position < inner_rows.len()
            invariant
                inner_position <= inner_rows.len(), values.wf(),
                inner_rows@.map_values(|row: (Key, Arc<i128>)| (row.0.view(), *row.1)) == values.entries(),
                positive_slices(previous) ==> positive_slices(*target),
                forall|query_outer: KeyView, query_inner: KeyView| #![trigger nested_count(*target, query_outer, query_inner)]
                    nested_count(*target, query_outer, query_inner) == nested_count(before_inner, query_outer, query_inner)
                        + if query_outer == outer.view() { sum(inner_rows@.take(inner_position as int), count_row_weight(query_inner)) } else { 0 },
            decreases inner_rows.len() - inner_position,
        {
            let (inner, delta) = &inner_rows[inner_position];
            some_or_return!(adjust_slice(target, outer.share(), inner.share(), **delta, true));
            proof {
                assert forall|query_outer: KeyView, query_inner: KeyView| #![trigger nested_count(*target, query_outer, query_inner)]
                    nested_count(*target, query_outer, query_inner) == nested_count(before_inner, query_outer, query_inner)
                        + if query_outer == outer.view() { sum(inner_rows@.take(inner_position as int + 1), count_row_weight(query_inner)) } else { 0 } by {
                    lemma_sum_take(inner_rows@, inner_position as int, count_row_weight(query_inner));
                }
            }
            inner_position += 1;
        }
        proof {
            assert(inner_rows@.take(inner_position as int) =~= inner_rows@);
            assert forall|query_outer: KeyView, query_inner: KeyView| #![trigger nested_count(*target, query_outer, query_inner)]
                nested_count(*target, query_outer, query_inner) == nested_count(previous, query_outer, query_inner)
                    + sum(outer_rows@.take(outer_position as int + 1), slice_row_weight(query_outer, query_inner)) by {
                lemma_count_sum(&**values, query_inner);
                lemma_sum_map(inner_rows@, |row: (Key, Arc<i128>)| (row.0.view(), *row.1), entry_weight(query_inner), count_row_weight(query_inner));
                lemma_sum_take(outer_rows@, outer_position as int, slice_row_weight(query_outer, query_inner));
            }
        }
        outer_position += 1;
    }
    proof {
        assert(outer_rows@.take(outer_position as int) =~= outer_rows@);
        assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(*target, outer, inner)]
            nested_count(*target, outer, inner) == nested_count(previous, outer, inner) + nested_count(*changes, outer, inner) by {
            lemma_nested_count_sum(changes, outer, inner);
            lemma_sum_map(outer_rows@, |row: (Key, Arc<Counts>)| (row.0.view(), *row.1), slice_entry_weight(outer, inner), slice_row_weight(outer, inner));
        }
    }
    Some(())
}

}
