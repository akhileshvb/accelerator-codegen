(* Generic FormalSQL GROUP-list exact facade.

   Packages Logos [eval_groups_outcome_exact_filter_map] so query proofs
   discharge per-bucket [group_filter_map_exact_at] (HAVING acceptance +
   select-value exactness + aggregate-finalize None) and obtain
   [eval_groups_outcome] success as [map project_row ∘ filter] — without
   nesting [EGroups_*] in query files.

   Combine with [GroupBagExactFacade.group_bag_success_of_exact_groups]
   for end-to-end bag success from the same local contract. *)

From Stdlib Require Import List.
From SQLFS Require Import
  OrderedSet FiniteSet FiniteBag FiniteCollection FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts Values GenericInstance Projection.
From Logos.FormalSQL Require Import
  TNullSyntax QueryTNullSyntax GroupedFilterOutcomeFacts.
From FormalSQLLib Require Import GroupBagExactFacade.

Import Tuple.
Import ListNotations.

Section GroupsExact.

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

Local Abbreviation eval_groups :=
  (@eval_groups_outcome T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

Local Abbreviation eval_group :=
  (@eval_group_bag_outcome T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

Local Abbreviation group_exact :=
  (@group_filter_map_exact_at T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

(** HAVING [SExpr_True] is always accepted. *)
Theorem having_true_acceptance_exact :
  forall env,
    @scalar_expr_acceptance_exact_at T relname basesort instance unknown
      symbol_runtime_error aggregate_runtime_error value_is_null
      boolean_schedule env SExpr_True true.
Proof.
  intros; apply scalar_expr_true_acceptance_exact.
Qed.

(** Exact per-group contract ⇒ whole GROUP-list success as filter/map. *)
Theorem groups_success_of_exact_filter_map :
  forall env select_list group_terms having groups keep values,
    (forall group,
      In group groups ->
      group_exact env select_list group_terms having keep values group) ->
    eval_groups env select_list group_terms having groups
      (SqlSuccess
        (map (fun group => project_row select_list (values group))
          (filter keep groups))).
Proof.
  intros env select_list group_terms having groups keep values Hexact.
  apply (proj2
    (@eval_groups_outcome_exact_filter_map T relname basesort instance unknown
      symbol_runtime_error aggregate_runtime_error value_is_null
      boolean_schedule env select_list group_terms having groups keep values
      Hexact
      (SqlSuccess
        (map (fun group => project_row select_list (values group))
          (filter keep groups))))).
  reflexivity.
Qed.

Lemma filter_keep_all_id :
  forall {A} (xs : list A),
    filter (fun _ => true) xs = xs.
Proof.
  induction xs as [|x xs IH]; [reflexivity|cbn; f_equal; exact IH].
Qed.

(** HAVING always-true specialization: keep every group. *)
Theorem groups_success_of_exact_map_having_true :
  forall env select_list group_terms groups values,
    (forall group,
      In group groups ->
      group_exact env select_list group_terms SExpr_True
        (fun _ => true) values group) ->
    eval_groups env select_list group_terms SExpr_True groups
      (SqlSuccess
        (map (fun group => project_row select_list (values group)) groups)).
Proof.
  intros env select_list group_terms groups values Hexact.
  replace
    (map (fun group => project_row select_list (values group)) groups)
    with
      (map (fun group => project_row select_list (values group))
        (filter (fun _ => true) groups))
    by (rewrite filter_keep_all_id; reflexivity).
  eapply groups_success_of_exact_filter_map; exact Hexact.
Qed.

(** End-to-end bag success from key decode + key-runtime None + per-group
    filter/map exactness (no [EGroups_*] / [EGroupBag_*] in query files). *)
Theorem group_bag_success_of_exact_filter_map :
  forall env select_list group_keys group_terms having
      input_bag representative keep values,
    scalar_group_key_terms group_keys = Some group_terms ->
    query_same_rows_as_bag representative input_bag ->
    @group_keys_runtime_error T symbol_runtime_error aggregate_runtime_error
      env group_terms representative = None ->
    let groups := query_make_groups env representative group_terms in
    (forall group,
      In group groups ->
      group_exact env select_list group_terms having keep values group) ->
    eval_group env select_list group_keys having input_bag
      (SqlSuccess
        (query_rows_bag
          (map (fun group => project_row select_list (values group))
            (filter keep groups)))).
Proof.
  intros env select_list group_keys group_terms having
    input_bag representative keep values
    Hterms Hrep Hkeys_ok groups Hexact.
  eapply (@group_bag_success_of_exact_groups T relname
    basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).
  - exact Hterms.
  - exact Hrep.
  - exact Hkeys_ok.
  - eapply groups_success_of_exact_filter_map; exact Hexact.
Qed.

Theorem group_bag_success_of_exact_map_having_true :
  forall env select_list group_keys group_terms
      input_bag representative values,
    scalar_group_key_terms group_keys = Some group_terms ->
    query_same_rows_as_bag representative input_bag ->
    @group_keys_runtime_error T symbol_runtime_error aggregate_runtime_error
      env group_terms representative = None ->
    let groups := query_make_groups env representative group_terms in
    (forall group,
      In group groups ->
      group_exact env select_list group_terms SExpr_True
        (fun _ => true) values group) ->
    eval_group env select_list group_keys SExpr_True input_bag
      (SqlSuccess
        (query_rows_bag
          (map (fun group => project_row select_list (values group))
            groups))).
Proof.
  intros env select_list group_keys group_terms
    input_bag representative values
    Hterms Hrep Hkeys_ok groups Hexact.
  replace
    (map (fun group => project_row select_list (values group)) groups)
    with
      (map (fun group => project_row select_list (values group))
        (filter (fun _ => true) groups))
    by (rewrite filter_keep_all_id; reflexivity).
  eapply group_bag_success_of_exact_filter_map; eassumption.
Qed.

End GroupsExact.

Section GroupsExact_TNull.

Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Definition tnull_value_is_null (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

Theorem tnull_having_true_acceptance_exact :
  forall env,
    @scalar_expr_acceptance_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env SExpr_True true.
Proof.
  intros; eapply (@having_true_acceptance_exact TNull relname
    basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule).
Qed.

Theorem tnull_groups_success_of_exact_filter_map :
  forall env select_list group_terms having groups keep values,
    (forall group,
      In group groups ->
      @group_filter_map_exact_at TNull relname basesort instance unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        tnull_value_is_null boolean_schedule
        env select_list group_terms having keep values group) ->
    @eval_groups_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      env select_list group_terms having groups
      (SqlSuccess
        (map (fun group => project_row select_list (values group))
          (filter keep groups))).
Proof.
  intros; eapply (@groups_success_of_exact_filter_map TNull relname
    basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule); eassumption.
Qed.

Theorem tnull_groups_success_of_exact_map_having_true :
  forall env select_list group_terms groups values,
    (forall group,
      In group groups ->
      @group_filter_map_exact_at TNull relname basesort instance unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        tnull_value_is_null boolean_schedule
        env select_list group_terms SExpr_True
        (fun _ => true) values group) ->
    @eval_groups_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      env select_list group_terms SExpr_True groups
      (SqlSuccess
        (map (fun group => project_row select_list (values group))
          groups)).
Proof.
  intros; eapply (@groups_success_of_exact_map_having_true TNull relname
    basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule); eassumption.
Qed.

Theorem tnull_group_bag_success_of_exact_filter_map :
  forall env select_list group_keys group_terms having
      input_bag representative keep values,
    scalar_group_key_terms group_keys = Some group_terms ->
    query_same_rows_as_bag representative input_bag ->
    @group_keys_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      env group_terms representative = None ->
    let groups := query_make_groups env representative group_terms in
    (forall group,
      In group groups ->
      @group_filter_map_exact_at TNull relname basesort instance unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        tnull_value_is_null boolean_schedule
        env select_list group_terms having keep values group) ->
    @eval_group_bag_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      env select_list group_keys having input_bag
      (SqlSuccess
        (query_rows_bag
          (map (fun group => project_row select_list (values group))
            (filter keep groups)))).
Proof.
  intros; eapply (@group_bag_success_of_exact_filter_map TNull relname
    basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule); eassumption.
Qed.

Theorem tnull_group_bag_success_of_exact_map_having_true :
  forall env select_list group_keys group_terms
      input_bag representative values,
    scalar_group_key_terms group_keys = Some group_terms ->
    query_same_rows_as_bag representative input_bag ->
    @group_keys_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      env group_terms representative = None ->
    let groups := query_make_groups env representative group_terms in
    (forall group,
      In group groups ->
      @group_filter_map_exact_at TNull relname basesort instance unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        tnull_value_is_null boolean_schedule
        env select_list group_terms SExpr_True
        (fun _ => true) values group) ->
    @eval_group_bag_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      env select_list group_keys SExpr_True input_bag
      (SqlSuccess
        (query_rows_bag
          (map (fun group => project_row select_list (values group))
            groups))).
Proof.
  intros; eapply (@group_bag_success_of_exact_map_having_true TNull relname
    basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule); eassumption.
Qed.

End GroupsExact_TNull.
