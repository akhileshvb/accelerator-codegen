// GENERIC: sorted sequences of unique (key, value) entries.
//
// Core theorem SameMapSameSortedSeq:
//   same unique key→value map + both strictly sorted by key ⇒ identical sequences.

module SortedUniqueMap {

  datatype Entry<V> = Entry(key: int, val: V)

  ghost predicate Sorted<V>(rows: seq<Entry<V>>) {
    forall i :: 0 <= i < |rows| - 1 ==> rows[i].key <= rows[i + 1].key
  }

  ghost predicate StrictSorted<V>(rows: seq<Entry<V>>) {
    forall i :: 0 <= i < |rows| - 1 ==> rows[i].key < rows[i + 1].key
  }

  ghost predicate UniqueKeys<V>(rows: seq<Entry<V>>) {
    forall i, j :: 0 <= i < j < |rows| ==> rows[i].key != rows[j].key
  }

  ghost predicate HasKey<V>(rows: seq<Entry<V>>, k: int) {
    exists i :: 0 <= i < |rows| && rows[i].key == k
  }

  function InsertSorted<V>(rows: seq<Entry<V>>, row: Entry<V>): seq<Entry<V>>
    decreases |rows|
  {
    if |rows| == 0 then
      [row]
    else if row.key < rows[0].key then
      [row] + rows
    else
      [rows[0]] + InsertSorted(rows[1..], row)
  }

  function SortByKey<V>(rows: seq<Entry<V>>): seq<Entry<V>>
    decreases |rows|
  {
    if |rows| == 0 then
      []
    else
      InsertSorted(SortByKey(rows[1..]), rows[0])
  }

  lemma InsertSorted_sorted<V>(rows: seq<Entry<V>>, row: Entry<V>)
    requires Sorted(rows)
    ensures Sorted(InsertSorted(rows, row))
    decreases |rows|
  {
    if |rows| == 0 {
    } else if row.key < rows[0].key {
    } else {
      assert Sorted(rows[1..]);
      InsertSorted_sorted(rows[1..], row);
    }
  }

  lemma SortByKey_sorted<V>(rows: seq<Entry<V>>)
    ensures Sorted(SortByKey(rows))
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      SortByKey_sorted(rows[1..]);
      InsertSorted_sorted(SortByKey(rows[1..]), rows[0]);
    }
  }

  lemma Strict_drop_head<V>(rows: seq<Entry<V>>)
    requires StrictSorted(rows)
    requires |rows| > 0
    ensures StrictSorted(rows[1..])
    ensures forall i :: 0 < i < |rows| ==> rows[0].key < rows[i].key
  {
    forall i | 0 < i < |rows|
      ensures rows[0].key < rows[i].key
    {
      var t := 0;
      while t < i - 1
        invariant 0 <= t < i
        invariant rows[0].key < rows[t + 1].key
        decreases i - 1 - t
      {
        assert rows[t + 1].key < rows[t + 2].key;
        t := t + 1;
      }
    }
  }

  lemma SortedUnique_implies_Strict<V>(rows: seq<Entry<V>>)
    requires Sorted(rows)
    requires UniqueKeys(rows)
    ensures StrictSorted(rows)
  {
    forall i | 0 <= i < |rows| - 1
      ensures rows[i].key < rows[i + 1].key
    {
      assert rows[i].key <= rows[i + 1].key;
      assert rows[i].key != rows[i + 1].key;
    }
  }

  lemma StrictSorted_implies_Unique<V>(rows: seq<Entry<V>>)
    requires StrictSorted(rows)
    ensures UniqueKeys(rows)
    decreases |rows|
  {
    if |rows| <= 1 {
    } else {
      Strict_drop_head(rows);
      StrictSorted_implies_Unique(rows[1..]);
      forall i, j | 0 <= i < j < |rows|
        ensures rows[i].key != rows[j].key
      {
        if i == 0 {
          assert rows[0].key < rows[j].key;
        } else {
          assert rows[1..][i - 1].key != rows[1..][j - 1].key;
        }
      }
    }
  }

  lemma SameKeys_nonempty_heads<V>(a: seq<Entry<V>>, b: seq<Entry<V>>)
    requires StrictSorted(a) && StrictSorted(b)
    requires |a| > 0 && |b| > 0
    requires forall k :: HasKey(a, k) <==> HasKey(b, k)
    ensures a[0].key == b[0].key
  {
    Strict_drop_head(a);
    Strict_drop_head(b);
    assert HasKey(a, a[0].key);
    assert HasKey(b, a[0].key);
    var jb :| 0 <= jb < |b| && b[jb].key == a[0].key;
    assert jb == 0 || b[0].key < b[jb].key;
    assert HasKey(b, b[0].key);
    assert HasKey(a, b[0].key);
    var ia :| 0 <= ia < |a| && a[ia].key == b[0].key;
    assert ia == 0 || a[0].key < a[ia].key;
    if a[0].key < b[0].key {
      assert b[jb].key == a[0].key < b[0].key;
      assert false;
    } else if b[0].key < a[0].key {
      assert a[ia].key == b[0].key < a[0].key;
      assert false;
    }
  }

  lemma SameMapSameSortedSeq_tails<V>(a: seq<Entry<V>>, b: seq<Entry<V>>)
    requires StrictSorted(a) && StrictSorted(b)
    requires |a| > 0 && |b| > 0
    requires a[0].key == b[0].key
    requires forall k :: HasKey(a, k) <==> HasKey(b, k)
    ensures forall k :: HasKey(a[1..], k) <==> HasKey(b[1..], k)
  {
    Strict_drop_head(a);
    Strict_drop_head(b);
    forall k
      ensures HasKey(a[1..], k) <==> HasKey(b[1..], k)
    {
      if HasKey(a[1..], k) {
        assert HasKey(a, k);
        assert HasKey(b, k);
        StrictSorted_implies_Unique(a);
        assert k != a[0].key;
      }
      if HasKey(b[1..], k) {
        assert HasKey(b, k);
        assert HasKey(a, k);
        StrictSorted_implies_Unique(b);
        assert k != b[0].key;
      }
    }
  }

  // ===== Main reusable theorem =====
  lemma SameMapSameSortedSeq<V>(a: seq<Entry<V>>, b: seq<Entry<V>>)
    requires StrictSorted(a) && StrictSorted(b)
    requires forall k {:trigger HasKey(a, k)} {:trigger HasKey(b, k)} ::
               HasKey(a, k) <==> HasKey(b, k)
    requires forall i, j :: 0 <= i < |a| && 0 <= j < |b| && a[i].key == b[j].key ==>
               a[i].val == b[j].val
    ensures a == b
    decreases |a|
  {
    if |a| == 0 {
      if |b| > 0 {
        assert HasKey(b, b[0].key);
        assert HasKey(a, b[0].key);
        assert false;
      }
    } else if |b| == 0 {
      assert HasKey(a, a[0].key);
      assert HasKey(b, a[0].key);
      assert false;
    } else {
      SameKeys_nonempty_heads(a, b);
      assert a[0].key == b[0].key;
      assert a[0].val == b[0].val;
      Strict_drop_head(a);
      Strict_drop_head(b);
      SameMapSameSortedSeq_tails(a, b);
      SameMapSameSortedSeq(a[1..], b[1..]);
    }
  }
}
