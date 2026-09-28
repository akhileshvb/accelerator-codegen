(* AggSpec — generic aggregation operators for RelAggregate. *)

From Stdlib Require Import List ZArith Bool Nat.
Import ListNotations.
Open Scope Z_scope.

From FormalSQLLogical Require Import SqlTypes Expr.

Inductive AggKind : Type :=
| AggCountStar
| AggCount      (* COUNT(expr) — NULLs skipped *)
| AggSum
| AggAvg        (* internal Acc = (sum, count); emit VAvgExact *)
| AggMin
| AggMax.

Record AggSpec : Type := mkAggSpec {
  agg_kind : AggKind;
  agg_expr : option Expr   (* None for CountStar *)
}.

Definition AggSumOf (e : Expr) : AggSpec := mkAggSpec AggSum (Some e).
Definition AggAvgOf (e : Expr) : AggSpec := mkAggSpec AggAvg (Some e).
Definition AggCountOf (e : Expr) : AggSpec := mkAggSpec AggCount (Some e).
Definition AggMinOf (e : Expr) : AggSpec := mkAggSpec AggMin (Some e).
Definition AggMaxOf (e : Expr) : AggSpec := mkAggSpec AggMax (Some e).
Definition AggCountStarSpec : AggSpec := mkAggSpec AggCountStar None.

Definition agg_kind_eqb (a b : AggKind) : bool :=
  match a, b with
  | AggCountStar, AggCountStar | AggCount, AggCount
  | AggSum, AggSum | AggAvg, AggAvg
  | AggMin, AggMin | AggMax, AggMax => true
  | _, _ => false
  end.

Definition option_expr_eqb (a b : option Expr) : bool :=
  match a, b with
  | None, None => true
  | Some x, Some y => expr_eqb x y
  | _, _ => false
  end.

Definition agg_eqb (a b : AggSpec) : bool :=
  andb (agg_kind_eqb (agg_kind a) (agg_kind b))
       (option_expr_eqb (agg_expr a) (agg_expr b)).

(** Per-agg accumulator: (sum_or_extremum, count, initialized?).
    For Min/Max, [sum] holds the extremum; [init]=false means empty. *)
Record AccSlot : Type := mkAccSlot {
  acc_sum : Z;
  acc_count : Z;
  acc_init : bool
}.

Definition AggAcc : Type := list AccSlot.

Definition empty_slot : AccSlot := mkAccSlot 0 0 false.

Fixpoint empty_acc (n : nat) : AggAcc :=
  match n with
  | 0%nat => []
  | S n' => empty_slot :: empty_acc n'
  end.
