use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::values::*;
use crate::storage::*;

verus! {

pub proof fn lemma_group_per_nation(db: Db, key: int, name: Seq<u8>, year: int, field: int)
    ensures sum(db.nation, |row: NationRow| if row.n_nationkey == key {
        if row.n_name == name { nation_total(db, row.n_nationkey, year, field) } else { 0int }
    } else { 0int }) == nation_count(db, key, name) * nation_total(db, key, year, field),
{
    let count = |row: NationRow| if row.n_nationkey == key && row.n_name == name { 1int } else { 0int };
    let filtered = |row: NationRow| if row.n_nationkey == key {
        if row.n_name == name { nation_total(db, row.n_nationkey, year, field) } else { 0int }
    } else { 0int };
    let amount = nation_total(db, key, year, field);
    lemma_sum_scale(db.nation, count, filtered, amount);
    assert(amount * nation_count(db, key, name) == nation_count(db, key, name) * amount) by (nonlinear_arith);
}

pub proof fn lemma_group_fold(db: Db, totals: Work<Vec<Total>>, names: Work<Vec<Name>>, name: Seq<u8>, year: int, field: int)
    requires database_keys_fit(db), nation_totals_ready(totals, db), names_ready(names, db), 0 <= field <= 1,
    ensures sum(totals.keys@, nation_work_weight(totals, names, name, year, field)) == group_database(db, name, year, field),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let source = |row: NationRow| if row.n_name == name { nation_total(db, row.n_nationkey, year, field) } else { 0int };
    let keys = totals.keys@.map_values(|key: i64| key as int);
    let key_of = |row: NationRow| row.n_nationkey;
    let per = |key: int| nation_count(db, key, name) * nation_total(db, key, year, field);
    assert(distinct(keys));
    assert forall|index: int| 0 <= index < db.nation.len() && source(db.nation[index]) != 0 implies keys.contains(key_of(#[trigger] db.nation[index])) by {
        let key = db.nation[index].n_nationkey;
        assert((key as i64) as int == key);
        assert(work_total(totals, key as i64, year, field) == nation_total(db, key, year, field));
        assert(totals.rows@.contains_key(key as i64));
        assert(totals.keys@.contains(key as i64));
        let position = choose|position: int| 0 <= position < totals.keys.len() && #[trigger] totals.keys[position] == key as i64;
        assert(keys[position] == key);
    }
    assert forall|key: int| #[trigger] per(key) == sum(db.nation, |row: NationRow| if key_of(row) == key { source(row) } else { 0int }) by {
        lemma_group_per_nation(db, key, name, year, field);
        lemma_sum_ext_all(db.nation,
            |row: NationRow| if row.n_nationkey == key { if row.n_name == name { nation_total(db, row.n_nationkey, year, field) } else { 0int } } else { 0int },
            |row: NationRow| if key_of(row) == key { source(row) } else { 0int });
    }
    lemma_sum_group(db.nation, keys, key_of, source, per);
    let projected = |key: i64| per(key as int);
    lemma_sum_map(totals.keys@, |key: i64| key as int, per, projected);
    assert forall|index: int| 0 <= index < totals.keys.len() implies
        nation_work_weight(totals, names, name, year, field)(#[trigger] totals.keys[index]) == projected(totals.keys[index]) by {
        let key = totals.keys[index];
        assert(work_name(names, key, name) == nation_count(db, key as int, name));
        assert(work_total(totals, key, year, field) == nation_total(db, key as int, year, field));
    }
    lemma_sum_ext(totals.keys@, nation_work_weight(totals, names, name, year, field), projected);
}

}
