// The general path's groups: per (nation name, year) the exact tuple count and profit,
// accumulated from the nation events and the nation-year totals.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::values::*;
use crate::wide::*;
use crate::wmap::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct GEnt { pub name: Vec<u8>, pub year: i64, pub a: Wide, pub b: Wide }

pub open spec fn g_weight(name: Seq<u8>, y: int, f: int) -> spec_fn(GEnt) -> int {
    |e: GEnt| if e.name@ == name && e.year == y { if f == 0 { e.a@ } else { e.b@ } } else { 0int }
}

pub open spec fn g_at(rows: Seq<GEnt>, name: Seq<u8>, y: int, f: int) -> int {
    sum(rows, g_weight(name, y, f))
}

pub open spec fn g_keys(rows: Seq<GEnt>) -> Seq<(Seq<u8>, int)> {
    rows.map_values(|e: GEnt| (e.name@, e.year as int))
}

/// rows[(name, year)] += (da, db).
pub fn bump_g(rows: &mut Vec<GEnt>, name: &Vec<u8>, year: i64, da: &Wide, db: &Wide)
    ensures
        forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 ==> #[trigger] g_at(final(rows)@, n, y, f)
            == g_at(old(rows)@, n, y, f) + if n == name@ && y == year as int { delta_of(da@, db@, f) } else { 0 },
        g_keys(old(rows)@).no_duplicates() ==> g_keys(final(rows)@).no_duplicates(),
{
    let ghost before = rows@;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), rows@ == before, before == old(rows)@,
            forall|position: int| 0 <= position < index ==> !((#[trigger] rows@[position]).name@ == name@ && rows@[position].year == year),
        decreases rows.len() - index,
    {
        if rows[index].year == year && bytes_equal(&rows[index].name, name) {
            let a2 = rows[index].a.add(da);
            let b2 = rows[index].b.add(db);
            let entry = GEnt { name: copy_bytes(name), year, a: a2, b: b2 };
            let ghost replacement = entry;
            rows.set(index, entry);
            proof {
                assert forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 implies #[trigger] g_at(rows@, n, y, f)
                    == g_at(before, n, y, f) + if n == name@ && y == year as int { delta_of(da@, db@, f) } else { 0 } by {
                    lemma_sum_update(before, index as int, replacement, g_weight(n, y, f));
                }
                assert(g_keys(rows@) =~= g_keys(before));
            }
            return;
        }
        index += 1;
    }
    let entry = GEnt { name: copy_bytes(name), year, a: da.dup(), b: db.dup() };
    let ghost appended = entry;
    rows.push(entry);
    proof {
        assert forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 implies #[trigger] g_at(rows@, n, y, f)
            == g_at(before, n, y, f) + if n == name@ && y == year as int { delta_of(da@, db@, f) } else { 0 } by {
            lemma_sum_push(before, appended, g_weight(n, y, f));
        }
        if g_keys(before).no_duplicates() {
            assert(!g_keys(before).contains((name@, year as int)));
            verus_prooflib::lemma_unique_push(g_keys(before), (name@, year as int));
            assert(g_keys(rows@) =~= g_keys(before).push((name@, year as int)));
        }
    }
}

}
