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
// e + 1, and the host installs the commits in the engine's commit order.
// The store checks that version e is still the newest at install; it cannot
// check that the host's changes are those of the next commit.
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

/// The newest version's state, to prepare the next version from.
pub struct Base<D, S: AccelState<D>> {
    e: u64,
    state: Arc<S>,
    timeline: Ghost<spec_fn(nat) -> D>,
}

impl<D, S: AccelState<D>> Base<D, S> {
    #[verifier::type_invariant]
    spec fn wf(self) -> bool {
        &&& self.e < u64::MAX
        &&& (*self.state).answers((self.timeline@)(self.e as nat))
    }

    /// The version it is.
    pub closed spec fn epoch(self) -> u64 {
        self.e
    }

    pub closed spec fn timeline(self) -> spec_fn(nat) -> D {
        self.timeline@
    }

    pub fn version(&self) -> (e: u64)
        ensures
            e == self.epoch(),
            e < u64::MAX,
    {
        proof {
            use_type_invariant(self);
        }
        self.e
    }

    /// The state: it answers for the database at its version.
    pub fn state(&self) -> (s: &S)
        ensures
            (*s).answers((self.timeline())(self.epoch() as nat)),
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
                Some(Base { e, state: a, timeline: Ghost(store.timeline()) })
            } else {
                None
            }
        },
        Slot::Invalid => None,
    }
}

/// Version e + 1 of the store: `next`'s state (answering for the database at
/// e + 1), or an invalid slot.
pub fn prepare<D, S: AccelState<D>>(store: &Store<D, Slot<S>>, e: u64, next: Option<S>) -> (p: Prepared<D, Slot<S>>)
    requires
        e < u64::MAX,
        next matches Some(s) ==> s.answers(store.db_at((e + 1) as nat)),
    ensures
        p.epoch() == e,
        p.timeline() == store.timeline(),
{
    let w = match next {
        Some(s) => Slot::Valid(Arc::new(s)),
        None => Slot::Invalid,
    };
    Prepared::new(e, w, Ghost(store.timeline()))
}

} // verus!
