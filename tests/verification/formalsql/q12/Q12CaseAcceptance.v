(* Q12CaseAcceptance — CASE project (scalar_select_list_2) on WHERE rows.

   High/low CASE bits ↔ ClassifyPriority under priority embed domain.
   Consumed by Q12JoinSpine ProjectTableOk for scalar_select_list_2. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueInteger
  ValueTemporal ValueString FiniteSet FTuples OrderedSet FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts Interp ATerms FTerms Formula ValuePredicates Projection.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax
  FormalSQL.GroupedFilterOutcomeFacts FormalSQL.StringTemporalFacts
  FormalSQL.ScalarPredicateFacts FormalSQL.QueryCardinality.
From FormalSQLLib Require Import
  FilterAcceptanceFacade ProjectExactFacade CaseExactFacade
  TemporalPredAcceptanceFacade NumericCallExactFacade.
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
Require Import Q12WhereAcceptance.

Import Q12EvalSpec.

Definition q12_value_is_null (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

(** * Priority domain (Char15 rtrim, twin of shipmode embed_ok) *)

Definition q12_priority_embed_ok (p : string) : Prop :=
  string_rtrim_spaces p = p.

Lemma urgent_bpchar_cast_id :
  string_explicit_cast StringBpchar "1-URGENT" = "1-URGENT".
Proof. vm_compute. reflexivity. Qed.

Lemma high_bpchar_cast_id :
  string_explicit_cast StringBpchar "2-HIGH" = "2-HIGH".
Proof. vm_compute. reflexivity. Qed.

Lemma q12_string_neq_acceptance_of_compare :
  forall typmod1 s1 typmod2 s2,
    Bool.is_true Bool3
      (NullPredicates.interp_predicate PredicateNeq
        (NullValues.Value_string (StringValue typmod1 (Some s1))
         :: NullValues.Value_string (StringValue typmod2 (Some s2)) :: nil)) =
    match sql_string_compare typmod1 s1 typmod2 s2 with
    | Eq => false
    | Lt | Gt => true
    end.
Proof.
  intros typmod1 s1 typmod2 s2.
  unfold NullPredicates.interp_predicate.
  rewrite (order_value_compare_string_nonnull typmod1 s1 typmod2 s2).
  destruct (sql_string_compare typmod1 s1 typmod2 s2); reflexivity.
Qed.

Lemma q12_priority_eq_urgent_bool :
  forall p,
    q12_priority_embed_ok p ->
    match sql_string_compare (StringChar 15) p StringBpchar
            (string_explicit_cast StringBpchar "1-URGENT") with
    | Eq => true
    | Lt | Gt => false
    end =
    String.eqb p "1-URGENT".
Proof.
  intros p Hrtrim.
  rewrite urgent_bpchar_cast_id.
  destruct (sql_string_compare (StringChar 15) p StringBpchar "1-URGENT")
    eqn:Hc.
  - apply (proj1 (sql_string_compare_eq_iff_semantic_values
                    (StringChar 15) p StringBpchar "1-URGENT")) in Hc.
    cbn in Hc.
    unfold q12_priority_embed_ok in Hrtrim.
    rewrite Hrtrim in Hc.
    change (string_rtrim_spaces "1-URGENT") with "1-URGENT" in Hc.
    apply String.eqb_eq in Hc; rewrite Hc; reflexivity.
  - assert (String.eqb p "1-URGENT" = false) as Hf.
    { apply String.eqb_neq. intro Heq. subst p.
      assert (sql_string_compare (StringChar 15) "1-URGENT" StringBpchar
                "1-URGENT" = Eq).
      { apply (proj2 (sql_string_compare_eq_iff_semantic_values
                        (StringChar 15) "1-URGENT" StringBpchar "1-URGENT")).
        cbn. reflexivity. }
      rewrite H in Hc; discriminate. }
    rewrite Hf; reflexivity.
  - assert (String.eqb p "1-URGENT" = false) as Hf.
    { apply String.eqb_neq. intro Heq. subst p.
      assert (sql_string_compare (StringChar 15) "1-URGENT" StringBpchar
                "1-URGENT" = Eq).
      { apply (proj2 (sql_string_compare_eq_iff_semantic_values
                        (StringChar 15) "1-URGENT" StringBpchar "1-URGENT")).
        cbn. reflexivity. }
      rewrite H in Hc; discriminate. }
    rewrite Hf; reflexivity.
Qed.

Lemma q12_priority_eq_high_bool :
  forall p,
    q12_priority_embed_ok p ->
    match sql_string_compare (StringChar 15) p StringBpchar
            (string_explicit_cast StringBpchar "2-HIGH") with
    | Eq => true
    | Lt | Gt => false
    end =
    String.eqb p "2-HIGH".
Proof.
  intros p Hrtrim.
  rewrite high_bpchar_cast_id.
  destruct (sql_string_compare (StringChar 15) p StringBpchar "2-HIGH")
    eqn:Hc.
  - apply (proj1 (sql_string_compare_eq_iff_semantic_values
                    (StringChar 15) p StringBpchar "2-HIGH")) in Hc.
    cbn in Hc.
    unfold q12_priority_embed_ok in Hrtrim.
    rewrite Hrtrim in Hc.
    change (string_rtrim_spaces "2-HIGH") with "2-HIGH" in Hc.
    apply String.eqb_eq in Hc; rewrite Hc; reflexivity.
  - assert (String.eqb p "2-HIGH" = false) as Hf.
    { apply String.eqb_neq. intro Heq. subst p.
      assert (sql_string_compare (StringChar 15) "2-HIGH" StringBpchar
                "2-HIGH" = Eq).
      { apply (proj2 (sql_string_compare_eq_iff_semantic_values
                        (StringChar 15) "2-HIGH" StringBpchar "2-HIGH")).
        cbn. reflexivity. }
      rewrite H in Hc; discriminate. }
    rewrite Hf; reflexivity.
  - assert (String.eqb p "2-HIGH" = false) as Hf.
    { apply String.eqb_neq. intro Heq. subst p.
      assert (sql_string_compare (StringChar 15) "2-HIGH" StringBpchar
                "2-HIGH" = Eq).
      { apply (proj2 (sql_string_compare_eq_iff_semantic_values
                        (StringChar 15) "2-HIGH" StringBpchar "2-HIGH")).
        cbn. reflexivity. }
      rewrite H in Hc; discriminate. }
    rewrite Hf; reflexivity.
Qed.

(** * CASE AST fragments (match Queries.scalar_select_list_2) *)

Definition q12_pred_priority_eq_urgent :
    scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateEq
    [@SExpr_Leaf TNull relname type_string
       (DotString "o_orderpriority" (StringChar 15));
     @SExpr_Leaf TNull relname type_string
       (CstString StringBpchar "1-URGENT")].

Definition q12_pred_priority_eq_high :
    scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateEq
    [@SExpr_Leaf TNull relname type_string
       (DotString "o_orderpriority" (StringChar 15));
     @SExpr_Leaf TNull relname type_string
       (CstString StringBpchar "2-HIGH")].

Definition q12_pred_priority_neq_urgent :
    scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateNeq
    [@SExpr_Leaf TNull relname type_string
       (DotString "o_orderpriority" (StringChar 15));
     @SExpr_Leaf TNull relname type_string
       (CstString StringBpchar "1-URGENT")].

Definition q12_pred_priority_neq_high :
    scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateNeq
    [@SExpr_Leaf TNull relname type_string
       (DotString "o_orderpriority" (StringChar 15));
     @SExpr_Leaf TNull relname type_string
       (CstString StringBpchar "2-HIGH")].

Definition q12_case_high_cond :
    scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_ConjList TNull relname ([[]; ["b0"]]) Or_F
    [q12_pred_priority_eq_urgent; q12_pred_priority_eq_high].

Definition q12_case_low_cond :
    scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_ConjList TNull relname ([[]; ["b1"]]) And_F
    [q12_pred_priority_neq_urgent; q12_pred_priority_neq_high].

Definition q12_case_high_expr :
    scalar_expr TNull relname ScalarResultValue :=
  @SExpr_Case TNull relname type_int32 q12_case_high_cond
    (@SExpr_Leaf TNull relname type_int32 (CstInt32 1))
    (@SExpr_Leaf TNull relname type_int32 (CstInt32 0)).

Definition q12_case_low_expr :
    scalar_expr TNull relname ScalarResultValue :=
  @SExpr_Case TNull relname type_int32 q12_case_low_cond
    (@SExpr_Leaf TNull relname type_int32 (CstInt32 1))
    (@SExpr_Leaf TNull relname type_int32 (CstInt32 0)).

Lemma q12_case_exprs_of_select_list_2 :
  map fst Queries.scalar_select_list_2 =
  [@SExpr_Leaf TNull relname type_string
     (DotString "l_shipmode" (StringChar 10));
   q12_case_high_expr;
   q12_case_low_expr].
Proof.
  unfold Queries.scalar_select_list_2, q12_case_high_expr, q12_case_low_expr,
    q12_case_high_cond, q12_case_low_cond,
    q12_pred_priority_eq_urgent, q12_pred_priority_eq_high,
    q12_pred_priority_neq_urgent, q12_pred_priority_neq_high.
  reflexivity.
Qed.

(** * Priority Eq / Neq acceptance *)

Lemma q12_acc_priority_eq_urgent :
  forall D sched o l,
    q12_priority_embed_ok (o_priority o) ->
    @scalar_expr_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      q12_pred_priority_eq_urgent
      (String.eqb (o_priority o) "1-URGENT").
Proof.
  intros D sched o l Hrtrim.
  set (row := join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)).
  set (env := Env.env_t TNull nil row).
  set (vp := embed_string_char15 (o_priority o)).
  set (vc := Value_string (StringValue StringBpchar
               (Some (string_explicit_cast StringBpchar "1-URGENT")))).
  assert (Hargs :
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
      [@SExpr_Leaf TNull relname type_string
         (DotString "o_orderpriority" (StringChar 15));
       @SExpr_Leaf TNull relname type_string
         (CstString StringBpchar "1-URGENT")]
      [vp; vc]).
  {
    eapply (@scalar_value_list_exact_cons TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
    - apply tnull_dot_string_exact.
      unfold env, row; rewrite interp_dot_env_t.
      + apply EmbedJoin_dot_o_priority.
      + apply EmbedJoin_attr_inS_orders; exact orders_attr_inS_priority.
    - eapply (@scalar_value_list_exact_cons TNull relname
        (@_basesort TNull D) (@_instance TNull D) unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
      + unfold CstString. apply tnull_leaf_constant_exact.
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
    [@SExpr_Leaf TNull relname type_string
       (DotString "o_orderpriority" (StringChar 15));
     @SExpr_Leaf TNull relname type_string
       (CstString StringBpchar "1-URGENT")]
    [vp; vc] Hargs) as Hacc.
  unfold q12_pred_priority_eq_urgent.
  refine (eq_rect _ _ Hacc _ _).
  unfold vp, vc, embed_string_char15.
  rewrite q12_string_eq_acceptance_of_compare.
  apply q12_priority_eq_urgent_bool; exact Hrtrim.
Qed.

Lemma q12_acc_priority_eq_high :
  forall D sched o l,
    q12_priority_embed_ok (o_priority o) ->
    @scalar_expr_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      q12_pred_priority_eq_high
      (String.eqb (o_priority o) "2-HIGH").
Proof.
  intros D sched o l Hrtrim.
  set (row := join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)).
  set (env := Env.env_t TNull nil row).
  set (vp := embed_string_char15 (o_priority o)).
  set (vc := Value_string (StringValue StringBpchar
               (Some (string_explicit_cast StringBpchar "2-HIGH")))).
  assert (Hargs :
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
      [@SExpr_Leaf TNull relname type_string
         (DotString "o_orderpriority" (StringChar 15));
       @SExpr_Leaf TNull relname type_string
         (CstString StringBpchar "2-HIGH")]
      [vp; vc]).
  {
    eapply (@scalar_value_list_exact_cons TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
    - apply tnull_dot_string_exact.
      unfold env, row; rewrite interp_dot_env_t.
      + apply EmbedJoin_dot_o_priority.
      + apply EmbedJoin_attr_inS_orders; exact orders_attr_inS_priority.
    - eapply (@scalar_value_list_exact_cons TNull relname
        (@_basesort TNull D) (@_instance TNull D) unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
      + unfold CstString. apply tnull_leaf_constant_exact.
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
    [@SExpr_Leaf TNull relname type_string
       (DotString "o_orderpriority" (StringChar 15));
     @SExpr_Leaf TNull relname type_string
       (CstString StringBpchar "2-HIGH")]
    [vp; vc] Hargs) as Hacc.
  unfold q12_pred_priority_eq_high.
  refine (eq_rect _ _ Hacc _ _).
  unfold vp, vc, embed_string_char15.
  rewrite q12_string_eq_acceptance_of_compare.
  apply q12_priority_eq_high_bool; exact Hrtrim.
Qed.

Lemma q12_acc_priority_neq_urgent :
  forall D sched o l,
    q12_priority_embed_ok (o_priority o) ->
    @scalar_expr_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      q12_pred_priority_neq_urgent
      (negb (String.eqb (o_priority o) "1-URGENT")).
Proof.
  intros D sched o l Hrtrim.
  set (row := join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)).
  set (env := Env.env_t TNull nil row).
  set (vp := embed_string_char15 (o_priority o)).
  set (vc := Value_string (StringValue StringBpchar
               (Some (string_explicit_cast StringBpchar "1-URGENT")))).
  assert (Hargs :
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
      [@SExpr_Leaf TNull relname type_string
         (DotString "o_orderpriority" (StringChar 15));
       @SExpr_Leaf TNull relname type_string
         (CstString StringBpchar "1-URGENT")]
      [vp; vc]).
  {
    eapply (@scalar_value_list_exact_cons TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
    - apply tnull_dot_string_exact.
      unfold env, row; rewrite interp_dot_env_t.
      + apply EmbedJoin_dot_o_priority.
      + apply EmbedJoin_attr_inS_orders; exact orders_attr_inS_priority.
    - eapply (@scalar_value_list_exact_cons TNull relname
        (@_basesort TNull D) (@_instance TNull D) unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
      + unfold CstString. apply tnull_leaf_constant_exact.
      + apply (@scalar_value_list_exact_nil TNull relname
          (@_basesort TNull D) (@_instance TNull D) unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
  }
  pose proof (@pred_acceptance_of_value_list TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
    PredicateNeq
    [@SExpr_Leaf TNull relname type_string
       (DotString "o_orderpriority" (StringChar 15));
     @SExpr_Leaf TNull relname type_string
       (CstString StringBpchar "1-URGENT")]
    [vp; vc] Hargs) as Hacc.
  unfold q12_pred_priority_neq_urgent.
  refine (eq_rect _ _ Hacc _ _).
  unfold vp, vc, embed_string_char15.
  rewrite q12_string_neq_acceptance_of_compare.
  pose proof (q12_priority_eq_urgent_bool (o_priority o) Hrtrim) as Heq.
  destruct (sql_string_compare (StringChar 15) (o_priority o) StringBpchar
              (string_explicit_cast StringBpchar "1-URGENT"));
    cbn [negb]; rewrite <- Heq; reflexivity.
Qed.

Lemma q12_acc_priority_neq_high :
  forall D sched o l,
    q12_priority_embed_ok (o_priority o) ->
    @scalar_expr_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      q12_pred_priority_neq_high
      (negb (String.eqb (o_priority o) "2-HIGH")).
Proof.
  intros D sched o l Hrtrim.
  set (row := join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)).
  set (env := Env.env_t TNull nil row).
  set (vp := embed_string_char15 (o_priority o)).
  set (vc := Value_string (StringValue StringBpchar
               (Some (string_explicit_cast StringBpchar "2-HIGH")))).
  assert (Hargs :
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
      [@SExpr_Leaf TNull relname type_string
         (DotString "o_orderpriority" (StringChar 15));
       @SExpr_Leaf TNull relname type_string
         (CstString StringBpchar "2-HIGH")]
      [vp; vc]).
  {
    eapply (@scalar_value_list_exact_cons TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
    - apply tnull_dot_string_exact.
      unfold env, row; rewrite interp_dot_env_t.
      + apply EmbedJoin_dot_o_priority.
      + apply EmbedJoin_attr_inS_orders; exact orders_attr_inS_priority.
    - eapply (@scalar_value_list_exact_cons TNull relname
        (@_basesort TNull D) (@_instance TNull D) unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
      + unfold CstString. apply tnull_leaf_constant_exact.
      + apply (@scalar_value_list_exact_nil TNull relname
          (@_basesort TNull D) (@_instance TNull D) unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
  }
  pose proof (@pred_acceptance_of_value_list TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
    PredicateNeq
    [@SExpr_Leaf TNull relname type_string
       (DotString "o_orderpriority" (StringChar 15));
     @SExpr_Leaf TNull relname type_string
       (CstString StringBpchar "2-HIGH")]
    [vp; vc] Hargs) as Hacc.
  unfold q12_pred_priority_neq_high.
  refine (eq_rect _ _ Hacc _ _).
  unfold vp, vc, embed_string_char15.
  rewrite q12_string_neq_acceptance_of_compare.
  pose proof (q12_priority_eq_high_bool (o_priority o) Hrtrim) as Heq.
  destruct (sql_string_compare (StringChar 15) (o_priority o) StringBpchar
              (string_explicit_cast StringBpchar "2-HIGH"));
    cbn [negb]; rewrite <- Heq; reflexivity.
Qed.

(** * High / low condition acceptance ↔ ClassifyPriority *)

Definition q12_priority_is_high (p : string) : bool :=
  orb (String.eqb p "1-URGENT") (String.eqb p "2-HIGH").

Lemma q12_priority_is_high_Classify :
  forall p,
    q12_priority_is_high p = true <-> ClassifyPriority p = High.
Proof.
  intro p; unfold q12_priority_is_high, ClassifyPriority.
  destruct (String.eqb p "1-URGENT") eqn:Hu; cbn.
  - split; [intros _; reflexivity|intros _; reflexivity].
  - destruct (String.eqb p "2-HIGH") eqn:Hh; cbn.
    + split; [intros _; reflexivity|intros _; reflexivity].
    + split; [discriminate|discriminate].
Qed.

Lemma q12_priority_is_high_negb_Classify_Low :
  forall p,
    negb (q12_priority_is_high p) = true <-> ClassifyPriority p = Low.
Proof.
  intro p.
  unfold q12_priority_is_high, ClassifyPriority.
  destruct (String.eqb p "1-URGENT") eqn:Hu; cbn.
  - split; [discriminate|discriminate].
  - destruct (String.eqb p "2-HIGH") eqn:Hh; cbn.
    + split; [discriminate|discriminate].
    + split; [intros _; reflexivity|intros _; reflexivity].
Qed.

Lemma q12_high_cond_fold :
  forall p,
    scalar_acceptance_fold Or_F
      [String.eqb p "1-URGENT"; String.eqb p "2-HIGH"] =
    q12_priority_is_high p.
Proof.
  intro p.
  unfold q12_priority_is_high, scalar_acceptance_fold,
    scalar_acceptance_combine, scalar_acceptance_identity.
  cbn. rewrite Bool.orb_false_r. reflexivity.
Qed.

Lemma q12_low_cond_fold :
  forall p,
    scalar_acceptance_fold And_F
      [negb (String.eqb p "1-URGENT"); negb (String.eqb p "2-HIGH")] =
    negb (q12_priority_is_high p).
Proof.
  intro p.
  unfold q12_priority_is_high, scalar_acceptance_fold,
    scalar_acceptance_combine, scalar_acceptance_identity.
  cbn. rewrite Bool.andb_true_r.
  destruct (String.eqb p "1-URGENT"); destruct (String.eqb p "2-HIGH");
    reflexivity.
Qed.

Definition q12_priority_eq_decide (p : string)
    (e : scalar_expr TNull relname ScalarResultBoolean) : bool :=
  match e with
  | SExpr_Pred PredicateEq
      (_ :: SExpr_Leaf _ (A_Expr _ (F_Constant _
        (NullValues.Value_string (_, Some s)))) :: _) =>
      String.eqb p s
  | _ => false
  end.

Definition q12_priority_neq_decide (p : string)
    (e : scalar_expr TNull relname ScalarResultBoolean) : bool :=
  match e with
  | SExpr_Pred PredicateNeq
      (_ :: SExpr_Leaf _ (A_Expr _ (F_Constant _
        (NullValues.Value_string (_, Some s)))) :: _) =>
      negb (String.eqb p s)
  | _ => false
  end.

Lemma q12_priority_eq_decide_urgent :
  forall p,
    q12_priority_eq_decide p q12_pred_priority_eq_urgent =
    String.eqb p "1-URGENT".
Proof.
  intro p.
  unfold q12_priority_eq_decide, q12_pred_priority_eq_urgent, CstString.
  cbn -[string_explicit_cast].
  rewrite urgent_bpchar_cast_id; reflexivity.
Qed.

Lemma q12_priority_eq_decide_high :
  forall p,
    q12_priority_eq_decide p q12_pred_priority_eq_high =
    String.eqb p "2-HIGH".
Proof.
  intro p.
  unfold q12_priority_eq_decide, q12_pred_priority_eq_high, CstString.
  cbn -[string_explicit_cast].
  rewrite high_bpchar_cast_id; reflexivity.
Qed.

Lemma q12_priority_neq_decide_urgent :
  forall p,
    q12_priority_neq_decide p q12_pred_priority_neq_urgent =
    negb (String.eqb p "1-URGENT").
Proof.
  intro p.
  unfold q12_priority_neq_decide, q12_pred_priority_neq_urgent, CstString.
  cbn -[string_explicit_cast].
  rewrite urgent_bpchar_cast_id; reflexivity.
Qed.

Lemma q12_priority_neq_decide_high :
  forall p,
    q12_priority_neq_decide p q12_pred_priority_neq_high =
    negb (String.eqb p "2-HIGH").
Proof.
  intro p.
  unfold q12_priority_neq_decide, q12_pred_priority_neq_high, CstString.
  cbn -[string_explicit_cast].
  rewrite high_bpchar_cast_id; reflexivity.
Qed.

Lemma q12_acc_case_high_cond :
  forall D sched o l,
    q12_priority_embed_ok (o_priority o) ->
    @scalar_expr_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      q12_case_high_cond
      (q12_priority_is_high (o_priority o)).
Proof.
  intros D sched o l Hrtrim.
  unfold q12_case_high_cond.
  rewrite <- (q12_high_cond_fold (o_priority o)).
  apply (@scalar_expr_conj_list_acceptance_exact TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched
    ([[]; ["b0"]])
    (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
    Or_F [q12_pred_priority_eq_urgent; q12_pred_priority_eq_high]
    (q12_priority_eq_decide (o_priority o))).
  intros e Hin.
  destruct Hin as [Heq|Hin]; [|destruct Hin as [Heq|Hin]; [|contradiction]].
  - subst e. rewrite q12_priority_eq_decide_urgent.
    apply q12_acc_priority_eq_urgent; exact Hrtrim.
  - subst e. rewrite q12_priority_eq_decide_high.
    apply q12_acc_priority_eq_high; exact Hrtrim.
Qed.

Lemma q12_acc_case_low_cond :
  forall D sched o l,
    q12_priority_embed_ok (o_priority o) ->
    @scalar_expr_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      q12_case_low_cond
      (negb (q12_priority_is_high (o_priority o))).
Proof.
  intros D sched o l Hrtrim.
  unfold q12_case_low_cond.
  rewrite <- (q12_low_cond_fold (o_priority o)).
  apply (@scalar_expr_conj_list_acceptance_exact TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched
    ([[]; ["b1"]])
    (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
    And_F [q12_pred_priority_neq_urgent; q12_pred_priority_neq_high]
    (q12_priority_neq_decide (o_priority o))).
  intros e Hin.
  destruct Hin as [Heq|Hin]; [|destruct Hin as [Heq|Hin]; [|contradiction]].
  - subst e. rewrite q12_priority_neq_decide_urgent.
    apply q12_acc_priority_neq_urgent; exact Hrtrim.
  - subst e. rewrite q12_priority_neq_decide_high.
    apply q12_acc_priority_neq_high; exact Hrtrim.
Qed.

(** * CASE value exactness *)

Definition q12_case_bit_z (accepted : bool) : Z :=
  match accepted with
  | true => 1
  | false => 0
  end.

Definition q12_case_bit (accepted : bool) : NullValues.value :=
  Value_int32 (int32_checked (q12_case_bit_z accepted)).

Lemma q12_case_bit_exact_leaf :
  forall D sched env accepted,
    @scalar_value_expr_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched env
      (@SExpr_Leaf TNull relname type_int32
         (CstInt32 (q12_case_bit_z accepted)))
      (q12_case_bit accepted).
Proof.
  intros D sched env accepted.
  unfold q12_case_bit, q12_case_bit_z, CstInt32.
  destruct accepted; apply tnull_leaf_constant_exact.
Qed.

Lemma q12_case_high_exact :
  forall D sched o l,
    q12_priority_embed_ok (o_priority o) ->
    @scalar_value_expr_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      q12_case_high_expr
      (q12_case_bit (q12_priority_is_high (o_priority o))).
Proof.
  intros D sched o l Hrtrim.
  unfold q12_case_high_expr.
  set (acc := q12_priority_is_high (o_priority o)).
  replace (q12_case_bit acc) with
    (if acc then q12_case_bit true else q12_case_bit false)
    by (destruct acc; reflexivity).
  eapply (@scalar_value_expr_case_exact_of_acceptance TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    q12_value_is_null sched
    _ _ _ _ _ acc (q12_case_bit true) (q12_case_bit false)).
  - apply q12_acc_case_high_cond; exact Hrtrim.
  - intro Ha.
    change (CstInt32 1) with (CstInt32 (q12_case_bit_z true)).
    apply q12_case_bit_exact_leaf.
  - intro Ha.
    change (CstInt32 0) with (CstInt32 (q12_case_bit_z false)).
    apply q12_case_bit_exact_leaf.
Qed.

Lemma q12_case_low_exact :
  forall D sched o l,
    q12_priority_embed_ok (o_priority o) ->
    @scalar_value_expr_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      q12_case_low_expr
      (q12_case_bit (negb (q12_priority_is_high (o_priority o)))).
Proof.
  intros D sched o l Hrtrim.
  unfold q12_case_low_expr.
  set (acc := negb (q12_priority_is_high (o_priority o))).
  replace (q12_case_bit acc) with
    (if acc then q12_case_bit true else q12_case_bit false)
    by (destruct acc; reflexivity).
  eapply (@scalar_value_expr_case_exact_of_acceptance TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    q12_value_is_null sched
    _ _ _ _ _ acc (q12_case_bit true) (q12_case_bit false)).
  - apply q12_acc_case_low_cond; exact Hrtrim.
  - intro Ha.
    change (CstInt32 1) with (CstInt32 (q12_case_bit_z true)).
    apply q12_case_bit_exact_leaf.
  - intro Ha.
    change (CstInt32 0) with (CstInt32 (q12_case_bit_z false)).
    apply q12_case_bit_exact_leaf.
Qed.

(** * Select-list exact + project rows *)

Definition q12_case_select_values (o : OrderRow) (l : LineRow)
  : list NullValues.value :=
  [ embed_string_char10 (l_shipmode l)
  ; q12_case_bit (q12_priority_is_high (o_priority o))
  ; q12_case_bit (negb (q12_priority_is_high (o_priority o)))
  ].

Lemma q12_case_select_exact :
  forall D sched o l,
    q12_priority_embed_ok (o_priority o) ->
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)))
      (map fst Queries.scalar_select_list_2)
      (q12_case_select_values o l).
Proof.
  intros D sched o l Hrtrim.
  rewrite q12_case_exprs_of_select_list_2.
  unfold q12_case_select_values.
  eapply (@scalar_value_list_exact_cons TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
  - apply tnull_dot_string_exact.
    rewrite interp_dot_env_t.
    + apply EmbedJoin_dot_l_shipmode.
    + apply EmbedJoin_attr_inS_lineitem; exact lineitem_attr_inS_shipmode.
  - eapply (@scalar_value_list_exact_cons TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
    + apply q12_case_high_exact; exact Hrtrim.
    + eapply (@scalar_value_list_exact_cons TNull relname
        (@_basesort TNull D) (@_instance TNull D) unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
      * apply q12_case_low_exact; exact Hrtrim.
      * apply (@scalar_value_list_exact_nil TNull relname
          (@_basesort TNull D) (@_instance TNull D) unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
Qed.

Definition q12_pre_group_row (o : OrderRow) (l : LineRow) : tuple TNull :=
  project_row Queries.scalar_select_list_2 (q12_case_select_values o l).

Definition q12_pre_group_rows
    (orders : list OrderRow) (lines : list LineRow)
  : list (tuple TNull) :=
  flat_map
    (fun o =>
      map (fun l => q12_pre_group_row o l)
        (List.filter (fun l => SpecPairKeep o l) lines))
    orders.

Lemma q12_where_rows_In_inv :
  forall orders lines row,
    In row (q12_where_rows orders lines) ->
    exists o l,
      In o orders /\ In l lines /\ SpecPairKeep o l = true /\
      row = join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l).
Proof.
  intros orders lines row Hin.
  unfold q12_where_rows in Hin.
  apply in_flat_map in Hin.
  destruct Hin as [o [Ho Hin]].
  apply in_map_iff in Hin.
  destruct Hin as [l [Heq Hin]].
  apply filter_In in Hin.
  destruct Hin as [Hl Hkeep].
  exists o, l. repeat split; try assumption.
  symmetry; exact Heq.
Qed.

(** Recover CASE select values from join-row dots (same as Spec CASE bits). *)
Definition q12_case_values_from_row (row : tuple TNull) : list NullValues.value :=
  match
    dot TNull row (AttrString "l_shipmode" (StringChar 10)),
    dot TNull row (AttrString "o_orderpriority" (StringChar 15))
  with
  | NullValues.Value_string (StringChar 10, Some mode),
    NullValues.Value_string (StringChar 15, Some pri) =>
      [ embed_string_char10 mode
      ; q12_case_bit (q12_priority_is_high pri)
      ; q12_case_bit (negb (q12_priority_is_high pri)) ]
  | _, _ =>
      [ embed_string_char10 ""
      ; q12_case_bit false
      ; q12_case_bit true ]
  end.

Lemma q12_case_values_from_row_of_join :
  forall o l,
    q12_case_values_from_row
      (join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l)) =
    q12_case_select_values o l.
Proof.
  intros o l.
  unfold q12_case_values_from_row, q12_case_select_values.
  rewrite EmbedJoin_dot_l_shipmode, EmbedJoin_dot_o_priority.
  unfold embed_string_char10, embed_string_char15.
  reflexivity.
Qed.

Lemma q12_pre_group_map_one_order :
  forall o lines,
    map (fun l => q12_pre_group_row o l)
      (List.filter (fun l => SpecPairKeep o l) lines) =
    map (fun row =>
      project_row Queries.scalar_select_list_2
        (q12_case_values_from_row row))
      (map (fun l => join_tuple TNull (EmbedOrderRow o) (EmbedLineRow l))
        (List.filter (fun l => SpecPairKeep o l) lines)).
Proof.
  intros o lines.
  induction lines as [|l ls IH]; cbn [filter map]; [reflexivity|].
  destruct (SpecPairKeep o l); cbn [map].
  - unfold q12_pre_group_row.
    rewrite q12_case_values_from_row_of_join.
    f_equal; exact IH.
  - exact IH.
Qed.

Lemma q12_pre_group_rows_as_project_map :
  forall orders lines,
    q12_pre_group_rows orders lines =
    map (fun row =>
      project_row Queries.scalar_select_list_2
        (q12_case_values_from_row row))
      (q12_where_rows orders lines).
Proof.
  intros orders lines.
  unfold q12_pre_group_rows, q12_where_rows.
  induction orders as [|o os IH]; cbn [flat_map]; [reflexivity|].
  rewrite map_app.
  f_equal; [apply q12_pre_group_map_one_order|exact IH].
Qed.

Lemma q12_project_case_success :
  forall D sched orders lines,
    (forall o l, In o orders -> In l lines ->
       q12_priority_embed_ok (o_priority o)) ->
    @eval_project_rows_outcome TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      nil Queries.scalar_select_list_2
      (q12_where_rows orders lines)
      (SqlSuccess (q12_pre_group_rows orders lines)).
Proof.
  intros D sched orders lines Hpri.
  rewrite q12_pre_group_rows_as_project_map.
  eapply (@tnull_project_rows_success_of_exact_map
    (@_basesort TNull D) (@_instance TNull D) sched
    nil Queries.scalar_select_list_2
    (q12_where_rows orders lines)
    q12_case_values_from_row).
  intros row Hin.
  apply q12_where_rows_In_inv in Hin.
  destruct Hin as [o [l [Ho [Hl [_ Heq]]]]].
  subst row.
  rewrite q12_case_values_from_row_of_join.
  apply q12_case_select_exact.
  apply (Hpri o l Ho Hl).
Qed.


