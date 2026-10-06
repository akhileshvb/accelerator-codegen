use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::order::*;
use verus_common::rel::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::dimensions::Nations;

verus! {
pub proof fn lemma_empty_where(db: Db, parameters: Params)
    requires forall|supplier: int, nation: int| #![trigger db.supplier[supplier], db.nation[nation]]
        0 <= supplier < db.supplier.len() && 0 <= nation < db.nation.len() ==>
            !where_clause(db, parameters, db.supplier[supplier], db.nation[nation]),
    ensures crate::gen_spec::rows(db, parameters).len() == 0,
        query_result(db, parameters, Seq::empty()),
{
    assert forall|supplier: int| #![trigger db.supplier[supplier]] 0 <= supplier < db.supplier.len()
        implies tuples_1(db, parameters, db.supplier[supplier]).len() == 0 by {
        let row = db.supplier[supplier];
        let inner = db.nation.map_values(|nation: NationRow| tuples_2(db, parameters, row, nation));
        assert forall|index: int| #![trigger inner[index]] 0 <= index < inner.len() implies inner[index].len() == 0 by {
            assert(inner[index] == tuples_2(db, parameters, row, db.nation[index]));
            assert(!where_clause(db, parameters, row, db.nation[index]));
        }
        lemma_flat_empty(inner);
    }
    let outer = db.supplier.map_values(|supplier: SupplierRow| tuples_1(db, parameters, supplier));
    assert forall|index: int| #![trigger outer[index]] 0 <= index < outer.len() implies outer[index].len() == 0 by {
        assert(outer[index] == tuples_1(db, parameters, db.supplier[index]));
    }
    lemma_flat_empty(outer);
    assert(crate::gen_spec::rows(db, parameters) =~= Seq::<OutRow>::empty());
}

pub proof fn lemma_no_aggregate(db: Db, parameters: Params)
    requires rel_agg_lineitem(db, parameters).len() == 0,
    ensures forall|supplier: int| !#[trigger] in_1(db, parameters, supplier),
{
    assert forall|index: int| #![trigger db.partsupp[index]] 0 <= index < db.partsupp.len()
        implies in_1_tuples_1(db, parameters, db.partsupp[index]).len() == 0 by {
        let row = db.partsupp[index];
        let inner = rel_agg_lineitem(db, parameters).map_values(|aggregate: AggLineitemRow| in_1_tuples_2(db, parameters, row, aggregate));
        lemma_flat_empty(inner);
    }
    let outer = db.partsupp.map_values(|row: PartsuppRow| in_1_tuples_1(db, parameters, row));
    assert forall|index: int| #![trigger outer[index]] 0 <= index < outer.len() implies outer[index].len() == 0 by {
        assert(outer[index] == in_1_tuples_1(db, parameters, db.partsupp[index]));
    }
    lemma_flat_empty(outer);
}

pub proof fn lemma_empty_interval(db: Db, parameters: Params)
    requires parameters.p1 >= parameters.p4,
    ensures query_result(db, parameters, Seq::empty()),
{
    let inputs = db.lineitem.map_values(|row: LineitemRow| agg_lineitem_tuples_1(db, parameters, row));
    assert forall|index: int| #![trigger inputs[index]] 0 <= index < inputs.len() implies inputs[index].len() == 0 by {
        assert(inputs[index] == agg_lineitem_tuples_1(db, parameters, db.lineitem[index]));
    }
    lemma_flat_empty(inputs);
    assert(agg_lineitem_tuples(db, parameters).len() == 0);
    assert(rel_agg_lineitem(db, parameters).len() == 0);
    lemma_no_aggregate(db, parameters);
    lemma_empty_where(db, parameters);
}

pub proof fn lemma_absent_nation(db: Db, nations: Nations, parameters: Params)
    requires domain_valid(db), nations.lookup(crate::profiles::name_hash(parameters.p3) as i64).is_none(),
        forall|key: i64| #![trigger nation_count(nations, parameters.p3, key)]
            nation_count(nations, parameters.p3, key) == sum(db.nation, |row: NationRow| if row.n_name == parameters.p3 && row.n_nationkey == key { 1 } else { 0 }),
    ensures query_result(db, parameters, Seq::empty()),
{
    assert forall|index: int| #![trigger db.nation[index]] 0 <= index < db.nation.len()
        implies db.nation[index].n_name != parameters.p3 by {
        if db.nation[index].n_name == parameters.p3 {
            let key = db.nation[index].n_nationkey as i64;
            let weight = |row: NationRow| if row.n_name == parameters.p3 && row.n_nationkey == key { 1int } else { 0int };
            assert(nation_count(nations, parameters.p3, key) == 0);
            assert(key as int == db.nation[index].n_nationkey);
            lemma_sum_pos(db.nation, weight, index);
            assert(false);
        }
    }
    lemma_empty_where(db, parameters);
}
}
