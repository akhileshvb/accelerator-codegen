// Q9 kernel: a rollup per tracked pattern '%color%', kept exact under every write.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rat::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::values::*;
use crate::rows::*;
use crate::dimensions::*;
use crate::database::*;
use crate::construction::*;
use crate::output::*;
use crate::aggregate::*;
use crate::readside::*;
use crate::query_proofs::*;
use crate::events::*;
use crate::orders::*;
use crate::rollup::*;
use crate::buildparts::*;
use crate::build2::*;
use crate::patterns::*;
use crate::exec::*;
use crate::maskupdate::*;
use crate::pairupdate::*;
use crate::yw::*;
use crate::applyspec::*;
use crate::applydiff::*;
use crate::finish::*;
use std::sync::Arc;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct State {
    db: Ghost<Db>,
    pub pats: Arc<Vec<Vec<u8>>>,
    pub masks: Pages<Vec<MaskRow>>,
    pub pairs: Pages<Vec<Pair>>,
    pub suppliers: Pages<Vec<Number>>,
    pub names: Pages<Vec<Name>>,
    pub rollup: Vec<Arc<Vec<RCell>>>,
}

pub open spec fn rollup_ready(r: Seq<Arc<Vec<RCell>>>, pats: Seq<Seq<u8>>, db: Db) -> bool {
    r.len() == pats.len()
        && forall|c: int, nk: int, y: int, f: int| 0 <= c < pats.len() && 0 <= f <= 1 ==> #[trigger] cell_at((*r[c])@, nk, y, f)
            == nation_total(filter_db(db, pats[c]), nk, y, f)
}

impl State {
    pub closed spec fn db(self) -> Db {
        self.db@
    }

    #[verifier::type_invariant]
    pub closed spec fn valid(self) -> bool {
        &&& (*self.pats).len() <= 127
        &&& database_keys_fit(self.db())
        &&& masks_ready(self.masks, self.db(), pat_views((*self.pats)@))
        &&& pairs_ready(self.pairs, self.db())
        &&& supplier_pages_ready(self.suppliers, self.db())
        &&& nation_pages_ready(self.names, self.db())
        &&& rollup_ready(self.rollup@, pat_views((*self.pats)@), self.db())
    }
}

#[verifier::spinoff_prover]
pub fn build(
    all_parts: Vec<PartIn>,
    all_orders: &[OrderIn],
    supplier_nations: &[SupplierIn],
    nation_names: Vec<NationIn>,
    all_partsupp: &[PartsuppIn],
    all_lines: &[LineIn],
    Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires
        scan_all_parts_ok(db, all_parts@),
        scan_all_orders_ok(db, all_orders@),
        scan_supplier_nations_ok(db, supplier_nations@),
        scan_nation_names_ok(db, nation_names@),
        scan_all_partsupp_ok(db, all_partsupp@),
        scan_all_lines_ok(db, all_lines@),
    ensures
        r matches Some(s) ==> s.db() == db,
{
    let pats = tracked_patterns();
    let ghost pv = pat_views(pats@);
    let masks = build_masks(&all_parts, &pats, Ghost(db))?;
    let pairs = build_pairs(all_partsupp, Ghost(db))?;
    let suppliers = build_suppliers(supplier_nations, Ghost(db))?;
    let names = build_names(&nation_names, Ghost(db))?;
    let dates_w = build_dates(all_orders, Ghost(db));
    let n = pats.len();
    let mut delta = build_rollup(all_lines, &dates_w, &masks, &pairs, &suppliers, n, Ghost(db), Ghost(pv))?;
    let ghost delta_view = delta@;
    let mut rollup: Vec<Arc<Vec<RCell>>> = Vec::new();
    while delta.len() > 0
        invariant n == pats.len(), n == pv.len(), pv == pat_views(pats@), delta_view.len() == n,
            delta@ == delta_view.skip(rollup.len() as int), rollup.len() + delta.len() == n,
            forall|c: int| 0 <= c < rollup.len() ==> (*rollup@[c])@ == delta_view[c]@,
        decreases delta.len(),
    {
        let ghost old_delta = delta@;
        let ghost old_rollup = rollup@;
        let v = delta.remove(0);
        let ghost vv = v@;
        rollup.push(Arc::new(v));
        proof {
            assert(delta@ =~= old_delta.skip(1));
            assert(delta_view.skip(old_rollup.len() as int).skip(1) =~= delta_view.skip(old_rollup.len() as int + 1));
            assert(old_delta[0] == delta_view.skip(old_rollup.len() as int)[0]);
            assert(delta_view.skip(old_rollup.len() as int)[0] == delta_view[old_rollup.len() as int]);
            assert forall|c: int| 0 <= c < rollup.len() implies (*#[trigger] rollup@[c])@ == delta_view[c]@ by {
                if c < old_rollup.len() { assert(rollup@[c] == old_rollup[c]); }
            }
        }
    }
    proof {
        lemma_supplier_key_ranges(db, supplier_nations@);
        lemma_nation_key_ranges(db, nation_names@);
        assert(database_keys_fit(db));
        assert(rollup.len() == n);
        assert forall|c: int, nk: int, y: int, f: int| 0 <= c < pv.len() && 0 <= f <= 1 implies #[trigger] cell_at((*rollup@[c])@, nk, y, f)
            == nation_total(filter_db(db, pv[c]), nk, y, f) by {
            assert(delta_view[c]@ == (*rollup@[c])@);
        }
    }
    Some(State { db: Ghost(db), pats: Arc::new(pats), masks, pairs, suppliers, names, rollup })
}


#[verifier::spinoff_prover]
pub fn execute(
    s: &State,
    p1: &[u8],
) -> (r: Option<Vec<(Vec<u8>, i64, i128)>>)
    ensures
        r matches Some(out) ==> query_result(s.db(), params(p1@), out_view(out@)),
{
    proof { use_type_invariant(s); }
    let ghost db = s.db();
    let ghost pv = pat_views((*s.pats)@);
    let pats = &*s.pats;
    let mut c = 0;
    while c < pats.len() && !bytes_eq_slice(p1, pats[c].as_slice())
        invariant c <= pats.len(), forall|j: int| 0 <= j < c ==> p1@ != (#[trigger] pats@[j])@,
        decreases pats.len() - c,
    { c += 1; }
    if c == pats.len() { return None; }
    let ghost dbp = filter_db(db, p1@);
    proof {
        assert(pv[c as int] == p1@);
        lemma_filter_same(db, p1@);
    }
    let nation_year = cells_to_totals(&s.rollup[c])?;
    let names = names_to_work(&s.names, Ghost(db))?;
    proof {
        assert(database_keys_fit(dbp)) by {
            assert(dbp.supplier == db.supplier && dbp.nation == db.nation);
        }
        assert(nation_totals_ready(nation_year, dbp)) by {
            assert forall|key: i64, year: int, field: int| 0 <= field <= 1 implies #[trigger] work_total(nation_year, key, year, field) == nation_total(dbp, key as int, year, field) by {
                assert(cell_at((*s.rollup@[c as int])@, key as int, year, field) == nation_total(filter_db(db, pv[c as int]), key as int, year, field));
            }
        }
        assert(names_ready(names, dbp)) by {
            assert forall|key: i64, name: Seq<u8>| #[trigger] work_name(names, key, name) == nation_count(dbp, key as int, name) by {}
        }
    }
    let mut groups: Work<Vec<Group>> = Work::new();
    let mut index = 0;
    proof {
        assert forall|target: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies
            #[trigger] work_group(groups, target, year, field) == sum(nation_year.keys@.take(0), nation_work_weight(nation_year, names, target, year, field)) by {
            lemma_sum_empty(nation_year.keys@.take(0), nation_work_weight(nation_year, names, target, year, field));
        }
    }
    while index < nation_year.keys.len()
        invariant index <= nation_year.keys.len(), nation_year.valid(), names.valid(), groups.valid(),
            totals_work_valid(nation_year), groups_work_valid(groups),
            nation_totals_ready(nation_year, dbp), names_ready(names, dbp),
            forall|target: Seq<u8>, year: int, field: int| 0 <= field <= 1 ==>
                #[trigger] work_group(groups, target, year, field) == sum(nation_year.keys@.take(index as int), nation_work_weight(nation_year, names, target, year, field)),
        decreases nation_year.keys.len() - index,
    {
        let key = nation_year.keys[index];
        if let Some(rows) = names.rows.get(&key) { add_nation(&mut groups, nation_year.rows.get(&key)?, rows, 1)?; }
        proof {
            assert forall|target: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies
                #[trigger] work_group(groups, target, year, field) == sum(nation_year.keys@.take(index as int + 1), nation_work_weight(nation_year, names, target, year, field)) by {
                lemma_sum_take(nation_year.keys@, index as int, nation_work_weight(nation_year, names, target, year, field));
                if !names.rows@.contains_key(key) {
                    assert(work_name(names, key, target) == 0);
                    assert(0int * work_total(nation_year, key, year, field) == 0) by (nonlinear_arith);
                }
            }
        }
        index += 1;
    }
    proof {
        assert(nation_year.keys@.take(index as int) =~= nation_year.keys@);
        assert forall|target: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies
            #[trigger] work_group(groups, target, year, field) == group_database(dbp, target, year, field) by {
            lemma_group_fold(dbp, nation_year, names, target, year, field);
        }
    }
    let pages = Pages::from_work(groups);
    proof {
        assert(groups_shape(pages)) by {
            assert forall|key: i64| #[trigger] pages.at(key).is_some() implies {
                let g = pages.at(key).unwrap();
                g.len() > 0 && g@.map_values(|group: Group| group.name@).no_duplicates()
                    && forall|index: int| 0 <= index < g.len() ==> {
                        let group = #[trigger] g[index];
                        name_hash(group.name@) == key as int && group.totals.len() > 0 && totals_valid(group.totals@)
                    }
            } by {}
        }
        assert forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies
            #[trigger] page_group(pages, name, year, field) == group_database(dbp, name, year, field) by {
            assert(page_group(pages, name, year, field) == work_group(groups, name, year, field));
        }
    }
    let out = emit(&pages)?;
    proof { lemma_read_meaning(db, p1@, pages, out@); }
    Some(out)
}

pub fn rescan_lines_for_orders_keys(s: &State, lineitem_delta: &[LineDelta], nation_delta: &Vec<NationDelta>, orders_delta: &[OrderDelta],
    part_delta: &Vec<PartDelta>, partsupp_delta: &[PartsuppDelta], supplier_delta: &[SupplierDelta]) -> (r: Vec<i64>)
{ collect_okeys(orders_delta) }

pub fn rescan_lines_for_parts_keys(s: &State, lineitem_delta: &[LineDelta], nation_delta: &Vec<NationDelta>, orders_delta: &[OrderDelta],
    part_delta: &Vec<PartDelta>, partsupp_delta: &[PartsuppDelta], supplier_delta: &[SupplierDelta]) -> (r: Vec<i64>)
{ collect_pkeys(part_delta, partsupp_delta) }

pub fn rescan_lines_for_suppliers_keys(s: &State, lineitem_delta: &[LineDelta], nation_delta: &Vec<NationDelta>, orders_delta: &[OrderDelta],
    part_delta: &Vec<PartDelta>, partsupp_delta: &[PartsuppDelta], supplier_delta: &[SupplierDelta]) -> (r: Vec<i64>)
{ collect_skeys(supplier_delta) }

pub fn rescan_partsupp_for_parts_keys(s: &State, lineitem_delta: &[LineDelta], nation_delta: &Vec<NationDelta>, orders_delta: &[OrderDelta],
    part_delta: &Vec<PartDelta>, partsupp_delta: &[PartsuppDelta], supplier_delta: &[SupplierDelta]) -> (r: Vec<i64>)
{ collect_pskeys(partsupp_delta) }

pub fn rescan_orders_for_keys_keys(s: &State, lineitem_delta: &[LineDelta], nation_delta: &Vec<NationDelta>, orders_delta: &[OrderDelta],
    part_delta: &Vec<PartDelta>, partsupp_delta: &[PartsuppDelta], supplier_delta: &[SupplierDelta]) -> (r: Vec<i64>)
{
    let mut keys = collect_okeys(orders_delta);
    let mut i = 0;
    while i < lineitem_delta.len()
        invariant i <= lineitem_delta.len(),
        decreases lineitem_delta.len() - i,
    { keys.push(lineitem_delta[i].row.l_orderkey); i += 1; }
    keys
}

#[verifier::spinoff_prover]
pub fn apply(
    s: &State,
    lineitem_delta: &[LineDelta],
    nation_delta: &Vec<NationDelta>,
    orders_delta: &[OrderDelta],
    part_delta: &Vec<PartDelta>,
    partsupp_delta: &[PartsuppDelta],
    supplier_delta: &[SupplierDelta],
    lines_for_orders_keys: &[i64],
    lines_for_orders_rows: &[LineIn],
    lines_for_parts_keys: &[i64],
    lines_for_parts_rows: &[LineIn],
    lines_for_suppliers_keys: &[i64],
    lines_for_suppliers_rows: &[LineIn],
    partsupp_for_parts_keys: &[i64],
    partsupp_for_parts_rows: &[PartsuppIn],
    orders_for_keys_keys: &[i64],
    orders_for_keys_rows: &[OrderIn],
    Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires
        capture_ok(s.db(), db2, line_delta_seq_view(lineitem_delta@), nation_delta_seq_view(nation_delta@), order_delta_seq_view(orders_delta@), part_delta_seq_view(part_delta@), partsupp_delta_seq_view(partsupp_delta@), supplier_delta_seq_view(supplier_delta@)),
        rescan_lines_for_orders_ok(db2, lines_for_orders_keys@, lines_for_orders_rows@),
        rescan_lines_for_parts_ok(db2, lines_for_parts_keys@, lines_for_parts_rows@),
        rescan_lines_for_suppliers_ok(db2, lines_for_suppliers_keys@, lines_for_suppliers_rows@),
        rescan_partsupp_for_parts_ok(db2, partsupp_for_parts_keys@, partsupp_for_parts_rows@),
        rescan_orders_for_keys_ok(db2, orders_for_keys_keys@, orders_for_keys_rows@),
    ensures
        r matches Ok(s2) ==> s2.db() == db2,
{
    proof { use_type_invariant(s); }
    let ghost db = s.db();
    let pats_ref = &*s.pats;
    let ghost pv = pat_views_of(pats_ref@);
    if nation_delta.len() > 0 { return Err(1); }
    let n = pats_ref.len();
    let mut mwork = match update_masks(&s.masks, part_delta, pats_ref, Ghost(db), Ghost(db2)) { Some(w) => w, None => return Err(2) };
    let masks2 = match publish_masks(&s.masks, &mut mwork, Ghost(db), Ghost(db2), Ghost(pv)) { Some(p) => p, None => return Err(3) };
    let mut swork = match update_suppliers(&s.suppliers, supplier_delta, Ghost(db), Ghost(db2)) { Some(w) => w, None => return Err(4) };
    let sups2 = match publish_suppliers(&s.suppliers, &mut swork, Ghost(db), Ghost(db2)) { Some(p) => p, None => return Err(5) };
    let pwork = match pairs_from_rescan(partsupp_for_parts_keys, partsupp_for_parts_rows) { Some(w) => w, None => return Err(6) };
    let pskk = collect_pskeys(partsupp_delta);
    if !covers(partsupp_for_parts_keys, &pskk) { return Err(7); }
    proof {
        assert forall|i: int| 0 <= i < partsupp_delta@.len() implies partsupp_for_parts_keys@.contains((#[trigger] partsupp_delta@[i]).row.ps_partkey) by {
            assert(pskk@[i] == partsupp_delta@[i].row.ps_partkey);
            assert(pskk@.contains(pskk@[i]));
        }
    }
    let pairs2 = match publish_pairs(&s.pairs, &pwork, partsupp_for_parts_keys, partsupp_delta, Ghost(db), Ghost(db2), Ghost(partsupp_for_parts_rows@)) { Some(p) => p, None => return Err(8) };
    let post = rescan_dates(orders_for_keys_keys, orders_for_keys_rows, Ghost(db2));
    let chg = match collect_order_changes(orders_for_keys_keys, orders_delta) { Some(w) => w, None => return Err(9) };
    let okk = collect_okeys(orders_delta);
    let pkk = collect_pkeys(part_delta, partsupp_delta);
    let skk = collect_skeys(supplier_delta);
    if !covers(lines_for_orders_keys, &okk) { return Err(10); }
    if !covers(lines_for_parts_keys, &pkk) { return Err(11); }
    if !covers(lines_for_suppliers_keys, &skk) { return Err(12); }
    let mut delta = empty_delta(n);
    let ghost zero = delta@;
    if diff_rows(lines_for_orders_rows, 0, &okk, &pkk, &skk, &post, &chg, orders_for_keys_keys, orders_delta,
        &s.masks, &s.pairs, &s.suppliers, &masks2, &pairs2, &sups2, n, &mut delta, Ghost(db), Ghost(db2), Ghost(pv)).is_none() { return Err(13); }
    if diff_rows(lines_for_parts_rows, 1, &okk, &pkk, &skk, &post, &chg, orders_for_keys_keys, orders_delta,
        &s.masks, &s.pairs, &s.suppliers, &masks2, &pairs2, &sups2, n, &mut delta, Ghost(db), Ghost(db2), Ghost(pv)).is_none() { return Err(14); }
    if diff_rows(lines_for_suppliers_rows, 2, &okk, &pkk, &skk, &post, &chg, orders_for_keys_keys, orders_delta,
        &s.masks, &s.pairs, &s.suppliers, &masks2, &pairs2, &sups2, n, &mut delta, Ghost(db), Ghost(db2), Ghost(pv)).is_none() { return Err(15); }
    if delta_lines(lineitem_delta, &post, &chg, orders_for_keys_keys, orders_delta, &s.masks, &s.pairs, &s.suppliers, n, &mut delta,
        Ghost(db), Ghost(db2), Ghost(pv)).is_none() { return Err(16); }
    let mut rollup2: Vec<Arc<Vec<RCell>>> = Vec::new();
    let mut c = 0;
    while c < n
        invariant c <= n, n == pv.len(), delta.len() == n, rollup2.len() == c, s.valid(), rollup_ready(s.rollup@, pv, db), pv == pat_views_of((*s.pats)@),
            forall|c2: int, nk: int, y: int, f: int| 0 <= c2 < c && 0 <= f <= 1 ==> #[trigger] cell_at((*rollup2@[c2])@, nk, y, f)
                == cell_at((*s.rollup@[c2])@, nk, y, f) + dval(delta@, c2, nk, y, f),
        decreases n - c,
    {
        if delta[c].len() == 0 {
            proof { lemma_sum_empty(delta@[c as int]@, |row: RCell| 0int); }
            rollup2.push(Arc::clone(&s.rollup[c]));
            proof {
                assert forall|nk: int, y: int, f: int| 0 <= f <= 1 implies cell_at((*rollup2@[c as int])@, nk, y, f) == cell_at((*s.rollup@[c as int])@, nk, y, f) + dval(delta@, c as int, nk, y, f) by {
                    lemma_sum_empty(delta@[c as int]@, |row: RCell| if row.nk as int == nk && row.year as int == y { if f == 0 { row.count as int } else { row.profit as int } } else { 0int });
                }
            }
        } else {
            let v = match merge_cells(&s.rollup[c], &delta[c]) { Some(v) => v, None => return Err(17) };
            rollup2.push(Arc::new(v));
        }
        c += 1;
    }
    proof {
        lemma_keys_fit_step(db, db2, supplier_delta@, nation_delta@);
        assert forall|key: i64, name: Seq<u8>| #[trigger] name_page(s.names, key, name) == nation_count(db2, key as int, name) by {
            lemma_nation_same(db, db2, nation_delta@, key as int, name);
        }
        assert(nation_pages_ready(s.names, db2));
        assert forall|c2: int, nk: int, y: int, f: int| 0 <= c2 < pv.len() && 0 <= f <= 1 implies #[trigger] cell_at((*rollup2@[c2])@, nk, y, f)
            == nation_total(filter_db(db2, pv[c2]), nk, y, f) by {
            lemma_rollup_step(db, db2, pv[c2], nk, y, f, lineitem_delta@, orders_delta@, part_delta@, partsupp_delta@, supplier_delta@,
                okk@, pkk@, skk@, lines_for_orders_keys@, lines_for_orders_rows@, lines_for_parts_keys@, lines_for_parts_rows@,
                lines_for_suppliers_keys@, lines_for_suppliers_rows@);
        }
    }
    Ok(State { db: Ghost(db2), pats: Arc::clone(&s.pats), masks: masks2, pairs: pairs2, suppliers: sups2, names: s.names.share(), rollup: rollup2 })
}

}
