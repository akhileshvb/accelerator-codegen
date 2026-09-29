// Q1ExecExact — Q1 execution against the RelNode Eval_Spec, exactly
// (same recipe as Q21: Q21Inv.Execute_correct / Q21Build.ExecuteFallback_correct).
//
//   Q1Rows(c, db) := the accelerator rows observed from
//                    Eval_Spec(LogicalSpec_Q1_rel, cutoff c, db):
//                    keys and sums copied, each AVG = PostgreSQL avg(numeric)
//                    of that cell's own VAvgExact(sum, count) (PgNumeric).
//
//   Q1ExecExact:  Inv(store, db) ==> ExecSeqM(db, cutoff) == Q1Rows(cutoff, db)
//
// No observation TCB and no intermediate AuxAnswerSpec: the only
// query-specific relation is ObsRow (keys/sums/avgs), and the AVG closed form
// PgNumeric.AvgOfScaledSum is proved equal to FormalSQL's
// numeric_div_at_scales on the Rocq side (NumericAggExact.pg_avg_exact).

include "Q1ResultSpecBridge.dfy"

module Q1ExecExact {
  import opened LogicalSpec
  import A = Q1Aux
  import F = Q1Finalize
  import PgNumeric
  import Spec = Q1ResultSpec
  import RelEval = RelEvalLogicalSpec
  import RelSpec = Q1RelNodeSpec
  import RelBridge = Q1RelNodeBridge
  import B = Q1ResultSpecBridge

  // ----- Observation of the RelNode rows -----

  ghost predicate ObsShape(r: Row)
  {
    |r| == 10 &&
    r[0].VInt? && r[1].VInt? && r[2].VInt? && r[3].VInt? && r[4].VInt? && r[5].VInt? &&
    r[6].VAvgExact? && r[7].VAvgExact? && r[8].VAvgExact? && r[9].VInt? &&
    0 <= r[0].z < 256 && 0 <= r[1].z < 256 &&
    r[6].count > 0 && r[7].count > 0 && r[8].count > 0
  }

  ghost function ObsRow(r: Row): seq<F.OutRow>
  {
    if ObsShape(r) then
      [F.OutRow(
        Spec.Pack(r[0].z, r[1].z),
        r[2].z, r[3].z, r[4].z, r[5].z,
        PgNumeric.AvgOfScaledSum(r[6].sum, F.kDecimalScale2, r[6].count),
        PgNumeric.AvgOfScaledSum(r[7].sum, F.kDecimalScale2, r[7].count),
        PgNumeric.AvgOfScaledSum(r[8].sum, F.kDecimalScale2, r[8].count),
        r[9].z)]
    else []
  }

  ghost function Q1Obs(rows: seq<Row>): seq<F.OutRow>
    decreases |rows|
  {
    if |rows| == 0 then [] else ObsRow(rows[0]) + Q1Obs(rows[1..])
  }

  ghost function Q1Rows(c: int, db: seq<Spec.Row>): seq<F.OutRow>
  {
    var ev := RelEval.Eval_Spec(RelSpec.LogicalSpec_Q1_rel(), RelSpec.ParamCutoff(c), RelSpec.DbOfAux(db));
    if ev.SomeRows? then Q1Obs(ev.rs) else []
  }

  // ----- From the exact bridge to the observation -----

  // AVG of a non-empty group (count > 0 on every row the bridge relates).
  ghost function AvgOr0(sum: int, n: int): PgNumeric.Decimal
  {
    if n > 0 then PgNumeric.AvgOfScaledSum(sum, F.kDecimalScale2, n) else PgNumeric.Decimal(0, 0)
  }

  ghost function Lift(s: Spec.SpecOutRow): F.OutRow
  {
    F.OutRow(s.group_key, s.sum_qty, s.sum_base_price, s.sum_disc_price, s.sum_charge,
      AvgOr0(s.sum_qty, s.count), AvgOr0(s.sum_base_price, s.count), AvgOr0(s.sum_discount, s.count),
      s.count)
  }

  ghost function LiftSeq(ss: seq<Spec.SpecOutRow>): seq<F.OutRow>
    decreases |ss|
  {
    if |ss| == 0 then [] else [Lift(ss[0])] + LiftSeq(ss[1..])
  }

  lemma ExactQ1_eq_Lift(accel: seq<F.OutRow>, spec: seq<Spec.SpecOutRow>)
    requires B.ExactQ1(accel, spec)
    ensures accel == LiftSeq(spec)
    decreases |accel|
  {
    if |accel| > 0 {
      assert B.ExactQ1Row(accel[0], spec[0]);
      assert B.ExactQ1(accel[1..], spec[1..]) by {
        forall i | 0 <= i < |accel[1..]| ensures B.ExactQ1Row(accel[1..][i], spec[1..][i]) {
          assert B.ExactQ1Row(accel[i + 1], spec[i + 1]);
        }
      }
      ExactQ1_eq_Lift(accel[1..], spec[1..]);
    }
  }

  lemma Obs_eq_LiftAdapt(rows: seq<Row>)
    requires forall i :: 0 <= i < |rows| ==> RelBridge.RelRowShape(rows[i])
    ensures Q1Obs(rows) == LiftSeq(RelSpec.Q1AdaptRel(rows))
    decreases |rows|
  {
    if |rows| > 0 {
      var r := rows[0];
      assert RelBridge.RelRowShape(r);
      assert RelSpec.Q1AdaptRowRel(r)
          == [Spec.SpecOutRow(Spec.Pack(r[0].z, r[1].z), r[2].z, r[3].z, r[4].z, r[5].z,
                               r[8].sum, r[9].z)];
      assert ObsShape(r);
      Obs_eq_LiftAdapt(rows[1..]);
      LiftSeq_append(RelSpec.Q1AdaptRowRel(r), RelSpec.Q1AdaptRel(rows[1..]));
      assert RelSpec.Q1AdaptRel(rows) == RelSpec.Q1AdaptRowRel(r) + RelSpec.Q1AdaptRel(rows[1..]);
    }
  }

  lemma LiftSeq_append(a: seq<Spec.SpecOutRow>, b: seq<Spec.SpecOutRow>)
    ensures LiftSeq(a + b) == LiftSeq(a) + LiftSeq(b)
    decreases |a|
  {
    if |a| == 0 {
      assert a + b == b;
    } else {
      assert (a + b)[0] == a[0];
      assert (a + b)[1..] == a[1..] + b;
      LiftSeq_append(a[1..], b);
      assert LiftSeq(a + b) == [Lift(a[0])] + LiftSeq(a[1..] + b);
    }
  }

  // Q1ExecCorrectOrdered (exact): the accelerator's finalized output for the
  // Aux store is the observed RelNode Eval_Spec result.
  lemma Q1ExecExact(store: A.Store, db: seq<A.Row>, delta: int)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    requires A.Inv(store, db)
    ensures B.ExecSeqM(db, Spec.Cutoff(delta)) == Q1Rows(Spec.Cutoff(delta), B.AuxRowsToSpec(db))
  {
    var c := Spec.Cutoff(delta);
    var sdb := B.AuxRowsToSpec(db);
    B.Q1ExecCorrectOrdered(store, db, delta);
    RelBridge.Eval_Spec_Q1_rel_rows_shape(c, sdb);
    var ev := RelEval.Eval_Spec(RelSpec.LogicalSpec_Q1_rel(), RelSpec.ParamCutoff(c), RelSpec.DbOfAux(sdb));
    assert RelSpec.Eval_Spec_Q1_OutRows_rel(c, sdb) == RelSpec.Q1AdaptRel(ev.rs);
    ExactQ1_eq_Lift(B.ExecSeqM(db, c), RelSpec.Q1AdaptRel(ev.rs));
    Obs_eq_LiftAdapt(ev.rs);
  }
}
