(* Q1LogicalSpec — Q1 as RelNode tree data in the generic LogicalSpec IR.
   Exact/fail-closed recognition still lives alongside (is_LogicalSpec_Q1). *)

From Stdlib Require Import List ZArith String.
Import ListNotations.
Open Scope Z_scope.
Open Scope string_scope.

From FormalSQLLogical Require Import
  SqlTypes Schema Expr Agg RelNode Typing Eval.
Require Import Q1Schema.

(** Column refs on Scan(lineitem) *)
Definition e_shipdate : Expr := EInputRef slot_shipdate.
Definition e_qty : Expr := EInputRef slot_qty.
Definition e_price : Expr := EInputRef slot_price.
Definition e_disc : Expr := EInputRef slot_disc.
Definition e_tax : Expr := EInputRef slot_tax.
Definition e_rf : Expr := EInputRef slot_rf.
Definition e_ls : Expr := EInputRef slot_ls.
Definition e_cutoff : Expr := EParam "cutoff" TDate.

(** 100 at numeric scale 2 = "1.00" *)
Definition e_one_scale2 : Expr := EConst (VInt 100) (TNumeric 2).

Definition e_disc_price : Expr :=
  EBinary BMul e_price (EBinary BSub e_one_scale2 e_disc).

Definition e_charge : Expr :=
  EBinary BMul e_disc_price (EBinary BAdd e_one_scale2 e_tax).

Definition q1_pred_shipdate : Pred :=
  EBinary BLe e_shipdate e_cutoff.

Definition q1_group_exprs : list Expr := [e_rf; e_ls].

Definition q1_aggs : list AggSpec :=
  [AggSumOf e_qty;
   AggSumOf e_price;
   AggSumOf e_disc_price;
   AggSumOf e_charge;
   AggAvgOf e_qty;
   AggAvgOf e_price;
   AggAvgOf e_disc;
   AggCountStarSpec].

(** Project: group refs + agg refs (8 aggs). *)
Definition q1_project_exprs : list Expr :=
  [EGroupRef 0%nat; EGroupRef 1%nat;
   EAggRef 0%nat; EAggRef 1%nat; EAggRef 2%nat; EAggRef 3%nat;
   EAggRef 4%nat; EAggRef 5%nat; EAggRef 6%nat; EAggRef 7%nat].

Definition q1_order_keys : list OrderKey :=
  [mkOrderKey (EInputRef 0%nat) SortAsc NullsLast;
   mkOrderKey (EInputRef 1%nat) SortAsc NullsLast].

(** Canonical Q1 LogicalSpec as a RelNode tree. *)
Definition LogicalSpec_Q1_rel : LogicalSpec :=
  RelSort
    (RelProject
      (RelAggregate
        (RelFilter (RelScan table_lineitem) q1_pred_shipdate)
        q1_group_exprs
        q1_aggs)
      q1_project_exprs)
    q1_order_keys.

Definition is_LogicalSpec_Q1_rel (s : LogicalSpec) : bool :=
  logical_spec_eqb s LogicalSpec_Q1_rel.

Lemma LogicalSpec_Q1_rel_wf :
  logical_spec_wf catalog_Q1 LogicalSpec_Q1_rel = true.
Proof. reflexivity. Qed.

Lemma is_LogicalSpec_Q1_rel_canonical :
  is_LogicalSpec_Q1_rel LogicalSpec_Q1_rel = true.
Proof. reflexivity. Qed.
