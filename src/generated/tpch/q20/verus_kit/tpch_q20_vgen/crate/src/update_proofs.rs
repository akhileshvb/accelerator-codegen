use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::like_match;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::profiles::*;
use crate::storage::*;
use crate::profile_proofs::*;
use crate::dimensions::*;

verus! {
pub proof fn lemma_stock_frame(before: Map<i64, PartWork>, after: Map<i64, PartWork>, parts: Table<Part>)
    requires before.dom() == after.dom(),
        forall|key: i64, supplier: i64, value: int| #[trigger] work_stock_count(after, key, supplier, value) == work_stock_count(before, key, supplier, value),
    ensures forall|key: i64, supplier: i64, value: int| #[trigger] combined_stock(after, parts, key, supplier, value) == combined_stock(before, parts, key, supplier, value),
{
    assert forall|key: i64, supplier: i64, value: int| #[trigger] combined_stock(after, parts, key, supplier, value) == combined_stock(before, parts, key, supplier, value) by {
        assert(work_stock_count(after, key, supplier, value) == work_stock_count(before, key, supplier, value));
    }
}
pub proof fn lemma_day_frame(before: Map<i64, PartWork>, after: Map<i64, PartWork>, parts: Table<Part>)
    requires before.dom() == after.dom(),
        forall|key: i64, supplier: i64, date: int| #[trigger] work_day_count(after, key, supplier, date) == work_day_count(before, key, supplier, date),
        forall|key: i64, supplier: i64, date: int| #[trigger] work_day_quantity(after, key, supplier, date) == work_day_quantity(before, key, supplier, date),
    ensures forall|key: i64, supplier: i64, date: int, quantity: bool| #[trigger] combined_day(after, parts, key, supplier, date, quantity) == combined_day(before, parts, key, supplier, date, quantity),
{
    assert forall|key: i64, supplier: i64, date: int, quantity: bool| #[trigger] combined_day(after, parts, key, supplier, date, quantity) == combined_day(before, parts, key, supplier, date, quantity) by {
        assert(work_day_count(after, key, supplier, date) == work_day_count(before, key, supplier, date));
        assert(work_day_quantity(after, key, supplier, date) == work_day_quantity(before, key, supplier, date));
    }
}
pub open spec fn stock_weight(key: i64, supplier: i64, value: int) -> spec_fn(StockInput) -> int {
    |row: StockInput| if row.ps_partkey == key && row.ps_suppkey == supplier && row.ps_availqty == value { 1 } else { 0 }
}
pub proof fn lemma_dimensions_share(db: Db, suppliers: Suppliers, nations: Nations, names: PartNames, shared_suppliers: Suppliers, shared_nations: Nations, shared_names: PartNames)
    requires supplier_correct(db, suppliers), nation_correct(db, nations), names_correct(db, names),
        forall|key: i64| #[trigger] shared_suppliers.lookup(key) == suppliers.lookup(key),
        forall|key: i64| #[trigger] shared_nations.lookup(key) == nations.lookup(key),
        forall|key: i64| #[trigger] shared_names.lookup(key) == names.lookup(key),
    ensures supplier_correct(db, shared_suppliers), nation_correct(db, shared_nations), names_correct(db, shared_names),
{
    assert(names_positive(shared_names));
    assert forall|key: i64, name: Seq<u8>| #[trigger] name_count(shared_names, key, name) == sum(db.part, |row: PartRow| if row.p_partkey == key && row.p_name == name { 1 } else { 0 }) by {
        assert(shared_names.lookup(name_slot(key, name)) == names.lookup(name_slot(key, name)));
        assert(name_count(names, key, name) == sum(db.part, |row: PartRow| if row.p_partkey == key && row.p_name == name { 1 } else { 0 }));
    }
    assert forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_count(shared_suppliers, nation, supplier, name, address) ==
        sum(db.supplier, |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == name && row.s_address == address { 1 } else { 0 }) by {
        assert(shared_suppliers.lookup(nation) == suppliers.lookup(nation));
        assert(supplier_count(suppliers, nation, supplier, name, address) == sum(db.supplier, |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == name && row.s_address == address { 1 } else { 0 }));
    }
    assert forall|name: Seq<u8>, key: i64| #[trigger] nation_count(shared_nations, name, key) == sum(db.nation, |row: NationRow| if row.n_name == name && row.n_nationkey == key { 1 } else { 0 }) by {
        assert(shared_nations.lookup(name_hash(name) as i64) == nations.lookup(name_hash(name) as i64));
        assert(nation_count(nations, name, key) == sum(db.nation, |row: NationRow| if row.n_name == name && row.n_nationkey == key { 1 } else { 0 }));
    }
}
pub open spec fn supplier_delta_weight(nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>) -> spec_fn(SupplierDelta) -> int {
    |delta: SupplierDelta| if delta.row.s_nationkey == nation && delta.row.s_suppkey == supplier && delta.row.s_name@ == name && delta.row.s_address@ == address { delta.sign as int } else { 0 }
}
pub open spec fn nation_delta_weight(name: Seq<u8>, key: i64) -> spec_fn(NationDelta) -> int {
    |delta: NationDelta| if delta.row.n_name@ == name && delta.row.n_nationkey == key { delta.sign as int } else { 0 }
}
pub proof fn lemma_supplier_updated(before: Db, after: Db, original: Suppliers, updated: Suppliers, deltas: Seq<SupplierDelta>)
    requires supplier_correct(before, original), suppliers_positive(updated), bag_step(before.supplier, after.supplier, supplier_delta_seq_view(deltas)),
        forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_count(updated, nation, supplier, name, address) == supplier_count(original, nation, supplier, name, address) + sum(deltas, supplier_delta_weight(nation, supplier, name, address)),
    ensures supplier_correct(after, updated),
{
    assert forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_count(updated, nation, supplier, name, address) == sum(after.supplier,
        |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == name && row.s_address == address { 1 } else { 0 }) by {
        let weight = |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == name && row.s_address == address { 1int } else { 0int };
        let signed = |entry: (SupplierRow, int)| entry.1 * weight(entry.0);
        assert(sum(after.supplier, weight) == sum(before.supplier, weight) + sum(supplier_delta_seq_view(deltas), signed));
        lemma_sum_map(deltas, |delta: SupplierDelta| (supplier_input_view(delta.row), delta.sign as int), signed, supplier_delta_weight(nation, supplier, name, address));
        assert(supplier_count(original, nation, supplier, name, address) == sum(before.supplier, weight));
    }
}
pub proof fn lemma_nation_updated(before: Db, after: Db, original: Nations, updated: Nations, deltas: Seq<NationDelta>)
    requires nation_correct(before, original), nations_positive(updated), bag_step(before.nation, after.nation, nation_delta_seq_view(deltas)),
        forall|name: Seq<u8>, key: i64| #[trigger] nation_count(updated, name, key) == nation_count(original, name, key) + sum(deltas, nation_delta_weight(name, key)),
    ensures nation_correct(after, updated),
{
    assert forall|name: Seq<u8>, key: i64| #[trigger] nation_count(updated, name, key) == sum(after.nation, |row: NationRow| if row.n_name == name && row.n_nationkey == key { 1 } else { 0 }) by {
        let weight = |row: NationRow| if row.n_name == name && row.n_nationkey == key { 1int } else { 0int };
        let signed = |entry: (NationRow, int)| entry.1 * weight(entry.0);
        assert(sum(after.nation, weight) == sum(before.nation, weight) + sum(nation_delta_seq_view(deltas), signed));
        lemma_sum_map(deltas, |delta: NationDelta| (nation_input_view(delta.row), delta.sign as int), signed, nation_delta_weight(name, key));
        assert(nation_count(original, name, key) == sum(before.nation, weight));
    }
}
pub open spec fn name_delta_weight(key: i64, name: Seq<u8>) -> spec_fn(PartDelta) -> int {
    |delta: PartDelta| if delta.row.p_partkey == key && delta.row.p_name@ == name { delta.sign as int } else { 0 }
}
pub proof fn lemma_names_updated(before: Db, after: Db, original: PartNames, updated: PartNames, deltas: Seq<PartDelta>)
    requires names_correct(before, original), names_positive(updated), bag_step(before.part, after.part, part_delta_seq_view(deltas)),
        forall|key: i64, name: Seq<u8>| #[trigger] name_count(updated, key, name) == name_count(original, key, name) + sum(deltas, name_delta_weight(key, name)),
    ensures names_correct(after, updated),
{
    assert forall|key: i64, name: Seq<u8>| #[trigger] name_count(updated, key, name) == sum(after.part, |row: PartRow| if row.p_partkey == key && row.p_name == name { 1 } else { 0 }) by {
        let weight = |row: PartRow| if row.p_partkey == key && row.p_name == name { 1int } else { 0int };
        let signed = |entry: (PartRow, int)| entry.1 * weight(entry.0);
        assert(sum(after.part, weight) == sum(before.part, weight) + sum(part_delta_seq_view(deltas), signed));
        lemma_sum_map(deltas, |delta: PartDelta| (part_input_view(delta.row), delta.sign as int), signed, name_delta_weight(key, name));
        assert(name_count(original, key, name) == sum(before.part, weight));
    }
}
pub open spec fn ship_weight(key: i64, supplier: i64, date: int, quantity: bool) -> spec_fn(ShipmentInput) -> int {
    |row: ShipmentInput| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { if quantity { row.l_quantity as int } else { 1 } } else { 0 }
}
pub open spec fn stock_delta_weight(key: i64, supplier: i64, value: int) -> spec_fn(PartsuppDelta) -> int {
    |delta: PartsuppDelta| delta.sign as int * stock_weight(key, supplier, value)(delta.row)
}
pub open spec fn ship_delta_weight(key: i64, supplier: i64, date: int, quantity: bool) -> spec_fn(LineitemDelta) -> int {
    |delta: LineitemDelta| delta.sign as int * ship_weight(key, supplier, date, quantity)(delta.row)
}
pub struct FactUpdate {
    pub before: Db,
    pub after: Db,
    pub parts: Table<Part>,
    pub counts: Map<i64, i128>,
    pub activated: Map<i64, bool>,
    pub stocks: Seq<StockInput>,
    pub ships: Seq<ShipmentInput>,
    pub stock_keys: Seq<i64>,
    pub ship_keys: Seq<i64>,
    pub stock_delta: Seq<PartsuppDelta>,
    pub ship_delta: Seq<LineitemDelta>,
    pub supplier_delta: Seq<SupplierDelta>,
    pub nation_delta: Seq<NationDelta>,
}
impl FactUpdate {
    pub open spec fn valid(self) -> bool {
        &&& parts_shape(self.parts)
        &&& forall|key: i64| #![trigger self.parts.lookup(key)] part_correct(self.before, key, self.parts.lookup(key))
        &&& forall|key: i64| #![trigger coalesced_part_count(self.parts, self.counts, key)] coalesced_part_count(self.parts, self.counts, key) == eligible(self.after, key as int)
        &&& forall|key: i64| #[trigger] self.activated.contains_key(key) == (self.parts.lookup(key).is_none() && eligible(self.after, key as int) > 0)
        &&& forall|key: i64| #![trigger self.activated.contains_key(key)] self.activated.contains_key(key) ==> self.stock_keys.contains(key) && self.ship_keys.contains(key)
        &&& rescan_stocks_for_parts_ok(self.after, self.stock_keys, self.stocks)
        &&& rescan_shipments_for_parts_ok(self.after, self.ship_keys, self.ships)
        &&& bag_step(self.before.partsupp, self.after.partsupp, partsupp_delta_seq_view(self.stock_delta))
        &&& bag_step(self.before.lineitem, self.after.lineitem, lineitem_delta_seq_view(self.ship_delta))
        &&& bag_step(self.before.supplier, self.after.supplier, supplier_delta_seq_view(self.supplier_delta))
        &&& bag_step(self.before.nation, self.after.nation, nation_delta_seq_view(self.nation_delta))
    }
    pub open spec fn stock(self, key: i64, supplier: i64, value: int, scan: int, delta: int) -> int {
        if self.activated.contains_key(key) { sum(self.stocks.take(scan), stock_weight(key, supplier, value)) }
        else { stock_count(self.before, key as int, supplier as int, value) + sum(self.stock_delta.take(delta), stock_delta_weight(key, supplier, value)) }
    }
    pub open spec fn day(self, key: i64, supplier: i64, date: int, quantity: bool, scan: int, delta: int) -> int {
        if self.activated.contains_key(key) { sum(self.ships.take(scan), ship_weight(key, supplier, date, quantity)) }
        else { (if quantity { daily_quantity(self.before, key as int, supplier as int, date) } else { daily_count(self.before, key as int, supplier as int, date) })
            + sum(self.ship_delta.take(delta), ship_delta_weight(key, supplier, date, quantity)) }
    }
    pub open spec fn progress(self, work: Map<i64, PartWork>, scans: int, ships: int, stocks: int, shipments: int) -> bool {
        &&& forall|key: i64, supplier: i64, value: int| #![trigger combined_stock(work, self.parts, key, supplier, value)]
            eligible(self.after, key as int) > 0 ==> combined_stock(work, self.parts, key, supplier, value) == self.stock(key, supplier, value, scans, stocks)
        &&& forall|key: i64, supplier: i64, date: int, quantity: bool| #![trigger combined_day(work, self.parts, key, supplier, date, quantity)]
            eligible(self.after, key as int) > 0 ==> combined_day(work, self.parts, key, supplier, date, quantity) == self.day(key, supplier, date, quantity, ships, shipments)
    }
    pub open spec fn coverage(self, work: Map<i64, PartWork>, stocks: int, shipments: int) -> bool {
        &&& forall|key: i64| #![trigger work.contains_key(key)] eligible(self.after, key as int) > 0 && (self.counts.contains_key(key) || self.activated.contains_key(key)) ==> work.contains_key(key)
        &&& forall|index: int| #![trigger self.stock_delta[index]] 0 <= index < stocks && eligible(self.after, self.stock_delta[index].row.ps_partkey as int) > 0 ==> work.contains_key(self.stock_delta[index].row.ps_partkey)
        &&& forall|index: int| #![trigger self.ship_delta[index]] 0 <= index < shipments && eligible(self.after, self.ship_delta[index].row.l_partkey as int) > 0 ==> work.contains_key(self.ship_delta[index].row.l_partkey)
    }
    pub proof fn stock_step(self, before: Map<i64, PartWork>, after: Map<i64, PartWork>, scans: int, ships: int, stocks: int, shipments: int, rescan: bool)
        requires self.progress(before, scans, ships, stocks, shipments),
            0 <= scans <= self.stocks.len(), 0 <= stocks <= self.stock_delta.len(),
            rescan ==> scans < self.stocks.len(), !rescan ==> stocks < self.stock_delta.len(),
            forall|key: i64, supplier: i64, value: int| #![trigger combined_stock(after, self.parts, key, supplier, value)] eligible(self.after, key as int) > 0 ==>
                combined_stock(after, self.parts, key, supplier, value) == combined_stock(before, self.parts, key, supplier, value)
                    + if self.activated.contains_key(key) == rescan { if rescan { stock_weight(key, supplier, value)(self.stocks[scans]) } else { stock_delta_weight(key, supplier, value)(self.stock_delta[stocks]) } } else { 0 },
            forall|key: i64, supplier: i64, date: int, quantity: bool| #[trigger] combined_day(after, self.parts, key, supplier, date, quantity) == combined_day(before, self.parts, key, supplier, date, quantity),
        ensures self.progress(after, scans + if rescan { 1int } else { 0int }, ships, stocks + if rescan { 0int } else { 1int }, shipments),
    {
        assert forall|key: i64, supplier: i64, value: int| eligible(self.after, key as int) > 0 implies
            #[trigger] combined_stock(after, self.parts, key, supplier, value) == self.stock(key, supplier, value, scans + if rescan { 1int } else { 0int }, stocks + if rescan { 0int } else { 1int }) by {
            if rescan { lemma_sum_take(self.stocks, scans, stock_weight(key, supplier, value)); }
            else { lemma_sum_take(self.stock_delta, stocks, stock_delta_weight(key, supplier, value)); }
            assert(combined_stock(before, self.parts, key, supplier, value) == self.stock(key, supplier, value, scans, stocks));
        }
    }
    pub proof fn ship_step(self, before: Map<i64, PartWork>, after: Map<i64, PartWork>, scans: int, ships: int, stocks: int, shipments: int, rescan: bool)
        requires self.progress(before, scans, ships, stocks, shipments),
            0 <= ships <= self.ships.len(), 0 <= shipments <= self.ship_delta.len(),
            rescan ==> ships < self.ships.len(), !rescan ==> shipments < self.ship_delta.len(),
            forall|key: i64, supplier: i64, date: int, quantity: bool| #![trigger combined_day(after, self.parts, key, supplier, date, quantity)] eligible(self.after, key as int) > 0 ==>
                combined_day(after, self.parts, key, supplier, date, quantity) == combined_day(before, self.parts, key, supplier, date, quantity)
                    + if self.activated.contains_key(key) == rescan { if rescan { ship_weight(key, supplier, date, quantity)(self.ships[ships]) } else { ship_delta_weight(key, supplier, date, quantity)(self.ship_delta[shipments]) } } else { 0 },
            forall|key: i64, supplier: i64, value: int| #[trigger] combined_stock(after, self.parts, key, supplier, value) == combined_stock(before, self.parts, key, supplier, value),
        ensures self.progress(after, scans, ships + if rescan { 1int } else { 0int }, stocks, shipments + if rescan { 0int } else { 1int }),
    {
        assert forall|key: i64, supplier: i64, date: int, quantity: bool| eligible(self.after, key as int) > 0 implies
            #[trigger] combined_day(after, self.parts, key, supplier, date, quantity) == self.day(key, supplier, date, quantity, ships + if rescan { 1int } else { 0int }, shipments + if rescan { 0int } else { 1int }) by {
            if rescan { lemma_sum_take(self.ships, ships, ship_weight(key, supplier, date, quantity)); }
            else { lemma_sum_take(self.ship_delta, shipments, ship_delta_weight(key, supplier, date, quantity)); }
            assert(combined_day(before, self.parts, key, supplier, date, quantity) == self.day(key, supplier, date, quantity, ships, shipments));
        }
    }
    pub proof fn final_stock(self, key: i64, supplier: i64, value: int)
        requires self.valid(), eligible(self.after, key as int) > 0,
        ensures self.stock(key, supplier, value, self.stocks.len() as int, self.stock_delta.len() as int) == stock_count(self.after, key as int, supplier as int, value),
    {
        let weight = |row: PartsuppRow| if row.ps_partkey == key && row.ps_suppkey == supplier && row.ps_availqty == value { 1int } else { 0int };
        assert(self.stocks.take(self.stocks.len() as int) =~= self.stocks);
        assert(self.stock_delta.take(self.stock_delta.len() as int) =~= self.stock_delta);
        if self.activated.contains_key(key) {
            let position = choose|position: int| #![trigger self.stock_keys[position]] 0 <= position < self.stock_keys.len() && self.stock_keys[position] == key;
            assert(seq_view_i64(self.stock_keys)[position] == key as int);
            let filtered = |row: PartsuppRow| if seq_view_i64(self.stock_keys).contains(row.ps_partkey) { weight(row) } else { 0int };
            lemma_sum_ext_all(self.after.partsupp, filtered, weight);
            assert(sum(stock_input_seq_view(self.stocks), weight) == sum(self.after.partsupp, filtered));
            lemma_sum_map(self.stocks, |row: StockInput| stock_input_view(row), weight, stock_weight(key, supplier, value));
        } else {
            let signed = |entry: (PartsuppRow, int)| entry.1 * weight(entry.0);
            assert(sum(self.after.partsupp, weight) == sum(self.before.partsupp, weight) + sum(partsupp_delta_seq_view(self.stock_delta), signed));
            lemma_sum_map(self.stock_delta, |entry: PartsuppDelta| (stock_input_view(entry.row), entry.sign as int), signed, stock_delta_weight(key, supplier, value));
        }
    }
    pub proof fn final_day(self, key: i64, supplier: i64, date: int, quantity: bool)
        requires self.valid(), eligible(self.after, key as int) > 0,
        ensures self.day(key, supplier, date, quantity, self.ships.len() as int, self.ship_delta.len() as int) ==
            if quantity { daily_quantity(self.after, key as int, supplier as int, date) } else { daily_count(self.after, key as int, supplier as int, date) },
    {
        let weight = |row: LineitemRow| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { if quantity { row.l_quantity } else { 1int } } else { 0int };
        assert(self.ships.take(self.ships.len() as int) =~= self.ships);
        assert(self.ship_delta.take(self.ship_delta.len() as int) =~= self.ship_delta);
        if quantity {
            lemma_sum_ext_all(self.after.lineitem, weight, |row: LineitemRow| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity } else { 0int });
            lemma_sum_ext_all(self.before.lineitem, weight, |row: LineitemRow| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity } else { 0int });
        } else {
            lemma_sum_ext_all(self.after.lineitem, weight, |row: LineitemRow| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { 1int } else { 0int });
            lemma_sum_ext_all(self.before.lineitem, weight, |row: LineitemRow| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { 1int } else { 0int });
        }
        if self.activated.contains_key(key) {
            let position = choose|position: int| #![trigger self.ship_keys[position]] 0 <= position < self.ship_keys.len() && self.ship_keys[position] == key;
            assert(seq_view_i64(self.ship_keys)[position] == key as int);
            let filtered = |row: LineitemRow| if seq_view_i64(self.ship_keys).contains(row.l_partkey) { weight(row) } else { 0int };
            lemma_sum_ext_all(self.after.lineitem, filtered, weight);
            assert(sum(shipment_input_seq_view(self.ships), weight) == sum(self.after.lineitem, filtered));
            lemma_sum_map(self.ships, |row: ShipmentInput| shipment_input_view(row), weight, ship_weight(key, supplier, date, quantity));
        } else {
            let signed = |entry: (LineitemRow, int)| entry.1 * weight(entry.0);
            assert(sum(self.after.lineitem, weight) == sum(self.before.lineitem, weight) + sum(lineitem_delta_seq_view(self.ship_delta), signed));
            lemma_sum_map(self.ship_delta, |entry: LineitemDelta| (shipment_input_view(entry.row), entry.sign as int), signed, ship_delta_weight(key, supplier, date, quantity));
        }
    }
    pub proof fn untouched(self, work: Map<i64, PartWork>, published: Table<Part>, key: i64)
        requires self.valid(), !work.contains_key(key), self.coverage(work, 0, 0),
            self.progress(work, self.stocks.len() as int, self.ships.len() as int, self.stock_delta.len() as int, self.ship_delta.len() as int),
            published.lookup(key) == if eligible(self.after, key as int) == 0 { None } else { self.parts.lookup(key) },
        ensures part_correct(self.after, key, published.lookup(key)),
    {
        crate::semantic_proofs::lemma_eligible_presence(self.after, key as int);
        if eligible(self.after, key as int) > 0 {
            assert(!self.counts.contains_key(key));
            assert(!self.activated.contains_key(key));
            assert(coalesced_part_count(self.parts, self.counts, key) == eligible(self.after, key as int));
            assert(part_correct(self.before, key, self.parts.lookup(key)));
            let part = self.parts.lookup(key).unwrap();
            assert forall|supplier: i64, value: int| #[trigger] part_stock(part.pairs@, supplier, value) == stock_count(self.after, key as int, supplier as int, value) by {
                self.final_stock(key, supplier, value);
                assert(combined_stock(work, self.parts, key, supplier, value) == self.stock(key, supplier, value, self.stocks.len() as int, self.stock_delta.len() as int));
            }
            assert forall|supplier: i64, date: int| #[trigger] part_day(part.pairs@, supplier, date, false) == daily_count(self.after, key as int, supplier as int, date)
                && part_day(part.pairs@, supplier, date, true) == daily_quantity(self.after, key as int, supplier as int, date) by {
                self.final_day(key, supplier, date, false); self.final_day(key, supplier, date, true);
                assert(combined_day(work, self.parts, key, supplier, date, false) == self.day(key, supplier, date, false, self.ships.len() as int, self.ship_delta.len() as int));
                assert(combined_day(work, self.parts, key, supplier, date, true) == self.day(key, supplier, date, true, self.ships.len() as int, self.ship_delta.len() as int));
            }
        }
    }
    pub proof fn finished_part(self, work: Map<i64, PartWork>, key: i64, source: PartWork, part: &Part)
        requires self.valid(), work.contains_key(key), source == work[key], source.shape(), source.count > 0,
            source.count == coalesced_part_count(self.parts, self.counts, key),
            self.progress(work, self.stocks.len() as int, self.ships.len() as int, self.stock_delta.len() as int, self.ship_delta.len() as int),
            part_shape(*part), part.count == source.count,
            forall|supplier: i64, value: int| #[trigger] part_stock(part.pairs@, supplier, value) == source.stock_for(supplier, value),
            forall|supplier: i64, date: int, quantity: bool| #[trigger] part_day(part.pairs@, supplier, date, quantity) == source.day_for(supplier, date, quantity),
        ensures part_correct(self.after, key, Some(*part)),
    {
        assert(coalesced_part_count(self.parts, self.counts, key) == eligible(self.after, key as int));
        assert forall|supplier: i64, value: int| #[trigger] part_stock(part.pairs@, supplier, value) == stock_count(self.after, key as int, supplier as int, value) by {
            self.final_stock(key, supplier, value);
            assert(combined_stock(work, self.parts, key, supplier, value) == self.stock(key, supplier, value, self.stocks.len() as int, self.stock_delta.len() as int));
        }
        assert forall|supplier: i64, date: int| #[trigger] part_day(part.pairs@, supplier, date, false) == daily_count(self.after, key as int, supplier as int, date)
            && part_day(part.pairs@, supplier, date, true) == daily_quantity(self.after, key as int, supplier as int, date) by {
            self.final_day(key, supplier, date, false); self.final_day(key, supplier, date, true);
            assert(combined_day(work, self.parts, key, supplier, date, false) == self.day(key, supplier, date, false, self.ships.len() as int, self.ship_delta.len() as int));
            assert(combined_day(work, self.parts, key, supplier, date, true) == self.day(key, supplier, date, true, self.ships.len() as int, self.ship_delta.len() as int));
        }
    }
}
pub proof fn lemma_apply_domain(before: Db, after: Db, parts: Seq<PartDelta>, stocks: Seq<PartsuppDelta>, shipments: Seq<LineitemDelta>, suppliers: Seq<SupplierDelta>, nations: Seq<NationDelta>, stock_keys: Seq<i64>, stock_rows: Seq<StockInput>, ship_keys: Seq<i64>, ship_rows: Seq<ShipmentInput>)
    requires domain_valid(before),
        capture_ok(before, after, lineitem_delta_seq_view(shipments), nation_delta_seq_view(nations), part_delta_seq_view(parts), partsupp_delta_seq_view(stocks), supplier_delta_seq_view(suppliers)),
        rescan_stocks_for_parts_ok(after, stock_keys, stock_rows), rescan_shipments_for_parts_ok(after, ship_keys, ship_rows),
        forall|key: i64| #![trigger eligible(after, key as int)] eligible(after, key as int) > 0 && eligible(before, key as int) == 0 ==> stock_keys.contains(key) && ship_keys.contains(key),
    ensures domain_valid(after),
{
    let part_property = |row: PartRow| machine_key(row.p_partkey);
    let full = part_delta_seq_view(parts);
    assert forall|index: int| #![trigger full[index]] 0 <= index < full.len() implies part_property(full[index].0) by {
        assert(parts[index].row.p_partkey as int == full[index].0.p_partkey);
    }
    lemma_bag_step_preserves_predicate(before.part, after.part, full, part_property);
    lemma_bag_step_preserves_predicate(before.supplier, after.supplier, supplier_delta_seq_view(suppliers), |row: SupplierRow| machine_key(row.s_suppkey) && machine_key(row.s_nationkey));
    lemma_bag_step_preserves_predicate(before.nation, after.nation, nation_delta_seq_view(nations), |row: NationRow| machine_key(row.n_nationkey));
    let keep_stock = |row: PartsuppRow| seq_view_i64(stock_keys).contains(row.ps_partkey);
    let keep_ship = |row: LineitemRow| seq_view_i64(ship_keys).contains(row.l_partkey);
    let project_stock = |row: PartsuppRow| row;
    let project_ship = |row: LineitemRow| row;
    assert forall|weight: spec_fn(PartsuppRow) -> int| #[trigger] sum(stock_input_seq_view(stock_rows), weight) == sum(after.partsupp, |row: PartsuppRow| if keep_stock(row) { weight(project_stock(row)) } else { 0 }) by {
        lemma_sum_ext_all(after.partsupp, |row: PartsuppRow| if keep_stock(row) { weight(project_stock(row)) } else { 0 },
            |row: PartsuppRow| if seq_view_i64(stock_keys).contains(row.ps_partkey) { weight(row) } else { 0 });
    }
    assert forall|weight: spec_fn(LineitemRow) -> int| #[trigger] sum(shipment_input_seq_view(ship_rows), weight) == sum(after.lineitem, |row: LineitemRow| if keep_ship(row) { weight(project_ship(row)) } else { 0 }) by {
        lemma_sum_ext_all(after.lineitem, |row: LineitemRow| if keep_ship(row) { weight(project_ship(row)) } else { 0 },
            |row: LineitemRow| if seq_view_i64(ship_keys).contains(row.l_partkey) { weight(row) } else { 0 });
    }
    lemma_projected_scan_property(stock_input_seq_view(stock_rows), after.partsupp, keep_stock, project_stock, |row: PartsuppRow| machine_key(row.ps_partkey) && machine_key(row.ps_suppkey));
    lemma_projected_scan_property(shipment_input_seq_view(ship_rows), after.lineitem, keep_ship, project_ship, |row: LineitemRow| machine_key(row.l_partkey) && machine_key(row.l_suppkey));
    assert forall|index: int| #![trigger after.partsupp[index]] 0 <= index < after.partsupp.len() && eligible(after, after.partsupp[index].ps_partkey) > 0 implies
        machine_key(after.partsupp[index].ps_partkey) && machine_key(after.partsupp[index].ps_suppkey) by {
        let key = after.partsupp[index].ps_partkey;
        crate::semantic_proofs::lemma_eligible_presence(after, key);
        let witness = choose|witness: int| #![trigger after.part[witness]] 0 <= witness < after.part.len() && crate::semantic_proofs::qualifying_weight(key)(after.part[witness]) == 1;
        assert(machine_key(key));
        crate::semantic_proofs::lemma_eligible_presence(before, key);
        if eligible(before, key) > 0 {
            let property = |row: PartsuppRow| row.ps_partkey != key || machine_key(row.ps_partkey) && machine_key(row.ps_suppkey);
            assert forall|position: int| #![trigger before.partsupp[position]] 0 <= position < before.partsupp.len() implies property(before.partsupp[position]) by {
                if before.partsupp[position].ps_partkey == key { assert(eligible(before, before.partsupp[position].ps_partkey) > 0); }
            }
            lemma_bag_step_preserves_predicate(before.partsupp, after.partsupp, partsupp_delta_seq_view(stocks), property);
        } else {
            let bounded = key as i64;
            assert(bounded as int == key);
            assert(stock_keys.contains(bounded));
            let position = choose|position: int| #![trigger stock_keys[position]] 0 <= position < stock_keys.len() && stock_keys[position] == bounded;
            assert(seq_view_i64(stock_keys)[position] == key);
            assert(keep_stock(after.partsupp[index]));
        }
    }
    assert forall|index: int| #![trigger after.lineitem[index]] 0 <= index < after.lineitem.len() && eligible(after, after.lineitem[index].l_partkey) > 0 implies
        machine_key(after.lineitem[index].l_partkey) && machine_key(after.lineitem[index].l_suppkey) by {
        let key = after.lineitem[index].l_partkey;
        crate::semantic_proofs::lemma_eligible_presence(after, key);
        let witness = choose|witness: int| #![trigger after.part[witness]] 0 <= witness < after.part.len() && crate::semantic_proofs::qualifying_weight(key)(after.part[witness]) == 1;
        assert(machine_key(key));
        crate::semantic_proofs::lemma_eligible_presence(before, key);
        if eligible(before, key) > 0 {
            let property = |row: LineitemRow| row.l_partkey != key || machine_key(row.l_partkey) && machine_key(row.l_suppkey);
            assert forall|position: int| #![trigger before.lineitem[position]] 0 <= position < before.lineitem.len() implies property(before.lineitem[position]) by {
                if before.lineitem[position].l_partkey == key { assert(eligible(before, before.lineitem[position].l_partkey) > 0); }
            }
            lemma_bag_step_preserves_predicate(before.lineitem, after.lineitem, lineitem_delta_seq_view(shipments), property);
        } else {
            let bounded = key as i64;
            assert(bounded as int == key);
            assert(ship_keys.contains(bounded));
            let position = choose|position: int| #![trigger ship_keys[position]] 0 <= position < ship_keys.len() && ship_keys[position] == bounded;
            assert(seq_view_i64(ship_keys)[position] == key);
            assert(keep_ship(after.lineitem[index]));
        }
    }
}
}
