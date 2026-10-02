// The aux-state store: every accelerator's state, kept for a window of
// recent database versions, behind one reader-writer lock.
//
// Model.  The store's versions are numbered 0, 1, 2, ... in install order;
// `timeline(v)` is the database version v answers for.  The timeline is
// ghost and fixed when the store is created: it is whatever the host's
// commits make it, and the host's scan / capture contracts are stated
// against it (world.rs).  The lock protects a `Cell`: a window of
// consecutive versions base, base+1, ..., newest, each with its world (one
// slot per accelerator).  The lock invariant (cell_ok) is
//
//     every kept version v:  world_v.ok(timeline(v))
//
// i.e. each version's states answer for the database at that version (or are
// invalid).  Because every access goes through the lock, each call sees the
// whole window at once: there are no interleavings to reason about, and each
// accelerator's proofs stay the sequential ones of its kernel.
//
// Engine epochs.  Each kept version also records the engine epoch from which
// it is current (`from`), and a commit in flight records the epoch from which
// it may be visible (`pending`).  A reader at snapshot epoch E (`resolve`)
// gets the newest version with from <= E, or waits while a pending commit may
// be in its snapshot; E = 0 means the newest, and a version from epoch 0 is
// current for every snapshot.  This is the C++ VersionedAuxStore's protocol
// (src/stateful/state).  The proofs say nothing about epochs: that version v
// is the one snapshot E sees is the host's contract, as is the order of the
// commits (world.rs).
//
// Concurrency.
//   * A reader resolves its snapshot and clones the state pointers (`Arc`)
//     under the read lock, and runs the query outside it.  Old snapshots
//     keep being answered after newer commits.
//   * A writer prepares the next version outside the lock from version e
//     (`Prepared`), then takes the write lock only to append it (`install`
//     / `commit`).  `begin` marks the commit pending first; it is also the
//     writer token: one commit at a time.  Aborted transactions drop their
//     `Prepared` and `abort`.
//   * Versions no open snapshot can select are pruned on each install (the
//     host passes the oldest snapshot epoch in use).
//
// This file is query- and accelerator-agnostic.  The lock is vstd's verified
// RwLock with fairer spinning (rwlock.rs); like it, nothing here proves that
// the lock is eventually released (no deadlock-freedom / liveness claim).
use vstd::prelude::*;
use crate::rwlock::*;
use std::sync::Arc;

verus! {

/// What a deployment's world (one slot per accelerator) must provide.
/// `D` is the model database type that the timeline ranges over.
pub trait AuxWorld<D>: Sized {
    /// Every state in the world answers for `db` (invalid slots always do).
    spec fn ok(self, db: D) -> bool;

    /// A world with every slot invalid: ok for every database.
    fn invalid() -> (w: Self)
        ensures
            forall|db: D| #[trigger] w.ok(db);

    /// A copy sharing the same immutable states (`Arc` clones).
    fn share(&self) -> (w: Self)
        ensures
            forall|db: D| self.ok(db) ==> #[trigger] w.ok(db);
}

/// One accelerator's state, or Invalid (queries then fall back to SQL).
pub enum Slot<S> {
    Invalid,
    Valid(Arc<S>),
}

/// The slot is invalid, or its state satisfies `p`.
pub open spec fn slot_ok<S>(slot: Slot<S>, p: spec_fn(S) -> bool) -> bool {
    match slot {
        Slot::Invalid => true,
        Slot::Valid(a) => p(*a),
    }
}

impl<S> Slot<S> {
    /// The slot's state, shared (an `Arc` clone), if it is valid.
    pub fn get(&self) -> (r: Option<Arc<S>>)
        ensures
            r matches Some(a) ==> (*self matches Slot::Valid(b) && *a == *b),
            *self matches Slot::Valid(_) ==> r is Some,
    {
        match self {
            Slot::Invalid => None,
            Slot::Valid(a) => Some(a.clone()),
        }
    }

    /// A copy of the slot sharing its state.
    pub fn share(&self) -> (r: Slot<S>)
        ensures
            forall|p: spec_fn(S) -> bool| slot_ok(*self, p) ==> #[trigger] slot_ok(r, p),
    {
        match self {
            Slot::Invalid => Slot::Invalid,
            Slot::Valid(a) => Slot::Valid(a.clone()),
        }
    }
}

/// Where a version is relative to the kept window.
pub enum Lookup<T> {
    Found(T),
    /// Older than every kept version (evicted).
    Evicted,
    /// Newer than the newest version (not installed yet).
    Future,
}

/// The value the lock protects: versions base, base + 1, ..., in order.
pub struct Cell<W> {
    pub base: u64,
    pub versions: Vec<W>,
    /// The engine epoch from which each kept version is current (host data).
    pub from: Vec<u64>,
    /// A commit in flight, visible from this engine epoch on (host data).
    pub pending: Option<u64>,
}

impl<W> Cell<W> {
    /// The newest kept version.
    pub open spec fn newest(self) -> int {
        self.base + self.versions.len() - 1
    }
}

/// The lock invariant: at least one version, version numbers fit in a u64,
/// one epoch per version, and every kept version's world is ok for the
/// timeline at that version.
pub open spec fn cell_ok<D, W: AuxWorld<D>>(c: Cell<W>, timeline: spec_fn(nat) -> D) -> bool {
    &&& c.versions.len() >= 1
    &&& c.newest() <= u64::MAX
    &&& c.from.len() == c.versions.len()
    &&& forall|i: int|
        0 <= i < c.versions.len() ==> #[trigger] c.versions@[i].ok(timeline((c.base + i) as nat))
}

impl<W> Cell<W> {
    /// The newest kept version.
    pub fn newest_epoch<D>(&self, Ghost(timeline): Ghost<spec_fn(nat) -> D>) -> (e: u64) where
        W: AuxWorld<D>,
        requires
            cell_ok(*self, timeline),
        ensures
            e == self.newest(),
    {
        self.base + (self.versions.len() - 1) as u64
    }

    /// Version v's world, if it is kept.
    pub fn at<D>(&self, v: u64, Ghost(timeline): Ghost<spec_fn(nat) -> D>) -> (r: Lookup<&W>) where
        W: AuxWorld<D>,
        requires
            cell_ok(*self, timeline),
        ensures
            r matches Lookup::Found(w) ==> w.ok(timeline(v as nat)),
            r is Evicted <==> v < self.base,
            r is Future <==> v > self.newest(),
    {
        if v < self.base {
            Lookup::Evicted
        } else if v > self.newest_epoch(Ghost(timeline)) {
            Lookup::Future
        } else {
            let i = (v - self.base) as usize;
            assert(self.versions@[i as int].ok(timeline((self.base + i) as nat)));
            Lookup::Found(&self.versions[i])
        }
    }

    /// Replace version v's world (v kept) with one ok for timeline(v).
    pub fn set<D>(&mut self, v: u64, w: W, Ghost(timeline): Ghost<spec_fn(nat) -> D>) where
        W: AuxWorld<D>,
        requires
            cell_ok(*old(self), timeline),
            old(self).base <= v <= old(self).newest(),
            w.ok(timeline(v as nat)),
        ensures
            cell_ok(*final(self), timeline),
            final(self).base == old(self).base,
            final(self).versions.len() == old(self).versions.len(),
            final(self).from == old(self).from,
            final(self).pending == old(self).pending,
    {
        let i = (v - self.base) as usize;
        self.versions[i] = w;
        assert forall|j: int| 0 <= j < self.versions.len() implies #[trigger] self.versions@[j].ok(
            timeline((self.base + j) as nat),
        ) by {
            if j != i {
                assert(self.versions@[j] == old(self).versions@[j]);
            }
        }
    }

    /// Append the version after the newest, current from engine epoch `from`.
    pub fn push<D>(&mut self, w: W, from: u64, Ghost(timeline): Ghost<spec_fn(nat) -> D>) where
        W: AuxWorld<D>,
        requires
            cell_ok(*old(self), timeline),
            old(self).newest() < u64::MAX,
            w.ok(timeline((old(self).newest() + 1) as nat)),
        ensures
            cell_ok(*final(self), timeline),
            final(self).base == old(self).base,
            final(self).newest() == old(self).newest() + 1,
            final(self).pending == old(self).pending,
    {
        self.versions.push(w);
        self.from.push(from);
        assert forall|j: int| 0 <= j < self.versions.len() implies #[trigger] self.versions@[j].ok(
            timeline((self.base + j) as nat),
        ) by {
            if j < old(self).versions.len() {
                assert(self.versions@[j] == old(self).versions@[j]);
            }
        }
    }

    /// Drop the versions older than min_v (the newest is always kept).
    pub fn evict<D>(&mut self, min_v: u64, Ghost(timeline): Ghost<spec_fn(nat) -> D>) where
        W: AuxWorld<D>,
        requires
            cell_ok(*old(self), timeline),
        ensures
            cell_ok(*final(self), timeline),
            final(self).newest() == old(self).newest(),
            final(self).pending == old(self).pending,
    {
        let newest = self.newest_epoch(Ghost(timeline));
        let keep_from = if min_v > newest {
            newest
        } else {
            min_v
        };
        if keep_from > self.base {
            let k = (keep_from - self.base) as usize;
            let ghost old_versions = self.versions@;
            let ghost old_base = self.base;
            let rest = self.versions.split_off(k);
            self.versions = rest;
            let rest_from = self.from.split_off(k);
            self.from = rest_from;
            self.base = keep_from;
            assert forall|j: int| 0 <= j < self.versions.len() implies #[trigger] self.versions@[j].ok(
                timeline((self.base + j) as nat),
            ) by {
                assert(self.versions@[j] == old_versions[k + j]);
                assert(old_versions[k + j].ok(timeline((old_base + (k + j)) as nat)));
            }
        }
    }

    /// Every kept version with every slot invalid (same window).
    pub fn invalidate<D>(&mut self, Ghost(timeline): Ghost<spec_fn(nat) -> D>) where
        W: AuxWorld<D>,
        requires
            cell_ok(*old(self), timeline),
        ensures
            cell_ok(*final(self), timeline),
            final(self).base == old(self).base,
            final(self).newest() == old(self).newest(),
            final(self).pending == old(self).pending,
    {
        let n = self.versions.len();
        let mut i: usize = 0;
        while i < n
            invariant
                0 <= i <= n,
                n == self.versions.len(),
                self.base == old(self).base,
                self.pending == old(self).pending,
                cell_ok(*self, timeline),
            decreases n - i,
        {
            let v = self.base + i as u64;
            self.set(v, W::invalid(), Ghost(timeline));
            i += 1;
        }
    }

    /// The kept version a snapshot at engine epoch `at` selects: the newest
    /// one current from at or before `at` (at = 0: the newest).  None if
    /// every kept version is newer than the snapshot.
    pub fn select<D>(&self, at: u64, Ghost(timeline): Ghost<spec_fn(nat) -> D>) -> (r: Option<u64>) where
        W: AuxWorld<D>,
        requires
            cell_ok(*self, timeline),
        ensures
            r matches Some(v) ==> self.base <= v <= self.newest(),
    {
        if at == 0 {
            return Some(self.newest_epoch(Ghost(timeline)));
        }
        let mut i = self.from.len();
        while i > 0
            invariant
                cell_ok(*self, timeline),
                i <= self.from.len(),
            decreases i,
        {
            if self.from[i - 1] <= at {
                return Some(self.base + (i - 1) as u64);
            }
            i -= 1;
        }
        None
    }

    /// Drop the versions no snapshot at or after engine epoch `floor` can
    /// select: keep the one a snapshot at `floor` selects, and every newer.
    pub fn prune<D>(&mut self, floor: u64, Ghost(timeline): Ghost<spec_fn(nat) -> D>) where
        W: AuxWorld<D>,
        requires
            cell_ok(*old(self), timeline),
        ensures
            cell_ok(*final(self), timeline),
            final(self).newest() == old(self).newest(),
            final(self).pending == old(self).pending,
    {
        let mut keep: usize = 0;
        let mut i: usize = 0;
        let n = self.from.len();
        while i < n
            invariant
                cell_ok(*self, timeline),
                n == self.from.len(),
                i <= n,
                keep < n,
            decreases n - i,
        {
            if self.from[i] <= floor {
                keep = i;
            }
            i += 1;
        }
        let min_v = self.base + keep as u64;
        self.evict(min_v, Ghost(timeline));
    }
}

/// A one-version cell: version e, with world w, current from engine epoch
/// `from`, nothing pending.
pub fn single<D, W: AuxWorld<D>>(e: u64, w: W, from: u64, Ghost(timeline): Ghost<spec_fn(nat) -> D>) -> (c: Cell<W>)
    requires
        w.ok(timeline(e as nat)),
    ensures
        cell_ok(c, timeline),
        c.base == e,
        c.newest() == e,
        c.pending is None,
{
    let mut versions = Vec::new();
    versions.push(w);
    let mut fv = Vec::new();
    fv.push(from);
    let c = Cell { base: e, versions, from: fv, pending: None };
    assert(c.versions@[0] == w);
    c
}

/// The lock invariant, as the lock's predicate.
pub ghost struct AtEpoch<D> {
    pub timeline: spec_fn(nat) -> D,
}

impl<D, W: AuxWorld<D>> RwLockPredicate<Cell<W>> for AtEpoch<D> {
    open spec fn inv(self, c: Cell<W>) -> bool {
        cell_ok(c, self.timeline)
    }
}

/// The next version's world, prepared from version e (outside the lock):
/// ok for timeline(e + 1).  Installed with `Store::install`, or dropped if
/// the host's transaction aborts.
pub struct Prepared<D, W: AuxWorld<D>> {
    e: u64,
    next: W,
    timeline: Ghost<spec_fn(nat) -> D>,
}

impl<D, W: AuxWorld<D>> Prepared<D, W> {
    #[verifier::type_invariant]
    spec fn wf(self) -> bool {
        &&& self.e < u64::MAX
        &&& self.next.ok((self.timeline@)((self.e + 1) as nat))
    }

    /// The version it was prepared from.
    pub closed spec fn epoch(self) -> u64 {
        self.e
    }

    pub closed spec fn timeline(self) -> spec_fn(nat) -> D {
        self.timeline@
    }

    pub fn new(e: u64, next: W, Ghost(timeline): Ghost<spec_fn(nat) -> D>) -> (p: Self)
        requires
            e < u64::MAX,
            next.ok(timeline((e + 1) as nat)),
        ensures
            p.epoch() == e,
            p.timeline() == timeline,
    {
        Prepared { e, next, timeline: Ghost(timeline) }
    }

    pub fn from_epoch(&self) -> (e: u64)
        ensures
            e == self.epoch(),
    {
        self.e
    }

    fn into_parts(self) -> (r: (u64, W))
        ensures
            r.0 == self.epoch(),
            r.0 < u64::MAX,
            r.1.ok((self.timeline())((r.0 + 1) as nat)),
    {
        proof {
            use_type_invariant(&self);
        }
        (self.e, self.next)
    }
}

/// What `install` did.
pub enum Install {
    /// Version e + 1 was appended.
    Applied,
    /// The newest version was not e (commits out of sequence): the store now
    /// holds only version e + 1, with every slot invalid.
    Stale,
}

/// What a snapshot sees (`resolve`).
pub enum Resolved<W> {
    /// Version v, with its world (shared).
    Found(u64, W),
    /// A commit the snapshot may include is in flight.
    Pending,
    /// Every kept version is newer than the snapshot.
    NotVisible,
}

/// The store.  Shared by all host threads (`&Store` is `Sync`).
pub struct Store<D, W: AuxWorld<D>> {
    lock: RwLock<Cell<W>, AtEpoch<D>>,
}

impl<D, W: AuxWorld<D>> Store<D, W> {
    /// The database at each version (ghost, fixed at creation).
    pub closed spec fn timeline(self) -> spec_fn(nat) -> D {
        self.lock.pred().timeline
    }

    /// The database at version e.
    pub open spec fn db_at(self, e: nat) -> D {
        (self.timeline())(e)
    }

    /// A store holding only version `epoch`, with `world`, current from
    /// engine epoch `from`.
    pub fn new(epoch: u64, world: W, from: u64, Ghost(timeline): Ghost<spec_fn(nat) -> D>) -> (s: Self)
        requires
            world.ok(timeline(epoch as nat)),
        ensures
            s.timeline() == timeline,
    {
        let ghost pred = AtEpoch::<D> { timeline };
        let c = single(epoch, world, from, Ghost(timeline));
        let lock = RwLock::new(c, Ghost(pred));
        Store { lock }
    }

    /// Read access to the window.  Release with `ReadGuard::end`.
    pub fn begin_read(&self) -> (g: ReadGuard<'_, D, W>)
        ensures
            g.store() == *self,
            cell_ok(g.cell(), self.timeline()),
    {
        let h = self.lock.acquire_read();
        ReadGuard { h, store: self }
    }

    /// Exclusive access to the window.  The caller must put back a window
    /// that satisfies the invariant (`WriteGuard::end`).
    pub fn begin_write(&self) -> (r: (Cell<W>, WriteGuard<'_, D, W>))
        ensures
            r.1.store() == *self,
            cell_ok(r.0, self.timeline()),
    {
        let (c, h) = self.lock.acquire_write();
        (c, WriteGuard { h, store: self })
    }

    /// The newest version.
    pub fn newest(&self) -> u64 {
        let g = self.begin_read();
        let e = g.borrow().newest_epoch(Ghost(self.timeline()));
        g.end();
        e
    }

    /// The oldest kept version.
    pub fn oldest(&self) -> u64 {
        let g = self.begin_read();
        let e = g.borrow().base;
        g.end();
        e
    }

    /// Wait until version v is installed (or was: it may be evicted), giving
    /// up after `tries` checks (each check backs off).  A snapshot one
    /// commit ahead of the store waits here for that commit's install.
    #[verifier::exec_allows_no_decreases_clause]
    pub fn wait_for(&self, v: u64, tries: u64) -> bool {
        let mut spins: u32 = 0;
        let mut n: u64 = 0;
        loop {
            if self.newest() >= v {
                return true;
            }
            if n >= tries {
                return false;
            }
            n += 1;
            backoff(&mut spins);
        }
    }

    /// Mark a commit in flight, visible from engine epoch `from` on: readers
    /// whose snapshot may include it wait (`resolve`) until `commit` or
    /// `abort`.  False if a commit is already in flight (one at a time).
    pub fn begin(&self, from: u64) -> bool {
        let (mut c, g) = self.begin_write();
        if c.pending.is_some() {
            g.end(c);
            false
        } else {
            c.pending = Some(from);
            g.end(c);
            true
        }
    }

    /// The commit in flight ended without a new version.
    pub fn abort(&self) {
        let (mut c, g) = self.begin_write();
        c.pending = None;
        g.end(c);
    }

    /// Append version e + 1, current from engine epoch `from`, if the newest
    /// version is e; then drop the versions no snapshot at or after `floor`
    /// selects.  The write lock is held only for the append.
    pub fn install(&self, p: Prepared<D, W>, from: u64, floor: u64) -> (r: Install)
        requires
            p.timeline() == self.timeline(),
    {
        self.append(p, from, floor, false)
    }

    /// `install`, ending the commit in flight (`begin`).
    pub fn commit(&self, p: Prepared<D, W>, from: u64, floor: u64) -> (r: Install)
        requires
            p.timeline() == self.timeline(),
    {
        self.append(p, from, floor, true)
    }

    fn append(&self, p: Prepared<D, W>, from: u64, floor: u64, end_pending: bool) -> (r: Install)
        requires
            p.timeline() == self.timeline(),
    {
        let (e, next) = p.into_parts();
        let (mut c, g) = self.begin_write();
        if end_pending {
            c.pending = None;
        }
        let newest = c.newest_epoch(Ghost(self.timeline()));
        if newest == e {
            // Epochs must increase along the window; a version from epoch 0
            // (every snapshot), or one not after the newest, replaces it.
            let last = c.from[c.from.len() - 1];
            let replaces = from == 0 || from <= last;
            c.push(next, from, Ghost(self.timeline()));
            if replaces {
                c.evict(e + 1, Ghost(self.timeline()));
            } else {
                c.prune(floor, Ghost(self.timeline()));
            }
            g.end(c);
            Install::Applied
        } else {
            let pending = c.pending;
            let mut s = single(e + 1, W::invalid(), from, Ghost(self.timeline()));
            s.pending = pending;
            g.end(s);
            Install::Stale
        }
    }

    /// The version a snapshot at engine epoch `at` sees (at = 0: the newest),
    /// with its world, shared.
    pub fn resolve(&self, at: u64) -> (r: Resolved<W>)
        ensures
            r matches Resolved::Found(v, w) ==> w.ok(self.db_at(v as nat)),
    {
        let g = self.begin_read();
        let c = g.borrow();
        let waits = match c.pending {
            Some(p) => at == 0 || at >= p,
            None => false,
        };
        let r = if waits {
            Resolved::Pending
        } else {
            match c.select(at, Ghost(self.timeline())) {
                None => Resolved::NotVisible,
                Some(v) => match c.at(v, Ghost(self.timeline())) {
                    Lookup::Found(w) => Resolved::Found(v, w.share()),
                    _ => Resolved::NotVisible,
                },
            }
        };
        g.end();
        r
    }

    /// `resolve`, waiting out a pending commit for up to `tries` checks
    /// (each check backs off); then Pending.
    #[verifier::exec_allows_no_decreases_clause]
    pub fn resolve_wait(&self, at: u64, tries: u64) -> (r: Resolved<W>)
        ensures
            r matches Resolved::Found(v, w) ==> w.ok(self.db_at(v as nat)),
    {
        let mut spins: u32 = 0;
        let mut n: u64 = 0;
        loop {
            let r = self.resolve(at);
            match r {
                Resolved::Pending => {},
                _ => return r,
            }
            if n >= tries {
                return r;
            }
            n += 1;
            backoff(&mut spins);
        }
    }

    /// The newest version, with its world (shared): what the next version is
    /// prepared from.
    pub fn newest_world(&self) -> (r: (u64, W))
        ensures
            r.1.ok(self.db_at(r.0 as nat)),
    {
        let g = self.begin_read();
        let c = g.borrow();
        let e = c.newest_epoch(Ghost(self.timeline()));
        let w = match c.at(e, Ghost(self.timeline())) {
            Lookup::Found(w) => w.share(),
            _ => W::invalid(),
        };
        g.end();
        (e, w)
    }

    /// How many versions are kept.
    pub fn version_count(&self) -> usize {
        let g = self.begin_read();
        let n = g.borrow().versions.len();
        g.end();
        n
    }

    /// Drop the versions older than min_v: the host passes the oldest
    /// version any open snapshot can still ask for.
    pub fn evict(&self, min_v: u64) {
        let (mut c, g) = self.begin_write();
        c.evict(min_v, Ghost(self.timeline()));
        g.end(c);
    }

    /// Invalidate every slot of every kept version (versions stay).
    pub fn invalidate_all(&self) {
        let (mut c, g) = self.begin_write();
        c.invalidate(Ghost(self.timeline()));
        g.end(c);
    }
}

/// Back off while waiting (spin hint, then yield).  No spec: it changes no
/// state.
#[verifier::external_body]
fn backoff(spins: &mut u32) {
    if *spins < 128 {
        *spins += 1;
        core::hint::spin_loop();
    } else {
        std::thread::yield_now();
    }
}

/// A held read lock.
pub struct ReadGuard<'a, D, W: AuxWorld<D>> {
    h: ReadHandle<'a, Cell<W>, AtEpoch<D>>,
    store: &'a Store<D, W>,
}

impl<'a, D, W: AuxWorld<D>> ReadGuard<'a, D, W> {
    #[verifier::type_invariant]
    spec fn wf(self) -> bool {
        &&& self.h.rwlock() == self.store.lock
        &&& self.store.lock.inv(self.h.view())
    }

    pub closed spec fn store(self) -> Store<D, W> {
        *self.store
    }

    pub closed spec fn cell(self) -> Cell<W> {
        self.h.view()
    }

    pub fn borrow(&self) -> (c: &Cell<W>)
        ensures
            *c == self.cell(),
            cell_ok(*c, self.store().timeline()),
    {
        proof {
            use_type_invariant(self);
        }
        self.h.borrow()
    }

    pub fn end(self) {
        self.h.release_read();
    }
}

/// A held write lock.
pub struct WriteGuard<'a, D, W: AuxWorld<D>> {
    h: WriteHandle<'a, Cell<W>, AtEpoch<D>>,
    store: &'a Store<D, W>,
}

impl<'a, D, W: AuxWorld<D>> WriteGuard<'a, D, W> {
    #[verifier::type_invariant]
    spec fn wf(self) -> bool {
        self.h.rwlock() == self.store.lock
    }

    pub closed spec fn store(self) -> Store<D, W> {
        *self.store
    }

    /// Put back a window that satisfies the invariant and release the lock.
    pub fn end(self, c: Cell<W>)
        requires
            cell_ok(c, self.store().timeline()),
    {
        proof {
            use_type_invariant(&self);
        }
        self.h.release_write(c);
    }
}

} // verus!
