use vstd::prelude::*;
use std::sync::Arc;
use crate::gen_obligations::*;
use crate::bags::*;
use crate::model::*;
use crate::storage::*;
use crate::output::*;
use crate::maintenance::*;

verus! {

/// Dense summary of one supplier key: 0 = no supplier row, 1 = exactly one row (nation, balance), 2 = other.
#[derive(Clone, Copy)]
pub struct SInfo {
    pub kind: u8,
    pub nation: i64,
    pub acctbal: i64,
}

pub open spec fn flat_limit() -> int { 4194304 }

pub open spec fn info_ok(info: SInfo, entry: Option<Arc<Bag<SupplierScan>>>) -> bool {
    match entry {
        None => info.kind == 0,
        Some(bag) => if (*bag).entries@.len() == 1 {
            info.kind == 1 && info.nation == (*(*bag).entries@[0].row).s_nationkey && info.acctbal == (*(*bag).entries@[0].row).s_acctbal
        } else { info.kind == 2 },
    }
}

pub open spec fn flat_ok(flat: Seq<SInfo>, suppliers: Directory<Bag<SupplierScan>>) -> bool {
    &&& flat.len() <= flat_limit()
    &&& forall|k: int| 0 <= k < flat.len() ==> info_ok(#[trigger] flat[k], suppliers.lookup(k as i64))
    &&& forall|k: i64| 0 <= k < flat_limit() && (#[trigger] suppliers.lookup(k)) is Some ==> k < flat.len()
}

pub fn info_of(bag: &Arc<Bag<SupplierScan>>) -> (result: SInfo)
    ensures info_ok(result, Some(*bag)),
{
    if bag.entries.len() == 1 {
        let entry = &bag.entries[0];
        SInfo { kind: 1, nation: entry.row.s_nationkey, acctbal: entry.row.s_acctbal }
    } else { SInfo { kind: 2, nation: 0, acctbal: 0 } }
}

/// The summary of every supplier key of the directory.
#[verifier::rlimit(40)]
pub fn build_flat(suppliers: &Directory<Bag<SupplierScan>>) -> (result: Vec<SInfo>)
    requires suppliers.valid(),
    ensures flat_ok(result@, *suppliers),
{
    let entries = suppliers.entries();
    let mut size: usize = 0;
    let mut index = 0;
    while index < entries.len()
        invariant index <= entries.len(), size <= 4194304,
            forall|offset: int| 0 <= offset < index ==> 0 <= (#[trigger] entries@[offset]).0 < 4194304 ==> entries@[offset].0 < size,
        decreases entries.len() - index,
    {
        let key = entries[index].0;
        if 0 <= key && key < 4194304 && (key as usize) + 1 > size { size = key as usize + 1; }
        index += 1;
    }
    let mut result: Vec<SInfo> = Vec::with_capacity(size);
    let mut position = 0;
    while position < size
        invariant position <= size, result@.len() == position,
            forall|k: int| 0 <= k < position ==> (#[trigger] result@[k]).kind == 0,
        decreases size - position,
    {
        result.push(SInfo { kind: 0, nation: 0, acctbal: 0 });
        position += 1;
    }
    let mut index = 0;
    while index < entries.len()
        invariant index <= entries.len(), size <= 4194304, result@.len() == size, suppliers.valid(),
            forall|offset: int| 0 <= offset < entries@.len() ==> suppliers.lookup((#[trigger] entries@[offset]).0) == Some(entries@[offset].1),
            forall|offset: int| 0 <= offset < entries@.len() ==> 0 <= (#[trigger] entries@[offset]).0 < 4194304 ==> entries@[offset].0 < size,
            forall|k: int| 0 <= k < size ==> info_ok(#[trigger] result@[k], suppliers.lookup(k as i64)) ||
                (!entry_keys(entries@.take(index as int)).contains(k as i64) && result@[k].kind == 0),
        decreases entries.len() - index,
    {
        let key = entries[index].0;
        if 0 <= key && key < 4194304 {
            let info = info_of(&entries[index].1);
            proof { assert(entries@[index as int].0 < size); }
            result.set(key as usize, info);
        }
        proof {
            assert(entry_keys(entries@.take(index as int + 1)) =~= entry_keys(entries@.take(index as int)).push(key));
            broadcast use vstd::seq_lib::group_seq_properties;
        }
        index += 1;
    }
    proof {
        assert(entries@.take(index as int) =~= entries@);
        assert forall|k: int| 0 <= k < size implies info_ok(#[trigger] result@[k], suppliers.lookup(k as i64)) by {
            if !entry_keys(entries@).contains(k as i64) { assert(suppliers.lookup(k as i64) is None); }
        }
        assert forall|k: i64| 0 <= k < 4194304 && (#[trigger] suppliers.lookup(k)) is Some implies k < size by {
            assert(entry_keys(entries@).contains(k));
            let offset = choose|offset: int| 0 <= offset < entries@.len() && entries@[offset].0 == k;
        }
    }
    result
}


/// The summary after the suppliers of `changes` were adjusted (all other keys are as they were).
#[verifier::rlimit(60)]
pub fn patch_flat(old: &Vec<SInfo>, before: &Directory<Bag<SupplierScan>>, after: &Directory<Bag<SupplierScan>>,
    changes: &Directory<Bag<SupplierScan>>) -> (result: Vec<SInfo>)
    requires after.valid(), changes.valid(), flat_ok(old@, *before),
        forall|k: i64| (#[trigger] changes.lookup(k)) is None ==> after.lookup(k) == before.lookup(k),
    ensures flat_ok(result@, *after),
{
    let keys = changes.entries();
    let mut result: Vec<SInfo> = Vec::with_capacity(old.len());
    let mut copied = 0;
    while copied < old.len()
        invariant copied <= old.len(), result@ == old@.take(copied as int),
        decreases old.len() - copied,
    {
        result.push(old[copied]);
        proof { assert(old@.take(copied as int + 1) =~= old@.take(copied as int).push(old@[copied as int])); }
        copied += 1;
    }
    proof { assert(old@.take(copied as int) =~= old@); }
    let ghost old_len = old@.len();
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(), after.valid(), changes.valid(), flat_ok(old@, *before), result@.len() <= 4194304, result@.len() >= old_len,
            old_len == old@.len(),
            forall|offset: int| 0 <= offset < keys@.len() ==> changes.lookup((#[trigger] keys@[offset]).0) == Some(keys@[offset].1),
            forall|k: i64| (#[trigger] changes.lookup(k)) is Some ==> entry_keys(keys@).contains(k),
            forall|k: i64| (#[trigger] changes.lookup(k)) is None ==> after.lookup(k) == before.lookup(k),
            forall|j: int| 0 <= j < result@.len() ==> info_ok(#[trigger] result@[j],
                if entry_keys(keys@.take(index as int)).contains(j as i64) { after.lookup(j as i64) } else { before.lookup(j as i64) }),
            forall|k: i64| 0 <= k < flat_limit() && entry_keys(keys@.take(index as int)).contains(k) && after.lookup(k) is Some ==> k < result@.len(),
            forall|k: i64| 0 <= k < flat_limit() && before.lookup(k) is Some ==> k < old_len,
        decreases keys.len() - index,
    {
        let key = keys[index].0;
        proof {
            assert(entry_keys(keys@.take(index as int + 1)) =~= entry_keys(keys@.take(index as int)).push(key));
            broadcast use vstd::seq_lib::group_seq_properties;
        }
        if 0 <= key && key < 4194304 {
            let ghost before_result = result@;
            while result.len() <= key as usize
                invariant result@.len() <= 4194304, key < 4194304, 0 <= key,
                    before_result.len() <= result@.len(),
                    forall|j: int| 0 <= j < before_result.len() ==> result@[j] == before_result[j],
                    forall|j: int| before_result.len() <= j < result@.len() ==> (#[trigger] result@[j]).kind == 0,
                decreases 4194304 - result.len(),
            {
                result.push(SInfo { kind: 0, nation: 0, acctbal: 0 });
            }
            let info = match after.get(key) { None => SInfo { kind: 0, nation: 0, acctbal: 0 }, Some(bag) => info_of(&bag) };
            let ghost extended = result@;
            result.set(key as usize, info);
            proof {
                assert forall|j: int| 0 <= j < result@.len() implies info_ok(#[trigger] result@[j],
                    if entry_keys(keys@.take(index as int + 1)).contains(j as i64) { after.lookup(j as i64) } else { before.lookup(j as i64) }) by {
                    if j == key as int { }
                    else {
                        assert(result@[j] == extended[j]);
                        if j < before_result.len() { assert(extended[j] == before_result[j]); }
                        else {
                            assert(extended[j].kind == 0);
                            assert(before.lookup(j as i64) is None) by {
                                if before.lookup(j as i64) is Some { assert(j < old_len); }
                            }
                        }
                    }
                }
            }
        } else {
            proof {
                assert forall|j: int| 0 <= j < result@.len() implies info_ok(#[trigger] result@[j],
                    if entry_keys(keys@.take(index as int + 1)).contains(j as i64) { after.lookup(j as i64) } else { before.lookup(j as i64) }) by {
                    assert(j as i64 != key);
                }
            }
        }
        index += 1;
    }
    proof {
        assert(keys@.take(index as int) =~= keys@);
        assert forall|j: int| 0 <= j < result@.len() implies info_ok(#[trigger] result@[j], after.lookup(j as i64)) by {
            if !entry_keys(keys@).contains(j as i64) { assert(changes.lookup(j as i64) is None); }
        }
        assert forall|k: i64| 0 <= k < flat_limit() && (#[trigger] after.lookup(k)) is Some implies k < result@.len() by {
            if entry_keys(keys@).contains(k) { } else {
                assert(changes.lookup(k) is None);
                assert(before.lookup(k) is Some);
            }
        }
    }
    result
}


/// `supported` for a quote, through the dense summary when it applies.
pub fn has_quote_support_flat(quote: &BrassPartsuppScan, flat: &Vec<SInfo>, suppliers: &Directory<Bag<SupplierScan>>,
    nations: &Directory<Bag<NationScan>>, regions: &Directory<Count>) -> (result: bool)
    requires suppliers.valid(), nations.valid(), regions.valid(), flat_ok(flat@, *suppliers), index_positive(*suppliers),
    ensures result == supported(*quote, *suppliers, *nations, *regions),
{
    let key = quote.ps_suppkey;
    if 0 <= key && key < 4194304 && (key as usize) < flat.len() {
        let info = &flat[key as usize];
        proof {
            assert((key as usize) as int == key as int);
            assert(((key as int) as i64) == key);
            assert(info_ok(flat@[key as int], suppliers.lookup((key as int) as i64)));
            assert(suppliers.lookup(key) == suppliers.lookup((key as int) as i64));
        }
        if info.kind == 0 { return false; }
        if info.kind == 1 {
            proof {
                let bag = suppliers.lookup(key)->0;
                assert((*bag).entries@.len() == 1);
                assert((*bag).valid());
                assert((*bag).entries@[0].count > 0);
            }
            return match nations.get(info.nation) {
                None => {
                    proof {
                        let bag = suppliers.lookup(key)->0;
                        assert(!supplier_bag_support(*bag, *nations, *regions)) by {
                            assert forall|index: int| 0 <= index < (*bag).entries@.len() implies !((#[trigger] (*bag).entries@[index]).count > 0 &&
                                supplier_supported(*(*bag).entries@[index].row, *nations, *regions)) by {
                                assert(index == 0);
                            }
                        }
                    }
                    false
                },
                Some(rows) => {
                    let result = has_nation_support(&rows, regions);
                    proof {
                        let bag = suppliers.lookup(key)->0;
                        assert(nations.lookup(info.nation) == Some(rows));
                        if result {
                            assert(supplier_supported(*(*bag).entries@[0].row, *nations, *regions));
                            assert(supplier_bag_support(*bag, *nations, *regions));
                        } else {
                            assert(!supplier_bag_support(*bag, *nations, *regions)) by {
                                assert forall|index: int| 0 <= index < (*bag).entries@.len() implies !((#[trigger] (*bag).entries@[index]).count > 0 &&
                                    supplier_supported(*(*bag).entries@[index].row, *nations, *regions)) by {
                                    assert(index == 0);
                                }
                            }
                        }
                    }
                    result
                },
            };
        }
    }
    has_quote_support(quote, suppliers, nations, regions)
}


/// Whether every row of this supplier's quote sorts after the last of the kept hundred.
pub fn skip_quote(best: &Vec<Candidate>, flat: &Vec<SInfo>, suppliers: &Directory<Bag<SupplierScan>>, key: i64) -> (result: bool)
    requires flat_ok(flat@, *suppliers), best@.len() <= 100,
    ensures result ==> best@.len() == 100 && match suppliers.lookup(key) {
        None => false,
        Some(bag) => (*bag).entries@.len() == 1 && (*(*bag).entries@[0].row).s_acctbal < (*best@[99].supplier).s_acctbal,
    },
{
    if best.len() < 100 { return false; }
    if 0 <= key && key < 4194304 && (key as usize) < flat.len() {
        let info = &flat[key as usize];
        proof {
            assert((key as usize) as int == key as int);
            assert(((key as int) as i64) == key);
            assert(info_ok(flat@[key as int], suppliers.lookup((key as int) as i64)));
        }
        if info.kind == 1 && info.acctbal < best[99].supplier.s_acctbal {
            proof {
                assert(suppliers.lookup(key) == suppliers.lookup((key as int) as i64));
                assert(suppliers.lookup(key) is Some);
                let bag = suppliers.lookup(key)->0;
                assert((*bag).entries@.len() == 1);
                assert(info.acctbal == (*(*bag).entries@[0].row).s_acctbal);
            }
            return true;
        }
    }
    false
}

}
