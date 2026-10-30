// The engine's float SUM / AVG overflow check.  DuckDB's SUM / AVG of DOUBLE give infinity, never an
// error: every group is fine.  (Postgres raises: postgres/engine.rs.)
use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Index, Key};
use crate::cells::*;
use crate::facts::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::numeric::*;
use crate::walk::*;

verus! {

/// No float SUM / AVG of the query errors on this engine.
pub open spec fn engine_safe(db: Db, p: Params) -> bool {
    true
}

/// Whether the engine can aggregate every group of the rows (those a HAVING drops included).
pub fn rows_safe(rows: &Vec<(Key, Arc<Cell>)>, c: &Consts, Ghost(db): Ghost<Db>, Ghost(p): Ghost<Params>, Ghost(g): Ghost<Index<Index<Cell>>>) -> (ok: bool)
    requires
        rep(g, tot_db(db)),
        consts_ok(*c),
        rows_cover(rows@, g, p),
    ensures
        ok == engine_safe(db, p),
{
    true
}

/// No answer exists when the engine cannot aggregate.
pub proof fn lemma_unsafe_unanswerable(db: Db, p: Params)
    requires !engine_safe(db, p),
    ensures !answer_fits(db, p),
{
}

}
