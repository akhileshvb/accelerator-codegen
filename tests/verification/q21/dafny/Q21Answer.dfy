// The Q21 answer as a set, and the SQL (RelNode Eval_Spec) result as its
// unique strictly sorted enumeration.
//
//   SQLRows_sorted_answer :
//     KeysOk(T) ==>
//       StrictSorted(SQLRows(T, nn)) &&
//       (forall x :: x in SQLRows(T, nn) <==> x in AnsSet(T, NationsNamed(T, nn)))
//
// where SQLRows(T, nn) = ToOuts(AnswerPairs(T, nn)) and
//   AnsSet(T, K) = { OutRow(nm, V(T, K, nm)) | nm a supplier name, V > 0 },
//   V(T, K, nm)  = sum of NW(T, sk) over suppliers sk named nm in nations K.

include "Q21Sorted.dfy"
include "Q21Contrib.dfy"

module Q21Answer {
  import opened Q21Types
  import opened Q21SqlCore
  import opened Q21SqlAgg
  import opened Q21Count
  import opened Q21SetSum
  import opened Q21Contrib
  import opened Q21ModuleTwin
  import opened Q21Sorted
  import opened RelEvalLogicalSpec

  // ----- Supplier / nation views of T (first match; unique under KeysOk) -----

  function SupFirst(S: seq<Sup>, sk: int): Sup
    requires exists s :: s in S && s.sk == sk
  { if S[0].sk == sk then S[0] else SupFirst(S[1..], sk) }

  ghost function SKeys(T: TDB): set<int> { set s | s in T.sup :: s.sk }

  ghost function SupNameOf(T: TDB): map<int, string>
  { map sk | sk in SKeys(T) :: SupFirst(T.sup, sk).name }

  ghost function SupNatOf(T: TDB): map<int, int>
  { map sk | sk in SKeys(T) :: SupFirst(T.sup, sk).nk }

  ghost function NationsNamed(T: TDB, nn: string): set<int>
  { set n | n in T.nats && n.name == nn :: n.nk }

  ghost function V(T: TDB, K: set<int>, nm: string): int
  {
    SetSum(SKeys(T), (sk: int) =>
      if sk in SKeys(T) && SupNameOf(T)[sk] == nm && SupNatOf(T)[sk] in K then NW(T, sk) else 0)
  }

  ghost function SupNames(T: TDB): set<string> { set s | s in T.sup :: s.name }

  ghost function AnsSet(T: TDB, K: set<int>): set<OutRow>
  { set nm | nm in SupNames(T) && V(T, K, nm) > 0 :: OutRow(nm, V(T, K, nm)) }

  ghost function SQLRows(T: TDB, nn: string): seq<OutRow> { ToOuts(AnswerPairs(T, nn)) }

  lemma SupFirst_unique(T: TDB, s: Sup)
    requires SupKeyUnique(T) && s in T.sup
    ensures SupFirst(T.sup, s.sk) == s
  {
    SupFirst_unique_seq(T.sup, s);
  }

  lemma SupFirst_unique_seq(S: seq<Sup>, s: Sup)
    requires forall i, j :: 0 <= i < j < |S| ==> S[i].sk != S[j].sk
    requires s in S
    ensures SupFirst(S, s.sk) == s
  {
    if S[0] != s {
      if S[0].sk == s.sk {
        var j :| 0 <= j < |S| && S[j] == s;
        assert j > 0 && S[0].sk != S[j].sk;
      }
      assert s in S[1..];
      SupFirst_unique_seq(S[1..], s);
    }
  }

  // ----- Sequence sums over unique-keyed suppliers equal set sums -----

  lemma SumF_sup_setsum(S: seq<Sup>, f: Sup -> int, g: int -> int)
    requires forall i, j :: 0 <= i < j < |S| ==> S[i].sk != S[j].sk
    requires forall s :: s in S ==> f(s) == g(s.sk)
    ensures SumF(S, f) == SetSum(set s | s in S :: s.sk, g)
  {
    if |S| == 0 {
      assert (set s | s in S :: s.sk) == {};
    } else {
      SumF_sup_setsum(S[1..], f, g);
      var ks := set s | s in S[1..] :: s.sk;
      assert S[0].sk !in ks by {
        if S[0].sk in ks {
          var s :| s in S[1..] && s.sk == S[0].sk;
          var j :| 0 <= j < |S[1..]| && S[1..][j] == s;
          assert S[0].sk != S[j + 1].sk;
        }
      }
      assert (set s | s in S :: s.sk) == ks + {S[0].sk} by {
        forall x ensures x in (set s | s in S :: s.sk) <==> x in ks + {S[0].sk} {
          if x in (set s | s in S :: s.sk) {
            var s :| s in S && s.sk == x;
            if s != S[0] { assert s in S[1..]; }
          }
        }
      }
      SetSum_add(ks, g, S[0].sk);
    }
  }

  lemma NameSum_is_V(T: TDB, nn: string, nm: string)
    requires KeysOk(T)
    ensures NameSum(T, nn, nm, T.sup) == V(T, NationsNamed(T, nn), nm)
  {
    var K := NationsNamed(T, nn);
    var g := (sk: int) =>
      if sk in SKeys(T) && SupNameOf(T)[sk] == nm && SupNatOf(T)[sk] in K then NW(T, sk) else 0;
    forall s | s in T.sup
      ensures (if s.name == nm && NatNamed(T, s.nk, nn) then NW(T, s.sk) else 0) == g(s.sk)
    {
      SupFirst_unique(T, s);
      assert NatNamed(T, s.nk, nn) <==> s.nk in K;
    }
    SumF_sup_setsum(T.sup, (s: Sup) => if s.name == nm && NatNamed(T, s.nk, nn) then NW(T, s.sk) else 0, g);
  }

  lemma V_pos_name(T: TDB, K: set<int>, nm: string)
    requires V(T, K, nm) != 0
    ensures nm in SupNames(T)
  {
    var g := (sk: int) =>
      if sk in SKeys(T) && SupNameOf(T)[sk] == nm && SupNatOf(T)[sk] in K then NW(T, sk) else 0;
    if nm !in SupNames(T) {
      forall sk | sk in SKeys(T) ensures g(sk) == 0 {
        var s :| s in T.sup && s.sk == sk;
        assert SupFirst(T.sup, sk) in T.sup by { SupFirst_in(T.sup, sk); }
      }
      SetSum_zero(SKeys(T), g);
    }
  }

  lemma SupFirst_in(S: seq<Sup>, sk: int)
    requires exists s :: s in S && s.sk == sk
    ensures SupFirst(S, sk) in S && SupFirst(S, sk).sk == sk
  {
    if S[0].sk != sk {
      var s :| s in S && s.sk == sk;
      assert s in S[1..];
      SupFirst_in(S[1..], sk);
    }
  }

  // ----- The group fold: distinct names, counts = occurrences -----

  function CountS(xs: seq<string>, nm: string): int
  { if |xs| == 0 then 0 else (if xs[0] == nm then 1 else 0) + CountS(xs[1..], nm) }

  lemma CountS_nonneg(xs: seq<string>, nm: string)
    ensures CountS(xs, nm) >= 0
  { if |xs| > 0 { CountS_nonneg(xs[1..], nm); } }

  lemma CountS_app(a: seq<string>, b: seq<string>, nm: string)
    ensures CountS(a + b, nm) == CountS(a, nm) + CountS(b, nm)
  {
    if |a| > 0 { assert (a + b)[1..] == a[1..] + b; CountS_app(a[1..], b, nm); }
    else { assert a + b == b; }
  }

  ghost predicate FoldInv(m: seq<NC>, p: seq<string>)
  {
    (forall i, j :: 0 <= i < j < |m| ==> m[i].name != m[j].name) &&
    (forall e :: e in m ==> e.c == CountS(p, e.name) && e.c > 0) &&
    (forall nm :: CountS(p, nm) > 0 ==> exists e :: e in m && e.name == nm)
  }

  lemma ModelAdd_facts(m: seq<NC>, nm: string)
    requires forall i, j :: 0 <= i < j < |m| ==> m[i].name != m[j].name
    ensures forall i, j :: 0 <= i < j < |ModelAdd(m, nm)| ==> ModelAdd(m, nm)[i].name != ModelAdd(m, nm)[j].name
    ensures forall e :: e in ModelAdd(m, nm) ==>
              (e.name == nm && e.c == ModelCnt(m, nm) + 1) || (e.name != nm && e in m)
    ensures forall e :: e in m && e.name != nm ==> e in ModelAdd(m, nm)
    ensures exists e :: e in ModelAdd(m, nm) && e.name == nm
    ensures (exists e :: e in m && e.name == nm) ==> exists e :: e in m && e.name == nm && e.c == ModelCnt(m, nm)
    decreases |m|
  {
    if |m| > 0 {
      assert forall i, j :: 0 <= i < j < |m[1..]| ==> m[1..][i].name != m[1..][j].name by {
        forall i, j | 0 <= i < j < |m[1..]| ensures m[1..][i].name != m[1..][j].name {
          assert m[i + 1].name != m[j + 1].name;
        }
      }
      if m[0].name != nm {
        ModelAdd_facts(m[1..], nm);
        var r := ModelAdd(m[1..], nm);
        assert ModelAdd(m, nm) == [m[0]] + r;
        forall e | e in r ensures e.name != m[0].name {
          if e.name == nm {} else {
            assert e in m[1..];
            var k :| 0 <= k < |m[1..]| && m[1..][k] == e;
            assert m[0].name != m[k + 1].name;
          }
        }
        forall i, j | 0 <= i < j < |ModelAdd(m, nm)| ensures ModelAdd(m, nm)[i].name != ModelAdd(m, nm)[j].name {
          if i == 0 { assert ModelAdd(m, nm)[j] in r; }
        }
        forall e | e in m && e.name != nm ensures e in ModelAdd(m, nm) {
          if e != m[0] { assert e in m[1..]; }
        }
        if exists e :: e in m && e.name == nm {
          var e :| e in m && e.name == nm;
          assert e in m[1..];
        }
        var w :| w in r && w.name == nm;
        assert w in ModelAdd(m, nm);
      } else {
        var r := [NC(nm, m[0].c + 1)] + m[1..];
        assert ModelAdd(m, nm) == r;
        forall e | e in m[1..] ensures e.name != nm {
          var k :| 0 <= k < |m[1..]| && m[1..][k] == e;
          assert m[0].name != m[k + 1].name;
        }
        forall e | e in m && e.name != nm ensures e in ModelAdd(m, nm) {
          assert e in m[1..];
        }
        assert r[0] in ModelAdd(m, nm);
      }
    } else {
      assert ModelAdd(m, nm)[0] == NC(nm, 1);
      assert ModelAdd(m, nm)[0] in ModelAdd(m, nm);
    }
  }

  lemma ModelFold_inv(m: seq<NC>, p: seq<string>, xs: seq<string>)
    requires FoldInv(m, p)
    ensures FoldInv(ModelFold(m, xs), p + xs)
    decreases |xs|
  {
    if |xs| == 0 {
      assert p + xs == p;
    } else {
      var nm := xs[0];
      var p2 := p + [nm];
      ModelAdd_facts(m, nm);
      var m2 := ModelAdd(m, nm);
      forall nm2 ensures CountS(p2, nm2) == CountS(p, nm2) + (if nm2 == nm then 1 else 0) {
        CountS_app(p, [nm], nm2);
      }
      var k := ModelCnt(m, nm);
      CountS_nonneg(p, nm);
      assert k == CountS(p, nm) by {
        if exists e0 :: e0 in m && e0.name == nm {
          var e0 :| e0 in m && e0.name == nm && e0.c == ModelCnt(m, nm);
        } else {
          ModelCnt_absent(m, nm);
          if CountS(p, nm) > 0 { assert exists e0 :: e0 in m && e0.name == nm; assert false; }
        }
      }
      forall e | e in m2 ensures e.c == CountS(p2, e.name) && e.c > 0 {
        if e.name == nm {
          assert e.c == k + 1;
        } else {
          assert e in m;
        }
      }
      forall nm2 | CountS(p2, nm2) > 0 ensures exists e :: e in m2 && e.name == nm2 {
        if nm2 != nm {
          var e :| e in m && e.name == nm2;
          assert e in m2;
        }
      }
      assert FoldInv(m2, p2);
      ModelFold_inv(m2, p2, xs[1..]);
      assert p2 + xs[1..] == p + xs;
    }
  }

  lemma CountNm_Names(X: seq<Q4>, nm: string)
    ensures CountNm(X, nm) == CountS(Names(X), nm)
  {
    if |X| > 0 { CountNm_Names(X[1..], nm); }
  }

  // ----- SortM: PairLe-sorted permutation -----

  lemma TextCLeb_trans(a: string, b: string, c: string)
    requires TextCLeb(a, b) && TextCLeb(b, c)
    ensures TextCLeb(a, c)
    decreases |a|
  {
    if |a| > 0 && |b| > 0 && |c| > 0 && a[0] == b[0] && b[0] == c[0] {
      TextCLeb_trans(a[1..], b[1..], c[1..]);
    }
  }

  lemma PairLe_trans(a: NC, b: NC, c: NC)
    requires PairLe(a, b) && PairLe(b, c)
    ensures PairLe(a, c)
  {
    if a.c == b.c && b.c == c.c { TextCLeb_trans(a.name, b.name, c.name); }
  }

  lemma PairLe_total(a: NC, b: NC)
    ensures PairLe(a, b) || PairLe(b, a)
  { TextCLeb_total(a.name, b.name); }

  ghost predicate PSorted(xs: seq<NC>) { forall i, j :: 0 <= i < j < |xs| ==> PairLe(xs[i], xs[j]) }

  lemma InsM_facts(x: NC, xs: seq<NC>)
    requires PSorted(xs)
    ensures PSorted(InsM(x, xs))
    ensures multiset(InsM(x, xs)) == multiset([x] + xs)
  {
    if |xs| > 0 {
      if PairLe(x, xs[0]) {
        forall j | 0 <= j < |xs| ensures PairLe(x, xs[j]) { if j > 0 { PairLe_trans(x, xs[0], xs[j]); } }
      } else {
        PairLe_total(x, xs[0]);
        assert PSorted(xs[1..]) by {
          forall i, j | 0 <= i < j < |xs[1..]| ensures PairLe(xs[1..][i], xs[1..][j]) { assert PairLe(xs[i + 1], xs[j + 1]); }
        }
        InsM_facts(x, xs[1..]);
        var r := InsM(x, xs[1..]);
        forall j | 0 <= j < |r| ensures PairLe(xs[0], r[j]) {
          assert r[j] in multiset([x] + xs[1..]);
          if r[j] != x {
            assert r[j] in xs[1..];
            var k :| 0 <= k < |xs[1..]| && xs[1..][k] == r[j];
            assert PairLe(xs[0], xs[k + 1]);
          }
        }
        assert xs == [xs[0]] + xs[1..];
      }
    }
  }

  lemma SortM_facts(xs: seq<NC>)
    ensures PSorted(SortM(xs))
    ensures multiset(SortM(xs)) == multiset(xs)
  {
    if |xs| > 0 {
      SortM_facts(xs[1..]);
      InsM_facts(xs[0], SortM(xs[1..]));
      assert xs == [xs[0]] + xs[1..];
    }
  }

  // ----- Main SQL-side characterization -----

  lemma SQLRows_sorted_answer(T: TDB, nn: string)
    requires KeysOk(T)
    ensures StrictSorted(SQLRows(T, nn))
    ensures forall x :: x in SQLRows(T, nn) <==> x in AnsSet(T, NationsNamed(T, nn))
  {
    var X := Spine(T, nn);
    var G := GroupPairs(T, nn);
    var K := NationsNamed(T, nn);
    assert FoldInv([], []);
    ModelFold_inv([], [], Names(X));
    assert [] + Names(X) == Names(X);
    var A := AnswerPairs(T, nn);
    SortM_facts(G);
    var R := SQLRows(T, nn);
    ToOuts_len_index(A);
    // Element characterization.
    forall e | e in G ensures e.c == V(T, K, e.name) && e.c > 0 && e.name in SupNames(T) {
      CountNm_Names(X, e.name);
      SpineNameCount(T, nn, e.name);
      NameSum_is_V(T, nn, e.name);
      V_pos_name(T, K, e.name);
    }
    forall x ensures x in R <==> x in AnsSet(T, K) {
      if x in R {
        var i :| 0 <= i < |R| && R[i] == x;
        assert A[i] in multiset(G);
      }
      if x in AnsSet(T, K) {
        var nm := x.name;
        CountNm_Names(X, nm);
        SpineNameCount(T, nn, nm);
        NameSum_is_V(T, nn, nm);
        var e :| e in G && e.name == nm;
        assert e in multiset(A);
        var i :| 0 <= i < |A| && A[i] == e;
        assert R[i] == ToOut(e);
      }
    }
    // Strict sortedness: PairLe on distinct names is Better.
    forall i, j | 0 <= i < j < |R| ensures Better(R[i], R[j]) {
      assert A[i] in multiset(G) && A[j] in multiset(G);
      assert multiset(A)[A[i]] == multiset(G)[A[i]];
      // distinct positions carry distinct names (names are distinct in G)
      if A[i].name == A[j].name {
        assert A[i].c == A[j].c;
        assert A[i] == A[j];
        DistinctMultiset(G, A[i]);
        assert multiset(A)[A[i]] >= 2 by { TwoPositions(A, i, j); }
        assert false;
      }
      TextCLeb_StrLt(A[i].name, A[j].name);
    }
  }

  lemma DistinctMultiset(G: seq<NC>, e: NC)
    requires forall i, j :: 0 <= i < j < |G| ==> G[i].name != G[j].name
    ensures multiset(G)[e] <= 1
    decreases |G|
  {
    if |G| > 0 {
      DistinctMultiset(G[1..], e);
      assert G == [G[0]] + G[1..];
      if G[0] == e {
        assert e !in G[1..] by {
          if e in G[1..] {
            var k :| 0 <= k < |G[1..]| && G[1..][k] == e;
            assert G[0].name != G[k + 1].name;
          }
        }
      }
    }
  }

  lemma TwoPositions<X>(A: seq<X>, i: int, j: int)
    requires 0 <= i < j < |A| && A[i] == A[j]
    ensures multiset(A)[A[i]] >= 2
  {
    assert A == A[..i] + [A[i]] + A[i+1..j] + [A[j]] + A[j+1..];
  }
}
