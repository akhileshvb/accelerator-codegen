// One accelerator's state as the store's world: the query-agnostic adapter
// between a verified kernel and the store (store.rs).
//
// A kernel state says which database it answers for (`AccelState::answers`,
// e.g. `s.db() == db`).  Its world is a `Slot`: valid with a state, or
// invalid (queries then fall back to SQL).  A writer prepares the next
// version from `Base`, the newest version's state, which carries the proof
// that it answers for the database at its version; `prepare` turns the
// kernel's result for the next database into a `Prepared` for the store.
//
// Host contract (TRUSTED, stated at each query's prepare functions): the
// inputs a writer prepares version e + 1 from (the commit's captured changes,
// or a build's scans) are those of the database the store's timeline has at
// e + 1, and the host installs the commits in the engine's commit order
// (store.rs states the contract).  The store checks that version e is still
// the newest at install; it cannot check that the host's changes are those of
// the next commit.
use vstd::prelude::*;
use std::sync::Arc;
use crate::store::*;

verus! {

/// A kernel state and the database it answers for.
pub trait AccelState<D>: Sized {
    spec fn answers(self, db: D) -> bool;
}

impl<D, S: AccelState<D>> AuxWorld<D> for Slot<S> {
    open spec fn ok(self, db: D) -> bool {
        match self {
            Slot::Invalid => true,
            Slot::Valid(a) => (*a).answers(db),
        }
    }

    fn invalid() -> (w: Self) {
        Slot::Invalid
    }

    fn share(&self) -> (w: Self) {
        match self {
            Slot::Invalid => Slot::Invalid,
            Slot::Valid(a) => Slot::Valid(a.clone()),
        }
    }
}

impl<D, S: AccelState<D>> Reading<D, Slot<S>> {
    /// The slot's state, if valid: it answers for the database at the
    /// version read, and, if the host's contract holds, for the snapshot's.
    pub fn state(&self) -> (r: Option<&S>)
        ensures
            r matches Some(s) ==> (*s).answers(self.db()) && (self.contract() ==> (*s).answers(self.snapshot_db())),
    {
        match self.world() {
            Slot::Valid(a) => Some(&**a),
            Slot::Invalid => None,
        }
    }
}

/// Two worlds side by side, for nested pairs of their databases: a
/// deployment's world is `Both<Slot<S1>, Both<Slot<S2>, ... Nothing>>`, one
/// slot per accelerator, over `(D1, (D2, ... ()))`, each accelerator's
/// projection of the one database.  One store holds all of them: one lock,
/// one version per commit, every slot answering for the same database version.
pub struct Both<A, B> {
    pub first: A,
    pub rest: B,
}

impl<DA, DB, A: AuxWorld<DA>, B: AuxWorld<DB>> AuxWorld<(DA, DB)> for Both<A, B> {
    open spec fn ok(self, db: (DA, DB)) -> bool {
        self.first.ok(db.0) && self.rest.ok(db.1)
    }

    fn invalid() -> (w: Self) {
        Both { first: A::invalid(), rest: B::invalid() }
    }

    fn share(&self) -> (w: Self) {
        Both { first: self.first.share(), rest: self.rest.share() }
    }
}

/// The end of a world's slots.
pub struct Nothing;

impl AuxWorld<()> for Nothing {
    open spec fn ok(self, db: ()) -> bool {
        true
    }

    fn invalid() -> (w: Self) {
        Nothing
    }

    fn share(&self) -> (w: Self) {
        Nothing
    }
}

/// The newest version e of a store, as a writer sees it: the version and the
/// timeline, with an identity.  Handles cloned from one head are `same`; that
/// is how a staged state is tied to the batch assembled from the same base.
pub struct Head<D> {
    inner: Arc<HeadData<D>>,
}

struct HeadData<D> {
    e: u64,
    timeline: Ghost<spec_fn(nat) -> D>,
}

impl<D> Head<D> {
    #[verifier::type_invariant]
    spec fn wf(self) -> bool {
        (*self.inner).e < u64::MAX
    }

    /// The version it is.
    pub closed spec fn epoch(self) -> u64 {
        (*self.inner).e
    }

    pub closed spec fn timeline(self) -> spec_fn(nat) -> D {
        (*self.inner).timeline@
    }

    /// The database at the next version.
    pub open spec fn next_db(self) -> D {
        (self.timeline())((self.epoch() + 1) as nat)
    }

    pub fn new(e: u64, Ghost(timeline): Ghost<spec_fn(nat) -> D>) -> (h: Self)
        requires
            e < u64::MAX,
        ensures
            h.epoch() == e,
            h.timeline() == timeline,
    {
        Head { inner: Arc::new(HeadData { e, timeline: Ghost(timeline) }) }
    }

    /// Another handle on this head.
    pub fn share(&self) -> (h: Self)
        ensures
            h.epoch() == self.epoch(),
            h.timeline() == self.timeline(),
    {
        proof {
            use_type_invariant(self);
        }
        Head { inner: self.inner.clone() }
    }

    pub fn version(&self) -> (e: u64)
        ensures
            e == self.epoch(),
            e < u64::MAX,
    {
        proof {
            use_type_invariant(self);
        }
        self.inner.e
    }

    /// Both handles are on one head (pointer identity): same version and timeline.
    #[verifier::external_body]
    pub fn same(&self, other: &Head<D>) -> (b: bool)
        ensures
            b ==> self.epoch() == other.epoch() && self.timeline() == other.timeline(),
    {
        Arc::ptr_eq(&self.inner, &other.inner)
    }

    /// The next version's slot, ok for the next database.
    pub fn stage<S: AccelState<D>>(&self, next: Option<S>) -> (st: Staged<D, S>)
        requires
            next matches Some(s) ==> s.answers(self.next_db()),
        ensures
            st.epoch() == self.epoch(),
            st.timeline() == self.timeline(),
    {
        proof {
            use_type_invariant(self);
        }
        let slot = match next {
            Some(s) => Slot::Valid(Arc::new(s)),
            None => Slot::Invalid,
        };
        Staged { head: self.share(), slot }
    }
}

/// A kernel state and the proof that it answers for the newest version's state at that version.
pub struct Base<D, S: AccelState<D>> {
    head: Head<D>,
    state: Arc<S>,
}

impl<D, S: AccelState<D>> Base<D, S> {
    #[verifier::type_invariant]
    spec fn wf(self) -> bool {
        (*self.state).answers((self.head.timeline())(self.head.epoch() as nat))
    }

    /// The version it is.
    pub closed spec fn epoch(self) -> u64 {
        self.head.epoch()
    }

    pub closed spec fn timeline(self) -> spec_fn(nat) -> D {
        self.head.timeline()
    }

    /// The state, as a value.
    pub closed spec fn state_spec(self) -> S {
        *self.state
    }

    pub fn new(head: Head<D>, state: Arc<S>) -> (b: Self)
        requires
            (*state).answers((head.timeline())(head.epoch() as nat)),
        ensures
            b.epoch() == head.epoch(),
            b.timeline() == head.timeline(),
            b.state_spec() == *state,
    {
        Base { head, state }
    }

    /// The head this base is the state of.
    pub fn head(&self) -> (h: Head<D>)
        ensures
            h.epoch() == self.epoch(),
            h.timeline() == self.timeline(),
    {
        self.head.share()
    }

    pub fn version(&self) -> (e: u64)
        ensures
            e == self.epoch(),
            e < u64::MAX,
    {
        self.head.version()
    }

    /// The state: it answers for the database at its version.
    pub fn state(&self) -> (s: &S)
        ensures
            (*s).answers((self.timeline())(self.epoch() as nat)),
            *s == self.state_spec(),
    {
        proof {
            use_type_invariant(self);
        }
        &*self.state
    }
}

/// The newest version's state, or None if that version's slot is invalid
/// (or no version can follow it).
pub fn base<D, S: AccelState<D>>(store: &Store<D, Slot<S>>) -> (r: Option<Base<D, S>>)
    ensures
        r matches Some(b) ==> b.timeline() == store.timeline(),
{
    let (e, w) = store.newest_world();
    match w {
        Slot::Valid(a) => {
            if e < u64::MAX {
                Some(Base::new(Head::new(e, Ghost(store.timeline())), a))
            } else {
                None
            }
        },
        Slot::Invalid => None,
    }
}

/// The newest version, if a version can follow it.
pub fn head<D, W: AuxWorld<D>>(store: &Store<D, W>) -> (r: Option<Head<D>>)
    ensures
        r matches Some(h) ==> h.timeline() == store.timeline(),
{
    let e = store.newest();
    if e < u64::MAX {
        Some(Head::new(e, Ghost(store.timeline())))
    } else {
        None
    }
}

/// Version e + 1's slot for one accelerator, produced from its head (by the
/// kernel's apply / build): ok for the database at e + 1.  It becomes a
/// version of its own store (`into_prepared`) or of a world (its head ties it
/// to the batch built on the same base).
pub struct Staged<D, S: AccelState<D>> {
    head: Head<D>,
    slot: Slot<S>,
}

impl<D, S: AccelState<D>> Staged<D, S> {
    #[verifier::type_invariant]
    spec fn wf(self) -> bool {
        self.slot.ok(self.head.next_db())
    }

    pub closed spec fn epoch(self) -> u64 {
        self.head.epoch()
    }

    pub closed spec fn timeline(self) -> spec_fn(nat) -> D {
        self.head.timeline()
    }

    pub fn head(&self) -> (h: &Head<D>)
        ensures
            h.epoch() == self.epoch(),
            h.timeline() == self.timeline(),
    {
        &self.head
    }

    /// The slot: ok for the database at e + 1.
    pub fn into_slot(self) -> (s: Slot<S>)
        ensures
            s.ok((self.timeline())((self.epoch() + 1) as nat)),
    {
        proof {
            use_type_invariant(&self);
        }
        self.slot
    }

    /// The next version of `store`, from the store this was staged on
    /// (HOST CONTRACT, TRUSTED: `requires`).
    pub fn into_prepared(self, store: &Store<D, Slot<S>>) -> (p: Prepared<D, Slot<S>>)
        requires
            self.timeline() == store.timeline(),
        ensures
            p.epoch() == self.epoch(),
            p.timeline() == store.timeline(),
    {
        proof {
            use_type_invariant(&self);
        }
        let e = self.head.version();
        Prepared::new(e, self.slot, Ghost(store.timeline()))
    }
}

} // verus!
