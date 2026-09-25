(* Generic FormalSQL project-rows exact facade.

   Packages Logos [eval_project_rows_exact_map] so query proofs discharge
   per-row [scalar_value_list_exact_at] and obtain project success as
   [map project_row] — without stepping [EProjectRows_Cons] in query files. *)

From Stdlib Require Import List.
From SQLFS Require Import
  OrderedSet FiniteSet FiniteBag FiniteCollection FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts Values GenericInstance Projection.
From Logos.FormalSQL Require Import
  TNullSyntax QueryTNullSyntax GroupedFilterOutcomeFacts.

Import Tuple.
Import ListNotations.

Section ProjectRowsExact.

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

Local Abbreviation eval_project :=
  (@eval_project_rows_outcome T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

Local Abbreviation value_list_exact :=
  (@scalar_value_list_exact_at T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

Theorem project_rows_success_of_exact_map :
  forall env select_list rows
      (values : tuple T -> list (value T)),
    (forall row,
      In row rows ->
      value_list_exact (env_t T env row) (map fst select_list) (values row)) ->
    eval_project env select_list rows
      (SqlSuccess (map (fun row => project_row select_list (values row)) rows)).
Proof.
  intros env select_list rows values Hexact.
  apply (proj2
    (@eval_project_rows_exact_map T relname basesort instance unknown
      symbol_runtime_error aggregate_runtime_error value_is_null
      boolean_schedule env select_list rows values Hexact
      (SqlSuccess
        (map (fun row => project_row select_list (values row)) rows)))).
  reflexivity.
Qed.

Theorem project_rows_outcome_iff_exact_map :
  forall env select_list rows
      (values : tuple T -> list (value T)) outcome,
    (forall row,
      In row rows ->
      value_list_exact (env_t T env row) (map fst select_list) (values row)) ->
    eval_project env select_list rows outcome <->
    outcome =
      SqlSuccess (map (fun row => project_row select_list (values row)) rows).
Proof.
  intros; eapply eval_project_rows_exact_map; eassumption.
Qed.

End ProjectRowsExact.

Section ProjectRowsExact_TNull.

Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Definition tnull_value_is_null (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

Theorem tnull_project_rows_success_of_exact_map :
  forall env select_list rows
      (values : tuple TNull -> list NullValues.value),
    (forall row,
      In row rows ->
      @scalar_value_list_exact_at TNull relname basesort instance unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        tnull_value_is_null boolean_schedule
        (env_t TNull env row) (map fst select_list) (values row)) ->
    @eval_project_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      env select_list rows
      (SqlSuccess
        (map (fun row => project_row select_list (values row)) rows)).
Proof.
  intros; eapply (@project_rows_success_of_exact_map TNull relname
    basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule); eassumption.
Qed.

End ProjectRowsExact_TNull.
