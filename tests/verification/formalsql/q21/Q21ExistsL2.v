(* Q21ExistsL2 — correlated EXISTS(l2) layer over the Qed ordinary outer spine.

   RelNode:   semi_exists_l2 = RelSemiJoin filter_outer scan_lineitem pred_exists_l2
   FormalSQL: q_exists_outer = Filter (EXISTS (Filter [l_orderkey = cor0_7 ;
              l_suppkey <> cor0_9] (Table lineitem))) (q_ordinary_outer)

   Milestones:
     A  Q21_exists_l2_row_correlation   (row-level l2 predicate, nested env)
     B  Q21_exists_l2_acceptance        (EXISTS ↔ existsb over embedded l2)
     C  Q21_exists_l2_bag               (filter-by-existence, multiplicity)
     D  Q21_ExistsL2StageCorrespond     (composed with the ordinary spine)

   NOT EXISTS / GROUP BY / Project / Sort out of scope.
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

Require Import Q21OuterOrdinarySpine.

From SQLFS Require ListSort.

(** * EXISTS(l2) stage — Milestone A: row-level correlation

    Lowered subquery: [Filter [l_orderkey = cor0_7 ; l_suppkey <> cor0_9]
    (Table lineitem)], evaluated per l2 row in env
    [env_t (env_t nil ft) t2] (inner l2 tuple first, then outer row).
    RelNode: [pred_exists_l2 = BEq(3,11) ∧ BNe(4,12)] on [rr ++ r2]. *)

Definition q21_l2_pred_orderkey : @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateEq
    [@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_orderkey");
     @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_7")].

Definition q21_l2_pred_suppkey : @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateNeq
    [@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_suppkey");
     @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_9")].

Lemma q21_l2_conjlist_shape :
  Queries.scalar_expr_predicate_2 =
  SExpr_ConjList ([[]; ["b28"]])%list And_F
    [q21_l2_pred_orderkey; q21_l2_pred_suppkey].
Proof. reflexivity. Qed.

(** ** Correlation / slot discipline *)

(** Env lookup falls through an inner tuple that lacks the attribute. *)
Lemma interp_dot_env_t_skip :
  forall env (row : tuple TNull) attr,
    (attr inS? labels TNull row) = false ->
    Interp.interp_dot TNull (Env.env_t TNull env row) attr =
    Interp.interp_dot TNull env attr.
Proof.
  intros env row attr Hout.
  unfold Env.env_t.
  rewrite (Interp.interp_dot_unfold TNull (_ :: env) attr).
  rewrite (ListSort.quicksort_1 (OTuple TNull) row).
  rewrite Hout; reflexivity.
Qed.

(** No alias capture: the l2 table exposes no [__logos_cor_*] attribute. *)
Lemma Q21_l2_no_capture :
  forall t2,
    labels TNull t2 =S= Fset.mk_set (A TNull) lineitem_formal_attrs ->
    (Attr_int32 "__logos_cor_cor0_7" inS? labels TNull t2) = false /\
    (Attr_int32 "__logos_cor_cor0_9" inS? labels TNull t2) = false.
Proof.
  intros t2 Hl; split; q21_fresh_of_labels Hl.
Qed.

(** Inner [l_orderkey]/[l_suppkey] resolve to the l2 tuple (SemiJoin right). *)
Lemma Q21_l2_inner_resolves :
  forall env t2,
    labels TNull t2 =S= Fset.mk_set (A TNull) lineitem_formal_attrs ->
    Interp.interp_dot TNull (Env.env_t TNull env t2) (Attr_int32 "l_orderkey") =
      dot TNull t2 (Attr_int32 "l_orderkey") /\
    Interp.interp_dot TNull (Env.env_t TNull env t2) (Attr_int32 "l_suppkey") =
      dot TNull t2 (Attr_int32 "l_suppkey").
Proof.
  intros env t2 Hl; split; apply interp_dot_env_t; q21_attr_inS_of_labels Hl.
Qed.

(** Outer [cor0_7]/[cor0_9] skip l2 and resolve to the outer l1 row. *)
Lemma Q21_l2_outer_resolves :
  forall ft t2,
    labels TNull t2 =S= Fset.mk_set (A TNull) lineitem_formal_attrs ->
    Attr_int32 "__logos_cor_cor0_7" inS labels TNull ft ->
    Attr_int32 "__logos_cor_cor0_9" inS labels TNull ft ->
    Interp.interp_dot TNull (Env.env_t TNull (Env.env_t TNull nil ft) t2)
      (Attr_int32 "__logos_cor_cor0_7") =
      dot TNull ft (Attr_int32 "__logos_cor_cor0_7") /\
    Interp.interp_dot TNull (Env.env_t TNull (Env.env_t TNull nil ft) t2)
      (Attr_int32 "__logos_cor_cor0_9") =
      dot TNull ft (Attr_int32 "__logos_cor_cor0_9").
Proof.
  intros ft t2 Hl H7 H9.
  destruct (Q21_l2_no_capture t2 Hl) as [N7 N9].
  split; rewrite interp_dot_env_t_skip by assumption;
    apply interp_dot_env_t; assumption.
Qed.

(** RelNode concatenated layout: outer slots 3/4 = l1 orderkey/suppkey,
    right slots 11/12 = l2 orderkey/suppkey (outer width 11). *)
Lemma Q21_exists_l2_slot_layout :
  pred_exists_l2 =
    EBinary BAnd
      (EBinary BEq (EInputRef 3%nat) (EInputRef 11%nat))
      (EBinary BNe (EInputRef 4%nat) (EInputRef 12%nat)) /\
  slot_l1_orderkey = 3%nat /\ slot_l1_suppkey = 4%nat /\
  slot_r_orderkey = (outer_width + 0)%nat /\
  slot_r_suppkey = (outer_width + 1)%nat.
Proof. repeat split; reflexivity. Qed.

Lemma Q21_exists_l2_concat_slots :
  forall (rr r2 : Row),
    List.length rr = outer_width ->
    nth_error (List.app rr r2) 3 = nth_error rr 3 /\
    nth_error (List.app rr r2) 4 = nth_error rr 4 /\
    nth_error (List.app rr r2) 11 = nth_error r2 0 /\
    nth_error (List.app rr r2) 12 = nth_error r2 1.
Proof.
  intros rr r2 Hlen; unfold outer_width in Hlen.
  repeat split.
  - apply nth_error_app1; lia.
  - apply nth_error_app1; lia.
  - rewrite nth_error_app2 by lia; rewrite Hlen; reflexivity.
  - rewrite nth_error_app2 by lia; rewrite Hlen; reflexivity.
Qed.

(** ** Value-level bridges *)

Lemma vc_int32_rel_of_value :
  forall o, value_correspond TInt (Value_int32 o) (rel_of_value (Value_int32 o)).
Proof. intros [i|]; constructor. Qed.

Lemma q21_int32_pair_neq_keep :
  forall o1 o2 rv1 rv2,
    value_correspond TInt (Value_int32 o1) rv1 ->
    value_correspond TInt (Value_int32 o2) rv2 ->
    Bool.is_true Bool3
      (interp_predicate PredicateNeq [Value_int32 o1; Value_int32 o2]) =
    truth3_is_true (truth3_not (cmp_int_eq rv1 rv2)).
Proof.
  intros o1 o2 rv1 rv2 H1 H2.
  inversion H1; subst; inversion H2; subst; try reflexivity.
  pose proof (NullPredicates.order_value_compare_integral
                (Value_int32 (Some i)) (Value_int32 (Some i0))
                (int32_value i) (int32_value i0) eq_refl eq_refl) as Hord.
  rewrite (interp_predicate_neq_of_order_compare _ _ _ Hord).
  unfold cmp_int_eq; simpl.
  destruct (Z.compare_spec (int32_value i) (int32_value i0)) as [He|Hlt|Hgt].
  - rewrite He, Z.eqb_refl; reflexivity.
  - assert (Z.eqb (int32_value i) (int32_value i0) = false) as ->
      by (apply Z.eqb_neq; lia); reflexivity.
  - assert (Z.eqb (int32_value i) (int32_value i0) = false) as ->
      by (apply Z.eqb_neq; lia); reflexivity.
Qed.

Lemma eval_pred_keep_bne_slots :
  forall (p : Params) (r : Row) (i j : nat) (vi vj : SqlValue),
    nth_error r i = Some vi ->
    nth_error r j = Some vj ->
    eval_pred_keep (EBinary BNe (EInputRef i) (EInputRef j)) p r =
    Some (truth3_is_true (truth3_not (cmp_int_eq vi vj))).
Proof.
  intros p r i j vi vj Hi Hj.
  unfold eval_pred_keep, eval_pred, eval_expr, ctx_lookup_input, row_nth.
  rewrite Hi, Hj; simpl.
  destruct (cmp_int_eq vi vj); reflexivity.
Qed.

(** FormalSQL-side match bit: RelNode [pred_exists_l2] on the rows determined
    by the watched outer cor slots and the l2 lineitem slots. *)
Definition q21_l2_match (nation_name : string) (ft t2 : tuple TNull) : bool :=
  q21_pkeep pred_exists_l2 nation_name
    (List.app (rel_row_of slot_map_outer_cor ft) (rel_row_of slot_map_lineitem t2)).

Lemma q21_l2_match_unfold :
  forall nation_name ft t2,
    q21_l2_match nation_name ft t2 =
    andb
      (truth3_is_true
         (cmp_int_eq (rel_of_value (dot TNull ft (Attr_int32 "__logos_cor_cor0_7")))
                     (rel_of_value (dot TNull t2 (Attr_int32 "l_orderkey")))))
      (truth3_is_true (truth3_not
         (cmp_int_eq (rel_of_value (dot TNull ft (Attr_int32 "__logos_cor_cor0_9")))
                     (rel_of_value (dot TNull t2 (Attr_int32 "l_suppkey")))))).
Proof.
  intros nation_name ft t2.
  set (R := List.app (rel_row_of slot_map_outer_cor ft)
                     (rel_row_of slot_map_lineitem t2)).
  assert (H3 : nth_error R 3 =
    Some (rel_of_value (dot TNull ft (Attr_int32 "__logos_cor_cor0_7"))))
    by reflexivity.
  assert (H11 : nth_error R 11 =
    Some (rel_of_value (dot TNull t2 (Attr_int32 "l_orderkey"))))
    by reflexivity.
  assert (H4 : nth_error R 4 =
    Some (rel_of_value (dot TNull ft (Attr_int32 "__logos_cor_cor0_9"))))
    by reflexivity.
  assert (H12 : nth_error R 12 =
    Some (rel_of_value (dot TNull t2 (Attr_int32 "l_suppkey"))))
    by reflexivity.
  unfold q21_l2_match, q21_pkeep; fold R.
  rewrite (proj1 Q21_exists_l2_slot_layout).
  rewrite (eval_pred_keep_band _ _ _ _ _ _
             (eval_pred_keep_beq_slots _ R 3 11 _ _ H3 H11)
             (eval_pred_keep_bne_slots _ R 4 12 _ _ H4 H12)).
  reflexivity.
Qed.

Section Q21ExistsL2Row.
Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Local Abbreviation accept_at :=
  (@scalar_expr_acceptance_exact_at TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null boolean_schedule).

(** Milestone A: the correlated l2 ConjList is accepted in the nested
    environment at exactly the RelNode [pred_exists_l2] bit. *)
Theorem Q21_exists_l2_row_correlation :
  forall nation_name ft t2,
    q21_outer_cor_labels_ok ft ->
    q21_lineitem_member t2 ->
    accept_at (Env.env_t TNull (Env.env_t TNull nil ft) t2)
      Queries.scalar_expr_predicate_2
      (q21_l2_match nation_name ft t2).
Proof.
  intros nation_name ft t2 Hlab [Hl2 [Sok [Ssk _]]].
  destruct (labels_ok_suppkey ft Hlab) as [_ [Hl9 [_ Hs9]]].
  destruct (labels_ok_orderkey ft Hlab) as [_ [Hl7 [_ Hs7]]].
  destruct (Q21_l2_inner_resolves (Env.env_t TNull nil ft) t2 Hl2) as [Iok Isk].
  destruct (Q21_l2_outer_resolves ft t2 Hl2 Hl7 Hl9) as [O7 O9].
  set (env := Env.env_t TNull (Env.env_t TNull nil ft) t2).
  assert (Hv1 : @scalar_value_list_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule env
      [@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_orderkey");
       @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_7")]
      [dot TNull t2 (Attr_int32 "l_orderkey");
       dot TNull ft (Attr_int32 "__logos_cor_cor0_7")]).
  { eapply scalar_value_list_exact_cons; [apply tnull_dot_int32_exact; exact Iok|].
    eapply scalar_value_list_exact_cons; [apply tnull_dot_int32_exact; exact O7|].
    apply scalar_value_list_exact_nil. }
  assert (Hv2 : @scalar_value_list_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule env
      [@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_suppkey");
       @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_9")]
      [dot TNull t2 (Attr_int32 "l_suppkey");
       dot TNull ft (Attr_int32 "__logos_cor_cor0_9")]).
  { eapply scalar_value_list_exact_cons; [apply tnull_dot_int32_exact; exact Isk|].
    eapply scalar_value_list_exact_cons; [apply tnull_dot_int32_exact; exact O9|].
    apply scalar_value_list_exact_nil. }
  unfold q21_is_int32 in Sok, Ssk.
  destruct (dot TNull t2 (Attr_int32 "l_orderkey")) as [ | | ok | | | | | | | | | ]
    eqn:Eok; try (exfalso; exact Sok).
  destruct (dot TNull t2 (Attr_int32 "l_suppkey")) as [ | | sk | | | | | | | | | ]
    eqn:Esk; try (exfalso; exact Ssk).
  destruct (dot TNull ft (Attr_int32 "__logos_cor_cor0_7")) as [ | | o7 | | | | | | | | | ]
    eqn:E7; try (exfalso; exact Hs7).
  destruct (dot TNull ft (Attr_int32 "__logos_cor_cor0_9")) as [ | | o9 | | | | | | | | | ]
    eqn:E9; try (exfalso; exact Hs9).
  rewrite ?Eok, ?E7 in Hv1; rewrite ?Esk, ?E9 in Hv2.
  pose proof (q21_pred_accept_of_values basesort instance boolean_schedule
                env PredicateEq _ _ Hv1) as A1.
  pose proof (q21_pred_accept_of_values basesort instance boolean_schedule
                env PredicateNeq _ _ Hv2) as A2.
  pose proof (q21_int32_pair_keep _ _ _ _
             (vc_int32_rel_of_value ok) (vc_int32_rel_of_value o7)) as K1.
  pose proof (q21_int32_pair_neq_keep _ _ _ _
             (vc_int32_rel_of_value sk) (vc_int32_rel_of_value o9)) as K2.
  rewrite q21_l2_match_unfold, Eok, Esk, E7, E9.
  rewrite (cmp_int_eq_sym (rel_of_value (Value_int32 o7))),
          (cmp_int_eq_sym (rel_of_value (Value_int32 o9))).
  set (b1 := truth3_is_true (cmp_int_eq (rel_of_value (Value_int32 ok))
                                        (rel_of_value (Value_int32 o7)))) in *.
  set (b2 := truth3_is_true (truth3_not
               (cmp_int_eq (rel_of_value (Value_int32 sk))
                           (rel_of_value (Value_int32 o9))))) in *.
  assert (A1' : accept_at env q21_l2_pred_orderkey b1)
    by (rewrite <- K1; exact A1).
  assert (A2' : accept_at env q21_l2_pred_suppkey b2)
    by (rewrite <- K2; exact A2).
  rewrite q21_l2_conjlist_shape.
  replace (andb b1 b2) with (scalar_acceptance_fold And_F [b1; b2])
    by (simpl; rewrite Bool.andb_true_r; reflexivity).
  set (decide := fun (e : @scalar_expr TNull relname ScalarResultBoolean) =>
    match e with
    | SExpr_Pred PredicateEq _ => b1
    | SExpr_Pred PredicateNeq _ => b2
    | _ => false
    end).
  change [b1; b2] with (map decide [q21_l2_pred_orderkey; q21_l2_pred_suppkey]).
  eapply (@scalar_expr_conj_list_acceptance_exact TNull relname
    basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null boolean_schedule ([[]; ["b28"]])%list env And_F
    [q21_l2_pred_orderkey; q21_l2_pred_suppkey] decide).
  intros e Hin; simpl in Hin.
  destruct Hin as [<-|[<-|[]]]; [exact A1'|exact A2'].
Qed.

End Q21ExistsL2Row.

(** * EXISTS(l2) stage — Milestone B: EXISTS ↔ existence of a matching l2 *)

From Logos.FormalSQL Require Import SubqueryFacts MembershipJoinCompositionFacts.

(** The lowered EXISTS subquery (verbatim shape of the catalog conjunct). *)
Definition q21_exists_l2_subquery : QueryExpr :=
  QExpr_Filter Queries.scalar_expr_predicate_2
    (@QExpr_Table TNull relname q21_lineitem_table_outputs (Rel "lineitem")).

Definition q21_exists_l2_sexpr : @scalar_expr TNull relname ScalarResultBoolean :=
  SExpr_Exists q21_exists_l2_subquery.

Lemma q21_is_true_bool3 :
  forall b : bool,
    Bool.is_true (B TNull) (if b then Bool.true (B TNull) else Bool.false (B TNull)) = b.
Proof. intros []; reflexivity. Qed.

Section Q21ExistsL2Accept.
Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Local Abbreviation accept_at :=
  (@scalar_expr_acceptance_exact_at TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null boolean_schedule).

Local Abbreviation eval_filter_exists :=
  (@eval_filter_exists_outcome TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null boolean_schedule).

Local Abbreviation eval_exists :=
  (@eval_query_exists_outcome TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null boolean_schedule).

Local Abbreviation eval_query :=
  (@eval_query_expr_outcome TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null boolean_schedule).

(** Generic: the short-circuit EXISTS row scan decides [existsb] of the
    per-row exact acceptance bits, and never errors. *)
Lemma filter_exists_outcome_exact :
  forall env formula rows (decide : tuple TNull -> bool),
    (forall row, List.In row rows ->
       accept_at (Env.env_t TNull env row) formula (decide row)) ->
    (forall outcome,
       eval_filter_exists env formula rows outcome ->
       outcome = SqlSuccess
         (if existsb decide rows then Bool.true (B TNull) else Bool.false (B TNull))) /\
    eval_filter_exists env formula rows
      (SqlSuccess
         (if existsb decide rows then Bool.true (B TNull) else Bool.false (B TNull))).
Proof.
  intros env formula rows decide; induction rows as [|row rows IH]; intros Hacc.
  - split; [intros outcome H; inversion H; reflexivity|constructor].
  - destruct (Hacc row (or_introl eq_refl)) as [[tr [Htr Hbit]] [Huniq Hnoerr]].
    destruct IH as [IHu IHe]; [intros r Hr; apply Hacc; right; exact Hr|].
    split.
    + intros outcome H; inversion H; subst.
      * exfalso; eapply Hnoerr; eassumption.
      * match goal with
        | Hs : eval_scalar_boolean_expr_outcome _ _ _ _ _ _ _ _ _ (SqlSuccess ?t),
          Ht : Bool.is_true _ ?t = true |- _ =>
            pose proof (Huniq t Hs) as Hd; rewrite Ht in Hd
        end.
        simpl; rewrite <- Hd; reflexivity.
      * match goal with
        | Hs : eval_scalar_boolean_expr_outcome _ _ _ _ _ _ _ _ _ (SqlSuccess ?t),
          Ht : Bool.is_true _ ?t = false,
          Hr : eval_filter_exists_outcome _ _ _ _ _ _ _ _ _ _ _ |- _ =>
            pose proof (Huniq t Hs) as Hd; rewrite Ht in Hd;
            rewrite (IHu _ Hr)
        end.
        simpl; rewrite <- Hd; reflexivity.
    + simpl; destruct (decide row) eqn:Hdr.
      * eapply EFilterExists_HeadTrue; [exact Htr|congruence].
      * eapply EFilterExists_HeadFalse; [exact Htr|congruence|exact IHe].
Qed.

End Q21ExistsL2Accept.

(** Tuple-equivalence properness (table rows are a bag up to [=t=]). *)
Lemma q21_lineitem_member_transport :
  forall t t',
    Oeset.compare (OTuple TNull) t t' = Eq ->
    q21_lineitem_member t -> q21_lineitem_member t'.
Proof.
  intros t t' Heq [L [S0 [S1 [S2 S3]]]].
  rewrite (tuple_eq TNull) in Heq; destruct Heq as [Hl Hd].
  assert (L' : labels TNull t' =S= Fset.mk_set (A TNull) lineitem_formal_attrs).
  { rewrite <- (Fset.equal_eq_1 _ _ _ _ Hl); exact L. }
  unfold q21_lineitem_member.
  rewrite <- !Hd by q21_attr_inS_of_labels L.
  repeat split; assumption.
Qed.

Lemma q21_lineitem_member_proper :
  tuple_property_proper q21_lineitem_member.
Proof.
  intros t t' Heq; split; [apply q21_lineitem_member_transport; exact Heq|].
  apply q21_lineitem_member_transport.
  rewrite Oeset.compare_lt_gt, Heq; reflexivity.
Qed.

Lemma q21_l2_match_proper :
  forall nation_name ft t2 t2',
    Oeset.compare (OTuple TNull) t2 t2' = Eq ->
    q21_lineitem_member t2 ->
    q21_l2_match nation_name ft t2 = q21_l2_match nation_name ft t2'.
Proof.
  intros nation_name ft t2 t2' Heq [L _].
  rewrite (tuple_eq TNull) in Heq; destruct Heq as [_ Hd].
  rewrite !q21_l2_match_unfold.
  rewrite <- !Hd by q21_attr_inS_of_labels L.
  reflexivity.
Qed.

Section Q21ExistsL2Embedded.
Variables ss' ls' os' ns' : list (tuple TNull).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Local Abbreviation D := (D_of_embeds ss' ls' os' ns').

Local Abbreviation accept_at :=
  (@scalar_expr_acceptance_exact_at TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null boolean_schedule).

(** Milestone B: the correlated EXISTS(l2) conjunct is accepted at exactly
    [existsb (q21_l2_match ft) ls'] — existence of a matching embedded l2 row,
    independent of the bag enumeration of [Table lineitem]. *)
Theorem Q21_exists_l2_acceptance :
  forall nation_name ft,
    List.Forall q21_lineitem_member ls' ->
    q21_outer_cor_labels_ok ft ->
    accept_at (Env.env_t TNull nil ft) q21_exists_l2_sexpr
      (existsb (q21_l2_match nation_name ft) ls').
Proof.
  intros nation_name ft Hmem Hlab.
  set (env := Env.env_t TNull nil ft).
  set (bag := @query_table_bag TNull relname
                (@_basesort TNull D) (@_instance TNull D)
                q21_lineitem_table_outputs (Rel "lineitem")).
  assert (Hls : query_same_rows_as_bag (T:=TNull) ls' bag)
    by apply q21_embed_table_same_rows_lineitem.
  (* per-row exactness on any legal enumeration of the l2 table *)
  assert (Hrows : forall rows,
    query_same_rows_as_bag (T:=TNull) rows bag ->
    List.Forall q21_lineitem_member rows /\
    existsb (q21_l2_match nation_name ft) rows =
      existsb (q21_l2_match nation_name ft) ls' /\
    (forall row, List.In row rows ->
       accept_at (Env.env_t TNull env row) Queries.scalar_expr_predicate_2
         (q21_l2_match nation_name ft row))).
  { intros rows Hsame.
    assert (Hm : List.Forall q21_lineitem_member rows)
      by exact (query_same_rows_as_bag_Forall_between _ ls' rows bag
                  q21_lineitem_member_proper Hls Hsame Hmem).
    split; [exact Hm|split].
    - symmetry.
      eapply existsb_rel_permut_with_left_property
        with (R := fun x y => Oeset.compare (OTuple TNull) x y = Eq)
             (P := q21_lineitem_member).
      + intros x y Hxy Hx; apply q21_l2_match_proper; assumption.
      + exact (query_same_rows_as_bag_permut_between _ ls' rows bag Hls Hsame).
      + exact Hmem.
    - intros row Hin; rewrite Forall_forall in Hm.
      apply Q21_exists_l2_row_correlation; [exact Hlab|exact (Hm row Hin)]. }
  unfold q21_exists_l2_sexpr.
  apply scalar_expr_correlated_exists_semijoin_acceptance_exact.
  - (* success exists *)
    destruct (Hrows ls' Hls) as [_ [_ Hacc]].
    eexists.
    eapply EExists_FilterRows; [apply EQuery_Table; exact Hls|].
    exact (proj2 (filter_exists_outcome_exact _ _ _ _ _ _ _ Hacc)).
  - (* every success decides existsb over ls' *)
    intros truth H; inversion H; subst;
      try (match goal with
           | Hq : query_exists_requires_rows _ = true |- _ => discriminate Hq
           | Hq : query_exists_uses_cardinality _ = true |- _ => discriminate Hq
           end).
    match goal with
    | Ht : eval_query_expr_outcome _ _ _ _ _ _ _ _ (QExpr_Table _ _) (SqlSuccess ?rows),
      Hf : eval_filter_exists_outcome _ _ _ _ _ _ _ _ _ _ _ |- _ =>
        inversion Ht; subst;
        match goal with
        | Hs : query_same_rows_as_bag rows _ |- _ =>
            destruct (Hrows rows Hs) as [_ [Hex Hacc]];
            pose proof (proj1 (filter_exists_outcome_exact _ _ _ _ _ _ _ Hacc) _ Hf)
              as Hout
        end
    end.
    injection Hout as ->.
    rewrite q21_is_true_bool3; exact Hex.
  - (* no error *)
    intros error H; inversion H; subst;
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
              destruct (Hrows rows Hs) as [_ [_ Hacc]];
              pose proof (proj1 (filter_exists_outcome_exact _ _ _ _ _ _ _ Hacc) _ Hf)
                as Hout; discriminate Hout
          end
      end.
Qed.

End Q21ExistsL2Embedded.

(** * EXISTS(l2) stage — Milestone C: bag / multiplicity *)

(** FormalSQL: Filter(EXISTS l2) over the outer rows is the occurrence-
    preserving semi-join filter (via Logos correlated semi-join sources). *)
Section Q21ExistsL2FilterRows.
Variables ss' ls' os' ns' : list (tuple TNull).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Local Abbreviation D := (D_of_embeds ss' ls' os' ns').

Theorem Q21_exists_l2_filter_rows :
  forall nation_name fts outcome,
    List.Forall q21_lineitem_member ls' ->
    List.Forall q21_outer_cor_labels_ok fts ->
    @eval_filter_rows_outcome TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      value_is_null boolean_schedule nil q21_exists_l2_sexpr fts outcome
    <->
    outcome =
      SqlSuccess
        (List.filter (fun ft => existsb (q21_l2_match nation_name ft) ls') fts).
Proof.
  intros nation_name fts outcome Hmem Hlabs.
  assert (Hexact :
    @correlated_filter_join_acceptance_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      value_is_null boolean_schedule nil q21_exists_l2_sexpr ls'
      (q21_l2_match nation_name) (fun matched => matched) fts).
  { intros left Hin; rewrite Forall_forall in Hlabs.
    apply Q21_exists_l2_acceptance; [exact Hmem|exact (Hlabs left Hin)]. }
  rewrite (eval_filter_rows_correlated_semijoin_sources_exact outcome Hexact).
  rewrite query_join_semi_sources_boolean_matrix, query_join_source_rows_left_map.
  reflexivity.
Qed.

End Q21ExistsL2FilterRows.

Lemma q21_existsb_map :
  forall (X Y : Type) (f : Y -> bool) (g : X -> Y) (l : list X),
    existsb f (List.map g l) = existsb (fun x => f (g x)) l.
Proof. intros X Y f g l; induction l as [|x l IH]; simpl; [reflexivity|rewrite IH; reflexivity]. Qed.

Lemma slot_map_lineitem_non_numeric : slot_map_non_numeric slot_map_lineitem.
Proof.
  unfold slot_map_non_numeric, slot_map_lineitem.
  repeat constructor; intros scale H; discriminate H.
Qed.

(** The embedded l2 table determines the RelNode lineitem scan row-by-row. *)
Lemma Q21_lineitem_rows_of_embeds :
  forall ls' ls,
    List.Forall2 (row_correspond slot_map_lineitem) ls' ls ->
    List.map (rel_row_of slot_map_lineitem) ls' = ls.
Proof.
  intros ls' ls H; induction H as [|t r ts rs Hr Hs IH]; [reflexivity|].
  cbn [List.map]; f_equal; [|exact IH].
  symmetry; exact (row_correspond_rel_row_of _ _ _ slot_map_lineitem_non_numeric Hr).
Qed.

Definition q21_rel_exists_l2_keep (nation_name : string) (ls : list Row)
    (rr : Row) : bool :=
  existsb (fun r => q21_pkeep pred_exists_l2 nation_name (List.app rr r)) ls.

(** Milestone C: EXISTS filtering preserves unordered bag correspondence.
    Both sides are [List.filter] over the outer bag by an existence bit, so
    each qualifying outer occurrence survives exactly once, non-qualifying
    occurrences are removed, and duplicates keep their multiplicity. *)
Theorem Q21_exists_l2_bag :
  forall nation_name fts rrs ls' ls,
    bag_correspond_unordered slot_map_outer_cor fts rrs ->
    List.Forall2 (row_correspond slot_map_lineitem) ls' ls ->
    bag_correspond_unordered slot_map_outer_cor
      (List.filter (fun ft => existsb (q21_l2_match nation_name ft) ls') fts)
      (List.filter (q21_rel_exists_l2_keep nation_name ls) rrs).
Proof.
  intros nation_name fts rrs ls' ls Hbag Hl.
  apply bag_correspond_unordered_filter; [exact Hbag|].
  intros ft rr Hrc.
  rewrite (row_correspond_rel_row_of _ _ _ slot_map_outer_cor_non_numeric Hrc).
  unfold q21_rel_exists_l2_keep.
  rewrite <- (Q21_lineitem_rows_of_embeds _ _ Hl), q21_existsb_map.
  reflexivity.
Qed.

(** RelNode: [semi_exists_l2] evaluates to the same existence filter. *)
Lemma eval_rel_relfilter_rows :
  forall input pr p db v,
    eval_rel (RelFilter input pr) p db = Some v -> exists rs, v = RVRows rs.
Proof.
  intros input pr p db v H; cbn [eval_rel] in H.
  destruct (eval_rel input p db) as [[rs|bags]|]; try discriminate.
  destruct (filter_rows pr p rs); try discriminate.
  injection H as <-; eauto.
Qed.

Lemma nth_error_lt_Some :
  forall (X : Type) (l : list X) i, (i < List.length l)%nat -> exists v, nth_error l i = Some v.
Proof.
  intros X l i Hi; destruct (nth_error l i) eqn:E; [eauto|].
  apply nth_error_None in E; lia.
Qed.

Lemma q21_exists_l2_total :
  forall nation_name (R : Row),
    (12 < List.length R)%nat ->
    eval_pred_keep pred_exists_l2 (Params_Q21 nation_name) R =
      Some (q21_pkeep pred_exists_l2 nation_name R).
Proof.
  intros nation_name R Hlen.
  destruct (nth_error_lt_Some _ R 3 ltac:(lia)) as [v3 H3].
  destruct (nth_error_lt_Some _ R 11 ltac:(lia)) as [v11 H11].
  destruct (nth_error_lt_Some _ R 4 ltac:(lia)) as [v4 H4].
  destruct (nth_error_lt_Some _ R 12 ltac:(lia)) as [v12 H12].
  eapply q21_pkeep_of_Some.
  rewrite (proj1 Q21_exists_l2_slot_layout).
  exact (eval_pred_keep_band _ _ _ _ _ _
           (eval_pred_keep_beq_slots _ R 3 11 _ _ H3 H11)
           (eval_pred_keep_bne_slots _ R 4 12 _ _ H4 H12)).
Qed.

Theorem Q21_semi_exists_l2_eval :
  forall db nation_name ss ls os ns rows0,
    Preconditions_Q21 db nation_name ->
    db_lookup db table_supplier = Some ss ->
    db_lookup db table_lineitem = Some ls ->
    db_lookup db table_orders = Some os ->
    db_lookup db table_nation = Some ns ->
    Eval_Spec filter_outer (Params_Q21 nation_name) db = Some rows0 ->
    (forall rr, List.In rr rows0 -> List.length rr = outer_width) ->
    Eval_Spec semi_exists_l2 (Params_Q21 nation_name) db =
      Some (List.filter (q21_rel_exists_l2_keep nation_name ls) rows0).
Proof.
  intros db nation_name ss ls os ns rows0 [Hok _] Hs Hl Ho Hn Hev Hw.
  destruct (DB_ok_Q21_widths db ss ls os ns Hok Hs Hl Ho Hn) as [_ [Wl _]].
  assert (Hf : eval_rel filter_outer (Params_Q21 nation_name) db =
               Some (RVRows rows0)).
  { unfold Eval_Spec in Hev.
    destruct (eval_rel filter_outer (Params_Q21 nation_name) db) as [v|] eqn:E;
      [|discriminate].
    destruct (eval_rel_relfilter_rows _ _ _ _ _ E) as [rs ->].
    injection Hev as ->; reflexivity. }
  assert (Hr : eval_rel scan_lineitem (Params_Q21 nation_name) db =
               Some (RVRows ls))
    by (unfold scan_lineitem; cbn [eval_rel]; rewrite Hl; reflexivity).
  unfold Eval_Spec, semi_exists_l2; cbn [eval_rel].
  rewrite Hf, Hr.
  rewrite (join_semi_rows_eq_filter_existsb _ _ _ _
             (q21_pkeep pred_exists_l2 nation_name)).
  - reflexivity.
  - intros lrow rrow Hlrow Hrrow.
    apply q21_exists_l2_total.
    rewrite List.length_app, (Hw lrow Hlrow), (Wl rrow Hrrow).
    unfold outer_width; lia.
Qed.

(** * EXISTS(l2) stage — Milestone D: compose with the ordinary spine *)

(** The EXISTS conjunct is taken verbatim from the lowered catalog predicate. *)
Lemma q21_exists_l2_is_catalog_conjunct :
  forall nn,
    match subst_nation_sexpr_bool nn Queries.scalar_expr_predicate_1 with
    | SExpr_ConjList _ _ [_; _; _; _; p4; _; _; _] => p4 = q21_exists_l2_sexpr
    | _ => False
    end.
Proof.
  intro nn.
  unfold Queries.scalar_expr_predicate_1; simpl.
  unfold subst_nation_sexpr_bool; simpl.
  unfold subst_nation_sexpr_val, subst_nation_aggterm; simpl.
  reflexivity.
Qed.

(** FormalSQL EXISTS stage: Filter(EXISTS l2) over the ordinary outer query. *)
Definition q_exists_outer (nation_name : string) : QueryExpr :=
  QExpr_Filter q21_exists_l2_sexpr (q_ordinary_outer nation_name).

Lemma q21_bag_rows_width :
  forall fts rrs,
    bag_correspond_unordered slot_map_outer_cor fts rrs ->
    forall rr, List.In rr rrs -> List.length rr = outer_width.
Proof.
  intros fts rrs [rrs' [HP HF]] rr Hin.
  apply (Permutation_in rr HP) in Hin.
  clear HP; induction HF as [|ft r fts0 rrs0 [Hlen _] _ IH]; [contradiction|].
  destruct Hin as [<-|Hin]; [rewrite Hlen; reflexivity|exact (IH Hin)].
Qed.

Definition Q21_ExistsL2StageCorrespond_statement : Prop :=
  forall (db : DB) (nation_name : string),
    Preconditions_Q21 db nation_name ->
    exists (D : db_state) (fts : list (tuple TNull)) (rows : list Row),
      EmbedDB_Q21 db = Some D /\
      FormalSQL_exec D (q_exists_outer nation_name) fts /\
      Eval_Spec semi_exists_l2 (Params_Q21 nation_name) db = Some rows /\
      bag_correspond_unordered slot_map_outer_cor fts rows /\
      List.Forall q21_outer_cor_labels_ok fts.

Theorem Q21_ExistsL2StageCorrespond :
  Q21_ExistsL2StageCorrespond_statement.
Proof.
  unfold Q21_ExistsL2StageCorrespond_statement.
  intros db nation_name Hpre.
  destruct (Q21_OuterOrdinarySpineCorrespond_strong db nation_name Hpre)
    as [D [fts0 [rows0 [HD [Hx [He [Hb Hlabs]]]]]]].
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
    (List.filter (fun ft => existsb (q21_l2_match nation_name ft) ls') fts0),
    (List.filter (q21_rel_exists_l2_keep nation_name ls) rows0).
  split; [exact HD|].
  split; [|split; [|split]].
  - (* FormalSQL: Filter(EXISTS) on top of the ordinary outer derivation *)
    unfold FormalSQL_exec, eval_query_expr_possible_outcome in *.
    destruct Hx as [sched Hq].
    exists sched.
    unfold q_exists_outer.
    eapply EQuery_FilterRows; [exact Hq|].
    apply (proj2 (Q21_exists_l2_filter_rows ss' ls' os' ns' sched
                    nation_name fts0 _ Ml Hlabs)).
    reflexivity.
  - (* RelNode: RelSemiJoin filter_outer scan_lineitem pred_exists_l2 *)
    exact (Q21_semi_exists_l2_eval db nation_name ss ls os ns rows0
             Hpre Hs Hl Ho Hn He (q21_bag_rows_width _ _ Hb)).
  - (* bag / multiplicity *)
    exact (Q21_exists_l2_bag nation_name fts0 rows0 ls' ls Hb Bl).
  - (* side condition carried to the next correlated layer *)
    apply Forall_forall; intros ft Hin.
    apply filter_In in Hin as [Hin _].
    rewrite Forall_forall in Hlabs; exact (Hlabs ft Hin).
Qed.

