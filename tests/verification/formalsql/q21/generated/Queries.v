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
  [(@SExpr_Leaf TNull relname type_string (DotString "s_name" (StringChar 25)), AttrString "__logos_output_0" (StringChar 25)); (@SExpr_Leaf TNull relname type_int64 (DotInt64 "numwait"), AttrInt64 "__logos_output_1")].

Definition scalar_select_list_1 :
    list ((@scalar_expr TNull relname ScalarResultValue * Tuple.attribute TNull)%type) :=
  [(@SExpr_Leaf TNull relname type_string (DotString "s_name" (StringChar 25)), AttrString "s_name" (StringChar 25)); (@SExpr_Leaf TNull relname type_int64 (ACountStar), AttrInt64 "numwait")].

Definition scalar_select_list_2 :
    list ((@scalar_expr TNull relname ScalarResultValue * Tuple.attribute TNull)%type) :=
  firstn (1%nat) (scalar_select_list_1).

Definition scalar_select_list_3 :
    list ((@scalar_expr TNull relname ScalarResultValue * Tuple.attribute TNull)%type) :=
  [(@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_0"), AttrInt32 "s_suppkey"); (@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_1" (StringChar 25)), AttrString "s_name" (StringChar 25)); (@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_2" (StringVarcharN 40)), AttrString "s_address" (StringVarcharN 40)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_3"), AttrInt32 "s_nationkey"); (@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_4" (StringChar 15)), AttrString "s_phone" (StringChar 15)); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "__logos_cor_cor0_5" 15 2), AttrDecimal "s_acctbal" 15 2); (@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_6" (StringVarcharN 101)), AttrString "s_comment" (StringVarcharN 101)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_7"), AttrInt32 "l_orderkey"); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_8"), AttrInt32 "l_partkey"); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_9"), AttrInt32 "l_suppkey"); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_10"), AttrInt32 "l_linenumber"); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "__logos_cor_cor0_11" 15 2), AttrDecimal "l_quantity" 15 2); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "__logos_cor_cor0_12" 15 2), AttrDecimal "l_extendedprice" 15 2); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "__logos_cor_cor0_13" 15 2), AttrDecimal "l_discount" 15 2); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "__logos_cor_cor0_14" 15 2), AttrDecimal "l_tax" 15 2); (@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_15" (StringChar 1)), AttrString "l_returnflag" (StringChar 1)); (@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_16" (StringChar 1)), AttrString "l_linestatus" (StringChar 1)); (@SExpr_Leaf TNull relname type_date (DotDate "__logos_cor_cor0_17"), AttrDate "l_shipdate"); (@SExpr_Leaf TNull relname type_date (DotDate "__logos_cor_cor0_18"), AttrDate "l_commitdate"); (@SExpr_Leaf TNull relname type_date (DotDate "__logos_cor_cor0_19"), AttrDate "l_receiptdate"); (@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_20" (StringChar 25)), AttrString "l_shipinstruct" (StringChar 25)); (@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_21" (StringChar 10)), AttrString "l_shipmode" (StringChar 10)); (@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_22" (StringVarcharN 44)), AttrString "l_comment" (StringVarcharN 44)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_23"), AttrInt32 "o_orderkey"); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_24"), AttrInt32 "o_custkey"); (@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_25" (StringChar 1)), AttrString "o_orderstatus" (StringChar 1)); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "__logos_cor_cor0_26" 15 2), AttrDecimal "o_totalprice" 15 2); (@SExpr_Leaf TNull relname type_date (DotDate "__logos_cor_cor0_27"), AttrDate "o_orderdate"); (@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_28" (StringChar 15)), AttrString "o_orderpriority" (StringChar 15)); (@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_29" (StringChar 15)), AttrString "o_clerk" (StringChar 15)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_30"), AttrInt32 "o_shippriority"); (@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_31" (StringVarcharN 79)), AttrString "o_comment" (StringVarcharN 79)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_32"), AttrInt32 "n_nationkey"); (@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_33" (StringChar 25)), AttrString "n_name" (StringChar 25)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_34"), AttrInt32 "n_regionkey"); (@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_35" (StringVarcharN 152)), AttrString "n_comment" (StringVarcharN 152))].

Definition scalar_select_list_4 :
    list ((@scalar_expr TNull relname ScalarResultValue * Tuple.attribute TNull)%type) :=
  [(@SExpr_Leaf TNull relname type_int32 (DotInt32 "s_suppkey"), AttrInt32 "__logos_cor_cor0_0"); (@SExpr_Leaf TNull relname type_string (DotString "s_name" (StringChar 25)), AttrString "__logos_cor_cor0_1" (StringChar 25)); (@SExpr_Leaf TNull relname type_string (DotString "s_address" (StringVarcharN 40)), AttrString "__logos_cor_cor0_2" (StringVarcharN 40)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "s_nationkey"), AttrInt32 "__logos_cor_cor0_3"); (@SExpr_Leaf TNull relname type_string (DotString "s_phone" (StringChar 15)), AttrString "__logos_cor_cor0_4" (StringChar 15)); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "s_acctbal" 15 2), AttrDecimal "__logos_cor_cor0_5" 15 2); (@SExpr_Leaf TNull relname type_string (DotString "s_comment" (StringVarcharN 101)), AttrString "__logos_cor_cor0_6" (StringVarcharN 101)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_orderkey"), AttrInt32 "__logos_cor_cor0_7"); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_partkey"), AttrInt32 "__logos_cor_cor0_8"); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_suppkey"), AttrInt32 "__logos_cor_cor0_9"); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_linenumber"), AttrInt32 "__logos_cor_cor0_10"); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_quantity" 15 2), AttrDecimal "__logos_cor_cor0_11" 15 2); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_extendedprice" 15 2), AttrDecimal "__logos_cor_cor0_12" 15 2); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_discount" 15 2), AttrDecimal "__logos_cor_cor0_13" 15 2); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_tax" 15 2), AttrDecimal "__logos_cor_cor0_14" 15 2); (@SExpr_Leaf TNull relname type_string (DotString "l_returnflag" (StringChar 1)), AttrString "__logos_cor_cor0_15" (StringChar 1)); (@SExpr_Leaf TNull relname type_string (DotString "l_linestatus" (StringChar 1)), AttrString "__logos_cor_cor0_16" (StringChar 1)); (@SExpr_Leaf TNull relname type_date (DotDate "l_shipdate"), AttrDate "__logos_cor_cor0_17"); (@SExpr_Leaf TNull relname type_date (DotDate "l_commitdate"), AttrDate "__logos_cor_cor0_18"); (@SExpr_Leaf TNull relname type_date (DotDate "l_receiptdate"), AttrDate "__logos_cor_cor0_19"); (@SExpr_Leaf TNull relname type_string (DotString "l_shipinstruct" (StringChar 25)), AttrString "__logos_cor_cor0_20" (StringChar 25)); (@SExpr_Leaf TNull relname type_string (DotString "l_shipmode" (StringChar 10)), AttrString "__logos_cor_cor0_21" (StringChar 10)); (@SExpr_Leaf TNull relname type_string (DotString "l_comment" (StringVarcharN 44)), AttrString "__logos_cor_cor0_22" (StringVarcharN 44)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "o_orderkey"), AttrInt32 "__logos_cor_cor0_23"); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "o_custkey"), AttrInt32 "__logos_cor_cor0_24"); (@SExpr_Leaf TNull relname type_string (DotString "o_orderstatus" (StringChar 1)), AttrString "__logos_cor_cor0_25" (StringChar 1)); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "o_totalprice" 15 2), AttrDecimal "__logos_cor_cor0_26" 15 2); (@SExpr_Leaf TNull relname type_date (DotDate "o_orderdate"), AttrDate "__logos_cor_cor0_27"); (@SExpr_Leaf TNull relname type_string (DotString "o_orderpriority" (StringChar 15)), AttrString "__logos_cor_cor0_28" (StringChar 15)); (@SExpr_Leaf TNull relname type_string (DotString "o_clerk" (StringChar 15)), AttrString "__logos_cor_cor0_29" (StringChar 15)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "o_shippriority"), AttrInt32 "__logos_cor_cor0_30"); (@SExpr_Leaf TNull relname type_string (DotString "o_comment" (StringVarcharN 79)), AttrString "__logos_cor_cor0_31" (StringVarcharN 79)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "n_nationkey"), AttrInt32 "__logos_cor_cor0_32"); (@SExpr_Leaf TNull relname type_string (DotString "n_name" (StringChar 25)), AttrString "__logos_cor_cor0_33" (StringChar 25)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "n_regionkey"), AttrInt32 "__logos_cor_cor0_34"); (@SExpr_Leaf TNull relname type_string (DotString "n_comment" (StringVarcharN 152)), AttrString "__logos_cor_cor0_35" (StringVarcharN 152))].

Definition scalar_select_list_5 :
    list ((@scalar_expr TNull relname ScalarResultValue * Tuple.attribute TNull)%type) :=
  [(@SExpr_Leaf TNull relname type_int32 (DotInt32 "s_suppkey"), AttrInt32 "s_suppkey"); (@SExpr_Leaf TNull relname type_string (DotString "s_name" (StringChar 25)), AttrString "s_name" (StringChar 25)); (@SExpr_Leaf TNull relname type_string (DotString "s_address" (StringVarcharN 40)), AttrString "s_address" (StringVarcharN 40)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "s_nationkey"), AttrInt32 "s_nationkey"); (@SExpr_Leaf TNull relname type_string (DotString "s_phone" (StringChar 15)), AttrString "s_phone" (StringChar 15)); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "s_acctbal" 15 2), AttrDecimal "s_acctbal" 15 2); (@SExpr_Leaf TNull relname type_string (DotString "s_comment" (StringVarcharN 101)), AttrString "s_comment" (StringVarcharN 101)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_orderkey"), AttrInt32 "l_orderkey"); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_partkey"), AttrInt32 "l_partkey"); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_suppkey"), AttrInt32 "l_suppkey"); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_linenumber"), AttrInt32 "l_linenumber"); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_quantity" 15 2), AttrDecimal "l_quantity" 15 2); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_extendedprice" 15 2), AttrDecimal "l_extendedprice" 15 2); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_discount" 15 2), AttrDecimal "l_discount" 15 2); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_tax" 15 2), AttrDecimal "l_tax" 15 2); (@SExpr_Leaf TNull relname type_string (DotString "l_returnflag" (StringChar 1)), AttrString "l_returnflag" (StringChar 1)); (@SExpr_Leaf TNull relname type_string (DotString "l_linestatus" (StringChar 1)), AttrString "l_linestatus" (StringChar 1)); (@SExpr_Leaf TNull relname type_date (DotDate "l_shipdate"), AttrDate "l_shipdate"); (@SExpr_Leaf TNull relname type_date (DotDate "l_commitdate"), AttrDate "l_commitdate"); (@SExpr_Leaf TNull relname type_date (DotDate "l_receiptdate"), AttrDate "l_receiptdate"); (@SExpr_Leaf TNull relname type_string (DotString "l_shipinstruct" (StringChar 25)), AttrString "l_shipinstruct" (StringChar 25)); (@SExpr_Leaf TNull relname type_string (DotString "l_shipmode" (StringChar 10)), AttrString "l_shipmode" (StringChar 10)); (@SExpr_Leaf TNull relname type_string (DotString "l_comment" (StringVarcharN 44)), AttrString "l_comment" (StringVarcharN 44)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "o_orderkey"), AttrInt32 "o_orderkey"); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "o_custkey"), AttrInt32 "o_custkey"); (@SExpr_Leaf TNull relname type_string (DotString "o_orderstatus" (StringChar 1)), AttrString "o_orderstatus" (StringChar 1)); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "o_totalprice" 15 2), AttrDecimal "o_totalprice" 15 2); (@SExpr_Leaf TNull relname type_date (DotDate "o_orderdate"), AttrDate "o_orderdate"); (@SExpr_Leaf TNull relname type_string (DotString "o_orderpriority" (StringChar 15)), AttrString "o_orderpriority" (StringChar 15)); (@SExpr_Leaf TNull relname type_string (DotString "o_clerk" (StringChar 15)), AttrString "o_clerk" (StringChar 15)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "o_shippriority"), AttrInt32 "o_shippriority"); (@SExpr_Leaf TNull relname type_string (DotString "o_comment" (StringVarcharN 79)), AttrString "o_comment" (StringVarcharN 79))].

Definition scalar_select_list_6 :
    list ((@scalar_expr TNull relname ScalarResultValue * Tuple.attribute TNull)%type) :=
  firstn (23%nat) (scalar_select_list_5).

Definition scalar_select_list_7 :
    list ((@scalar_expr TNull relname ScalarResultValue * Tuple.attribute TNull)%type) :=
  firstn (7%nat) (scalar_select_list_6).

Definition scalar_select_list_8 :
    list ((@scalar_expr TNull relname ScalarResultValue * Tuple.attribute TNull)%type) :=
  [(@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_orderkey"), AttrInt32 "l_orderkey"); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_partkey"), AttrInt32 "l_partkey"); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_suppkey"), AttrInt32 "l_suppkey"); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_linenumber"), AttrInt32 "l_linenumber"); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_quantity" 15 2), AttrDecimal "l_quantity" 15 2); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_extendedprice" 15 2), AttrDecimal "l_extendedprice" 15 2); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_discount" 15 2), AttrDecimal "l_discount" 15 2); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_tax" 15 2), AttrDecimal "l_tax" 15 2); (@SExpr_Leaf TNull relname type_string (DotString "l_returnflag" (StringChar 1)), AttrString "l_returnflag" (StringChar 1)); (@SExpr_Leaf TNull relname type_string (DotString "l_linestatus" (StringChar 1)), AttrString "l_linestatus" (StringChar 1)); (@SExpr_Leaf TNull relname type_date (DotDate "l_shipdate"), AttrDate "l_shipdate"); (@SExpr_Leaf TNull relname type_date (DotDate "l_commitdate"), AttrDate "l_commitdate"); (@SExpr_Leaf TNull relname type_date (DotDate "l_receiptdate"), AttrDate "l_receiptdate"); (@SExpr_Leaf TNull relname type_string (DotString "l_shipinstruct" (StringChar 25)), AttrString "l_shipinstruct" (StringChar 25)); (@SExpr_Leaf TNull relname type_string (DotString "l_shipmode" (StringChar 10)), AttrString "l_shipmode" (StringChar 10)); (@SExpr_Leaf TNull relname type_string (DotString "l_comment" (StringVarcharN 44)), AttrString "l_comment" (StringVarcharN 44))].

Definition scalar_select_list_9 :
    list ((@scalar_expr TNull relname ScalarResultValue * Tuple.attribute TNull)%type) :=
  [(@SExpr_Leaf TNull relname type_int32 (DotInt32 "o_orderkey"), AttrInt32 "o_orderkey"); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "o_custkey"), AttrInt32 "o_custkey"); (@SExpr_Leaf TNull relname type_string (DotString "o_orderstatus" (StringChar 1)), AttrString "o_orderstatus" (StringChar 1)); (@SExpr_Leaf TNull relname type_numeric (DotDecimal "o_totalprice" 15 2), AttrDecimal "o_totalprice" 15 2); (@SExpr_Leaf TNull relname type_date (DotDate "o_orderdate"), AttrDate "o_orderdate"); (@SExpr_Leaf TNull relname type_string (DotString "o_orderpriority" (StringChar 15)), AttrString "o_orderpriority" (StringChar 15)); (@SExpr_Leaf TNull relname type_string (DotString "o_clerk" (StringChar 15)), AttrString "o_clerk" (StringChar 15)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "o_shippriority"), AttrInt32 "o_shippriority"); (@SExpr_Leaf TNull relname type_string (DotString "o_comment" (StringVarcharN 79)), AttrString "o_comment" (StringVarcharN 79))].

Definition scalar_select_list_10 :
    list ((@scalar_expr TNull relname ScalarResultValue * Tuple.attribute TNull)%type) :=
  [(@SExpr_Leaf TNull relname type_int32 (DotInt32 "n_nationkey"), AttrInt32 "n_nationkey"); (@SExpr_Leaf TNull relname type_string (DotString "n_name" (StringChar 25)), AttrString "n_name" (StringChar 25)); (@SExpr_Leaf TNull relname type_int32 (DotInt32 "n_regionkey"), AttrInt32 "n_regionkey"); (@SExpr_Leaf TNull relname type_string (DotString "n_comment" (StringVarcharN 152)), AttrString "n_comment" (StringVarcharN 152))].

Definition scalar_expr_predicate_0 :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_True TNull relname.

Definition scalar_expr_predicate_1 :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_ConjList TNull relname ([[]; ["b0"]; ["b1"; "b2"]; ["b3"; "b4"; "b5"]; ["b6"; "b7"; "b8"; "b9"]; ["b10"; "b11"; "b12"; "b13"; "b14"]; ["b15"; "b16"; "b17"; "b18"; "b19"; "b20"]; ["b21"; "b22"; "b23"; "b24"; "b25"; "b26"; "b27"]]) And_F ([@SExpr_Pred TNull relname (PredicateEq : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_0"); @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_9")]); @SExpr_Pred TNull relname (PredicateEq : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_23"); @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_7")]); @SExpr_Pred TNull relname (PredicateEq : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_25" (StringChar 1)); @SExpr_Leaf TNull relname type_string (CstString StringBpchar "F")]); @SExpr_Pred TNull relname (PredicateGt : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_date (DotDate "__logos_cor_cor0_19"); @SExpr_Leaf TNull relname type_date (DotDate "__logos_cor_cor0_18")]); @SExpr_Exists TNull relname (QExpr_Filter (@SExpr_ConjList TNull relname ([[]; ["b28"]]) And_F ([@SExpr_Pred TNull relname (PredicateEq : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_orderkey"); @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_7")]); @SExpr_Pred TNull relname (PredicateNeq : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_suppkey"); @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_9")])])) (@QExpr_Table TNull relname ([AttrInt32 "l_orderkey"; AttrInt32 "l_partkey"; AttrInt32 "l_suppkey"; AttrInt32 "l_linenumber"; AttrDecimal "l_quantity" 15 2; AttrDecimal "l_extendedprice" 15 2; AttrDecimal "l_discount" 15 2; AttrDecimal "l_tax" 15 2; AttrString "l_returnflag" (StringChar 1); AttrString "l_linestatus" (StringChar 1); AttrDate "l_shipdate"; AttrDate "l_commitdate"; AttrDate "l_receiptdate"; AttrString "l_shipinstruct" (StringChar 25); AttrString "l_shipmode" (StringChar 10); AttrString "l_comment" (StringVarcharN 44)]) (Rel "lineitem"))); @SExpr_Not TNull relname (@SExpr_Exists TNull relname (QExpr_Filter (@SExpr_ConjList TNull relname ([[]; ["b29"]; ["b30"; "b31"]]) And_F ([@SExpr_Pred TNull relname (PredicateEq : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_orderkey"); @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_7")]); @SExpr_Pred TNull relname (PredicateNeq : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_suppkey"); @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_9")]); @SExpr_Pred TNull relname (PredicateGt : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_date (DotDate "l_receiptdate"); @SExpr_Leaf TNull relname type_date (DotDate "l_commitdate")])])) (@QExpr_Table TNull relname ([AttrInt32 "l_orderkey"; AttrInt32 "l_partkey"; AttrInt32 "l_suppkey"; AttrInt32 "l_linenumber"; AttrDecimal "l_quantity" 15 2; AttrDecimal "l_extendedprice" 15 2; AttrDecimal "l_discount" 15 2; AttrDecimal "l_tax" 15 2; AttrString "l_returnflag" (StringChar 1); AttrString "l_linestatus" (StringChar 1); AttrDate "l_shipdate"; AttrDate "l_commitdate"; AttrDate "l_receiptdate"; AttrString "l_shipinstruct" (StringChar 25); AttrString "l_shipmode" (StringChar 10); AttrString "l_comment" (StringVarcharN 44)]) (Rel "lineitem")))); @SExpr_Pred TNull relname (PredicateEq : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_3"); @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_32")]); @SExpr_Pred TNull relname (PredicateEq : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_string (DotString "__logos_cor_cor0_33" (StringChar 25)); @SExpr_Leaf TNull relname type_string (CstString StringBpchar "SAUDI ARABIA")])]).

Definition scalar_expr_predicate_2 :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_ConjList TNull relname ([[]; ["b28"]]) And_F ([@SExpr_Pred TNull relname (PredicateEq : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_orderkey"); @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_7")]); @SExpr_Pred TNull relname (PredicateNeq : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_suppkey"); @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_9")])]).

Definition scalar_expr_predicate_3 :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_ConjList TNull relname ([[]; ["b29"]; ["b30"; "b31"]]) And_F ([@SExpr_Pred TNull relname (PredicateEq : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_orderkey"); @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_7")]); @SExpr_Pred TNull relname (PredicateNeq : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_suppkey"); @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_9")]); @SExpr_Pred TNull relname (PredicateGt : FTuples.Tuple.predicate TNull) ([@SExpr_Leaf TNull relname type_date (DotDate "l_receiptdate"); @SExpr_Leaf TNull relname type_date (DotDate "l_commitdate")])]).

Definition shared_query_expr_0 : QueryExpr :=
  QExpr_Project (scalar_select_list_0) (QExpr_OrderBy ([SortDescNullsFirst (AttrInt64 "numwait"); SortAscNullsLast (AttrString "s_name" (StringChar 25))]) (QExpr_Group (scalar_select_list_1) ([@SExpr_Leaf TNull relname type_string (DotString "s_name" (StringChar 25))]) (scalar_expr_predicate_0) (QExpr_Project (scalar_select_list_2) (QExpr_Project (scalar_select_list_3) (QExpr_Filter (scalar_expr_predicate_1) (QExpr_Project (scalar_select_list_4) (QExpr_Filter (scalar_expr_predicate_0) (QExpr_CrossJoin (QExpr_Project (scalar_select_list_5) (QExpr_Filter (scalar_expr_predicate_0) (QExpr_CrossJoin (QExpr_Project (scalar_select_list_6) (QExpr_Filter (scalar_expr_predicate_0) (QExpr_CrossJoin (QExpr_Project (scalar_select_list_7) (@QExpr_Table TNull relname ([AttrInt32 "s_suppkey"; AttrString "s_name" (StringChar 25); AttrString "s_address" (StringVarcharN 40); AttrInt32 "s_nationkey"; AttrString "s_phone" (StringChar 15); AttrDecimal "s_acctbal" 15 2; AttrString "s_comment" (StringVarcharN 101)]) (Rel "supplier"))) (QExpr_Project (scalar_select_list_8) (@QExpr_Table TNull relname ([AttrInt32 "l_orderkey"; AttrInt32 "l_partkey"; AttrInt32 "l_suppkey"; AttrInt32 "l_linenumber"; AttrDecimal "l_quantity" 15 2; AttrDecimal "l_extendedprice" 15 2; AttrDecimal "l_discount" 15 2; AttrDecimal "l_tax" 15 2; AttrString "l_returnflag" (StringChar 1); AttrString "l_linestatus" (StringChar 1); AttrDate "l_shipdate"; AttrDate "l_commitdate"; AttrDate "l_receiptdate"; AttrString "l_shipinstruct" (StringChar 25); AttrString "l_shipmode" (StringChar 10); AttrString "l_comment" (StringVarcharN 44)]) (Rel "lineitem")))))) (QExpr_Project (scalar_select_list_9) (@QExpr_Table TNull relname ([AttrInt32 "o_orderkey"; AttrInt32 "o_custkey"; AttrString "o_orderstatus" (StringChar 1); AttrDecimal "o_totalprice" 15 2; AttrDate "o_orderdate"; AttrString "o_orderpriority" (StringChar 15); AttrString "o_clerk" (StringChar 15); AttrInt32 "o_shippriority"; AttrString "o_comment" (StringVarcharN 79)]) (Rel "orders")))))) (QExpr_Project (scalar_select_list_10) (@QExpr_Table TNull relname ([AttrInt32 "n_nationkey"; AttrString "n_name" (StringChar 25); AttrInt32 "n_regionkey"; AttrString "n_comment" (StringVarcharN 152)]) (Rel "nation"))))))))))).

Definition shared_query_expr_0_expected_outputs :
    list (Tuple.attribute TNull) :=
  query_expr_outputs shared_query_expr_0.

Lemma generated_table_sort_0 :
  @_basesort TNull Schema.generated_schema (Rel "lineitem") =S=
  Fset.mk_set (Tuple.A TNull) ([AttrInt32 "l_orderkey"; AttrInt32 "l_partkey"; AttrInt32 "l_suppkey"; AttrInt32 "l_linenumber"; AttrDecimal "l_quantity" 15 2; AttrDecimal "l_extendedprice" 15 2; AttrDecimal "l_discount" 15 2; AttrDecimal "l_tax" 15 2; AttrString "l_returnflag" (StringChar 1); AttrString "l_linestatus" (StringChar 1); AttrDate "l_shipdate"; AttrDate "l_commitdate"; AttrDate "l_receiptdate"; AttrString "l_shipinstruct" (StringChar 25); AttrString "l_shipmode" (StringChar 10); AttrString "l_comment" (StringVarcharN 44)]).
Proof.
  solve_generated_query_metadata.
Qed.

Lemma generated_table_sort_1 :
  @_basesort TNull Schema.generated_schema (Rel "supplier") =S=
  Fset.mk_set (Tuple.A TNull) ([AttrInt32 "s_suppkey"; AttrString "s_name" (StringChar 25); AttrString "s_address" (StringVarcharN 40); AttrInt32 "s_nationkey"; AttrString "s_phone" (StringChar 15); AttrDecimal "s_acctbal" 15 2; AttrString "s_comment" (StringVarcharN 101)]).
Proof.
  solve_generated_query_metadata.
Qed.

Lemma generated_table_sort_2 :
  @_basesort TNull Schema.generated_schema (Rel "orders") =S=
  Fset.mk_set (Tuple.A TNull) ([AttrInt32 "o_orderkey"; AttrInt32 "o_custkey"; AttrString "o_orderstatus" (StringChar 1); AttrDecimal "o_totalprice" 15 2; AttrDate "o_orderdate"; AttrString "o_orderpriority" (StringChar 15); AttrString "o_clerk" (StringChar 15); AttrInt32 "o_shippriority"; AttrString "o_comment" (StringVarcharN 79)]).
Proof.
  solve_generated_query_metadata.
Qed.

Lemma generated_table_sort_3 :
  @_basesort TNull Schema.generated_schema (Rel "nation") =S=
  Fset.mk_set (Tuple.A TNull) ([AttrInt32 "n_nationkey"; AttrString "n_name" (StringChar 25); AttrInt32 "n_regionkey"; AttrString "n_comment" (StringVarcharN 152)]).
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
  split.
  {
  solve_generated_query_metadata.
  }
  {
  split.
  {
  solve_generated_query_metadata.
  }
  {
  split.
  {
  solve_generated_query_metadata.
  }
  {
  constructor.
  {
  solve_generated_query_free_scalar_admissibility.
  }
  {
  constructor.
  {
  solve_generated_query_free_scalar_admissibility.
  }
  {
  constructor.
  {
  solve_generated_query_free_scalar_admissibility.
  }
  {
  constructor.
  {
  solve_generated_query_free_scalar_admissibility.
  }
  {
  constructor.
  {
  split.
  {
  solve_generated_query_metadata.
  }
  {
  eapply query_expr_admissible_of_with_outputs.
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
  solve_generated_query_free_scalar_admissibility.
  }
  }
  }
  }
  {
  constructor.
  {
  split.
  {
  solve_generated_query_metadata.
  }
  {
  eapply query_expr_admissible_of_with_outputs.
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
  solve_generated_query_free_scalar_admissibility.
  }
  }
  }
  }
  {
  constructor.
  {
  solve_generated_query_free_scalar_admissibility.
  }
  {
  constructor.
  {
  solve_generated_query_free_scalar_admissibility.
  }
  {
  constructor.
  }
  }
  }
  }
  }
  }
  }
  }
  }
  }
  }
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

Lemma scalar_expr_predicate_2_admissible_where_generated_schema :
  TNullScalarExprAdmissible
    (@_basesort TNull Schema.generated_schema) ScalarPhaseWhere scalar_expr_predicate_2.
Proof.
  unfold TNullScalarExprAdmissible, scalar_expr_predicate_2.
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

Lemma scalar_expr_predicate_3_admissible_where_generated_schema :
  TNullScalarExprAdmissible
    (@_basesort TNull Schema.generated_schema) ScalarPhaseWhere scalar_expr_predicate_3.
Proof.
  unfold TNullScalarExprAdmissible, scalar_expr_predicate_3.
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

Lemma scalar_expr_predicate_0_admissible_where_generated_schema :
  TNullScalarExprAdmissible
    (@_basesort TNull Schema.generated_schema) ScalarPhaseWhere scalar_expr_predicate_0.
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
  unfold scalar_select_list_2, scalar_select_list_1.
  solve_generated_query_free_select_list_admissibility.
Qed.

Lemma scalar_select_list_3_admissible_row_select_generated_schema :
  prop_forall
    (fun item =>
      @scalar_expr_admissible TNull relname
        (@_basesort TNull Schema.generated_schema)
        TNullLeafHasType TNullCallHasType TNullPredicateHasTypes
        type_int64 type_bool NullValues.is_null_value
        ScalarPhaseRowSelect ScalarResultValue (fst item) /\
      scalar_expr_type (fst item) =
        Tuple.type_of_attribute TNull (snd item))
    scalar_select_list_3.
Proof.
  unfold scalar_select_list_3.
  solve_generated_query_free_select_list_admissibility.
Qed.

Lemma scalar_select_list_4_admissible_row_select_generated_schema :
  prop_forall
    (fun item =>
      @scalar_expr_admissible TNull relname
        (@_basesort TNull Schema.generated_schema)
        TNullLeafHasType TNullCallHasType TNullPredicateHasTypes
        type_int64 type_bool NullValues.is_null_value
        ScalarPhaseRowSelect ScalarResultValue (fst item) /\
      scalar_expr_type (fst item) =
        Tuple.type_of_attribute TNull (snd item))
    scalar_select_list_4.
Proof.
  unfold scalar_select_list_4.
  solve_generated_query_free_select_list_admissibility.
Qed.

Lemma scalar_select_list_5_admissible_row_select_generated_schema :
  prop_forall
    (fun item =>
      @scalar_expr_admissible TNull relname
        (@_basesort TNull Schema.generated_schema)
        TNullLeafHasType TNullCallHasType TNullPredicateHasTypes
        type_int64 type_bool NullValues.is_null_value
        ScalarPhaseRowSelect ScalarResultValue (fst item) /\
      scalar_expr_type (fst item) =
        Tuple.type_of_attribute TNull (snd item))
    scalar_select_list_5.
Proof.
  unfold scalar_select_list_5.
  solve_generated_query_free_select_list_admissibility.
Qed.

Lemma scalar_select_list_6_admissible_row_select_generated_schema :
  prop_forall
    (fun item =>
      @scalar_expr_admissible TNull relname
        (@_basesort TNull Schema.generated_schema)
        TNullLeafHasType TNullCallHasType TNullPredicateHasTypes
        type_int64 type_bool NullValues.is_null_value
        ScalarPhaseRowSelect ScalarResultValue (fst item) /\
      scalar_expr_type (fst item) =
        Tuple.type_of_attribute TNull (snd item))
    scalar_select_list_6.
Proof.
  unfold scalar_select_list_6.
  eapply (@prop_forall_firstn
    _ _ 23%nat (scalar_select_list_5)).
  exact (scalar_select_list_5_admissible_row_select_generated_schema).
Qed.

Lemma scalar_select_list_7_admissible_row_select_generated_schema :
  prop_forall
    (fun item =>
      @scalar_expr_admissible TNull relname
        (@_basesort TNull Schema.generated_schema)
        TNullLeafHasType TNullCallHasType TNullPredicateHasTypes
        type_int64 type_bool NullValues.is_null_value
        ScalarPhaseRowSelect ScalarResultValue (fst item) /\
      scalar_expr_type (fst item) =
        Tuple.type_of_attribute TNull (snd item))
    scalar_select_list_7.
Proof.
  unfold scalar_select_list_7.
  eapply (@prop_forall_firstn
    _ _ 7%nat (scalar_select_list_6)).
  exact (scalar_select_list_6_admissible_row_select_generated_schema).
Qed.

Lemma scalar_select_list_8_admissible_row_select_generated_schema :
  prop_forall
    (fun item =>
      @scalar_expr_admissible TNull relname
        (@_basesort TNull Schema.generated_schema)
        TNullLeafHasType TNullCallHasType TNullPredicateHasTypes
        type_int64 type_bool NullValues.is_null_value
        ScalarPhaseRowSelect ScalarResultValue (fst item) /\
      scalar_expr_type (fst item) =
        Tuple.type_of_attribute TNull (snd item))
    scalar_select_list_8.
Proof.
  unfold scalar_select_list_8.
  solve_generated_query_free_select_list_admissibility.
Qed.

Lemma scalar_select_list_9_admissible_row_select_generated_schema :
  prop_forall
    (fun item =>
      @scalar_expr_admissible TNull relname
        (@_basesort TNull Schema.generated_schema)
        TNullLeafHasType TNullCallHasType TNullPredicateHasTypes
        type_int64 type_bool NullValues.is_null_value
        ScalarPhaseRowSelect ScalarResultValue (fst item) /\
      scalar_expr_type (fst item) =
        Tuple.type_of_attribute TNull (snd item))
    scalar_select_list_9.
Proof.
  unfold scalar_select_list_9.
  solve_generated_query_free_select_list_admissibility.
Qed.

Lemma scalar_select_list_10_admissible_row_select_generated_schema :
  prop_forall
    (fun item =>
      @scalar_expr_admissible TNull relname
        (@_basesort TNull Schema.generated_schema)
        TNullLeafHasType TNullCallHasType TNullPredicateHasTypes
        type_int64 type_bool NullValues.is_null_value
        ScalarPhaseRowSelect ScalarResultValue (fst item) /\
      scalar_expr_type (fst item) =
        Tuple.type_of_attribute TNull (snd item))
    scalar_select_list_10.
Proof.
  unfold scalar_select_list_10.
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
eapply query_expr_admissible_with_outputs_project.
{
eapply query_expr_admissible_with_outputs_filter.
{
eapply query_expr_admissible_with_outputs_project.
{
eapply query_expr_admissible_with_outputs_filter.
{
eapply query_expr_admissible_with_outputs_cross_join.
{
eapply query_expr_admissible_with_outputs_project.
{
eapply query_expr_admissible_with_outputs_filter.
{
eapply query_expr_admissible_with_outputs_cross_join.
{
eapply query_expr_admissible_with_outputs_project.
{
eapply query_expr_admissible_with_outputs_filter.
{
eapply query_expr_admissible_with_outputs_cross_join.
{
eapply query_expr_admissible_with_outputs_project.
{
eapply query_expr_admissible_with_outputs_table.
{
solve_generated_query_metadata.
}
{
apply generated_table_sort_1.
}
}
{
solve_generated_query_metadata.
}
{
apply (scalar_select_list_7_admissible_row_select_generated_schema).
}
}
{
eapply query_expr_admissible_with_outputs_project.
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
solve_generated_query_metadata.
}
{
apply (scalar_select_list_8_admissible_row_select_generated_schema).
}
}
{
solve_generated_query_metadata.
}
}
{
apply (scalar_expr_predicate_0_admissible_where_generated_schema).
}
}
{
solve_generated_query_metadata.
}
{
apply (scalar_select_list_6_admissible_row_select_generated_schema).
}
}
{
eapply query_expr_admissible_with_outputs_project.
{
eapply query_expr_admissible_with_outputs_table.
{
solve_generated_query_metadata.
}
{
apply generated_table_sort_2.
}
}
{
solve_generated_query_metadata.
}
{
apply (scalar_select_list_9_admissible_row_select_generated_schema).
}
}
{
solve_generated_query_metadata.
}
}
{
apply (scalar_expr_predicate_0_admissible_where_generated_schema).
}
}
{
solve_generated_query_metadata.
}
{
apply (scalar_select_list_5_admissible_row_select_generated_schema).
}
}
{
eapply query_expr_admissible_with_outputs_project.
{
eapply query_expr_admissible_with_outputs_table.
{
solve_generated_query_metadata.
}
{
apply generated_table_sort_3.
}
}
{
solve_generated_query_metadata.
}
{
apply (scalar_select_list_10_admissible_row_select_generated_schema).
}
}
{
solve_generated_query_metadata.
}
}
{
apply (scalar_expr_predicate_0_admissible_where_generated_schema).
}
}
{
solve_generated_query_metadata.
}
{
apply (scalar_select_list_4_admissible_row_select_generated_schema).
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
apply (scalar_select_list_3_admissible_row_select_generated_schema).
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
  [AttrString "__logos_output_0" (StringChar 25); AttrInt64 "__logos_output_1"].

Definition source_output_signature : list (Tuple.attribute TNull) :=
  [AttrString "__logos_output_0" (StringChar 25); AttrInt64 "__logos_output_1"].

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
  [AttrString "__logos_output_0" (StringChar 25); AttrInt64 "__logos_output_1"].

Definition target_output_signature : list (Tuple.attribute TNull) :=
  [AttrString "__logos_output_0" (StringChar 25); AttrInt64 "__logos_output_1"].

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
