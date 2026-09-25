(* Q1 FormalSQL proof — Embed bridge + FormalSQLNoError / empty AuxCorrect.
   Fixed LLM-facing obligations live in Q1Obligations.v.
   Theorems name AuxAnswerSpec / Embed only — never Dafny twin identifiers.
   q̂_Q1 comes only from Logos-lowered Queries.v (exact Cat(Q1)). *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueNumericTypmod
  ValueString ValueInteger ValueTemporal Formula SchemaConstraints SqlOutcome
  SqlQuerySyntax SqlQuerySemantics SqlQueryFacts SqlErrorSemantics SqlOrder
  FiniteBag FiniteCollection FiniteSet FTuples Bool3 OrderedSet.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax FormalSQL.WitnessFacts
  FormalSQL.AggregateRuntimeFacts
  FormalSQL.VerificationConditions.
From FormalSQLLib Require Import QuerySpineFacade.
From LogosGenerated Require Import Schema Queries.
From JetStreamFS Require Import Q1AuxSpec.
From Stdlib Require Import String Ascii List ZArith Lia Sorted Permutation.
Import ListNotations.
Import Tuple.
Open Scope string_scope.
Open Scope Z_scope.

Require Import Q1Catalog.

Definition catalog_sql_sha256_ok : Prop :=
  Q1Catalog.catalog_sql_sha256 =
  "25c5f92b4fb8cc4d63dbf09505868cf8c25a6dc9b6bf8638668f5c51af48340d"%string.

Lemma catalog_sql_sha256_matches : catalog_sql_sha256_ok.
Proof. reflexivity. Qed.

(** * Templatized Q1 parameter

    Runtime / TPC-H hole: interval days [delta] in
    [l_shipdate <= DATE '1998-12-01' - INTERVAL '<delta>' DAY].
    Catalog SQL fills [delta = 90]; proofs are ∀ [DeltaOk_Q1 delta]. *)
Definition q1_base_date : Z := 10561.  (* DATE '1998-12-01' *)
Definition q1_catalog_delta : Z := 90.

(** AuxSpec cutoff day = base − delta (matches [shipdate <= cutoff]). *)
Definition EmbedParams_Q1 (delta : Z) : Z := q1_base_date - delta.
Definition q1_catalog_cutoff : Z := EmbedParams_Q1 q1_catalog_delta.

(** WHERE predicate at parameter [delta] — Logos shape with [CstZ (-delta)]. *)
Definition q1_filter_at (delta : Z)
  : @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname
    (PredicateDateLteTimestamp : FTuples.Tuple.predicate TNull)
    [@SExpr_Leaf TNull relname type_date (DotDate "l_shipdate")
    ; @SExpr_Call TNull relname type_timestamp
        (ScalarTimestampAdd ScalarTimestampDay)
        [@SExpr_Call TNull relname type_timestamp
           (ScalarCast ScalarCastDateToTimestamp)
           [@SExpr_Leaf TNull relname type_date (CstDate q1_base_date)]
        ; @SExpr_Leaf TNull relname type_Z (CstZ (-delta))]].

(** Q1 TFGOP AST parameters (shared with QuerySpineFacade instantiation). *)
Definition q1_table_outputs : list (attribute TNull) :=
  [ AttrInt64 "l_orderkey"
  ; AttrInt64 "l_partkey"
  ; AttrInt64 "l_suppkey"
  ; AttrInt32 "l_linenumber"
  ; AttrDecimal "l_quantity" 15 2
  ; AttrDecimal "l_extendedprice" 15 2
  ; AttrDecimal "l_discount" 15 2
  ; AttrDecimal "l_tax" 15 2
  ; AttrString "l_returnflag" (StringChar 1)
  ; AttrString "l_linestatus" (StringChar 1)
  ; AttrDate "l_shipdate"
  ; AttrDate "l_commitdate"
  ; AttrDate "l_receiptdate"
  ; AttrString "l_shipinstruct" (StringChar 25)
  ; AttrString "l_shipmode" (StringChar 10)
  ; AttrString "l_comment" (StringVarcharN 44)
  ].
Definition q1_table : relname := Rel "lineitem".
Definition q1_inner_select := Queries.scalar_select_list_2.
Definition q1_group_select := Queries.scalar_select_list_1.
Definition q1_group_keys : list (scalar_expr TNull relname ScalarResultValue) :=
  [ @SExpr_Leaf TNull relname type_string (DotString "l_returnflag" (StringChar 1))
  ; @SExpr_Leaf TNull relname type_string (DotString "l_linestatus" (StringChar 1))
  ].
Definition q1_having := Queries.scalar_expr_predicate_0.
Definition q1_order_keys : list (sort_key TNull) :=
  [ SortAscNullsLast (AttrString "l_returnflag" (StringChar 1))
  ; SortAscNullsLast (AttrString "l_linestatus" (StringChar 1))
  ].
Definition q1_outer_select := Queries.scalar_select_list_0.

(** Templatized q̂: TFGOP spine with WHERE at [delta]. *)
Definition qhat_Q1 (delta : Z) : QueryExpr :=
  @query_spine_TFGOP TNull relname
    q1_table_outputs q1_table (q1_filter_at delta) q1_inner_select
    q1_group_select q1_group_keys q1_having q1_order_keys q1_outer_select.

(** Catalog fill-in: [delta = 90] matches Logos-lowered [Queries.source_query_expr]. *)
Lemma qhat_Q1_catalog_instance :
  qhat_Q1 q1_catalog_delta = Queries.source_query_expr.
Proof.
  unfold qhat_Q1, query_spine_TFGOP, q1_filter_at, q1_catalog_delta, q1_base_date.
  unfold Queries.source_query_expr, Queries.shared_query_expr_0,
    Queries.scalar_expr_predicate_1.
  unfold q1_table_outputs, q1_table, q1_inner_select, q1_group_select,
    q1_group_keys, q1_having, q1_order_keys, q1_outer_select.
  reflexivity.
Qed.

Definition qhat_Q1_program := Queries.source_query_program.

(** Abbreviations for the catalog instance (delta = 90). *)
Definition q1_filter := q1_filter_at q1_catalog_delta.

Lemma qhat_Q1_is_TFGOP :
  qhat_Q1 q1_catalog_delta =
  @query_spine_TFGOP TNull relname
    q1_table_outputs q1_table q1_filter q1_inner_select
    q1_group_select q1_group_keys q1_having q1_order_keys q1_outer_select.
Proof.
  reflexivity.
Qed.

Definition lineitem_attrs : list (Tuple.attribute TNull) :=
  Attr_int64 "l_orderkey"
  :: Attr_int64 "l_partkey"
  :: Attr_int64 "l_suppkey"
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

Definition embed_decimal2 (raw : Z) : NullValues.value :=
  Value_numeric (Some (numeric_of_scaled raw 2)).

Definition embed_decimal_at (raw scale : Z) : NullValues.value :=
  Value_numeric (Some (numeric_of_scaled raw scale)).

(** AVG embedding: FormalSQL AggregateAverageNumericFixed on scale-2 inputs
    uses [numeric_div_at_scales] (PG select_div_scale), not scale-2 AvgRound. *)
Definition embed_avg_from_sum_scale2 (sum_coeff count : Z) : NullValues.value :=
  match numeric_div_at_scales
          (numeric_of_scaled sum_coeff 2) 2
          (numeric_of_Z count) 0 with
  | Some n => Value_numeric (Some n)
  | None => Value_numeric None
  end.

Definition embed_flag_char (code : Z) : NullValues.value :=
  let c := ascii_of_N (Z.to_N (Z.modulo code 256)) in
  Value_string (StringValue (StringChar 1) (Some (String c EmptyString))).

Definition empty_char25 : NullValues.value :=
  Value_string (StringValue (StringChar 25) (Some "")).
Definition empty_char10 : NullValues.value :=
  Value_string (StringValue (StringChar 10) (Some "")).
Definition empty_varchar44 : NullValues.value :=
  Value_string (StringValue (StringVarcharN 44) (Some "")).

Definition zero_int64 : int64 := int64_from_twos_complement 0.
Definition zero_int32 : int32 := int32_from_twos_complement 0.

Definition EmbedRow_Q1 (r : Q1AuxSpec.Row) : tuple TNull :=
  mk_tuple_lists lineitem_attrs
    [ Value_int64 (Some zero_int64)
    ; Value_int64 (Some zero_int64)
    ; Value_int64 (Some zero_int64)
    ; Value_int32 (Some zero_int32)
    ; embed_decimal2 (Q1AuxSpec.qty r)
    ; embed_decimal2 (Q1AuxSpec.price r)
    ; embed_decimal2 (Q1AuxSpec.disc r)
    ; embed_decimal2 (Q1AuxSpec.tax r)
    ; embed_flag_char (Q1AuxSpec.rf r)
    ; embed_flag_char (Q1AuxSpec.ls r)
    ; Value_date (Some (Q1AuxSpec.shipdate r))
    ; Value_date (Some (Q1AuxSpec.shipdate r))
    ; Value_date (Some (Q1AuxSpec.shipdate r))
    ; empty_char25
    ; empty_char10
    ; empty_varchar44
    ].

Definition EmbedDB_Q1 (db : list Q1AuxSpec.Row) : db_state :=
  witness_database Schema.generated_schema
    [WitnessTable (Rel "lineitem") (map EmbedRow_Q1 db)].

Definition q1_out_attrs : list (Tuple.attribute TNull) :=
  Attr_string "__logos_output_0" (StringChar 1)
  :: Attr_string "__logos_output_1" (StringChar 1)
  :: Attr_numeric "__logos_output_2"
  :: Attr_numeric "__logos_output_3"
  :: Attr_numeric "__logos_output_4"
  :: Attr_numeric "__logos_output_5"
  :: Attr_numeric "__logos_output_6"
  :: Attr_numeric "__logos_output_7"
  :: Attr_numeric "__logos_output_8"
  :: Attr_int64 "__logos_output_9"
  :: nil.

Definition embed_outrow_values (o : Q1AuxSpec.OutRow) : list NullValues.value :=
  [ embed_flag_char (Q1AuxSpec.out_rf o)
  ; embed_flag_char (Q1AuxSpec.out_ls o)
  ; embed_decimal2 (Q1AuxSpec.out_sum_qty o)
  ; embed_decimal2 (Q1AuxSpec.out_sum_base o)
  ; embed_decimal_at (Q1AuxSpec.out_sum_disc_price o) 4
  ; embed_decimal_at (Q1AuxSpec.out_sum_charge o) 6
  ; embed_avg_from_sum_scale2 (Q1AuxSpec.out_sum_qty o) (Q1AuxSpec.out_count o)
  ; embed_avg_from_sum_scale2 (Q1AuxSpec.out_sum_base o) (Q1AuxSpec.out_count o)
  ; embed_avg_from_sum_scale2 (Q1AuxSpec.out_sum_discount o) (Q1AuxSpec.out_count o)
  ; NullValues.value_int64_checked (Q1AuxSpec.out_count o)
  ].

(** Final observation row: outer-project shape (same as FormalSQL outer select). *)
Definition EmbedOutRow_Q1 (o : Q1AuxSpec.OutRow) : tuple TNull :=
  project_row Queries.scalar_select_list_0 (embed_outrow_values o).

Definition EmbedResult_Q1 (outs : list Q1AuxSpec.OutRow) : list (tuple TNull) :=
  map EmbedOutRow_Q1 outs.

(** TPC-H lineitem value domain + cardinality bounds.

    These bounds are what make the universal [FormalSQLNoError_Q1] /
    [FormalSQLAuxCorrect_Q1] statements TRUE (see A6): they keep every checked
    NUMERIC sum inside [postgres_numeric_max_integer_digits] and every checked
    int64 COUNT inside [int64_max].  All coefficients are scale-2 integers
    (i.e. the value times 100), matching the DECIMAL(15,2) embedding:

      - qty   : 0..50.00       => coefficient 0..5000
      - price : 0..10^10 .00   => coefficient 0..10^12
      - disc  : 0.00..0.10     => coefficient 0..10
      - tax   : 0.00..0.08     => coefficient 0..8
      - rf,ls : one ASCII byte => 0..255 *)
(** Finite DATE whose midnight TIMESTAMP is also in range.  Needed so FormalSQL
    [PredicateDateLteTimestamp] coincides with AuxSpec [Z.leb] on the catalog
    cutoff (TPC-H shipdates sit well inside this carrier). *)
Definition ShipdateOk_Q1 (d : Z) : Prop :=
  postgres_date_min <= d < postgres_date_end /\
  postgres_timestamp_min <= cast_date_to_timestamp d < postgres_timestamp_end /\
  date_is_neg_infinity_bool d = false /\
  date_is_pos_infinity_bool d = false.

Definition RowOk_Q1 (r : Q1AuxSpec.Row) : Prop :=
  ShipdateOk_Q1 (Q1AuxSpec.shipdate r) /\
  0 <= Q1AuxSpec.rf r < 256 /\
  0 <= Q1AuxSpec.ls r < 256 /\
  0 <= Q1AuxSpec.qty r <= 5000 /\
  0 <= Q1AuxSpec.price r <= 1000000000000 /\
  0 <= Q1AuxSpec.disc r <= 10 /\
  0 <= Q1AuxSpec.tax r <= 8.

(** * A1 — q̂_Q1 observed vs. unobserved lineitem columns.

    q̂_Q1 (= [Queries.shared_query_expr_0]) observes exactly the following
    lineitem attributes (see the [Dot ...] leaves of [scalar_select_list_1],
    [scalar_select_list_2] and [scalar_expr_predicate_1] in the generated
    [Queries.v]):

      - l_quantity      (SUM / AVG input, and $f4/$f5 factor)
      - l_extendedprice (SUM / AVG input, and $f4/$f5 factor)
      - l_discount      (AVG input, and $f4/$f5 factor)
      - l_tax           ($f5 factor)
      - l_returnflag    (GROUP BY key + output)
      - l_linestatus    (GROUP BY key + output)
      - l_shipdate      (WHERE filter: shipdate <= cutoff)

    plus the two derived projected columns

      - $f4 = l_extendedprice * (1 - l_discount)
      - $f5 = l_extendedprice * (1 - l_discount) * (1 + l_tax)

    The following lineitem columns are NEVER read by q̂_Q1:

      l_orderkey, l_partkey, l_suppkey, l_linenumber,
      l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment.

    [EmbedRow_Q1] stores canonical constants (zero integers / empty strings /
    the shipdate replicated into the unread commit/receipt slots) in exactly
    these unobserved positions.  Because the AuxSpec [Q1AuxSpec.Row] record
    carries *only* the observed attributes, the embedding cannot depend on —
    nor leak — any unobserved catalog value into q̂. *)

(** Valid template parameter: resulting cutoff day is a finite in-range DATE
    (so FormalSQL date/timestamp predicates are defined). *)
Definition DeltaOk_Q1 (delta : Z) : Prop :=
  ShipdateOk_Q1 (EmbedParams_Q1 delta).

(** Preconditions: catalog identity + any valid [delta] + TPC-H row domain.
    Not pinned to catalog [delta = 90] — theorems are ∀ [DeltaOk_Q1 delta]. *)
Definition Preconditions_Q1 (db : list Q1AuxSpec.Row) (delta : Z) : Prop :=
  catalog_sql_sha256_ok /\
  DeltaOk_Q1 delta /\
  Forall RowOk_Q1 db /\
  Z.of_nat (length db) <= int64_max.

Definition value_is_null (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

(** Membership in outcomes of templatized [qhat_Q1 delta]. *)
Definition FormalSQL_exec (D : db_state) (delta : Z)
    (outs : list (tuple TNull)) : Prop :=
  @eval_query_expr_possible_outcome TNull relname
    (@_basesort TNull D)
    (@_instance TNull D)
    unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null
    nil
    (qhat_Q1 delta)
    (SqlSuccess outs).

Lemma Q1_unobserved_fields_irrelevant :
  forall r r' : Q1AuxSpec.Row,
    Q1AuxSpec.shipdate r = Q1AuxSpec.shipdate r' ->
    Q1AuxSpec.qty r = Q1AuxSpec.qty r' ->
    Q1AuxSpec.price r = Q1AuxSpec.price r' ->
    Q1AuxSpec.disc r = Q1AuxSpec.disc r' ->
    Q1AuxSpec.tax r = Q1AuxSpec.tax r' ->
    Q1AuxSpec.rf r = Q1AuxSpec.rf r' ->
    Q1AuxSpec.ls r = Q1AuxSpec.ls r' ->
    EmbedRow_Q1 r = EmbedRow_Q1 r'.
Proof.
  intros r r' Hship Hqty Hprice Hdisc Htax Hrf Hls.
  unfold EmbedRow_Q1.
  rewrite Hship, Hqty, Hprice, Hdisc, Htax, Hrf, Hls.
  reflexivity.
Qed.

Lemma AuxAnswerSpec_nil :
  forall theta, Q1AuxSpec.AuxAnswerSpec nil theta = nil.
Proof. intros; reflexivity. Qed.

Lemma EmbedResult_nil : EmbedResult_Q1 nil = nil.
Proof. reflexivity. Qed.

(** * ORDER BY operator bridge (spec side, real Qed).

    [Q1AuxSpec.AuxAnswerSpec] now finalizes the grouped answer with
    [Q1AuxSpec.OutRowSort], an insertion sort by [(out_rf, out_ls)].  FormalSQL
    lowers the Q1 ORDER BY to
    [SortAscNullsLast l_returnflag; SortAscNullsLast l_linestatus] on the
    one-char [l_returnflag]/[l_linestatus] output columns (see
    [Queries.shared_query_expr_0]); ascending string order on a single character
    is ascending order on its [Z] ASCII code, i.e. on [out_rf]/[out_ls].

    We prove two facts with real [Qed]s: the finalized answer is [Sorted] in
    exactly that lexicographic order, and it is a [Permutation] of the unsorted
    group finalization (so [OutRowSort] is a bag-preserving reordering, matching
    the [order_by_rows] "same rows as bag + ordered" contract).  These are the
    spec-side obligations of the FormalSQL ORDER BY step. *)
Definition OutRle (a b : Q1AuxSpec.OutRow) : Prop :=
  Q1AuxSpec.OutRowLe a b = true.

Lemma OutRowLe_total :
  forall a b, Q1AuxSpec.OutRowLe a b = true \/ Q1AuxSpec.OutRowLe b a = true.
Proof.
  intros a b. unfold Q1AuxSpec.OutRowLe.
  destruct (Z.eqb (Q1AuxSpec.out_rf a) (Q1AuxSpec.out_rf b)) eqn:Eab;
  destruct (Z.eqb (Q1AuxSpec.out_rf b) (Q1AuxSpec.out_rf a)) eqn:Eba.
  - destruct (Z.leb (Q1AuxSpec.out_ls a) (Q1AuxSpec.out_ls b)) eqn:E1.
    + left; reflexivity.
    + right. apply Z.leb_gt in E1. apply Z.leb_le. lia.
  - apply Z.eqb_eq in Eab. apply Z.eqb_neq in Eba. lia.
  - apply Z.eqb_neq in Eab. apply Z.eqb_eq in Eba. lia.
  - destruct (Z.leb (Q1AuxSpec.out_rf a) (Q1AuxSpec.out_rf b)) eqn:E1.
    + left; reflexivity.
    + right. apply Z.leb_gt in E1. apply Z.leb_le. lia.
Qed.

Lemma HdRel_OutRowInsert :
  forall a x l,
    OutRle a x -> HdRel OutRle a l -> HdRel OutRle a (Q1AuxSpec.OutRowInsert x l).
Proof.
  intros a x l Hax Hal.
  destruct l as [|y ys]; simpl.
  - constructor. exact Hax.
  - destruct (Q1AuxSpec.OutRowLe x y).
    + constructor. exact Hax.
    + constructor. inversion Hal; subst. assumption.
Qed.

Lemma OutRowInsert_sorted :
  forall x l, Sorted OutRle l -> Sorted OutRle (Q1AuxSpec.OutRowInsert x l).
Proof.
  intros x l; induction l as [|y ys IH]; intro Hs; simpl.
  - repeat constructor.
  - destruct (Q1AuxSpec.OutRowLe x y) eqn:E.
    + constructor; [ exact Hs | constructor; exact E ].
    + inversion Hs; subst. constructor.
      * apply IH; assumption.
      * apply HdRel_OutRowInsert; [ | assumption ].
        unfold OutRle.
        destruct (OutRowLe_total x y) as [H|H]; [ rewrite E in H; discriminate | exact H ].
Qed.

Lemma OutRowSort_sorted :
  forall l, Sorted OutRle (Q1AuxSpec.OutRowSort l).
Proof.
  induction l as [|x xs IH]; simpl.
  - constructor.
  - apply OutRowInsert_sorted; assumption.
Qed.

Lemma OutRowInsert_perm :
  forall x l, Permutation (x :: l) (Q1AuxSpec.OutRowInsert x l).
Proof.
  intros x l; induction l as [|y ys IH]; simpl.
  - apply Permutation_refl.
  - destruct (Q1AuxSpec.OutRowLe x y).
    + apply Permutation_refl.
    + apply perm_trans with (y :: x :: ys).
      * apply (perm_swap y x ys).
      * apply perm_skip. apply IH.
Qed.

Lemma OutRowSort_perm :
  forall l, Permutation l (Q1AuxSpec.OutRowSort l).
Proof.
  induction l as [|x xs IH]; simpl.
  - apply Permutation_refl.
  - apply perm_trans with (x :: Q1AuxSpec.OutRowSort xs).
    + apply perm_skip. exact IH.
    + apply OutRowInsert_perm.
Qed.

(** The finalized Q1 answer is sorted ascending by [(out_rf, out_ls)] — exactly
    the FormalSQL ORDER BY key order. *)
Lemma AuxAnswerSpec_sorted :
  forall DB theta, Sorted OutRle (Q1AuxSpec.AuxAnswerSpec DB theta).
Proof.
  intros DB theta. unfold Q1AuxSpec.AuxAnswerSpec. apply OutRowSort_sorted.
Qed.

(** ORDER BY is a bag-preserving reordering: the sorted answer is a permutation
    of the unsorted group finalization. *)
Lemma AuxAnswerSpec_perm :
  forall DB theta,
    Permutation
      (Q1AuxSpec.SpecFinalize (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec DB) theta))
      (Q1AuxSpec.AuxAnswerSpec DB theta).
Proof.
  intros DB theta. unfold Q1AuxSpec.AuxAnswerSpec. apply OutRowSort_perm.
Qed.

(** * A3 — Empty lineitem database via QuerySpineFacade.

    Constructor composition for Table→Filter→Project→Group→OrderBy→Project
    is [query_spine_TFGOP_success] (FormalSQLLib).  This file only discharges
    the empty intermediate leaves. *)

Definition q1_boolean_schedule : boolean_site -> boolean_evaluation_order :=
  fun _ => BooleanLeftFirst.

(** The embedded empty database has an empty concrete lineitem instance. *)
Lemma embed_nil_instance_lineitem :
  @_instance TNull (EmbedDB_Q1 nil) (Rel "lineitem")
  = Febag.mk_bag (Fecol.CBag (CTuple TNull)) nil.
Proof. reflexivity. Qed.

(** Both branches of the table denotation collapse to the empty bag, so the
    empty row list is a legal exact observation of the scan. *)
Lemma embed_nil_table_same_rows :
  forall outputs,
    query_same_rows_as_bag (T:=TNull) nil
      (@query_table_bag TNull relname
         (@_basesort TNull (EmbedDB_Q1 nil))
         (@_instance TNull (EmbedDB_Q1 nil))
         outputs (Rel "lineitem")).
Proof.
  intro outputs.
  unfold query_same_rows_as_bag.
  rewrite Febag.nb_occ_equal; intro x.
  unfold query_table_bag.
  rewrite embed_nil_instance_lineitem.
  destruct (@query_outputs_sort TNull outputs =S?=
            @_basesort TNull (EmbedDB_Q1 nil) (Rel "lineitem"));
    rewrite ?Febag.nb_occ_mk_bag, ?Febag.nb_occ_empty; cbn; reflexivity.
Qed.

Lemma q1_empty_filter_rows :
  forall sched delta,
    @eval_filter_rows_outcome TNull relname
      (@_basesort TNull (EmbedDB_Q1 nil)) (@_instance TNull (EmbedDB_Q1 nil))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      nil (q1_filter_at delta) nil (SqlSuccess nil).
Proof. intros; apply EFilterRows_Nil. Qed.

Lemma q1_empty_inner_project_rows :
  forall sched,
    @eval_project_rows_outcome TNull relname
      (@_basesort TNull (EmbedDB_Q1 nil)) (@_instance TNull (EmbedDB_Q1 nil))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      nil q1_inner_select nil (SqlSuccess nil).
Proof. intros; apply EProjectRows_Nil. Qed.

Lemma q1_empty_group_bag :
  forall sched,
    @eval_group_bag_outcome TNull relname
      (@_basesort TNull (EmbedDB_Q1 nil)) (@_instance TNull (EmbedDB_Q1 nil))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      nil q1_group_select q1_group_keys q1_having
      (query_rows_bag (@nil (tuple TNull))) (SqlSuccess (query_rows_bag nil)).
Proof.
  intros sched.
  eapply EGroupBag_Success.
  - reflexivity.
  - apply Febag.equal_refl.
  - reflexivity.
  - rewrite query_make_groups_empty_shape; cbn; apply EGroups_Nil.
  - apply Febag.equal_refl.
Qed.

Lemma q1_empty_order_rows :
  @order_by_rows TNull value_is_null q1_order_keys nil nil.
Proof. split; [apply Febag.equal_refl | exact I]. Qed.

Lemma q1_empty_outer_project_rows :
  forall sched,
    @eval_project_rows_outcome TNull relname
      (@_basesort TNull (EmbedDB_Q1 nil)) (@_instance TNull (EmbedDB_Q1 nil))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      nil q1_outer_select nil (SqlSuccess nil).
Proof. intros; apply EProjectRows_Nil. Qed.

(** Empty lineitem: templatized q̂ succeeds with nil for every [delta]. *)
Lemma FormalSQL_empty_db_success_nil :
  forall delta,
    FormalSQL_exec (EmbedDB_Q1 nil) delta nil.
Proof.
  intro delta.
  unfold FormalSQL_exec, qhat_Q1.
  exists q1_boolean_schedule.
  eapply (@query_spine_TFGOP_success TNull relname
    (@_basesort TNull (EmbedDB_Q1 nil))
    (@_instance TNull (EmbedDB_Q1 nil))
    unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null
    q1_boolean_schedule).
  - apply embed_nil_table_same_rows.
  - apply q1_empty_filter_rows.
  - apply q1_empty_inner_project_rows.
  - apply q1_empty_group_bag.
  - apply Febag.equal_refl.
  - apply q1_empty_order_rows.
  - apply q1_empty_outer_project_rows.
Qed.

(** Empty-DB AuxCorrect for any valid [delta]. *)
Lemma FormalSQLAuxCorrect_Q1_nil :
  forall delta,
    Preconditions_Q1 nil delta ->
    FormalSQL_exec (EmbedDB_Q1 nil) delta
      (EmbedResult_Q1 (Q1AuxSpec.AuxAnswerSpec nil (EmbedParams_Q1 delta))).
Proof.
  intros delta Hpre.
  rewrite AuxAnswerSpec_nil, EmbedResult_nil.
  apply FormalSQL_empty_db_success_nil.
Qed.

(** [FormalSQL_outcome D delta]: possible outcomes of [qhat_Q1 delta]. *)
Definition FormalSQL_outcome (D : db_state) (delta : Z)
    (o : sql_outcome (list (tuple TNull))) : Prop :=
  @eval_query_expr_possible_outcome TNull relname
    (@_basesort TNull D)
    (@_instance TNull D)
    unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null
    nil
    (qhat_Q1 delta)
    o.

(** A cons row-list can never be bag-equal to the empty row list: its head
    occurs at least once. *)
Lemma rows_bag_cons_not_nil_bag :
  forall (r : tuple TNull) rs,
    query_rows_bag (r :: rs) =BE= query_rows_bag (@nil (tuple TNull)) -> False.
Proof.
  intros r rs H.
  rewrite Febag.nb_occ_equal in H.
  specialize (H r).
  unfold query_rows_bag in H.
  rewrite !Febag.nb_occ_mk_bag in H.
  cbn [Oeset.nb_occ] in H.
  rewrite Oeset.compare_eq_refl in H.
  destruct (Oeset.nb_occ _ r rs); cbn in H; discriminate.
Qed.

(** Consequently a row list bag-equal to the empty bag is syntactically nil. *)
Lemma bag_forces_nil :
  forall l : list (tuple TNull),
    query_rows_bag l =BE= query_rows_bag (@nil (tuple TNull)) -> l = nil.
Proof.
  intros [|r rs] H; [reflexivity|].
  exfalso; eapply rows_bag_cons_not_nil_bag; exact H.
Qed.

Local Notation EQ sched q o :=
  (@eval_query_expr_outcome TNull relname
     (@_basesort TNull (EmbedDB_Q1 nil)) (@_instance TNull (EmbedDB_Q1 nil))
     unknown3 NullValues.interp_scalar_operator_runtime_error
     NullValues.interp_aggregate_runtime_error value_is_null sched
     nil q o).

Local Notation EGB sched sl gk hv ib o :=
  (@eval_group_bag_outcome TNull relname
     (@_basesort TNull (EmbedDB_Q1 nil)) (@_instance TNull (EmbedDB_Q1 nil))
     unknown3 NullValues.interp_scalar_operator_runtime_error
     NullValues.interp_aggregate_runtime_error value_is_null sched
     nil sl gk hv ib o).

(** GROUP BY over the empty input bag cannot raise a runtime error: there are
    no rows to trigger a key-evaluation error, and (for a nonempty key list)
    grouping produces no groups, so the per-group aggregate/HAVING pipeline is
    never entered.  Stated with an explicit key term list so no substitution of
    the enclosing query AST can rename it away. *)
Lemma group_bag_empty_no_error :
  forall sched sl gk hv gt err,
    scalar_group_key_terms gk = Some gt ->
    gt <> nil ->
    EGB sched sl gk hv (query_rows_bag nil) (SqlError err) ->
    False.
Proof.
  intros sched sl gk hv gt err Hkeys Hne Hgb.
  (* [inversion; subst] may substitute the key-term variable, so we never refer
     to it by name: we use [Hkeys] (which follows the substitution) to pin the
     grouped key terms to [gt], and force the representative row list to nil. *)
  inversion Hgb; subst.
  all: match goal with
       | Hrep : query_same_rows_as_bag ?rep (query_rows_bag nil) |- _ =>
           unfold query_same_rows_as_bag in Hrep;
           apply bag_forces_nil in Hrep; subst rep
       end.
  (* Close whichever error branch (KeyError / ProcessError) inversion left in
     either order: empty input has no key runtime error and no groups. *)
  all: first
      [ (* KeyError: a key runtime error on the empty row list is None. *)
        match goal with
        | Hgke : group_keys_runtime_error _ _ _ _ _ = Some _ |- _ =>
            cbn in Hgke; discriminate Hgke
        end
      | (* ProcessError: nonempty keys over empty input yield no groups. *)
        match goal with
        | Hk : scalar_group_key_terms _ = Some ?g1,
          Hgroups : context[eval_groups_outcome] |- _ =>
            rewrite Hkeys in Hk; injection Hk as Hk; subst g1;
            rewrite query_make_groups_empty_shape in Hgroups;
            destruct gt as [|g gt']; [contradiction|];
            cbn in Hgroups; inversion Hgroups
        end ].
Qed.

(** GROUP BY over the empty input bag succeeds with a bag that is bag-equal to
    the empty bag (no groups are produced for a nonempty key list). *)
Lemma group_bag_empty_success_bag :
  forall sched sl gk hv gt ob,
    scalar_group_key_terms gk = Some gt ->
    gt <> nil ->
    EGB sched sl gk hv (query_rows_bag nil) (SqlSuccess ob) ->
    query_rows_bag nil =BE= ob.
Proof.
  intros sched sl gk hv gt ob Hkeys Hne Hgb.
  inversion Hgb; subst.
  match goal with
  | Hk : scalar_group_key_terms gk = Some ?g1 |- _ =>
      rewrite Hkeys in Hk; injection Hk as Hk; subst g1
  end.
  match goal with
  | Hrep : query_same_rows_as_bag ?rep (query_rows_bag nil) |- _ =>
      unfold query_same_rows_as_bag in Hrep;
      apply bag_forces_nil in Hrep; subst rep
  end.
  match goal with
  | Hgroups : context[eval_groups_outcome] |- _ =>
      rewrite query_make_groups_empty_shape in Hgroups;
      destruct gt as [|g gt']; [contradiction|];
      inversion Hgroups; subst
  end.
  match goal with
  | Hout : query_same_rows_as_bag ?gr ob |- _ =>
      unfold query_same_rows_as_bag in Hout; exact Hout
  end.
Qed.

(** Empty table scan yields only the empty observation. *)
Lemma empty_table_only_nil :
  forall sched outs o,
    EQ sched (QExpr_Table outs (Rel "lineitem")) o ->
    o = SqlSuccess nil.
Proof.
  intros sched outs o Ho.
  inversion Ho; subst.
  f_equal.
  apply bag_forces_nil.
  match goal with
  | Hsame : query_same_rows_as_bag ?rows _ |- _ =>
      unfold query_same_rows_as_bag in Hsame;
      eapply Febag.equal_trans; [exact Hsame|]
  end.
  apply Febag.equal_sym.
  exact (embed_nil_table_same_rows outs).
Qed.

(** Filter over a child that only yields nil yields only nil. *)
Lemma filter_preserves_only_nil :
  forall sched f child,
    (forall o, EQ sched child o -> o = SqlSuccess nil) ->
    forall o, EQ sched (QExpr_Filter f child) o -> o = SqlSuccess nil.
Proof.
  intros sched f child IH o Ho.
  inversion Ho; subst; clear Ho;
    match goal with
    | Hc : context[child] |- _ => apply IH in Hc; rename Hc into Hchild
    end.
  - discriminate Hchild.
  - injection Hchild as Hchild; subst;
    match goal with
    | Hf : context[eval_filter_rows_outcome] |- _ =>
        inversion Hf; subst; reflexivity
    end.
Qed.

(** Project over a child that only yields nil yields only nil. *)
Lemma project_preserves_only_nil :
  forall sched sl child,
    (forall o, EQ sched child o -> o = SqlSuccess nil) ->
    forall o, EQ sched (QExpr_Project sl child) o -> o = SqlSuccess nil.
Proof.
  intros sched sl child IH o Ho.
  inversion Ho; subst; clear Ho;
    match goal with
    | Hc : context[child] |- _ => apply IH in Hc; rename Hc into Hchild
    end.
  - discriminate Hchild.
  - injection Hchild as Hchild; subst;
    match goal with
    | Hp : context[eval_project_rows_outcome] |- _ =>
        inversion Hp; subst; reflexivity
    end.
Qed.

(** OrderBy over a child that only yields nil yields only nil. *)
Lemma orderby_preserves_only_nil :
  forall sched keys child,
    (forall o, EQ sched child o -> o = SqlSuccess nil) ->
    forall o, EQ sched (QExpr_OrderBy keys child) o -> o = SqlSuccess nil.
Proof.
  intros sched keys child IH o Ho.
  inversion Ho; subst; clear Ho;
    match goal with
    | Hc : context[child] |- _ => apply IH in Hc; rename Hc into Hchild
    end.
  - discriminate Hchild.
  - injection Hchild as Hchild; subst.
    match goal with
    | Horder : order_by_rows _ _ _ _ |- _ =>
        destruct Horder as [Hsame _]
    end.
    f_equal.
    apply bag_forces_nil.
    unfold query_same_rows_as_bag in Hsame.
    exact Hsame.
Qed.

(** Keyed Group (nonempty key list) over a child that only yields nil yields
    only nil: the empty input produces no groups. *)
Lemma group_preserves_only_nil :
  forall sched sl gkeys having child gt,
    scalar_group_key_terms gkeys = Some gt ->
    gt <> nil ->
    (forall o, EQ sched child o -> o = SqlSuccess nil) ->
    forall o, EQ sched (QExpr_Group sl gkeys having child) o ->
      o = SqlSuccess nil.
Proof.
  intros sched sl gkeys having child gt Hkeys Hne IH o Ho.
  inversion Ho; subst; clear Ho;
    match goal with
    | Hc : context[child] |- _ => apply IH in Hc; rename Hc into Hchild
    end.
  - (* child error *)
    discriminate Hchild.
  - (* group-bag error over the empty input bag: impossible *)
    injection Hchild as Hchild; subst.
    exfalso.
    match goal with
    | Hgb : context[eval_group_bag_outcome] |- _ =>
        eapply group_bag_empty_no_error; [ exact Hkeys | exact Hne | exact Hgb ]
    end.
  - (* group-bag success over the empty input bag *)
    injection Hchild as Hchild; subst.
    match goal with
    | Hgb : context[eval_group_bag_outcome] |- _ =>
        pose proof (group_bag_empty_success_bag _ _ _ _ _ _ Hkeys Hne Hgb) as Hbe
    end.
    f_equal.
    apply bag_forces_nil.
    match goal with
    | Hout : query_same_rows_as_bag ?output ?ob,
      Hbe : query_rows_bag nil =BE= ?ob |- _ =>
        unfold query_same_rows_as_bag in Hout;
        eapply Febag.equal_trans; [ exact Hout | apply Febag.equal_sym; exact Hbe ]
    end.
Qed.

Theorem FormalSQLNoError_Q1_nil :
  forall (delta : Z) (o : sql_outcome (list (tuple TNull))),
    Preconditions_Q1 nil delta ->
    FormalSQL_outcome (EmbedDB_Q1 nil) delta o ->
    exists rows, o = SqlSuccess rows.
Proof.
  intros delta o _ [sched Heval].
  unfold qhat_Q1, query_spine_TFGOP in Heval.
  exists nil.
  eapply project_preserves_only_nil with (sched := sched); [| exact Heval].
  intros o1 H1.
  eapply orderby_preserves_only_nil with (sched := sched); [| exact H1].
  intros o2 H2.
  eapply group_preserves_only_nil with (sched := sched).
  4: exact H2.
  1: reflexivity.
  1: discriminate.
  intros o3 H3.
  eapply project_preserves_only_nil with (sched := sched); [| exact H3].
  intros o4 H4.
  eapply filter_preserves_only_nil with (sched := sched); [| exact H4].
  intros o5 H5.
  eapply empty_table_only_nil; exact H5.
Qed.

(** [Q1NumericSafe]: for every valid-domain [delta], every FormalSQL outcome of
    [qhat_Q1 delta] is [SqlSuccess] (overflow only; see STATUS.md TCB). *)
Definition Q1NumericSafe (db : list Q1AuxSpec.Row) : Prop :=
  forall (delta : Z) (o : sql_outcome (list (tuple TNull))),
    FormalSQL_outcome (EmbedDB_Q1 db) delta o ->
    exists rows, o = SqlSuccess rows.

Theorem FormalSQLNoError_Q1 :
  forall (db : list Q1AuxSpec.Row) (delta : Z)
         (o : sql_outcome (list (tuple TNull))),
    Preconditions_Q1 db delta ->
    Q1NumericSafe db ->
    FormalSQL_outcome (EmbedDB_Q1 db) delta o ->
    exists rows, o = SqlSuccess rows.
Proof.
  intros db delta o _ Hsafe Hout.
  exact (Hsafe delta o Hout).
Qed.

(** EmbedResult AVG uses [numeric_div_at_scales], not Twin [AvgRound]. *)
Lemma Q1_avg_output_scale_is_20 :
  numeric_pg_div_scale (numeric_of_scaled 1 2) 2 (numeric_of_Z 2) 0 = Some 20.
Proof. vm_compute. reflexivity. Qed.

Lemma Q1_avg_formalsql_vs_avground_mismatch :
  NullValues.interp_avg_numeric_fixed 15 2
    (Value_numeric (Some (numeric_of_scaled 0 2))
     :: Value_numeric (Some (numeric_of_scaled 1 2)) :: nil)
  <> Value_numeric (Some (numeric_of_scaled (Q1AuxSpec.AvgRound 1 2) 2)).
Proof.
  intro H.
  apply (f_equal (fun v =>
    match v with
    | NullValues.Value_numeric (Some (NumericFinite q)) =>
        QArith_base.Qden (Qcanon.this q)
    | _ => 1%positive
    end)) in H.
  vm_compute in H.
  discriminate H.
Qed.

(** COUNT-star overflows at [int64_max+1] (witness that unbounded Pre is unsafe). *)
Lemma Q1_count_star_overflow_errors :
  forall values,
    NullValues.row_count values = int64_max + 1 ->
    NullValues.count_runtime_error values <> None.
Proof.
  intros values Hlen Hnone.
  apply count_runtime_error_none_iff in Hnone.
  destruct Hnone as [_ Hhi].
  rewrite Hlen in Hhi.
  revert Hhi. unfold int64_max. lia.
Qed.
