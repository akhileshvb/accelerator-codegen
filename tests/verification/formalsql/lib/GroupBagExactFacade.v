(* Generic FormalSQL GROUP bag success facade.

   Proves [EGroupBag_Success] construction ONCE from exact group-list
   observation hypotheses.  Query proofs discharge per-bucket select /
   HAVING acceptance (via AggregateOutcomeBridgeFacts / GroupingRewriteFacts)
   and apply [group_bag_success_of_exact_groups] — no [EGroups_*] nesting
   in query files. *)

From Stdlib Require Import List.
From SQLFS Require Import
  OrderedSet FiniteSet FiniteBag FiniteCollection FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts Values GenericInstance.
From Logos.FormalSQL Require Import
  TNullSyntax QueryTNullSyntax GroupedFilterOutcomeFacts.

Import Tuple.
Import ListNotations.

Section GroupBagExact.

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

Local Abbreviation eval_group :=
  (@eval_group_bag_outcome T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

Local Abbreviation eval_groups :=
  (@eval_groups_outcome T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

(** Exact group-list observation ⇒ bag success with the observed row list. *)
Theorem group_bag_success_of_exact_groups :
  forall env select_list group_keys group_terms having
      input_bag representative grouped_rows,
    scalar_group_key_terms group_keys = Some group_terms ->
    query_same_rows_as_bag representative input_bag ->
    @group_keys_runtime_error T symbol_runtime_error aggregate_runtime_error
      env group_terms representative = None ->
    eval_groups env select_list group_terms having
      (query_make_groups env representative group_terms)
      (SqlSuccess grouped_rows) ->
    eval_group env select_list group_keys having input_bag
      (SqlSuccess (query_rows_bag grouped_rows)).
Proof.
  intros env select_list group_keys group_terms having
    input_bag representative grouped_rows
    Hterms Hrep Hkeys_ok Hgroups.
  eapply EGroupBag_Success.
  - exact Hterms.
  - exact Hrep.
  - exact Hkeys_ok.
  - exact Hgroups.
  - apply Febag.equal_refl.
Qed.

(** Same, packaging the output bag equality hypothesis explicitly. *)
Theorem group_bag_success_of_exact_groups_bag :
  forall env select_list group_keys group_terms having
      input_bag representative grouped_rows output_bag,
    scalar_group_key_terms group_keys = Some group_terms ->
    query_same_rows_as_bag representative input_bag ->
    @group_keys_runtime_error T symbol_runtime_error aggregate_runtime_error
      env group_terms representative = None ->
    eval_groups env select_list group_terms having
      (query_make_groups env representative group_terms)
      (SqlSuccess grouped_rows) ->
    query_same_rows_as_bag grouped_rows output_bag ->
    eval_group env select_list group_keys having input_bag
      (SqlSuccess output_bag).
Proof.
  intros env select_list group_keys group_terms having
    input_bag representative grouped_rows output_bag
    Hterms Hrep Hkeys_ok Hgroups Hout.
  eapply EGroupBag_Success; eassumption.
Qed.

End GroupBagExact.

Section GroupBagExact_TNull.

Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Definition tnull_value_is_null (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

Theorem tnull_group_bag_success_of_exact_groups :
  forall env select_list group_keys group_terms having
      input_bag representative grouped_rows,
    scalar_group_key_terms group_keys = Some group_terms ->
    query_same_rows_as_bag representative input_bag ->
    @group_keys_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      env group_terms representative = None ->
    @eval_groups_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      env select_list group_terms having
      (query_make_groups env representative group_terms)
      (SqlSuccess grouped_rows) ->
    @eval_group_bag_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      env select_list group_keys having input_bag
      (SqlSuccess (query_rows_bag grouped_rows)).
Proof.
  intros; eapply (@group_bag_success_of_exact_groups TNull relname
    basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule); eassumption.
Qed.

End GroupBagExact_TNull.
