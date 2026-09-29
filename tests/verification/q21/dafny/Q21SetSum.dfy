// Finite sums over integer key sets (query-independent helper for Q21).

module Q21SetSum {

  ghost function SetSum(s: set<int>, f: int -> int): int
    decreases |s|
  {
    if s == {} then 0
    else
      var x :| x in s;
      f(x) + SetSum(s - {x}, f)
  }

  lemma SetSum_remove(s: set<int>, f: int -> int, y: int)
    requires y in s
    ensures SetSum(s, f) == f(y) + SetSum(s - {y}, f)
    decreases |s|
  {
    var x :| x in s && SetSum(s, f) == f(x) + SetSum(s - {x}, f);
    if x != y {
      SetSum_remove(s - {x}, f, y);
      SetSum_remove(s - {y}, f, x);
      assert s - {x} - {y} == s - {y} - {x};
    }
  }

  lemma SetSum_empty(f: int -> int)
    ensures SetSum({}, f) == 0
  {}

  lemma SetSum_single(x: int, f: int -> int)
    ensures SetSum({x}, f) == f(x)
  {
    SetSum_remove({x}, f, x);
    assert {x} - {x} == {};
  }

  lemma SetSum_ext(s: set<int>, f: int -> int, g: int -> int)
    requires forall x :: x in s ==> f(x) == g(x)
    ensures SetSum(s, f) == SetSum(s, g)
    decreases |s|
  {
    if s != {} {
      var x :| x in s;
      SetSum_remove(s, f, x);
      SetSum_remove(s, g, x);
      SetSum_ext(s - {x}, f, g);
    }
  }

  lemma SetSum_add(s: set<int>, f: int -> int, y: int)
    requires y !in s
    ensures SetSum(s + {y}, f) == f(y) + SetSum(s, f)
  {
    SetSum_remove(s + {y}, f, y);
    assert s + {y} - {y} == s;
  }

  lemma SetSum_union(a: set<int>, b: set<int>, f: int -> int)
    requires a !! b
    ensures SetSum(a + b, f) == SetSum(a, f) + SetSum(b, f)
    decreases |b|
  {
    if b != {} {
      var y :| y in b;
      SetSum_union(a, b - {y}, f);
      assert a + b == (a + (b - {y})) + {y};
      SetSum_add(a + (b - {y}), f, y);
      SetSum_remove(b, f, y);
    } else {
      assert a + b == a;
    }
  }

  lemma SetSum_zero(s: set<int>, f: int -> int)
    requires forall x :: x in s ==> f(x) == 0
    ensures SetSum(s, f) == 0
    decreases |s|
  {
    if s != {} {
      var x :| x in s;
      SetSum_remove(s, f, x);
      SetSum_zero(s - {x}, f);
    }
  }

  // Extending the index set by keys where f is zero does not change the sum.
  lemma SetSum_superset(s: set<int>, t: set<int>, f: int -> int)
    requires s <= t
    requires forall x :: x in t && x !in s ==> f(x) == 0
    ensures SetSum(t, f) == SetSum(s, f)
  {
    assert t == s + (t - s);
    SetSum_union(s, t - s, f);
    SetSum_zero(t - s, f);
  }

  lemma SetSum_nonneg(s: set<int>, f: int -> int)
    requires forall x :: x in s ==> f(x) >= 0
    ensures SetSum(s, f) >= 0
    decreases |s|
  {
    if s != {} {
      var x :| x in s;
      SetSum_remove(s, f, x);
      SetSum_nonneg(s - {x}, f);
    }
  }

  lemma SetSum_plus(s: set<int>, f: int -> int, g: int -> int)
    ensures SetSum(s, x => f(x) + g(x)) == SetSum(s, f) + SetSum(s, g)
    decreases |s|
  {
    if s != {} {
      var x :| x in s;
      SetSum_remove(s, x => f(x) + g(x), x);
      SetSum_remove(s, f, x);
      SetSum_remove(s, g, x);
      SetSum_plus(s - {x}, f, g);
    }
  }

  lemma SetSum_minus(s: set<int>, f: int -> int, g: int -> int)
    ensures SetSum(s, x => f(x) - g(x)) == SetSum(s, f) - SetSum(s, g)
    decreases |s|
  {
    if s != {} {
      var x :| x in s;
      SetSum_remove(s, x => f(x) - g(x), x);
      SetSum_remove(s, f, x);
      SetSum_remove(s, g, x);
      SetSum_minus(s - {x}, f, g);
    }
  }

  // If f and g agree outside K, the sums differ by the K-local differences.
  lemma SetSum_local_change(s: set<int>, K: set<int>, f: int -> int, g: int -> int)
    requires K <= s
    requires forall x :: x in s && x !in K ==> f(x) == g(x)
    ensures SetSum(s, g) == SetSum(s, f) + SetSum(K, x => g(x) - f(x))
  {
    assert s == K + (s - K);
    SetSum_union(K, s - K, f);
    SetSum_union(K, s - K, g);
    SetSum_ext(s - K, f, g);
    SetSum_minus(K, g, f);
  }
}
