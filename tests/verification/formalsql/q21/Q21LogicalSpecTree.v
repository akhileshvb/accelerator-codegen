(* Q21LogicalSpec — parameterized RelNode tree for TPC-H Q21.
   Authority: Rocq DATA. Nation via EParam "nation_name" (not baked SAUDI ARABIA).
   EXISTS / NOT EXISTS → RelSemiJoin / RelAntiJoin with explicit l1/l2/l3 slots. *)

From Stdlib Require Import List ZArith String.
Import ListNotations.
Open Scope string_scope.

From FormalSQLLogical Require Import
  SqlTypes Schema Expr Agg RelNode Typing Eval.
Require Import Q21Schema.

(** ----- Predicates on outer spine ----- *)

Definition e_s_suppkey : Expr := EInputRef slot_s_suppkey.
Definition e_s_name : Expr := EInputRef slot_s_name.
Definition e_s_nationkey : Expr := EInputRef slot_s_nationkey.
Definition e_l1_orderkey : Expr := EInputRef slot_l1_orderkey.
Definition e_l1_suppkey : Expr := EInputRef slot_l1_suppkey.
Definition e_l1_commitdate : Expr := EInputRef slot_l1_commitdate.
Definition e_l1_receiptdate : Expr := EInputRef slot_l1_receiptdate.
Definition e_o_orderkey : Expr := EInputRef slot_o_orderkey.
Definition e_o_orderstatus : Expr := EInputRef slot_o_orderstatus.
Definition e_n_nationkey : Expr := EInputRef slot_n_nationkey.
Definition e_n_name : Expr := EInputRef slot_n_name.

Definition e_status_F : Expr := EConst (VText "F") TText.

Definition pred_join_supp_l1 : Pred :=
  EBinary BEq e_s_suppkey (EInputRef 4%nat). (* after supplier⋈l1: l_suppkey at 4 *)

(** Build spine incrementally so join conds use correct concatenated slots. *)

Definition scan_supplier : RelNode := RelScan table_supplier.
Definition scan_lineitem : RelNode := RelScan table_lineitem.
Definition scan_orders : RelNode := RelScan table_orders.
Definition scan_nation : RelNode := RelScan table_nation.

(** supplier ⋈ l1 on s_suppkey = l_suppkey
    left schema 3, right schema 4 → combined width 7
    left.0 = s_suppkey, right.1 = l_suppkey → slots 0 and 4 *)
Definition join_supp_l1 : RelNode :=
  RelJoin JoinInner scan_supplier scan_lineitem
    (EBinary BEq (EInputRef 0%nat) (EInputRef 4%nat)).

(** ⋈ orders on l_orderkey = o_orderkey
    left width 7, orders [0,1] → o_orderkey at 7, l_orderkey at 3 *)
Definition join_supp_l1_ord : RelNode :=
  RelJoin JoinInner join_supp_l1 scan_orders
    (EBinary BEq (EInputRef 3%nat) (EInputRef 7%nat)).

(** ⋈ nation on s_nationkey = n_nationkey
    left width 9, nation [0,1] → n_nationkey at 9, s_nationkey at 2 *)
Definition join_outer_spine : RelNode :=
  RelJoin JoinInner join_supp_l1_ord scan_nation
    (EBinary BEq (EInputRef 2%nat) (EInputRef 9%nat)).

Definition pred_status_F : Pred :=
  EBinary BEq (EInputRef slot_o_orderstatus) e_status_F.

Definition pred_late_l1 : Pred :=
  EBinary BGt (EInputRef slot_l1_receiptdate) (EInputRef slot_l1_commitdate).

Definition pred_nation_param : Pred :=
  EBinary BEq (EInputRef slot_n_name) param_nation_name.

Definition pred_outer_filters : Pred :=
  EBinary BAnd pred_status_F
    (EBinary BAnd pred_late_l1 pred_nation_param).

Definition filter_outer : RelNode :=
  RelFilter join_outer_spine pred_outer_filters.

(** EXISTS l2: same orderkey, different suppkey.
    l2 bound to SemiJoin right; correlations to outer l1 at slots 3/4. *)
Definition pred_exists_l2 : Pred :=
  EBinary BAnd
    (EBinary BEq (EInputRef slot_l1_orderkey) (EInputRef slot_r_orderkey))
    (EBinary BNe (EInputRef slot_l1_suppkey) (EInputRef slot_r_suppkey)).

Definition semi_exists_l2 : RelNode :=
  RelSemiJoin filter_outer scan_lineitem pred_exists_l2.

(** NOT EXISTS l3: same orderkey, different suppkey, late(l3).
    l3 = late-filtered lineitem on AntiJoin right. *)
Definition pred_late_lineitem_local : Pred :=
  (** On RelScan(lineitem) alone: slots 2=commit, 3=receipt *)
  EBinary BGt (EInputRef 3%nat) (EInputRef 2%nat).

Definition scan_lineitem_late : RelNode :=
  RelFilter scan_lineitem pred_late_lineitem_local.

Definition pred_not_exists_l3 : Pred :=
  EBinary BAnd
    (EBinary BEq (EInputRef slot_l1_orderkey) (EInputRef slot_r_orderkey))
    (EBinary BNe (EInputRef slot_l1_suppkey) (EInputRef slot_r_suppkey)).

Definition anti_not_exists_l3 : RelNode :=
  RelAntiJoin semi_exists_l2 scan_lineitem_late pred_not_exists_l3.

Definition q21_group_exprs : list Expr := [EInputRef slot_s_name].

Definition q21_aggs : list AggSpec := [AggCountStarSpec].

Definition q21_project_exprs : list Expr :=
  [EGroupRef 0%nat; EAggRef 0%nat].

(** After project: 0 = s_name, 1 = numwait.
    Logos: SortDescNullsFirst numwait; SortAscNullsLast s_name. *)
Definition q21_order_keys : list OrderKey :=
  [mkOrderKey (EInputRef 1%nat) SortDesc NullsFirst;
   mkOrderKey (EInputRef 0%nat) SortAsc NullsLast].

Definition LogicalSpec_Q21_rel : LogicalSpec :=
  RelSort
    (RelProject
      (RelAggregate anti_not_exists_l3 q21_group_exprs q21_aggs)
      q21_project_exprs)
    q21_order_keys.

Definition is_LogicalSpec_Q21_rel (s : LogicalSpec) : bool :=
  logical_spec_eqb s LogicalSpec_Q21_rel.

Lemma LogicalSpec_Q21_rel_wf :
  logical_spec_wf catalog_Q21 LogicalSpec_Q21_rel = true.
Proof. reflexivity. Qed.

Lemma is_LogicalSpec_Q21_rel_canonical :
  is_LogicalSpec_Q21_rel LogicalSpec_Q21_rel = true.
Proof. reflexivity. Qed.
