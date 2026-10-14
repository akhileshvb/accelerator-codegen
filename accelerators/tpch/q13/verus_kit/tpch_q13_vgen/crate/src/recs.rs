// Per-customer records of 17 u16 fields (field 0: customer multiplicity; fields 1..=16: the
// number of the customer's orders NOT matching tracked pattern f-1), stored flat in copy-on-write
// pages.  With u = key + 2^31 (keys are SQL INTEGERs: -2^31 <= key < 2^31), the record of a key
// lives at top[u / 2^23] -> mid1[(u / 2^15) % 256] -> mid2[(u / 128) % 256] ->
// page.cells[(u % 128) * 17 ..][..17].  A missing node reads as zeros.
use vstd::prelude::*;
use std::sync::Arc;

verus! {

pub struct Page {
    pub cells: Vec<u16>,
}

pub struct Mid2 {
    pub pages: Vec<Option<Arc<Page>>>,
}

pub struct Mid1 {
    pub mids: Vec<Option<Arc<Mid2>>>,
}

pub struct Recs {
    top: Vec<Option<Arc<Mid1>>>,
    // records of keys outside the tree's range (not SQL INTEGERs), a short list
    far_keys: Vec<i64>,
    far_cells: Vec<u16>,
}

pub open spec fn page_ok(p: Option<Arc<Page>>) -> bool {
    match p { Some(a) => (*a).cells@.len() == 2176, None => true }
}

pub open spec fn page_cell(p: Option<Arc<Page>>, i: int) -> int {
    match p {
        Some(a) => if 0 <= i < (*a).cells@.len() { (*a).cells@[i] as int } else { 0 },
        None => 0,
    }
}

pub open spec fn mid2_ok(m: Option<Arc<Mid2>>) -> bool {
    match m {
        Some(a) => (*a).pages@.len() == 256
            && forall|i: int| 0 <= i < 256 ==> page_ok(#[trigger] (*a).pages@[i]),
        None => true,
    }
}

pub open spec fn mid2_page(m: Option<Arc<Mid2>>, i: int) -> Option<Arc<Page>> {
    match m {
        Some(a) => if 0 <= i < (*a).pages@.len() { (*a).pages@[i] } else { None },
        None => None,
    }
}

pub open spec fn mid1_ok(m: Option<Arc<Mid1>>) -> bool {
    match m {
        Some(a) => (*a).mids@.len() == 256
            && forall|i: int| 0 <= i < 256 ==> mid2_ok(#[trigger] (*a).mids@[i]),
        None => true,
    }
}

pub open spec fn mid1_mid(m: Option<Arc<Mid1>>, i: int) -> Option<Arc<Mid2>> {
    match m {
        Some(a) => if 0 <= i < (*a).mids@.len() { (*a).mids@[i] } else { None },
        None => None,
    }
}

/// The keys the tree holds (SQL INTEGERs); other keys go to the short far list.
pub open spec fn tree_ok(key: int) -> bool {
    -2147483648 <= key < 2147483648
}

/// A representable key (any i64).
pub open spec fn key_ok(key: int) -> bool {
    i64::MIN <= key <= i64::MAX
}

pub open spec fn far_find(keys: Seq<i64>, key: int) -> bool {
    exists|i: int| 0 <= i < keys.len() && #[trigger] keys[i] as int == key
}

pub open spec fn far_cell(keys: Seq<i64>, cells: Seq<u16>, key: int, f: int) -> int {
    if 0 <= f < 17 && far_find(keys, key) {
        let i = choose|i: int| 0 <= i < keys.len() && #[trigger] keys[i] as int == key;
        if 0 <= i * 17 + f < cells.len() { cells[i * 17 + f] as int } else { 0 }
    } else { 0 }
}

pub open spec fn far_wf(keys: Seq<i64>, cells: Seq<u16>) -> bool {
    &&& cells.len() == keys.len() * 17
    &&& keys.len() <= 4096
    &&& forall|i: int, j: int| 0 <= i < j < keys.len() ==> #[trigger] keys[i] != #[trigger] keys[j]
    &&& forall|i: int| 0 <= i < keys.len() ==> !tree_ok(#[trigger] keys[i] as int)
}

fn far_index(keys: &Vec<i64>, key: i64) -> (r: Option<usize>)
    ensures
        r matches Some(i) ==> i < keys.len() && keys@[i as int] == key,
        r is None ==> forall|i: int| 0 <= i < keys.len() ==> keys@[i] != key,
{
    let mut i = 0usize;
    while i < keys.len()
        invariant i <= keys.len(), forall|j: int| 0 <= j < i ==> keys@[j] != key,
        decreases keys.len() - i,
    {
        if keys[i] == key { return Some(i); }
        i += 1;
    }
    None
}

proof fn lemma_far_cell_at(keys: Seq<i64>, cells: Seq<u16>, i: int, f: int)
    requires far_wf(keys, cells), 0 <= i < keys.len(), 0 <= f < 17,
    ensures far_cell(keys, cells, keys[i] as int, f) == cells[i * 17 + f] as int,
{
    assert(far_find(keys, keys[i] as int));
    let j = choose|j: int| 0 <= j < keys.len() && #[trigger] keys[j] as int == keys[i] as int;
    if j != i {
        if j < i { assert(keys[j] != keys[i]); } else { assert(keys[i] != keys[j]); }
    }
    assert(0 <= i * 17 + f < cells.len()) by (nonlinear_arith)
        requires 0 <= i < keys.len(), 0 <= f < 17, cells.len() == keys.len() * 17;
}

impl Recs {
    pub closed spec fn wf(self) -> bool {
        &&& self.top@.len() == 512
        &&& forall|t: int| 0 <= t < 512 ==> mid1_ok(#[trigger] self.top@[t])
        &&& far_wf(self.far_keys@, self.far_cells@)
    }

    /// Field f of the record of `key`; zero outside the representable range.
    pub closed spec fn cell(self, key: int, f: int) -> int {
        if !tree_ok(key) { far_cell(self.far_keys@, self.far_cells@, key, f) }
        else if 0 <= f < 17 && (key + 2147483648) / 8388608 < self.top@.len() {
            page_cell(
                mid2_page(
                    mid1_mid(self.top@[(key + 2147483648) / 8388608], ((key + 2147483648) / 32768) % 256),
                    ((key + 2147483648) / 128) % 256),
                ((key + 2147483648) % 128) * 17 + f)
        } else { 0 }
    }

    pub fn empty() -> (r: Recs)
        ensures r.wf(), forall|key: int, f: int| #[trigger] r.cell(key, f) == 0,
    {
        proof { assert forall|key: int| !far_find(Seq::<i64>::empty(), key) by {} }
        let mut top: Vec<Option<Arc<Mid1>>> = Vec::new();
        let mut i = 0usize;
        while i < 512
            invariant i <= 512, top@.len() == i as int,
                forall|t: int| 0 <= t < i ==> (#[trigger] top@[t]) is None,
            decreases 512 - i,
        {
            top.push(None);
            i += 1;
        }
        Recs { top, far_keys: Vec::new(), far_cells: Vec::new() }
    }

    pub fn get(&self, key: i64, f: usize) -> (r: u16)
        requires self.wf(), f < 17,
        ensures r as int == self.cell(key as int, f as int),
    {
        if key < -2147483648 || key >= 2147483648 {
            return match far_index(&self.far_keys, key) {
                Some(i) => {
                    proof { lemma_far_cell_at(self.far_keys@, self.far_cells@, i as int, f as int); }
                    self.far_cells[i * 17 + f]
                },
                None => {
                    proof { assert(!far_find(self.far_keys@, key as int)); }
                    0
                },
            };
        }
        let u = (key + 2147483648i64) as usize;
        let t = u / 8388608;
        let m1 = (u / 32768) % 256;
        let m2 = (u / 128) % 256;
        let c = (u % 128) * 17 + f;
        match &self.top[t] {
            None => 0,
            Some(a) => match &a.mids[m1] {
                None => 0,
                Some(b) => match &b.pages[m2] {
                    None => 0,
                    Some(page) => page.cells[c],
                },
            },
        }
    }

    /// A second handle on the same contents (shares every node below the top).
    pub fn dup(&self) -> (r: Recs)
        requires self.wf(),
        ensures r.wf(), forall|key: int, f: int| #[trigger] r.cell(key, f) == self.cell(key, f),
    {
        let mut top: Vec<Option<Arc<Mid1>>> = Vec::new();
        let mut i = 0usize;
        while i < 512
            invariant i <= 512, top@.len() == i as int, self.wf(),
                forall|t: int| 0 <= t < i ==> (#[trigger] top@[t]) == self.top@[t],
            decreases 512 - i,
        {
            let e = match &self.top[i] { None => None, Some(a) => Some(a.clone()) };
            top.push(e);
            i += 1;
        }
        let far_keys = self.far_keys.clone();
        let far_cells = self.far_cells.clone();
        proof {
            assert(far_keys@ =~= self.far_keys@);
            assert(far_cells@ =~= self.far_cells@);
        }
        let r = Recs { top, far_keys, far_cells };
        proof {
            assert forall|t: int| 0 <= t < 512 implies mid1_ok(#[trigger] r.top@[t]) by {
                assert(r.top@[t] == self.top@[t]);
            }
            assert forall|key: int, f: int| #[trigger] r.cell(key, f) == self.cell(key, f) by {
                if tree_ok(key) && 0 <= f < 17 {
                    assert((key + 2147483648) / 8388608 < 512);
                    assert(r.top@[(key + 2147483648) / 8388608] == self.top@[(key + 2147483648) / 8388608]);
                }
            }
        }
        r
    }

    /// Replace the record of `key` by `rec` (copying one page and one node per level).  Fails
    /// (false) only for a key outside the tree's range when the far list is full.
    pub fn put(&mut self, key: i64, rec: &[u16]) -> (ok: bool)
        requires old(self).wf(), rec@.len() == 17,
        ensures ok ==> final(self).wf(),
            ok ==> forall|k: int, f: int| #[trigger] final(self).cell(k, f)
                == if k == key as int && 0 <= f < 17 { rec@[f] as int } else { old(self).cell(k, f) },
    {
        if key < -2147483648 || key >= 2147483648 { return self.put_far(key, rec); }
        let u = (key + 2147483648i64) as usize;
        let t = u / 8388608;
        let m1 = (u / 32768) % 256;
        let m2 = (u / 128) % 256;
        let c = (u % 128) * 17;
        let ghost before = *old(self);
        let ghost old_mid1 = before.top@[t as int];
        let ghost old_mid2 = mid1_mid(old_mid1, m1 as int);
        let ghost old_page = mid2_page(old_mid2, m2 as int);
        // the new page
        let mut cells: Vec<u16> = Vec::new();
        {
            let mut i = 0usize;
            while i < 2176
                invariant i <= 2176, cells@.len() == i as int, self.wf(), t < 512, m1 < 256, m2 < 256,
                    self.top@ == before.top@, old_page == mid2_page(mid1_mid(self.top@[t as int], m1 as int), m2 as int),
                    forall|j: int| 0 <= j < i ==> (#[trigger] cells@[j]) as int == page_cell(old_page, j),
                decreases 2176 - i,
            {
                let v: u16 = match &self.top[t] {
                    None => 0,
                    Some(a) => match &a.mids[m1] {
                        None => 0,
                        Some(b) => match &b.pages[m2] {
                            None => 0,
                            Some(page) => page.cells[i],
                        },
                    },
                };
                cells.push(v);
                i += 1;
            }
        }
        let ghost cells_old = cells@;
        let mut f = 0usize;
        while f < 17
            invariant f <= 17, cells@.len() == 2176, c == (u % 128) * 17, c + 17 <= 2176,
                rec@.len() == 17, cells_old.len() == 2176,
                forall|j: int| 0 <= j < 2176 ==> (#[trigger] cells@[j]) == if c <= j < c + f { rec@[j - c as int] } else { cells_old[j] },
            decreases 17 - f,
        {
            cells.set(c + f, rec[f]);
            f += 1;
        }
        // the new level-2 node
        let mut pages: Vec<Option<Arc<Page>>> = Vec::new();
        {
            let mut i = 0usize;
            while i < 256
                invariant i <= 256, pages@.len() == i as int, self.wf(), t < 512, m1 < 256, m2 < 256,
                    self.top@ == before.top@, old_mid2 == mid1_mid(self.top@[t as int], m1 as int),
                    forall|j: int| 0 <= j < i ==> (#[trigger] pages@[j]) == if j == m2 as int { None }
                        else { mid2_page(old_mid2, j) },
                decreases 256 - i,
            {
                let e: Option<Arc<Page>> = if i == m2 { None } else {
                    match &self.top[t] {
                        None => None,
                        Some(a) => match &a.mids[m1] {
                            None => None,
                            Some(b) => match &b.pages[i] { None => None, Some(p) => Some(p.clone()) },
                        },
                    }
                };
                pages.push(e);
                i += 1;
            }
        }
        pages.set(m2, Some(Arc::new(Page { cells })));
        let mid2 = Arc::new(Mid2 { pages });
        // the new level-1 node
        let mut mids: Vec<Option<Arc<Mid2>>> = Vec::new();
        {
            let mut i = 0usize;
            while i < 256
                invariant i <= 256, mids@.len() == i as int, self.wf(), t < 512, m1 < 256,
                    self.top@ == before.top@, old_mid1 == self.top@[t as int],
                    forall|j: int| 0 <= j < i ==> (#[trigger] mids@[j]) == if j == m1 as int { None }
                        else { mid1_mid(old_mid1, j) },
                decreases 256 - i,
            {
                let e: Option<Arc<Mid2>> = if i == m1 { None } else {
                    match &self.top[t] {
                        None => None,
                        Some(a) => match &a.mids[i] { None => None, Some(b) => Some(b.clone()) },
                    }
                };
                mids.push(e);
                i += 1;
            }
        }
        mids.set(m1, Some(mid2));
        let mid1 = Arc::new(Mid1 { mids });
        self.top.set(t, Some(mid1));
        proof {
            assert forall|j: int| 0 <= j < 256 implies page_ok(#[trigger] (*mid2).pages@[j]) by {
                if j != m2 as int { assert(page_ok(mid2_page(old_mid2, j))); }
            }
            assert forall|j: int| 0 <= j < 256 implies mid2_ok(#[trigger] (*mid1).mids@[j]) by {
                if j != m1 as int { assert(mid2_ok(mid1_mid(old_mid1, j))); }
            }
            assert forall|tt: int| 0 <= tt < 512 implies mid1_ok(#[trigger] self.top@[tt]) by {
                if tt != t as int { assert(self.top@[tt] == before.top@[tt]); }
            }
            assert forall|k: int, ff: int| #[trigger] self.cell(k, ff)
                == if k == key as int && 0 <= ff < 17 { rec@[ff] as int } else { before.cell(k, ff) } by {
                if tree_ok(k) && 0 <= ff < 17 {
                    let uk = k + 2147483648;
                    assert(uk / 8388608 < 512);
                    if uk / 8388608 == t as int && (uk / 32768) % 256 == m1 as int && (uk / 128) % 256 == m2 as int {
                        if k == key as int {
                            assert((uk % 128) * 17 + ff == c + ff);
                        } else {
                            assert(uk % 128 != u as int % 128);
                            assert((uk % 128) * 17 + ff < c || (uk % 128) * 17 + ff >= c + 17);
                        }
                    }
                }
            }
        }
        true
    }

    fn put_far(&mut self, key: i64, rec: &[u16]) -> (ok: bool)
        requires old(self).wf(), !tree_ok(key as int), rec@.len() == 17,
        ensures ok ==> final(self).wf(),
            ok ==> forall|k: int, f: int| #[trigger] final(self).cell(k, f)
                == if k == key as int && 0 <= f < 17 { rec@[f] as int } else { old(self).cell(k, f) },
    {
        let ghost before = *old(self);
        let ghost keys0 = self.far_keys@;
        let ghost cells0 = self.far_cells@;
        match far_index(&self.far_keys, key) {
            Some(i) => {
                let mut cells = self.far_cells.clone();
                proof { assert(cells@ =~= cells0); }
                let mut f = 0usize;
                while f < 17
                    invariant f <= 17, cells@.len() == cells0.len(), rec@.len() == 17,
                        i < keys0.len(), cells0.len() == keys0.len() * 17, keys0.len() <= 4096,
                        forall|j: int| 0 <= j < cells0.len() ==> #[trigger] cells@[j]
                            == if i * 17 <= j < i * 17 + f { rec@[j - i * 17] } else { cells0[j] },
                    decreases 17 - f,
                {
                    proof { assert(i * 17 + f < cells0.len()) by (nonlinear_arith)
                        requires i < keys0.len(), f < 17, cells0.len() == keys0.len() * 17; }
                    cells.set(i * 17 + f, rec[f]);
                    f += 1;
                }
                self.far_cells = cells;
                proof {
                    assert(far_wf(keys0, self.far_cells@));
                    assert forall|k: int, ff: int| #[trigger] self.cell(k, ff)
                        == if k == key as int && 0 <= ff < 17 { rec@[ff] as int } else { before.cell(k, ff) } by {
                        if !tree_ok(k) && 0 <= ff < 17 {
                            if k == key as int {
                                lemma_far_cell_at(keys0, self.far_cells@, i as int, ff);
                                assert(self.far_cells@[i * 17 + ff] == rec@[ff]);
                            } else if far_find(keys0, k) {
                                let j = choose|j: int| 0 <= j < keys0.len() && #[trigger] keys0[j] as int == k;
                                lemma_far_cell_at(keys0, cells0, j, ff);
                                lemma_far_cell_at(keys0, self.far_cells@, j, ff);
                                assert(j != i as int);
                                assert(j * 17 + ff < i * 17 || j * 17 + ff >= i * 17 + 17) by (nonlinear_arith)
                                    requires j != i as int, 0 <= ff < 17, 0 <= j, 0 <= i;
                            }
                        }
                    }
                }
                true
            },
            None => {
                if self.far_keys.len() >= 4096 { return false; }
                let mut keys = self.far_keys.clone();
                let mut cells = self.far_cells.clone();
                proof { assert(keys@ =~= keys0); assert(cells@ =~= cells0); }
                keys.push(key);
                let mut f = 0usize;
                while f < 17
                    invariant f <= 17, cells@.len() == cells0.len() + f, rec@.len() == 17,
                        cells@.take(cells0.len() as int) =~= cells0,
                        forall|j: int| 0 <= j < f ==> #[trigger] cells@[cells0.len() + j] == rec@[j],
                    decreases 17 - f,
                {
                    cells.push(rec[f]);
                    f += 1;
                    proof {
                        assert(cells@.take(cells0.len() as int) =~= cells0);
                    }
                }
                self.far_keys = keys;
                self.far_cells = cells;
                let ghost n = keys0.len();
                proof {
                    assert forall|i: int| 0 <= i < self.far_keys@.len() implies !tree_ok(#[trigger] self.far_keys@[i] as int) by {
                        if i < n as int { assert(self.far_keys@[i] == keys0[i]); }
                    }
                    assert forall|i: int, j: int| 0 <= i < j < self.far_keys@.len() implies #[trigger] self.far_keys@[i] != #[trigger] self.far_keys@[j] by {
                        if j == n as int {
                            assert(keys0[i] != key);
                        } else { assert(self.far_keys@[i] == keys0[i] && self.far_keys@[j] == keys0[j]); }
                    }
                    assert(far_wf(self.far_keys@, self.far_cells@));
                    assert forall|k: int, ff: int| #[trigger] self.cell(k, ff)
                        == if k == key as int && 0 <= ff < 17 { rec@[ff] as int } else { before.cell(k, ff) } by {
                        if !tree_ok(k) && 0 <= ff < 17 {
                            if k == key as int {
                                lemma_far_cell_at(self.far_keys@, self.far_cells@, n as int, ff);
                                assert(self.far_cells@[n * 17 + ff] == rec@[ff]);
                            } else if far_find(keys0, k) {
                                let j = choose|j: int| 0 <= j < keys0.len() && #[trigger] keys0[j] as int == k;
                                lemma_far_cell_at(keys0, cells0, j, ff);
                                lemma_far_cell_at(self.far_keys@, self.far_cells@, j, ff);
                                assert(self.far_keys@[j] == keys0[j]);
                                assert(j * 17 + ff < n * 17) by (nonlinear_arith)
                                    requires j < n, 0 <= ff < 17, 0 <= j;
                                assert(self.far_cells@[j * 17 + ff] == cells0[j * 17 + ff]);
                            } else {
                                assert(!far_find(self.far_keys@, k)) by {
                                    if far_find(self.far_keys@, k) {
                                        let j = choose|j: int| 0 <= j < self.far_keys@.len() && #[trigger] self.far_keys@[j] as int == k;
                                        if j < n as int { assert(keys0[j] as int == k); assert(far_find(keys0, k)); }
                                    }
                                }
                            }
                        }
                    }
                }
                true
            },
        }
    }
}

/// What slot `key0` (a multiple of 128) of a table frozen from `flat` (k records) holds.
pub open spec fn good_slot(flat: Seq<u16>, k: int, key0: int, p: Option<Arc<Page>>) -> bool {
    if key0 < 0 || key0 >= k { p is None } else {
        &&& p is Some
        &&& page_ok(p)
        &&& forall|ci: int| 0 <= ci < 2176 ==> page_cell(p, ci)
            == (if key0 * 17 + ci < k * 17 { flat[key0 * 17 + ci] as int } else { 0int })
    }
}

pub open spec fn good_mid2(flat: Seq<u16>, k: int, base: int, m: Option<Arc<Mid2>>) -> bool {
    if base + 32768 <= 0 || base >= k { m is None } else {
        &&& m is Some
        &&& mid2_ok(m)
        &&& forall|mm: int| 0 <= mm < 256 ==> good_slot(flat, k, base + mm * 128, #[trigger] mid2_page(m, mm))
    }
}

pub open spec fn good_mid1(flat: Seq<u16>, k: int, base: int, m: Option<Arc<Mid1>>) -> bool {
    if base + 8388608 <= 0 || base >= k { m is None } else {
        &&& m is Some
        &&& mid1_ok(m)
        &&& forall|mm: int| 0 <= mm < 256 ==> good_mid2(flat, k, base + mm * 32768, #[trigger] mid1_mid(m, mm))
    }
}

impl Recs {
    /// Freeze a flat record array (records of keys 0..k, 17 fields each) into pages.
    pub fn from_flat(flat: &Vec<u16>, k: usize) -> (r: Recs)
        requires k <= 67108864, flat@.len() == k * 17,
        ensures r.wf(),
            forall|key: int, f: int| #[trigger] r.cell(key, f)
                == if 0 <= key < k && 0 <= f < 17 { flat@[key * 17 + f] as int } else { 0int },
    {
        let mut top: Vec<Option<Arc<Mid1>>> = Vec::new();
        let mut t = 0usize;
        while t < 512
            invariant t <= 512, top@.len() == t as int, k <= 67108864, flat@.len() == k * 17,
                forall|tt: int| 0 <= tt < t ==> good_mid1(flat@, k as int, tt * 8388608 - 2147483648, #[trigger] top@[tt]),
            decreases 512 - t,
        {
            let u1: u64 = t as u64 * 8388608;
            if u1 + 8388608 <= 2147483648 || u1 >= 2147483648 + k as u64 {
                top.push(None);
            } else {
                let base1 = u1 as i64 - 2147483648i64;
                let mut mids: Vec<Option<Arc<Mid2>>> = Vec::new();
                let mut m1 = 0usize;
                while m1 < 256
                    invariant m1 <= 256, mids@.len() == m1 as int, k <= 67108864, flat@.len() == k * 17,
                        t < 512, u1 == t * 8388608, base1 == u1 as int - 2147483648,
                        forall|mm: int| 0 <= mm < m1 ==> good_mid2(flat@, k as int, base1 + mm * 32768, #[trigger] mids@[mm]),
                    decreases 256 - m1,
                {
                    let u2: u64 = u1 + m1 as u64 * 32768;
                    if u2 + 32768 <= 2147483648 || u2 >= 2147483648 + k as u64 {
                        mids.push(None);
                    } else {
                        let base2 = u2 as i64 - 2147483648i64;
                        let mut pages: Vec<Option<Arc<Page>>> = Vec::new();
                        let mut m2 = 0usize;
                        while m2 < 256
                            invariant m2 <= 256, pages@.len() == m2 as int, k <= 67108864, flat@.len() == k * 17,
                                m1 < 256, t < 512, u2 == t * 8388608 + m1 * 32768, base2 == u2 as int - 2147483648,
                                forall|mm: int| 0 <= mm < m2 ==> good_slot(flat@, k as int, base2 + mm * 128, #[trigger] pages@[mm]),
                            decreases 256 - m2,
                        {
                            let u3: u64 = u2 + m2 as u64 * 128;
                            if u3 < 2147483648 || u3 >= 2147483648 + k as u64 {
                                pages.push(None);
                            } else {
                                let key0 = (u3 - 2147483648) as usize;
                                let mut cells: Vec<u16> = Vec::new();
                                let mut ci = 0usize;
                                while ci < 2176
                                    invariant ci <= 2176, cells@.len() == ci as int, key0 < k, k <= 67108864,
                                        flat@.len() == k * 17, key0 as int == base2 + m2 * 128, m2 < 256,
                                        forall|j: int| 0 <= j < ci ==> (#[trigger] cells@[j]) as int
                                            == (if key0 * 17 + j < k * 17 { flat@[key0 * 17 + j] as int } else { 0int }),
                                    decreases 2176 - ci,
                                {
                                    let idx = key0 * 17 + ci;
                                    let v: u16 = if idx < k * 17 { flat[idx] } else { 0 };
                                    cells.push(v);
                                    ci += 1;
                                }
                                pages.push(Some(Arc::new(Page { cells })));
                            }
                            m2 += 1;
                        }
                        mids.push(Some(Arc::new(Mid2 { pages })));
                    }
                    m1 += 1;
                }
                top.push(Some(Arc::new(Mid1 { mids })));
            }
            t += 1;
        }
        let r = Recs { top, far_keys: Vec::new(), far_cells: Vec::new() };
        proof {
            assert forall|tt: int| 0 <= tt < 512 implies mid1_ok(#[trigger] r.top@[tt]) by {
                assert(good_mid1(flat@, k as int, tt * 8388608 - 2147483648, r.top@[tt]));
            }
            assert forall|key: int, f: int| #[trigger] r.cell(key, f)
                == if 0 <= key < k && 0 <= f < 17 { flat@[key * 17 + f] as int } else { 0int } by {
                if tree_ok(key) && 0 <= f < 17 {
                    let uk = key + 2147483648;
                    let tt = uk / 8388608;
                    let m1 = (uk / 32768) % 256;
                    let m2 = (uk / 128) % 256;
                    assert(0 <= tt < 512);
                    let base1 = tt * 8388608 - 2147483648;
                    assert(good_mid1(flat@, k as int, base1, r.top@[tt]));
                    if !(base1 + 8388608 <= 0 || base1 >= k) {
                        let base2 = base1 + m1 * 32768;
                        assert(good_mid2(flat@, k as int, base2, mid1_mid(r.top@[tt], m1)));
                        if !(base2 + 32768 <= 0 || base2 >= k) {
                            let key0 = base2 + m2 * 128;
                            assert(good_slot(flat@, k as int, key0, mid2_page(mid1_mid(r.top@[tt], m1), m2)));
                            assert(key0 == key - key % 128);
                        }
                    }
                }
            }
        }
        r
    }
}

} // verus!
