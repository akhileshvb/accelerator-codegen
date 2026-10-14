use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::storage::*;
use crate::semantics::*;

verus! {

#[verifier::opaque]
pub open spec fn signed_part_count(rows: Seq<(PartRow, int)>, key: i64, brand: Seq<u8>, container: Seq<u8>) -> int {
    sum(rows, |entry: (PartRow, int)| entry.1 * ind(entry.0.p_partkey == key as int && entry.0.p_brand == brand && entry.0.p_container == container))
}

pub proof fn lemma_part_step(before: Db, after: Db, changes: Seq<(PartRow, int)>, key: i64, brand: Seq<u8>, container: Seq<u8>)
    requires bag_step(before.part, after.part, changes),
    ensures part_count(after.part, key as int, brand, container) == part_count(before.part, key as int, brand, container) + signed_part_count(changes, key, brand, container),
{
    reveal(part_count); reveal(signed_part_count);
    let weight = |row: PartRow| ind(row.p_partkey == key as int && row.p_brand == brand && row.p_container == container);
    assert(sum(after.part, weight) == sum(before.part, weight) + sum(changes, |entry: (PartRow, int)| entry.1 * weight(entry.0)));
    assert((|entry: (PartRow, int)| entry.1 * weight(entry.0)) =~= (|entry: (PartRow, int)| entry.1 * ind(entry.0.p_partkey == key as int && entry.0.p_brand == brand && entry.0.p_container == container)));
}

pub proof fn lemma_part_capture_fit(before: Db, after: Db, changes: Seq<(PartRow, int)>)
    requires part_keys_fit(before.part), bag_step(before.part, after.part, changes),
        forall|index: int| #![trigger changes[index]] 0 <= index < changes.len() ==> i64::MIN <= changes[index].0.p_partkey <= i64::MAX,
    ensures part_keys_fit(after.part),
{
    reveal(part_keys_fit);
    let weight = |row: PartRow| bad_part(row);
    lemma_sum_zero(before.part, weight);
    let signed = |entry: (PartRow, int)| entry.1 * weight(entry.0);
    lemma_sum_zero(changes, signed);
    assert(sum(after.part, weight) == sum(before.part, weight) + sum(changes, signed));
    lemma_fit_zero(after.part);
}

pub proof fn lemma_factor_delta(rows: Seq<(PartRow, int)>, keys: Seq<i64>, weight: spec_fn(i64) -> int, brand: Seq<u8>, container: Seq<u8>)
    requires keys.no_duplicates(),
        forall|index: int| #![trigger rows[index]] 0 <= index < rows.len() ==> i64::MIN <= rows[index].0.p_partkey <= i64::MAX && keys.contains(rows[index].0.p_partkey as i64),
    ensures sum(rows, |entry: (PartRow, int)| if entry.0.p_brand == brand && entry.0.p_container == container {
        entry.1 * weight(entry.0.p_partkey as i64)
    } else { 0 }) == sum(keys, |key: i64| signed_part_count(rows, key, brand, container) * weight(key)),
{
    let by_row = |entry: (PartRow, int)| if entry.0.p_brand == brand && entry.0.p_container == container { entry.1 * weight(entry.0.p_partkey as i64) } else { 0int };
    let by_key = |key: i64| signed_part_count(rows, key, brand, container) * weight(key);
    assert forall|key: i64| #[trigger] by_key(key) == sum(rows, |entry: (PartRow, int)| if entry.0.p_partkey as i64 == key { by_row(entry) } else { 0int }) by {
        reveal(signed_part_count);
        let count_weight = |entry: (PartRow, int)| entry.1 * ind(entry.0.p_partkey == key as int && entry.0.p_brand == brand && entry.0.p_container == container);
        let scaled = |entry: (PartRow, int)| count_weight(entry) * weight(key);
        assert(signed_part_count(rows, key, brand, container) == sum(rows, count_weight));
        lemma_sum_scale(rows, count_weight, scaled, weight(key));
        assert forall|index: int| 0 <= index < rows.len() implies #[trigger] scaled(rows[index])
            == (if rows[index].0.p_partkey as i64 == key { by_row(rows[index]) } else { 0int }) by {
            assert(rows[index].0.p_partkey as i64 as int == rows[index].0.p_partkey);
            if rows[index].0.p_partkey as i64 == key && rows[index].0.p_brand == brand && rows[index].0.p_container == container {
                assert(count_weight(rows[index]) == rows[index].1);
                assert(by_row(rows[index]) == rows[index].1 * weight(key));
            } else {
                assert(count_weight(rows[index]) == 0);
                assert(scaled(rows[index]) == count_weight(rows[index]) * weight(key));
                assert(scaled(rows[index]) == 0);
            }
        }
        lemma_sum_ext(rows, scaled, |entry: (PartRow, int)| if entry.0.p_partkey as i64 == key { by_row(entry) } else { 0int });
        assert(sum(rows, count_weight) * weight(key) == weight(key) * sum(rows, count_weight)) by (nonlinear_arith);
    }
    assert forall|key: i64| #[trigger] by_key(key) == sum(rows, |entry: (PartRow, int)| if (|row: (PartRow, int)| row.0.p_partkey as i64)(entry) == key { by_row(entry) } else { 0int }) by {
        assert((|entry: (PartRow, int)| if entry.0.p_partkey as i64 == key { by_row(entry) } else { 0int })
            =~= (|entry: (PartRow, int)| if (|row: (PartRow, int)| row.0.p_partkey as i64)(entry) == key { by_row(entry) } else { 0int }));
    }
    lemma_sum_group(rows, keys, |entry: (PartRow, int)| entry.0.p_partkey as i64, by_row, by_key);
}

pub proof fn lemma_join_rows(db: Db, lines: Store<Moment>, brand: Seq<u8>, container: Seq<u8>, price: bool)
    requires line_model(lines, db.lineitem), part_keys_fit(db.part),
    ensures sum(db.part, |row: PartRow| if row.p_brand == brand && row.p_container == container { moment_value(lines, row.p_partkey as i64, price) } else { 0 }) == joined_value(db, brand, container, price),
{
    let stored = |row: PartRow| if row.p_brand == brand && row.p_container == container { moment_value(lines, row.p_partkey as i64, price) } else { 0int };
    let meaning = |row: PartRow| if row.p_brand == brand && row.p_container == container { line_value(db.lineitem, row.p_partkey, price) } else { 0int };
    assert forall|index: int| 0 <= index < db.part.len() implies #[trigger] stored(db.part[index]) == meaning(db.part[index]) by {
        reveal(part_keys_fit);
        let row = db.part[index];
        assert(row.p_partkey as i64 as int == row.p_partkey);
        assert(moment_at(lines, row.p_partkey as i64).count as int == line_count(db.lineitem, row.p_partkey));
        assert(moment_at(lines, row.p_partkey as i64).price as int == line_price(db.lineitem, row.p_partkey));
    }
    lemma_sum_ext(db.part, stored, meaning);
    reveal(joined_count); reveal(joined_price);
    if price { assert(meaning =~= (|row: PartRow| if row.p_brand == brand && row.p_container == container { line_price(db.lineitem, row.p_partkey) } else { 0int })); }
    else { assert(meaning =~= (|row: PartRow| if row.p_brand == brand && row.p_container == container { line_count(db.lineitem, row.p_partkey) } else { 0int })); }
}

pub proof fn lemma_join_step(before: Db, after: Db, changes: Seq<(PartRow, int)>, touched: Seq<i64>, part_keys: Seq<i64>, before_lines: Store<Moment>, after_lines: Store<Moment>, brand: Seq<u8>, container: Seq<u8>, price: bool)
    requires line_model(before_lines, before.lineitem), line_model(after_lines, after.lineitem),
        part_keys_fit(before.part), part_keys_fit(after.part), bag_step(before.part, after.part, changes),
        touched.no_duplicates(), part_keys.no_duplicates(),
        forall|index: int| #![trigger changes[index]] 0 <= index < changes.len() ==> i64::MIN <= changes[index].0.p_partkey <= i64::MAX && part_keys.contains(changes[index].0.p_partkey as i64),
        forall|key: i64| !touched.contains(key) ==> #[trigger] moment_value(after_lines, key, price) == moment_value(before_lines, key, price),
    ensures joined_value(after, brand, container, price) == joined_value(before, brand, container, price)
        + sum(touched, |key: i64| part_count(before.part, key as int, brand, container) * (moment_value(after_lines, key, price) - moment_value(before_lines, key, price)))
        + sum(part_keys, |key: i64| signed_part_count(changes, key, brand, container) * moment_value(after_lines, key, price)),
{
    let old = |row: PartRow| if row.p_brand == brand && row.p_container == container { moment_value(before_lines, row.p_partkey as i64, price) } else { 0int };
    let new = |row: PartRow| if row.p_brand == brand && row.p_container == container { moment_value(after_lines, row.p_partkey as i64, price) } else { 0int };
    let difference = |row: PartRow| if row.p_brand == brand && row.p_container == container { moment_value(after_lines, row.p_partkey as i64, price) - moment_value(before_lines, row.p_partkey as i64, price) } else { 0int };
    lemma_join_rows(before, before_lines, brand, container, price);
    lemma_join_rows(after, after_lines, brand, container, price);
    lemma_sum_sub(before.part, new, old, difference);
    lemma_factor_parts(before.part, touched, |key: i64| moment_value(after_lines, key, price) - moment_value(before_lines, key, price), brand, container);
    lemma_factor_delta(changes, part_keys, |key: i64| moment_value(after_lines, key, price), brand, container);
    let signed = |entry: (PartRow, int)| entry.1 * new(entry.0);
    assert(sum(after.part, new) == sum(before.part, new) + sum(changes, signed));
    let factor_signed = |entry: (PartRow, int)| if entry.0.p_brand == brand && entry.0.p_container == container { entry.1 * moment_value(after_lines, entry.0.p_partkey as i64, price) } else { 0int };
    lemma_sum_ext_all(changes, signed, factor_signed);
    assert((|row: PartRow| if row.p_brand == brand && row.p_container == container { (|key: i64| moment_value(after_lines, key, price) - moment_value(before_lines, key, price))(row.p_partkey as i64) } else { 0int }) =~= difference);
    assert((|key: i64| part_count(before.part, key as int, brand, container) * (|entry: i64| moment_value(after_lines, entry, price) - moment_value(before_lines, entry, price))(key))
        =~= (|key: i64| part_count(before.part, key as int, brand, container) * (moment_value(after_lines, key, price) - moment_value(before_lines, key, price))));
    assert((|entry: (PartRow, int)| if entry.0.p_brand == brand && entry.0.p_container == container { entry.1 * (|key: i64| moment_value(after_lines, key, price))(entry.0.p_partkey as i64) } else { 0int }) =~= factor_signed);
    assert((|key: i64| signed_part_count(changes, key, brand, container) * (|entry: i64| moment_value(after_lines, entry, price))(key))
        =~= (|key: i64| signed_part_count(changes, key, brand, container) * moment_value(after_lines, key, price)));
}

pub proof fn lemma_reference_step(before: Db, after: Db, changes: Seq<(PartRow, int)>, keys: Seq<i64>, brand: Seq<u8>, container: Seq<u8>)
    requires bag_step(before.part, after.part, changes), keys.no_duplicates(),
        forall|index: int| #![trigger changes[index]] 0 <= index < changes.len() ==> i64::MIN <= changes[index].0.p_partkey <= i64::MAX && keys.contains(changes[index].0.p_partkey as i64),
    ensures references(after.part, brand, container) == references(before.part, brand, container) + sum(keys, |key: i64| signed_part_count(changes, key, brand, container)),
{
    reveal(references);
    let weight = |row: PartRow| ind(row.p_brand == brand && row.p_container == container);
    let signed = |entry: (PartRow, int)| entry.1 * weight(entry.0);
    assert(sum(after.part, weight) == sum(before.part, weight) + sum(changes, signed));
    lemma_factor_delta(changes, keys, |key: i64| 1int, brand, container);
    assert((|entry: (PartRow, int)| if entry.0.p_brand == brand && entry.0.p_container == container { entry.1 * (|key: i64| 1int)(entry.0.p_partkey as i64) } else { 0int }) =~= signed);
    assert((|key: i64| signed_part_count(changes, key, brand, container) * (|entry: i64| 1int)(key)) =~= (|key: i64| signed_part_count(changes, key, brand, container)));
}

pub proof fn lemma_apply_state(after: Db, lines: Store<Moment>, parts: Store<Vec<Variant>>, groups: Store<Vec<Group>>)
    requires line_model(lines, after.lineitem), part_model(parts, after.part), group_model(groups, after), part_keys_fit(after.part),
    ensures state_meaning(after, lines, parts, groups),
{}

}
