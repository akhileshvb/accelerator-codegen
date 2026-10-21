use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bytes::bytes_eq_exec;
use verus_common::sums::*;
use verus_prooflib::*;
use verus_prooflib::wide::Wide;
use crate::gen_obligations::*;
use crate::model::*;
use crate::storage::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub fn copy_bytes(bytes: &[u8]) -> (result: Vec<u8>)
    ensures result@ == bytes@,
{
    let mut result = Vec::new();
    let mut index = 0usize;
    while index < bytes.len()
        invariant index <= bytes.len(), result@ == bytes@.take(index as int),
        decreases bytes.len() - index,
    {
        result.push(bytes[index]);
        index += 1;
    }
    result
}

pub fn hash_brand(bytes: &[u8]) -> (result: u64)
    ensures result == fingerprint(bytes@), result < 1000000007,
{
    proof { reveal(fingerprint); }
    let mut result = 0u64;
    let mut index = 0usize;
    while index < bytes.len()
        invariant index <= bytes.len(), result < 1000000007,
            result == fingerprint(bytes@.take(index as int)),
        decreases bytes.len() - index,
    {
        proof {
            assert(bytes@.take(index as int + 1).drop_last() =~= bytes@.take(index as int));
            assert(bytes@.take(index as int + 1).last() == bytes@[index as int]);
            reveal(fingerprint);
        }
        result = (result * 257 + bytes[index] as u64 + 1) % 1000000007;
        index += 1;
    }
    proof { assert(bytes@.take(bytes.len() as int) =~= bytes@); }
    result
}

pub fn hash_hist(class: u8, brand: &[u8], quantity: i64) -> (result: u64)
    requires class < 3,
    ensures result == hist_key(class, brand@, quantity),
{
    hash_hist_from_brand(class, hash_brand(brand), quantity)
}

pub fn hash_hist_from_brand(class: u8, brand_hash: u64, quantity: i64) -> (result: u64)
    requires class < 3, brand_hash < 1000000007,
    ensures result == ((brand_hash as int * 3 + class as int) * 1000000007
        + (quantity as u64) as int % 1000000007) as u64,
{
    (brand_hash * 3 + class as u64) * 1000000007 + (quantity as u64) % 1000000007
}

pub fn input_class(part: &PartInput) -> (result: u8)
    requires classified(part_input_view(*part)),
    ensures result < 3,
        forall|class: u8| #![trigger class_matches(part_input_view(*part), class)]
        (result == class) == class_matches(part_input_view(*part), class),
{
    if part.p_size < 1 { proof { assert(false); } return 0; }
    let text = &part.p_container;
    if part.p_size <= 5 && (bytes_eq_exec(text, &[b'S', b'M', b' ', b'C', b'A', b'S', b'E']) || bytes_eq_exec(text, &[b'S', b'M', b' ', b'B', b'O', b'X'])
        || bytes_eq_exec(text, &[b'S', b'M', b' ', b'P', b'A', b'C', b'K']) || bytes_eq_exec(text, &[b'S', b'M', b' ', b'P', b'K', b'G'])) {
        proof {
            assert forall|class: u8| #![trigger class_matches(part_input_view(*part), class)]
                (0u8 == class) == class_matches(part_input_view(*part), class) by {
                if class_matches(part_input_view(*part), class) { crate::semantic::lemma_classes_disjoint(part_input_view(*part), 0, class); }
            }
        }
        return 0;
    }
    if part.p_size <= 10 && (bytes_eq_exec(text, &[b'M', b'E', b'D', b' ', b'B', b'A', b'G']) || bytes_eq_exec(text, &[b'M', b'E', b'D', b' ', b'B', b'O', b'X'])
        || bytes_eq_exec(text, &[b'M', b'E', b'D', b' ', b'P', b'K', b'G']) || bytes_eq_exec(text, &[b'M', b'E', b'D', b' ', b'P', b'A', b'C', b'K'])) {
        proof {
            assert forall|class: u8| #![trigger class_matches(part_input_view(*part), class)]
                (1u8 == class) == class_matches(part_input_view(*part), class) by {
                if class_matches(part_input_view(*part), class) { crate::semantic::lemma_classes_disjoint(part_input_view(*part), 1, class); }
            }
        }
        return 1;
    }
    if part.p_size <= 15 && (bytes_eq_exec(text, &[b'L', b'G', b' ', b'C', b'A', b'S', b'E']) || bytes_eq_exec(text, &[b'L', b'G', b' ', b'B', b'O', b'X'])
        || bytes_eq_exec(text, &[b'L', b'G', b' ', b'P', b'A', b'C', b'K']) || bytes_eq_exec(text, &[b'L', b'G', b' ', b'P', b'K', b'G'])) {
        proof {
            assert forall|class: u8| #![trigger class_matches(part_input_view(*part), class)]
                (2u8 == class) == class_matches(part_input_view(*part), class) by {
                if class_matches(part_input_view(*part), class) { crate::semantic::lemma_classes_disjoint(part_input_view(*part), 2, class); }
            }
        }
        return 2;
    }
    proof { assert(false); }
    0
}

pub fn duplicate_part(cell: &PartCell) -> (result: PartCell)
    ensures result == *cell, result.class == cell.class, (*result.brand)@ == (*cell.brand)@, result.count == cell.count,
{
    PartCell { class: cell.class, brand: Arc::clone(&cell.brand), count: cell.count.dup() }
}

pub fn duplicate_hist(cell: &HistCell) -> (result: HistCell)
    ensures result == *cell, result.class == cell.class, (*result.brand)@ == (*cell.brand)@,
        result.quantity == cell.quantity, result.count == cell.count, result.revenue == cell.revenue,
{
    HistCell { class: cell.class, brand: Arc::clone(&cell.brand), quantity: cell.quantity,
        count: cell.count.dup(), revenue: cell.revenue.dup() }
}

pub fn add_part(cells: &mut Vec<PartCell>, class: u8, brand: &Arc<Vec<u8>>, change: &Wide)
    requires part_cells_valid(old(cells)@, false), class < 3,
    ensures part_cells_valid(final(cells)@, false), forall|other_class: u8, other_brand: Seq<u8>|
        #![trigger part_cells_value(final(cells)@, other_class, other_brand)]
        part_cells_value(final(cells)@, other_class, other_brand)
            == part_cells_value(old(cells)@, other_class, other_brand)
                + if other_class == class && other_brand == (**brand)@ { change@ } else { 0 },
{
    let ghost before = cells@;
    let mut index = 0usize;
    while index < cells.len()
        invariant index <= cells.len(), cells@ == before, before == old(cells)@, part_cells_valid(before, false),
        decreases cells.len() - index,
    {
        if cells[index].class == class && bytes_eq_exec(&**brand, &*cells[index].brand) {
            let count = cells[index].count.add(change);
            cells.set(index, PartCell { class, brand: Arc::clone(brand), count });
            proof {
                assert forall|other_class: u8, other_brand: Seq<u8>|
                    #![trigger part_cells_value(cells@, other_class, other_brand)]
                    part_cells_value(cells@, other_class, other_brand)
                        == part_cells_value(before, other_class, other_brand)
                            + if other_class == class && other_brand == (**brand)@ { change@ } else { 0 }
                by {
                    let weight = |cell: PartCell| part_cell_weight(cell, other_class, other_brand);
                    lemma_sum_update(before, index as int, cells@[index as int], weight);
                }
                assert(part_cells_valid(cells@, false));
            }
            return;
        }
        index += 1;
    }
    cells.push(PartCell { class, brand: Arc::clone(brand), count: change.dup() });
    proof {
        assert forall|other_class: u8, other_brand: Seq<u8>|
            #![trigger part_cells_value(cells@, other_class, other_brand)]
            part_cells_value(cells@, other_class, other_brand)
                == part_cells_value(before, other_class, other_brand)
                    + if other_class == class && other_brand == (**brand)@ { change@ } else { 0 }
        by {
            let weight = |cell: PartCell| part_cell_weight(cell, other_class, other_brand);
            lemma_sum_push(before, cells@.last(), weight);
        }
    }
}

pub fn add_hist(cells: &mut Vec<HistCell>, class: u8, brand: &Arc<Vec<u8>>, quantity: i64,
    count_change: &Wide, revenue_change: &Wide)
    requires class < 3, hist_cells_valid(old(cells)@, hist_key(class, (**brand)@, quantity), false),
    ensures hist_cells_valid(final(cells)@, hist_key(class, (**brand)@, quantity), false),
        forall|other_class: u8, other_brand: Seq<u8>, other_quantity: int, is_revenue: bool|
            #![trigger hist_cells_value(final(cells)@, other_class, other_brand, other_quantity, is_revenue)]
            hist_cells_value(final(cells)@, other_class, other_brand, other_quantity, is_revenue)
                == hist_cells_value(old(cells)@, other_class, other_brand, other_quantity, is_revenue)
                    + if other_class == class && other_brand == (**brand)@ && other_quantity == quantity {
                        if is_revenue { revenue_change@ } else { count_change@ }
                    } else { 0 },
{
    let ghost before = cells@;
    let mut index = 0usize;
    while index < cells.len()
        invariant index <= cells.len(), cells@ == before,
            before == old(cells)@,
            hist_cells_valid(before, hist_key(class, (**brand)@, quantity), false),
        decreases cells.len() - index,
    {
        if cells[index].class == class && cells[index].quantity == quantity
            && bytes_eq_exec(&**brand, &*cells[index].brand) {
            let count = cells[index].count.add(count_change);
            let revenue = cells[index].revenue.add(revenue_change);
            cells.set(index, HistCell { class, brand: Arc::clone(brand), quantity, count, revenue });
            proof {
                assert forall|other_class: u8, other_brand: Seq<u8>, other_quantity: int, is_revenue: bool|
                    #![trigger hist_cells_value(cells@, other_class, other_brand, other_quantity, is_revenue)]
                    hist_cells_value(cells@, other_class, other_brand, other_quantity, is_revenue)
                        == hist_cells_value(before, other_class, other_brand, other_quantity, is_revenue)
                            + if other_class == class && other_brand == (**brand)@ && other_quantity == quantity {
                                if is_revenue { revenue_change@ } else { count_change@ }
                            } else { 0 }
                by {
                    let weight = |cell: HistCell| hist_cell_weight(cell, other_class, other_brand, other_quantity, is_revenue);
                    lemma_sum_update(before, index as int, cells@[index as int], weight);
                }
                assert(hist_cells_valid(cells@, hist_key(class, (**brand)@, quantity), false));
            }
            return;
        }
        index += 1;
    }
    cells.push(HistCell { class, brand: Arc::clone(brand), quantity, count: count_change.dup(), revenue: revenue_change.dup() });
    proof {
        assert forall|other_class: u8, other_brand: Seq<u8>, other_quantity: int, is_revenue: bool|
            #![trigger hist_cells_value(cells@, other_class, other_brand, other_quantity, is_revenue)]
            hist_cells_value(cells@, other_class, other_brand, other_quantity, is_revenue)
                == hist_cells_value(before, other_class, other_brand, other_quantity, is_revenue)
                    + if other_class == class && other_brand == (**brand)@ && other_quantity == quantity {
                        if is_revenue { revenue_change@ } else { count_change@ }
                    } else { 0 }
        by {
            let weight = |cell: HistCell| hist_cell_weight(cell, other_class, other_brand, other_quantity, is_revenue);
            lemma_sum_push(before, cells@.last(), weight);
        }
    }
}

pub fn patch_hist(work: &mut Work<HistCell>, cell: &PartCell, quantity: i64, count: &Wide, revenue: &Wide)
    requires hist_patch_valid(*old(work)), cell.class < 3,
    ensures hist_patch_valid(*final(work)), forall|class: u8, brand: Seq<u8>, amount: int, is_revenue: bool|
        #![trigger hist_work_value(*final(work), class, brand, amount, is_revenue)]
        hist_work_value(*final(work), class, brand, amount, is_revenue)
            == hist_work_value(*old(work), class, brand, amount, is_revenue)
                + if class == cell.class && brand == (*cell.brand)@ && amount == quantity {
                    if is_revenue { revenue@ } else { count@ }
                } else { 0 },
{
    let key = hash_hist(cell.class, &*cell.brand, quantity);
    let mut cells = work.take(key);
    add_hist(&mut cells, cell.class, &cell.brand, quantity, count, revenue);
    work.put(key, cells);
}

pub fn amount(price: i64, discount: i64) -> (result: Wide)
    ensures result@ == price as int * (100 - discount as int),
{
    Wide::from_i128(price as i128).mul(&Wide::from_i128(100i128 - discount as i128))
}

pub fn contribute(work: &mut Work<HistCell>, cells: &Vec<PartCell>, quantity: i64, sign: i64, value: &Wide)
    requires hist_patch_valid(*old(work)), part_cells_valid(cells@, false),
    ensures hist_patch_valid(*final(work)), forall|class: u8, brand: Seq<u8>, target: int, revenue: bool|
        #![trigger hist_work_value(*final(work), class, brand, target, revenue)]
        hist_work_value(*final(work), class, brand, target, revenue)
            == hist_work_value(*old(work), class, brand, target, revenue)
                + if target == quantity { part_cells_value(cells@, class, brand) * sign as int * if revenue { value@ } else { 1 } } else { 0 },
{
    let ghost before = *work;
    let mut index = 0usize;
    while index < cells.len()
        invariant index <= cells.len(), before == *old(work), hist_patch_valid(*work), part_cells_valid(cells@, false),
            forall|class: u8, brand: Seq<u8>, target: int, revenue: bool|
                #![trigger hist_work_value(*work, class, brand, target, revenue)]
                hist_work_value(*work, class, brand, target, revenue)
                    == hist_work_value(before, class, brand, target, revenue)
                        + sum(cells@.take(index as int), |cell: PartCell| join_cell_weight(cell, class, brand, target, quantity as int, sign as int, value@, revenue)),
        decreases cells.len() - index,
    {
        let ghost prior = *work;
        let cell = &cells[index];
        if !cell.count.is_zero() {
            let count = cell.count.mul(&Wide::from_i128(sign as i128));
            let revenue = value.mul(&count);
            proof {
                assert(value@ * count@ == cell.count@ * (sign as int) * value@) by (nonlinear_arith)
                    requires count@ == cell.count@ * sign as int;
            }
            patch_hist(work, cell, quantity, &count, &revenue);
            proof {
                assert forall|class: u8, brand: Seq<u8>, target: int, is_revenue: bool|
                    #![trigger hist_work_value(*work, class, brand, target, is_revenue)]
                    hist_work_value(*work, class, brand, target, is_revenue)
                        == hist_work_value(prior, class, brand, target, is_revenue)
                            + join_cell_weight(cells@[index as int], class, brand, target, quantity as int, sign as int, value@, is_revenue) by {}
            }
        } else {
            proof {
                assert(*work == prior);
                assert forall|class: u8, brand: Seq<u8>, target: int, is_revenue: bool|
                    #[trigger] join_cell_weight(cells@[index as int], class, brand, target, quantity as int, sign as int, value@, is_revenue) == 0 by {
                    let factor = if is_revenue { value@ } else { 1 };
                    assert(0int * (sign as int) * factor == 0) by (nonlinear_arith);
                }
            }
        }
        proof {
            assert forall|class: u8, brand: Seq<u8>, target: int, is_revenue: bool|
                #![trigger hist_work_value(*work, class, brand, target, is_revenue)]
                hist_work_value(*work, class, brand, target, is_revenue)
                    == hist_work_value(prior, class, brand, target, is_revenue)
                        + join_cell_weight(cells@[index as int], class, brand, target, quantity as int, sign as int, value@, is_revenue)
            by {
                let count = cells@[index as int].count@;
                let signed = sign as int;
                let amount = value@;
                assert(amount * (count * signed) == count * signed * amount) by (nonlinear_arith);
            }
            assert forall|class: u8, brand: Seq<u8>, target: int, revenue: bool|
                #![trigger hist_work_value(*work, class, brand, target, revenue)]
                hist_work_value(*work, class, brand, target, revenue)
                    == hist_work_value(before, class, brand, target, revenue)
                        + sum(cells@.take(index as int + 1), |cell: PartCell| join_cell_weight(cell, class, brand, target, quantity as int, sign as int, value@, revenue))
            by {
                lemma_sum_take(cells@, index as int, |cell: PartCell| join_cell_weight(cell, class, brand, target, quantity as int, sign as int, value@, revenue));
                let count = cells@[index as int].count@;
                let signed = sign as int;
                let amount = value@;
                assert(amount * (count * signed) == count * signed * amount) by (nonlinear_arith);
            }
        }
        index += 1;
    }
    proof {
        assert forall|class: u8, brand: Seq<u8>, target: int, revenue: bool|
            #![trigger hist_work_value(*work, class, brand, target, revenue)]
            hist_work_value(*work, class, brand, target, revenue)
                == hist_work_value(before, class, brand, target, revenue)
                    + if target == quantity { part_cells_value(cells@, class, brand) * sign as int * if revenue { value@ } else { 1 } } else { 0 }
        by { lemma_join_cell_factor(cells@, class, brand, target, quantity as int, sign as int, value@, revenue); }
    }
}

pub fn patch_part(work: &mut Work<PartCell>, key: i64, class: u8, brand: &Arc<Vec<u8>>, change: &Wide)
    requires part_patch_valid(*old(work)), class < 3,
    ensures part_patch_valid(*final(work)), forall|other_key: int, other_class: u8, other_brand: Seq<u8>|
        #![trigger part_work_value(*final(work), other_key, other_class, other_brand)]
        part_work_value(*final(work), other_key, other_class, other_brand)
            == part_work_value(*old(work), other_key, other_class, other_brand)
                + if other_key == key && other_class == class && other_brand == (**brand)@ { change@ } else { 0 },
{
    let mut cells = work.take(key as u64);
    add_part(&mut cells, class, brand, change);
    work.put(key as u64, cells);
    proof {
        assert forall|other_key: int, other_class: u8, other_brand: Seq<u8>|
            #![trigger part_work_value(*work, other_key, other_class, other_brand)]
            part_work_value(*work, other_key, other_class, other_brand)
                == part_work_value(*old(work), other_key, other_class, other_brand)
                    + if other_key == key && other_class == class && other_brand == (**brand)@ { change@ } else { 0 }
        by {
            if i64::MIN <= other_key <= i64::MAX {
                let other = other_key as i64;
                assert((other as u64 == key as u64) == (other == key)) by (bit_vector);
            }
        }
    }
}

pub fn coalesce_parts(delta: &Vec<PartDelta>) -> (work: Work<PartCell>)
    ensures part_patch_valid(work) && forall|key: int, class: u8, brand: Seq<u8>|
        #![trigger part_work_value(work, key, class, brand)]
        part_work_value(work, key, class, brand) == part_delta_prefix(delta@, delta.len() as int, key, class, brand),
{
    let mut work = Work::new();
    let mut index = 0usize;
    while index < delta.len()
        invariant index <= delta.len(), part_patch_valid(work),
            forall|key: int, class: u8, brand: Seq<u8>| #![trigger part_work_value(work, key, class, brand)]
                part_work_value(work, key, class, brand) == part_delta_prefix(delta@, index as int, key, class, brand),
        decreases delta.len() - index,
    {
        let ghost processed = index as int;
        let row = &delta[index];
        let brand = Arc::new(copy_bytes(&row.p_brand));
        let ghost before = work;
        let mut class = 0u8;
        while class < 3
            invariant class <= 3, part_patch_valid(work),
                (*brand)@ == row.p_brand@,
                forall|other_key: int, other_class: u8, other_brand: Seq<u8>|
                    #![trigger part_work_value(work, other_key, other_class, other_brand)]
                    part_work_value(work, other_key, other_class, other_brand)
                        == part_work_value(before, other_key, other_class, other_brand)
                            + if other_class < class { part_delta_weight(*row, other_key, other_class, other_brand) } else { 0 },
            decreases 3 - class,
        {
            let flag = if class == 0 { row.small } else if class == 1 { row.medium } else { row.large };
            if flag != 0 { patch_part(&mut work, row.p_partkey, class, &brand, &Wide::from_i128(row.sign as i128)); }
            class += 1;
        }
        proof {
            assert forall|other_key: int, other_class: u8, other_brand: Seq<u8>|
                #![trigger part_work_value(work, other_key, other_class, other_brand)]
                part_work_value(work, other_key, other_class, other_brand)
                    == part_delta_prefix(delta@, index as int + 1, other_key, other_class, other_brand)
            by {
                lemma_sum_take(delta@, index as int, |row: PartDelta| part_delta_weight(row, other_key, other_class, other_brand));
            }
        }
        index += 1;
    }
    work
}

pub fn changed(cells: &Vec<PartCell>) -> (result: bool)
    ensures result == exists|index: int| #![trigger cells@[index]] 0 <= index < cells.len() && cells@[index].count@ != 0,
{
    let mut index = 0usize;
    while index < cells.len()
        invariant index <= cells.len(),
            forall|seen: int| #![trigger cells@[seen]] 0 <= seen < index ==> cells@[seen].count@ == 0,
        decreases cells.len() - index,
    {
        if !cells[index].count.is_zero() { return true; }
        index += 1;
    }
    false
}

pub fn merge_parts(base: &Vec<PartCell>, delta: &Vec<PartCell>) -> (result: Vec<PartCell>)
    requires part_cells_valid(base@, false), part_cells_valid(delta@, false),
    ensures part_cells_valid(result@, false) && forall|class: u8, brand: Seq<u8>|
        #![trigger part_cells_value(result@, class, brand)]
        part_cells_value(result@, class, brand) == part_cells_value(base@, class, brand) + part_cells_value(delta@, class, brand),
{
    let mut cells = Vec::new();
    let mut index = 0usize;
    while index < base.len()
        invariant index <= base.len(), cells@ == base@.take(index as int), part_cells_valid(base@, false),
        decreases base.len() - index,
    {
        cells.push(duplicate_part(&base[index]));
        index += 1;
    }
    let mut index = 0usize;
    while index < delta.len()
        invariant index <= delta.len(), part_cells_valid(cells@, false), part_cells_valid(delta@, false),
            forall|class: u8, brand: Seq<u8>| #![trigger part_cells_value(cells@, class, brand)]
                part_cells_value(cells@, class, brand) == part_cells_value(base@, class, brand)
                    + part_cells_value(delta@.take(index as int), class, brand),
        decreases delta.len() - index,
    {
        add_part(&mut cells, delta[index].class, &delta[index].brand, &delta[index].count);
        proof {
            assert forall|class: u8, brand: Seq<u8>| #![trigger part_cells_value(cells@, class, brand)]
                part_cells_value(cells@, class, brand) == part_cells_value(base@, class, brand)
                    + part_cells_value(delta@.take(index as int + 1), class, brand) by {
                lemma_sum_take(delta@, index as int, |cell: PartCell| part_cell_weight(cell, class, brand));
            }
        }
        index += 1;
    }
    let mut rows = Vec::new();
    let mut index = 0usize;
    while index < cells.len()
        invariant index <= cells.len(), part_cells_valid(cells@, false), part_cells_valid(rows@, false),
            forall|class: u8, brand: Seq<u8>| #![trigger part_cells_value(cells@, class, brand)]
                part_cells_value(cells@, class, brand) == part_cells_value(base@, class, brand) + part_cells_value(delta@, class, brand),
            forall|class: u8, brand: Seq<u8>| #![trigger part_cells_value(rows@, class, brand)]
                part_cells_value(rows@, class, brand) == part_cells_value(cells@.take(index as int), class, brand),
        decreases cells.len() - index,
    {
        let ghost previous = rows@;
        if !cells[index].count.is_zero() {
            rows.push(duplicate_part(&cells[index]));
            proof { assert(rows@ =~= previous.push(cells@[index as int])); }
        }
        proof {
            assert forall|class: u8, brand: Seq<u8>| #![trigger part_cells_value(rows@, class, brand)]
                part_cells_value(rows@, class, brand) == part_cells_value(cells@.take(index as int + 1), class, brand) by {
                assert(part_cells_value(previous, class, brand) == part_cells_value(cells@.take(index as int), class, brand));
                lemma_sum_take(cells@, index as int, |cell: PartCell| part_cell_weight(cell, class, brand));
                if cells@[index as int].count@ != 0 { lemma_sum_push(previous, cells@[index as int], |cell: PartCell| part_cell_weight(cell, class, brand)); }
                else { assert(rows@ == previous); assert(part_cell_weight(cells@[index as int], class, brand) == 0); }
            }
        }
        index += 1;
    }
    rows
}

pub fn merge_histogram(base: &Vec<HistCell>, delta: &Vec<HistCell>, key: u64) -> (result: Vec<HistCell>)
    requires hist_cells_valid(base@, key, false), hist_cells_valid(delta@, key, false),
    ensures hist_cells_valid(result@, key, false) && forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
        #![trigger hist_cells_value(result@, class, brand, quantity, revenue)]
        hist_cells_value(result@, class, brand, quantity, revenue)
            == hist_cells_value(base@, class, brand, quantity, revenue) + hist_cells_value(delta@, class, brand, quantity, revenue),
{
    let mut cells = Vec::new();
    let mut index = 0usize;
    while index < base.len()
        invariant index <= base.len(), cells@ == base@.take(index as int), hist_cells_valid(base@, key, false),
        decreases base.len() - index,
    {
        cells.push(duplicate_hist(&base[index]));
        index += 1;
    }
    let mut index = 0usize;
    while index < delta.len()
        invariant index <= delta.len(), hist_cells_valid(cells@, key, false), hist_cells_valid(delta@, key, false),
            forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
                #![trigger hist_cells_value(cells@, class, brand, quantity, revenue)]
                hist_cells_value(cells@, class, brand, quantity, revenue)
                    == hist_cells_value(base@, class, brand, quantity, revenue) + hist_cells_value(delta@.take(index as int), class, brand, quantity, revenue),
        decreases delta.len() - index,
    {
        let cell = &delta[index];
        add_hist(&mut cells, cell.class, &cell.brand, cell.quantity, &cell.count, &cell.revenue);
        proof {
            assert forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
                #![trigger hist_cells_value(cells@, class, brand, quantity, revenue)]
                hist_cells_value(cells@, class, brand, quantity, revenue)
                    == hist_cells_value(base@, class, brand, quantity, revenue) + hist_cells_value(delta@.take(index as int + 1), class, brand, quantity, revenue) by {
                lemma_sum_take(delta@, index as int, |cell: HistCell| hist_cell_weight(cell, class, brand, quantity, revenue));
            }
        }
        index += 1;
    }
    let mut rows = Vec::new();
    let mut index = 0usize;
    while index < cells.len()
        invariant index <= cells.len(), hist_cells_valid(cells@, key, false), hist_cells_valid(rows@, key, false),
            forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
                #![trigger hist_cells_value(cells@, class, brand, quantity, revenue)]
                hist_cells_value(cells@, class, brand, quantity, revenue)
                    == hist_cells_value(base@, class, brand, quantity, revenue) + hist_cells_value(delta@, class, brand, quantity, revenue),
            forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
                #![trigger hist_cells_value(rows@, class, brand, quantity, revenue)]
                hist_cells_value(rows@, class, brand, quantity, revenue) == hist_cells_value(cells@.take(index as int), class, brand, quantity, revenue),
        decreases cells.len() - index,
    {
        let ghost previous = rows@;
        if !(cells[index].count.is_zero() && cells[index].revenue.is_zero()) {
            rows.push(duplicate_hist(&cells[index]));
            proof { assert(rows@ =~= previous.push(cells@[index as int])); }
        }
        proof {
            assert forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
                #![trigger hist_cells_value(rows@, class, brand, quantity, revenue)]
                hist_cells_value(rows@, class, brand, quantity, revenue) == hist_cells_value(cells@.take(index as int + 1), class, brand, quantity, revenue) by {
                assert(hist_cells_value(previous, class, brand, quantity, revenue) == hist_cells_value(cells@.take(index as int), class, brand, quantity, revenue));
                lemma_sum_take(cells@, index as int, |cell: HistCell| hist_cell_weight(cell, class, brand, quantity, revenue));
                if !(cells@[index as int].count@ == 0 && cells@[index as int].revenue@ == 0) { lemma_sum_push(previous, cells@[index as int], |cell: HistCell| hist_cell_weight(cell, class, brand, quantity, revenue)); }
                else { assert(rows@ == previous); assert(hist_cell_weight(cells@[index as int], class, brand, quantity, revenue) == 0); }
            }
        }
        index += 1;
    }
    rows
}

}
