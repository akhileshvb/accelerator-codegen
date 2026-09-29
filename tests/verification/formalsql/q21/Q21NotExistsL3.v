(* Q21NotExistsL3 — correlated NOT EXISTS(l3) layer over the Qed EXISTS(l2) stage.

   RelNode:   anti_not_exists_l3 =
                RelAntiJoin semi_exists_l2 scan_lineitem_late pred_not_exists_l3
              scan_lineitem_late = RelFilter scan_lineitem pred_late_lineitem_local
   FormalSQL: q_not_exists_outer =
                Filter (NOT EXISTS (Filter [l_orderkey = cor0_7 ;
                  l_suppkey <> cor0_9 ; l_receiptdate > l_commitdate]
                  (Table lineitem))) (q_exists_outer)

   Milestones:
     A  Q21_not_exists_l3_row_correlation
     B  Q21_not_exists_l3_acceptance
     C  Q21_not_exists_l3_bag (+ filter_rows / anti eval)
     D  Q21_NotExistsL3StageCorrespond

   GROUP BY / Project / Sort and the 8-conjunct fusion are out of scope.
   No Admit/Axiom. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueNumericTypmod
  ValueString ValueInteger ValueTemporal ValuePredicates Formula
  SchemaConstraints SqlOutcome SqlQuerySyntax SqlQuerySemantics SqlOrder
  FiniteBag FiniteCollection FiniteSet FTuples Bool3 OrderedSet Env Interp Join.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax FormalSQL.ScalarPredicateFacts
  FormalSQL.StringTemporalFacts FormalSQL.WitnessFacts
  FormalSQL.GroupedFilterOutcomeFacts FormalSQL.QueryCardinality
  FormalSQL.RelationalAlgebraFacts.
From FormalSQLLib Require Import
  FilterAcceptanceFacade TemporalPredAcceptanceFacade NumericCallExactFacade
  ProjectExactFacade.
From LogosGenerated Require Import Schema Queries.
From FormalSQLLogical Require Import SqlTypes Schema Expr RelNode Eval.
From FormalSQLLogical.proof Require Import ValueCorrespond SlotMap Morphisms.
From Stdlib Require Import String List ZArith Nat Bool Lia Sorting.Permutation.
Import ListNotations.
Import Tuple.
Import NullValues.
Open Scope string_scope.
Open Scope Z_scope.

Require Import Q21Schema.
Require Import Q21LogicalSpecTree.
Require Import Q21FormalSQLProof.
Require Import Q21Qhat.
Require Import Q21LogicalNormalize.

Require Import Q21OuterOrdinarySpine Q21ExistsL2.

From Logos.FormalSQL Require Import SubqueryFacts MembershipJoinCompositionFacts.

(** ** Shapes and slot discipline *)

Definition q21_l3_pred_late : @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateGt
    [@SExpr_Leaf TNull relname type_date (DotDate "l_receiptdate");
     @SExpr_Leaf TNull relname type_date (DotDate "l_commitdate")].

Lemma q21_l3_conjlist_shape :
  Queries.scalar_expr_predicate_3 =
  SExpr_ConjList ([[]; ["b29"]; ["b30"; "b31"]])%list And_F
    [q21_l2_pred_orderkey; q21_l2_pred_suppkey; q21_l3_pred_late].
Proof. reflexivity. Qed.

(** RelNode: the join condition is the l2 one; late(l3) is a right-side
    RelFilter on the bare lineitem row (local slots 3 = receipt, 2 = commit). *)
Lemma Q21_not_exists_l3_slot_layout :
  pred_not_exists_l3 = pred_exists_l2 /\
  pred_not_exists_l3 =
    EBinary BAnd
      (EBinary BEq (EInputRef 3%nat) (EInputRef 11%nat))
      (EBinary BNe (EInputRef 4%nat) (EInputRef 12%nat)) /\
  scan_lineitem_late = RelFilter scan_lineitem pred_late_lineitem_local /\
  pred_late_lineitem_local = EBinary BGt (EInputRef 3%nat) (EInputRef 2%nat).
Proof. repeat split; reflexivity. Qed.

(** No alias capture: l3 exposes no [__logos_cor_*] attribute. *)
Lemma Q21_l3_no_capture :
  forall t3,
    labels TNull t3 =S= Fset.mk_set (A TNull) lineitem_formal_attrs ->
    (Attr_int32 "__logos_cor_cor0_7" inS? labels TNull t3) = false /\
    (Attr_int32 "__logos_cor_cor0_9" inS? labels TNull t3) = false.
Proof. exact Q21_l2_no_capture. Qed.

(** l3 orderkey/suppkey and both late dates resolve to the l3 tuple. *)
Lemma Q21_l3_inner_resolves :
  forall env t3,
    labels TNull t3 =S= Fset.mk_set (A TNull) lineitem_formal_attrs ->
    Interp.interp_dot TNull (Env.env_t TNull env t3) (Attr_int32 "l_orderkey") =
      dot TNull t3 (Attr_int32 "l_orderkey") /\
    Interp.interp_dot TNull (Env.env_t TNull env t3) (Attr_int32 "l_suppkey") =
      dot TNull t3 (Attr_int32 "l_suppkey") /\
    Interp.interp_dot TNull (Env.env_t TNull env t3) (Attr_date "l_receiptdate") =
      dot TNull t3 (Attr_date "l_receiptdate") /\
    Interp.interp_dot TNull (Env.env_t TNull env t3) (Attr_date "l_commitdate") =
      dot TNull t3 (Attr_date "l_commitdate").
Proof.
  intros env t3 Hl; repeat split; apply interp_dot_env_t; q21_attr_inS_of_labels Hl.
Qed.

(** Outer l1 orderkey/suppkey skip l3 and resolve to the left row. *)
Lemma Q21_l3_outer_resolves :
  forall ft t3,
    labels TNull t3 =S= Fset.mk_set (A TNull) lineitem_formal_attrs ->
    Attr_int32 "__logos_cor_cor0_7" inS labels TNull ft ->
    Attr_int32 "__logos_cor_cor0_9" inS labels TNull ft ->
    Interp.interp_dot TNull (Env.env_t TNull (Env.env_t TNull nil ft) t3)
      (Attr_int32 "__logos_cor_cor0_7") =
      dot TNull ft (Attr_int32 "__logos_cor_cor0_7") /\
    Interp.interp_dot TNull (Env.env_t TNull (Env.env_t TNull nil ft) t3)
      (Attr_int32 "__logos_cor_cor0_9") =
      dot TNull ft (Attr_int32 "__logos_cor_cor0_9").
Proof. exact Q21_l2_outer_resolves. Qed.

(** ** RelNode late keep on the bare l3 row *)

Definition q21_late_keep (nation_name : string) (r : Row) : bool :=
  q21_pkeep pred_late_lineitem_local nation_name r.

Lemma q21_late_keep_unfold :
  forall nation_name t3,
    q21_late_keep nation_name (rel_row_of slot_map_lineitem t3) =
    truth3_is_true
      (cmp_truth BGt (rel_of_value (dot TNull t3 (Attr_date "l_receiptdate")))
                     (rel_of_value (dot TNull t3 (Attr_date "l_commitdate")))).
Proof.
  intros nation_name t3.
  set (R := rel_row_of slot_map_lineitem t3).
  assert (H3 : nth_error R 3 =
    Some (rel_of_value (dot TNull t3 (Attr_date "l_receiptdate")))) by reflexivity.
  assert (H2 : nth_error R 2 =
    Some (rel_of_value (dot TNull t3 (Attr_date "l_commitdate")))) by reflexivity.
  unfold q21_late_keep, q21_pkeep; fold R.
  rewrite (proj2 (proj2 (proj2 Q21_not_exists_l3_slot_layout))).
  rewrite (eval_pred_keep_bgt_slots _ R 3 2 _ _ H3 H2).
  reflexivity.
Qed.

(** FormalSQL-side l3 match bit: late(l3) ∧ l2-style correlation. *)
Definition q21_l3_match (nation_name : string) (ft t3 : tuple TNull) : bool :=
  andb (q21_late_keep nation_name (rel_row_of slot_map_lineitem t3))
       (q21_l2_match nation_name ft t3).

(** ** Value-shape inversions *)

Lemma q21_is_int32_inv :
  forall v, q21_is_int32 v -> exists o, v = Value_int32 o.
Proof. intros v H; destruct v; simpl in H; try contradiction; eauto. Qed.

Lemma q21_is_date_inv :
  forall v, q21_is_date v -> exists o, v = Value_date o.
Proof. intros v H; destruct v; simpl in H; try contradiction; eauto. Qed.

Lemma vc_date_rel_of_value :
  forall o, value_correspond TDate (Value_date o) (rel_of_value (Value_date o)).
Proof. intros [d|]; constructor. Qed.

(** ** Milestone A: row-level l3 correlation *)

Section Q21NotExistsL3Row.
Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Local Abbreviation accept_at :=
  (@scalar_expr_acceptance_exact_at TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null boolean_schedule).

Local Abbreviation values_exact :=
  (@scalar_value_list_exact_at TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null boolean_schedule).

(** The correlated l3 ConjList is accepted in the nested environment at
    exactly: RelNode [pred_late_lineitem_local] keeps the bare l3 row AND
    RelNode [pred_not_exists_l3] keeps the concatenated outer ++ l3 row. *)
Theorem Q21_not_exists_l3_row_correlation :
  forall nation_name ft t3,
    q21_outer_cor_labels_ok ft ->
    q21_lineitem_member t3 ->
    accept_at (Env.env_t TNull (Env.env_t TNull nil ft) t3)
      Queries.scalar_expr_predicate_3
      (q21_l3_match nation_name ft t3) /\
    q21_l3_match nation_name ft t3 =
      andb (q21_pkeep pred_late_lineitem_local nation_name
              (rel_row_of slot_map_lineitem t3))
           (q21_pkeep pred_not_exists_l3 nation_name
              (List.app (rel_row_of slot_map_outer_cor ft)
                        (rel_row_of slot_map_lineitem t3))).
Proof.
  intros nation_name ft t3 Hlab [Hl3 [Sok [Ssk [Scd Srd]]]].
  split; [|reflexivity].
  destruct (labels_ok_suppkey ft Hlab) as [_ [Hl9 [_ Hs9]]].
  destruct (labels_ok_orderkey ft Hlab) as [_ [Hl7 [_ Hs7]]].
  destruct (Q21_l3_inner_resolves (Env.env_t TNull nil ft) t3 Hl3)
    as [Iok [Isk [Ird Icd]]].
  destruct (Q21_l3_outer_resolves ft t3 Hl3 Hl7 Hl9) as [O7 O9].
  set (env := Env.env_t TNull (Env.env_t TNull nil ft) t3).
  assert (Hv1 : values_exact env
      [@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_orderkey");
       @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_7")]
      [dot TNull t3 (Attr_int32 "l_orderkey");
       dot TNull ft (Attr_int32 "__logos_cor_cor0_7")]).
  { eapply scalar_value_list_exact_cons; [apply tnull_dot_int32_exact; exact Iok|].
    eapply scalar_value_list_exact_cons; [apply tnull_dot_int32_exact; exact O7|].
    apply scalar_value_list_exact_nil. }
  assert (Hv2 : values_exact env
      [@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_suppkey");
       @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_9")]
      [dot TNull t3 (Attr_int32 "l_suppkey");
       dot TNull ft (Attr_int32 "__logos_cor_cor0_9")]).
  { eapply scalar_value_list_exact_cons; [apply tnull_dot_int32_exact; exact Isk|].
    eapply scalar_value_list_exact_cons; [apply tnull_dot_int32_exact; exact O9|].
    apply scalar_value_list_exact_nil. }
  assert (Hv3 : values_exact env
      [@SExpr_Leaf TNull relname type_date (DotDate "l_receiptdate");
       @SExpr_Leaf TNull relname type_date (DotDate "l_commitdate")]
      [dot TNull t3 (Attr_date "l_receiptdate");
       dot TNull t3 (Attr_date "l_commitdate")]).
  { eapply scalar_value_list_exact_cons; [apply tnull_dot_date_exact; exact Ird|].
    eapply scalar_value_list_exact_cons; [apply tnull_dot_date_exact; exact Icd|].
    apply scalar_value_list_exact_nil. }
  destruct (q21_is_int32_inv _ Sok) as [ok Eok].
  destruct (q21_is_int32_inv _ Ssk) as [sk Esk].
  destruct (q21_is_date_inv _ Scd) as [cd Ecd].
  destruct (q21_is_date_inv _ Srd) as [rd Erd].
  destruct (q21_is_int32_inv _ Hs7) as [o7 E7].
  destruct (q21_is_int32_inv _ Hs9) as [o9 E9].
  rewrite Eok, E7 in Hv1; rewrite Esk, E9 in Hv2; rewrite Erd, Ecd in Hv3.
  pose proof (q21_pred_accept_of_values basesort instance boolean_schedule
                env PredicateEq _ _ Hv1) as A1.
  pose proof (q21_pred_accept_of_values basesort instance boolean_schedule
                env PredicateNeq _ _ Hv2) as A2.
  pose proof (q21_pred_accept_of_values basesort instance boolean_schedule
                env PredicateGt _ _ Hv3) as A3.
  pose proof (q21_int32_pair_keep _ _ _ _
             (vc_int32_rel_of_value ok) (vc_int32_rel_of_value o7)) as K1.
  pose proof (q21_int32_pair_neq_keep _ _ _ _
             (vc_int32_rel_of_value sk) (vc_int32_rel_of_value o9)) as K2.
  pose proof (q21_date_pair_keep _ _ _ _
             (vc_date_rel_of_value rd) (vc_date_rel_of_value cd)) as K3.
  unfold q21_l3_match.
  rewrite q21_late_keep_unfold, q21_l2_match_unfold, Eok, Esk, E7, E9, Ecd, Erd.
  rewrite (cmp_int_eq_sym (rel_of_value (Value_int32 o7))),
          (cmp_int_eq_sym (rel_of_value (Value_int32 o9))).
  set (b1 := truth3_is_true (cmp_int_eq (rel_of_value (Value_int32 ok))
                                        (rel_of_value (Value_int32 o7)))) in *.
  set (b2 := truth3_is_true (truth3_not
               (cmp_int_eq (rel_of_value (Value_int32 sk))
                           (rel_of_value (Value_int32 o9))))) in *.
  set (b3 := truth3_is_true (cmp_truth BGt (rel_of_value (Value_date rd))
                                           (rel_of_value (Value_date cd)))) in *.
  assert (A1' : accept_at env q21_l2_pred_orderkey b1)
    by (rewrite <- K1; exact A1).
  assert (A2' : accept_at env q21_l2_pred_suppkey b2)
    by (rewrite <- K2; exact A2).
  assert (A3' : accept_at env q21_l3_pred_late b3)
    by (rewrite <- K3; exact A3).
  rewrite q21_l3_conjlist_shape.
  replace (andb b3 (andb b1 b2)) with (scalar_acceptance_fold And_F [b1; b2; b3])
    by (simpl; destruct b1, b2, b3; reflexivity).
  set (decide := fun (e : @scalar_expr TNull relname ScalarResultBoolean) =>
    match e with
    | SExpr_Pred PredicateEq _ => b1
    | SExpr_Pred PredicateNeq _ => b2
    | SExpr_Pred PredicateGt _ => b3
    | _ => false
    end).
  change [b1; b2; b3]
    with (map decide [q21_l2_pred_orderkey; q21_l2_pred_suppkey; q21_l3_pred_late]).
  eapply (@scalar_expr_conj_list_acceptance_exact TNull relname
    basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null boolean_schedule ([[]; ["b29"]; ["b30"; "b31"]])%list env And_F
    [q21_l2_pred_orderkey; q21_l2_pred_suppkey; q21_l3_pred_late] decide).
  intros e Hin; simpl in Hin.
  destruct Hin as [<-|[<-|[<-|[]]]]; [exact A1'|exact A2'|exact A3'].
Qed.

End Q21NotExistsL3Row.

(** ** Milestone B: NOT EXISTS ↔ no matching late l3 *)

Definition q21_not_exists_l3_subquery : QueryExpr :=
  QExpr_Filter Queries.scalar_expr_predicate_3
    (@QExpr_Table TNull relname q21_lineitem_table_outputs (Rel "lineitem")).

Definition q21_not_exists_l3_sexpr : @scalar_expr TNull relname ScalarResultBoolean :=
  SExpr_Not (SExpr_Exists q21_not_exists_l3_subquery).

(** The l3 match bit depends on an l3 tuple only through its lineitem slots. *)
Lemma q21_rel_row_lineitem_proper :
  forall t t',
    Oeset.compare (OTuple TNull) t t' = Eq ->
    q21_lineitem_member t ->
    rel_row_of slot_map_lineitem t = rel_row_of slot_map_lineitem t'.
Proof.
  intros t t' Heq [L _].
  rewrite (tuple_eq TNull) in Heq; destruct Heq as [_ Hd].
  unfold rel_row_of, slot_map_lineitem; cbn [List.map sb_formal].
  rewrite <- !Hd by q21_attr_inS_of_labels L.
  reflexivity.
Qed.

Lemma q21_l3_match_proper :
  forall nation_name ft t3 t3',
    Oeset.compare (OTuple TNull) t3 t3' = Eq ->
    q21_lineitem_member t3 ->
    q21_l3_match nation_name ft t3 = q21_l3_match nation_name ft t3'.
Proof.
  intros nation_name ft t3 t3' Heq Hm.
  unfold q21_l3_match, q21_l2_match.
  rewrite (q21_rel_row_lineitem_proper _ _ Heq Hm); reflexivity.
Qed.

Section Q21LineitemTableExists.
Variables ss' ls' os' ns' : list (tuple TNull).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Local Abbreviation D := (D_of_embeds ss' ls' os' ns').

Local Abbreviation accept_at :=
  (@scalar_expr_acceptance_exact_at TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null boolean_schedule).

Local Abbreviation eval_exists :=
  (@eval_query_exists_outcome TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null boolean_schedule).

(** Q21 table-EXISTS helper: native EXISTS over [Filter formula (Table lineitem)]
    succeeds, is deterministic, never errors, and decides [existsb m ls']
    — independent of the bag enumeration of the table. *)
Lemma q21_lineitem_filter_exists_exact :
  forall env formula (m : tuple TNull -> bool),
    List.Forall q21_lineitem_member ls' ->
    (forall t t', Oeset.compare (OTuple TNull) t t' = Eq ->
       q21_lineitem_member t -> m t = m t') ->
    (forall t, q21_lineitem_member t ->
       accept_at (Env.env_t TNull env t) formula (m t)) ->
    eval_exists env
      (QExpr_Filter formula
         (@QExpr_Table TNull relname q21_lineitem_table_outputs (Rel "lineitem")))
      (SqlSuccess (if existsb m ls' then Bool.true (B TNull) else Bool.false (B TNull))) /\
    (forall outcome,
       eval_exists env
         (QExpr_Filter formula
            (@QExpr_Table TNull relname q21_lineitem_table_outputs (Rel "lineitem")))
         outcome ->
       outcome =
         SqlSuccess (if existsb m ls' then Bool.true (B TNull) else Bool.false (B TNull))).
Proof.
  intros env formula m Hmem Hprop Hacc.
  set (bag := @query_table_bag TNull relname
                (@_basesort TNull D) (@_instance TNull D)
                q21_lineitem_table_outputs (Rel "lineitem")).
  assert (Hls : query_same_rows_as_bag (T:=TNull) ls' bag)
    by apply q21_embed_table_same_rows_lineitem.
  assert (Hrows : forall rows,
    query_same_rows_as_bag (T:=TNull) rows bag ->
    existsb m rows = existsb m ls' /\
    (forall row, List.In row rows ->
       accept_at (Env.env_t TNull env row) formula (m row))).
  { intros rows Hsame.
    assert (Hm : List.Forall q21_lineitem_member rows)
      by exact (query_same_rows_as_bag_Forall_between _ ls' rows bag
                  q21_lineitem_member_proper Hls Hsame Hmem).
    split.
    - symmetry.
      eapply existsb_rel_permut_with_left_property
        with (R := fun x y => Oeset.compare (OTuple TNull) x y = Eq)
             (P := q21_lineitem_member).
      + intros x y Hxy Hx; apply Hprop; assumption.
      + exact (query_same_rows_as_bag_permut_between _ ls' rows bag Hls Hsame).
      + exact Hmem.
    - intros row Hin; rewrite Forall_forall in Hm; apply Hacc; exact (Hm row Hin). }
  split.
  - destruct (Hrows ls' Hls) as [_ Hacc'].
    eapply EExists_FilterRows; [apply EQuery_Table; exact Hls|].
    exact (proj2 (filter_exists_outcome_exact _ _ _ _ _ _ _ Hacc')).
  - intros outcome H; inversion H; subst;
      try (match goal with
           | Hq : query_exists_requires_rows _ = true |- _ => discriminate Hq
           | Hq : query_exists_uses_cardinality _ = true |- _ => discriminate Hq
           end).
    + match goal with
      | Ht : eval_query_expr_outcome _ _ _ _ _ _ _ _ (QExpr_Table _ _) (SqlError _) |- _ =>
          inversion Ht
      end.
    + match goal with
      | Ht : eval_query_expr_outcome _ _ _ _ _ _ _ _ (QExpr_Table _ _) (SqlSuccess ?rows),
        Hf : eval_filter_exists_outcome _ _ _ _ _ _ _ _ _ _ _ |- _ =>
          inversion Ht; subst;
          match goal with
          | Hs : query_same_rows_as_bag rows _ |- _ =>
              destruct (Hrows rows Hs) as [Hex Hacc'];
              rewrite (proj1 (filter_exists_outcome_exact _ _ _ _ _ _ _ Hacc') _ Hf), Hex;
              reflexivity
          end
      end.
Qed.

Lemma q21_exists_truth_negb :
  forall b : bool,
    (if b then Bool.true (B TNull) else Bool.false (B TNull)) =
    exists_truth_from_empty (negb b).
Proof. intros []; reflexivity. Qed.

(** Milestone B: NOT EXISTS(l3) is accepted at exactly
    [negb (existsb (q21_l3_match ft) ls')] (via Logos anti-join contract). *)
Theorem Q21_not_exists_l3_acceptance :
  forall nation_name ft,
    List.Forall q21_lineitem_member ls' ->
    q21_outer_cor_labels_ok ft ->
    accept_at (Env.env_t TNull nil ft) q21_not_exists_l3_sexpr
      (negb (existsb (q21_l3_match nation_name ft) ls')).
Proof.
  intros nation_name ft Hmem Hlab.
  destruct (q21_lineitem_filter_exists_exact (Env.env_t TNull nil ft)
              Queries.scalar_expr_predicate_3 (q21_l3_match nation_name ft) Hmem
              (fun t t' H Hm => q21_l3_match_proper nation_name ft t t' H Hm)
              (fun t Hm => proj1 (Q21_not_exists_l3_row_correlation
                                    _ _ boolean_schedule nation_name ft t Hlab Hm)))
    as [Hsucc Huniq].
  unfold q21_not_exists_l3_sexpr.
  apply scalar_expr_correlated_not_exists_antijoin_acceptance_exact.
  - eexists; exact Hsucc.
  - intros truth Htr; pose proof (Huniq _ Htr) as E; injection E as ->.
    apply q21_exists_truth_negb.
  - intros error Herr; pose proof (Huniq _ Herr) as E; discriminate E.
Qed.

(** Milestone C (FormalSQL): Filter(NOT EXISTS) keeps each outer occurrence
    exactly once iff no matching late l3 exists (Logos anti-join sources). *)
Theorem Q21_not_exists_l3_filter_rows :
  forall nation_name fts outcome,
    List.Forall q21_lineitem_member ls' ->
    List.Forall q21_outer_cor_labels_ok fts ->
    @eval_filter_rows_outcome TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      value_is_null boolean_schedule nil q21_not_exists_l3_sexpr fts outcome
    <->
    outcome =
      SqlSuccess
        (List.filter
           (fun ft => negb (existsb (q21_l3_match nation_name ft) ls')) fts).
Proof.
  intros nation_name fts outcome Hmem Hlabs.
  assert (Hexact :
    @correlated_filter_join_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      value_is_null boolean_schedule nil q21_not_exists_l3_sexpr ls'
      (q21_l3_match nation_name) negb fts).
  { intros left Hin; rewrite Forall_forall in Hlabs.
    apply Q21_not_exists_l3_acceptance; [exact Hmem|exact (Hlabs left Hin)]. }
  rewrite (eval_filter_rows_correlated_antijoin_sources_exact outcome Hexact).
  rewrite query_join_anti_sources_boolean_matrix, query_join_source_rows_left_map.
  reflexivity.
Qed.

End Q21LineitemTableExists.

(** ** Milestone C: RelNode anti-join and bag / multiplicity *)

Definition q21_rel_not_exists_l3_keep (nation_name : string) (ls : list Row)
    (rr : Row) : bool :=
  negb (existsb (fun r => q21_pkeep pred_not_exists_l3 nation_name (List.app rr r))
          (List.filter (q21_late_keep nation_name) ls)).

Theorem Q21_not_exists_l3_bag :
  forall nation_name fts rrs ls' ls,
    bag_correspond_unordered slot_map_outer_cor fts rrs ->
    List.Forall2 (row_correspond slot_map_lineitem) ls' ls ->
    bag_correspond_unordered slot_map_outer_cor
      (List.filter
         (fun ft => negb (existsb (q21_l3_match nation_name ft) ls')) fts)
      (List.filter (q21_rel_not_exists_l3_keep nation_name ls) rrs).
Proof.
  intros nation_name fts rrs ls' ls Hbag Hl.
  apply bag_correspond_unordered_filter; [exact Hbag|].
  intros ft rr Hrc.
  rewrite (row_correspond_rel_row_of _ _ _ slot_map_outer_cor_non_numeric Hrc).
  unfold q21_rel_not_exists_l3_keep.
  rewrite existsb_filter_andb.
  rewrite <- (Q21_lineitem_rows_of_embeds _ _ Hl), q21_existsb_map.
  reflexivity.
Qed.

Lemma eval_rel_relsemijoin_rows :
  forall l r c p db v,
    eval_rel (RelSemiJoin l r c) p db = Some v -> exists rs, v = RVRows rs.
Proof.
  intros l r c p db v H; cbn [eval_rel] in H.
  destruct (eval_rel l p db) as [[ls|]|], (eval_rel r p db) as [[rs|]|];
    try discriminate.
  destruct (join_semi_rows c p ls rs); try discriminate.
  injection H as <-; eauto.
Qed.

Theorem Q21_anti_not_exists_l3_eval :
  forall db nation_name ss ls os ns rows1,
    Preconditions_Q21 db nation_name ->
    db_lookup db table_supplier = Some ss ->
    db_lookup db table_lineitem = Some ls ->
    db_lookup db table_orders = Some os ->
    db_lookup db table_nation = Some ns ->
    Eval_Spec semi_exists_l2 (Params_Q21 nation_name) db = Some rows1 ->
    (forall rr, List.In rr rows1 -> List.length rr = outer_width) ->
    Eval_Spec anti_not_exists_l3 (Params_Q21 nation_name) db =
      Some (List.filter (q21_rel_not_exists_l3_keep nation_name ls) rows1).
Proof.
  intros db nation_name ss ls os ns rows1 [Hok _] Hs Hl Ho Hn Hev Hw.
  destruct (DB_ok_Q21_widths db ss ls os ns Hok Hs Hl Ho Hn) as [_ [Wl _]].
  assert (Hsemi : eval_rel semi_exists_l2 (Params_Q21 nation_name) db =
                  Some (RVRows rows1)).
  { unfold Eval_Spec in Hev.
    destruct (eval_rel semi_exists_l2 (Params_Q21 nation_name) db) as [v|] eqn:E;
      [|discriminate].
    destruct (eval_rel_relsemijoin_rows _ _ _ _ _ _ E) as [rs ->].
    injection Hev as ->; reflexivity. }
  assert (Hlate : eval_rel scan_lineitem_late (Params_Q21 nation_name) db =
                  Some (RVRows (List.filter (q21_late_keep nation_name) ls))).
  { unfold scan_lineitem_late, scan_lineitem; cbn [eval_rel]; rewrite Hl.
    rewrite (filter_rows_eq_List_filter _ _ _ (q21_late_keep nation_name)).
    - reflexivity.
    - intros r Hr; unfold q21_late_keep, pred_late_lineitem_local.
      apply q21_cmp_refs_total; [right; reflexivity| |];
        rewrite (Wl r Hr); lia. }
  unfold Eval_Spec, anti_not_exists_l3; cbn [eval_rel].
  rewrite Hsemi, Hlate.
  rewrite (join_anti_rows_eq_filter_negb_existsb _ _ _ _
             (q21_pkeep pred_not_exists_l3 nation_name)).
  - reflexivity.
  - intros lrow rrow Hlrow Hrrow.
    apply filter_In in Hrrow as [Hrrow _].
    assert (Hlen : (12 < List.length (List.app lrow rrow))%nat)
      by (rewrite List.length_app, (Hw lrow Hlrow), (Wl rrow Hrrow);
          unfold outer_width; lia).
    exact (q21_exists_l2_total nation_name _ Hlen).
Qed.

(** ** Milestone D: staged composition ordinary → EXISTS(l2) → NOT EXISTS(l3) *)

Lemma q21_not_exists_l3_is_catalog_conjunct :
  forall nn,
    match subst_nation_sexpr_bool nn Queries.scalar_expr_predicate_1 with
    | SExpr_ConjList _ _ [_; _; _; _; _; p5; _; _] => p5 = q21_not_exists_l3_sexpr
    | _ => False
    end.
Proof.
  intro nn.
  unfold Queries.scalar_expr_predicate_1; simpl.
  unfold subst_nation_sexpr_bool; simpl.
  unfold subst_nation_sexpr_val, subst_nation_aggterm; simpl.
  reflexivity.
Qed.

Definition q_not_exists_outer (nation_name : string) : QueryExpr :=
  QExpr_Filter q21_not_exists_l3_sexpr (q_exists_outer nation_name).

Definition Q21_NotExistsL3StageCorrespond_statement : Prop :=
  forall (db : DB) (nation_name : string),
    Preconditions_Q21 db nation_name ->
    exists (D : db_state) (fts : list (tuple TNull)) (rows : list Row),
      EmbedDB_Q21 db = Some D /\
      FormalSQL_exec D (q_not_exists_outer nation_name) fts /\
      Eval_Spec anti_not_exists_l3 (Params_Q21 nation_name) db = Some rows /\
      bag_correspond_unordered slot_map_outer_cor fts rows /\
      List.Forall q21_outer_cor_labels_ok fts.

Theorem Q21_NotExistsL3StageCorrespond :
  Q21_NotExistsL3StageCorrespond_statement.
Proof.
  unfold Q21_NotExistsL3StageCorrespond_statement.
  intros db nation_name Hpre.
  destruct (Q21_ExistsL2StageCorrespond db nation_name Hpre)
    as [D [fts1 [rows1 [HD [Hx [He [Hb Hlabs]]]]]]].
  pose proof HD as HD'.
  unfold EmbedDB_Q21 in HD'.
  destruct (db_lookup db table_supplier) as [ss|] eqn:Hs;
  destruct (db_lookup db table_lineitem) as [ls|] eqn:Hl;
  destruct (db_lookup db table_orders) as [os|] eqn:Ho;
  destruct (db_lookup db table_nation) as [ns|] eqn:Hn;
    try discriminate HD'.
  destruct (embed_rows EmbedRow_supplier ss) as [ss'|] eqn:Hes;
  destruct (embed_rows EmbedRow_lineitem ls) as [ls'|] eqn:Hel;
  destruct (embed_rows EmbedRow_orders os) as [os'|] eqn:Heo;
  destruct (embed_rows EmbedRow_nation ns) as [ns'|] eqn:Hen;
    try discriminate HD'.
  clear HD'.
  pose proof (EmbedDB_Q21_as_D_of_embeds db D ss ls os ns ss' ls' os' ns'
                HD Hs Hl Ho Hn Hes Hel Heo Hen) as HDe; subst D.
  pose proof (embed_rows_Forall_of EmbedRow_lineitem (fun _ => True)
    q21_lineitem_member ls ls'
    (fun r ft _ He => EmbedRow_lineitem_member r ft He)
    (proj2 (Forall_forall (fun _ => True) ls) (fun _ _ => I))
    Hel) as Ml.
  destruct (Q21_scan_bags db nation_name ss ls os ns ss' ls' os' ns'
              Hpre Hs Hl Ho Hn Hes Hel Heo Hen) as [_ [Bl _]].
  exists (D_of_embeds ss' ls' os' ns'),
    (List.filter
       (fun ft => negb (existsb (q21_l3_match nation_name ft) ls')) fts1),
    (List.filter (q21_rel_not_exists_l3_keep nation_name ls) rows1).
  split; [exact HD|].
  split; [|split; [|split]].
  - unfold FormalSQL_exec, eval_query_expr_possible_outcome in *.
    destruct Hx as [sched Hq].
    exists sched.
    unfold q_not_exists_outer.
    eapply EQuery_FilterRows; [exact Hq|].
    apply (proj2 (Q21_not_exists_l3_filter_rows ss' ls' os' ns' sched
                    nation_name fts1 _ Ml Hlabs)).
    reflexivity.
  - exact (Q21_anti_not_exists_l3_eval db nation_name ss ls os ns rows1
             Hpre Hs Hl Ho Hn He (q21_bag_rows_width _ _ Hb)).
  - exact (Q21_not_exists_l3_bag nation_name fts1 rows1 ls' ls Hb Bl).
  - apply Forall_forall; intros ft Hin.
    apply filter_In in Hin as [Hin _].
    rewrite Forall_forall in Hlabs; exact (Hlabs ft Hin).
Qed.
