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
// (substrate/aux/state).
//
// Proved about epochs (`resolve_seen`): epochs increase with version numbers
// (lock invariant), and what a snapshot at E gets is decided on the window as
// it is under the read lock: Found(v) only if v is the newest kept version
// current at E (`sees`) and no commit in flight may be visible at E; Pending
// if one may be; NotVisible if every kept version is newer than E.
//
// Lemma 5.2 (extended).  A ghost log records the `from` of every install ever
// made (never truncated); the window's `from` is its suffix (`window_ok`), so
// v is the newest of ALL installs with from <= E, pruned ones included
// (`newest_installed`).  `append` keeps the log non-decreasing: an install
// earlier than the last one (a stale epoch behind a later install) keeps only
// an invalid placeholder; a build is refused while a commit is in flight.
//
// Lemma 5.3 (`lemma_snapshot`, via `Reading::world`).  Against the host's
// ghost engine model (`Engine`: snap(E), the database a snapshot sees, and
// stamp(E), when the newest relevant commit visible at E became visible), the
// version a snapshot at E != 0 gets holds snap(E), given the commit-order
// contract: (i) installs in commit order (enforced by `append`, above);
// (ii) each install is the database a snapshot at its `from` sees
// (`visible_from`, TRUSTED `requires` of install / commit / new); (iii) the
// newest relevant commit visible at E is installed or marked pending when the
// read happens (`read_instant`, TRUSTED, a fact about the engine at that
// instant, so a predicate, not an invariant).  Nothing else is assumed.
//
// Still trusted (world.rs, the FFI): the engine model and the clauses above;
// that the state a writer prepares answers for the database at its version
// (the capture and scan contracts); `floor` is at most the oldest open
// snapshot.  With one store for every accelerator (a world: one slot each,
// world.rs `Both`), one install per commit makes the same hold for all.
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
    /// The `from` of every version ever installed, in install order; `from`
    /// is always its suffix (ghost).
    pub log: Ghost<Seq<u64>>,
    /// Per kept version: its world is the database a snapshot at its `from`
    /// sees (false for the invalid placeholder of an out-of-order install).
    pub exact: Ghost<Seq<bool>>,
}

impl<W> Cell<W> {
    /// The newest kept version.
    pub open spec fn newest(self) -> int {
        self.base + self.versions.len() - 1
    }

    /// What a snapshot's selection depends on (ghost).
    pub open spec fn window(self) -> Window {
        Window { base: self.base as int, from: self.from@, pending: self.pending, log: self.log@, exact: self.exact@ }
    }
}

/// A window's version numbers, the engine epoch from which each kept version
/// is current, and the commit in flight: what decides which version a
/// snapshot sees.
pub ghost struct Window {
    pub base: int,
    pub from: Seq<u64>,
    pub pending: Option<u64>,
    pub log: Seq<u64>,
    pub exact: Seq<bool>,
}

impl Window {
    pub open spec fn newest(self) -> int {
        self.base + self.from.len() - 1
    }
}

/// Install epochs never go backwards.
pub open spec fn nondecreasing(log: Seq<u64>) -> bool {
    forall|i: int, j: int| 0 <= i <= j < log.len() ==> #[trigger] log[i] <= #[trigger] log[j]
}

/// The window's `from` is the suffix of the install log, one `exact` per version.
pub open spec fn window_ok(w: Window) -> bool {
    &&& w.exact.len() == w.from.len()
    &&& w.from.len() <= w.log.len()
    &&& nondecreasing(w.log)
    &&& forall|i: int| 0 <= i < w.from.len() ==> #[trigger] w.from[i] == w.log[w.log.len() - w.from.len() + i]
}

/// Log position of kept version v.
pub open spec fn log_index(w: Window, v: int) -> int {
    w.log.len() - w.from.len() + (v - w.base)
}

/// LEMMA 5.2, EXTENDED.  Log position k is the newest install current at
/// `at`: its `from` is at most `at` and every later install's is after it
/// (at = 0: the last install).  Pruned versions are included.
pub open spec fn newest_installed(log: Seq<u64>, at: u64, k: int) -> bool {
    &&& 0 <= k < log.len()
    &&& if at == 0 {
        k == log.len() - 1
    } else {
        &&& log[k] <= at
        &&& forall|j: int| k < j < log.len() ==> #[trigger] log[j] > at
    }
}

/// The host's ghost model of the engine: what a snapshot sees, and when
/// relevant commits become visible.
pub ghost struct Engine<D> {
    /// The database a snapshot at engine epoch `at` sees (as far as any
    /// accelerator depends on it).
    pub snap: spec_fn(u64) -> D,
    /// The epoch at which the newest relevant commit visible to a snapshot at
    /// `at` became visible.
    pub stamp: spec_fn(u64) -> u64,
}

impl<D> Engine<D> {
    /// Well-formed: the stamp is a monotone projection below its argument and
    /// snapshots between two relevant commits see the same database.
    pub open spec fn wf(self) -> bool {
        &&& forall|a: u64| #[trigger] (self.stamp)(a) <= a
        &&& forall|a: u64, b: u64| a <= b ==> #[trigger] (self.stamp)(a) <= #[trigger] (self.stamp)(b)
        &&& forall|a: u64| #[trigger] (self.stamp)((self.stamp)(a)) == (self.stamp)(a)
        &&& forall|a: u64| #[trigger] (self.snap)(a) == (self.snap)((self.stamp)(a))
    }
}

/// COMMIT-ORDER CONTRACT (iii), first half (TRUSTED, host-supplied; a fact
/// about the engine at the instant of a read).  The newest relevant commit
/// visible to the snapshot at `at` has been installed.
pub open spec fn commit_installed<D>(w: Window, eng: Engine<D>, at: u64) -> bool {
    exists|k: int| 0 <= k < w.log.len() && #[trigger] w.log[k] == (eng.stamp)(at)
}

/// COMMIT-ORDER CONTRACT (iii), second half (TRUSTED, same instant).  Or it
/// is marked pending, from an epoch not after it became visible.
pub open spec fn commit_marked<D>(w: Window, eng: Engine<D>, at: u64) -> bool {
    w.pending matches Some(p) && p <= (eng.stamp)(at)
}

/// THE READ-INSTANT PREDICATE (TRUSTED).  A snapshot at `at` != 0 sees no
/// relevant commit that is neither installed nor marked pending.
pub open spec fn read_instant<D>(w: Window, eng: Engine<D>, at: u64) -> bool {
    &&& at != 0
    &&& (commit_installed(w, eng, at) || commit_marked(w, eng, at))
}

/// LEMMA 5.3.  The version a snapshot at `at` is given (`sees`, newest
/// install current at `at`) holds the database that snapshot sees, if it was
/// installed for the database at its `from` (`exact`) and the read-instant
/// predicate holds.  Uses (i): installs have non-decreasing `from`
/// (`window_ok`, enforced by `append`).
pub proof fn lemma_snapshot<D>(eng: Engine<D>, w: Window, at: u64, v: int)
    requires
        eng.wf(),
        window_ok(w),
        sees(w, at, v),
        newest_installed(w.log, at, log_index(w, v)),
        read_instant(w, eng, at),
    ensures
        (eng.snap)(at) == (eng.snap)(w.from[v - w.base]),
{
    let k = log_index(w, v);
    let s = (eng.stamp)(at);
    let f = w.from[v - w.base];
    assert(f == w.log[k]);
    if commit_installed(w, eng, at) {
        let j = choose|j: int| 0 <= j < w.log.len() && #[trigger] w.log[j] == s;
        assert(s <= at);
        assert(j <= k);
        assert(w.log[j] <= w.log[k]);
    } else {
        let p = w.pending.unwrap();
        assert(s <= at);
        assert(false);
    }
    assert((eng.stamp)(f) <= s);
    assert((eng.stamp)(s) <= (eng.stamp)(f));
}

/// Engine epochs increase with the version number.
pub open spec fn increasing(from: Seq<u64>) -> bool {
    forall|i: int, j: int| 0 <= i < j < from.len() ==> #[trigger] from[i] < #[trigger] from[j]
}

/// No commit in flight may be visible at `at` (at = 0: none in flight).
pub open spec fn none_in_flight(w: Window, at: u64) -> bool {
    match w.pending {
        Some(p) => at != 0 && p > at,
        None => true,
    }
}

/// THE SNAPSHOT RULE.  Version v is the one a snapshot at engine epoch `at`
/// sees in window w: the newest kept version current at `at` (no kept version
/// after it is), and no commit in flight may be visible at `at`.  at = 0
/// (no snapshot): the newest version, nothing in flight.  Epochs increase
/// with version numbers and nothing is newer than the window's newest, so v
/// is the newest version, of all installed, current at `at`.
pub open spec fn sees(w: Window, at: u64, v: int) -> bool {
    &&& w.base <= v <= w.newest()
    &&& none_in_flight(w, at)
    &&& if at == 0 {
        v == w.newest()
    } else {
        &&& w.from[v - w.base] <= at
        &&& forall|i: int| v - w.base < i < w.from.len() ==> #[trigger] w.from[i] > at
    }
}

/// A commit in flight may be visible at `at`: the snapshot must wait.
pub open spec fn in_flight(w: Window, at: u64) -> bool {
    match w.pending {
        Some(p) => at == 0 || p <= at,
        None => false,
    }
}

/// Every kept version is newer than a snapshot at `at` (and nothing in
/// flight is visible): the version it should see was evicted.
pub open spec fn none_current(w: Window, at: u64) -> bool {
    &&& at != 0
    &&& none_in_flight(w, at)
    &&& forall|i: int| 0 <= i < w.from.len() ==> #[trigger] w.from[i] > at
}

/// The lock invariant: at least one version, version numbers fit in a u64,
/// one epoch per version, and every kept version's world is ok for the
/// timeline at that version.
pub open spec fn cell_ok<D, W: AuxWorld<D>>(c: Cell<W>, timeline: spec_fn(nat) -> D) -> bool {
    &&& c.versions.len() >= 1
    &&& c.newest() <= u64::MAX
    &&& c.from.len() == c.versions.len()
    &&& increasing(c.from@)
    &&& window_ok(c.window())
    &&& forall|i: int|
        0 <= i < c.versions.len() ==> #[trigger] c.versions@[i].ok(timeline((c.base + i) as nat))
}

/// Every `exact` version holds the database a snapshot at its `from` sees.
pub open spec fn exact_ok<D, W>(c: Cell<W>, timeline: spec_fn(nat) -> D, eng: Engine<D>) -> bool {
    forall|i: int|
        0 <= i < c.from@.len() && c.exact@[i] ==> #[trigger] timeline((c.base + i) as nat) == (eng.snap)(c.from@[i])
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
            final(self).log == old(self).log,
            final(self).exact == old(self).exact,
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
    pub fn push<D>(&mut self, w: W, from: u64, Ghost(timeline): Ghost<spec_fn(nat) -> D>, Ghost(eng): Ghost<Engine<D>>) where
        W: AuxWorld<D>,
        requires
            cell_ok(*old(self), timeline),
            exact_ok(*old(self), timeline, eng),
            old(self).newest() < u64::MAX,
            w.ok(timeline((old(self).newest() + 1) as nat)),
            timeline((old(self).newest() + 1) as nat) == (eng.snap)(from),
            old(self).from@.last() < from,
        ensures
            cell_ok(*final(self), timeline),
            exact_ok(*final(self), timeline, eng),
            final(self).base == old(self).base,
            final(self).newest() == old(self).newest() + 1,
            final(self).pending == old(self).pending,
            final(self).log@ == old(self).log@.push(from),
    {
        let ghost old_from = self.from@;
        let ghost old_log = self.log@;
        let ghost old_exact = self.exact@;
        self.versions.push(w);
        self.from.push(from);
        self.log = Ghost(old_log.push(from));
        self.exact = Ghost(old_exact.push(true));
        assert(old_log.last() == old_from.last());
        assert(window_ok(self.window())) by {
            let w = self.window();
            assert forall|i: int, j: int| 0 <= i <= j < w.log.len() implies #[trigger] w.log[i] <= #[trigger] w.log[j] by {
                if j == old_log.len() {
                    if i < j {
                        assert(old_log[i] <= old_log[old_log.len() - 1]);
                    }
                }
            }
            assert forall|i: int| 0 <= i < w.from.len() implies #[trigger] w.from[i] == w.log[w.log.len() - w.from.len() + i] by {
                if i < old_from.len() {
                    assert(old_from[i] == old_log[old_log.len() - old_from.len() + i]);
                }
            }
        }
        assert(self.from@ == old_from.push(from));
        assert forall|i: int, j: int| 0 <= i < j < self.from@.len() implies #[trigger] self.from@[i] < #[trigger] self.from@[j] by {
            if j == old_from.len() {
                if i < old_from.len() - 1 {
                    assert(old_from[i] < old_from[old_from.len() - 1]);
                }
            }
        }
        assert forall|j: int| 0 <= j < self.versions.len() implies #[trigger] self.versions@[j].ok(
            timeline((self.base + j) as nat),
        ) by {
            if j < old(self).versions.len() {
                assert(self.versions@[j] == old(self).versions@[j]);
            }
        }
        assert forall|i: int| 0 <= i < self.from@.len() && self.exact@[i] implies #[trigger] timeline((self.base + i) as nat)
            == (eng.snap)(self.from@[i]) by {
            if i < old_from.len() {
                assert(old_exact[i] == self.exact@[i]);
            }
        }
    }

    /// Drop the versions older than min_v (the newest is always kept).
    pub fn evict<D>(&mut self, min_v: u64, Ghost(timeline): Ghost<spec_fn(nat) -> D>, Ghost(eng): Ghost<Engine<D>>) where
        W: AuxWorld<D>,
        requires
            cell_ok(*old(self), timeline),
            exact_ok(*old(self), timeline, eng),
        ensures
            cell_ok(*final(self), timeline),
            exact_ok(*final(self), timeline, eng),
            final(self).newest() == old(self).newest(),
            final(self).pending == old(self).pending,
            final(self).log == old(self).log,
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
            let ghost old_from = self.from@;
            let rest_from = self.from.split_off(k);
            self.from = rest_from;
            self.base = keep_from;
            let ghost old_exact = self.exact@;
            self.exact = Ghost(old_exact.subrange(k as int, old_exact.len() as int));
            assert(self.from@ == old_from.subrange(k as int, old_from.len() as int));
            assert forall|i: int| 0 <= i < self.from@.len() implies #[trigger] self.from@[i]
                == self.log@[self.log@.len() - self.from@.len() + i] by {
                assert(self.from@[i] == old_from[k + i]);
            }
            assert forall|i: int| 0 <= i < self.from@.len() && self.exact@[i] implies #[trigger] timeline((self.base + i) as nat)
                == (eng.snap)(self.from@[i]) by {
                assert(self.from@[i] == old_from[k + i]);
                assert(old_exact[k + i] == self.exact@[i]);
                assert(old(self).base + (k + i) == self.base + i);
                assert(timeline((old(self).base + (k + i)) as nat) == (eng.snap)(old_from[k + i]));
            }
            assert forall|i: int, j: int| 0 <= i < j < self.from@.len() implies #[trigger] self.from@[i] < #[trigger] self.from@[j] by {
                assert(self.from@[i] == old_from[k + i]);
                assert(self.from@[j] == old_from[k + j]);
            }
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
            final(self).from == old(self).from,
            final(self).log == old(self).log,
            final(self).exact == old(self).exact,
    {
        let n = self.versions.len();
        let mut i: usize = 0;
        while i < n
            invariant
                0 <= i <= n,
                n == self.versions.len(),
                self.base == old(self).base,
                self.pending == old(self).pending,
                self.from == old(self).from,
                self.log == old(self).log,
                self.exact == old(self).exact,
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
            r matches Some(v) ==> {
                &&& self.base <= v <= self.newest()
                &&& if at == 0 {
                    v == self.newest()
                } else {
                    &&& self.from@[v - self.base] <= at
                    &&& forall|i: int| v - self.base < i < self.from@.len() ==> #[trigger] self.from@[i] > at
                }
            },
            r is None ==> at != 0 && forall|i: int| 0 <= i < self.from@.len() ==> #[trigger] self.from@[i] > at,
    {
        if at == 0 {
            return Some(self.newest_epoch(Ghost(timeline)));
        }
        let mut i = self.from.len();
        while i > 0
            invariant
                cell_ok(*self, timeline),
                at != 0,
                i <= self.from.len(),
                forall|j: int| i <= j < self.from@.len() ==> #[trigger] self.from@[j] > at,
            decreases i,
        {
            if self.from[i - 1] <= at {
                let v = self.base + (i - 1) as u64;
                assert(v - self.base == i - 1);
                assert(self.from@[v - self.base] <= at);
                assert forall|k: int| v - self.base < k < self.from@.len() implies #[trigger] self.from@[k] > at by {
                    assert(i <= k);
                }
                return Some(v);
            }
            i -= 1;
        }
        None
    }

    /// Drop the versions no snapshot at or after engine epoch `floor` can
    /// select: keep the one a snapshot at `floor` selects, and every newer.
    pub fn prune<D>(&mut self, floor: u64, Ghost(timeline): Ghost<spec_fn(nat) -> D>, Ghost(eng): Ghost<Engine<D>>) where
        W: AuxWorld<D>,
        requires
            cell_ok(*old(self), timeline),
            exact_ok(*old(self), timeline, eng),
        ensures
            cell_ok(*final(self), timeline),
            exact_ok(*final(self), timeline, eng),
            final(self).newest() == old(self).newest(),
            final(self).pending == old(self).pending,
            final(self).log == old(self).log,
    {
        let mut keep: usize = 0;
        let mut i: usize = 0;
        let n = self.from.len();
        while i < n
            invariant
                cell_ok(*self, timeline),
                exact_ok(*self, timeline, eng),
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
        self.evict(min_v, Ghost(timeline), Ghost(eng));
    }
}

/// A one-version cell: version e, with world w, current from engine epoch
/// `from`, nothing pending, after the installs `prior`.
pub fn single<D, W: AuxWorld<D>>(
    e: u64,
    w: W,
    from: u64,
    Ghost(prior): Ghost<Seq<u64>>,
    Ghost(exact): Ghost<bool>,
    Ghost(timeline): Ghost<spec_fn(nat) -> D>,
    Ghost(eng): Ghost<Engine<D>>,
) -> (c: Cell<W>)
    requires
        w.ok(timeline(e as nat)),
        nondecreasing(prior),
        prior.len() > 0 ==> prior.last() <= from,
        exact ==> timeline(e as nat) == (eng.snap)(from),
    ensures
        cell_ok(c, timeline),
        exact_ok(c, timeline, eng),
        c.base == e,
        c.newest() == e,
        c.pending is None,
        c.log@ == prior.push(from),
        c.from@ == seq![from],
        c.exact@ == seq![exact],
{
    let mut versions = Vec::new();
    versions.push(w);
    let mut fv = Vec::new();
    fv.push(from);
    let ghost log = prior.push(from);
    let ghost ex = seq![exact];
    let c = Cell { base: e, versions, from: fv, pending: None, log: Ghost(log), exact: Ghost(ex) };
    assert(c.versions@[0] == w);
    assert(nondecreasing(prior.push(from))) by {
        assert forall|i: int, j: int| 0 <= i <= j < prior.push(from).len() implies prior.push(from)[i] <= prior.push(from)[j] by {
            if j == prior.len() && i < j {
                assert(prior[i] <= prior[prior.len() - 1]);
            }
        }
    }
    c
}

/// The lock invariant, as the lock's predicate.
pub ghost struct AtEpoch<D> {
    pub timeline: spec_fn(nat) -> D,
    pub engine: Engine<D>,
}

impl<D, W: AuxWorld<D>> RwLockPredicate<Cell<W>> for AtEpoch<D> {
    open spec fn inv(self, c: Cell<W>) -> bool {
        &&& cell_ok(c, self.timeline)
        &&& exact_ok(c, self.timeline, self.engine)
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
    /// Not appended as prepared: the newest version was not e, or the install
    /// is earlier in engine epochs than the last one (commits out of order),
    /// and the store now holds only version e + 1, with every slot invalid;
    /// or a build was refused while a commit is in flight (store unchanged).
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

/// What a read of version v at `at` in window w established.
pub open spec fn found_ok<D>(w: Window, timeline: spec_fn(nat) -> D, eng: Engine<D>, at: u64, v: int) -> bool {
    &&& window_ok(w)
    &&& sees(w, at, v)
    &&& newest_installed(w.log, at, log_index(w, v))
    &&& w.exact[v - w.base] ==> timeline(v as nat) == (eng.snap)(w.from[v - w.base])
}

proof fn lemma_newest_installed(w: Window, at: u64, v: int)
    requires
        window_ok(w),
        sees(w, at, v),
    ensures
        newest_installed(w.log, at, log_index(w, v)),
{
    let k = log_index(w, v);
    assert(w.from[v - w.base] == w.log[k]);
    if at != 0 {
        assert forall|j: int| k < j < w.log.len() implies #[trigger] w.log[j] > at by {
            let i = j - (w.log.len() - w.from.len());
            assert(w.from[i] == w.log[j]);
        }
    }
}

/// A version a snapshot was given, with its world (shared): the one handle
/// through which a host reads, carrying what the store proved about it.
pub struct Reading<D, W: AuxWorld<D>> {
    v: u64,
    at: u64,
    world: W,
    timeline: Ghost<spec_fn(nat) -> D>,
    engine: Ghost<Engine<D>>,
    window: Ghost<Window>,
}

impl<D, W: AuxWorld<D>> Reading<D, W> {
    #[verifier::type_invariant]
    spec fn wf(self) -> bool {
        &&& self.world.ok((self.timeline@)(self.v as nat))
        &&& self.engine@.wf()
        &&& found_ok(self.window@, self.timeline@, self.engine@, self.at, self.v as int)
    }

    /// The store version read.
    pub closed spec fn version(self) -> u64 {
        self.v
    }

    /// The engine epoch of the snapshot (0: the newest, no snapshot).
    pub closed spec fn at(self) -> u64 {
        self.at
    }

    /// The database the version answers for: timeline(version).
    pub closed spec fn db(self) -> D {
        (self.timeline@)(self.v as nat)
    }

    /// The database the snapshot sees, per the engine model.
    pub closed spec fn snapshot_db(self) -> D {
        (self.engine@.snap)(self.at)
    }

    /// The host's contract holds for this read: a snapshot (at != 0), the
    /// version was installed for its epoch (not an out-of-order placeholder),
    /// and `read_instant` held when it was read.
    pub closed spec fn contract(self) -> bool {
        &&& self.at != 0
        &&& self.window@.exact[self.v - self.window@.base]
        &&& read_instant(self.window@, self.engine@, self.at)
    }

    /// LEMMA 5.2 EXTENDED: the version is the newest install current at `at`,
    /// pruned ones included.
    pub closed spec fn newest(self) -> bool {
        newest_installed(self.window@.log, self.at, log_index(self.window@, self.v as int))
    }

    /// The world: it answers for the database at its version, and, if the
    /// contract holds, for the snapshot's database.  LEMMA 5.3.
    pub fn world(&self) -> (w: &W)
        ensures
            w.ok(self.db()),
            self.newest(),
            self.contract() ==> self.db() == self.snapshot_db(),
    {
        proof {
            use_type_invariant(self);
            let (w, eng, at, v) = (self.window@, self.engine@, self.at, self.v as int);
            if self.contract() {
                lemma_snapshot(eng, w, at, v);
            }
        }
        &self.world
    }
}

impl<D, W: AuxWorld<D>> Reading<D, W> {
    /// The same read narrowed to one accelerator's view: `proj` maps the
    /// database to the accelerator's, `w2` is its part of the world.
    pub fn narrow<D2, W2: AuxWorld<D2>>(self, Ghost(proj): Ghost<spec_fn(D) -> D2>, w2: W2) -> (r: Reading<D2, W2>)
        requires
            w2.ok(proj(self.db())),
        ensures
            r.version() == self.version(),
            r.at() == self.at(),
            r.db() == proj(self.db()),
            r.snapshot_db() == proj(self.snapshot_db()),
            r.contract() == self.contract(),
            r.newest() == self.newest(),
    {
        proof {
            use_type_invariant(&self);
        }
        let ghost tl = self.timeline@;
        let ghost eng = self.engine@;
        let ghost tl2 = |n: nat| proj(tl(n));
        let ghost eng2 = Engine::<D2> { snap: |a: u64| proj((eng.snap)(a)), stamp: eng.stamp };
        proof {
            assert(eng2.wf()) by {
                assert forall|a: u64| #[trigger] (eng2.snap)(a) == (eng2.snap)((eng2.stamp)(a)) by {
                    assert((eng.snap)(a) == (eng.snap)((eng.stamp)(a)));
                }
            }
        }
        Reading { v: self.v, at: self.at, world: w2, timeline: Ghost(tl2), engine: Ghost(eng2), window: self.window }
    }
}

/// What reading at an epoch gave.
pub enum Read<D, W: AuxWorld<D>> {
    Found(Reading<D, W>),
    /// A commit the snapshot may include is still in flight.
    Pending,
    /// Every kept version is newer than the snapshot.
    NotVisible,
}

/// The store.  Shared by all host threads (`&Store` is `Sync`).
pub struct Store<D, W: AuxWorld<D>> {
    lock: RwLock<Cell<W>, AtEpoch<D>>,
}

impl<D, W: AuxWorld<D>> Store<D, W> {
    #[verifier::type_invariant]
    spec fn wf(self) -> bool {
        self.engine().wf()
    }

    /// The database at each version (ghost, fixed at creation).
    pub closed spec fn timeline(self) -> spec_fn(nat) -> D {
        self.lock.pred().timeline
    }

    /// The host's engine model (ghost, fixed at creation).
    pub closed spec fn engine(self) -> Engine<D> {
        self.lock.pred().engine
    }

    /// The database a snapshot at engine epoch `at` sees.
    pub open spec fn snap_db(self, at: u64) -> D {
        (self.engine().snap)(at)
    }

    /// COMMIT-ORDER CONTRACT (ii) (TRUSTED, `requires` of `install` /
    /// `commit`): the version prepared by `p` is the database a snapshot at
    /// `from` sees, `from` being the epoch its commit becomes visible (a
    /// build: the epoch of its scan).  (i), that installs are in commit
    /// order, is enforced by `append`; (iii) is `read_instant`.
    pub open spec fn visible_from(self, p: Prepared<D, W>, from: u64) -> bool {
        self.db_at((p.epoch() + 1) as nat) == self.snap_db(from)
    }

    /// The database at version e.
    pub open spec fn db_at(self, e: nat) -> D {
        (self.timeline())(e)
    }

    /// A store holding only version `epoch`, with `world`, current from
    /// engine epoch `from` (the database a snapshot there sees).
    pub fn new(epoch: u64, world: W, from: u64, Ghost(timeline): Ghost<spec_fn(nat) -> D>, Ghost(engine): Ghost<Engine<D>>) -> (s: Self)
        requires
            world.ok(timeline(epoch as nat)),
            engine.wf(),
            timeline(epoch as nat) == (engine.snap)(from),
        ensures
            s.timeline() == timeline,
            s.engine() == engine,
    {
        let ghost pred = AtEpoch::<D> { timeline, engine };
        let c = single(epoch, world, from, Ghost(Seq::empty()), Ghost(true), Ghost(timeline), Ghost(engine));
        let lock = RwLock::new(c, Ghost(pred));
        Store { lock }
    }

    /// Read access to the window.  Release with `ReadGuard::end`.
    pub fn begin_read(&self) -> (g: ReadGuard<'_, D, W>)
        ensures
            g.store() == *self,
            cell_ok(g.cell(), self.timeline()),
            exact_ok(g.cell(), self.timeline(), self.engine()),
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
            exact_ok(r.0, self.timeline(), self.engine()),
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
            self.visible_from(p, from),
    {
        self.append(p, from, floor, false)
    }

    /// `install`, ending the commit in flight (`begin`).
    pub fn commit(&self, p: Prepared<D, W>, from: u64, floor: u64) -> (r: Install)
        requires
            p.timeline() == self.timeline(),
            self.visible_from(p, from),
    {
        self.append(p, from, floor, true)
    }

    fn append(&self, p: Prepared<D, W>, from: u64, floor: u64, end_pending: bool) -> (r: Install)
        requires
            p.timeline() == self.timeline(),
            self.visible_from(p, from),
    {
        let ghost eng = self.engine();
        let (e, next) = p.into_parts();
        let (mut c, g) = self.begin_write();
        if !end_pending && c.pending.is_some() {
            // a build is not serialized by `begin`: it would take the version
            // number the commit in flight is about to install
            g.end(c);
            return Install::Stale;
        }
        if end_pending {
            c.pending = None;
        }
        let newest = c.newest_epoch(Ghost(self.timeline()));
        let last = c.from[c.from.len() - 1];
        let pending = c.pending;
        let ghost prior = c.log@;
        assert(prior.last() == last);
        if from < last {
            // Out of order (a stale epoch behind a later install): no version
            // may claim an epoch it was not built for; only version e + 1 is
            // kept, invalid, from the last epoch.
            let mut s = single(e + 1, W::invalid(), last, Ghost(prior), Ghost(false), Ghost(self.timeline()), Ghost(eng));
            s.pending = pending;
            g.end(s);
            Install::Stale
        } else if newest == e {
            if from == last {
                // the same epoch as the newest: it replaces it, and only
                // version e + 1 is kept (what evicting every older one leaves)
                let mut s = single(e + 1, next, from, Ghost(prior), Ghost(true), Ghost(self.timeline()), Ghost(eng));
                s.pending = pending;
                g.end(s);
            } else {
                c.push(next, from, Ghost(self.timeline()), Ghost(eng));
                c.prune(floor, Ghost(self.timeline()), Ghost(eng));
                g.end(c);
            }
            Install::Applied
        } else {
            let mut s = single(e + 1, W::invalid(), from, Ghost(prior), Ghost(true), Ghost(self.timeline()), Ghost(eng));
            s.pending = pending;
            g.end(s);
            Install::Stale
        }
    }

    /// The version a snapshot at engine epoch `at` sees (at = 0: the newest),
    /// with its world, shared; and the window it was decided on (ghost: the
    /// store's window at the instant the read lock was held).
    ///
    /// SNAPSHOT THEOREM.  Found(v, w): v is the version the snapshot rule
    /// (`sees`) selects in that window, and w answers for the database at v.
    /// Pending: a commit in flight may be visible at `at`.  NotVisible: every
    /// kept version is newer than the snapshot (its version was evicted).
    pub fn resolve_seen(&self, at: u64) -> (r: (Resolved<W>, Ghost<Window>))
        ensures
            increasing(r.1@.from),
            r.0 matches Resolved::Found(v, w) ==> w.ok(self.db_at(v as nat))
                && found_ok(r.1@, self.timeline(), self.engine(), at, v as int),
            r.0 is Pending ==> in_flight(r.1@, at),
            r.0 is NotVisible ==> none_current(r.1@, at),
    {
        let g = self.begin_read();
        let c = g.borrow();
        let ghost win = c.window();
        assert(window_ok(win));
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
                    Lookup::Found(w) => {
                        proof {
                            assert(sees(win, at, v as int));
                            lemma_newest_installed(win, at, v as int);
                            let i = v - win.base;
                            assert(0 <= i < c.from@.len());
                            assert((c.base + i) as nat == v as nat);
                            assert(c.exact@[i] ==> self.timeline()((c.base + i) as nat) == (self.engine().snap)(c.from@[i]));
                            assert(win.exact[i] ==> self.db_at(v as nat) == self.snap_db(win.from[i]));
                        }
                        Resolved::Found(v, w.share())
                    },
                    Lookup::Evicted => {
                        assert(false);
                        Resolved::NotVisible
                    },
                    Lookup::Future => {
                        assert(false);
                        Resolved::NotVisible
                    },
                },
            }
        };
        g.end();
        (r, Ghost(win))
    }

    /// `resolve_seen` without the window.
    pub fn resolve(&self, at: u64) -> (r: Resolved<W>)
        ensures
            r matches Resolved::Found(v, w) ==> w.ok(self.db_at(v as nat)),
    {
        self.resolve_seen(at).0
    }

    /// `resolve_seen`, waiting out a pending commit for up to `tries` checks
    /// (each check backs off); then Pending.
    #[verifier::exec_allows_no_decreases_clause]
    pub fn resolve_wait_seen(&self, at: u64, tries: u64) -> (r: (Resolved<W>, Ghost<Window>))
        ensures
            increasing(r.1@.from),
            r.0 matches Resolved::Found(v, w) ==> w.ok(self.db_at(v as nat))
                && found_ok(r.1@, self.timeline(), self.engine(), at, v as int),
            r.0 is Pending ==> in_flight(r.1@, at),
            r.0 is NotVisible ==> none_current(r.1@, at),
    {
        let mut spins: u32 = 0;
        let mut n: u64 = 0;
        loop {
            let r = self.resolve_seen(at);
            match r.0 {
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

    /// `resolve_wait_seen` without the window.
    #[verifier::exec_allows_no_decreases_clause]
    pub fn resolve_wait(&self, at: u64, tries: u64) -> (r: Resolved<W>)
        ensures
            r matches Resolved::Found(v, w) ==> w.ok(self.db_at(v as nat)),
    {
        self.resolve_wait_seen(at, tries).0
    }

    /// `resolve_wait_seen`, packaged as a `Reading`.
    #[verifier::exec_allows_no_decreases_clause]
    pub fn read(&self, at: u64, tries: u64) -> (r: Read<D, W>)
        ensures
            r matches Read::Found(g) ==> g.db() == self.db_at(g.version() as nat) && g.at() == at
                && g.snapshot_db() == self.snap_db(at),
    {
        proof {
            use_type_invariant(self);
        }
        let (res, win) = self.resolve_wait_seen(at, tries);
        match res {
            Resolved::Found(v, world) => Read::Found(Reading {
                v,
                at,
                world,
                timeline: Ghost(self.timeline()),
                engine: Ghost(self.engine()),
                window: win,
            }),
            Resolved::Pending => Read::Pending,
            Resolved::NotVisible => Read::NotVisible,
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
        c.evict(min_v, Ghost(self.timeline()), Ghost(self.engine()));
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
            exact_ok(*c, self.store().timeline(), self.store().engine()),
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
            exact_ok(c, self.store().timeline(), self.store().engine()),
    {
        proof {
            use_type_invariant(&self);
        }
        self.h.release_write(c);
    }
}

} // verus!
