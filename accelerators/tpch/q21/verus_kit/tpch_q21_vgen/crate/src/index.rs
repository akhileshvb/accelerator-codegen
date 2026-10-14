use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::sums::*;

verus! {

pub struct Key {
    pub number: i64,
    pub text: Arc<Vec<u8>>,
}

pub type KeyView = (int, Seq<u8>);

impl Key {
    pub open spec fn view(self) -> KeyView {
        (self.number as int, (*self.text)@)
    }

    pub fn number(number: i64) -> (key: Self)
        ensures key.view() == (number as int, Seq::<u8>::empty()),
    {
        Key { number, text: Arc::new(Vec::new()) }
    }

    pub fn text(text: Vec<u8>) -> (key: Self)
        ensures key.view() == (0int, text@),
    {
        Key { number: 0, text: Arc::new(text) }
    }

    pub fn share(&self) -> (key: Self)
        ensures key.view() == self.view(),
    {
        Key { number: self.number, text: self.text.clone() }
    }

    pub fn with_number(&self, number: i64) -> (key: Self)
        ensures key.view() == (number as int, self.view().1),
    {
        Key { number, text: self.text.clone() }
    }
}

pub open spec fn key_lt(left: KeyView, right: KeyView) -> bool {
    left.0 < right.0 || (left.0 == right.0 && bytes_lt(left.1, right.1))
}

pub proof fn lemma_key_order(left: KeyView, right: KeyView)
    ensures
        !key_lt(left, left),
        !(key_lt(left, right) && key_lt(right, left)),
        left != right ==> key_lt(left, right) || key_lt(right, left),
{
    lemma_bytes_lt_irrefl(left.1);
    lemma_bytes_lt_asym(left.1, right.1);
    if left.0 == right.0 && left != right {
        if left.1 == right.1 { assert(left == right); }
        lemma_bytes_lt_total(left.1, right.1);
    }
}

pub proof fn lemma_key_trans(first: KeyView, middle: KeyView, last: KeyView)
    requires key_lt(first, middle), key_lt(middle, last),
    ensures key_lt(first, last),
{
    if first.0 == middle.0 && middle.0 == last.0 {
        lemma_bytes_lt_trans(first.1, middle.1, last.1);
    }
}

pub fn compare(left: &Key, right: &Key) -> (order: i8)
    ensures
        (order < 0) == key_lt(left.view(), right.view()),
        (order == 0) == (left.view() == right.view()),
        (order > 0) == key_lt(right.view(), left.view()),
{
    if left.number < right.number { return -1; }
    if left.number > right.number { return 1; }
    compare_bytes(left.text.as_slice(), right.text.as_slice())
}

pub fn compare_bytes(left: &[u8], right: &[u8]) -> (order: i8)
    ensures
        (order < 0) == bytes_lt(left@, right@),
        (order == 0) == (left@ == right@),
        (order > 0) == bytes_lt(right@, left@),
{
    if bytes_eq_exec(left, right) {
        proof { lemma_bytes_lt_irrefl(left@); }
        return 0;
    }
    proof {
        lemma_bytes_lt_total(left@, right@);
        lemma_bytes_lt_asym(left@, right@);
    }
    if bytes_lt_exec(left, right) { -1 } else { 1 }
}

pub enum Tree<Value> {
    Empty,
    Branch {
        key: Key,
        value: Arc<Value>,
        left: Arc<Tree<Value>>,
        right: Arc<Tree<Value>>,
        height: usize,
    },
}

pub struct Index<Value> {
    root: Arc<Tree<Value>>,
}

pub struct Cursor<Value> {
    stack: Vec<Arc<Tree<Value>>>,
    current: Arc<Tree<Value>>,
}

impl<Value> Cursor<Value> {
    pub closed spec fn work(self) -> int { pending_work(*self.current, self.stack@) }

    pub closed spec fn remaining(self) -> Seq<(KeyView, Value)> {
        (*self.current).entries() + pending_entries(self.stack@)
    }

    #[verifier::rlimit(20)]
    pub fn next(&mut self) -> (result: Option<(Key, Arc<Value>)>)
        ensures
            final(self).work() >= 0,
            match result {
                Some(row) => {
                    &&& old(self).remaining().len() > 0
                    &&& (row.0.view(), *row.1) == old(self).remaining()[0]
                    &&& final(self).remaining() == old(self).remaining().skip(1)
                    &&& final(self).work() < old(self).work()
                },
                None => old(self).remaining().len() == 0 && final(self).remaining().len() == 0,
            },
    {
        let ghost initial_remaining = self.remaining();
        let ghost initial_work = self.work();
        proof { lemma_pending_nonnegative(&*self.current, self.stack@); }
        loop
            invariant
                initial_remaining == old(self).remaining(),
                initial_work == old(self).work(),
                self.remaining() == initial_remaining,
                0 <= self.work() <= initial_work,
            decreases pending_work(*self.current, self.stack@),
        {
            match &*self.current {
                Tree::Branch { left, .. } => {
                    let next = left.clone();
                    let ghost stack_before = self.stack@;
                    self.stack.push(self.current.clone());
                    self.current = next;
                    proof {
                        lemma_sum_push(stack_before, self.stack@.last(), |node: Arc<Tree<Value>>| pending_cost(node));
                        assert(self.stack@.drop_last() == stack_before);
                        lemma_pending_nonnegative(&*self.current, self.stack@);
                        assert(self.remaining() == initial_remaining);
                    }
                },
                Tree::Empty => {
                    let ghost stack_before = self.stack@;
                    let node = match self.stack.pop() {
                        Some(node) => node,
                        None => {
                            proof {
                                assert(initial_remaining.len() == 0);
                                assert(self.remaining().len() == 0);
                            }
                            return None;
                        },
                    };
                    proof {
                        assert(stack_before == self.stack@.push(node));
                        lemma_sum_push(self.stack@, node, |node: Arc<Tree<Value>>| pending_cost(node));
                    }
                    match &*node {
                        Tree::Empty => {
                            proof { lemma_pending_nonnegative(&*self.current, self.stack@); }
                        },
                        Tree::Branch { key, value, right, .. } => {
                            self.current = right.clone();
                            proof {
                                lemma_pending_nonnegative(&*self.current, self.stack@);
                                assert(initial_remaining =~= seq![(key.view(), **value)] + self.remaining());
                                assert(self.remaining() =~= initial_remaining.skip(1));
                                assert(initial_remaining.len() > 0);
                                assert(initial_remaining[0] == (key.view(), **value));
                                assert(self.work() < initial_work);
                            }
                            return Some((key.share(), value.clone()));
                        },
                    }
                },
            }
        }
    }
}

impl<Value> Tree<Value> {
    pub open spec fn at(self, query: KeyView) -> Option<Value>
        decreases self,
    {
        match self {
            Tree::Empty => None,
            Tree::Branch { key, value, left, right, .. } => {
                if query == key.view() { Some(*value) }
                else if key_lt(query, key.view()) { (*left).at(query) }
                else { (*right).at(query) }
            },
        }
    }

    pub open spec fn size(self) -> nat
        decreases self,
    {
        match self {
            Tree::Empty => 0,
            Tree::Branch { left, right, .. } => 1 + (*left).size() + (*right).size(),
        }
    }

    pub open spec fn entries(self) -> Seq<(KeyView, Value)>
        decreases self,
    {
        match self {
            Tree::Empty => Seq::empty(),
            Tree::Branch { key, value, left, right, .. } =>
                (*left).entries() + seq![(key.view(), *value)] + (*right).entries(),
        }
    }

    pub open spec fn depth(self) -> nat
        decreases self,
    {
        match self {
            Tree::Empty => 0,
            Tree::Branch { left, right, .. } =>
                1 + if (*left).depth() > (*right).depth() { (*left).depth() } else { (*right).depth() },
        }
    }

    pub open spec fn wf(self) -> bool
        decreases self,
    {
        match self {
            Tree::Empty => true,
            Tree::Branch { key, left, right, .. } => {
                &&& (*left).wf() && (*right).wf()
                &&& forall|index: int| #![auto] 0 <= index < (*left).entries().len() ==>
                    key_lt((*left).entries()[index].0, key.view())
                &&& forall|index: int| #![auto] 0 <= index < (*right).entries().len() ==>
                    key_lt(key.view(), (*right).entries()[index].0)
            },
        }
    }
}

pub open spec fn pending_cost<Value>(node: Arc<Tree<Value>>) -> int {
    match *node {
        Tree::Empty => 1,
        Tree::Branch { right, .. } => 1 + 2 * (*right).size() as int,
    }
}

pub open spec fn pending_work<Value>(current: Tree<Value>, stack: Seq<Arc<Tree<Value>>>) -> int {
    2 * current.size() + sum(stack, |node: Arc<Tree<Value>>| pending_cost(node))
}

pub proof fn lemma_pending_nonnegative<Value>(current: &Tree<Value>, stack: Seq<Arc<Tree<Value>>>)
    ensures pending_work(*current, stack) >= 0,
{
    assert forall|position: int| #![trigger stack[position]] 0 <= position < stack.len() implies pending_cost(stack[position]) >= 0 by {};
    lemma_sum_nonneg(stack, |node: Arc<Tree<Value>>| pending_cost(node));
}

pub open spec fn pending_entries<Value>(stack: Seq<Arc<Tree<Value>>>) -> Seq<(KeyView, Value)>
    decreases stack.len(),
{
    if stack.len() == 0 { Seq::empty() } else {
        let tail = match *stack.last() {
            Tree::Empty => Seq::empty(),
            Tree::Branch { key, value, right, .. } => seq![(key.view(), *value)] + (*right).entries(),
        };
        tail + pending_entries(stack.drop_last())
    }
}

pub open spec fn ordered_rows<Value>(rows: Seq<(KeyView, Value)>) -> bool {
    forall|first: int, last: int| #![auto] 0 <= first < last < rows.len() ==> key_lt(rows[first].0, rows[last].0)
}

pub open spec fn rows_of<Value>(tree: &Tree<Value>) -> Seq<(KeyView, Value)> { tree.entries() }
pub open spec fn branch_row<Value>(key: &Key, value: &Arc<Value>) -> Seq<(KeyView, Value)> { seq![(key.view(), **value)] }

pub open spec fn drop_key<Value>(rows: Seq<(KeyView, Value)>, key: KeyView) -> Seq<(KeyView, Value)>
    decreases rows.len(),
{
    if rows.len() == 0 { Seq::empty() }
    else if rows.last().0 == key { drop_key(rows.drop_last(), key) }
    else { drop_key(rows.drop_last(), key).push(rows.last()) }
}

pub proof fn lemma_push_contains<Value>(rows: Seq<Value>, last: Value, query: Value)
    ensures rows.push(last).contains(query) <==> rows.contains(query) || last == query,
{
    if rows.contains(query) {
        let position = choose|position: int| 0 <= position < rows.len() && rows[position] == query;
        assert(rows.push(last)[position] == query);
    }
    if last == query { assert(rows.push(last)[rows.len() as int] == query); }
    if rows.push(last).contains(query) {
        let position = choose|position: int| 0 <= position < rows.push(last).len() && rows.push(last)[position] == query;
        if position < rows.len() { assert(rows[position] == query); }
        else { assert(last == query); }
    }
}

pub proof fn lemma_drop_concat<Value>(left: Seq<(KeyView, Value)>, right: Seq<(KeyView, Value)>, key: KeyView)
    ensures drop_key(left + right, key) == drop_key(left, key) + drop_key(right, key),
    decreases right.len(),
{
    if right.len() == 0 { assert(left + right =~= left); }
    else {
        assert((left + right).drop_last() =~= left + right.drop_last());
        lemma_drop_concat(left, right.drop_last(), key);
        if right.last().0 != key {
            assert((drop_key(left, key) + drop_key(right.drop_last(), key)).push(right.last()) =~=
                drop_key(left, key) + drop_key(right.drop_last(), key).push(right.last()));
        }
    }
}

pub proof fn lemma_drop_absent<Value>(rows: Seq<(KeyView, Value)>, key: KeyView)
    requires forall|position: int| #![auto] 0 <= position < rows.len() ==> rows[position].0 != key,
    ensures drop_key(rows, key) == rows,
    decreases rows.len(),
{
    if rows.len() > 0 {
        lemma_drop_absent(rows.drop_last(), key);
        assert(rows =~= rows.drop_last().push(rows.last()));
    }
}

pub proof fn lemma_drop_bindings<Value>(rows: Seq<(KeyView, Value)>, key: KeyView)
    ensures forall|row: (KeyView, Value)| #![trigger drop_key(rows, key).contains(row)]
        drop_key(rows, key).contains(row) <==> rows.contains(row) && row.0 != key,
    decreases rows.len(),
{
    if rows.len() > 0 {
        lemma_drop_bindings(rows.drop_last(), key);
        assert(rows =~= rows.drop_last().push(rows.last()));
        assert forall|row: (KeyView, Value)| #![trigger drop_key(rows, key).contains(row)]
            drop_key(rows, key).contains(row) <==> rows.contains(row) && row.0 != key by {
            lemma_push_contains(rows.drop_last(), rows.last(), row);
            assert(drop_key(rows.drop_last(), key).contains(row) <==> rows.drop_last().contains(row) && row.0 != key);
            if rows.last().0 != key {
                lemma_push_contains(drop_key(rows.drop_last(), key), rows.last(), row);
            }
        }
    }
}

pub proof fn lemma_drop_ordered<Value>(rows: Seq<(KeyView, Value)>, key: KeyView)
    requires ordered_rows(rows),
    ensures ordered_rows(drop_key(rows, key)),
    decreases rows.len(),
{
    if rows.len() > 0 {
        let prefix = rows.drop_last();
        assert(ordered_rows(prefix));
        lemma_drop_ordered(prefix, key);
        lemma_drop_bindings(prefix, key);
        if rows.last().0 != key {
            let retained = drop_key(prefix, key);
            assert forall|first: int, last: int| #![auto] 0 <= first < last < retained.len() + 1 implies
                key_lt(retained.push(rows.last())[first].0, retained.push(rows.last())[last].0) by {
                if last < retained.len() { assert(ordered_rows(retained)); }
                else {
                    assert(retained.contains(retained[first]));
                    let position = choose|position: int| 0 <= position < prefix.len() && prefix[position] == retained[first];
                    assert(prefix[position] == rows[position]);
                    assert(key_lt(rows[position].0, rows.last().0));
                }
            }
        }
    }
}

pub proof fn lemma_drop_first<Value>(rows: Seq<(KeyView, Value)>)
    requires ordered_rows(rows), rows.len() > 0,
    ensures drop_key(rows, rows[0].0) == rows.skip(1),
{
    reveal_with_fuel(drop_key, 2);
    let tail = rows.skip(1);
    assert forall|position: int| #![auto] 0 <= position < tail.len() implies tail[position].0 != rows[0].0 by {
        assert(tail[position] == rows[position + 1]);
        lemma_key_order(rows[0].0, tail[position].0);
    }
    lemma_drop_absent(tail, rows[0].0);
    assert(rows =~= seq![rows[0]] + tail);
    lemma_drop_concat(seq![rows[0]], tail, rows[0].0);
}

pub proof fn lemma_join_ordered<Value>(left: Seq<(KeyView, Value)>, key: KeyView, value: &Value, right: Seq<(KeyView, Value)>)
    requires
        ordered_rows(left), ordered_rows(right),
        forall|position: int| #![auto] 0 <= position < left.len() ==> key_lt(left[position].0, key),
        forall|position: int| #![auto] 0 <= position < right.len() ==> key_lt(key, right[position].0),
    ensures ordered_rows(left + seq![(key, *value)] + right),
{
    let rows = left + seq![(key, *value)] + right;
    assert forall|first: int, last: int| #![auto] 0 <= first < last < rows.len() implies key_lt(rows[first].0, rows[last].0) by {
        if last < left.len() {
            assert(rows[first] == left[first] && rows[last] == left[last]);
        } else if first > left.len() {
            assert(rows[first] == right[first - left.len() - 1]);
            assert(rows[last] == right[last - left.len() - 1]);
        } else if first == left.len() {
            assert(rows[first].0 == key);
            assert(rows[last] == right[last - left.len() - 1]);
        } else if last == left.len() {
            assert(rows[first] == left[first]);
            assert(rows[last].0 == key);
        } else {
            assert(rows[first] == left[first]);
            assert(rows[last] == right[last - left.len() - 1]);
            lemma_key_trans(rows[first].0, key, rows[last].0);
        }
    }
}

pub proof fn lemma_wf_rows<Value>(tree: &Tree<Value>)
    ensures tree.wf() <==> ordered_rows(tree.entries()),
    decreases tree.depth(),
{
    match tree {
        Tree::Empty => {},
        Tree::Branch { key, value, left, right, .. } => {
            lemma_wf_rows(&**left);
            lemma_wf_rows(&**right);
            let rows = tree.entries();
            let low = rows_of(&**left);
            let high = rows_of(&**right);
            if tree.wf() {
                lemma_join_ordered(low, key.view(), &**value, high);
            }
            if ordered_rows(rows) {
                assert forall|first: int, last: int| #![auto] 0 <= first < last < low.len() implies key_lt(low[first].0, low[last].0) by {
                    assert(low[first] == rows[first]);
                    assert(low[last] == rows[last]);
                }
                assert forall|first: int, last: int| #![auto] 0 <= first < last < high.len() implies key_lt(high[first].0, high[last].0) by {
                    assert(high[first] == rows[low.len() + 1 + first]);
                    assert(high[last] == rows[low.len() + 1 + last]);
                }
                assert(rows[low.len() as int].0 == key.view());
                assert forall|position: int| #![auto] 0 <= position < low.len() implies key_lt(low[position].0, key.view()) by {
                    assert(low[position] == rows[position]);
                }
                assert forall|position: int| #![auto] 0 <= position < high.len() implies key_lt(key.view(), high[position].0) by {
                    assert(high[position] == rows[low.len() + 1 + position]);
                }
            }
        },
    }
}

pub proof fn lemma_at_rows<Value>(tree: &Tree<Value>, query: KeyView)
    requires tree.wf(),
    ensures
        forall|position: int| #![auto] 0 <= position < tree.entries().len() && tree.entries()[position].0 == query ==>
            tree.at(query) == Some(tree.entries()[position].1),
        tree.at(query).is_some() <==> exists|position: int| #![auto] 0 <= position < tree.entries().len() && tree.entries()[position].0 == query,
    decreases tree.depth(),
{
    match tree {
        Tree::Empty => {},
        Tree::Branch { key, left, right, .. } => {
            lemma_at_rows(&**left, query);
            lemma_at_rows(&**right, query);
            lemma_key_order(query, key.view());
            let rows = tree.entries();
            let low = rows_of(&**left);
            let high = rows_of(&**right);
            assert forall|position: int| #![auto] 0 <= position < rows.len() && rows[position].0 == query implies tree.at(query) == Some(rows[position].1) by {
                if position < low.len() {
                    assert(rows[position] == low[position]);
                    assert(key_lt(query, key.view()));
                } else if position == low.len() {
                    assert(rows[position].0 == key.view());
                } else {
                    let offset = position - low.len() - 1;
                    assert(rows[position] == high[offset]);
                    assert(key_lt(key.view(), query));
                }
            }
            if tree.at(query).is_some() {
                if query == key.view() {
                    assert(rows[low.len() as int].0 == query);
                } else if key_lt(query, key.view()) {
                    let position = choose|position: int| #![auto] 0 <= position < low.len() && low[position].0 == query;
                    assert(rows[position].0 == query);
                } else {
                    let position = choose|position: int| #![auto] 0 <= position < high.len() && high[position].0 == query;
                    assert(rows[low.len() + 1 + position].0 == query);
                }
            }
        },
    }
}

pub proof fn lemma_same_rows<Value>(first: &Tree<Value>, second: &Tree<Value>)
    requires first.wf(), second.wf(), first.entries() == second.entries(),
    ensures forall|query: KeyView| #![trigger first.at(query), second.at(query)] first.at(query) == second.at(query),
{
    hide(Tree::at);
    assert forall|query: KeyView| #![trigger first.at(query), second.at(query)] first.at(query) == second.at(query) by {
        lemma_at_rows(first, query);
        lemma_at_rows(second, query);
        if first.at(query).is_some() {
            let position = choose|position: int| #![auto] 0 <= position < first.entries().len() && first.entries()[position].0 == query;
            assert(second.entries()[position].0 == query);
        } else if second.at(query).is_some() {
            let position = choose|position: int| #![auto] 0 <= position < second.entries().len() && second.entries()[position].0 == query;
            assert(first.entries()[position].0 == query);
        }
    }
}

pub open spec fn joined<Value>(key: Key, value: Arc<Value>, left: Arc<Tree<Value>>, right: Arc<Tree<Value>>) -> Tree<Value> {
    Tree::Branch { key, value, left, right, height: 0 }
}

fn finish_balance<Value>(result: Arc<Tree<Value>>, Ghost(expected): Ghost<Tree<Value>>) -> (output: Arc<Tree<Value>>)
    requires (*result).entries() == expected.entries(),
    ensures
        output == result,
        (*output).wf() == expected.wf(),
        expected.wf() ==> forall|query: KeyView| #![trigger (*output).at(query), expected.at(query)] (*output).at(query) == expected.at(query),
{
    proof {
        lemma_wf_rows(&*result);
        lemma_wf_rows(&expected);
        if expected.wf() { lemma_same_rows(&*result, &expected); }
    }
    result
}

#[verifier::rlimit(20)]
fn finish_erase<Value>(result: Arc<Tree<Value>>, original: &Arc<Tree<Value>>, key: &Key) -> (output: Arc<Tree<Value>>)
    requires (**original).wf(), (*result).entries() == drop_key((**original).entries(), key.view()),
    ensures
        output == result, (*output).wf(),
        (*output).entries() == drop_key((**original).entries(), key.view()),
        forall|query: KeyView| #![trigger (*output).at(query)] (*output).at(query) == if query == key.view() { None } else { (**original).at(query) },
{
    proof {
        lemma_wf_rows(&**original);
        lemma_drop_ordered(rows_of(&**original), key.view());
        lemma_wf_rows(&*result);
        lemma_drop_bindings(rows_of(&**original), key.view());
        assert forall|query: KeyView| #![trigger (*result).at(query)] (*result).at(query) == if query == key.view() { None } else { (**original).at(query) } by {
            lemma_at_rows(&**original, query);
            lemma_at_rows(&*result, query);
            if query != key.view() && (**original).at(query).is_some() {
                let old_position = choose|position: int| #![auto] 0 <= position < (**original).entries().len() && (**original).entries()[position].0 == query;
                let row = rows_of(&**original)[old_position];
                assert((**original).entries().contains(row));
                assert((*result).entries().contains(row));
                let new_position = choose|position: int| #![auto] 0 <= position < (*result).entries().len() && (*result).entries()[position] == row;
            } else if (*result).at(query).is_some() {
                let new_position = choose|position: int| #![auto] 0 <= position < (*result).entries().len() && (*result).entries()[position].0 == query;
                let row = rows_of(&*result)[new_position];
                assert((*result).entries().contains(row));
                assert((**original).entries().contains(row) && row.0 != key.view());
                let old_position = choose|position: int| #![auto] 0 <= position < (**original).entries().len() && (**original).entries()[position] == row;
            }
        }
    }
    result
}

impl<Value> Index<Value> {
    #[verifier::type_invariant]
    spec fn valid(self) -> bool { self.wf() }

    pub closed spec fn tree(self) -> Tree<Value> { *self.root }
    pub open spec fn entries(self) -> Seq<(KeyView, Value)> { self.tree().entries() }
    pub open spec fn wf(self) -> bool { self.tree().wf() }

    pub open spec fn at(self, key: KeyView) -> Option<Value> {
        self.tree().at(key)
    }

    pub proof fn model(&self)
        requires self.wf(),
        ensures
            self.wf(),
            ordered_rows(self.entries()),
            forall|position: int| #![trigger self.entries()[position]] 0 <= position < self.entries().len() ==>
                self.at(self.entries()[position].0) == Some(self.entries()[position].1),
            forall|query: KeyView| #![trigger self.at(query)] self.at(query).is_some() <==>
                exists|position: int| #![auto] 0 <= position < self.entries().len() && self.entries()[position].0 == query,
    {
        lemma_wf_rows(&*self.root);
        assert forall|position: int| #![trigger self.entries()[position]] 0 <= position < self.entries().len() implies
            self.at(self.entries()[position].0) == Some(self.entries()[position].1) by {
            lemma_at_rows(&*self.root, self.entries()[position].0);
        }
        assert forall|query: KeyView| #![trigger self.at(query)] self.at(query).is_some() <==>
            exists|position: int| #![auto] 0 <= position < self.entries().len() && self.entries()[position].0 == query by {
            lemma_at_rows(&*self.root, query);
        }
    }

    pub fn new() -> (result: Self)
        ensures result.wf(), result.entries() == Seq::<(KeyView, Value)>::empty(),
            forall|query: KeyView| #![trigger result.at(query)] result.at(query).is_none(),
    {
        Index { root: Arc::new(Tree::Empty) }
    }

    pub fn share(&self) -> (result: Self)
        ensures self.wf(), result.wf(), result.entries() == self.entries(), result.tree() == self.tree(),
    {
        proof { use_type_invariant(self); }
        Index { root: self.root.clone() }
    }

    pub fn get(&self, key: &Key) -> (result: Option<Arc<Value>>)
        ensures
            match result { Some(value) => self.at(key.view()) == Some(*value), None => self.at(key.view()).is_none() },
    {
        proof { use_type_invariant(self); }
        let mut cursor = self.root.clone();
        loop
            invariant self.at(key.view()) == (*cursor).at(key.view()),
            decreases (*cursor).depth(),
        {
            match &*cursor {
                Tree::Empty => return None,
                Tree::Branch { key: stored, value, left, right, .. } => {
                    let order = compare(key, stored);
                    if order == 0 { return Some(value.clone()); }
                    cursor = if order < 0 { left.clone() } else { right.clone() };
                },
            }
        }
    }

    pub fn put(&mut self, key: Key, value: Arc<Value>)
        ensures
            final(self).wf(),
            forall|query: KeyView| #![auto] final(self).at(query) == if query == key.view() { Some(*value) } else { old(self).at(query) },
    {
        proof { use_type_invariant(&*self); }
        self.root = insert(&self.root, key, value);
    }

    pub fn remove(&mut self, key: &Key)
        ensures
            final(self).wf(),
            forall|query: KeyView| #![auto] final(self).at(query) == if query == key.view() { None } else { old(self).at(query) },
    {
        proof { use_type_invariant(&*self); }
        self.root = erase(&self.root, key);
    }

    pub fn is_empty(&self) -> (result: bool)
        ensures result == (self.entries().len() == 0),
    {
        match &*self.root { Tree::Empty => true, _ => false }
    }

    pub fn cursor(&self) -> (result: Cursor<Value>)
        ensures result.remaining() == self.entries(), result.work() >= 0,
    {
        Cursor { stack: Vec::new(), current: self.root.clone() }
    }

    pub fn rows(&self) -> (result: Vec<(Key, Arc<Value>)>)
        ensures self.wf(), result@.map_values(|row: (Key, Arc<Value>)| (row.0.view(), *row.1)) == self.entries(),
    {
        proof { use_type_invariant(self); }
        let mut result = Vec::new();
        let mut cursor = self.cursor();
        loop
            invariant
                self.wf(),
                result@.map_values(|row: (Key, Arc<Value>)| (row.0.view(), *row.1)) + cursor.remaining() == self.entries(),
                cursor.work() >= 0,
            decreases cursor.work(),
        {
            let ghost previous_remaining = cursor.remaining();
            let ghost prefix = result@.map_values(|row: (Key, Arc<Value>)| (row.0.view(), *row.1));
            let row = match cursor.next() { Some(row) => row, None => return result };
            let ghost item = previous_remaining[0];
            result.push(row);
            proof {
                assert(previous_remaining =~= seq![item] + cursor.remaining());
                assert(result@.map_values(|row: (Key, Arc<Value>)| (row.0.view(), *row.1)) =~= prefix.push(item));
            }
        }
    }
}

fn height<Value>(tree: &Arc<Tree<Value>>) -> usize {
    match &**tree { Tree::Empty => 0, Tree::Branch { height, .. } => *height }
}

fn branch<Value>(key: Key, value: Arc<Value>, left: Arc<Tree<Value>>, right: Arc<Tree<Value>>) -> (result: Arc<Tree<Value>>)
    ensures
        (*result).entries() == joined(key, value, left, right).entries(),
        (*result).wf() == joined(key, value, left, right).wf(),
        forall|query: KeyView| #![trigger (*result).at(query)] (*result).at(query) == joined(key, value, left, right).at(query),
{
    let left_height = height(&left);
    let right_height = height(&right);
    let maximum = if left_height > right_height { left_height } else { right_height };
    let height = if maximum == usize::MAX { maximum } else { maximum + 1 };
    Arc::new(Tree::Branch { key, value, left, right, height })
}

#[verifier::rlimit(20)]
fn balance<Value>(key: Key, value: Arc<Value>, left: Arc<Tree<Value>>, right: Arc<Tree<Value>>) -> (result: Arc<Tree<Value>>)
    ensures
        (*result).entries() == joined(key, value, left, right).entries(),
        (*result).wf() == joined(key, value, left, right).wf(),
        joined(key, value, left, right).wf() ==> forall|query: KeyView| #![trigger (*result).at(query)]
            (*result).at(query) == joined(key, value, left, right).at(query),
{
    hide(Tree::wf);
    hide(Tree::at);
    let ghost expected = joined(key, value, left, right);
    let left_height = height(&left);
    let right_height = height(&right);
    if left_height > right_height && left_height - right_height > 1 {
        if let Tree::Branch { key: pivot, value: pivot_value, left: outer, right: inner, .. } = &*left {
            proof {
                assert((*left).entries() == (**outer).entries() + seq![(pivot.view(), **pivot_value)] + (**inner).entries());
                assert(expected.entries() == (*left).entries() + seq![(key.view(), *value)] + (*right).entries());
            }
            if height(outer) >= height(inner) {
                let down = branch(key, value, inner.clone(), right);
                let result = branch(pivot.share(), pivot_value.clone(), outer.clone(), down);
                proof { assert((*result).entries() =~= expected.entries()); }
                return finish_balance(result, Ghost(expected));
            }
            if let Tree::Branch { key: middle, value: middle_value, left: inner_left, right: inner_right, .. } = &**inner {
                proof {
                    assert((**inner).entries() == (**inner_left).entries() + seq![(middle.view(), **middle_value)] + (**inner_right).entries());
                }
                let low = branch(pivot.share(), pivot_value.clone(), outer.clone(), inner_left.clone());
                let high = branch(key, value, inner_right.clone(), right);
                let result = branch(middle.share(), middle_value.clone(), low, high);
                proof { assert((*result).entries() =~= expected.entries()); }
                return finish_balance(result, Ghost(expected));
            }
        }
    }
    if right_height > left_height && right_height - left_height > 1 {
        if let Tree::Branch { key: pivot, value: pivot_value, left: inner, right: outer, .. } = &*right {
            proof {
                assert((*right).entries() == (**inner).entries() + seq![(pivot.view(), **pivot_value)] + (**outer).entries());
                assert(expected.entries() == (*left).entries() + seq![(key.view(), *value)] + (*right).entries());
            }
            if height(outer) >= height(inner) {
                let down = branch(key, value, left, inner.clone());
                let result = branch(pivot.share(), pivot_value.clone(), down, outer.clone());
                proof { assert((*result).entries() =~= expected.entries()); }
                return finish_balance(result, Ghost(expected));
            }
            if let Tree::Branch { key: middle, value: middle_value, left: inner_left, right: inner_right, .. } = &**inner {
                proof {
                    assert((**inner).entries() == (**inner_left).entries() + seq![(middle.view(), **middle_value)] + (**inner_right).entries());
                }
                let low = branch(key, value, left, inner_left.clone());
                let high = branch(pivot.share(), pivot_value.clone(), inner_right.clone(), outer.clone());
                let result = branch(middle.share(), middle_value.clone(), low, high);
                proof { assert((*result).entries() =~= expected.entries()); }
                return finish_balance(result, Ghost(expected));
            }
        }
    }
    let result = branch(key, value, left, right);
    finish_balance(result, Ghost(expected))
}

#[verifier::rlimit(20)]
fn insert<Value>(tree: &Arc<Tree<Value>>, key: Key, value: Arc<Value>) -> (result: Arc<Tree<Value>>)
    requires (**tree).wf(),
    ensures
        (*result).wf(),
        forall|query: KeyView| #![trigger (*result).at(query)] (*result).at(query) == if query == key.view() { Some(*value) } else { (**tree).at(query) },
    decreases (**tree).depth(),
{
    match &**tree {
        Tree::Empty => branch(key, value, Arc::new(Tree::Empty), Arc::new(Tree::Empty)),
        Tree::Branch { key: stored, value: stored_value, left, right, .. } => {
            let order = compare(&key, stored);
            if order == 0 { return branch(stored.share(), value, left.clone(), right.clone()); }
            if order < 0 {
                let low = insert(left, key, value);
                proof {
                    assert forall|position: int| #![auto] 0 <= position < (*low).entries().len() implies key_lt((*low).entries()[position].0, stored.view()) by {
                        let query = (*low).entries()[position].0;
                        lemma_at_rows(&*low, query);
                        if query != key.view() {
                            lemma_at_rows(&**left, query);
                            let old_position = choose|old_position: int| #![auto] 0 <= old_position < (**left).entries().len() && (**left).entries()[old_position].0 == query;
                            assert(key_lt((**left).entries()[old_position].0, stored.view()));
                        }
                    }
                }
                let result = balance(stored.share(), stored_value.clone(), low.clone(), right.clone());
                proof {
                    assert forall|query: KeyView| #![trigger (*result).at(query)] (*result).at(query) == if query == key.view() { Some(*value) } else { (**tree).at(query) } by {
                        assert((*low).at(query) == if query == key.view() { Some(*value) } else { (**left).at(query) });
                        assert((*result).at(query) == joined(*stored, *stored_value, low, *right).at(query));
                    }
                }
                result
            } else {
                let high = insert(right, key, value);
                proof {
                    assert forall|position: int| #![auto] 0 <= position < (*high).entries().len() implies key_lt(stored.view(), (*high).entries()[position].0) by {
                        let query = (*high).entries()[position].0;
                        lemma_at_rows(&*high, query);
                        if query != key.view() {
                            lemma_at_rows(&**right, query);
                            let old_position = choose|old_position: int| #![auto] 0 <= old_position < (**right).entries().len() && (**right).entries()[old_position].0 == query;
                            assert(key_lt(stored.view(), (**right).entries()[old_position].0));
                        }
                    }
                }
                let result = balance(stored.share(), stored_value.clone(), left.clone(), high.clone());
                proof {
                    assert forall|query: KeyView| #![trigger (*result).at(query)] (*result).at(query) == if query == key.view() { Some(*value) } else { (**tree).at(query) } by {
                        assert((*high).at(query) == if query == key.view() { Some(*value) } else { (**right).at(query) });
                        assert((*result).at(query) == joined(*stored, *stored_value, *left, high).at(query));
                    }
                }
                result
            }
        },
    }
}

#[verifier::rlimit(20)]
fn erase<Value>(tree: &Arc<Tree<Value>>, key: &Key) -> (result: Arc<Tree<Value>>)
    requires (**tree).wf(),
    ensures
        (*result).wf(),
        (*result).entries() == drop_key((**tree).entries(), key.view()),
        forall|query: KeyView| #![trigger (*result).at(query)] (*result).at(query) == if query == key.view() { None } else { (**tree).at(query) },
    decreases (**tree).depth(),
{
    match &**tree {
        Tree::Empty => tree.clone(),
        Tree::Branch { key: stored, value, left, right, .. } => {
            let ghost low_rows = rows_of(&**left);
            let ghost high_rows = rows_of(&**right);
            let ghost middle = branch_row(stored, value);
            proof {
                lemma_drop_concat(low_rows, middle, key.view());
                lemma_drop_concat(low_rows + middle, high_rows, key.view());
            }
            let order = compare(key, stored);
            if order < 0 {
                proof {
                    assert forall|position: int| #![auto] 0 <= position < high_rows.len() implies high_rows[position].0 != key.view() by {
                        lemma_key_trans(key.view(), stored.view(), high_rows[position].0);
                        lemma_key_order(key.view(), high_rows[position].0);
                    }
                    lemma_drop_absent(high_rows, key.view());
                    lemma_drop_absent(middle, key.view());
                }
                let low = erase(left, key);
                let result = balance(stored.share(), value.clone(), low, right.clone());
                proof { assert((*result).entries() =~= drop_key((**tree).entries(), key.view())); }
                return finish_erase(result, tree, key);
            }
            if order > 0 {
                proof {
                    assert forall|position: int| #![auto] 0 <= position < low_rows.len() implies low_rows[position].0 != key.view() by {
                        lemma_key_trans(low_rows[position].0, stored.view(), key.view());
                        lemma_key_order(low_rows[position].0, key.view());
                    }
                    lemma_drop_absent(low_rows, key.view());
                    lemma_drop_absent(middle, key.view());
                }
                let high = erase(right, key);
                let result = balance(stored.share(), value.clone(), left.clone(), high);
                proof { assert((*result).entries() =~= drop_key((**tree).entries(), key.view())); }
                return finish_erase(result, tree, key);
            }
            proof {
                assert forall|position: int| #![auto] 0 <= position < low_rows.len() implies low_rows[position].0 != key.view() by {
                    lemma_key_order(low_rows[position].0, key.view());
                }
                assert forall|position: int| #![auto] 0 <= position < high_rows.len() implies high_rows[position].0 != key.view() by {
                    lemma_key_order(key.view(), high_rows[position].0);
                }
                lemma_drop_absent(low_rows, key.view());
                lemma_drop_absent(high_rows, key.view());
                lemma_drop_first(middle);
                assert(middle[0].0 == key.view());
                assert(drop_key(middle, key.view()) =~= Seq::<(KeyView, Value)>::empty());
                assert(drop_key((**tree).entries(), key.view()) == low_rows + high_rows);
            }
            if let Tree::Empty = &**left { return finish_erase(right.clone(), tree, key); }
            if let Tree::Empty = &**right { return finish_erase(left.clone(), tree, key); }
            proof { lemma_wf_rows(&**right); }
            let mut cursor = right.clone();
            loop
                invariant
                    (**tree).wf(),
                    drop_key((**tree).entries(), key.view()) == low_rows + high_rows,
                    (**left).entries() == low_rows,
                    (**right).entries() == high_rows,
                    (**right).wf(),
                    ordered_rows(high_rows), high_rows.len() > 0,
                    (*cursor).wf(), (*cursor).entries().len() > 0,
                    (*cursor).entries()[0] == high_rows[0],
                    (*right).depth() < (**tree).depth(),
                    (*cursor).depth() <= (*right).depth(),
                decreases (*cursor).depth(),
            {
                match &*cursor {
                    Tree::Empty => return left.clone(),
                    Tree::Branch { key: next, value: next_value, left: next_left, .. } => {
                        if let Tree::Empty = &**next_left {
                            proof {
                                assert(branch_row(next, next_value) == seq![high_rows[0]]);
                                lemma_drop_first(high_rows);
                                assert(high_rows =~= seq![high_rows[0]] + high_rows.skip(1));
                            }
                            let high = erase(right, next);
                            proof { assert((*high).entries() == high_rows.skip(1)); }
                            let result = balance(next.share(), next_value.clone(), left.clone(), high);
                            proof { assert((*result).entries() =~= low_rows + high_rows); }
                            return finish_erase(result, tree, key);
                        }
                        cursor = next_left.clone();
                    },
                }
            }
        },
    }
}

}
