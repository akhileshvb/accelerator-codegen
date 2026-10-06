// Year weights of an order key before and after a transaction.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::values::*;
use crate::events::*;
use crate::orders::*;
use crate::build2::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

proof fn lemma_ysum_push2(ws: Seq<YearW>, w: YearW, y: int)
    ensures ysum(ws.push(w), y) == ysum(ws, y) + if w.year as int == y { w.weight as int } else { 0int },
{
    lemma_sum_push(ws, w, |x: YearW| if x.year as int == y { x.weight as int } else { 0int });
}

proof fn lemma_year_at_take2(years: Seq<Year>, i: int, y: int)
    requires 0 <= i < years.len(),
    ensures year_at(years.take(i + 1), y) == year_at(years.take(i), y) + if years[i].year as int == y { years[i].count as int } else { 0int },
{
    lemma_sum_take(years, i, |r: Year| if r.year as int == y { r.count as int } else { 0int });
}

#[verifier::spinoff_prover]
pub fn year_ws(post: &Work<Vec<Year>>, chg: &Work<Vec<Year>>, ok: i64, okeys: &[i64], changes: &[OrderDelta], Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>)
    -> (r: Option<(Vec<YearW>, Vec<YearW>)>)
    requires post_dates_ready(*post, okeys@, db2), order_changes_ready(*chg, changes@),
        bag_step(db.orders, db2.orders, order_delta_seq_view(changes@)),
    ensures r matches Some((w1, w2)) ==> (forall|y: int| #[trigger] ysum(w1@, y) == dates(db, ok as int, y))
        && (forall|y: int| #[trigger] ysum(w2@, y) == dates(db2, ok as int, y)),
{
    if !has_key(okeys, ok) { return None; }
    let years = post.rows.get(&ok)?;
    let w2 = years_to_ws(years);
    let mut w1 = years_to_ws(years);
    proof {
        assert forall|y: int| #[trigger] ysum(w2@, y) == dates(db2, ok as int, y) by {
            assert(post_year(*post, ok, y) == dates(db2, ok as int, y));
            assert(post_year(*post, ok, y) == year_at(post.rows@[ok]@, y));
        }
    }
    let ghost base = w1@;
    match chg.rows.get(&ok) {
        None => {
            proof {
                assert forall|y: int| #[trigger] ysum(w1@, y) == dates(db, ok as int, y) by {
                    assert(post_year(*chg, ok, y) == 0);
                    lemma_order_capture(db, db2, changes@, ok, y);
                    assert(post_year(*post, ok, y) == year_at(post.rows@[ok]@, y));
                }
            }
        }
        Some(cy) => {
            let mut index = 0;
            proof {
                assert forall|y: int| #[trigger] ysum(w1@, y) == ysum(base, y) - year_at(cy@.take(0), y) by {
                    lemma_sum_empty(cy@.take(0), |r: Year| if r.year as int == y { r.count as int } else { 0int });
                }
            }
            while index < cy.len()
                invariant index <= cy.len(), forall|y: int| #[trigger] ysum(base, y) == year_at(years@, y),
                    forall|y: int| #[trigger] ysum(w1@, y) == ysum(base, y) - year_at(cy@.take(index as int), y),
                decreases cy.len() - index,
            {
                let before = Ghost(w1@);
                let neg = 0i128.checked_sub(cy[index].count)?;
                w1.push(YearW { year: cy[index].year, weight: neg });
                proof {
                    assert(w1@ =~= before@.push(YearW { year: cy[index as int].year, weight: neg }));
                    assert forall|y: int| #[trigger] ysum(w1@, y) == ysum(base, y) - year_at(cy@.take(index as int + 1), y) by {
                        lemma_ysum_push2(before@, YearW { year: cy[index as int].year, weight: neg }, y);
                        lemma_year_at_take2(cy@, index as int, y);
                    }
                }
                index += 1;
            }
            proof {
                assert(cy@.take(index as int) =~= cy@);
                assert forall|y: int| #[trigger] ysum(w1@, y) == dates(db, ok as int, y) by {
                    assert(post_year(*chg, ok, y) == year_at(cy@, y));
                    lemma_order_capture(db, db2, changes@, ok, y);
                    assert(post_year(*post, ok, y) == year_at(post.rows@[ok]@, y));
                    assert(ysum(base, y) == year_at(years@, y));
                }
            }
        }
    }
    Some((w1, w2))
}

}
