// The class index: (fy, uom) -> (value key, adsh) -> the answer rows of one filing at that value.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::model::*;
use crate::f64bits::*;
use verus_common::rel::*;

verus! {

pub struct Row {
    pub name: Option<Arc<Vec<u8>>>,
    pub tag: Arc<Vec<u8>>,
    pub val: u64,
}

impl Row {
    pub open spec fn view(self) -> OutRow {
        OutRow {
            name: match self.name { Some(n) => Some((*n)@), None => None },
            tag: Some((*self.tag)@),
            value: Some(f64_val(self.val)),
        }
    }
}

pub type Inner = Index<Vec<Row>>;
pub type Cells = Index<Inner>;

pub open spec fn views(c: Seq<Row>) -> Seq<OutRow> {
    c.map_values(|r: Row| r.view())
}

pub struct CellId {
    pub fy: int,
    pub uom: Seq<u8>,
    pub d: int,
    pub a: Seq<u8>,
}

pub open spec fn class_key(id: CellId) -> KeyView { (id.fy, id.uom) }

pub open spec fn cell_key(id: CellId) -> KeyView { (id.d, id.a) }

pub open spec fn cell_at(c: Cells, id: CellId) -> Option<Vec<Row>> {
    match c.at(class_key(id)) {
        Some(inner) => inner.at(cell_key(id)),
        None => None,
    }
}

/// The weight family a state stands for: Σ g over the rows of a cell.
pub type Fam = spec_fn(CellId, spec_fn(OutRow) -> int) -> int;

pub open spec fn fam_w(w: Fam, id: CellId, g: spec_fn(OutRow) -> int) -> int { w(id, g) }

/// v is the value of the double with bits b, whose index number is d.
pub open spec fn vkey(v: int, d: int) -> bool {
    exists|b: u64| f64_finite(b) && f64_val(b) == v && f64_desc_key(b) == d
}

pub open spec fn cell_ok(c: Option<Vec<Row>>, w: Fam, id: CellId) -> bool {
    match c {
        Some(v) => {
            &&& forall|g: spec_fn(OutRow) -> int| #[trigger] sum(views(v@), g) == fam_w(w, id, g)
            &&& forall|k: int| 0 <= k < v@.len() ==> f64_finite((#[trigger] v@[k]).val) && f64_desc_key(v@[k].val) == id.d
        },
        None => forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(w, id, g) == 0,
    }
}

pub proof fn lemma_cell_ok_congr(c: Option<Vec<Row>>, w1: Fam, w2: Fam, id: CellId)
    requires
        cell_ok(c, w1, id),
        forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(w2, id, g) == fam_w(w1, id, g),
    ensures
        cell_ok(c, w2, id),
{
}

pub proof fn lemma_rep_congr(c: Cells, w1: Fam, w2: Fam)
    requires
        cells_rep(c, w1),
        forall|id: CellId, g: spec_fn(OutRow) -> int| #[trigger] fam_w(w2, id, g) == fam_w(w1, id, g),
    ensures
        cells_rep(c, w2),
{
    assert forall|id: CellId| cell_ok(#[trigger] cell_at(c, id), w2, id) by {
        lemma_cell_ok_congr(cell_at(c, id), w1, w2, id);
    }
}

pub open spec fn cells_rep(c: Cells, w: Fam) -> bool {
    &&& c.wf()
    &&& forall|k: KeyView| #[trigger] c.at(k) is Some ==> c.at(k)->0.wf()
    &&& forall|id: CellId| cell_ok(#[trigger] cell_at(c, id), w, id)
}

pub fn kkey(number: i64, text: &Arc<Vec<u8>>) -> (k: Key)
    ensures k.view() == (number as int, (**text)@),
{
    Key::shared(number, text.clone())
}

pub fn cells_get(c: &Cells, fy: i64, uom: &Arc<Vec<u8>>, d: i64, a: &Arc<Vec<u8>>) -> (r: Option<Arc<Vec<Row>>>)
    requires
        c.wf(),
        forall|k: KeyView| #[trigger] c.at(k) is Some ==> c.at(k)->0.wf(),
    ensures
        match r {
            Some(v) => cell_at(*c, CellId { fy: fy as int, uom: (**uom)@, d: d as int, a: (**a)@ }) == Some(*v),
            None => cell_at(*c, CellId { fy: fy as int, uom: (**uom)@, d: d as int, a: (**a)@ }) is None,
        },
{
    let ck = kkey(fy, uom);
    match c.get(&ck) {
        None => None,
        Some(inner) => {
            let key = kkey(d, a);
            inner.get(&key)
        }
    }
}

pub fn cells_put(c: &mut Cells, fy: i64, uom: &Arc<Vec<u8>>, d: i64, a: &Arc<Vec<u8>>, cell: Vec<Row>)
    requires
        old(c).wf(),
        forall|k: KeyView| #[trigger] old(c).at(k) is Some ==> old(c).at(k)->0.wf(),
    ensures
        final(c).wf(),
        forall|k: KeyView| #[trigger] final(c).at(k) is Some ==> final(c).at(k)->0.wf(),
        forall|id: CellId| #[trigger] cell_at(*final(c), id) == if id == (CellId { fy: fy as int, uom: (**uom)@, d: d as int, a: (**a)@ }) {
            Some(cell)
        } else {
            cell_at(*old(c), id)
        },
{
    let ck = kkey(fy, uom);
    let key = kkey(d, a);
    let ghost me = CellId { fy: fy as int, uom: (**uom)@, d: d as int, a: (**a)@ };
    match c.get(&ck) {
        None => {
            let mut inner: Inner = Index::new();
            inner.put(key, Arc::new(cell));
            c.put(ck, Arc::new(inner));
        }
        Some(old_inner) => {
            let mut inner = old_inner.share();
            inner.put(key, Arc::new(cell));
            c.put(ck, Arc::new(inner));
        }
    }
    proof {
        assert forall|id: CellId| #[trigger] cell_at(*final(c), id) == if id == me { Some(cell) } else { cell_at(*old(c), id) } by {
            if class_key(id) == class_key(me) {
                if id != me { assert(cell_key(id) != cell_key(me)); }
            }
        }
    }
}


pub fn cells_remove(c: &mut Cells, fy: i64, uom: &Arc<Vec<u8>>, d: i64, a: &Arc<Vec<u8>>)
    requires
        old(c).wf(),
        forall|k: KeyView| #[trigger] old(c).at(k) is Some ==> old(c).at(k)->0.wf(),
    ensures
        final(c).wf(),
        forall|k: KeyView| #[trigger] final(c).at(k) is Some ==> final(c).at(k)->0.wf(),
        forall|id: CellId| #[trigger] cell_at(*final(c), id) == if id == (CellId { fy: fy as int, uom: (**uom)@, d: d as int, a: (**a)@ }) {
            None
        } else {
            cell_at(*old(c), id)
        },
{
    let ck = kkey(fy, uom);
    let key = kkey(d, a);
    let ghost me = CellId { fy: fy as int, uom: (**uom)@, d: d as int, a: (**a)@ };
    match c.get(&ck) {
        None => {}
        Some(old_inner) => {
            let mut inner = old_inner.share();
            inner.remove(&key);
            c.put(ck, Arc::new(inner));
        }
    }
    proof {
        assert forall|id: CellId| #[trigger] cell_at(*final(c), id) == if id == me { None } else { cell_at(*old(c), id) } by {
            if class_key(id) == class_key(me) {
                if id != me { assert(cell_key(id) != cell_key(me)); }
            }
        }
    }
}

pub fn copy_row(r: &Row) -> (c: Row)
    ensures c == *r,
{
    Row { name: match &r.name { Some(n) => Some(n.clone()), None => None }, tag: r.tag.clone(), val: r.val }
}

pub fn copy_cell(src: &Vec<Row>) -> (out: Vec<Row>)
    ensures out@ == src@,
{
    let mut out: Vec<Row> = Vec::new();
    let mut i: usize = 0;
    while i < src.len()
        invariant
            i <= src@.len(),
            out@ == src@.take(i as int),
        decreases src.len() - i,
    {
        out.push(copy_row(&src[i]));
        proof { assert(src@.take(i as int + 1) =~= src@.take(i as int).push(src@[i as int])); }
        i += 1;
    }
    proof { assert(src@.take(src@.len() as int) =~= src@); }
    out
}

/// Append a row to one cell (creating it).
pub fn cells_push(c: &mut Cells, fy: i64, uom: &Arc<Vec<u8>>, d: i64, a: &Arc<Vec<u8>>, row: Row)
    requires
        old(c).wf(),
        forall|k: KeyView| #[trigger] old(c).at(k) is Some ==> old(c).at(k)->0.wf(),
    ensures
        final(c).wf(),
        forall|k: KeyView| #[trigger] final(c).at(k) is Some ==> final(c).at(k)->0.wf(),
        ({
            let me = CellId { fy: fy as int, uom: (**uom)@, d: d as int, a: (**a)@ };
            let before = match cell_at(*old(c), me) { Some(v) => v@, None => Seq::empty() };
            &&& cell_at(*final(c), me) is Some
            &&& cell_at(*final(c), me)->0@ == before.push(row)
            &&& forall|id: CellId| id != me ==> #[trigger] cell_at(*final(c), id) == cell_at(*old(c), id)
        }),
{
    let ghost me = CellId { fy: fy as int, uom: (**uom)@, d: d as int, a: (**a)@ };
    let mut cell = match cells_get(c, fy, uom, d, a) {
        Some(v) => copy_cell(&*v),
        None => Vec::new(),
    };
    cell.push(row);
    cells_put(c, fy, uom, d, a, cell);
}


/// Σ g over the answer rows of filing a in cell (fy, uom, d).
pub open spec fn cell_w(db: Db, id: CellId, g: spec_fn(OutRow) -> int) -> int {
    sum(db.num, |n: NumRow| if picked(db.num, n, id.uom) && n.adsh == Some(id.a) && vkey(n.value->0, id.d) {
        pair_w(db.sub, n, id.fy, g)
    } else {
        0
    })
}

pub open spec fn cw(db: Db) -> Fam {
    |id: CellId, g: spec_fn(OutRow) -> int| cell_w(db, id, g)
}


pub proof fn lemma_vkey_iff(b: u64, d: int)
    requires
        f64_finite(b),
    ensures
        vkey(f64_val(b), d) == (f64_desc_key(b) == d),
{
    if vkey(f64_val(b), d) {
        let b2 = choose|b2: u64| f64_finite(b2) && f64_val(b2) == f64_val(b) && f64_desc_key(b2) == d;
        lemma_desc_key(b, b2);
    }
}

} // verus!
