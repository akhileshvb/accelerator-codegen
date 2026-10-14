use vstd::prelude::*;
use verus_prooflib::like_match_exec;
use std::sync::Arc;
use verus_common::rel::{flat, min_of};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::bags::*;
use crate::model::*;
use crate::maintenance::{region_slice, index_positive};
use crate::storage::*;
use crate::semantics::*;
use crate::selection::*;
use crate::enumeration::*;
use crate::fast_proofs::*;
use crate::skip_proofs::*;
use crate::minimum_proofs::lemma_regions_nonnegative;
use verus_common::sqlfns::like_match;
use crate::parts::*;
use verus_common::order::nondesc;

verus! {

pub type Output = (i128, Vec<u8>, Vec<u8>, i64, Vec<u8>, Vec<u8>, Vec<u8>, Vec<u8>);

pub struct Candidate {
    pub part: Arc<BrassPartScan>,
    pub supplier: Arc<SupplierScan>,
    pub nation: Arc<NationScan>,
}

impl Candidate {
    pub open spec fn view(self) -> OutRow {
        OutRow {
            s_acctbal: (*self.supplier).s_acctbal as int,
            s_name: (*self.supplier).s_name@,
            n_name: (*self.nation).n_name@,
            p_partkey: (*self.part).p_partkey as int,
            p_mfgr: (*self.part).p_mfgr@,
            s_address: (*self.supplier).s_address@,
            s_phone: (*self.supplier).s_phone@,
            s_comment: (*self.supplier).s_comment@,
        }
    }

    pub fn share(&self) -> (result: Self)
        ensures result == *self,
    {
        Candidate { part: Arc::clone(&self.part), supplier: Arc::clone(&self.supplier), nation: Arc::clone(&self.nation) }
    }

    pub fn hydrate(&self) -> (result: Output)
        ensures out_view(seq![result])[0] == self.view(),
    {
        (self.supplier.s_acctbal as i128, copy_bytes(&self.supplier.s_name), copy_bytes(&self.nation.n_name),
            self.part.p_partkey, copy_bytes(&self.part.p_mfgr), copy_bytes(&self.supplier.s_address),
            copy_bytes(&self.supplier.s_phone), copy_bytes(&self.supplier.s_comment))
    }
}

pub fn before(left: &Candidate, right: &Candidate) -> (result: bool)
    ensures result == ext_lt(left.view(), right.view()),
{
    if left.supplier.s_acctbal != right.supplier.s_acctbal { return left.supplier.s_acctbal > right.supplier.s_acctbal; }
    let nation = compare_bytes(&left.nation.n_name, &right.nation.n_name);
    if nation != 0 { return nation < 0; }
    let supplier = compare_bytes(&left.supplier.s_name, &right.supplier.s_name);
    if supplier != 0 { return supplier < 0; }
    if left.part.p_partkey != right.part.p_partkey { return left.part.p_partkey < right.part.p_partkey; }
    let manufacturer = compare_bytes(&left.part.p_mfgr, &right.part.p_mfgr);
    if manufacturer != 0 { return manufacturer < 0; }
    let address = compare_bytes(&left.supplier.s_address, &right.supplier.s_address);
    if address != 0 { return address < 0; }
    let phone = compare_bytes(&left.supplier.s_phone, &right.supplier.s_phone);
    if phone != 0 { return phone < 0; }
    compare_bytes(&left.supplier.s_comment, &right.supplier.s_comment) < 0
}

pub fn retain(best: &mut Vec<Candidate>, candidate: &Candidate, copies: i128)
    requires old(best).len() <= 100, 0 <= copies <= 100, nondesc(candidate_rows(old(best)@), ext_order()),
    ensures final(best).len() <= 100, nondesc(candidate_rows(final(best)@), ext_order()),
        candidate_rows(final(best)@) == repeat_retained(candidate_rows(old(best)@), candidate.view(), copies as nat),
{
    let ghost original = candidate_rows(best@);
    let mut remaining = copies;
    while remaining > 0
        invariant best.len() <= 100, 0 <= remaining <= copies, nondesc(candidate_rows(best@), ext_order()),
            original == candidate_rows(old(best)@),
            candidate_rows(best@) == repeat_retained(original, candidate.view(), (copies - remaining) as nat),
        decreases remaining,
    {
        if best.len() == 100 && !before(candidate, &best[99]) {
            proof {
                lemma_reject_limited(candidate_rows(best@), candidate.view());
                lemma_repeat_unchanged(candidate_rows(best@), candidate.view(), remaining as nat);
                lemma_repeat_join(original, candidate.view(), (copies - remaining) as nat, remaining as nat);
                assert((copies - remaining) as nat + remaining as nat == copies as nat);
            }
            return;
        }
        let mut position = 0;
        while position < best.len() && !before(candidate, &best[position])
            invariant position <= best.len(), best.len() <= 100, nondesc(candidate_rows(best@), ext_order()),
                forall|index: int| 0 <= index < position ==> !ext_lt(candidate.view(), (#[trigger] best@[index]).view()),
            decreases best.len() - position,
        {
            position += 1;
        }
        let ghost previous = best@;
        proof {
            lemma_ordered_insert(candidate_rows(previous), candidate.view(), position as int);
            lemma_insert_position(candidate_rows(previous), candidate.view(), position as int);
        }
        best.insert(position, candidate.share());
        proof { assert(candidate_rows(best@) =~= candidate_rows(previous).insert(position as int, candidate.view())); }
        if best.len() > 100 {
            let ghost before_pop = best@;
            best.pop();
            proof { assert(candidate_rows(best@) =~= candidate_rows(before_pop).drop_last()); }
        }
        proof {
            assert(candidate_rows(best@) == bounded_insert(candidate_rows(previous), candidate.view()));
            assert(candidate_rows(best@) == repeat_retained(original, candidate.view(), (copies - remaining + 1) as nat));
        }
        remaining -= 1;
    }
}

pub open spec fn candidate_rows(candidates: Seq<Candidate>) -> Seq<OutRow> {
    candidates.map_values(|candidate: Candidate| candidate.view())
}

pub open spec fn supported(quote: BrassPartsuppScan, suppliers: Directory<Bag<SupplierScan>>,
    nations: Directory<Bag<NationScan>>, regions: Directory<Count>) -> bool
{
    match suppliers.lookup(quote.ps_suppkey) {
        None => false,
        Some(supplier) => supplier_bag_support(*supplier, nations, regions),
    }
}

pub open spec fn supplier_bag_support(rows: Bag<SupplierScan>, nations: Directory<Bag<NationScan>>, regions: Directory<Count>) -> bool {
    exists|index: int| 0 <= index < rows.entries@.len() &&
        (#[trigger] rows.entries@[index]).count > 0 && supplier_supported(*rows.entries@[index].row, nations, regions)
}

pub open spec fn nation_support(rows: Bag<NationScan>, regions: Directory<Count>) -> bool {
    exists|index: int| 0 <= index < rows.entries@.len() &&
        (#[trigger] rows.entries@[index]).count > 0 && count_at(regions, (*rows.entries@[index].row).n_regionkey) > 0
}

pub open spec fn supplier_supported(row: SupplierScan, nations: Directory<Bag<NationScan>>, regions: Directory<Count>) -> bool {
    match nations.lookup(row.s_nationkey) { None => false, Some(rows) => nation_support(*rows, regions) }
}

pub fn has_nation_support(rows: &Bag<NationScan>, regions: &Directory<Count>) -> (result: bool)
    requires regions.valid(),
    ensures result == nation_support(*rows, *regions),
{
    let mut index = 0;
    while index < rows.entries.len()
        invariant
            index <= rows.entries.len(), regions.valid(),
            forall|offset: int| 0 <= offset < index ==>
                (#[trigger] rows.entries@[offset]).count <= 0 || count_at(*regions, (*rows.entries@[offset].row).n_regionkey) <= 0,
        decreases rows.entries.len() - index,
    {
        let entry = &rows.entries[index];
        if entry.count > 0 {
            if let Some(region) = regions.get(entry.row.n_regionkey) {
                if region.value > 0 {
                    proof {
                        assert(rows.entries@[index as int].count > 0);
                        assert(count_at(*regions, (*rows.entries@[index as int].row).n_regionkey) > 0);
                    }
                    return true;
                }
            }
        }
        index += 1;
    }
    false
}

pub fn has_supplier_support(row: &SupplierScan, nations: &Directory<Bag<NationScan>>, regions: &Directory<Count>) -> (result: bool)
    requires nations.valid(), regions.valid(),
    ensures result == supplier_supported(*row, *nations, *regions),
{
    match nations.get(row.s_nationkey) {
        None => false,
        Some(rows) => has_nation_support(&rows, regions),
    }
}

pub fn has_quote_support(quote: &BrassPartsuppScan, suppliers: &Directory<Bag<SupplierScan>>,
    nations: &Directory<Bag<NationScan>>, regions: &Directory<Count>) -> (result: bool)
    requires suppliers.valid(), nations.valid(), regions.valid(),
    ensures result == supported(*quote, *suppliers, *nations, *regions),
{
    let supplier = match suppliers.get(quote.ps_suppkey) { None => return false, Some(rows) => rows };
    let mut index = 0;
    while index < supplier.entries.len()
        invariant
            index <= supplier.entries.len(), nations.valid(), regions.valid(),
            suppliers.lookup(quote.ps_suppkey) == Some(supplier),
            forall|offset: int| 0 <= offset < index ==> (#[trigger] supplier.entries@[offset]).count <= 0 ||
                !supplier_supported(*supplier.entries@[offset].row, *nations, *regions),
        decreases supplier.entries.len() - index,
    {
        let entry = &supplier.entries[index];
        if entry.count > 0 && has_supplier_support(&entry.row, nations, regions) {
            proof {
                assert(supplier.entries@[index as int].count > 0);
                assert(supplier_supported(*supplier.entries@[index as int].row, *nations, *regions));
                assert(exists|offset: int| 0 <= offset < supplier.entries@.len() &&
                    (#[trigger] supplier.entries@[offset]).count > 0 && supplier_supported(*supplier.entries@[offset].row, *nations, *regions));
                assert(suppliers.lookup(quote.ps_suppkey) == Some(supplier));
                assert((*supplier).entries@ == supplier.entries@);
                assert(exists|offset: int| 0 <= offset < (*supplier).entries@.len() &&
                    (#[trigger] (*supplier).entries@[offset]).count > 0 && supplier_supported(*(*supplier).entries@[offset].row, *nations, *regions));
                assert(supplier_bag_support(*supplier, *nations, *regions));
                assert(supported(*quote, *suppliers, *nations, *regions));
            }
            return true;
        }
        index += 1;
    }
    false
}

pub open spec fn supported_costs(quotes: Bag<BrassPartsuppScan>, suppliers: Directory<Bag<SupplierScan>>,
    nations: Directory<Bag<NationScan>>, regions: Directory<Count>) -> Seq<int>
{
    supported_costs_entries(quotes.entries@, suppliers, nations, regions)
}

pub open spec fn supported_costs_entries(entries: Seq<Counted<BrassPartsuppScan>>, suppliers: Directory<Bag<SupplierScan>>,
    nations: Directory<Bag<NationScan>>, regions: Directory<Count>) -> Seq<int>
{
    flat(entries.map_values(|entry: Counted<BrassPartsuppScan>|
        if entry.count > 0 && supported(*entry.row, suppliers, nations, regions) { seq![(*entry.row).ps_supplycost as int] }
        else { Seq::empty() }))
}

pub proof fn lemma_supported_cost_step(entries: Seq<Counted<BrassPartsuppScan>>, index: int,
    suppliers: Directory<Bag<SupplierScan>>, nations: Directory<Bag<NationScan>>, regions: Directory<Count>)
    requires 0 <= index < entries.len(),
    ensures supported_costs_entries(entries.take(index + 1), suppliers, nations, regions) ==
        if entries[index].count > 0 && supported(*entries[index].row, suppliers, nations, regions) {
            supported_costs_entries(entries.take(index), suppliers, nations, regions).push((*entries[index].row).ps_supplycost as int)
        } else { supported_costs_entries(entries.take(index), suppliers, nations, regions) },
{
    let mapper = |entry: Counted<BrassPartsuppScan>|
        if entry.count > 0 && supported(*entry.row, suppliers, nations, regions) { seq![(*entry.row).ps_supplycost as int] }
        else { Seq::<int>::empty() };
    let prefix = entries.take(index + 1).map_values(mapper);
    assert(prefix.drop_last() =~= entries.take(index).map_values(mapper));
    assert(prefix.last() == mapper(entries[index]));
    assert(flat(prefix) == flat(prefix.drop_last()) + prefix.last());
    if entries[index].count > 0 && supported(*entries[index].row, suppliers, nations, regions) {
        assert(flat(prefix.drop_last()) + seq![(*entries[index].row).ps_supplycost as int] =~=
            flat(prefix.drop_last()).push((*entries[index].row).ps_supplycost as int));
    } else { assert(flat(prefix.drop_last()) + Seq::<int>::empty() =~= flat(prefix.drop_last())); }
}

pub fn minimum(quotes: &Bag<BrassPartsuppScan>, flat: &Vec<crate::flat::SInfo>, suppliers: &Directory<Bag<SupplierScan>>,
    nations: &Directory<Bag<NationScan>>, regions: &Directory<Count>) -> (result: Option<i64>)
    requires suppliers.valid(), nations.valid(), regions.valid(), crate::flat::flat_ok(flat@, *suppliers), index_positive(*suppliers),
    ensures
        result == if supported_costs(*quotes, *suppliers, *nations, *regions).len() == 0 { None }
            else { Some(min_of(supported_costs(*quotes, *suppliers, *nations, *regions)) as i64) },
{
    let mut result: Option<i64> = None;
    let mut quote_index = 0;
    while quote_index < quotes.entries.len()
        invariant
            quote_index <= quotes.entries.len(), suppliers.valid(), nations.valid(), regions.valid(), crate::flat::flat_ok(flat@, *suppliers), index_positive(*suppliers),
            (result is None) == (supported_costs_entries(quotes.entries@.take(quote_index as int), *suppliers, *nations, *regions).len() == 0),
            result matches Some(cost) ==> cost as int == min_of(supported_costs_entries(quotes.entries@.take(quote_index as int), *suppliers, *nations, *regions)),
        decreases quotes.entries.len() - quote_index,
    {
        let quote = &quotes.entries[quote_index];
        let ghost previous = supported_costs_entries(quotes.entries@.take(quote_index as int), *suppliers, *nations, *regions);
        if quote.count > 0 && crate::flat::has_quote_support_flat(&quote.row, flat, suppliers, nations, regions) {
            match result {
                None => { result = Some(quote.row.ps_supplycost); },
                Some(cost) => { if quote.row.ps_supplycost < cost { result = Some(quote.row.ps_supplycost); } },
            }
        }
        proof {
            lemma_supported_cost_step(quotes.entries@, quote_index as int, *suppliers, *nations, *regions);
            if quote.count > 0 && supported(*quote.row, *suppliers, *nations, *regions) {
                let cost = quote.row.ps_supplycost as int;
                if previous.len() == 0 { assert(min_of(previous.push(cost)) == cost); }
                else {
                    assert(previous.push(cost).drop_last() =~= previous);
                    assert(min_of(previous.push(cost)) == if cost < min_of(previous) { cost } else { min_of(previous) });
                }
            }
        }
        quote_index += 1;
    }
    proof { assert(quotes.entries@.take(quote_index as int) =~= quotes.entries@); }
    result
}



pub fn enumerate(s: &State, size: i64, pat: &[u8], name: &[u8]) -> (result: Vec<Output>)
    ensures query_result(s.db(), params(size as int, pat@, name@), out_view(result@)),
{
    proof { use_type_invariant(s); s.database_witness(); }
    let regions = match region_slice(&s.regions, name) {
        None => {
            let empty: Vec<Output> = Vec::new();
            proof { lemma_region_absent(s.db(), s.regions, name@); lemma_no_region_result(s.db(), size as int, pat@, name@); }
            proof { assert(out_view(empty@) =~= Seq::<OutRow>::empty()); }
            return empty;
        },
        Some(slice) => slice,
    };
    proof {
        assert(s.meaning());
        assert(s.parts.meaning(s.db()));
        lemma_regions_nonnegative(*s, regions, name@);
        assert forall|index: int| 0 <= index < s.db().part.len() implies fits_i64((#[trigger] s.db().part[index]).p_size) && fits_i64(s.db().part[index].p_partkey) by {
            assert(retained(s.db().part[index]));
        }
    }
    let dict = &*s.parts.dict;
    // the pattern, once for every distinct string
    let mut ok: Vec<bool> = Vec::new();
    let mut ti = 0;
    while ti < dict.strings.len()
        invariant ti <= dict.strings.len(), ok@.len() == ti, (*s.parts.dict).valid(), s.db() == s.database@, dict == &*s.parts.dict, s.db() == s.database@, db_fits(s.db()), s.parts.meaning(s.db()), forall|region: i64| #[trigger] count_at(regions, region) == region_amount(s.regions, name@, region),
            forall|j: int| 0 <= j < ti ==> #[trigger] ok@[j] == like_match(s.parts.dtext(j), pat@),
        decreases dict.strings.len() - ti,
    {
        let matched = like_match_exec(&dict.strings[ti], pat);
        proof { assert(matched == like_match(s.parts.dtext(ti as int), pat@)); }
        let ghost before_ok = ok@;
        ok.push(matched);
        proof {
            assert forall|j: int| 0 <= j < ti + 1 implies #[trigger] ok@[j] == like_match(s.parts.dtext(j), pat@) by {
                if j < ti as int { assert(ok@[j] == before_ok[j]); }
            }
        }
        ti += 1;
    }
    let mut best: Vec<Candidate> = Vec::new();
    let mut enumeration: Ghost<Seq<OutRow>> = Ghost(Seq::empty());
    let mut ordered_enumeration: Ghost<Seq<OutRow>> = Ghost(Seq::empty());
    let ghost dl = s.parts.dl();
    proof { assert(types_outputs(*s, regions, size, pat@, 0) =~= Seq::<OutRow>::empty()); }
    let mut ti = 0;
    while ti < dict.strings.len()
        invariant ti <= dict.strings.len(), ok@.len() == dict.strings.len(), dict.strings@.len() == dl, (*s.parts.dict).valid(), s.meaning(), regions.valid(), dict == &*s.parts.dict, dl == s.parts.dl(),
            s.db() == s.database@, db_fits(s.db()), s.parts.meaning(s.db()), forall|region: i64| #[trigger] count_at(regions, region) == region_amount(s.regions, name@, region),
            best.len() <= 100, nondesc(candidate_rows(best@), ext_order()),
            nondesc(ordered_enumeration@, ext_order()), candidate_rows(best@) == limit_rows(ordered_enumeration@, 100),
            ordered_enumeration@.to_multiset() == enumeration@.to_multiset(),
            forall|j: int| 0 <= j < ok@.len() ==> #[trigger] ok@[j] == like_match(s.parts.dtext(j), pat@),
            enumeration@ == types_outputs(*s, regions, size, pat@, ti as int),
        decreases dict.strings.len() - ti,
    {
        let ghost type_start = enumeration@;
        if ok[ti] {
            let rows = s.parts.tl.get((size, ti as i64, 0));
            let ghost all_rows = tl_rows(s.parts, size, ti as int);
            proof { s.parts.tl.lemma_shaped_valid(); }
            let mut j = 0;
            while j < rows.len()
                invariant j <= rows.len(), (*rows)@ == all_rows, all_rows == tl_rows(s.parts, size, ti as int), ti < dict.strings.len(),
                    ok@.len() == dict.strings.len(), dict.strings@.len() == dl, (*s.parts.dict).valid(), s.meaning(), regions.valid(), dict == &*s.parts.dict, dl == s.parts.dl(),
                    s.db() == s.database@, db_fits(s.db()), s.parts.meaning(s.db()), forall|region: i64| #[trigger] count_at(regions, region) == region_amount(s.regions, name@, region),
                    ok@[ti as int] == like_match(s.parts.dtext(ti as int), pat@),
                    best.len() <= 100, nondesc(candidate_rows(best@), ext_order()),
                    nondesc(ordered_enumeration@, ext_order()), candidate_rows(best@) == limit_rows(ordered_enumeration@, 100),
                    ordered_enumeration@.to_multiset() == enumeration@.to_multiset(),
                    enumeration@ == type_start + cell_outputs(*s, regions, size, ti as int, all_rows.take(j as int)),
                decreases rows.len() - j,
            {
                let cell = rows[j];
                let ghost cell_start = enumeration@;
                proof { assert(cell_start == type_start + cell_outputs(*s, regions, size, ti as int, all_rows.take(j as int))); }
                proof {
                    assert(s.parts.meaning(s.db()));
                    assert(0 <= ti as int && (ti as int) < s.parts.dl());
                    assert(0 <= j as int && (j as int) < tl_rows(s.parts, size, ti as int).len());
                    lemma_tl_cell_facts(s.db(), s.parts, size, ti as int, j as int);
                    assert(cell == all_rows[j as int]);
                }
                let key = cell.first;
                let mfgr_id = cell.second as usize;
                proof { assert(mfgr_id as int == cell.second as int); assert(mfgr_id < dict.strings.len()); }
                let part_row = Arc::new(BrassPartScan {
                    p_partkey: key, p_mfgr: copy_bytes(&dict.strings[mfgr_id]), p_size: size, p_type: copy_bytes(&dict.strings[ti]),
                });
                let pe = Counted { row: part_row, count: cell.count };
                let ghost cnt = cell_cnt(s.parts, size, ti as int, cell);
                let ghost pcs = seq![cnt];
                proof {
                    assert((*pe.row).rv() == cnt.row);
                    assert(pents_positive(pcs));
                    assert(cell_output(*s, regions, size, ti as int)(cell) == key_output_p(*s, key, pcs, regions));
                }
                if let Some(quotes) = s.quotes.get(key) {
                    if let Some(cost) = minimum(&quotes, &s.sflat, &s.suppliers, &s.nations, &regions) {
                        let mut quote_index = 0;
                        while quote_index < quotes.entries.len()
                            invariant quote_index <= quotes.entries.len(), best.len() <= 100, s.meaning(), regions.valid(), nondesc(candidate_rows(best@), ext_order()),
                                nondesc(ordered_enumeration@, ext_order()), candidate_rows(best@) == limit_rows(ordered_enumeration@, 100),
                                ordered_enumeration@.to_multiset() == enumeration@.to_multiset(),
                                enumeration@ == cell_start + quote_outputs(quotes.entries@.take(quote_index as int), pcs, cost, s.suppliers, s.nations, regions),
                                pents_positive(pcs), quotes.valid(), pcs.len() == 1, pe.count == pcs[0].count, (*pe.row).rv() == pcs[0].row,
                            decreases quotes.entries.len() - quote_index,
                        {
                            let quote = &quotes.entries[quote_index];
                            let ghost quote_start = enumeration@;
                            proof { assert(quote_start == cell_start + quote_outputs(quotes.entries@.take(quote_index as int), pcs, cost, s.suppliers, s.nations, regions)); }
                            if quote.row.ps_supplycost == cost {
                                let skip = crate::flat::skip_quote(&best, &s.sflat, &s.suppliers, quote.row.ps_suppkey);
                                if skip {
                                    proof {
                                        let block = quote_output(*quote, pcs, cost, s.suppliers, s.nations, regions);
                                        let bag = s.suppliers.lookup(quote.row.ps_suppkey)->0;
                                        let a = (*(*bag).entries@[0].row).s_acctbal as int;
                                        assert forall|i: int| 0 <= i < (*bag).entries@.len() implies (*(#[trigger] (*bag).entries@[i]).row).s_acctbal as int == a by {
                                            assert(i == 0);
                                        }
                                        lemma_quote_output_acctbal(*quote, pcs, cost, s.suppliers, s.nations, regions, a);
                                        assert(candidate_rows(best@)[99].s_acctbal == (*best@[99].supplier).s_acctbal as int);
                                        assert forall|i: int| 0 <= i < block.len() implies ext_lt(limit_rows(ordered_enumeration@, 100)[99], #[trigger] block[i]) by {
                                            assert(block[i].s_acctbal == a);
                                        }
                                    }
                                    let ghost block = quote_output(*quote, pcs, cost, s.suppliers, s.nations, regions);
                                    let ghost previous_enumeration = enumeration@;
                                    let ghost previous_ordered = ordered_enumeration@;
                                    let ghost skipped = lemma_skip_block(previous_ordered, block);
                                    enumeration = Ghost(previous_enumeration + block);
                                    ordered_enumeration = Ghost(skipped);
                                    proof {
                                        verus_common::sums::lemma_to_multiset_concat(previous_enumeration, block);
                                        verus_common::sums::lemma_to_multiset_concat(previous_ordered, block);
                                        assert(enumeration@ == quote_start + block);
                                    }
                                } else if let Some(suppliers) = s.suppliers.get(quote.row.ps_suppkey) {
                                    let mut supplier_index = 0;
                                    while supplier_index < suppliers.entries.len()
                                        invariant supplier_index <= suppliers.entries.len(), best.len() <= 100, s.meaning(), regions.valid(), nondesc(candidate_rows(best@), ext_order()),
                                            nondesc(ordered_enumeration@, ext_order()), candidate_rows(best@) == limit_rows(ordered_enumeration@, 100),
                                            ordered_enumeration@.to_multiset() == enumeration@.to_multiset(),
                                            enumeration@ == quote_start + supplier_outputs(suppliers.entries@.take(supplier_index as int), pcs, *quote, s.nations, regions),
                                            pents_positive(pcs), quotes.valid(), suppliers.valid(), quote.count > 0, pcs.len() == 1, pe.count == pcs[0].count, (*pe.row).rv() == pcs[0].row,
                                        decreases suppliers.entries.len() - supplier_index,
                                    {
                                        let supplier = &suppliers.entries[supplier_index];
                                        let ghost supplier_start = enumeration@;
                                        proof { assert(supplier_start == quote_start + supplier_outputs(suppliers.entries@.take(supplier_index as int), pcs, *quote, s.nations, regions)); }
                                        if let Some(nations) = s.nations.get(supplier.row.s_nationkey) {
                                            let mut nation_index = 0;
                                            while nation_index < nations.entries.len()
                                                invariant nation_index <= nations.entries.len(), best.len() <= 100, s.meaning(), regions.valid(), nondesc(candidate_rows(best@), ext_order()),
                                                    nondesc(ordered_enumeration@, ext_order()), candidate_rows(best@) == limit_rows(ordered_enumeration@, 100),
                                                    ordered_enumeration@.to_multiset() == enumeration@.to_multiset(),
                                                    enumeration@ == supplier_start + nation_outputs(nations.entries@.take(nation_index as int), pcs, *quote, *supplier, regions),
                                                    pents_positive(pcs), quotes.valid(), nations.valid(), quote.count > 0, supplier.count > 0, pcs.len() == 1, pe.count == pcs[0].count, (*pe.row).rv() == pcs[0].row,
                                                decreases nations.entries.len() - nation_index,
                                            {
                                                let nation = &nations.entries[nation_index];
                                                let ghost nation_start = enumeration@;
                                                proof { assert(nation_start == supplier_start + nation_outputs(nations.entries@.take(nation_index as int), pcs, *quote, *supplier, regions)); }
                                                if let Some(region) = regions.get(nation.row.n_regionkey) {
                                                    if region.value > 0 {
                                                    let mut part_index: usize = 0;
                                                    while part_index < 1
                                                        invariant part_index <= 1, best.len() <= 100, s.meaning(), regions.valid(), nondesc(candidate_rows(best@), ext_order()),
                                                            nondesc(ordered_enumeration@, ext_order()), candidate_rows(best@) == limit_rows(ordered_enumeration@, 100),
                                                            ordered_enumeration@.to_multiset() == enumeration@.to_multiset(),
                                                            enumeration@ == nation_start + part_outputs(pcs.take(part_index as int), *quote, *supplier, *nation, region.value),
                                                            pents_positive(pcs), quotes.valid(), quote.count > 0, supplier.count > 0, nation.count > 0, region.value > 0, pcs.len() == 1, pe.count == pcs[0].count, (*pe.row).rv() == pcs[0].row,
                                                        decreases 1 - part_index,
                                                    {
                                                        let part = &pe;
                                                        let candidate = Candidate { part: Arc::clone(&part.row), supplier: Arc::clone(&supplier.row), nation: Arc::clone(&nation.row) };
                                                        let ghost multiplicity = part.count as int * quote.count as int * supplier.count as int * nation.count as int * region.value as int;
                                                        let ghost previous_ordered = ordered_enumeration@;
                                                        let ghost previous_enumeration = enumeration@;
                                                        proof { assert(previous_enumeration == nation_start + part_outputs(pcs.take(part_index as int), *quote, *supplier, *nation, region.value)); }
                                                        let ghost previous_best = candidate_rows(best@);
                                                        let first_copies = capped_product(part.count, quote.count);
                                                        let second_copies = capped_product(first_copies, supplier.count);
                                                        let third_copies = capped_product(second_copies, nation.count);
                                                        let copies = capped_product(third_copies, region.value);
                                                        proof {
                                                            let first = part.count as int * quote.count as int;
                                                            let second = first * supplier.count as int;
                                                            let third = second * nation.count as int;
                                                            verus_prooflib::lemma_positive_product(part.count as int, quote.count as int);
                                                            verus_prooflib::lemma_positive_product(first, supplier.count as int);
                                                            verus_prooflib::lemma_positive_product(second, nation.count as int);
                                                            verus_prooflib::lemma_positive_product(third, region.value as int);
                                                            lemma_cap_multiply(first, supplier.count as int);
                                                            lemma_cap_multiply(second, nation.count as int);
                                                            lemma_cap_multiply(third, region.value as int);
                                                            assert(copies as int == cap_multiplicity(multiplicity));
                                                        }
                                                        retain(&mut best, &candidate, copies);
                                                        proof {
                                                            broadcast use vstd::multiset::group_multiset_axioms;
                                                            lemma_retained_product(previous_ordered, candidate.view(), multiplicity as nat, previous_best, candidate_rows(best@));
                                                            lemma_repeat_append(previous_ordered, candidate.view(), multiplicity as nat);
                                                            verus_common::sums::lemma_to_multiset_concat(previous_ordered, copies_of(candidate.view(), multiplicity as nat));
                                                            verus_common::sums::lemma_to_multiset_concat(previous_enumeration, copies_of(candidate.view(), multiplicity as nat));
                                                        }
                                                        enumeration = Ghost(previous_enumeration + copies_of(candidate.view(), multiplicity as nat));
                                                        ordered_enumeration = Ghost(repeat_sorted(previous_ordered, candidate.view(), multiplicity as nat));
                                                        proof {
                                                            assert(pcs.len() == 1); assert((part_index as int) < 1); assert(0 <= part_index as int);
                                                            lemma_part_outputs_step(pcs, part_index as int, *quote, *supplier, *nation, region.value);
                                                            assert(candidate.view() == projected_output(pcs[part_index as int].row, *supplier.row, *nation.row));
                                                            assert(part_output(pcs[part_index as int], *quote, *supplier, *nation, region.value) == copies_of(candidate.view(), multiplicity as nat));
                                                            assert(enumeration@ =~= nation_start + part_outputs(pcs.take(part_index as int + 1), *quote, *supplier, *nation, region.value));
                                                        }
                                                        part_index += 1;
                                                    }
                                                    proof { assert(pcs.take(part_index as int) =~= pcs); }
                                                    }
                                                }
                                                proof {
                                                    assert(enumeration@ == nation_start + nation_output(*nation, pcs, *quote, *supplier, regions));
                                                    lemma_nation_outputs_step(nations.entries@, nation_index as int, pcs, *quote, *supplier, regions);
                                                    assert(enumeration@ =~= supplier_start + nation_outputs(nations.entries@.take(nation_index as int + 1), pcs, *quote, *supplier, regions));
                                                }
                                                nation_index += 1;
                                            }
                                            proof { assert(nations.entries@.take(nation_index as int) =~= nations.entries@); }
                                        }
                                        proof {
                                            assert(enumeration@ == supplier_start + supplier_output(*supplier, pcs, *quote, s.nations, regions));
                                            lemma_supplier_outputs_step(suppliers.entries@, supplier_index as int, pcs, *quote, s.nations, regions);
                                            assert(enumeration@ =~= quote_start + supplier_outputs(suppliers.entries@.take(supplier_index as int + 1), pcs, *quote, s.nations, regions));
                                        }
                                        supplier_index += 1;
                                    }
                                    proof { assert(suppliers.entries@.take(supplier_index as int) =~= suppliers.entries@); }
                                }
                            }
                            proof {
                                assert(enumeration@ == quote_start + quote_output(*quote, pcs, cost, s.suppliers, s.nations, regions));
                                lemma_quote_outputs_step(quotes.entries@, quote_index as int, pcs, cost, s.suppliers, s.nations, regions);
                                assert(enumeration@ =~= cell_start + quote_outputs(quotes.entries@.take(quote_index as int + 1), pcs, cost, s.suppliers, s.nations, regions));
                            }
                            quote_index += 1;
                        }
                        proof { assert(quotes.entries@.take(quote_index as int) =~= quotes.entries@); }
                    }
                }
                proof {
                    assert(enumeration@ == cell_start + key_output_p(*s, key, pcs, regions));
                    lemma_cell_outputs_step(*s, regions, size, ti as int, all_rows, j as int);
                    assert(cell_output(*s, regions, size, ti as int)(cell) == key_output_p(*s, key, pcs, regions));
                    assert(cell == all_rows[j as int]);
                    assert(cell_output(*s, regions, size, ti as int)(all_rows[j as int]) == key_output_p(*s, key, pcs, regions));
                    assert(cell_start == type_start + cell_outputs(*s, regions, size, ti as int, all_rows.take(j as int)));
                    assert((type_start + cell_outputs(*s, regions, size, ti as int, all_rows.take(j as int))) + key_output_p(*s, key, pcs, regions) =~=
                        type_start + (cell_outputs(*s, regions, size, ti as int, all_rows.take(j as int)) + key_output_p(*s, key, pcs, regions)));
                    assert(enumeration@ == type_start + cell_outputs(*s, regions, size, ti as int, all_rows.take(j as int + 1)));
                }
                j += 1;
            }
            proof {
                assert(all_rows.take(j as int) =~= all_rows);
                assert(type_outputs(*s, regions, size, pat@, ti as int) == cell_outputs(*s, regions, size, ti as int, all_rows));
            }
        } else {
            proof { assert(type_outputs(*s, regions, size, pat@, ti as int) =~= Seq::<OutRow>::empty()); }
        }
        proof {
            assert(enumeration@ == type_start + type_outputs(*s, regions, size, pat@, ti as int));
            lemma_types_outputs_step(*s, regions, size, pat@, ti as int);
        }
        ti += 1;
    }
    proof {
        assert(ti == dl);
        assert forall|first: int, second: int| 0 <= first < second < ordered_enumeration@.len() implies
            !row_lt(#[trigger] ordered_enumeration@[second], #[trigger] ordered_enumeration@[first]) by {
            lemma_row_lt_ext(ordered_enumeration@[second], ordered_enumeration@[first]);
        }
        lemma_types_outputs_bag(*s, size, name@, pat@, regions);
        assert(enumeration@.to_multiset() == rows(s.db(), params(size as int, pat@, name@)).to_multiset());
        assert(full_result(s.db(), params(size as int, pat@, name@), ordered_enumeration@));
    }
    let mut result = Vec::new();
    let mut offset = 0;
    while offset < best.len()
        invariant offset <= best.len(), result.len() == offset,
            out_view(result@) == candidate_rows(best@).take(offset as int),
            candidate_rows(best@) == limit_rows(ordered_enumeration@, 100),
            full_result(s.db(), params(size as int, pat@, name@), ordered_enumeration@),
        decreases best.len() - offset,
    {
        result.push(best[offset].hydrate());
        offset += 1;
    }
    proof { assert(out_view(result@) == limit_rows(ordered_enumeration@, 100)); }
    result
}

}
