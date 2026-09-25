(* Q12NormalizeSound — Eval_FS(qhat_Q12) = Embed(EvalSpec(S_Q12)) scaffolding.

   Compositional split:
   - Spec-side join lookup / filter / CASE / CollectModes / SortByMode:
       Qed in Q12SemanticBridge (operator lemmas).
   - normalize(qhat)=Some(S_Q12_catalog): Qed.
   - Bag-sort + FormalSQL_exec of_holds: Qed in Q12GroupsCorrespond
       (NormalizeSound_Q12 under Pre + domain/priority/sum_bound packaging;
        no GroupRowsPerm residual).
   - Corresponds_Q12(C++, Model): documented foundational TCB.
*)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueNumericTypmod
  ValueString ValueInteger ValueTemporal Formula SchemaConstraints SqlOutcome
  SqlQuerySyntax SqlQuerySemantics SqlQueryFacts SqlErrorSemantics SqlOrder
  FiniteBag FiniteCollection FiniteSet FTuples Bool3 OrderedSet.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax FormalSQL.WitnessFacts
  FormalSQL.AggregateRuntimeFacts
  FormalSQL.VerificationConditions.
From LogosGenerated Require Import Schema Queries.
From FormalSQLResultSpec Require Import
  QueryResultSpecQ12 NormalizeQueryQ12 Q12EvalSpec Q12SemanticBridge.
From Stdlib Require Import String Ascii List ZArith.
Import ListNotations.
Import Tuple.
Open Scope string_scope.
Open Scope Z_scope.

Require Import Q12Catalog.
Require Import Q12ResultSpecNormalize.

Import Q12EvalSpec.

Definition catalog_sql_sha256_ok : Prop :=
  Q12Catalog.catalog_sql_sha256 =
  "4d783e0eeb8050b8d083f505a6ee8bd6aa96a382531e33108c539bbd6e2f3f42"%string.

Lemma catalog_sql_sha256_matches : catalog_sql_sha256_ok.
Proof. reflexivity. Qed.

(** * Embed helpers (observed Q12 columns; unobserved slots canonical) *)

Definition embed_string_char15 (s : string) : NullValues.value :=
  Value_string (StringValue (StringChar 15) (Some s)).

Definition embed_string_char10 (s : string) : NullValues.value :=
  Value_string (StringValue (StringChar 10) (Some s)).

Definition embed_empty_char1 : NullValues.value :=
  Value_string (StringValue (StringChar 1) (Some "O")).

Definition embed_empty_char25 : NullValues.value :=
  Value_string (StringValue (StringChar 25) (Some "")).

Definition embed_empty_varchar79 : NullValues.value :=
  Value_string (StringValue (StringVarcharN 79) (Some "")).

Definition embed_empty_varchar44 : NullValues.value :=
  Value_string (StringValue (StringVarcharN 44) (Some "")).

Definition embed_decimal0 : NullValues.value :=
  Value_numeric (Some (numeric_of_scaled 0 2)).

Definition zero_int64 : int64 := int64_from_twos_complement 0.
Definition zero_int32 : int32 := int32_from_twos_complement 0.

Definition orders_attrs : list (Tuple.attribute TNull) :=
  Attr_int32 "o_orderkey"
  :: Attr_int32 "o_custkey"
  :: Attr_string "o_orderstatus" (StringChar 1)
  :: Attr_decimal "o_totalprice" 15 2
  :: Attr_date "o_orderdate"
  :: Attr_string "o_orderpriority" (StringChar 15)
  :: Attr_string "o_clerk" (StringChar 15)
  :: Attr_int32 "o_shippriority"
  :: Attr_string "o_comment" (StringVarcharN 79)
  :: nil.

Definition lineitem_attrs : list (Tuple.attribute TNull) :=
  Attr_int32 "l_orderkey"
  :: Attr_int32 "l_partkey"
  :: Attr_int32 "l_suppkey"
  :: Attr_int32 "l_linenumber"
  :: Attr_decimal "l_quantity" 15 2
  :: Attr_decimal "l_extendedprice" 15 2
  :: Attr_decimal "l_discount" 15 2
  :: Attr_decimal "l_tax" 15 2
  :: Attr_string "l_returnflag" (StringChar 1)
  :: Attr_string "l_linestatus" (StringChar 1)
  :: Attr_date "l_shipdate"
  :: Attr_date "l_commitdate"
  :: Attr_date "l_receiptdate"
  :: Attr_string "l_shipinstruct" (StringChar 25)
  :: Attr_string "l_shipmode" (StringChar 10)
  :: Attr_string "l_comment" (StringVarcharN 44)
  :: nil.

Definition EmbedOrderRow (o : OrderRow) : tuple TNull :=
  mk_tuple_lists orders_attrs
    [ Value_int32 (Some (int32_from_twos_complement (o_orderkey o)))
    ; Value_int32 (Some zero_int32)
    ; embed_empty_char1
    ; embed_decimal0
    ; Value_date (Some 0)
    ; embed_string_char15 (o_priority o)
    ; embed_string_char15 ""
    ; Value_int32 (Some zero_int32)
    ; embed_empty_varchar79
    ].

Definition EmbedLineRow (l : LineRow) : tuple TNull :=
  mk_tuple_lists lineitem_attrs
    [ Value_int32 (Some (int32_from_twos_complement (l_orderkey l)))
    ; Value_int32 (Some zero_int32)
    ; Value_int32 (Some zero_int32)
    ; Value_int32 (Some zero_int32)
    ; embed_decimal0
    ; embed_decimal0
    ; embed_decimal0
    ; embed_decimal0
    ; Value_string (StringValue (StringChar 1) (Some "N"))
    ; Value_string (StringValue (StringChar 1) (Some "O"))
    ; Value_date (Some (l_shipdate l))
    ; Value_date (Some (l_commitdate l))
    ; Value_date (Some (l_receiptdate l))
    ; embed_empty_char25
    ; embed_string_char10 (l_shipmode l)
    ; embed_empty_varchar44
    ].

Definition EmbedDB_Q12 (orders : list OrderRow) (lines : list LineRow) : db_state :=
  witness_database Schema.generated_schema
    [ WitnessTable (Rel "nation") nil
    ; WitnessTable (Rel "supplier") nil
    ; WitnessTable (Rel "orders") (map EmbedOrderRow orders)
    ; WitnessTable (Rel "lineitem") (map EmbedLineRow lines)
    ].

Definition q12_out_attrs : list (Tuple.attribute TNull) :=
  Attr_string "__logos_output_0" (StringChar 10)
  :: Attr_int64 "__logos_output_1"
  :: Attr_int64 "__logos_output_2"
  :: nil.

Definition EmbedOutRow_Q12 (o : SpecOutRow) : tuple TNull :=
  project_row Queries.scalar_select_list_0
    [ embed_string_char10 (out_shipmode o)
    ; Value_int64 (Some (int64_from_twos_complement (out_high o)))
    ; Value_int64 (Some (int64_from_twos_complement (out_low o)))
    ].

Definition EmbedResult_Q12 (outs : list SpecOutRow) : list (tuple TNull) :=
  map EmbedOutRow_Q12 outs.

Definition value_is_null (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

Definition FormalSQL_exec_Q12 (D : db_state) (outs : list (tuple TNull)) : Prop :=
  @eval_query_expr_possible_outcome TNull relname
    (@_basesort TNull D)
    (@_instance TNull D)
    unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null
    nil
    Queries.source_query_expr
    (SqlSuccess outs).

(** * Preconditions *)

Definition UniqueOrderKeys (orders : list OrderRow) : Prop :=
  NoDup (map o_orderkey orders).

Definition Preconditions_Q12 (orders : list OrderRow) (lines : list LineRow) : Prop :=
  catalog_sql_sha256_ok /\
  UniqueOrderKeys orders.

(** * Spec group bags (CollectModes ∘ SpecView) — Qed-linked to EvalSpec *)

Definition Q12_spec_group_outs (orders : list OrderRow) (lines : list LineRow)
  : list SpecOutRow :=
  CollectModes "MAIL" "SHIP"
    (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP").

Lemma Q12_EvalSpec_eq_Sort_group_outs :
  forall orders lines,
    EvalSpec_Q12 S_Q12_catalog orders lines =
    SortByMode (Q12_spec_group_outs orders lines).
Proof.
  intros; unfold Q12_spec_group_outs; apply EvalSpec_Q12_catalog_decomp.
Qed.

(** Bag-sort + FormalSQL_exec composition discharged in [Q12GroupsCorrespond]
    ([Q12_GroupBagsPerm_holds]; [FormalSQLAuxCorrect_Q12_of_holds];
     hyp-free [NormalizeSound_Q12] under Pre + domain/priority/sum_bound packaging). *)

Theorem NormalizeSound_Q12_nil_eval :
  EvalSpec_Q12 S_Q12_catalog nil nil = nil.
Proof. apply EvalSpec_Q12_nil_orders_lines. Qed.

(** Empty Spec bags — group outs nil (Qed). Full FormalSQL empty-DB spine
    success is the hard join-spine lemma noted above; not claimed here. *)
Lemma Q12_spec_group_outs_nil :
  Q12_spec_group_outs nil nil = nil.
Proof.
  unfold Q12_spec_group_outs.
  rewrite SpecView_nil_nil, CollectModes_empty; reflexivity.
Qed.

(** Corresponds_Q12(C++, Model): TCB. Unit/differential tests support it;
    not proved here. *)
Definition Corresponds_Q12_documented_TCB : unit := tt.
