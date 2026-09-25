(* Q21 instantiation of FormalSQLLib.QueryJoinSpineFacade.

   Constructor composition is [query_spine_join4_success] (proved once).
   Empty-DB leaves and of_leaves composition live here. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore
  SqlOutcome SqlQuerySyntax SqlQuerySemantics SqlQueryFacts SqlErrorSemantics
  SqlOrder FiniteBag FiniteCollection FiniteSet FTuples Bool3 OrderedSet Env.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax
  FormalSQL.AggregateRuntimeFacts.
From FormalSQLLib Require Import QueryJoinSpineFacade.
From JetStreamFS Require Import Q21AuxSpec.
From Stdlib Require Import String List ZArith Lia.
Import ListNotations.
Import Tuple.
Open Scope string_scope.
Open Scope Z_scope.

Require Import Q21Catalog Q21FormalSQLProof Q21Obligations.

(** * Leaf interface *)

Definition Q21_SupplierTableOk (units : list (Z * list Q21AuxSpec.LineRow))
    (rows : list (tuple TNull)) : Prop :=
  query_same_rows_as_bag rows
    (@query_table_bag TNull relname
       (@_basesort TNull (EmbedDB_Q21 units))
       (@_instance TNull (EmbedDB_Q21 units))
       q21_supplier_outputs q21_supplier).

Definition Q21_LineitemTableOk (units : list (Z * list Q21AuxSpec.LineRow))
    (rows : list (tuple TNull)) : Prop :=
  query_same_rows_as_bag rows
    (@query_table_bag TNull relname
       (@_basesort TNull (EmbedDB_Q21 units))
       (@_instance TNull (EmbedDB_Q21 units))
       q21_lineitem_outputs q21_lineitem).

Definition Q21_OrdersTableOk (units : list (Z * list Q21AuxSpec.LineRow))
    (rows : list (tuple TNull)) : Prop :=
  query_same_rows_as_bag rows
    (@query_table_bag TNull relname
       (@_basesort TNull (EmbedDB_Q21 units))
       (@_instance TNull (EmbedDB_Q21 units))
       q21_orders_outputs q21_orders).

Definition Q21_NationTableOk (units : list (Z * list Q21AuxSpec.LineRow))
    (rows : list (tuple TNull)) : Prop :=
  query_same_rows_as_bag rows
    (@query_table_bag TNull relname
       (@_basesort TNull (EmbedDB_Q21 units))
       (@_instance TNull (EmbedDB_Q21 units))
       q21_nation_outputs q21_nation).

Definition Q21_WhereFilterOk (units : list (Z * list Q21AuxSpec.LineRow))
    (join_out where_rows : list (tuple TNull)) : Prop :=
  forall sched,
    @eval_filter_rows_outcome TNull relname
      (@_basesort TNull (EmbedDB_Q21 units)) (@_instance TNull (EmbedDB_Q21 units))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      nil q21_where join_out (SqlSuccess where_rows).

Definition Q21_GroupBagOk (units : list (Z * list Q21AuxSpec.LineRow))
    (pre_group group_rows : list (tuple TNull)) : Prop :=
  forall sched,
    exists group_bag,
      @eval_group_bag_outcome TNull relname
        (@_basesort TNull (EmbedDB_Q21 units)) (@_instance TNull (EmbedDB_Q21 units))
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error value_is_null sched
        nil q21_group_select q21_group_keys q21_having
        (query_rows_bag pre_group) (SqlSuccess group_bag) /\
      query_same_rows_as_bag group_rows group_bag.

Definition Q21_OrderOk (group_rows ordered_rows : list (tuple TNull)) : Prop :=
  @order_by_rows TNull value_is_null q21_order_keys group_rows ordered_rows.

Definition Q21_OuterProjectOk (units : list (Z * list Q21AuxSpec.LineRow))
    (ordered_rows final_rows : list (tuple TNull)) : Prop :=
  forall sched,
    @eval_project_rows_outcome TNull relname
      (@_basesort TNull (EmbedDB_Q21 units)) (@_instance TNull (EmbedDB_Q21 units))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      nil q21_outer_select ordered_rows (SqlSuccess final_rows).

(** * Empty leaves *)

Lemma Q21_SupplierTableOk_nil :
  Q21_SupplierTableOk nil nil.
Proof. apply embed_nil_table_same_rows. Qed.

Lemma Q21_LineitemTableOk_nil :
  Q21_LineitemTableOk nil nil.
Proof. apply embed_nil_table_same_rows. Qed.

Lemma Q21_OrdersTableOk_nil :
  Q21_OrdersTableOk nil nil.
Proof. apply embed_nil_table_same_rows. Qed.

Lemma Q21_NationTableOk_nil :
  Q21_NationTableOk nil nil.
Proof. apply embed_nil_table_same_rows. Qed.

Lemma Q21_WhereFilterOk_nil :
  Q21_WhereFilterOk nil nil nil.
Proof. intros sched; apply EFilterRows_Nil. Qed.

Lemma Q21_GroupBagOk_nil :
  Q21_GroupBagOk nil nil nil.
Proof.
  intro sched; exists (query_rows_bag nil); split.
  - apply q21_empty_group_bag.
  - apply Febag.equal_refl.
Qed.

Lemma Q21_OrderOk_nil :
  Q21_OrderOk nil nil.
Proof. exact q21_empty_order_rows. Qed.

Lemma Q21_OuterProjectOk_nil :
  Q21_OuterProjectOk nil nil nil.
Proof. intros sched; apply EProjectRows_Nil. Qed.

(** * of_leaves: empty path via join-spine facade (not TCB) *)

Lemma FormalSQLAuxCorrect_Q21_of_empty_leaves :
  Preconditions_Q21 nil 0%Z ->
  Q21_SupplierTableOk nil nil ->
  Q21_LineitemTableOk nil nil ->
  Q21_OrdersTableOk nil nil ->
  Q21_NationTableOk nil nil ->
  Q21_GroupBagOk nil nil nil ->
  Q21_OrderOk nil nil ->
  FormalSQL_exec (EmbedDB_Q21 nil)
    (EmbedResult_Q21 (Q21AuxSpec.AuxAnswerSpec nil 0%Z)).
Proof.
  intros Hpre Hs Hl Ho Hn Hgroup Horder.
  rewrite AuxAnswerSpec_nil, EmbedResult_nil.
  unfold FormalSQL_exec.
  exists q21_boolean_schedule.
  eapply (@query_spine_join4_empty_success TNull relname
    (@_basesort TNull (EmbedDB_Q21 nil))
    (@_instance TNull (EmbedDB_Q21 nil))
    unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null
    q21_boolean_schedule).
  - exact Hs.
  - exact Hl.
  - exact Ho.
  - exact Hn.
  - apply q21_empty_group_bag.
  - exact Horder.
Qed.

Lemma FormalSQLAuxCorrect_Q21_nil_of_holds :
  forall theta,
    Preconditions_Q21 nil theta ->
    FormalSQL_exec (EmbedDB_Q21 nil)
      (EmbedResult_Q21 (Q21AuxSpec.AuxAnswerSpec nil theta)).
Proof.
  intros theta Hpre.
  apply FormalSQLAuxCorrect_Q21_nil; exact Hpre.
Qed.
