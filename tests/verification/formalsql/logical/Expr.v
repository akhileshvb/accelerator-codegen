(* Expr / Pred — typed expression language for LogicalSpec RelNode trees.
   Uses InputRef slots (not physical column enums). Query catalogs bind names→slots. *)

From Stdlib Require Import List ZArith String Bool Nat.
Import ListNotations.
Open Scope Z_scope.
Open Scope string_scope.

From FormalSQLLogical Require Import SqlTypes Schema.

(** * Unary / binary operators *)

Inductive UnaryOp : Type :=
| UNeg | UNot | UIsNull | UIsNotNull.

Inductive BinaryOp : Type :=
| BAdd | BSub | BMul | BDiv
| BEq | BNe | BLt | BLe | BGt | BGe
| BAnd | BOr
| BLike | BPrefixMatch.

(** * Expressions *)

Inductive Expr : Type :=
| EInputRef (slot : nat)
| EConst (v : SqlValue) (ty : SqlType)
| EParam (id : string) (ty : SqlType)
| EUnary (op : UnaryOp) (e : Expr)
| EBinary (op : BinaryOp) (lhs rhs : Expr)
| ECase (branches : list (Expr * Expr)) (else_expr : Expr)
| ECast (e : Expr) (ty : SqlType)
| EGroupRef (i : nat)          (* post-aggregate: i-th group key *)
| EAggRef (i : nat)            (* post-aggregate: i-th aggregate value *)
| EFunction (id : string) (args : list Expr).

(** Predicates are boolean Expressions (SQL three-valued).
    [Pred] is a thin alias for documentation / morphism statements. *)
Definition Pred : Type := Expr.

(** Decidable structural equality (fail-closed recognition). *)

Definition unary_op_eqb (a b : UnaryOp) : bool :=
  match a, b with
  | UNeg, UNeg | UNot, UNot | UIsNull, UIsNull | UIsNotNull, UIsNotNull => true
  | _, _ => false
  end.

Definition binary_op_eqb (a b : BinaryOp) : bool :=
  match a, b with
  | BAdd, BAdd | BSub, BSub | BMul, BMul | BDiv, BDiv
  | BEq, BEq | BNe, BNe | BLt, BLt | BLe, BLe | BGt, BGt | BGe, BGe
  | BAnd, BAnd | BOr, BOr | BLike, BLike | BPrefixMatch, BPrefixMatch => true
  | _, _ => false
  end.

Fixpoint expr_eqb (a b : Expr) : bool :=
  match a, b with
  | EInputRef i, EInputRef j => Nat.eqb i j
  | EConst v1 t1, EConst v2 t2 =>
      andb (sql_value_eqb v1 v2) (sql_type_eqb t1 t2)
  | EParam id1 t1, EParam id2 t2 =>
      andb (String.eqb id1 id2) (sql_type_eqb t1 t2)
  | EUnary o1 e1, EUnary o2 e2 =>
      andb (unary_op_eqb o1 o2) (expr_eqb e1 e2)
  | EBinary o1 l1 r1, EBinary o2 l2 r2 =>
      andb (binary_op_eqb o1 o2)
        (andb (expr_eqb l1 l2) (expr_eqb r1 r2))
  | ECase br1 el1, ECase br2 el2 =>
      let fix branches_eqb xs ys :=
        match xs, ys with
        | [], [] => true
        | (c1, t1) :: xs', (c2, t2) :: ys' =>
            andb (andb (expr_eqb c1 c2) (expr_eqb t1 t2)) (branches_eqb xs' ys')
        | _, _ => false
        end in
      andb (branches_eqb br1 br2) (expr_eqb el1 el2)
  | ECast e1 t1, ECast e2 t2 =>
      andb (expr_eqb e1 e2) (sql_type_eqb t1 t2)
  | EGroupRef i, EGroupRef j | EAggRef i, EAggRef j => Nat.eqb i j
  | EFunction id1 args1, EFunction id2 args2 =>
      let fix args_eqb xs ys :=
        match xs, ys with
        | [], [] => true
        | x :: xs', y :: ys' => andb (expr_eqb x y) (args_eqb xs' ys')
        | _, _ => false
        end in
      andb (String.eqb id1 id2) (args_eqb args1 args2)
  | _, _ => false
  end.

Definition pred_eqb : Pred -> Pred -> bool := expr_eqb.
