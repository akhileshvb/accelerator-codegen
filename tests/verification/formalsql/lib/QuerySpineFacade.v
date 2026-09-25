(* Generic FormalSQL query-spine facade.

   Common TPC-H / Logos shape:
     Project ( OrderBy ( Group ( Project ( Filter ( Table ) ) ) ) )

   Proves the EQuery_* constructor composition ONCE.  Query-specific work
   discharges the named intermediate-success hypotheses (filter / inner
   project / group aggregates / order / outer project) and applies
   [query_spine_TFGOP_success] — no per-query EQuery nesting.

   Lives under tests/verification/formalsql/lib so Q1/Q21 (and future LLM
   agents) can Require it without editing the Logos pin. *)

Set Implicit Arguments.

From Stdlib Require Import List.
From SQLFS Require Import
  OrderedSet FiniteSet FiniteBag FiniteCollection FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts Values GenericInstance.
From Logos.FormalSQL Require Import TNullSyntax QueryTNullSyntax.

Import Tuple.
Import ListNotations.

Section QuerySpineTFGOP.

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

(** The fixed query AST shape this facade covers. *)
Definition query_spine_TFGOP
    (table_outputs : list (attribute T))
    (table : relname)
    (filter_formula : bool_expr)
    (inner_select : select_list)
    (group_select : select_list)
    (group_keys : group_key_list)
    (having : bool_expr)
    (order_keys : list (sort_key T))
    (outer_select : select_list)
  : query_expr T relname :=
  QExpr_Project outer_select
    (QExpr_OrderBy order_keys
      (QExpr_Group group_select group_keys having
        (QExpr_Project inner_select
          (QExpr_Filter filter_formula
            (QExpr_Table table_outputs table))))).

(** * Constructor composition (proved once)

    Hypotheses are FormalSQL intermediate successes.  Query agents discharge
    them from FilterEq / projection correspondence / GroupEq / aggregate
    values / OrderEq / outer ProjectionEq (NoError rules out SqlError).
    The facade does not assume what [final_rows] are — the caller pins that. *)
Lemma query_spine_TFGOP_success :
  forall (env : Env.env T)
    table_outputs table
    filter_formula
    inner_select
    group_select group_keys having
    order_keys
    outer_select
    (table_rows filtered_rows inner_proj_rows
     group_rows ordered_rows final_rows : list (tuple T))
    (group_output_bag : Febag.bag (Fecol.CBag (CTuple T))),
    query_same_rows_as_bag table_rows
      (@query_table_bag T relname basesort instance table_outputs table) ->
    eval_filter env filter_formula table_rows (SqlSuccess filtered_rows) ->
    eval_project env inner_select filtered_rows (SqlSuccess inner_proj_rows) ->
    eval_group env group_select group_keys having
      (query_rows_bag inner_proj_rows) (SqlSuccess group_output_bag) ->
    query_same_rows_as_bag group_rows group_output_bag ->
    order_rows order_keys group_rows ordered_rows ->
    eval_project env outer_select ordered_rows (SqlSuccess final_rows) ->
    eval_query env
      (query_spine_TFGOP table_outputs table filter_formula inner_select
         group_select group_keys having order_keys outer_select)
      (SqlSuccess final_rows).
Proof.
  intros env table_outputs table filter_formula inner_select
    group_select group_keys having order_keys outer_select
    table_rows filtered_rows inner_proj_rows group_rows ordered_rows final_rows
    group_output_bag
    Htable Hfilter Hinner Hgroup Hgroup_rows Horder Houter.
  eapply EQuery_ProjectRows; [| exact Houter ].
  eapply EQuery_OrderBySuccess; [| exact Horder ].
  eapply EQuery_GroupBagSuccess.
  - eapply EQuery_ProjectRows; [| exact Hinner ].
    eapply EQuery_FilterRows; [| exact Hfilter ].
    eapply EQuery_Table. exact Htable.
  - exact Hgroup.
  - exact Hgroup_rows.
Qed.

Lemma query_spine_TFGOP_success_of_table_eval :
  forall (env : Env.env T)
    table_outputs table
    filter_formula
    inner_select
    group_select group_keys having
    order_keys
    outer_select
    (table_rows filtered_rows inner_proj_rows
     group_rows ordered_rows final_rows : list (tuple T))
    (group_output_bag : Febag.bag (Fecol.CBag (CTuple T))),
    eval_query env (QExpr_Table table_outputs table) (SqlSuccess table_rows) ->
    eval_filter env filter_formula table_rows (SqlSuccess filtered_rows) ->
    eval_project env inner_select filtered_rows (SqlSuccess inner_proj_rows) ->
    eval_group env group_select group_keys having
      (query_rows_bag inner_proj_rows) (SqlSuccess group_output_bag) ->
    query_same_rows_as_bag group_rows group_output_bag ->
    order_rows order_keys group_rows ordered_rows ->
    eval_project env outer_select ordered_rows (SqlSuccess final_rows) ->
    eval_query env
      (query_spine_TFGOP table_outputs table filter_formula inner_select
         group_select group_keys having order_keys outer_select)
      (SqlSuccess final_rows).
Proof.
  intros env table_outputs table filter_formula inner_select
    group_select group_keys having order_keys outer_select
    table_rows filtered_rows inner_proj_rows group_rows ordered_rows final_rows
    group_output_bag
    Htable Hfilter Hinner Hgroup Hgroup_rows Horder Houter.
  eapply EQuery_ProjectRows; [| exact Houter ].
  eapply EQuery_OrderBySuccess; [| exact Horder ].
  eapply EQuery_GroupBagSuccess.
  - eapply EQuery_ProjectRows; [| exact Hinner ].
    eapply EQuery_FilterRows; [| exact Hfilter ].
    exact Htable.
  - exact Hgroup.
  - exact Hgroup_rows.
Qed.

End QuerySpineTFGOP.

(** * TNull specialization *)

Section QuerySpineTFGOP_TNull.

Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Definition tnull_value_is_null (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

Lemma tnull_query_spine_TFGOP_success :
  forall (env : Env.env TNull)
    table_outputs table
    filter_formula
    inner_select
    group_select group_keys having
    order_keys
    outer_select
    table_rows filtered_rows inner_proj_rows
    group_rows ordered_rows final_rows
    group_output_bag,
    query_same_rows_as_bag table_rows
      (@query_table_bag TNull relname basesort instance table_outputs table) ->
    @eval_filter_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      env filter_formula table_rows (SqlSuccess filtered_rows) ->
    @eval_project_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      env inner_select filtered_rows (SqlSuccess inner_proj_rows) ->
    @eval_group_bag_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      env group_select group_keys having
      (query_rows_bag inner_proj_rows) (SqlSuccess group_output_bag) ->
    query_same_rows_as_bag group_rows group_output_bag ->
    @order_by_rows TNull tnull_value_is_null
      order_keys group_rows ordered_rows ->
    @eval_project_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      env outer_select ordered_rows (SqlSuccess final_rows) ->
    @eval_query_expr_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      env
      (@query_spine_TFGOP TNull relname
         table_outputs table filter_formula inner_select
         group_select group_keys having order_keys outer_select)
      (SqlSuccess final_rows).
Proof.
  intros; eapply (@query_spine_TFGOP_success TNull relname basesort instance
    unknown3 NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error tnull_value_is_null
    boolean_schedule); eassumption.
Qed.

End QuerySpineTFGOP_TNull.
