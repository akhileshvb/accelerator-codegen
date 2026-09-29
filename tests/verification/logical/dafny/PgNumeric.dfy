// PostgreSQL NUMERIC division / avg(numeric), query-generic.
//
// Integer model of FormalSQL's numeric_div_at_scales for the operands avg()
// uses (Rocq: formalsql/lib/NumericAggExact.v proves
//   numeric_div_at_scales (numeric_of_scaled sum s) s (numeric_of_Z n) 0
//     = Some (numeric_of_scaled (AvgOfScaledSum(sum,s,n).coeff) (...scale))
// using the identically named definitions pg_avg_* there).
// C++ twin: include/accelerator_codegen/stateful/core/pg_numeric.hpp.
//
//   weight      numeric_pg_weight      base-10000 weight, floor division
//   FirstDigit  numeric_pg_first_digit leading base-10000 digit
//   DivScale    numeric_pg_div_scale   PostgreSQL select_div_scale
//   RoundHalfAway numeric_round_quot   half away from zero
module PgNumeric {
  const DecDigits: int := 4
  const MinSigDigits: int := 16
  const MinDisplayScale: int := 0
  const MaxDisplayScale: int := 1000

  // value = coeff / 10^scale, displayed with `scale` fractional digits
  datatype Decimal = Decimal(coeff: int, scale: int)

  function Pow10(n: nat): nat
    ensures Pow10(n) >= 1
  {
    if n == 0 then 1 else 10 * Pow10(n - 1)
  }

  function Abs(z: int): nat { if z < 0 then -z else z }

  function Max(a: int, b: int): int { if a < b then b else a }
  function Min(a: int, b: int): int { if a < b then a else b }

  function DigitCount(a: nat): nat
    decreases a
  {
    if a < 10 then 1 else 1 + DigitCount(a / 10)
  }

  // Dafny `/` with a positive divisor is floor division (Z.div).
  function Weight(coeff: int, scale: int): int
  {
    if coeff == 0 then 0 else (DigitCount(Abs(coeff)) - 1 - scale) / DecDigits
  }

  lemma MulNat(a: nat, b: nat)
    ensures a * b >= 0
  {}

  function ShiftPow10(a: nat, e: int): nat
  {
    if e >= 0 then a / Pow10(e)
    else
      var p := Pow10(-e);
      MulNat(a, p);
      a * p
  }

  function FirstDigit(coeff: int, scale: int): nat
  {
    if coeff == 0 then 0
    else ShiftPow10(Abs(coeff), scale + Weight(coeff, scale) * DecDigits)
  }

  function DivScale(c1: int, s1: int, c2: int, s2: int): int
  {
    var w1 := Weight(c1, s1);
    var w2 := Weight(c2, s2);
    var qweight := if FirstDigit(c1, s1) <= FirstDigit(c2, s2) then w1 - w2 - 1 else w1 - w2;
    var r := MinSigDigits - qweight * DecDigits;
    Min(Max(Max(Max(r, s1), s2), MinDisplayScale), MaxDisplayScale)
  }

  function RoundHalfAway(num: int, den: int): int
    requires den > 0
  {
    var q := Abs(num) / den;
    var r := Abs(num) % den;
    var m := if 2 * r >= den then q + 1 else q;
    if num < 0 then -m else m
  }

  lemma DivScale_ge_left(c1: int, s1: int, c2: int, s2: int)
    requires s1 <= MaxDisplayScale
    ensures s1 <= DivScale(c1, s1, c2, s2) <= MaxDisplayScale
  {}

  // avg(numeric) of values at display scale s with coefficient sum `sum` and
  // count n (numeric_avg_from_scale_state, finite case).
  function AvgOfScaledSum(sum: int, s: nat, n: int): Decimal
    requires n > 0 && s <= MaxDisplayScale
  {
    DivScale_ge_left(sum, s, n, 0);
    var rs := DivScale(sum, s, n, 0);
    Decimal(RoundHalfAway(sum * Pow10(rs - s), n), rs)
  }
}
