(* Q12WhereAcceptance — WHERE predicate_1 acceptance on join tuples.

   Per-conjunct exactness + Spec keep correspondence under UniqueOrderKeys.
   Consumed by Q12JoinSpine FilterOk for scalar_expr_predicate_1. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueInteger
  ValueTemporal ValueString FiniteSet FTuples OrderedSet FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts Interp ATerms FTerms Formula ValuePredicates.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax
  FormalSQL.GroupedFilterOutcomeFacts FormalSQL.StringTemporalFacts
  FormalSQL.ScalarPredicateFacts FormalSQL.QueryCardinality.
From FormalSQLLib Require Import
  FilterAcceptanceFacade TemporalPredAcceptanceFacade NumericCallExactFacade.
From FormalSQLResultSpec Require Import
  QueryResultSpecQ12 Q12EvalSpec Q12SemanticBridge.
From LogosGenerated Require Import Schema Queries.
From Stdlib Require Import String List ZArith Bool Lia.
Import ListNotations.
Import Tuple.
Open Scope string_scope.
Open Scope Z_scope.

Require Import Q12Catalog.
Require Import Q12NormalizeSound.
Require Import Q12EmbedDots.

Import Q12EvalSpec.

Definition q12_value_is_null (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

(** Spec keep on a (order, line) pair — matches ApplyLine's guards + join key. *)
Definition SpecPairKeep (o : OrderRow) (l : LineRow) : bool :=
  andb (Z.eqb (o_orderkey o) (l_orderkey l))
    (andb (LineQualifies l)
      (andb (InWindow (l_receiptdate l) q12_catalog_start q12_catalog_end)
        (ModeAccepted (l_shipmode l) "MAIL" "SHIP"))).

Lemma SpecPairKeep_iff :
  forall o l,
    SpecPairKeep o l = true <->
    o_orderkey o = l_orderkey l /\
    LineQualifies l = true /\
    InWindow (l_receiptdate l) q12_catalog_start q12_catalog_end = true /\
    ModeAccepted (l_shipmode l) "MAIL" "SHIP" = true.
Proof.
  intros o l; unfold SpecPairKeep; split.
  - intro H.
    apply andb_true_iff in H; destruct H as [Hk H1].
    apply andb_true_iff in H1; destruct H1 as [Hq H2].
    apply andb_true_iff in H2; destruct H2 as [Hw Hm].
    apply Z.eqb_eq in Hk; repeat split; assumption.
  - intros [Hk [Hq [Hw Hm]]].
    apply andb_true_iff; split; [apply Z.eqb_eq; exact Hk|].
    apply andb_true_iff; split; [exact Hq|].
    apply andb_true_iff; split; [exact Hw|exact Hm].
Qed.

(** * join_tuple preserves disjoint-side dots *)

(** * join_tuple preserves disjoint-side dots (reuse FTuples) *)

Lemma join_tuple_dot_left :
  forall (t1 t2 : tuple TNull) attr,
    attr inS labels TNull t1 ->
    dot TNull (join_tuple TNull t1 t2) attr = dot TNull t1 attr.
Proof. intros; apply dot_join_tuple_1; assumption. Qed.

Lemma join_tuple_dot_right :
  forall (t1 t2 : tuple TNull) attr,
    (attr inS? labels TNull t1) = false ->
    attr inS labels TNull t2 ->
    dot TNull (join_tuple TNull t1 t2) attr = dot TNull t2 attr.
Proof. intros; apply dot_join_tuple_2; assumption. Qed.

Lemma orders_lineitem_attrs_disjoint_orderkey :
  AttrInt32 "l_orderkey" inS? Fset.mk_set (A TNull) orders_attrs = false.
Proof. vm_compute; reflexivity. Qed.

Lemma EmbedJoin_dot_o_orderkey :
  forall o l,
    dot TNull (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l))
      (AttrInt32 "o_orderkey") =
    Value_int32 (Some (int32_from_twos_complement (o_orderkey o))).
Proof.
  intros o l.
  rewrite join_tuple_dot_left by
    (apply EmbedOrderRow_attr_inS; exact orders_attr_inS_orderkey).
  apply EmbedOrderRow_dot_orderkey.
Qed.

Lemma EmbedJoin_dot_l_orderkey :
  forall o l,
    dot TNull (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l))
      (AttrInt32 "l_orderkey") =
    Value_int32 (Some (int32_from_twos_complement (l_orderkey l))).
Proof.
  intros o l.
  rewrite join_tuple_dot_right.
  - apply EmbedLineRow_dot_orderkey.
  - change (AttrInt32 "l_orderkey" inS?
              labels TNull (EmbedOrderRow o) = false).
    unfold EmbedOrderRow.
    match goal with
    | |- context [mk_tuple_lists ?atts ?vals] =>
        rewrite (Fset.mem_eq_2 _ _ _ (mk_tuple_lists_labels atts vals))
    end.
    exact orders_lineitem_attrs_disjoint_orderkey.
  - apply EmbedLineRow_attr_inS; exact lineitem_attr_inS_orderkey.
Qed.

Lemma EmbedJoin_dot_l_shipmode :
  forall o l,
    dot TNull (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l))
      (AttrString "l_shipmode" (StringChar 10)) =
    embed_string_char10 (l_shipmode l).
Proof.
  intros o l.
  assert (Hdisj :
    AttrString "l_shipmode" (StringChar 10) inS?
      Fset.mk_set (A TNull) orders_attrs = false).
  { vm_compute; reflexivity. }
  rewrite join_tuple_dot_right.
  - apply EmbedLineRow_dot_shipmode.
  - unfold EmbedOrderRow.
    match goal with
    | |- context [mk_tuple_lists ?atts ?vals] =>
        rewrite (Fset.mem_eq_2 _ _ _ (mk_tuple_lists_labels atts vals))
    end.
    exact Hdisj.
  - apply EmbedLineRow_attr_inS; exact lineitem_attr_inS_shipmode.
Qed.

Lemma EmbedJoin_dot_l_commitdate :
  forall o l,
    dot TNull (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l))
      (AttrDate "l_commitdate") =
    Value_date (Some (l_commitdate l)).
Proof.
  intros o l.
  assert (Hdisj :
    AttrDate "l_commitdate" inS?
      Fset.mk_set (A TNull) orders_attrs = false).
  { vm_compute; reflexivity. }
  rewrite join_tuple_dot_right.
  - apply EmbedLineRow_dot_commitdate.
  - unfold EmbedOrderRow.
    match goal with
    | |- context [mk_tuple_lists ?atts ?vals] =>
        rewrite (Fset.mem_eq_2 _ _ _ (mk_tuple_lists_labels atts vals))
    end.
    exact Hdisj.
  - apply EmbedLineRow_attr_inS; exact lineitem_attr_inS_commitdate.
Qed.

Lemma EmbedJoin_dot_l_receiptdate :
  forall o l,
    dot TNull (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l))
      (AttrDate "l_receiptdate") =
    Value_date (Some (l_receiptdate l)).
Proof.
  intros o l.
  assert (Hdisj :
    AttrDate "l_receiptdate" inS?
      Fset.mk_set (A TNull) orders_attrs = false).
  { vm_compute; reflexivity. }
  rewrite join_tuple_dot_right.
  - apply EmbedLineRow_dot_receiptdate.
  - unfold EmbedOrderRow.
    match goal with
    | |- context [mk_tuple_lists ?atts ?vals] =>
        rewrite (Fset.mem_eq_2 _ _ _ (mk_tuple_lists_labels atts vals))
    end.
    exact Hdisj.
  - apply EmbedLineRow_attr_inS; exact lineitem_attr_inS_receiptdate.
Qed.

Lemma EmbedJoin_dot_l_shipdate :
  forall o l,
    dot TNull (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l))
      (AttrDate "l_shipdate") =
    Value_date (Some (l_shipdate l)).
Proof.
  intros o l.
  assert (Hdisj :
    AttrDate "l_shipdate" inS?
      Fset.mk_set (A TNull) orders_attrs = false).
  { vm_compute; reflexivity. }
  rewrite join_tuple_dot_right.
  - apply EmbedLineRow_dot_shipdate.
  - unfold EmbedOrderRow.
    match goal with
    | |- context [mk_tuple_lists ?atts ?vals] =>
        rewrite (Fset.mem_eq_2 _ _ _ (mk_tuple_lists_labels atts vals))
    end.
    exact Hdisj.
  - apply EmbedLineRow_attr_inS; exact lineitem_attr_inS_shipdate.
Qed.

Lemma EmbedJoin_dot_o_priority :
  forall o l,
    dot TNull (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l))
      (AttrString "o_orderpriority" (StringChar 15)) =
    embed_string_char15 (o_priority o).
Proof.
  intros o l.
  rewrite join_tuple_dot_left by
    (apply EmbedOrderRow_attr_inS; exact orders_attr_inS_priority).
  apply EmbedOrderRow_dot_priority.
Qed.

(** * Domain hypotheses for Spec ↔ FormalSQL acceptance *)

Definition q12_orderkey_embed_ok (k : Z) : Prop :=
  int32_value (int32_from_twos_complement k) = k.

Definition q12_shipmode_embed_ok (mode : string) : Prop :=
  string_rtrim_spaces mode = mode.

Definition q12_pair_domain_ok (o : OrderRow) (l : LineRow) : Prop :=
  q12_orderkey_embed_ok (o_orderkey o) /\
  q12_orderkey_embed_ok (l_orderkey l) /\
  q12_shipmode_embed_ok (l_shipmode l) /\
  finite_date_timestamp_ok (l_commitdate l) /\
  finite_date_timestamp_ok (l_receiptdate l) /\
  finite_date_timestamp_ok (l_shipdate l).

Lemma q12_catalog_start_finite : finite_date_timestamp_ok q12_catalog_start.
Proof.
  unfold q12_catalog_start, finite_date_timestamp_ok, cast_date_to_timestamp.
  unfold postgres_date_min, postgres_date_end,
    postgres_timestamp_min, postgres_timestamp_end,
    date_is_neg_infinity_bool, date_is_pos_infinity_bool,
    postgres_date_neg_infinity, postgres_date_pos_infinity,
    micros_per_day, micros_per_hour, micros_per_minute, micros_per_second.
  repeat split; lia.
Qed.

Lemma q12_catalog_end_finite : finite_date_timestamp_ok q12_catalog_end.
Proof.
  unfold q12_catalog_end, finite_date_timestamp_ok, cast_date_to_timestamp.
  unfold postgres_date_min, postgres_date_end,
    postgres_timestamp_min, postgres_timestamp_end,
    date_is_neg_infinity_bool, date_is_pos_infinity_bool,
    postgres_date_neg_infinity, postgres_date_pos_infinity,
    micros_per_day, micros_per_hour, micros_per_minute, micros_per_second.
  repeat split; lia.
Qed.

Lemma mail_bpchar_cast_id :
  string_explicit_cast StringBpchar "MAIL" = "MAIL".
Proof. vm_compute. reflexivity. Qed.

Lemma ship_bpchar_cast_id :
  string_explicit_cast StringBpchar "SHIP" = "SHIP".
Proof. vm_compute. reflexivity. Qed.

(** * Join label membership *)

Lemma EmbedJoin_attr_inS_orders :
  forall o l attr,
    attr inS Fset.mk_set (A TNull) orders_attrs ->
    attr inS labels TNull
      (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)).
Proof.
  intros o l attr Hattr.
  change (attr inS?
            labels TNull (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l))
          = true).
  unfold join_tuple.
  rewrite (Fset.mem_eq_2 _ _ _ (labels_mk_tuple TNull _ _)).
  rewrite Fset.mem_union.
  apply Bool.orb_true_intro; left.
  change (attr inS? labels TNull (EmbedOrderRow o) = true).
  apply EmbedOrderRow_attr_inS; exact Hattr.
Qed.

Lemma EmbedJoin_attr_inS_lineitem :
  forall o l attr,
    attr inS Fset.mk_set (A TNull) lineitem_attrs ->
    attr inS labels TNull
      (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)).
Proof.
  intros o l attr Hattr.
  change (attr inS?
            labels TNull (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l))
          = true).
  unfold join_tuple.
  rewrite (Fset.mem_eq_2 _ _ _ (labels_mk_tuple TNull _ _)).
  rewrite Fset.mem_union.
  apply Bool.orb_true_intro; right.
  change (attr inS? labels TNull (EmbedLineRow l) = true).
  apply EmbedLineRow_attr_inS; exact Hattr.
Qed.

(** * Z.compare → bool bridges for PredicateEq/Lt/Gte *)

Lemma Z_eqb_of_compare :
  forall a b,
    Bool.is_true Bool3
      (match Z.compare a b with Eq => true3 | Lt | Gt => false3 end) =
    Z.eqb a b.
Proof.
  intros a b.
  destruct (Z.compare_spec a b) as [H|H|H].
  - subst b. rewrite Z.eqb_refl. reflexivity.
  - rewrite (proj2 (Z.eqb_neq a b) ltac:(lia)). reflexivity.
  - rewrite (proj2 (Z.eqb_neq a b) ltac:(lia)). reflexivity.
Qed.

Lemma Z_ltb_of_compare :
  forall a b,
    Bool.is_true Bool3
      (match Z.compare a b with Lt => true3 | Eq | Gt => false3 end) =
    Z.ltb a b.
Proof.
  intros a b.
  destruct (Z.compare_spec a b) as [H|H|H].
  - subst b. rewrite Z.ltb_irrefl. reflexivity.
  - rewrite (proj2 (Z.ltb_lt a b) H). reflexivity.
  - rewrite (proj2 (Z.ltb_ge a b) ltac:(lia)). reflexivity.
Qed.

Lemma Z_leb_of_gte_compare :
  forall a b,
    Bool.is_true Bool3
      (match Z.compare a b with Lt => false3 | Eq | Gt => true3 end) =
    Z.leb b a.
Proof.
  intros a b.
  destruct (Z.compare_spec a b) as [H|H|H].
  - subst b. rewrite Z.leb_refl. reflexivity.
  - rewrite (proj2 (Z.leb_gt b a) H). reflexivity.
  - rewrite (proj2 (Z.leb_le b a) ltac:(lia)). reflexivity.
Qed.

(** * Named conjunct ASTs (match Queries.scalar_expr_predicate_1) *)

Definition q12_pred_eq_orderkeys :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateEq
    [@SExpr_Leaf TNull relname type_int32 (DotInt32 "o_orderkey");
     @SExpr_Leaf TNull relname type_int32 (DotInt32 "l_orderkey")].

Definition q12_pred_mode_mail :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateEq
    [@SExpr_Leaf TNull relname type_string (DotString "l_shipmode" (StringChar 10));
     @SExpr_Leaf TNull relname type_string (CstString StringBpchar "MAIL")].

Definition q12_pred_mode_ship :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateEq
    [@SExpr_Leaf TNull relname type_string (DotString "l_shipmode" (StringChar 10));
     @SExpr_Leaf TNull relname type_string (CstString StringBpchar "SHIP")].

Definition q12_pred_mode_or :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_ConjList TNull relname ([[]; ["b17"]]) Or_F
    [q12_pred_mode_mail; q12_pred_mode_ship].

Definition q12_pred_commit_lt_receipt :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateLt
    [@SExpr_Leaf TNull relname type_date (DotDate "l_commitdate");
     @SExpr_Leaf TNull relname type_date (DotDate "l_receiptdate")].

Definition q12_pred_ship_lt_commit :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateLt
    [@SExpr_Leaf TNull relname type_date (DotDate "l_shipdate");
     @SExpr_Leaf TNull relname type_date (DotDate "l_commitdate")].

Definition q12_pred_receipt_gte_start :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateGte
    [@SExpr_Leaf TNull relname type_date (DotDate "l_receiptdate");
     @SExpr_Leaf TNull relname type_date (CstDate q12_catalog_start)].

Definition q12_pred_receipt_lt_yearend :
    @scalar_expr TNull relname ScalarResultBoolean :=
  date_lt_timestamp_add_year_pred "l_receiptdate" q12_catalog_start.

Lemma q12_predicate_1_structure :
  Queries.scalar_expr_predicate_1 =
  @SExpr_ConjList TNull relname
    ([[]; ["b2"]; ["b3"; "b4"]; ["b5"; "b6"; "b7"];
      ["b8"; "b9"; "b10"; "b11"]; ["b12"; "b13"; "b14"; "b15"; "b16"]])
    And_F
    [q12_pred_eq_orderkeys; q12_pred_mode_or;
     q12_pred_commit_lt_receipt; q12_pred_ship_lt_commit;
     q12_pred_receipt_gte_start; q12_pred_receipt_lt_yearend].
Proof. reflexivity. Qed.

(** * Per-conjunct acceptance on EmbedJoin rows *)

Lemma q12_acc_eq_orderkeys :
  forall D sched o l,
    q12_orderkey_embed_ok (o_orderkey o) ->
    q12_orderkey_embed_ok (l_orderkey l) ->
    @scalar_expr_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      q12_pred_eq_orderkeys
      (Z.eqb (o_orderkey o) (l_orderkey l)).
Proof.
  intros D sched o l Ho Hl.
  set (row := join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)).
  set (env := Env.env_t TNull nil row).
  set (vo := Value_int32 (Some (int32_from_twos_complement (o_orderkey o)))).
  set (vl := Value_int32 (Some (int32_from_twos_complement (l_orderkey l)))).
  assert (Hargs :
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
      [@SExpr_Leaf TNull relname type_int32 (DotInt32 "o_orderkey");
       @SExpr_Leaf TNull relname type_int32 (DotInt32 "l_orderkey")]
      [vo; vl]).
  {
    eapply (@scalar_value_list_exact_cons TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
    - apply tnull_dot_int32_exact.
      unfold env, row; rewrite interp_dot_env_t.
      + apply EmbedJoin_dot_o_orderkey.
      + apply EmbedJoin_attr_inS_orders; exact orders_attr_inS_orderkey.
    - eapply (@scalar_value_list_exact_cons TNull relname
        (@_basesort TNull D) (@_instance TNull D) unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
      + apply tnull_dot_int32_exact.
        unfold env, row; rewrite interp_dot_env_t.
        * apply EmbedJoin_dot_l_orderkey.
        * apply EmbedJoin_attr_inS_lineitem; exact lineitem_attr_inS_orderkey.
      + apply (@scalar_value_list_exact_nil TNull relname
          (@_basesort TNull D) (@_instance TNull D) unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
  }
  pose proof (@pred_acceptance_of_value_list TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
    PredicateEq
    [@SExpr_Leaf TNull relname type_int32 (DotInt32 "o_orderkey");
     @SExpr_Leaf TNull relname type_int32 (DotInt32 "l_orderkey")]
    [vo; vl] Hargs) as Hacc.
  unfold q12_pred_eq_orderkeys.
  refine (eq_rect _ _ Hacc _ _).
  unfold vo, vl.
  rewrite interp_predicate_int32_nonnull_equal.
  unfold q12_orderkey_embed_ok in Ho, Hl; rewrite Ho, Hl.
  apply Z_eqb_of_compare.
Qed.

Lemma date_lt_acceptance_ltb :
  forall a b,
    Bool.is_true Bool3
      (NullPredicates.interp_predicate PredicateLt
        (NullValues.Value_date (Some a)
         :: NullValues.Value_date (Some b) :: nil)) =
    Z.ltb a b.
Proof.
  intros a b.
  unfold NullPredicates.interp_predicate.
  rewrite (order_value_compare_date_nonnull a b).
  destruct (Z.compare_spec a b) as [H|H|H].
  - subst b. rewrite Z.ltb_irrefl. reflexivity.
  - rewrite (proj2 (Z.ltb_lt a b) H). reflexivity.
  - rewrite (proj2 (Z.ltb_ge a b) ltac:(lia)). reflexivity.
Qed.

Lemma date_gte_acceptance_leb :
  forall a b,
    Bool.is_true Bool3
      (NullPredicates.interp_predicate PredicateGte
        (NullValues.Value_date (Some a)
         :: NullValues.Value_date (Some b) :: nil)) =
    Z.leb b a.
Proof.
  intros a b.
  unfold NullPredicates.interp_predicate.
  rewrite (order_value_compare_date_nonnull a b).
  destruct (Z.compare_spec a b) as [H|H|H].
  - subst b. rewrite Z.leb_refl. reflexivity.
  - rewrite (proj2 (Z.leb_gt b a) H). reflexivity.
  - rewrite (proj2 (Z.leb_le b a) ltac:(lia)). reflexivity.
Qed.

Lemma q12_acc_commit_lt_receipt :
  forall D sched o l,
    @scalar_expr_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      q12_pred_commit_lt_receipt
      (Z.ltb (l_commitdate l) (l_receiptdate l)).
Proof.
  intros D sched o l.
  set (row := join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)).
  set (env := Env.env_t TNull nil row).
  set (v1 := Value_date (Some (l_commitdate l))).
  set (v2 := Value_date (Some (l_receiptdate l))).
  assert (Hargs :
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
      [@SExpr_Leaf TNull relname type_date (DotDate "l_commitdate");
       @SExpr_Leaf TNull relname type_date (DotDate "l_receiptdate")]
      [v1; v2]).
  {
    eapply (@scalar_value_list_exact_cons TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
    - apply tnull_dot_date_exact.
      unfold env, row; rewrite interp_dot_env_t.
      + apply EmbedJoin_dot_l_commitdate.
      + apply EmbedJoin_attr_inS_lineitem; exact lineitem_attr_inS_commitdate.
    - eapply (@scalar_value_list_exact_cons TNull relname
        (@_basesort TNull D) (@_instance TNull D) unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
      + apply tnull_dot_date_exact.
        unfold env, row; rewrite interp_dot_env_t.
        * apply EmbedJoin_dot_l_receiptdate.
        * apply EmbedJoin_attr_inS_lineitem; exact lineitem_attr_inS_receiptdate.
      + apply (@scalar_value_list_exact_nil TNull relname
          (@_basesort TNull D) (@_instance TNull D) unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
  }
  pose proof (@pred_acceptance_of_value_list TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
    PredicateLt
    [@SExpr_Leaf TNull relname type_date (DotDate "l_commitdate");
     @SExpr_Leaf TNull relname type_date (DotDate "l_receiptdate")]
    [v1; v2] Hargs) as Hacc.
  unfold q12_pred_commit_lt_receipt.
  refine (eq_rect _ _ Hacc _ _).
  unfold v1, v2.
  apply date_lt_acceptance_ltb.
Qed.

Lemma q12_acc_ship_lt_commit :
  forall D sched o l,
    @scalar_expr_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      q12_pred_ship_lt_commit
      (Z.ltb (l_shipdate l) (l_commitdate l)).
Proof.
  intros D sched o l.
  set (row := join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)).
  set (env := Env.env_t TNull nil row).
  set (v1 := Value_date (Some (l_shipdate l))).
  set (v2 := Value_date (Some (l_commitdate l))).
  assert (Hargs :
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
      [@SExpr_Leaf TNull relname type_date (DotDate "l_shipdate");
       @SExpr_Leaf TNull relname type_date (DotDate "l_commitdate")]
      [v1; v2]).
  {
    eapply (@scalar_value_list_exact_cons TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
    - apply tnull_dot_date_exact.
      unfold env, row; rewrite interp_dot_env_t.
      + apply EmbedJoin_dot_l_shipdate.
      + apply EmbedJoin_attr_inS_lineitem; exact lineitem_attr_inS_shipdate.
    - eapply (@scalar_value_list_exact_cons TNull relname
        (@_basesort TNull D) (@_instance TNull D) unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
      + apply tnull_dot_date_exact.
        unfold env, row; rewrite interp_dot_env_t.
        * apply EmbedJoin_dot_l_commitdate.
        * apply EmbedJoin_attr_inS_lineitem; exact lineitem_attr_inS_commitdate.
      + apply (@scalar_value_list_exact_nil TNull relname
          (@_basesort TNull D) (@_instance TNull D) unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
  }
  pose proof (@pred_acceptance_of_value_list TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
    PredicateLt
    [@SExpr_Leaf TNull relname type_date (DotDate "l_shipdate");
     @SExpr_Leaf TNull relname type_date (DotDate "l_commitdate")]
    [v1; v2] Hargs) as Hacc.
  unfold q12_pred_ship_lt_commit.
  refine (eq_rect _ _ Hacc _ _).
  unfold v1, v2.
  apply date_lt_acceptance_ltb.
Qed.

Lemma q12_acc_receipt_gte_start :
  forall D sched o l,
    @scalar_expr_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      q12_pred_receipt_gte_start
      (Z.leb q12_catalog_start (l_receiptdate l)).
Proof.
  intros D sched o l.
  set (row := join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)).
  set (env := Env.env_t TNull nil row).
  set (vr := Value_date (Some (l_receiptdate l))).
  set (vs := Value_date (Some q12_catalog_start)).
  assert (Hargs :
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
      [@SExpr_Leaf TNull relname type_date (DotDate "l_receiptdate");
       @SExpr_Leaf TNull relname type_date (CstDate q12_catalog_start)]
      [vr; vs]).
  {
    eapply (@scalar_value_list_exact_cons TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
    - apply tnull_dot_date_exact.
      unfold env, row; rewrite interp_dot_env_t.
      + apply EmbedJoin_dot_l_receiptdate.
      + apply EmbedJoin_attr_inS_lineitem; exact lineitem_attr_inS_receiptdate.
    - eapply (@scalar_value_list_exact_cons TNull relname
        (@_basesort TNull D) (@_instance TNull D) unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
      + change (CstDate q12_catalog_start) with
          (AExpr (Constant (Value_date (Some q12_catalog_start)))).
        apply tnull_leaf_constant_exact.
      + apply (@scalar_value_list_exact_nil TNull relname
          (@_basesort TNull D) (@_instance TNull D) unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
  }
  pose proof (@pred_acceptance_of_value_list TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
    PredicateGte
    [@SExpr_Leaf TNull relname type_date (DotDate "l_receiptdate");
     @SExpr_Leaf TNull relname type_date (CstDate q12_catalog_start)]
    [vr; vs] Hargs) as Hacc.
  unfold q12_pred_receipt_gte_start.
  refine (eq_rect _ _ Hacc _ _).
  unfold vr, vs.
  apply date_gte_acceptance_leb.
Qed.

Lemma q12_acc_receipt_lt_yearend :
  forall D sched o l,
    finite_date_timestamp_ok (l_receiptdate l) ->
    @scalar_expr_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      q12_pred_receipt_lt_yearend
      (Z.ltb (l_receiptdate l) q12_catalog_end).
Proof.
  intros D sched o l Hd.
  unfold q12_pred_receipt_lt_yearend, q12_catalog_start, q12_catalog_end.
  eapply date_lt_timestamp_add_year_pred_acceptance.
  - apply EmbedJoin_attr_inS_lineitem; exact lineitem_attr_inS_receiptdate.
  - apply EmbedJoin_dot_l_receiptdate.
  - exact Hd.
  - exact q12_catalog_start_finite.
  - exact q12_catalog_end_finite.
  - exact q12_catalog_year_end_timestamp.
Qed.

Lemma q12_string_eq_acceptance_of_compare :
  forall typmod1 s1 typmod2 s2,
    Bool.is_true Bool3
      (NullPredicates.interp_predicate PredicateEq
        (NullValues.Value_string (StringValue typmod1 (Some s1))
         :: NullValues.Value_string (StringValue typmod2 (Some s2)) :: nil)) =
    match sql_string_compare typmod1 s1 typmod2 s2 with
    | Eq => true
    | Lt | Gt => false
    end.
Proof.
  intros typmod1 s1 typmod2 s2.
  unfold NullPredicates.interp_predicate.
  rewrite (order_value_compare_string_nonnull typmod1 s1 typmod2 s2).
  destruct (sql_string_compare typmod1 s1 typmod2 s2); reflexivity.
Qed.

Lemma q12_mode_eq_mail_bool :
  forall mode,
    q12_shipmode_embed_ok mode ->
    match sql_string_compare (StringChar 10) mode StringBpchar
            (string_explicit_cast StringBpchar "MAIL") with
    | Eq => true
    | Lt | Gt => false
    end =
    String.eqb mode "MAIL".
Proof.
  intros mode Hrtrim.
  rewrite mail_bpchar_cast_id.
  destruct (sql_string_compare (StringChar 10) mode StringBpchar "MAIL")
    eqn:Hc.
  - apply (proj1 (sql_string_compare_eq_iff_semantic_values
                    (StringChar 10) mode StringBpchar "MAIL")) in Hc.
    cbn in Hc.
    unfold q12_shipmode_embed_ok in Hrtrim.
    rewrite Hrtrim in Hc.
    change (string_rtrim_spaces "MAIL") with "MAIL" in Hc.
    apply String.eqb_eq in Hc. rewrite Hc. reflexivity.
  - assert (String.eqb mode "MAIL" = false) as Hf.
    { apply String.eqb_neq. intro Heq. subst mode.
      assert (sql_string_compare (StringChar 10) "MAIL" StringBpchar "MAIL" = Eq).
      { apply (proj2 (sql_string_compare_eq_iff_semantic_values
                        (StringChar 10) "MAIL" StringBpchar "MAIL")).
        cbn. reflexivity. }
      rewrite H in Hc; discriminate. }
    rewrite Hf; reflexivity.
  - assert (String.eqb mode "MAIL" = false) as Hf.
    { apply String.eqb_neq. intro Heq. subst mode.
      assert (sql_string_compare (StringChar 10) "MAIL" StringBpchar "MAIL" = Eq).
      { apply (proj2 (sql_string_compare_eq_iff_semantic_values
                        (StringChar 10) "MAIL" StringBpchar "MAIL")).
        cbn. reflexivity. }
      rewrite H in Hc; discriminate. }
    rewrite Hf; reflexivity.
Qed.

Lemma q12_mode_eq_ship_bool :
  forall mode,
    q12_shipmode_embed_ok mode ->
    match sql_string_compare (StringChar 10) mode StringBpchar
            (string_explicit_cast StringBpchar "SHIP") with
    | Eq => true
    | Lt | Gt => false
    end =
    String.eqb mode "SHIP".
Proof.
  intros mode Hrtrim.
  rewrite ship_bpchar_cast_id.
  destruct (sql_string_compare (StringChar 10) mode StringBpchar "SHIP")
    eqn:Hc.
  - apply (proj1 (sql_string_compare_eq_iff_semantic_values
                    (StringChar 10) mode StringBpchar "SHIP")) in Hc.
    cbn in Hc.
    unfold q12_shipmode_embed_ok in Hrtrim.
    rewrite Hrtrim in Hc.
    change (string_rtrim_spaces "SHIP") with "SHIP" in Hc.
    apply String.eqb_eq in Hc. rewrite Hc. reflexivity.
  - assert (String.eqb mode "SHIP" = false) as Hf.
    { apply String.eqb_neq. intro Heq. subst mode.
      assert (sql_string_compare (StringChar 10) "SHIP" StringBpchar "SHIP" = Eq).
      { apply (proj2 (sql_string_compare_eq_iff_semantic_values
                        (StringChar 10) "SHIP" StringBpchar "SHIP")).
        cbn. reflexivity. }
      rewrite H in Hc; discriminate. }
    rewrite Hf; reflexivity.
  - assert (String.eqb mode "SHIP" = false) as Hf.
    { apply String.eqb_neq. intro Heq. subst mode.
      assert (sql_string_compare (StringChar 10) "SHIP" StringBpchar "SHIP" = Eq).
      { apply (proj2 (sql_string_compare_eq_iff_semantic_values
                        (StringChar 10) "SHIP" StringBpchar "SHIP")).
        cbn. reflexivity. }
      rewrite H in Hc; discriminate. }
    rewrite Hf; reflexivity.
Qed.

Lemma q12_acc_mode_mail :
  forall D sched o l,
    q12_shipmode_embed_ok (l_shipmode l) ->
    @scalar_expr_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      q12_pred_mode_mail
      (String.eqb (l_shipmode l) "MAIL").
Proof.
  intros D sched o l Hrtrim.
  set (row := join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)).
  set (env := Env.env_t TNull nil row).
  set (vm := embed_string_char10 (l_shipmode l)).
  set (vc := Value_string (StringValue StringBpchar
               (Some (string_explicit_cast StringBpchar "MAIL")))).
  assert (Hargs :
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
      [@SExpr_Leaf TNull relname type_string (DotString "l_shipmode" (StringChar 10));
       @SExpr_Leaf TNull relname type_string (CstString StringBpchar "MAIL")]
      [vm; vc]).
  {
    eapply (@scalar_value_list_exact_cons TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
    - apply tnull_dot_string_exact.
      unfold env, row; rewrite interp_dot_env_t.
      + apply EmbedJoin_dot_l_shipmode.
      + apply EmbedJoin_attr_inS_lineitem; exact lineitem_attr_inS_shipmode.
    - eapply (@scalar_value_list_exact_cons TNull relname
        (@_basesort TNull D) (@_instance TNull D) unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
      + unfold CstString.
        apply tnull_leaf_constant_exact.
      + apply (@scalar_value_list_exact_nil TNull relname
          (@_basesort TNull D) (@_instance TNull D) unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
  }
  pose proof (@pred_acceptance_of_value_list TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
    PredicateEq
    [@SExpr_Leaf TNull relname type_string (DotString "l_shipmode" (StringChar 10));
     @SExpr_Leaf TNull relname type_string (CstString StringBpchar "MAIL")]
    [vm; vc] Hargs) as Hacc.
  unfold q12_pred_mode_mail.
  refine (eq_rect _ _ Hacc _ _).
  unfold vm, vc, embed_string_char10.
  rewrite q12_string_eq_acceptance_of_compare.
  apply q12_mode_eq_mail_bool; exact Hrtrim.
Qed.

Lemma q12_acc_mode_ship :
  forall D sched o l,
    q12_shipmode_embed_ok (l_shipmode l) ->
    @scalar_expr_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      q12_pred_mode_ship
      (String.eqb (l_shipmode l) "SHIP").
Proof.
  intros D sched o l Hrtrim.
  set (row := join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)).
  set (env := Env.env_t TNull nil row).
  set (vm := embed_string_char10 (l_shipmode l)).
  set (vc := Value_string (StringValue StringBpchar
               (Some (string_explicit_cast StringBpchar "SHIP")))).
  assert (Hargs :
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
      [@SExpr_Leaf TNull relname type_string (DotString "l_shipmode" (StringChar 10));
       @SExpr_Leaf TNull relname type_string (CstString StringBpchar "SHIP")]
      [vm; vc]).
  {
    eapply (@scalar_value_list_exact_cons TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
    - apply tnull_dot_string_exact.
      unfold env, row; rewrite interp_dot_env_t.
      + apply EmbedJoin_dot_l_shipmode.
      + apply EmbedJoin_attr_inS_lineitem; exact lineitem_attr_inS_shipmode.
    - eapply (@scalar_value_list_exact_cons TNull relname
        (@_basesort TNull D) (@_instance TNull D) unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
      + unfold CstString.
        apply tnull_leaf_constant_exact.
      + apply (@scalar_value_list_exact_nil TNull relname
          (@_basesort TNull D) (@_instance TNull D) unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
  }
  pose proof (@pred_acceptance_of_value_list TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
    PredicateEq
    [@SExpr_Leaf TNull relname type_string (DotString "l_shipmode" (StringChar 10));
     @SExpr_Leaf TNull relname type_string (CstString StringBpchar "SHIP")]
    [vm; vc] Hargs) as Hacc.
  unfold q12_pred_mode_ship.
  refine (eq_rect _ _ Hacc _ _).
  unfold vm, vc, embed_string_char10.
  rewrite q12_string_eq_acceptance_of_compare.
  apply q12_mode_eq_ship_bool; exact Hrtrim.
Qed.

Definition q12_mode_decide (l : LineRow)
    (e : scalar_expr TNull relname ScalarResultBoolean) : bool :=
  match e with
  | SExpr_Pred PredicateEq
      (_ :: SExpr_Leaf _ (A_Expr _ (F_Constant _
        (NullValues.Value_string (_, Some s)))) :: _) =>
      String.eqb (l_shipmode l) s
  | _ => false
  end.

Lemma q12_mode_decide_mail :
  forall l,
    q12_mode_decide l q12_pred_mode_mail =
    String.eqb (l_shipmode l) "MAIL".
Proof.
  intro l.
  unfold q12_mode_decide, q12_pred_mode_mail, CstString.
  cbn -[string_explicit_cast].
  rewrite mail_bpchar_cast_id.
  reflexivity.
Qed.

Lemma q12_mode_decide_ship :
  forall l,
    q12_mode_decide l q12_pred_mode_ship =
    String.eqb (l_shipmode l) "SHIP".
Proof.
  intro l.
  unfold q12_mode_decide, q12_pred_mode_ship, CstString.
  cbn -[string_explicit_cast].
  rewrite ship_bpchar_cast_id.
  reflexivity.
Qed.

Lemma q12_mode_fold_eq_ModeAccepted :
  forall l,
    scalar_acceptance_fold Or_F
      (map (q12_mode_decide l) [q12_pred_mode_mail; q12_pred_mode_ship]) =
    ModeAccepted (l_shipmode l) "MAIL" "SHIP".
Proof.
  intro l.
  cbn [map].
  rewrite q12_mode_decide_mail, q12_mode_decide_ship.
  unfold ModeAccepted, scalar_acceptance_fold, scalar_acceptance_combine,
    scalar_acceptance_identity.
  cbn.
  rewrite Bool.orb_false_r.
  reflexivity.
Qed.

Lemma q12_acc_mode_or :
  forall D sched o l,
    q12_shipmode_embed_ok (l_shipmode l) ->
    @scalar_expr_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      q12_pred_mode_or
      (ModeAccepted (l_shipmode l) "MAIL" "SHIP").
Proof.
  intros D sched o l Hrtrim.
  unfold q12_pred_mode_or.
  rewrite <- (q12_mode_fold_eq_ModeAccepted l).
  apply (@scalar_expr_conj_list_acceptance_exact TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched
    ([[]; ["b17"]])
    (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
    Or_F [q12_pred_mode_mail; q12_pred_mode_ship]
    (q12_mode_decide l)).
  intros e Hin.
  destruct Hin as [Heq|Hin]; [|destruct Hin as [Heq|Hin]; [|contradiction]].
  - subst e.
    rewrite q12_mode_decide_mail.
    apply q12_acc_mode_mail; exact Hrtrim.
  - subst e.
    rewrite q12_mode_decide_ship.
    apply q12_acc_mode_ship; exact Hrtrim.
Qed.

(** * Full WHERE decide + SpecPairKeep correspondence *)

Definition q12_where_decide (o : OrderRow) (l : LineRow)
    (e : scalar_expr TNull relname ScalarResultBoolean) : bool :=
  match e with
  | SExpr_Pred PredicateEq
      (SExpr_Leaf _ (A_Expr _ (F_Dot _ (Attr_int32 _))) :: _) =>
      Z.eqb (o_orderkey o) (l_orderkey l)
  | SExpr_ConjList _ Or_F _ =>
      ModeAccepted (l_shipmode l) "MAIL" "SHIP"
  | SExpr_Pred PredicateLt
      (SExpr_Leaf _ (A_Expr _ (F_Dot _ (Attr_date name))) :: _) =>
      if String.eqb name "l_commitdate"
      then Z.ltb (l_commitdate l) (l_receiptdate l)
      else if String.eqb name "l_shipdate"
      then Z.ltb (l_shipdate l) (l_commitdate l)
      else false
  | SExpr_Pred PredicateGte _ =>
      Z.leb q12_catalog_start (l_receiptdate l)
  | SExpr_Pred PredicateDateLtTimestamp _ =>
      Z.ltb (l_receiptdate l) q12_catalog_end
  | _ => false
  end.

Lemma q12_where_decide_eq_orderkeys :
  forall o l,
    q12_where_decide o l q12_pred_eq_orderkeys =
    Z.eqb (o_orderkey o) (l_orderkey l).
Proof. intros; reflexivity. Qed.

Lemma q12_where_decide_mode_or :
  forall o l,
    q12_where_decide o l q12_pred_mode_or =
    ModeAccepted (l_shipmode l) "MAIL" "SHIP".
Proof. intros; reflexivity. Qed.

Lemma q12_where_decide_commit_lt :
  forall o l,
    q12_where_decide o l q12_pred_commit_lt_receipt =
    Z.ltb (l_commitdate l) (l_receiptdate l).
Proof. intros; reflexivity. Qed.

Lemma q12_where_decide_ship_lt :
  forall o l,
    q12_where_decide o l q12_pred_ship_lt_commit =
    Z.ltb (l_shipdate l) (l_commitdate l).
Proof. intros; reflexivity. Qed.

Lemma q12_where_decide_gte_start :
  forall o l,
    q12_where_decide o l q12_pred_receipt_gte_start =
    Z.leb q12_catalog_start (l_receiptdate l).
Proof. intros; reflexivity. Qed.

Lemma q12_where_decide_lt_yearend :
  forall o l,
    q12_where_decide o l q12_pred_receipt_lt_yearend =
    Z.ltb (l_receiptdate l) q12_catalog_end.
Proof. intros; reflexivity. Qed.

Lemma q12_where_fold_eq_SpecPairKeep :
  forall o l,
    scalar_acceptance_fold And_F
      (map (q12_where_decide o l)
        [q12_pred_eq_orderkeys; q12_pred_mode_or;
         q12_pred_commit_lt_receipt; q12_pred_ship_lt_commit;
         q12_pred_receipt_gte_start; q12_pred_receipt_lt_yearend]) =
    SpecPairKeep o l.
Proof.
  intros o l.
  cbn [map].
  rewrite q12_where_decide_eq_orderkeys, q12_where_decide_mode_or,
    q12_where_decide_commit_lt, q12_where_decide_ship_lt,
    q12_where_decide_gte_start, q12_where_decide_lt_yearend.
  unfold SpecPairKeep, LineQualifies, InWindow, ModeAccepted,
    scalar_acceptance_fold, scalar_acceptance_combine,
    scalar_acceptance_identity.
  cbn.
  rewrite Bool.andb_true_r.
  destruct (Z.eqb (o_orderkey o) (l_orderkey l));
  destruct (String.eqb (l_shipmode l) "MAIL");
  destruct (String.eqb (l_shipmode l) "SHIP");
  destruct (Z.ltb (l_commitdate l) (l_receiptdate l));
  destruct (Z.ltb (l_shipdate l) (l_commitdate l));
  destruct (Z.leb q12_catalog_start (l_receiptdate l));
  destruct (Z.ltb (l_receiptdate l) q12_catalog_end);
    reflexivity.
Qed.

Lemma q12_where_acceptance_embed_join :
  forall D sched o l,
    q12_pair_domain_ok o l ->
    @scalar_expr_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      Queries.scalar_expr_predicate_1
      (SpecPairKeep o l).
Proof.
  intros D sched o l Hdom.
  destruct Hdom as [Hok [Hlk [Hmode [Hc [Hr Hs]]]]].
  rewrite q12_predicate_1_structure.
  rewrite <- (q12_where_fold_eq_SpecPairKeep o l).
  apply (@scalar_expr_conj_list_acceptance_exact TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched
    ([[]; ["b2"]; ["b3"; "b4"]; ["b5"; "b6"; "b7"];
      ["b8"; "b9"; "b10"; "b11"]; ["b12"; "b13"; "b14"; "b15"; "b16"]])
    (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
    And_F
    [q12_pred_eq_orderkeys; q12_pred_mode_or;
     q12_pred_commit_lt_receipt; q12_pred_ship_lt_commit;
     q12_pred_receipt_gte_start; q12_pred_receipt_lt_yearend]
    (q12_where_decide o l)).
  intros e Hin.
  repeat (destruct Hin as [Heq|Hin]; [subst e|]); try contradiction.
  - rewrite q12_where_decide_eq_orderkeys.
    apply q12_acc_eq_orderkeys; assumption.
  - rewrite q12_where_decide_mode_or.
    apply q12_acc_mode_or; exact Hmode.
  - rewrite q12_where_decide_commit_lt.
    apply q12_acc_commit_lt_receipt.
  - rewrite q12_where_decide_ship_lt.
    apply q12_acc_ship_lt_commit.
  - rewrite q12_where_decide_gte_start.
    apply q12_acc_receipt_gte_start.
  - rewrite q12_where_decide_lt_yearend.
    apply q12_acc_receipt_lt_yearend; exact Hr.
Qed.

(** * FilterOk observation: EmbedJoin cross-product filtered by SpecPairKeep *)

Definition q12_embed_join_rows
    (orders : list OrderRow) (lines : list LineRow)
  : list (tuple TNull) :=
  flat_map
    (fun o =>
      map (fun l => join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l))
        lines)
    orders.

Definition q12_where_rows
    (orders : list OrderRow) (lines : list LineRow)
  : list (tuple TNull) :=
  flat_map
    (fun o =>
      map (fun l => join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l))
        (List.filter (fun l => SpecPairKeep o l) lines))
    orders.

(** Dot-based keep: equals SpecPairKeep on EmbedJoin rows under domain. *)
Definition q12_where_keep_dots (row : tuple TNull) : bool :=
  match dot TNull row (AttrInt32 "o_orderkey") with
  | NullValues.Value_int32 (Some ok) =>
      match dot TNull row (AttrInt32 "l_orderkey") with
      | NullValues.Value_int32 (Some lk) =>
          match dot TNull row (AttrString "l_shipmode" (StringChar 10)) with
          | NullValues.Value_string (StringChar 10, Some mode) =>
              match dot TNull row (AttrDate "l_commitdate") with
              | NullValues.Value_date (Some commit) =>
                  match dot TNull row (AttrDate "l_receiptdate") with
                  | NullValues.Value_date (Some receipt) =>
                      match dot TNull row (AttrDate "l_shipdate") with
                      | NullValues.Value_date (Some ship) =>
                          andb (Z.eqb (int32_value ok) (int32_value lk))
                            (andb (andb (Z.ltb commit receipt)
                                        (Z.ltb ship commit))
                              (andb
                                (andb (Z.leb q12_catalog_start receipt)
                                      (Z.ltb receipt q12_catalog_end))
                                (ModeAccepted mode "MAIL" "SHIP")))
                      | _ => false
                      end
                  | _ => false
                  end
              | _ => false
              end
          | _ => false
          end
      | _ => false
      end
  | _ => false
  end.

Lemma q12_where_keep_dots_embed_join :
  forall o l,
    q12_pair_domain_ok o l ->
    q12_where_keep_dots
      (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)) =
    SpecPairKeep o l.
Proof.
  intros o l [Hok [Hlk [Hmode _]]].
  unfold q12_where_keep_dots.
  rewrite EmbedJoin_dot_o_orderkey, EmbedJoin_dot_l_orderkey,
    EmbedJoin_dot_l_shipmode, EmbedJoin_dot_l_commitdate,
    EmbedJoin_dot_l_receiptdate, EmbedJoin_dot_l_shipdate.
  unfold embed_string_char10.
  cbv [Value_int32 Value_date Value_string StringValue].
  simpl.
  unfold q12_orderkey_embed_ok, int32_value, int32_from_twos_complement
    in Hok, Hlk.
  unfold SpecPairKeep, LineQualifies, InWindow.
  rewrite Hok, Hlk.
  reflexivity.
Qed.

(** where observation = List.filter keep_dots on EmbedJoin cross-product. *)

Definition q12_where_rows_dots
    (orders : list OrderRow) (lines : list LineRow)
  : list (tuple TNull) :=
  List.filter q12_where_keep_dots (q12_embed_join_rows orders lines).

Lemma q12_embed_join_rows_In :
  forall orders lines row,
    In row (q12_embed_join_rows orders lines) ->
    exists o l,
      In o orders /\ In l lines /\
      row = join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l).
Proof.
  intros orders lines row Hin.
  unfold q12_embed_join_rows in Hin.
  apply in_flat_map in Hin.
  destruct Hin as [o [Ho Hin]].
  apply in_map_iff in Hin.
  destruct Hin as [l [Heq Hl]].
  exists o, l; repeat split; [exact Ho | exact Hl | symmetry; exact Heq].
Qed.

Lemma q12_filter_embed_lines_keep :
  forall o lines,
    (forall l, In l lines -> q12_pair_domain_ok o l) ->
    List.filter q12_where_keep_dots
      (map (fun l => join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l))
         lines) =
    map (fun l => join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l))
      (List.filter (fun l => SpecPairKeep o l) lines).
Proof.
  intros o lines Hdom.
  induction lines as [|l ls IH]; [reflexivity|].
  cbn [map List.filter].
  rewrite (q12_where_keep_dots_embed_join o l (Hdom l (or_introl eq_refl))).
  destruct (SpecPairKeep o l); cbn [map];
    [f_equal|]; apply IH; intros l' Hl'; apply Hdom; right; exact Hl'.
Qed.

Lemma q12_where_rows_dots_eq :
  forall orders lines,
    (forall o l, In o orders -> In l lines -> q12_pair_domain_ok o l) ->
    q12_where_rows_dots orders lines = q12_where_rows orders lines.
Proof.
  intros orders lines Hdom.
  unfold q12_where_rows_dots, q12_where_rows, q12_embed_join_rows.
  induction orders as [|o os IH]; [reflexivity|].
  cbn [flat_map].
  rewrite filter_app.
  f_equal.
  - apply q12_filter_embed_lines_keep.
    intros l Hl; apply Hdom; [left; reflexivity|exact Hl].
  - apply IH.
    intros o' l' Ho' Hl'.
    apply Hdom; [right; exact Ho'|exact Hl'].
Qed.

Lemma q12_filter_where_dots_success :
  forall D orders lines,
    (forall o l, In o orders -> In l lines -> q12_pair_domain_ok o l) ->
    forall sched,
      @eval_filter_rows_outcome TNull relname
        (@_basesort TNull D) (@_instance TNull D)
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error q12_value_is_null sched
        nil Queries.scalar_expr_predicate_1
        (q12_embed_join_rows orders lines)
        (SqlSuccess (q12_where_rows_dots orders lines)).
Proof.
  intros D orders lines Hdom sched.
  unfold q12_where_rows_dots.
  eapply (@filter_rows_success_of_acceptance TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
  intros row Hin.
  destruct (q12_embed_join_rows_In orders lines row Hin)
    as [o [l [Ho [Hl Heq]]]].
  subst row.
  pose proof (Hdom o l Ho Hl) as Hpair.
  rewrite (q12_where_keep_dots_embed_join o l Hpair).
  apply q12_where_acceptance_embed_join; exact Hpair.
Qed.
