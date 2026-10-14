use vstd::prelude::*;
use verus_common::rat::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::table::*;

verus! {

pub open spec fn read_total(data: Data, parameters: Params, year: int, volume: bool) -> int {
    match (data.dictionary.resolve_spec(parameters.p2), data.dictionary.resolve_spec(parameters.p3)) {
        (Some(region), Some(kind)) => if i64::MIN <= year <= i64::MAX {
            data.totals.at((region, kind, 0), (year as i64, 0, 0), volume)
        } else { 0 },
        _ => 0,
    }
}

pub open spec fn read_numerator(data: Data, parameters: Params, year: int) -> int {
    match (data.dictionary.resolve_spec(parameters.p2), data.dictionary.resolve_spec(parameters.p3), data.dictionary.resolve_spec(parameters.p1)) {
        (Some(region), Some(kind), Some(nation)) => if i64::MIN <= year <= i64::MAX {
            data.numerators.at((region, kind, nation), (year as i64, 0, 0), true)
        } else { 0 },
        _ => 0,
    }
}

#[verifier::opaque]
pub open spec fn read_valid(data: Data, db: Db) -> bool {
    &&& forall|parameters: Params, year: int| #[trigger] read_total(data, parameters, year, false) == group_count(db, parameters, year)
    &&& forall|parameters: Params, year: int| #[trigger] read_total(data, parameters, year, true) == group_sum(db, parameters, year, |tuple: Tuple| tuple.all_nations.volume)
    &&& forall|parameters: Params, year: int| #[trigger] read_numerator(data, parameters, year) == group_sum(db, parameters, year, |tuple: Tuple|
        if tuple.all_nations.nation == parameters.p1 { tuple.all_nations.volume } else { 0 })
    &&& forall|parameters: Params, year: int| #[trigger] group_count(db, parameters, year) > 0 ==> year == 1995 || year == 1996
}

pub proof fn lemma_empty(data: Data, db: Db, parameters: Params)
    requires read_valid(data, db),
        data.dictionary.resolve_spec(parameters.p2) is None || data.dictionary.resolve_spec(parameters.p3) is None,
    ensures query_result(db, parameters, Seq::empty()),
{
    hide(group_count);
    hide(group_sum);
    reveal(read_valid);
    assert forall|year: int| #[trigger] group_count(db, parameters, year) == 0 by {
        assert(read_total(data, parameters, year, false) == 0);
    }
}

pub proof fn lemma_statistics(data: Data, db: Db, parameters: Params, region: i64, kind: i64, nation: Option<i64>, year: i64)
    requires read_valid(data, db),
        data.dictionary.resolve_spec(parameters.p2) == Some(region),
        data.dictionary.resolve_spec(parameters.p3) == Some(kind),
        data.dictionary.resolve_spec(parameters.p1) == nation,
    ensures
        data.totals.at((region, kind, 0), (year, 0, 0), false) == group_count(db, parameters, year as int),
        data.totals.at((region, kind, 0), (year, 0, 0), true) == group_sum(db, parameters, year as int, |tuple: Tuple| tuple.all_nations.volume),
        (match nation { Some(id) => data.numerators.at((region, kind, id), (year, 0, 0), true), None => 0 }) ==
            group_sum(db, parameters, year as int, |tuple: Tuple| if tuple.all_nations.nation == parameters.p1 { tuple.all_nations.volume } else { 0 }),
{
    hide(group_count);
    hide(group_sum);
    reveal(read_valid);
    assert(read_total(data, parameters, year as int, false) == group_count(db, parameters, year as int));
    assert(read_total(data, parameters, year as int, true) == group_sum(db, parameters, year as int, |tuple: Tuple| tuple.all_nations.volume));
    assert(read_numerator(data, parameters, year as int) == group_sum(db, parameters, year as int, |tuple: Tuple|
        if tuple.all_nations.nation == parameters.p1 { tuple.all_nations.volume } else { 0 }));
}

pub proof fn lemma_push(db: Db, parameters: Params, output: Seq<OutRow>, row: OutRow, boundary: int)
    requires
        row_ok(db, parameters, row), row.o_year == boundary,
        forall|index: int| 0 <= index < output.len() ==> row_ok(db, parameters, #[trigger] output[index]) && output[index].o_year < boundary,
        forall|left: int, right: int| 0 <= left < right < output.len() ==> (#[trigger] output[left]).o_year < (#[trigger] output[right]).o_year,
    ensures
        forall|index: int| 0 <= index < output.push(row).len() ==> row_ok(db, parameters, #[trigger] output.push(row)[index]) && output.push(row)[index].o_year <= boundary,
        forall|left: int, right: int| 0 <= left < right < output.push(row).len() ==> (#[trigger] output.push(row)[left]).o_year < (#[trigger] output.push(row)[right]).o_year,
{
    assert forall|index: int| 0 <= index < output.push(row).len() implies
        row_ok(db, parameters, #[trigger] output.push(row)[index]) && output.push(row)[index].o_year <= boundary by {
        if index < output.len() { assert(output.push(row)[index] == output[index]); }
        else { assert(output.push(row)[index] == row); }
    }
    assert forall|left: int, right: int| 0 <= left < right < output.push(row).len() implies
        (#[trigger] output.push(row)[left]).o_year < (#[trigger] output.push(row)[right]).o_year by {
        if right < output.len() {
            assert(output.push(row)[left] == output[left]);
            assert(output.push(row)[right] == output[right]);
        } else {
            assert(output.push(row)[left] == output[left]);
            assert(output.push(row)[right] == row);
        }
    }
}

}
