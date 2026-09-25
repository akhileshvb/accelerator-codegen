From SQLFS Require Import FTuples FiniteSet FiniteBag FiniteCollection FlatData SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueNumericTypmod ValueString SchemaConstraints SqlOutcome SqlOrder Formula SqlQuerySyntax SqlQuerySemantics SqlQueryWellFormed.
From Logos Require Import FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax.
From LogosGenerated Require Import Schema.
From Stdlib Require Import String ZArith List.
Import ListNotations.
Open Scope string_scope.
Open Scope Z_scope.

(* Each call is confined to one emitter-known, closed metadata side condition;
   recursive query admissibility is discharged compositionally below. *)
Ltac solve_generated_query_metadata :=
  first [
    intros;
    repeat match goal with
    | H : False |- _ => contradiction
    | H : _ /\ _ |- _ => destruct H
    | H : _ \/ _ |- _ => destruct H
    | H : exists _, _ |- _ => destruct H
    | H : ?left = ?right |- _ => first [subst left | subst right]
    end;
    solve [contradiction | reflexivity | congruence | tauto |
           eexists; reflexivity]
  | cbn;
    intros;
    repeat match goal with
    | H : False |- _ => contradiction
    | H : _ /\ _ |- _ => destruct H
    | H : _ \/ _ |- _ => destruct H
    | H : exists _, _ |- _ => destruct H
    | H : ?left = ?right |- _ => first [subst left | subst right]
    end;
    solve [contradiction | reflexivity | congruence | tauto |
           eexists; reflexivity]
  ].

(* This tactic is used only for one closed scalar-signature equality at a
   time.  Keep reduction local to the TNull type catalog: typed scalar
   certificates below traverse query and expression syntax compositionally. *)
Ltac solve_generated_scalar_type :=
  unfold TNullLeafHasType, TNullCallHasType, TNullPredicateHasTypes;
  cbn [scalar_expr_type
       TNullAggTermType TNullAggTermTypeFuel TNullAggTermTypesFuel
       TNullFunTermType TNullFunTermTypeFuel TNullFunTermTypesFuel
       TNullScalarOperatorOutputType TNullRequireArgumentTypes
       TNullTypeListEqb TNullTypeEqb TNullPredicateArgumentTypesValid
       TNullEqualityPairTypes TNullGenericOrderPairTypes TNullIntegralType
       TNullNumericKindType TNullNumericSourceType TNullCaseResultType
       TNullAggregateFunctionArgumentTypeValid
       TNullAggregateArgumentTypeValid TNullAggregateOutputType
       TNullAggregateFunctionOutputType];
  repeat split; reflexivity.

(* Query-free scalar trees are closed syntax. This bounded structural tactic
   proves their one canonical phase-and-type judgment without serializing a
   whitespace-heavy proof tree into every generated query certificate. *)
Ltac solve_generated_query_free_scalar_admissibility :=
  cbn [scalar_expr_admissible prop_forall];
  repeat split;
  first [match goal with
         | |- forall truth, _ => intros []; reflexivity
         end |
         left; reflexivity | right; reflexivity |
         solve_generated_scalar_type | solve_generated_query_metadata].

Ltac solve_generated_query_free_scalar_list_admissibility :=
  cbn [prop_forall fst snd];
  lazymatch goal with
  | |- True => constructor
  | |- _ /\ _ => split;
      [ solve_generated_query_free_scalar_admissibility
      | solve_generated_query_free_scalar_list_admissibility ]
  end.

Ltac solve_generated_query_free_select_list_admissibility :=
  cbn [prop_forall fst snd firstn app];
  lazymatch goal with
  | |- True => constructor
  | |- (_ /\ _) /\ _ => split;
      [ split;
        [ solve_generated_query_free_scalar_admissibility
        | solve_generated_query_metadata ]
      | solve_generated_query_free_select_list_admissibility ]
  end.

Definition scalar_select_list_0 :
    list ((@scalar_expr TNull relname ScalarResultValue * Tuple.attribute TNull)%type) :=
  [(@SExpr_Leaf TNull relname type_string (DotString "l_returnflag" (StringChar 1)), AttrString "__logos_output_0" (StringChar 1)); (@SExpr_Leaf TNull relname type_string (DotString "l_linestatus" (StringChar 1)), AttrString "__logos_output_1" (StringChar 1)); (@SExpr_Leaf TNull relname type_numeric (DotNumeric "sum_qty"), AttrNumeric "__logos_output_2"); (@SExpr_Leaf TNull relname type_numeric (DotNumeric "sum_base_price"), AttrNumeric "__logos_output_3"); (@SExpr_Leaf TNull relname type_numeric (DotNumeric "sum_disc_price"), AttrNumeric "__logos_output_4"); (@SExpr_Leaf TNull relname type_numeric (DotNumeric "sum_charge"), AttrNumeric "__logos_output_5"); (@SExpr_Leaf TNull relname type_numeric (DotNumeric "avg_qty"), AttrNumeric "__logos_output_6"); (@SExpr_Leaf TNull relname type_numeric (DotNumeric "avg_price"), AttrNumeric "__logos_output_7"); (@SExpr_Leaf TNull relname type_numeric (DotNumeric "avg_disc"), AttrNumeric "__logos_output_8"); (@SExpr_Leaf TNull relname type_int64 (DotInt64 "count_order"), AttrInt64 "__logos_output_9")].

Definition scalar_select_list_1 :
    list ((@scalar_expr TNull relname ScalarResultValue * Tuple.attribute TNull)%type) :=
  [(@SExpr_Leaf TNull relname type_string (DotString "l_returnflag" (StringChar 1)), AttrString "l_returnflag" (StringChar 1)); (@SExpr_Leaf TNull relname type_string (DotString "l_linestatus" (StringChar 1)), AttrString "l_linestatus" (StringChar 1)); (@SExpr_Leaf TNull relname type_numeric (AAggregate AggregateSumNumeric AggregateAll (Dot (AttrDecimal "l_quantity" 15 2))), AttrNumeric "sum_qty"); (@SExpr_Leaf TNull relname type_numeric (AAggregate AggregateSumNumeric AggregateAll (Dot (AttrDecimal "l_extendedprice" 15 2))), AttrNumeric "sum_base_price"); (@SExpr_Leaf TNull relname type_numeric (AAggregate AggregateSumNumeric AggregateAll (Dot (AttrNumeric "$f4"))), AttrNumeric "sum_disc_price"); (@SExpr_Leaf TNull relname type_numeric (AAggregate AggregateSumNumeric AggregateAll (Dot (AttrNumeric "$f5"))), AttrNumeric "sum_charge"); (@SExpr_Leaf TNull relname type_numeric (AAggregate (AggregateAverageNumericFixed (15)%Z (2)%Z) AggregateAll (Dot (AttrDecimal "l_quantity" 15 2))), AttrNumeric "avg_qty"); (@SExpr_Leaf TNull relname type_numeric (AAggregate (AggregateAverageNumericFixed (15)%Z (2)%Z) AggregateAll (Dot (AttrDecimal "l_extendedprice" 15 2))), AttrNumeric "avg_price"); (@SExpr_Leaf TNull relname type_numeric (AAggregate (AggregateAverageNumericFixed (15)%Z (2)%Z) AggregateAll (Dot (AttrDecimal "l_discount" 15 2))), AttrNumeric "avg_disc"); (@SExpr_Leaf TNull relname type_int64 (ACountStar), AttrInt64 "count_order")].

Definition scalar_select_list_2 :
    list ((@scalar_expr TNull relname ScalarResultValue * Tuple.attribute TNull)%type) :=
  [(@SExpr_Leaf TNull relname type_string (DotString "l_returnflag" (StringChar 1)), AttrString "l_returnflag" (StringChar 1)); (@SExpr_Leaf TNull relname type_string (DotString "l_linestatus" (StringChar 1)), AttrString "l_linestatus" (StringChar 1)); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_quantity" 15 2), AttrDecimal "l_quantity" 15 2); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_extendedprice" 15 2), AttrDecimal "l_extendedprice" 15 2); (@SExpr_Call TNull relname type_numeric (ScalarMultiply ScalarNumeric) ([@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_extendedprice" 15 2); @SExpr_Call TNull relname type_numeric (ScalarSubtract ScalarNumeric) ([@SExpr_Call TNull relname type_numeric (ScalarCast (ScalarCastToNumeric ScalarSourceInt32)) ([@SExpr_Leaf TNull relname type_int32 (CstInt32 (1))]); @SExpr_Leaf TNull relname type_numeric (DotDecimal "l_discount" 15 2)])]), AttrNumeric "$f4"); (@SExpr_Call TNull relname type_numeric (ScalarMultiply ScalarNumeric) ([@SExpr_Call TNull relname type_numeric (ScalarMultiply ScalarNumeric) ([@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_extendedprice" 15 2); @SExpr_Call TNull relname type_numeric (ScalarSubtract ScalarNumeric) ([@SExpr_Call TNull relname type_numeric (ScalarCast (ScalarCastToNumeric ScalarSourceInt32)) ([@SExpr_Leaf TNull relname type_int32 (CstInt32 (1))]); @SExpr_Leaf TNull relname type_numeric (DotDecimal "l_discount" 15 2)])]); @SExpr_Call TNull relname type_numeric (ScalarAdd ScalarNumeric) ([@SExpr_Call TNull relname type_numeric (ScalarCast (ScalarCastToNumeric ScalarSourceInt32)) ([@SExpr_Leaf TNull relname type_int32 (CstInt32 (1))]); @SExpr_Leaf TNull relname type_numeric (DotDecimal "l_tax" 15 2)])]), AttrNumeric "$f5"); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_discount" 15 2), AttrDecimal "l_discount" 15 2)].

Definition scalar_expr_predicate_0 :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_True TNull relname.

Definition scalar_expr_predicate_1 :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname (PredicateDateLteTimestamp : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_date (DotDate "l_shipdate"); @SExpr_Call TNull relname type_timestamp (ScalarTimestampAdd ScalarTimestampDay) ([@SExpr_Call TNull relname type_timestamp (ScalarCast ScalarCastDateToTimestamp) ([@SExpr_Leaf TNull relname type_date (CstDate (10561))]); @SExpr_Leaf TNull relname type_Z (CstZ (-90))])]).

Definition shared_query_expr_0 : QueryExpr :=
  QExpr_Project (scalar_select_list_0) (QExpr_OrderBy ([SortAscNullsLast (AttrString "l_returnflag" (StringChar 1)); SortAscNullsLast (AttrString "l_linestatus" (StringChar 1))]) (QExpr_Group (scalar_select_list_1) ([@SExpr_Leaf TNull relname type_string (DotString "l_returnflag" (StringChar 1)); @SExpr_Leaf TNull relname type_string (DotString "l_linestatus" (StringChar 1))]) (scalar_expr_predicate_0) (QExpr_Project (scalar_select_list_2) (QExpr_Filter (scalar_expr_predicate_1) (@QExpr_Table TNull relname ([AttrInt64 "l_orderkey"; AttrInt64 "l_partkey"; AttrInt64 "l_suppkey"; AttrInt32 "l_linenumber"; AttrDecimal "l_quantity" 15 2; AttrDecimal "l_extendedprice" 15 2; AttrDecimal "l_discount" 15 2; AttrDecimal "l_tax" 15 2; AttrString "l_returnflag" (StringChar 1); AttrString "l_linestatus" (StringChar 1); AttrDate "l_shipdate"; AttrDate "l_commitdate"; AttrDate "l_receiptdate"; AttrString "l_shipinstruct" (StringChar 25); AttrString "l_shipmode" (StringChar 10); AttrString "l_comment" (StringVarcharN 44)]) (Rel "lineitem")))))).

Definition shared_query_expr_0_expected_outputs :
    list (Tuple.attribute TNull) :=
  query_expr_outputs shared_query_expr_0.

Lemma generated_table_sort_0 :
  @_basesort TNull Schema.generated_schema (Rel "lineitem") =S=
  Fset.mk_set (Tuple.A TNull) ([AttrInt64 "l_orderkey"; AttrInt64 "l_partkey"; AttrInt64 "l_suppkey"; AttrInt32 "l_linenumber"; AttrDecimal "l_quantity" 15 2; AttrDecimal "l_extendedprice" 15 2; AttrDecimal "l_discount" 15 2; AttrDecimal "l_tax" 15 2; AttrString "l_returnflag" (StringChar 1); AttrString "l_linestatus" (StringChar 1); AttrDate "l_shipdate"; AttrDate "l_commitdate"; AttrDate "l_receiptdate"; AttrString "l_shipinstruct" (StringChar 25); AttrString "l_shipmode" (StringChar 10); AttrString "l_comment" (StringVarcharN 44)]).
Proof.
  solve_generated_query_metadata.
Qed.

Lemma scalar_expr_predicate_0_admissible_having_generated_schema :
  TNullScalarExprAdmissible
    (@_basesort TNull Schema.generated_schema) ScalarPhaseHaving scalar_expr_predicate_0.
Proof.
  unfold TNullScalarExprAdmissible, scalar_expr_predicate_0.
  split.
  {
  solve_generated_query_free_scalar_admissibility.
  }
  {
  split.
  {
  reflexivity.
  }
  {
  unfold scalar_expr_boolean_sites_well_formed.
  apply boolean_sites_well_formedb_sound.
  reflexivity.
  }
  }
Qed.

Lemma scalar_expr_predicate_1_admissible_where_generated_schema :
  TNullScalarExprAdmissible
    (@_basesort TNull Schema.generated_schema) ScalarPhaseWhere scalar_expr_predicate_1.
Proof.
  unfold TNullScalarExprAdmissible, scalar_expr_predicate_1.
  split.
  {
  solve_generated_query_free_scalar_admissibility.
  }
  {
  split.
  {
  reflexivity.
  }
  {
  unfold scalar_expr_boolean_sites_well_formed.
  apply boolean_sites_well_formedb_sound.
  reflexivity.
  }
  }
Qed.

Lemma scalar_select_list_0_admissible_row_select_generated_schema :
  prop_forall
    (fun item =>
      @scalar_expr_admissible TNull relname
        (@_basesort TNull Schema.generated_schema)
        TNullLeafHasType TNullCallHasType TNullPredicateHasTypes
        type_int64 type_bool NullValues.is_null_value
        ScalarPhaseRowSelect ScalarResultValue (fst item) /\
      scalar_expr_type (fst item) =
        Tuple.type_of_attribute TNull (snd item))
    scalar_select_list_0.
Proof.
  unfold scalar_select_list_0.
  solve_generated_query_free_select_list_admissibility.
Qed.

Lemma scalar_select_list_1_admissible_select_generated_schema :
  prop_forall
    (fun item =>
      @scalar_expr_admissible TNull relname
        (@_basesort TNull Schema.generated_schema)
        TNullLeafHasType TNullCallHasType TNullPredicateHasTypes
        type_int64 type_bool NullValues.is_null_value
        ScalarPhaseSelect ScalarResultValue (fst item) /\
      scalar_expr_type (fst item) =
        Tuple.type_of_attribute TNull (snd item))
    scalar_select_list_1.
Proof.
  unfold scalar_select_list_1.
  solve_generated_query_free_select_list_admissibility.
Qed.

Lemma scalar_select_list_2_admissible_row_select_generated_schema :
  prop_forall
    (fun item =>
      @scalar_expr_admissible TNull relname
        (@_basesort TNull Schema.generated_schema)
        TNullLeafHasType TNullCallHasType TNullPredicateHasTypes
        type_int64 type_bool NullValues.is_null_value
        ScalarPhaseRowSelect ScalarResultValue (fst item) /\
      scalar_expr_type (fst item) =
        Tuple.type_of_attribute TNull (snd item))
    scalar_select_list_2.
Proof.
  unfold scalar_select_list_2.
  solve_generated_query_free_select_list_admissibility.
Qed.

Lemma shared_query_expr_0_admissible_with_outputs_generated_schema :
  TNullQueryExprAdmissibleWithOutputs
    (@_basesort TNull Schema.generated_schema)
    shared_query_expr_0 shared_query_expr_0_expected_outputs.
Proof.
  unfold shared_query_expr_0, shared_query_expr_0_expected_outputs.
  eapply TNullQueryExprAdmissibleWithOutputs_intro.
  - eapply query_expr_admissible_with_outputs_change.
{
eapply query_expr_admissible_with_outputs_project.
{
eapply query_expr_admissible_with_outputs_order_by.
{
eapply query_expr_admissible_with_outputs_group.
{
eapply query_expr_admissible_with_outputs_project.
{
eapply query_expr_admissible_with_outputs_filter.
{
eapply query_expr_admissible_with_outputs_table.
{
solve_generated_query_metadata.
}
{
apply generated_table_sort_0.
}
}
{
apply (scalar_expr_predicate_1_admissible_where_generated_schema).
}
}
{
solve_generated_query_metadata.
}
{
apply (scalar_select_list_2_admissible_row_select_generated_schema).
}
}
{
solve_generated_query_metadata.
}
{
apply (scalar_select_list_1_admissible_select_generated_schema).
}
{
apply (scalar_expr_predicate_0_admissible_having_generated_schema).
}
{
solve_generated_query_free_scalar_list_admissibility.
}
{
solve_generated_query_metadata.
}
}
{
eapply query_sort_keys_in_outputs.
{
constructor.
{
solve_generated_query_metadata.
}
{
constructor.
{
solve_generated_query_metadata.
}
{
constructor.
}
}
}
}
}
{
solve_generated_query_metadata.
}
{
apply (scalar_select_list_0_admissible_row_select_generated_schema).
}
}
{
solve_generated_query_metadata.
}
  - reflexivity.
  - unfold query_expr_boolean_sites_well_formed.
    apply boolean_sites_well_formedb_sound.
    reflexivity.
Qed.

Definition source_query_expr : QueryExpr :=
  shared_query_expr_0.

Definition source_query_expr_expected_outputs :
    list (Tuple.attribute TNull) :=
  [AttrString "__logos_output_0" (StringChar 1); AttrString "__logos_output_1" (StringChar 1); AttrNumeric "__logos_output_2"; AttrNumeric "__logos_output_3"; AttrNumeric "__logos_output_4"; AttrNumeric "__logos_output_5"; AttrNumeric "__logos_output_6"; AttrNumeric "__logos_output_7"; AttrNumeric "__logos_output_8"; AttrInt64 "__logos_output_9"].

Definition source_output_signature : list (Tuple.attribute TNull) :=
  [AttrString "__logos_output_0" (StringChar 1); AttrString "__logos_output_1" (StringChar 1); AttrNumeric "__logos_output_2"; AttrNumeric "__logos_output_3"; AttrNumeric "__logos_output_4"; AttrNumeric "__logos_output_5"; AttrNumeric "__logos_output_6"; AttrNumeric "__logos_output_7"; AttrNumeric "__logos_output_8"; AttrInt64 "__logos_output_9"].

Lemma source_query_expr_admissible_with_outputs_generated_schema :
  TNullQueryExprAdmissibleWithOutputs
    (@_basesort TNull Schema.generated_schema)
    source_query_expr source_query_expr_expected_outputs.
Proof.
  unfold source_query_expr, source_query_expr_expected_outputs.
  eapply TNullQueryExprAdmissibleWithOutputs_intro.
  - split.
{
exact (proj1 (proj1 (shared_query_expr_0_admissible_with_outputs_generated_schema))).
}
{
exact (proj2 (shared_query_expr_0_admissible_with_outputs_generated_schema)).
}
  - reflexivity.
  - unfold query_expr_boolean_sites_well_formed.
    apply boolean_sites_well_formedb_sound.
    reflexivity.
Qed.

Lemma source_query_expr_admissible (db : db_state) :
  Schema.generated_schema_conforms db ->
  TNullQueryExprAdmissible (@_basesort TNull db)
    source_query_expr.
Proof.
  intro Hschema.
  apply (query_expr_admissible_database_schema_transport
    Schema.generated_schema Schema.generated_schema_constraints db).
  { exact Hschema. }
  { exact (proj1 source_query_expr_admissible_with_outputs_generated_schema). }
Qed.

Definition target_query_expr : QueryExpr :=
  shared_query_expr_0.

Definition target_query_expr_expected_outputs :
    list (Tuple.attribute TNull) :=
  [AttrString "__logos_output_0" (StringChar 1); AttrString "__logos_output_1" (StringChar 1); AttrNumeric "__logos_output_2"; AttrNumeric "__logos_output_3"; AttrNumeric "__logos_output_4"; AttrNumeric "__logos_output_5"; AttrNumeric "__logos_output_6"; AttrNumeric "__logos_output_7"; AttrNumeric "__logos_output_8"; AttrInt64 "__logos_output_9"].

Definition target_output_signature : list (Tuple.attribute TNull) :=
  [AttrString "__logos_output_0" (StringChar 1); AttrString "__logos_output_1" (StringChar 1); AttrNumeric "__logos_output_2"; AttrNumeric "__logos_output_3"; AttrNumeric "__logos_output_4"; AttrNumeric "__logos_output_5"; AttrNumeric "__logos_output_6"; AttrNumeric "__logos_output_7"; AttrNumeric "__logos_output_8"; AttrInt64 "__logos_output_9"].

Lemma target_query_expr_admissible_with_outputs_generated_schema :
  TNullQueryExprAdmissibleWithOutputs
    (@_basesort TNull Schema.generated_schema)
    target_query_expr target_query_expr_expected_outputs.
Proof.
  unfold target_query_expr, target_query_expr_expected_outputs.
  eapply TNullQueryExprAdmissibleWithOutputs_intro.
  - split.
{
exact (proj1 (proj1 (shared_query_expr_0_admissible_with_outputs_generated_schema))).
}
{
exact (proj2 (shared_query_expr_0_admissible_with_outputs_generated_schema)).
}
  - reflexivity.
  - unfold query_expr_boolean_sites_well_formed.
    apply boolean_sites_well_formedb_sound.
    reflexivity.
Qed.

Lemma target_query_expr_admissible (db : db_state) :
  Schema.generated_schema_conforms db ->
  TNullQueryExprAdmissible (@_basesort TNull db)
    target_query_expr.
Proof.
  intro Hschema.
  apply (query_expr_admissible_database_schema_transport
    Schema.generated_schema Schema.generated_schema_constraints db).
  { exact Hschema. }
  { exact (proj1 target_query_expr_admissible_with_outputs_generated_schema). }
Qed.

Definition source_query_program : list QueryExpr :=
  [source_query_expr].

Definition target_query_program : list QueryExpr :=
  [target_query_expr].

Definition source_program_output_signatures : list (list (Tuple.attribute TNull)) :=
  [source_output_signature].

Definition target_program_output_signatures : list (list (Tuple.attribute TNull)) :=
  [target_output_signature].

Lemma source_query_program_admissible_generated_schema :
  Forall
    (TNullQueryExprAdmissible
      (@_basesort TNull Schema.generated_schema))
    source_query_program.
Proof.
  unfold source_query_program.
  constructor.
  {
  exact (proj1 source_query_expr_admissible_with_outputs_generated_schema).
  }
  {
  constructor.
  }
Qed.

Lemma source_query_program_admissible
    (db : db_state) :
  Schema.generated_schema_conforms db ->
  Forall
    (TNullQueryExprAdmissible (@_basesort TNull db))
    source_query_program.
Proof.
  intro Hschema.
  unfold source_query_program.
  constructor.
  {
  apply (source_query_expr_admissible db Hschema).
  }
  {
  constructor.
  }
Qed.

Lemma target_query_program_admissible_generated_schema :
  Forall
    (TNullQueryExprAdmissible
      (@_basesort TNull Schema.generated_schema))
    target_query_program.
Proof.
  unfold target_query_program.
  constructor.
  {
  exact (proj1 target_query_expr_admissible_with_outputs_generated_schema).
  }
  {
  constructor.
  }
Qed.

Lemma target_query_program_admissible
    (db : db_state) :
  Schema.generated_schema_conforms db ->
  Forall
    (TNullQueryExprAdmissible (@_basesort TNull db))
    target_query_program.
Proof.
  intro Hschema.
  unfold target_query_program.
  constructor.
  {
  apply (target_query_expr_admissible db Hschema).
  }
  {
  constructor.
  }
Qed.
