// Output-order facts shared by the SQL side and the Twin:
//   * SortBetter (twin of std::sort with BetterOutputRow) yields a strictly
//     Better-sorted permutation when names are distinct;
//   * strictly sorted sequences with the same element set are equal
//     (so every correct sort of the merged rows gives the same result);
//   * TextCLeb (RelNode / FormalSQL C-collation order) and StrLt (C++
//     std::string operator<) agree on distinct strings.

include "Q21ModuleTwin.dfy"
include "Q21SqlAgg.dfy"

module Q21Sorted {
  import opened Q21ModuleTwin
  import opened RelEvalLogicalSpec
  import Agg = Q21SqlAgg

  ghost predicate StrictSorted(xs: seq<OutRow>)
  { forall i, j :: 0 <= i < j < |xs| ==> Better(xs[i], xs[j]) }

  ghost predicate DistinctNames(xs: seq<OutRow>)
  { forall i, j :: 0 <= i < j < |xs| ==> xs[i].name != xs[j].name }

  lemma Better_trans(a: OutRow, b: OutRow, c: OutRow)
    requires Better(a, b) && Better(b, c)
    ensures Better(a, c)
  {
    if a.numwait == b.numwait && b.numwait == c.numwait { StrLt_trans(a.name, b.name, c.name); }
  }

  lemma Better_irrefl(a: OutRow)
    ensures !Better(a, a)
  { StrLt_irrefl(a.name); }

  lemma Better_asym(a: OutRow, b: OutRow)
    requires Better(a, b)
    ensures !Better(b, a)
  {
    if a.numwait == b.numwait && Better(b, a) { StrLt_trans(a.name, b.name, a.name); StrLt_irrefl(a.name); }
  }

  lemma Better_total(a: OutRow, b: OutRow)
    requires a.name != b.name
    ensures Better(a, b) || Better(b, a)
  { StrLt_total(a.name, b.name); }

  // ----- SortBetter -----

  lemma InsertBetter_perm(x: OutRow, xs: seq<OutRow>)
    ensures multiset(InsertBetter(x, xs)) == multiset([x] + xs)
  {
    if |xs| > 0 && Better(xs[0], x) {
      InsertBetter_perm(x, xs[1..]);
      assert xs == [xs[0]] + xs[1..];
    }
  }

  lemma SortBetter_perm(xs: seq<OutRow>)
    ensures multiset(SortBetter(xs)) == multiset(xs)
  {
    if |xs| > 0 {
      SortBetter_perm(xs[1..]);
      InsertBetter_perm(xs[0], SortBetter(xs[1..]));
      assert xs == [xs[0]] + xs[1..];
    }
  }

  lemma InsertBetter_sorted(x: OutRow, xs: seq<OutRow>)
    requires StrictSorted(xs)
    requires forall y :: y in xs ==> y.name != x.name
    ensures StrictSorted(InsertBetter(x, xs))
  {
    if |xs| > 0 {
      if Better(xs[0], x) {
        InsertBetter_sorted(x, xs[1..]);
        InsertBetter_perm(x, xs[1..]);
        var r := InsertBetter(x, xs[1..]);
        forall j | 0 <= j < |r| ensures Better(xs[0], r[j]) {
          assert r[j] in multiset([x] + xs[1..]);
          if r[j] != x {
            assert r[j] in xs[1..];
            var k :| 0 <= k < |xs[1..]| && xs[1..][k] == r[j];
            assert Better(xs[0], xs[k + 1]);
          }
        }
        assert InsertBetter(x, xs) == [xs[0]] + r;
      } else {
        Better_total(x, xs[0]);
        forall j | 0 <= j < |xs| ensures Better(x, xs[j]) {
          if j > 0 { Better_trans(x, xs[0], xs[j]); }
        }
      }
    }
  }

  lemma SortBetter_sorted(xs: seq<OutRow>)
    requires DistinctNames(xs)
    ensures StrictSorted(SortBetter(xs))
  {
    if |xs| > 0 {
      assert DistinctNames(xs[1..]) by {
        forall i, j | 0 <= i < j < |xs[1..]| ensures xs[1..][i].name != xs[1..][j].name {
          assert xs[i + 1].name != xs[j + 1].name;
        }
      }
      SortBetter_sorted(xs[1..]);
      SortBetter_perm(xs[1..]);
      forall y | y in SortBetter(xs[1..]) ensures y.name != xs[0].name {
        assert y in multiset(xs[1..]);
        var k :| 0 <= k < |xs[1..]| && xs[1..][k] == y;
        assert xs[0].name != xs[k + 1].name;
      }
      InsertBetter_sorted(xs[0], SortBetter(xs[1..]));
    }
  }

  // ----- Uniqueness of the strictly sorted enumeration -----

  lemma StrictSorted_unique(a: seq<OutRow>, b: seq<OutRow>)
    requires StrictSorted(a) && StrictSorted(b)
    requires forall x :: x in a <==> x in b
    ensures a == b
    decreases |a|
  {
    if |a| == 0 {
      if |b| > 0 { assert b[0] in b; }
    } else if |b| == 0 {
      assert a[0] in a;
    } else {
      // a[0] and b[0] are both the Better-maximum of the common set.
      if a[0] != b[0] {
        assert a[0] in b;
        var i :| 0 <= i < |b| && b[i] == a[0];
        assert b[0] in a;
        var j :| 0 <= j < |a| && a[j] == b[0];
        assert i > 0 && j > 0;
        assert Better(b[0], a[0]) && Better(a[0], b[0]);
        Better_asym(b[0], a[0]);
      }
      forall x ensures x in a[1..] <==> x in b[1..] {
        if x in a[1..] {
          var k :| 0 <= k < |a[1..]| && a[1..][k] == x;
          assert Better(a[0], a[k + 1]);
          if x == b[0] { Better_irrefl(x); }
          assert x in b;
        }
        if x in b[1..] {
          var k :| 0 <= k < |b[1..]| && b[1..][k] == x;
          assert Better(b[0], b[k + 1]);
          if x == a[0] { Better_irrefl(x); }
          assert x in a;
        }
      }
      StrictSorted_unique(a[1..], b[1..]);
      assert a == [a[0]] + a[1..] && b == [b[0]] + b[1..];
    }
  }

  // ----- Order bridge: TextCLeb (SQL side) vs StrLt (C++ side) -----

  lemma TextCLeb_StrLt(x: string, y: string)
    requires x != y
    ensures TextCLeb(x, y) <==> StrLt(x, y)
    decreases |x|
  {
    if |x| > 0 && |y| > 0 && x[0] == y[0] {
      assert x[1..] != y[1..] by { if x[1..] == y[1..] { assert x == [x[0]] + x[1..] && y == [y[0]] + y[1..]; } }
      TextCLeb_StrLt(x[1..], y[1..]);
    }
  }

  function ToOut(x: Agg.NC): OutRow { OutRow(x.name, x.c) }
  function ToOuts(xs: seq<Agg.NC>): seq<OutRow>
  { if |xs| == 0 then [] else [ToOut(xs[0])] + ToOuts(xs[1..]) }

  lemma ToOuts_len_index(xs: seq<Agg.NC>)
    ensures |ToOuts(xs)| == |xs|
    ensures forall i :: 0 <= i < |xs| ==> ToOuts(xs)[i] == ToOut(xs[i])
  { if |xs| > 0 { ToOuts_len_index(xs[1..]); } }
}
