(* RelNode — typed logical relational tree (LogicalSpec).
   Logical only: no Pack, hash tables, indexes, partitions, aux-state, physical plans. *)

From Stdlib Require Import List ZArith String Bool Nat.
Import ListNotations.
Open Scope string_scope.

From FormalSQLLogical Require Import SqlTypes Schema Expr Agg.

(** * Ordering *)

Inductive SortDir : Type := SortAsc | SortDesc.

Inductive NullOrder : Type := NullsFirst | NullsLast.

Record OrderKey : Type := mkOrderKey {
  ok_expr : Expr;
  ok_dir : SortDir;
  ok_nulls : NullOrder
}.

Definition sort_dir_eqb (a b : SortDir) : bool :=
  match a, b with
  | SortAsc, SortAsc | SortDesc, SortDesc => true
  | _, _ => false
  end.

Definition null_order_eqb (a b : NullOrder) : bool :=
  match a, b with
  | NullsFirst, NullsFirst | NullsLast, NullsLast => true
  | _, _ => false
  end.

Definition order_key_eqb (a b : OrderKey) : bool :=
  andb (expr_eqb (ok_expr a) (ok_expr b))
  (andb (sort_dir_eqb (ok_dir a) (ok_dir b))
        (null_order_eqb (ok_nulls a) (ok_nulls b))).

(** * Join kinds *)

Inductive JoinKind : Type :=
| JoinInner | JoinLeft | JoinRight | JoinFull | JoinCross.

Definition join_kind_eqb (a b : JoinKind) : bool :=
  match a, b with
  | JoinInner, JoinInner | JoinLeft, JoinLeft | JoinRight, JoinRight
  | JoinFull, JoinFull | JoinCross, JoinCross => true
  | _, _ => false
  end.

(** * Relational operators *)

Inductive RelNode : Type :=
| RelScan (table : TableId)
| RelFilter (input : RelNode) (pred : Pred)
| RelProject (input : RelNode) (exprs : list Expr)
| RelJoin (kind : JoinKind) (lft rgt : RelNode) (cond : Pred)
(** Semi/Anti: EXISTS / NOT EXISTS fragment.
    Output schema = left schema. [cond] is typed over left++right slots.
    Semi emits each left row at most once (SQL EXISTS multiplicity). *)
| RelSemiJoin (lft rgt : RelNode) (cond : Pred)
| RelAntiJoin (lft rgt : RelNode) (cond : Pred)
| RelAggregate (input : RelNode) (group_exprs : list Expr) (aggs : list AggSpec)
| RelSort (input : RelNode) (order_keys : list OrderKey)
| RelDistinct (input : RelNode)
| RelLimit (input : RelNode) (offset count : nat).

(** Canonical name: a LogicalSpec *is* a RelNode tree. *)
Definition LogicalSpec : Type := RelNode.

Fixpoint list_eqb {A} (eqb : A -> A -> bool) (xs ys : list A) : bool :=
  match xs, ys with
  | [], [] => true
  | x :: xs', y :: ys' => andb (eqb x y) (list_eqb eqb xs' ys')
  | _, _ => false
  end.

Fixpoint rel_eqb (a b : RelNode) : bool :=
  match a, b with
  | RelScan t1, RelScan t2 => String.eqb t1 t2
  | RelFilter i1 p1, RelFilter i2 p2 =>
      andb (rel_eqb i1 i2) (pred_eqb p1 p2)
  | RelProject i1 es1, RelProject i2 es2 =>
      andb (rel_eqb i1 i2) (list_eqb expr_eqb es1 es2)
  | RelJoin k1 l1 r1 c1, RelJoin k2 l2 r2 c2 =>
      andb (join_kind_eqb k1 k2)
      (andb (rel_eqb l1 l2)
      (andb (rel_eqb r1 r2) (pred_eqb c1 c2)))
  | RelSemiJoin l1 r1 c1, RelSemiJoin l2 r2 c2
  | RelAntiJoin l1 r1 c1, RelAntiJoin l2 r2 c2 =>
      andb (rel_eqb l1 l2) (andb (rel_eqb r1 r2) (pred_eqb c1 c2))
  | RelAggregate i1 g1 a1, RelAggregate i2 g2 a2 =>
      andb (rel_eqb i1 i2)
      (andb (list_eqb expr_eqb g1 g2) (list_eqb agg_eqb a1 a2))
  | RelSort i1 o1, RelSort i2 o2 =>
      andb (rel_eqb i1 i2) (list_eqb order_key_eqb o1 o2)
  | RelDistinct i1, RelDistinct i2 => rel_eqb i1 i2
  | RelLimit i1 off1 c1, RelLimit i2 off2 c2 =>
      andb (rel_eqb i1 i2) (andb (Nat.eqb off1 off2) (Nat.eqb c1 c2))
  | _, _ => false
  end.

Definition logical_spec_eqb : LogicalSpec -> LogicalSpec -> bool := rel_eqb.
