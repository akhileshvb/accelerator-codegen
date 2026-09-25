(* Generic FormalSQL 2-table join spine facade.

   Logos / Calcite shape for INNER JOIN expanded to CrossJoin+Filter:

     Project ( OrderBy ( Group ( Project (
       Filter_where ( Filter_true (
         CrossJoin
           (Project (Table left))
           (Project (Table right))
       ))))) )

   Proves EQuery_* constructor composition ONCE.  Query proofs discharge
   named intermediate successes (table/project/join/filter/group/order/
   outer project) and apply [query_spine_join2_success].

   Empty-DB path: [query_spine_join2_empty_success] never evaluates the
   WHERE filter body (Filter on nil uses [EFilterRows_Nil]).

   Sibling of QuerySpineFacade (single-table TFGOP) and
   QueryJoinSpineFacade (4-table Q21 ladder). *)

Set Implicit Arguments.

From Stdlib Require Import List NArith.
Open Scope N_scope.
From SQLFS Require Import
  OrderedSet FiniteSet FiniteBag FiniteCollection FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts Values GenericInstance.
From Logos.FormalSQL Require Import TNullSyntax QueryTNullSyntax.

Import Tuple.
Import ListNotations.

Section QueryJoin2Spine.

Context {T : Tuple.Rcd} {relname : Type}.

Variable basesort : relname -> Fset.set (A T).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple T)).
Variable unknown : Bool.b (B T).
Variable symbol_runtime_error :
  scalar_operator T -> list (option sql_runtime_error * value T) ->
  option sql_runtime_error.
Variable aggregate_runtime_error :
  aggregate T -> list (option sql_runtime_error * value T) ->
  option sql_runtime_error.
Variable value_is_null : value T -> bool.
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Local Abbreviation eval_query :=
  (@eval_query_expr_outcome T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

Local Abbreviation eval_filter :=
  (@eval_filter_rows_outcome T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

Local Abbreviation eval_project :=
  (@eval_project_rows_outcome T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

Local Abbreviation eval_group :=
  (@eval_group_bag_outcome T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

Local Abbreviation order_rows :=
  (@order_by_rows T value_is_null).

Definition select_list := list (scalar_expr T relname ScalarResultValue * attribute T).
Definition group_key_list := list (scalar_expr T relname ScalarResultValue).
Definition bool_expr := scalar_expr T relname ScalarResultBoolean.

(** [elements (mk_bag nil)] is empty as a list. *)
Lemma elements_mk_bag_nil_j2 :
  forall (BA : Febag.Rcd (OTuple T)),
    Febag.elements BA (Febag.mk_bag BA nil) = nil.
Proof.
  intro BA.
  set (els := Febag.elements BA (Febag.mk_bag BA nil)).
  assert (Hall : forall y, Oeset.nb_occ (OTuple T) y els = 0).
  {
    intro y; unfold els.
    rewrite <- Febag.nb_occ_elements, Febag.nb_occ_mk_bag.
    reflexivity.
  }
  destruct els as [|a l]; [reflexivity|].
  specialize (Hall a); cbn in Hall.
  rewrite Oeset.compare_eq_refl in Hall.
  destruct (Oeset.nb_occ _ a l); discriminate.
Qed.

Lemma query_cross_join_nil_nil_j2 :
  query_same_rows_as_bag (T:=T) nil
    (query_cross_join_bag (query_rows_bag nil) (query_rows_bag nil)).
Proof.
  unfold query_same_rows_as_bag, query_cross_join_bag, query_rows_bag.
  rewrite Febag.nb_occ_equal; intro x.
  rewrite !Febag.nb_occ_mk_bag, !elements_mk_bag_nil_j2.
  reflexivity.
Qed.

Lemma query_cross_join_nil_left_j2 :
  forall right_rows,
    query_same_rows_as_bag (T:=T) nil
      (query_cross_join_bag
         (query_rows_bag nil) (query_rows_bag right_rows)).
Proof.
  intro right_rows.
  unfold query_same_rows_as_bag, query_cross_join_bag, query_rows_bag.
  rewrite Febag.nb_occ_equal; intro x.
  rewrite !Febag.nb_occ_mk_bag, elements_mk_bag_nil_j2.
  reflexivity.
Qed.

(** Fixed 2-table AST: CrossJoin + Filter_true + WHERE + Project into
    Group / Order / outer Project. *)
Definition query_spine_join2
    (left_outputs right_outputs : list (attribute T))
    (left_rel right_rel : relname)
    (proj_left proj_right
       proj_pre_group group_select outer_select : select_list)
    (filter_true where_formula having : bool_expr)
    (group_keys : group_key_list)
    (order_keys : list (sort_key T))
  : query_expr T relname :=
  QExpr_Project outer_select
    (QExpr_OrderBy order_keys
      (QExpr_Group group_select group_keys having
        (QExpr_Project proj_pre_group
          (QExpr_Filter where_formula
            (QExpr_Filter filter_true
              (QExpr_CrossJoin
                (QExpr_Project proj_left
                  (QExpr_Table left_outputs left_rel))
                (QExpr_Project proj_right
                  (QExpr_Table right_outputs right_rel)))))))).

(** Constructor composition: leaves ⇒ membership. *)
Lemma query_spine_join2_success :
  forall (env : Env.env T)
    left_outputs right_outputs
    left_rel right_rel
    proj_left proj_right
    proj_pre_group group_select outer_select
    filter_true where_formula having
    group_keys order_keys
    (left_rows right_rows
     proj_l_rows proj_r_rows
     join_rows filt_true_rows where_rows pre_group_rows
     group_rows ordered_rows final_rows : list (tuple T))
    (group_output_bag : Febag.bag (Fecol.CBag (CTuple T))),
    query_same_rows_as_bag left_rows
      (@query_table_bag T relname basesort instance
         left_outputs left_rel) ->
    query_same_rows_as_bag right_rows
      (@query_table_bag T relname basesort instance
         right_outputs right_rel) ->
    eval_project env proj_left left_rows (SqlSuccess proj_l_rows) ->
    eval_project env proj_right right_rows (SqlSuccess proj_r_rows) ->
    query_same_rows_as_bag join_rows
      (query_cross_join_bag
         (query_rows_bag proj_l_rows) (query_rows_bag proj_r_rows)) ->
    eval_filter env filter_true join_rows (SqlSuccess filt_true_rows) ->
    eval_filter env where_formula filt_true_rows (SqlSuccess where_rows) ->
    eval_project env proj_pre_group where_rows (SqlSuccess pre_group_rows) ->
    eval_group env group_select group_keys having
      (query_rows_bag pre_group_rows) (SqlSuccess group_output_bag) ->
    query_same_rows_as_bag group_rows group_output_bag ->
    order_rows order_keys group_rows ordered_rows ->
    eval_project env outer_select ordered_rows (SqlSuccess final_rows) ->
    eval_query env
      (query_spine_join2
         left_outputs right_outputs left_rel right_rel
         proj_left proj_right
         proj_pre_group group_select outer_select
         filter_true where_formula having
         group_keys order_keys)
      (SqlSuccess final_rows).
Proof.
  intros env
    left_outputs right_outputs left_rel right_rel
    proj_left proj_right proj_pre_group group_select outer_select
    filter_true where_formula having group_keys order_keys
    left_rows right_rows proj_l_rows proj_r_rows
    join_rows filt_true_rows where_rows pre_group_rows
    group_rows ordered_rows final_rows
    group_output_bag
    Hl Hr Hpl Hpr Hjoin Hft Hw Hpre Hgroup Hgroup_rows Horder Houter.
  eapply EQuery_ProjectRows; [| exact Houter ].
  eapply EQuery_OrderBySuccess; [| exact Horder ].
  eapply EQuery_GroupBagSuccess.
  2: exact Hgroup.
  2: exact Hgroup_rows.
  eapply EQuery_ProjectRows; [| exact Hpre ].
  eapply EQuery_FilterRows; [| exact Hw ].
  eapply EQuery_FilterRows; [| exact Hft ].
  eapply EQuery_CrossJoinSuccess.
  - eapply EQuery_ProjectRows; [| exact Hpl ].
    eapply EQuery_Table; exact Hl.
  - eapply EQuery_ProjectRows; [| exact Hpr ].
    eapply EQuery_Table; exact Hr.
  - exact Hjoin.
Qed.

(** Empty specialization: all intermediate row lists are nil. *)
Lemma query_spine_join2_empty_success :
  forall (env : Env.env T)
    left_outputs right_outputs
    left_rel right_rel
    proj_left proj_right
    proj_pre_group group_select outer_select
    filter_true where_formula having
    group_keys order_keys,
    query_same_rows_as_bag nil
      (@query_table_bag T relname basesort instance
         left_outputs left_rel) ->
    query_same_rows_as_bag nil
      (@query_table_bag T relname basesort instance
         right_outputs right_rel) ->
    eval_group env group_select group_keys having
      (query_rows_bag (@nil (tuple T))) (SqlSuccess (query_rows_bag nil)) ->
    order_rows order_keys nil nil ->
    eval_query env
      (query_spine_join2
         left_outputs right_outputs left_rel right_rel
         proj_left proj_right
         proj_pre_group group_select outer_select
         filter_true where_formula having
         group_keys order_keys)
      (SqlSuccess nil).
Proof.
  intros env
    left_outputs right_outputs left_rel right_rel
    proj_left proj_right proj_pre_group group_select outer_select
    filter_true where_formula having group_keys order_keys
    Hl Hr Hgroup Horder.
  eapply query_spine_join2_success
    with
      (left_rows := nil) (right_rows := nil)
      (proj_l_rows := nil) (proj_r_rows := nil)
      (join_rows := nil) (filt_true_rows := nil) (where_rows := nil)
      (pre_group_rows := nil) (group_rows := nil)
      (ordered_rows := nil) (final_rows := nil)
      (group_output_bag := query_rows_bag nil).
  all: try exact Hl; try exact Hr.
  all: try apply EProjectRows_Nil.
  all: try apply EFilterRows_Nil.
  all: try apply query_cross_join_nil_nil_j2.
  - exact Hgroup.
  - apply Febag.equal_refl.
  - exact Horder.
Qed.

End QueryJoin2Spine.

(** * TNull specialization *)

Section QueryJoin2Spine_TNull.

Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Definition tnull_value_is_null_j2 (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

Lemma tnull_query_spine_join2_success :
  forall (env : Env.env TNull)
    left_outputs right_outputs left_rel right_rel
    proj_left proj_right proj_pre_group group_select outer_select
    filter_true where_formula having group_keys order_keys
    left_rows right_rows proj_l_rows proj_r_rows
    join_rows filt_true_rows where_rows pre_group_rows
    group_rows ordered_rows final_rows group_output_bag,
    query_same_rows_as_bag left_rows
      (@query_table_bag TNull relname basesort instance
         left_outputs left_rel) ->
    query_same_rows_as_bag right_rows
      (@query_table_bag TNull relname basesort instance
         right_outputs right_rel) ->
    @eval_project_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null_j2 boolean_schedule
      env proj_left left_rows (SqlSuccess proj_l_rows) ->
    @eval_project_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null_j2 boolean_schedule
      env proj_right right_rows (SqlSuccess proj_r_rows) ->
    query_same_rows_as_bag join_rows
      (query_cross_join_bag
         (query_rows_bag proj_l_rows) (query_rows_bag proj_r_rows)) ->
    @eval_filter_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null_j2 boolean_schedule
      env filter_true join_rows (SqlSuccess filt_true_rows) ->
    @eval_filter_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null_j2 boolean_schedule
      env where_formula filt_true_rows (SqlSuccess where_rows) ->
    @eval_project_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null_j2 boolean_schedule
      env proj_pre_group where_rows (SqlSuccess pre_group_rows) ->
    @eval_group_bag_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null_j2 boolean_schedule
      env group_select group_keys having
      (query_rows_bag pre_group_rows) (SqlSuccess group_output_bag) ->
    query_same_rows_as_bag group_rows group_output_bag ->
    @order_by_rows TNull tnull_value_is_null_j2
      order_keys group_rows ordered_rows ->
    @eval_project_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null_j2 boolean_schedule
      env outer_select ordered_rows (SqlSuccess final_rows) ->
    @eval_query_expr_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null_j2 boolean_schedule
      env
      (@query_spine_join2 TNull relname
         left_outputs right_outputs left_rel right_rel
         proj_left proj_right
         proj_pre_group group_select outer_select
         filter_true where_formula having
         group_keys order_keys)
      (SqlSuccess final_rows).
Proof.
  intros; eapply (@query_spine_join2_success TNull relname basesort instance
    unknown3 NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error tnull_value_is_null_j2
    boolean_schedule); eassumption.
Qed.

Lemma tnull_query_spine_join2_empty_success :
  forall (env : Env.env TNull)
    left_outputs right_outputs left_rel right_rel
    proj_left proj_right proj_pre_group group_select outer_select
    filter_true where_formula having group_keys order_keys,
    query_same_rows_as_bag nil
      (@query_table_bag TNull relname basesort instance
         left_outputs left_rel) ->
    query_same_rows_as_bag nil
      (@query_table_bag TNull relname basesort instance
         right_outputs right_rel) ->
    @eval_group_bag_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null_j2 boolean_schedule
      env group_select group_keys having
      (query_rows_bag (@nil (tuple TNull)))
      (SqlSuccess (query_rows_bag nil)) ->
    @order_by_rows TNull tnull_value_is_null_j2 order_keys nil nil ->
    @eval_query_expr_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null_j2 boolean_schedule
      env
      (@query_spine_join2 TNull relname
         left_outputs right_outputs left_rel right_rel
         proj_left proj_right
         proj_pre_group group_select outer_select
         filter_true where_formula having
         group_keys order_keys)
      (SqlSuccess nil).
Proof.
  intros; eapply (@query_spine_join2_empty_success TNull relname basesort
    instance unknown3 NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error tnull_value_is_null_j2
    boolean_schedule); eassumption.
Qed.

End QueryJoin2Spine_TNull.
