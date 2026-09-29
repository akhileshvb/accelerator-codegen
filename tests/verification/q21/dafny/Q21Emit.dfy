// Output-path algebra of the Twin (query-generic in T):
//   MergeByName_sorted : MergeByNameAndSort(rows) is StrictSorted and its
//                        elements are exactly (nm, SumRows(rows, nm)) for the
//                        names occurring in rows.
//   NationRows_sum     : SumRows over NationRowsLoop(nw, names, en) equals the
//                        set-sum over dom nw of the positive, named entries.

include "Q21Sorted.dfy"
include "Q21SetSum.dfy"
include "Q21Count.dfy"

module Q21Emit {
  import opened Q21ModuleTwin
  import opened Q21Sorted
  import opened Q21SetSum
  import opened Q21Count

  ghost function SumRows(rows: seq<OutRow>, nm: string): int
  { SumF(rows, (r: OutRow) => if r.name == nm then r.numwait else 0) }

  ghost function NamesOf(rows: seq<OutRow>): set<string> { set r | r in rows :: r.name }

  function Lookup(acc: seq<OutRow>, nm: string): int
  { if |acc| == 0 then 0 else if acc[0].name == nm then acc[0].numwait else Lookup(acc[1..], nm) }

  lemma Lookup_absent(acc: seq<OutRow>, nm: string)
    requires nm !in NamesOf(acc)
    ensures Lookup(acc, nm) == 0
  {
    if |acc| > 0 { assert acc[0] in acc; Lookup_absent(acc[1..], nm); }
  }

  lemma Lookup_member(acc: seq<OutRow>, x: OutRow)
    requires DistinctNames(acc) && x in acc
    ensures Lookup(acc, x.name) == x.numwait
  {
    if acc[0] != x {
      if acc[0].name == x.name {
        var k :| 0 <= k < |acc| && acc[k] == x;
        assert k > 0 && acc[0].name != acc[k].name;
      }
      assert x in acc[1..];
      assert DistinctNames(acc[1..]) by {
        forall i, j | 0 <= i < j < |acc[1..]| ensures acc[1..][i].name != acc[1..][j].name {
          assert acc[i + 1].name != acc[j + 1].name;
        }
      }
      Lookup_member(acc[1..], x);
    }
  }

  lemma Distinct_tail(acc: seq<OutRow>)
    requires |acc| > 0 && DistinctNames(acc)
    ensures DistinctNames(acc[1..])
    ensures forall y :: y in acc[1..] ==> y.name != acc[0].name
  {
    forall i, j | 0 <= i < j < |acc[1..]| ensures acc[1..][i].name != acc[1..][j].name {
      assert acc[i + 1].name != acc[j + 1].name;
    }
    forall y | y in acc[1..] ensures y.name != acc[0].name {
      var k :| 0 <= k < |acc[1..]| && acc[1..][k] == y;
      assert acc[0].name != acc[k + 1].name;
    }
  }

  lemma AddByName_names(acc: seq<OutRow>, r: OutRow)
    ensures NamesOf(AddByName(acc, r)) == NamesOf(acc) + {r.name}
    decreases |acc|
  {
    if |acc| > 0 {
      assert acc == [acc[0]] + acc[1..];
      assert NamesOf(acc) == {acc[0].name} + NamesOf(acc[1..]);
      if acc[0].name != r.name {
        AddByName_names(acc[1..], r);
        assert AddByName(acc, r) == [acc[0]] + AddByName(acc[1..], r);
        assert NamesOf(AddByName(acc, r)) == {acc[0].name} + NamesOf(AddByName(acc[1..], r));
      } else {
        assert AddByName(acc, r) == [OutRow(r.name, acc[0].numwait + r.numwait)] + acc[1..];
        assert NamesOf(AddByName(acc, r)) == {r.name} + NamesOf(acc[1..]);
      }
    } else {
      assert AddByName(acc, r) == [r];
    }
  }

  lemma AddByName_distinct(acc: seq<OutRow>, r: OutRow)
    requires DistinctNames(acc)
    ensures DistinctNames(AddByName(acc, r))
    decreases |acc|
  {
    if |acc| > 0 {
      Distinct_tail(acc);
      var res := AddByName(acc, r);
      if acc[0].name == r.name {
        assert res == [OutRow(r.name, acc[0].numwait + r.numwait)] + acc[1..];
        forall i, j | 0 <= i < j < |res| ensures res[i].name != res[j].name {
          if i == 0 { assert res[j] in acc[1..]; } else { assert res[i] == acc[i] && res[j] == acc[j]; }
        }
      } else {
        AddByName_distinct(acc[1..], r);
        AddByName_names(acc[1..], r);
        var rest := AddByName(acc[1..], r);
        assert res == [acc[0]] + rest;
        forall y | y in rest ensures y.name != acc[0].name {
          assert y.name in NamesOf(rest);
          if y.name != r.name {
            var z :| z in acc[1..] && z.name == y.name;
          }
        }
        forall i, j | 0 <= i < j < |res| ensures res[i].name != res[j].name {
          if i == 0 { assert res[j] in rest; }
          else { assert res[i] == rest[i - 1] && res[j] == rest[j - 1]; }
        }
      }
    }
  }

  lemma AddByName_lookup(acc: seq<OutRow>, r: OutRow, nm: string)
    requires DistinctNames(acc)
    ensures Lookup(AddByName(acc, r), nm) == Lookup(acc, nm) + (if r.name == nm then r.numwait else 0)
    decreases |acc|
  {
    if |acc| > 0 {
      Distinct_tail(acc);
      if acc[0].name == r.name {
        if nm == r.name { Lookup_absent(acc[1..], nm); }
      } else {
        AddByName_lookup(acc[1..], r, nm);
      }
    }
  }

  lemma AddByName_facts(acc: seq<OutRow>, r: OutRow)
    requires DistinctNames(acc)
    ensures DistinctNames(AddByName(acc, r))
    ensures NamesOf(AddByName(acc, r)) == NamesOf(acc) + {r.name}
    ensures forall nm :: Lookup(AddByName(acc, r), nm) ==
                         Lookup(acc, nm) + (if r.name == nm then r.numwait else 0)
  {
    AddByName_distinct(acc, r);
    AddByName_names(acc, r);
    forall nm ensures Lookup(AddByName(acc, r), nm) == Lookup(acc, nm) + (if r.name == nm then r.numwait else 0) {
      AddByName_lookup(acc, r, nm);
    }
  }

  lemma MergeRows_facts(acc: seq<OutRow>, rows: seq<OutRow>)
    requires DistinctNames(acc)
    ensures DistinctNames(MergeRows(acc, rows))
    ensures NamesOf(MergeRows(acc, rows)) == NamesOf(acc) + NamesOf(rows)
    ensures forall nm :: Lookup(MergeRows(acc, rows), nm) == Lookup(acc, nm) + SumRows(rows, nm)
    decreases |rows|
  {
    if |rows| == 0 {
      assert NamesOf(rows) == {};
    } else {
      AddByName_facts(acc, rows[0]);
      MergeRows_facts(AddByName(acc, rows[0]), rows[1..]);
      assert rows == [rows[0]] + rows[1..];
      assert NamesOf(rows) == {rows[0].name} + NamesOf(rows[1..]);
    }
  }

  lemma MergeByName_sorted(rows: seq<OutRow>)
    ensures StrictSorted(MergeByNameAndSort(rows))
    ensures forall x :: x in MergeByNameAndSort(rows) <==>
              (x.name in NamesOf(rows) && x.numwait == SumRows(rows, x.name))
  {
    var m := MergeRows([], rows);
    MergeRows_facts([], rows);
    assert NamesOf([]) == {};
    SortBetter_sorted(m);
    SortBetter_perm(m);
    forall x ensures x in MergeByNameAndSort(rows) <==> (x.name in NamesOf(rows) && x.numwait == SumRows(rows, x.name)) {
      if x in MergeByNameAndSort(rows) {
        assert x in multiset(m);
        Lookup_member(m, x);
      }
      if x.name in NamesOf(rows) && x.numwait == SumRows(rows, x.name) {
        var y :| y in m && y.name == x.name;
        Lookup_member(m, y);
        assert y == x;
        assert x in multiset(SortBetter(m));
      }
    }
  }

  // ----- Nation row loops as set sums -----

  lemma SumRows_app(a: seq<OutRow>, b: seq<OutRow>, nm: string)
    ensures SumRows(a + b, nm) == SumRows(a, nm) + SumRows(b, nm)
  { SumF_app(a, b, (r: OutRow) => if r.name == nm then r.numwait else 0); }

  ghost function RowTerm(nw: map<int, int>, names: map<int, string>, nm: string): int -> int
  { (sk: int) => if sk in nw && nw[sk] > 0 && sk in names && names[sk] == nm then nw[sk] else 0 }

  lemma NationRows_sum(nw: map<int, int>, names: map<int, string>, en: seq<int>, nm: string)
    requires IsEnum(en, nw.Keys)
    ensures SumRows(NationRowsLoop(nw, names, en), nm) == SetSum(nw.Keys, RowTerm(nw, names, nm))
    decreases |en|
  {
    if |en| == 0 {
      assert nw.Keys == {} by { forall x | x in nw.Keys ensures false { assert x in en; } }
    } else {
      var sk := en[0];
      var ks := nw.Keys - {sk};
      assert IsEnum(en[1..], ks) by {
        forall x ensures x in en[1..] <==> x in ks {
          if x in en[1..] {
            var k :| 0 <= k < |en[1..]| && en[1..][k] == x;
            assert en[0] != en[k + 1];
          }
          if x in ks { assert x in en; if x != en[0] { assert x in en[1..]; } }
        }
      }
      var nw' := map k | k in ks :: nw[k];
      assert nw'.Keys == ks;
      // The loop over en[1..] reads nw only at keys in ks.
      NationRowsLoop_restrict(nw, nw', names, en[1..]);
      NationRows_sum(nw', names, en[1..], nm);
      SetSum_ext(ks, RowTerm(nw', names, nm), RowTerm(nw, names, nm));
      SetSum_remove(nw.Keys, RowTerm(nw, names, nm), sk);
      var head := if nw[sk] <= 0 then [] else if sk in names then [OutRow(names[sk], nw[sk])] else [];
      assert NationRowsLoop(nw, names, en) == head + NationRowsLoop(nw, names, en[1..]);
      SumRows_app(head, NationRowsLoop(nw, names, en[1..]), nm);
    }
  }

  lemma NationRowsLoop_restrict(nw: map<int, int>, nw': map<int, int>, names: map<int, string>, en: seq<int>)
    requires forall x :: x in en ==> x in nw' && x in nw && nw'[x] == nw[x]
    ensures NationRowsLoop(nw, names, en) == NationRowsLoop(nw', names, en)
    decreases |en|
  {
    if |en| > 0 { NationRowsLoop_restrict(nw, nw', names, en[1..]); }
  }

  lemma NationRowsLoop_pos(nw: map<int, int>, names: map<int, string>, en: seq<int>)
    requires forall x :: x in en ==> x in nw
    ensures forall r :: r in NationRowsLoop(nw, names, en) ==> r.numwait > 0
    decreases |en|
  {
    if |en| > 0 { NationRowsLoop_pos(nw, names, en[1..]); }
  }
}
