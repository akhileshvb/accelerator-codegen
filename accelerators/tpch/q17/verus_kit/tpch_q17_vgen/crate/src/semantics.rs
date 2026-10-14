use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::storage::*;

verus! {

pub open spec fn key_weight(key: int, weight: spec_fn(LineitemRow) -> int) -> spec_fn(LineitemRow) -> int {
    |row: LineitemRow| if row.l_partkey == key { weight(row) } else { 0 }
}

#[verifier::rlimit(1)]
pub proof fn lemma_key_equal(before: Seq<LineitemRow>, after: Seq<LineitemRow>, key: int)
    requires forall|weight: spec_fn(LineitemRow) -> int| #[trigger] sum(after, key_weight(key, weight)) == sum(before, key_weight(key, weight)),
    ensures number(before, key) == number(after, key), quantity(before, key) == quantity(after, key),
        line_count(before, key) == line_count(after, key), line_price(before, key) == line_price(after, key),
{
    reveal(number); reveal(quantity);
    let ones = |row: LineitemRow| 1int;
    let quantities = |row: LineitemRow| row.l_quantity;
    assert(sum(after, key_weight(key, ones)) == sum(before, key_weight(key, ones)));
    assert(sum(after, key_weight(key, quantities)) == sum(before, key_weight(key, quantities)));
    lemma_sum_ext_all(before, key_weight(key, ones), |row: LineitemRow| ind(row.l_partkey == key));
    lemma_sum_ext_all(after, key_weight(key, ones), |row: LineitemRow| ind(row.l_partkey == key));
    assert(number(before, key) == number(after, key));
    assert((key_weight(key, quantities)) =~= (|row: LineitemRow| if row.l_partkey == key { row.l_quantity } else { 0int }));
    assert(quantity(before, key) == quantity(after, key));
    assert forall|row: LineitemRow| #[trigger] qualifies(before, row) && row.l_partkey == key
        <==> qualifies(after, row) && row.l_partkey == key by { reveal(qualifies); }
    let count_weight = |row: LineitemRow| ind(qualifies(before, row));
    let price_weight = |row: LineitemRow| if qualifies(before, row) { row.l_extendedprice } else { 0int };
    assert(sum(after, key_weight(key, count_weight)) == sum(before, key_weight(key, count_weight)));
    assert(sum(after, key_weight(key, price_weight)) == sum(before, key_weight(key, price_weight)));
    lemma_sum_ext_all(before, key_weight(key, count_weight), |row: LineitemRow| ind(row.l_partkey == key && qualifies(before, row)));
    lemma_sum_ext_all(after, key_weight(key, count_weight), |row: LineitemRow| ind(row.l_partkey == key && qualifies(after, row)));
    lemma_sum_ext_all(before, key_weight(key, price_weight), |row: LineitemRow| if row.l_partkey == key && qualifies(before, row) { row.l_extendedprice } else { 0int });
    lemma_sum_ext_all(after, key_weight(key, price_weight), |row: LineitemRow| if row.l_partkey == key && qualifies(after, row) { row.l_extendedprice } else { 0int });
    reveal(line_count); reveal(line_price);
}

pub proof fn lemma_scan_lines(db: Db, rows: Seq<LineInput>, key: int)
    requires scan_lines_ok(db, rows),
    ensures line_count(line_input_seq_view(rows), key) == line_count(db.lineitem, key),
        line_price(line_input_seq_view(rows), key) == line_price(db.lineitem, key),
{
    assert forall|weight: spec_fn(LineitemRow) -> int| #[trigger] sum(db.lineitem, key_weight(key, weight))
        == sum(line_input_seq_view(rows), key_weight(key, weight)) by {
        assert(sum(line_input_seq_view(rows), key_weight(key, weight)) == sum(db.lineitem, key_weight(key, weight)));
    }
    lemma_key_equal(line_input_seq_view(rows), db.lineitem, key);
}

pub proof fn lemma_rescan_lines(db: Db, keys: Seq<i64>, rows: Seq<LineInput>, key: i64)
    requires rescan_lines_for_parts_ok(db, keys, rows), keys.contains(key),
    ensures line_count(line_input_seq_view(rows), key as int) == line_count(db.lineitem, key as int),
        line_price(line_input_seq_view(rows), key as int) == line_price(db.lineitem, key as int),
{
    let position = choose|position: int| 0 <= position < keys.len() && #[trigger] keys[position] == key;
    assert(seq_view_i64(keys)[position] == key as int);
    assert(seq_view_i64(keys).contains(key as int));
    assert forall|weight: spec_fn(LineitemRow) -> int| #[trigger] sum(db.lineitem, key_weight(key as int, weight))
        == sum(line_input_seq_view(rows), key_weight(key as int, weight)) by {
        let filtered = |row: LineitemRow| if seq_view_i64(keys).contains(row.l_partkey) { key_weight(key as int, weight)(row) } else { 0int };
        assert(sum(line_input_seq_view(rows), key_weight(key as int, weight)) == sum(db.lineitem, filtered));
        lemma_sum_ext_all(db.lineitem, filtered, key_weight(key as int, weight));
    }
    lemma_key_equal(line_input_seq_view(rows), db.lineitem, key as int);
}

pub proof fn lemma_untouched_lines(before: Db, after: Db, changes: Seq<(LineitemRow, int)>, key: i64)
    requires bag_step(before.lineitem, after.lineitem, changes),
        forall|index: int| #![trigger changes[index]] 0 <= index < changes.len() ==> changes[index].0.l_partkey != key as int,
    ensures line_count(before.lineitem, key as int) == line_count(after.lineitem, key as int),
        line_price(before.lineitem, key as int) == line_price(after.lineitem, key as int),
{
    assert forall|weight: spec_fn(LineitemRow) -> int| #[trigger] sum(after.lineitem, key_weight(key as int, weight))
        == sum(before.lineitem, key_weight(key as int, weight)) by {
        let signed = |entry: (LineitemRow, int)| entry.1 * key_weight(key as int, weight)(entry.0);
        lemma_sum_zero(changes, signed);
        assert(sum(after.lineitem, key_weight(key as int, weight)) == sum(before.lineitem, key_weight(key as int, weight)) + sum(changes, signed));
    }
    lemma_key_equal(before.lineitem, after.lineitem, key as int);
}

#[verifier::rlimit(1)]
pub proof fn lemma_factor_parts(rows: Seq<PartRow>, keys: Seq<i64>, weight: spec_fn(i64) -> int, brand: Seq<u8>, container: Seq<u8>)
    requires part_keys_fit(rows), keys.no_duplicates(),
        forall|index: int| #![trigger rows[index]] 0 <= index < rows.len() && rows[index].p_brand == brand && rows[index].p_container == container
            && weight(rows[index].p_partkey as i64) != 0 ==> keys.contains(rows[index].p_partkey as i64),
    ensures sum(rows, |row: PartRow| if row.p_brand == brand && row.p_container == container { weight(row.p_partkey as i64) } else { 0 })
        == sum(keys, |key: i64| part_count(rows, key as int, brand, container) * weight(key)),
{
    reveal(part_keys_fit); reveal(part_count);
    let by_row = |row: PartRow| if row.p_brand == brand && row.p_container == container { weight(row.p_partkey as i64) } else { 0int };
    let by_key = |key: i64| part_count(rows, key as int, brand, container) * weight(key);
    assert forall|key: i64| #[trigger] by_key(key) == sum(rows, |row: PartRow| if row.p_partkey as i64 == key { by_row(row) } else { 0int }) by {
        reveal(part_count);
        let count_weight = |row: PartRow| ind(row.p_partkey == key as int && row.p_brand == brand && row.p_container == container);
        let scaled = |row: PartRow| count_weight(row) * weight(key);
        assert(part_count(rows, key as int, brand, container) == sum(rows, count_weight));
        assert forall|index: int| 0 <= index < rows.len() implies #[trigger] scaled(rows[index])
            == (if rows[index].p_partkey as i64 == key { by_row(rows[index]) } else { 0int }) by {
            assert(i64::MIN <= rows[index].p_partkey <= i64::MAX);
            assert(rows[index].p_partkey as i64 as int == rows[index].p_partkey);
        }
        lemma_sum_scale(rows, count_weight, scaled, weight(key));
        lemma_sum_ext(rows, scaled, |row: PartRow| if row.p_partkey as i64 == key { by_row(row) } else { 0int });
        assert(sum(rows, count_weight) * weight(key) == weight(key) * sum(rows, count_weight)) by (nonlinear_arith);
    }
    assert forall|key: i64| #[trigger] by_key(key) == sum(rows, |row: PartRow| if (|entry: PartRow| entry.p_partkey as i64)(row) == key { by_row(row) } else { 0int }) by {
        assert((|row: PartRow| if row.p_partkey as i64 == key { by_row(row) } else { 0int })
            =~= (|row: PartRow| if (|entry: PartRow| entry.p_partkey as i64)(row) == key { by_row(row) } else { 0int }));
    }
    lemma_sum_group(rows, keys, |row: PartRow| row.p_partkey as i64, by_row, by_key);
}

pub open spec fn bad_part(row: PartRow) -> int { ind(row.p_partkey < i64::MIN || row.p_partkey > i64::MAX) }

pub proof fn lemma_fit_zero(rows: Seq<PartRow>)
    requires sum(rows, |row: PartRow| bad_part(row)) == 0,
    ensures part_keys_fit(rows),
{
    reveal(part_keys_fit);
    assert forall|index: int| #![trigger rows[index]] 0 <= index < rows.len() implies i64::MIN <= rows[index].p_partkey <= i64::MAX by {
        if bad_part(rows[index]) > 0 { lemma_sum_pos(rows, |row: PartRow| bad_part(row), index); }
    }
}

pub proof fn lemma_input_parts_fit(rows: Seq<PartInput>)
    ensures part_keys_fit(part_input_seq_view(rows)),
{
    reveal(part_keys_fit);
}

pub proof fn lemma_scan_parts_fit(db: Db, rows: Seq<PartInput>)
    requires scan_parts_ok(db, rows),
    ensures part_keys_fit(db.part),
{
    lemma_input_parts_fit(rows);
    reveal(part_keys_fit);
    lemma_sum_zero(part_input_seq_view(rows), |row: PartRow| bad_part(row));
    assert(sum(db.part, |row: PartRow| bad_part(row)) == sum(part_input_seq_view(rows), |row: PartRow| bad_part(row)));
    lemma_fit_zero(db.part);
}

pub proof fn lemma_factor_lines(db: Db, lines: Store<Moment>, keys: Seq<i64>, brand: Seq<u8>, container: Seq<u8>, price: bool)
    requires line_model(lines, db.lineitem), part_keys_fit(db.part), keys.no_duplicates(),
        forall|index: int| #![trigger db.part[index]] 0 <= index < db.part.len() ==> keys.contains(db.part[index].p_partkey as i64),
    ensures sum(keys, |key: i64| part_count(db.part, key as int, brand, container) * moment_value(lines, key, price)) == joined_value(db, brand, container, price),
{
    lemma_factor_parts(db.part, keys, |key: i64| moment_value(lines, key, price), brand, container);
    assert((|key: i64| part_count(db.part, key as int, brand, container) * (|entry: i64| moment_value(lines, entry, price))(key))
        =~= (|key: i64| part_count(db.part, key as int, brand, container) * moment_value(lines, key, price)));
    let stored = |row: PartRow| if row.p_brand == brand && row.p_container == container { moment_value(lines, row.p_partkey as i64, price) } else { 0int };
    let meaning = |row: PartRow| if row.p_brand == brand && row.p_container == container { line_value(db.lineitem, row.p_partkey, price) } else { 0int };
    assert((|row: PartRow| if row.p_brand == brand && row.p_container == container { (|key: i64| moment_value(lines, key, price))(row.p_partkey as i64) } else { 0int }) =~= stored);
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

pub proof fn lemma_build_group_model(db: Db, lines: Store<Moment>, groups: Store<Vec<Group>>, keys: Seq<i64>)
    requires line_model(lines, db.lineitem), part_keys_fit(db.part), keys.no_duplicates(), group_store_wf(groups),
        forall|index: int| #![trigger db.part[index]] 0 <= index < db.part.len() ==> keys.contains(db.part[index].p_partkey as i64),
        forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(groups, brand, container)
            == sum(keys, |key: i64| part_count(db.part, key as int, brand, container) * moment_at(lines, key).count as int),
        forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(groups, brand, container)
            == sum(keys, |key: i64| part_count(db.part, key as int, brand, container) * moment_at(lines, key).price as int),
        forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(groups, brand, container)
            == sum(keys, |key: i64| part_count(db.part, key as int, brand, container)),
    ensures group_model(groups, db),
{
    assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(groups, brand, container) == joined_count(db, brand, container) by {
        lemma_factor_lines(db, lines, keys, brand, container, false);
        assert((|key: i64| part_count(db.part, key as int, brand, container) * moment_value(lines, key, false))
            =~= (|key: i64| part_count(db.part, key as int, brand, container) * moment_at(lines, key).count as int));
    }
    assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(groups, brand, container) == joined_price(db, brand, container) by {
        lemma_factor_lines(db, lines, keys, brand, container, true);
        assert((|key: i64| part_count(db.part, key as int, brand, container) * moment_value(lines, key, true))
            =~= (|key: i64| part_count(db.part, key as int, brand, container) * moment_at(lines, key).price as int));
    }
    assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(groups, brand, container) == references(db.part, brand, container) by {
        lemma_factor_parts(db.part, keys, |key: i64| 1int, brand, container);
        assert((|row: PartRow| if row.p_brand == brand && row.p_container == container { (|key: i64| 1int)(row.p_partkey as i64) } else { 0int })
            =~= (|row: PartRow| ind(row.p_brand == brand && row.p_container == container)));
        assert((|key: i64| part_count(db.part, key as int, brand, container) * (|entry: i64| 1int)(key))
            =~= (|key: i64| part_count(db.part, key as int, brand, container)));
        lemma_sum_ext_all(keys, |key: i64| part_count(db.part, key as int, brand, container) * 1,
            |key: i64| part_count(db.part, key as int, brand, container));
        reveal(references);
        lemma_sum_ext_all(db.part, |row: PartRow| if row.p_brand == brand && row.p_container == container { 1int } else { 0int },
            |row: PartRow| ind(row.p_brand == brand && row.p_container == container));
    }
}

#[verifier::rlimit(1)]
pub proof fn lemma_build_state(db: Db, input_lines: Seq<LineInput>, input_parts: Seq<PartInput>, lines: Store<Moment>, parts: Store<Vec<Variant>>, groups: Store<Vec<Group>>)
    requires scan_lines_ok(db, input_lines), scan_parts_ok(db, input_parts),
        line_model(lines, line_input_seq_view(input_lines)), part_model(parts, part_input_seq_view(input_parts)),
        group_model(groups, Db { lineitem: line_input_seq_view(input_lines), part: part_input_seq_view(input_parts) }),
    ensures state_meaning(db, lines, parts, groups),
{
    assert forall|key: i64| #[trigger] moment_at(lines, key).count as int == line_count(db.lineitem, key as int) by { lemma_scan_lines(db, input_lines, key as int); }
    assert forall|key: i64| #[trigger] moment_at(lines, key).price as int == line_price(db.lineitem, key as int) by { lemma_scan_lines(db, input_lines, key as int); }
    assert forall|key: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] multiplicity(parts, key, brand, container) == part_count(db.part, key as int, brand, container) by {
        reveal(part_count);
        let weight = |row: PartRow| ind(row.p_partkey == key as int && row.p_brand == brand && row.p_container == container);
        assert(sum(part_input_seq_view(input_parts), weight) == sum(db.part, weight));
    }
    assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(groups, brand, container) == joined_count(db, brand, container) by {
        reveal(joined_count);
        let scanned = |row: PartRow| if row.p_brand == brand && row.p_container == container { line_count(line_input_seq_view(input_lines), row.p_partkey) } else { 0int };
        let database = |row: PartRow| if row.p_brand == brand && row.p_container == container { line_count(db.lineitem, row.p_partkey) } else { 0int };
        assert forall|row: PartRow| #[trigger] scanned(row) == database(row) by { lemma_scan_lines(db, input_lines, row.p_partkey); }
        lemma_sum_ext_all(part_input_seq_view(input_parts), scanned, database);
        assert(sum(part_input_seq_view(input_parts), database) == sum(db.part, database));
    }
    assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(groups, brand, container) == joined_price(db, brand, container) by {
        reveal(joined_price);
        let scanned = |row: PartRow| if row.p_brand == brand && row.p_container == container { line_price(line_input_seq_view(input_lines), row.p_partkey) } else { 0int };
        let database = |row: PartRow| if row.p_brand == brand && row.p_container == container { line_price(db.lineitem, row.p_partkey) } else { 0int };
        assert forall|row: PartRow| #[trigger] scanned(row) == database(row) by { lemma_scan_lines(db, input_lines, row.p_partkey); }
        lemma_sum_ext_all(part_input_seq_view(input_parts), scanned, database);
        assert(sum(part_input_seq_view(input_parts), database) == sum(db.part, database));
    }
    assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(groups, brand, container) == references(db.part, brand, container) by {
        reveal(references);
        let weight = |row: PartRow| ind(row.p_brand == brand && row.p_container == container);
        assert(sum(part_input_seq_view(input_parts), weight) == sum(db.part, weight));
    }
    lemma_scan_parts_fit(db, input_parts);
}

}
