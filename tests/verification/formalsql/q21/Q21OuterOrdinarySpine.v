(* Q21OuterOrdinarySpine — FormalSQL↔RelNode correspondence for the Q21
   outer join/filter spine + ordinary (non-EXISTS) conjuncts only.

   Ordinary predicates (exact lowered logical shape):
     s_suppkey = l1.l_suppkey
     o_orderkey = l1.l_orderkey
     o_orderstatus = 'F'
     l1.l_receiptdate > l1.l_commitdate
     s_nationkey = n_nationkey
     n_name = nation_name

   EXISTS / NOT EXISTS are out of scope (next milestone).
   No Admit/Axiom. No FormalSQL→RelNode compiler stack. *)

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

(** * Outer SlotMap (supplier ∥ l1 ∥ orders ∥ nation) — width 11 *)

Definition slot_map_lineitem : SlotMap :=
  [mkSlotBinding (Attr_int32 "l_orderkey") (attr "l_orderkey" TInt);
   mkSlotBinding (Attr_int32 "l_suppkey") (attr "l_suppkey" TInt);
   mkSlotBinding (Attr_date "l_commitdate") (attr "l_commitdate" TDate);
   mkSlotBinding (Attr_date "l_receiptdate") (attr "l_receiptdate" TDate)].

Definition slot_map_orders : SlotMap :=
  [mkSlotBinding (Attr_int32 "o_orderkey") (attr "o_orderkey" TInt);
   mkSlotBinding (Attr_string "o_orderstatus" (StringChar 1))
                 (attr "o_orderstatus" TText)].

Definition slot_map_nation : SlotMap :=
  [mkSlotBinding (Attr_int32 "n_nationkey") (attr "n_nationkey" TInt);
   mkSlotBinding (Attr_string "n_name" (StringChar 25))
                 (attr "n_name" TText)].

Definition slot_map_outer : SlotMap :=
  List.app slot_map_supplier
    (List.app slot_map_lineitem
      (List.app slot_map_orders slot_map_nation)).

Lemma slot_map_outer_length :
  List.length slot_map_outer = outer_width.
Proof. reflexivity. Qed.

Lemma slot_map_outer_layout :
  nth_error slot_map_outer slot_s_suppkey =
    Some (mkSlotBinding (Attr_int32 "s_suppkey") (attr "s_suppkey" TInt)) /\
  nth_error slot_map_outer slot_s_nationkey =
    Some (mkSlotBinding (Attr_int32 "s_nationkey") (attr "s_nationkey" TInt)) /\
  nth_error slot_map_outer slot_l1_orderkey =
    Some (mkSlotBinding (Attr_int32 "l_orderkey") (attr "l_orderkey" TInt)) /\
  nth_error slot_map_outer slot_l1_suppkey =
    Some (mkSlotBinding (Attr_int32 "l_suppkey") (attr "l_suppkey" TInt)) /\
  nth_error slot_map_outer slot_l1_commitdate =
    Some (mkSlotBinding (Attr_date "l_commitdate") (attr "l_commitdate" TDate)) /\
  nth_error slot_map_outer slot_l1_receiptdate =
    Some (mkSlotBinding (Attr_date "l_receiptdate") (attr "l_receiptdate" TDate)) /\
  nth_error slot_map_outer slot_o_orderkey =
    Some (mkSlotBinding (Attr_int32 "o_orderkey") (attr "o_orderkey" TInt)) /\
  nth_error slot_map_outer slot_o_orderstatus =
    Some (mkSlotBinding (Attr_string "o_orderstatus" (StringChar 1))
            (attr "o_orderstatus" TText)) /\
  nth_error slot_map_outer slot_n_nationkey =
    Some (mkSlotBinding (Attr_int32 "n_nationkey") (attr "n_nationkey" TInt)) /\
  nth_error slot_map_outer slot_n_name =
    Some (mkSlotBinding (Attr_string "n_name" (StringChar 25))
            (attr "n_name" TText)).
Proof. repeat split; reflexivity. Qed.

Lemma slot_map_lineitem_wf : SlotMap_wf slot_map_lineitem.
Proof.
  unfold SlotMap_wf, slot_map_lineitem, slot_binding_wf,
    slot_formal_name, slot_rel_name, slot_rel_type, slot_map_names.
  repeat split;
    try (repeat constructor; simpl; intuition discriminate);
    repeat constructor; simpl; try reflexivity.
Qed.

Lemma slot_map_orders_wf : SlotMap_wf slot_map_orders.
Proof.
  unfold SlotMap_wf, slot_map_orders, slot_binding_wf,
    slot_formal_name, slot_rel_name, slot_rel_type, slot_map_names.
  repeat split;
    try (repeat constructor; simpl; intuition discriminate);
    repeat constructor; simpl; try reflexivity.
Qed.

Lemma slot_map_nation_wf : SlotMap_wf slot_map_nation.
Proof.
  unfold SlotMap_wf, slot_map_nation, slot_binding_wf,
    slot_formal_name, slot_rel_name, slot_rel_type, slot_map_names.
  repeat split;
    try (repeat constructor; simpl; intuition discriminate);
    repeat constructor; simpl; try reflexivity.
Qed.

Lemma slot_map_outer_wf : SlotMap_wf slot_map_outer.
Proof.
  unfold slot_map_outer.
  (* Discharge by unfolding + NoDup constructors on the concrete 11-name lists. *)
  unfold SlotMap_wf, slot_binding_wf,
    slot_formal_name, slot_rel_name, slot_rel_type, slot_map_names.
  unfold slot_map_supplier, slot_map_lineitem, slot_map_orders, slot_map_nation.
  simpl.
  repeat split;
    try (repeat constructor; simpl; intuition discriminate);
    repeat constructor; simpl; try reflexivity.
Qed.

(** * RelNode ordinary predicate (all 6 conjuncts on width-11 rows) *)

Definition pred_eq_supp_l1 : Pred :=
  EBinary BEq (EInputRef slot_s_suppkey) (EInputRef slot_l1_suppkey).

Definition pred_eq_ord_l1 : Pred :=
  EBinary BEq (EInputRef slot_o_orderkey) (EInputRef slot_l1_orderkey).

Definition pred_eq_nationkey : Pred :=
  EBinary BEq (EInputRef slot_s_nationkey) (EInputRef slot_n_nationkey).

(** Join keys + outer filters — ordinary only (no EXISTS).
    LogicalSpec splits joins into RelJoin and filters into RelFilter;
    [pred_ordinary_outer] packages the same six conjuncts. *)
Definition pred_ordinary_outer : Pred :=
  EBinary BAnd pred_eq_supp_l1
    (EBinary BAnd pred_eq_ord_l1
      (EBinary BAnd pred_status_F
        (EBinary BAnd pred_late_l1
          (EBinary BAnd pred_eq_nationkey pred_nation_param)))).

Lemma pred_outer_filters_structure :
  pred_outer_filters =
  EBinary BAnd pred_status_F
    (EBinary BAnd pred_late_l1 pred_nation_param).
Proof. reflexivity. Qed.

Lemma join_outer_spine_structure :
  join_outer_spine =
  RelJoin JoinInner
    (RelJoin JoinInner
      (RelJoin JoinInner scan_supplier scan_lineitem
         (EBinary BEq (EInputRef 0%nat) (EInputRef 4%nat)))
      scan_orders
      (EBinary BEq (EInputRef 3%nat) (EInputRef 7%nat)))
    scan_nation
    (EBinary BEq (EInputRef 2%nat) (EInputRef 9%nat)).
Proof. reflexivity. Qed.

Lemma filter_outer_structure :
  filter_outer = RelFilter join_outer_spine pred_outer_filters.
Proof. reflexivity. Qed.

(** * Column-reference equalities under row_correspond *)

Lemma row_correspond_nth :
  forall sm ft rr i sb v,
    row_correspond sm ft rr ->
    nth_error sm i = Some sb ->
    nth_error rr i = Some v ->
    value_correspond (slot_rel_type sb) (dot TNull ft (sb_formal sb)) v.
Proof.
  intros sm ft rr i sb v [Hlen Hdots] Hi Hv.
  eapply Hdots; eassumption.
Qed.

(** Slot facts used by ordinary predicates (FormalSQL Attr ↔ RelNode slot). *)
Lemma Q21_ordinary_slot_refs :
  slot_s_suppkey = 0%nat /\
  slot_l1_suppkey = 4%nat /\
  slot_l1_orderkey = 3%nat /\
  slot_o_orderkey = 7%nat /\
  slot_o_orderstatus = 8%nat /\
  slot_l1_commitdate = 5%nat /\
  slot_l1_receiptdate = 6%nat /\
  slot_s_nationkey = 2%nat /\
  slot_n_nationkey = 9%nat /\
  slot_n_name = 10%nat.
Proof. repeat split; reflexivity. Qed.

(** * 3VL bridges: ValueCorrespond → FormalSQL interp ↔ RelNode cmp

    Supported fragment for ordinary Q21 predicates:
      VC_int32 / VC_date / VC_text (CHAR/Bpchar, non-NULL). *)

Definition formal_is_true (b3 : Bool3.bool3) : bool :=
  match b3 with true3 => true | _ => false end.

(** [string_rtrim_spaces] is idempotent — used to turn CharN cast-identity
    into the no-trailing-space fact that aligns [sql_string_compare] with
    RelNode [String.eqb]. *)
Lemma drop_leading_spaces_idempotent :
  forall xs,
    drop_leading_spaces (drop_leading_spaces xs) = drop_leading_spaces xs.
Proof.
  induction xs as [|a xs IH]; simpl; [reflexivity|].
  destruct (ascii_is_space a) eqn:Ha.
  - exact IH.
  - simpl; rewrite Ha; reflexivity.
Qed.

Lemma string_rtrim_spaces_idempotent :
  forall s,
    string_rtrim_spaces (string_rtrim_spaces s) = string_rtrim_spaces s.
Proof.
  intro s.
  unfold string_rtrim_spaces.
  rewrite ascii_list_to_string_to_ascii_list.
  rewrite rev_involutive.
  rewrite drop_leading_spaces_idempotent.
  reflexivity.
Qed.

Lemma CharNCanonical_implies_rtrim :
  forall n s,
    string_explicit_cast (StringChar n) s = s ->
    string_rtrim_spaces s = s.
Proof.
  intros n s Hcan.
  unfold string_explicit_cast, string_canonical_value in Hcan.
  rewrite <- Hcan.
  apply string_rtrim_spaces_idempotent.
Qed.

Lemma Char1Canonical_implies_rtrim :
  forall s, Char1Canonical s -> string_rtrim_spaces s = s.
Proof.
  intros s H; unfold Char1Canonical in H; eapply CharNCanonical_implies_rtrim; exact H.
Qed.

Lemma Char25Canonical_implies_rtrim :
  forall s, Char25Canonical s -> string_rtrim_spaces s = s.
Proof.
  intros s H; unfold Char25Canonical in H; eapply CharNCanonical_implies_rtrim; exact H.
Qed.

Lemma string_comparison_values_char_char :
  forall tm1 s1 tm2 s2,
    string_typmod_is_char tm1 = true ->
    string_typmod_is_char tm2 = true ->
    string_comparison_values tm1 s1 tm2 s2 =
      (string_rtrim_spaces s1, string_rtrim_spaces s2).
Proof.
  intros tm1 s1 tm2 s2 H1 H2.
  unfold string_comparison_values.
  rewrite H1, H2; simpl.
  destruct tm1; try discriminate H1;
  destruct tm2; try discriminate H2;
  reflexivity.
Qed.

(** Generic CHAR/Bpchar text equality 3VL bridge.
    Under no-trailing-space carriers, FormalSQL [sql_string_compare]
    (rtrim both) coincides with RelNode [String.eqb]. *)
Lemma VC_text_eq_3vl :
  forall tm1 s1 tm2 s2,
    string_typmod_is_char tm1 = true ->
    string_typmod_is_char tm2 = true ->
    string_rtrim_spaces s1 = s1 ->
    string_rtrim_spaces s2 = s2 ->
    formal_is_true
      (interp_predicate PredicateEq
         [Value_string (StringValue tm1 (Some s1));
          Value_string (StringValue tm2 (Some s2))]) =
    truth3_is_true (cmp_int_eq (VText s1) (VText s2)).
Proof.
  intros tm1 s1 tm2 s2 Htm1 Htm2 Hr1 Hr2.
  pose proof (order_value_compare_string_nonnull tm1 s1 tm2 s2) as Hord.
  rewrite (interp_predicate_eq_of_order_compare _ _ _ Hord).
  unfold formal_is_true, truth3_is_true, cmp_int_eq; simpl.
  pose proof (sql_string_compare_eq_iff_semantic_values tm1 s1 tm2 s2) as Hiff.
  rewrite (string_comparison_values_char_char tm1 s1 tm2 s2 Htm1 Htm2) in Hiff.
  simpl in Hiff; rewrite Hr1, Hr2 in Hiff.
  destruct (String.eqb s1 s2) eqn:Heq.
  - apply String.eqb_eq in Heq; subst s2.
    assert (sql_string_compare tm1 s1 tm2 s1 = Eq) as ->
      by (apply (proj2 Hiff); reflexivity).
    reflexivity.
  - apply String.eqb_neq in Heq.
    destruct (sql_string_compare tm1 s1 tm2 s2) eqn:Hc; try reflexivity.
    exfalso; apply Heq; apply (proj1 Hiff); reflexivity.
Qed.

(** Q21 leaf: [o_orderstatus = 'F']
    FormalSQL: DotString CHAR(1) vs [CstString StringBpchar "F"]
    (constant already stores [string_explicit_cast Bpchar "F"] = "F"). *)
Lemma VC_status_F_3vl :
  forall status,
    Char1Canonical status ->
    formal_is_true
      (interp_predicate PredicateEq
         [Value_string (StringValue (StringChar 1) (Some status));
          Value_string (StringValue StringBpchar
            (Some (string_explicit_cast StringBpchar "F")))]) =
    truth3_is_true (cmp_int_eq (VText status) (VText "F")).
Proof.
  intros status Hcan.
  change (string_explicit_cast StringBpchar "F") with "F".
  apply VC_text_eq_3vl.
  - reflexivity.
  - reflexivity.
  - apply Char1Canonical_implies_rtrim; exact Hcan.
  - reflexivity.
Qed.

(** Q21 leaf: [n_name = nation_name]
    FormalSQL: DotString CHAR(25) vs [CstString StringBpchar nation_name]
    (constant stores rtrim(nation); [NationNameOk] makes that = nation). *)
Lemma VC_nation_name_3vl :
  forall nn nation_name,
    Char25Canonical nn ->
    NationNameOk nation_name ->
    formal_is_true
      (interp_predicate PredicateEq
         [Value_string (StringValue (StringChar 25) (Some nn));
          Value_string (StringValue StringBpchar
            (Some (string_explicit_cast StringBpchar nation_name)))]) =
    truth3_is_true (cmp_int_eq (VText nn) (VText nation_name)).
Proof.
  intros nn nation_name Hcan [Hlen Hrtrim].
  unfold string_explicit_cast, string_canonical_value.
  rewrite Hrtrim.
  apply VC_text_eq_3vl.
  - reflexivity.
  - reflexivity.
  - apply Char25Canonical_implies_rtrim; exact Hcan.
  - exact Hrtrim.
Qed.

Lemma VC_int32_eq_3vl :
  forall i j,
    formal_is_true
      (interp_predicate PredicateEq
         [Value_int32 (Some i); Value_int32 (Some j)]) =
    truth3_is_true (cmp_int_eq (VInt (int32_value i)) (VInt (int32_value j))).
Proof.
  intros i j.
  pose proof (NullPredicates.order_value_compare_integral
                (Value_int32 (Some i)) (Value_int32 (Some j))
                (int32_value i) (int32_value j) eq_refl eq_refl) as Hord.
  rewrite (interp_predicate_eq_of_order_compare _ _ _ Hord).
  unfold formal_is_true, truth3_is_true, cmp_int_eq; simpl.
  destruct (Z.compare_spec (int32_value i) (int32_value j)) as [He|Hl|Hg].
  - rewrite He, Z.eqb_refl; reflexivity.
  - assert (Z.eqb (int32_value i) (int32_value j) = false) as ->
      by (apply Z.eqb_neq; lia).
    reflexivity.
  - assert (Z.eqb (int32_value i) (int32_value j) = false) as ->
      by (apply Z.eqb_neq; lia).
    reflexivity.
Qed.

Lemma VC_date_gt_3vl :
  forall d1 d2,
    formal_is_true
      (interp_predicate PredicateGt
         [Value_date (Some d1); Value_date (Some d2)]) =
    truth3_is_true (cmp_truth BGt (VInt d1) (VInt d2)).
Proof.
  intros d1 d2.
  assert (Hord :
    NullPredicates.order_value_compare
      (Value_date (Some d1)) (Value_date (Some d2)) =
    Some (Z.compare d1 d2)).
  { unfold NullPredicates.order_value_compare,
      NullPredicates.date_order_value,
      NullPredicates.integral_order_value; simpl; reflexivity. }
  rewrite (interp_predicate_gt_of_order_compare _ _ _ Hord).
  unfold formal_is_true, truth3_is_true, cmp_truth,
    cmp_int_leb, cmp_int_eq, truth3_and, truth3_not, truth3_of_bool; simpl.
  destruct (Z.compare_spec d1 d2) as [He|Hl|Hg].
  - subst; rewrite Z.leb_refl, Z.eqb_refl; simpl; reflexivity.
  - assert (Z.leb d2 d1 = false) as -> by (apply Z.leb_gt; exact Hl).
    simpl; reflexivity.
  - assert (Z.leb d2 d1 = true) as -> by (apply Z.leb_le; lia).
    assert (Z.eqb d1 d2 = false) as -> by (apply Z.eqb_neq; lia).
    simpl; reflexivity.
Qed.

(** * RelNode Eval of [filter_outer] — ordinary spine characterization *)

Definition Eval_outer_ordinary (nation_name : string) (db : DB)
  : option (list Row) :=
  match Eval_Spec filter_outer (Params_Q21 nation_name) db with
  | Some rows => Some rows
  | None => None
  end.

Lemma Eval_outer_ordinary_eq_filter_outer :
  forall nation_name db,
    Eval_outer_ordinary nation_name db =
    Eval_Spec filter_outer (Params_Q21 nation_name) db.
Proof.
  intros nation_name db.
  unfold Eval_outer_ordinary.
  destruct (Eval_Spec filter_outer (Params_Q21 nation_name) db); reflexivity.
Qed.

(** Milestone: FormalSQL CrossJoin + __logos_cor Project + ordinary 6-conjunct
    ConjList ↔ RelNode [filter_outer], bag-preserving, no EXISTS. *)

(** Extract the CrossJoin spine under the outer Filter/Project from q̂. *)
Definition q21_cross_spine_of (q : QueryExpr) : option QueryExpr :=
  match q with
  | QExpr_Project _
      (QExpr_OrderBy _
        (QExpr_Group _ _ _
          (QExpr_Project _
            (QExpr_Project _
              (QExpr_Filter _
                (QExpr_Project _
                  (QExpr_Filter _ spine))))))) =>
      Some spine
  | _ => None
  end.

Lemma q21_cross_spine_source_Some :
  match q21_cross_spine_of Queries.source_query_expr with
  | Some spine => is_q21_cross_spine spine = true
  | None => False
  end.
Proof. vm_compute; reflexivity. Qed.

(** Ordinary 6-conjunct ConjList: strip EXISTS / NOT EXISTS from the lowered
    outer filter, then substitute [nation_name]. *)
Definition ordinary_outer_conjlist (nation_name : string)
  : @scalar_expr TNull relname ScalarResultBoolean :=
  match subst_nation_sexpr_bool nation_name Queries.scalar_expr_predicate_1 with
  | SExpr_ConjList sites And_F [p0; p1; p2; p3; _; _; p6; p7] =>
      SExpr_ConjList
        ([[]; ["b0"]; ["b1"; "b2"]; ["b3"; "b4"; "b5"];
          ["b6"; "b7"; "b8"; "b9"]; ["b10"; "b11"]])%list
        And_F [p0; p1; p2; p3; p6; p7]
  | e => e
  end.

(** FormalSQL ordinary outer query: Filter(ordinary_6) ∘ Project(cor rename) ∘
    Filter(True) ∘ CrossJoin spine — same shape as lowered Q21 without EXISTS. *)
Definition q_ordinary_outer (nation_name : string) : QueryExpr :=
  match q21_cross_spine_of (qhat_Q21 nation_name) with
  | Some spine =>
      QExpr_Filter (ordinary_outer_conjlist nation_name)
        (QExpr_Project Queries.scalar_select_list_4
          (QExpr_Filter SExpr_True spine))
  | None =>
      QExpr_Filter (ordinary_outer_conjlist nation_name)
        (QExpr_Project Queries.scalar_select_list_4
          (QExpr_Filter SExpr_True Queries.source_query_expr))
  end.

(** SlotMap after __logos_cor rename (watched 11 attrs only). *)
Definition slot_map_outer_cor : SlotMap :=
  [mkSlotBinding (Attr_int32 "__logos_cor_cor0_0") (attr "s_suppkey" TInt);
   mkSlotBinding (Attr_string "__logos_cor_cor0_1" (StringChar 25))
                 (attr "s_name" TText);
   mkSlotBinding (Attr_int32 "__logos_cor_cor0_3") (attr "s_nationkey" TInt);
   mkSlotBinding (Attr_int32 "__logos_cor_cor0_7") (attr "l_orderkey" TInt);
   mkSlotBinding (Attr_int32 "__logos_cor_cor0_9") (attr "l_suppkey" TInt);
   mkSlotBinding (Attr_date "__logos_cor_cor0_18") (attr "l_commitdate" TDate);
   mkSlotBinding (Attr_date "__logos_cor_cor0_19") (attr "l_receiptdate" TDate);
   mkSlotBinding (Attr_int32 "__logos_cor_cor0_23") (attr "o_orderkey" TInt);
   mkSlotBinding (Attr_string "__logos_cor_cor0_25" (StringChar 1))
                 (attr "o_orderstatus" TText);
   mkSlotBinding (Attr_int32 "__logos_cor_cor0_32") (attr "n_nationkey" TInt);
   mkSlotBinding (Attr_string "__logos_cor_cor0_33" (StringChar 25))
                 (attr "n_name" TText)].

Lemma slot_map_outer_cor_length :
  List.length slot_map_outer_cor = outer_width.
Proof. reflexivity. Qed.

(** Why unordered bags: FormalSQL [query_cross_join_bag] reifies via
    [Febag.elements] (canonical order); RelNode [join_inner] uses nested-loop
    order. Pre-ORDER-BY SQL semantics require multiset equivalence, not
    identical enumeration. [bag_correspond] (Forall2) remains for ordered
    witnesses (e.g. scan [map] embed). *)

(** Full ordinary outer spine correspondence (target theorem). *)
Definition Q21_OuterOrdinarySpineCorrespond_statement : Prop :=
  forall (db : DB) (nation_name : string),
    Preconditions_Q21 db nation_name ->
    exists (D : db_state) (fts : list (tuple TNull)) (rows : list Row),
      EmbedDB_Q21 db = Some D /\
      FormalSQL_exec D (q_ordinary_outer nation_name) fts /\
      Eval_Spec filter_outer (Params_Q21 nation_name) db = Some rows /\
      bag_correspond_unordered slot_map_outer_cor fts rows.

(** RelNode half: [filter_outer] is definitionally the RelFilter of the
    equijoin spine by the three ordinary outer predicates (status/late/nation).
    Join keys are the RelJoin conditions (see [join_outer_eq_conds_slots]). *)

Lemma filter_outer_is_relfilter :
  filter_outer = RelFilter join_outer_spine pred_outer_filters.
Proof. reflexivity. Qed.

Lemma join_outer_eq_conds_slots :
  (* supplier ⋈ l1 *) (0%nat = slot_s_suppkey /\ 4%nat = slot_l1_suppkey) /\
  (* ⋈ orders *)     (3%nat = slot_l1_orderkey /\ 7%nat = slot_o_orderkey) /\
  (* ⋈ nation *)     (2%nat = slot_s_nationkey /\ 9%nat = slot_n_nationkey).
Proof. repeat split; reflexivity. Qed.

(** RelNode empty half (immediate). *)
Lemma Eval_Spec_filter_outer_empty :
  forall nation_name,
    Eval_Spec filter_outer (Params_Q21 nation_name) db_empty_Q21 = Some [].
Proof.
  intros nation_name.
  unfold Eval_Spec, filter_outer, join_outer_spine, join_supp_l1_ord,
    join_supp_l1, scan_supplier, scan_lineitem, scan_orders, scan_nation,
    db_empty_Q21, db_lookup, Params_Q21; simpl.
  reflexivity.
Qed.

(** * FormalSQL empty-spine scaffolding

    Target: [FormalSQL_exec] of [q_ordinary_outer] on empty EmbedDB yields [nil],
    which unordered-corresponds to RelNode [filter_outer] = [[]]. *)

Definition D_empty_Q21 : db_state :=
  witness_database Schema.generated_schema
    [WitnessTable (Rel "supplier") nil;
     WitnessTable (Rel "lineitem") nil;
     WitnessTable (Rel "orders") nil;
     WitnessTable (Rel "nation") nil].

Lemma EmbedDB_Q21_empty_D :
  EmbedDB_Q21 db_empty_Q21 = Some D_empty_Q21.
Proof. apply EmbedDB_Q21_empty. Qed.

Lemma embed_empty_instance_supplier :
  @_instance TNull D_empty_Q21 (Rel "supplier")
  = Febag.mk_bag (Fecol.CBag (CTuple TNull)) nil.
Proof. unfold D_empty_Q21; reflexivity. Qed.

Lemma embed_empty_instance_lineitem :
  @_instance TNull D_empty_Q21 (Rel "lineitem")
  = Febag.mk_bag (Fecol.CBag (CTuple TNull)) nil.
Proof. unfold D_empty_Q21; reflexivity. Qed.

Lemma embed_empty_instance_orders :
  @_instance TNull D_empty_Q21 (Rel "orders")
  = Febag.mk_bag (Fecol.CBag (CTuple TNull)) nil.
Proof. unfold D_empty_Q21; reflexivity. Qed.

Lemma embed_empty_instance_nation :
  @_instance TNull D_empty_Q21 (Rel "nation")
  = Febag.mk_bag (Fecol.CBag (CTuple TNull)) nil.
Proof. unfold D_empty_Q21; reflexivity. Qed.

Lemma embed_empty_table_same_rows_named :
  forall outputs name,
    (name = "supplier" \/ name = "lineitem" \/
     name = "orders" \/ name = "nation") ->
    query_same_rows_as_bag (T:=TNull) nil
      (@query_table_bag TNull relname
         (@_basesort TNull D_empty_Q21)
         (@_instance TNull D_empty_Q21)
         outputs (Rel name)).
Proof.
  intros outputs name Hname.
  unfold query_same_rows_as_bag, query_table_bag, query_rows_bag.
  rewrite Febag.nb_occ_equal; intro x.
  assert (Hinst :
    @_instance TNull D_empty_Q21 (Rel name)
    = Febag.mk_bag (Fecol.CBag (CTuple TNull)) nil).
  { destruct Hname as [-> | [-> | [-> | ->]]].
    - apply embed_empty_instance_supplier.
    - apply embed_empty_instance_lineitem.
    - apply embed_empty_instance_orders.
    - apply embed_empty_instance_nation. }
  rewrite Hinst.
  destruct (@query_outputs_sort TNull outputs =S?=
            @_basesort TNull D_empty_Q21 (Rel name));
    rewrite ?Febag.nb_occ_mk_bag, ?Febag.nb_occ_empty; cbn; reflexivity.
Qed.

Lemma query_cross_join_empty_empty :
  query_same_rows_as_bag (T:=TNull) nil
    (query_cross_join_bag
       (query_rows_bag (@nil (tuple TNull)))
       (query_rows_bag (@nil (tuple TNull)))).
Proof.
  unfold query_same_rows_as_bag, query_cross_join_bag, query_rows_bag.
  rewrite Febag.nb_occ_equal; intro x.
  rewrite !Febag.nb_occ_mk_bag.
  cbn.
  reflexivity.
Qed.

(** Empty filter / project leaves. *)
Lemma q21_empty_filter_rows :
  forall formula,
    @eval_filter_rows_outcome TNull relname
      (@_basesort TNull D_empty_Q21) (@_instance TNull D_empty_Q21)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      q21_boolean_schedule
      nil formula nil (SqlSuccess nil).
Proof. intros; apply EFilterRows_Nil. Qed.

Lemma q21_empty_project_rows :
  forall select_list,
    @eval_project_rows_outcome TNull relname
      (@_basesort TNull D_empty_Q21) (@_instance TNull D_empty_Q21)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      q21_boolean_schedule
      nil select_list nil (SqlSuccess nil).
Proof. intros; apply EProjectRows_Nil. Qed.

Local Abbreviation EQ :=
  (@eval_query_expr_outcome TNull relname
     (@_basesort TNull D_empty_Q21) (@_instance TNull D_empty_Q21)
     unknown3 NullValues.interp_scalar_operator_runtime_error
     NullValues.interp_aggregate_runtime_error value_is_null
     q21_boolean_schedule nil).

Lemma q21_empty_table_success :
  forall outputs name,
    (name = "supplier" \/ name = "lineitem" \/
     name = "orders" \/ name = "nation") ->
    EQ (QExpr_Table outputs (Rel name)) (SqlSuccess nil).
Proof.
  intros outputs name Hname.
  apply EQuery_Table.
  apply embed_empty_table_same_rows_named; exact Hname.
Qed.

Lemma q21_empty_cross_join_success :
  forall left right,
    EQ left (SqlSuccess nil) ->
    EQ right (SqlSuccess nil) ->
    EQ (QExpr_CrossJoin left right) (SqlSuccess nil).
Proof.
  intros left right Hl Hr.
  eapply EQuery_CrossJoinSuccess; [exact Hl|exact Hr|].
  apply query_cross_join_empty_empty.
Qed.

Lemma q21_empty_filter_success :
  forall formula input,
    EQ input (SqlSuccess nil) ->
    EQ (QExpr_Filter formula input) (SqlSuccess nil).
Proof.
  intros formula input Hin.
  eapply EQuery_FilterRows; [exact Hin|].
  apply q21_empty_filter_rows.
Qed.

Lemma q21_empty_project_success :
  forall select_list input,
    EQ input (SqlSuccess nil) ->
    EQ (QExpr_Project select_list input) (SqlSuccess nil).
Proof.
  intros select_list input Hin.
  eapply EQuery_ProjectRows; [exact Hin|].
  apply q21_empty_project_rows.
Qed.

(** [q_ordinary_outer] wraps an extracted CrossJoin spine. *)
Lemma q_ordinary_outer_shape_of_Some :
  forall nation_name spine,
    q21_cross_spine_of (qhat_Q21 nation_name) = Some spine ->
    q_ordinary_outer nation_name =
      QExpr_Filter (ordinary_outer_conjlist nation_name)
        (QExpr_Project Queries.scalar_select_list_4
          (QExpr_Filter SExpr_True spine)).
Proof.
  intros nation_name spine Hs.
  unfold q_ordinary_outer; rewrite Hs; reflexivity.
Qed.

Lemma q21_cross_spine_catalog_Some :
  match q21_cross_spine_of (qhat_Q21 catalog_nation_name) with
  | Some spine => is_q21_cross_spine spine = true
  | None => False
  end.
Proof.
  rewrite qhat_Q21_catalog_instance.
  apply q21_cross_spine_source_Some.
Qed.

(** * Intermediate obligations (nonempty spine)

    (a) CrossJoin bag ↔ RelNode nested-loop cartesian/equijoin
    (b) Project [scalar_select_list_4] preserves [row_correspond] under rename
    (c) ordinary ConjList keep ↔ [pred_ordinary_outer] under [row_correspond]
    (d) filtering corresponding bags preserves [bag_correspond_unordered]
    (e) compose into [Q21_OuterOrdinarySpineCorrespond_statement] *)

(** RelNode: [eval_pred_keep] of join-key / ordinary preds succeeds on typed rows. *)
Lemma eval_pred_keep_beq_int_Some :
  forall (p : Params) (r : Row) (i j : nat) (x y : Z),
    nth_error r i = Some (VInt x) ->
    nth_error r j = Some (VInt y) ->
    exists b,
      eval_pred_keep
        (EBinary BEq (EInputRef i) (EInputRef j)) p r = Some b.
Proof.
  intros p r i j x y Hi Hj.
  unfold eval_pred_keep, eval_pred, eval_expr, ctx_lookup_input, row_nth.
  rewrite Hi, Hj; simpl.
  unfold cmp_truth, cmp_int_eq, truth3_of_bool.
  destruct (Z.eqb x y); [exists true|exists false]; reflexivity.
Qed.

Lemma eval_pred_keep_beq_text_Some :
  forall (p : Params) (r : Row) (i : nat) (s t : string),
    nth_error r i = Some (VText s) ->
    exists b,
      eval_pred_keep
        (EBinary BEq (EInputRef i) (EConst (VText t) TText)) p r = Some b.
Proof.
  intros p r i s t Hi.
  unfold eval_pred_keep, eval_pred, eval_expr, ctx_lookup_input, row_nth.
  rewrite Hi; simpl.
  unfold cmp_truth, cmp_int_eq, truth3_of_bool.
  destruct (String.eqb s t); [exists true|exists false]; reflexivity.
Qed.

Lemma eval_pred_keep_beq_param_text_Some :
  forall (nation_name : string) (r : Row) (i : nat) (s : string),
    nth_error r i = Some (VText s) ->
    exists b,
      eval_pred_keep
        (EBinary BEq (EInputRef i) param_nation_name)
        (Params_Q21 nation_name) r = Some b.
Proof.
  intros nation_name r i s Hi.
  unfold eval_pred_keep, eval_pred, eval_expr, ctx_lookup_input, row_nth,
    Params_Q21, params_nation, params_lookup, param_nation_name.
  rewrite Hi; cbn.
  unfold cmp_truth, cmp_int_eq, truth3_of_bool.
  destruct (String.eqb s nation_name); [exists true|exists false]; reflexivity.
Qed.

Lemma eval_pred_keep_bgt_date_Some :
  forall (p : Params) (r : Row) (i j : nat) (x y : Z),
    nth_error r i = Some (VInt x) ->
    nth_error r j = Some (VInt y) ->
    exists b,
      eval_pred_keep
        (EBinary BGt (EInputRef i) (EInputRef j)) p r = Some b.
Proof.
  intros p r i j x y Hi Hj.
  unfold eval_pred_keep, eval_pred, eval_expr, ctx_lookup_input, row_nth.
  rewrite Hi, Hj; simpl.
  unfold cmp_truth, cmp_int_leb, cmp_int_eq, truth3_and, truth3_not,
    truth3_of_bool.
  destruct (Z.leb y x); destruct (Z.eqb x y);
    try (exists true; reflexivity); exists false; reflexivity.
Qed.

(** Precise remaining semantic lemma for the spine theorem:

    FormalSQL nested CrossJoin → Project([scalar_select_list_4]) →
    Filter([ordinary_outer_conjlist]) success bag
    ↔
    RelNode nested [join_inner] → [filter_rows pred_outer_filters]
    under [bag_correspond_unordered slot_map_outer_cor], preserving
    multiset multiplicity (via [filter_brute_left_join_list_as_theta] /
    [raw_cross_same_rows_as_bag] on the FormalSQL side and nested-loop
    [join_inner] on the RelNode side).

    Includes FormalSQL_exec existence on EmbedDB (Logos CrossJoin/Project/
    ConjList acceptance) and the bag relation. No FormalSQL↔RelNode join
    morphism exists yet beyond ScanCorrespond. *)
Definition Q21_CrossJoin_ConjList_JoinInner_Bag : Prop :=
  forall (db : DB) (nation_name : string),
    Preconditions_Q21 db nation_name ->
    exists (D : db_state) (fts : list (tuple TNull)) (rows : list Row),
      EmbedDB_Q21 db = Some D /\
      FormalSQL_exec D (q_ordinary_outer nation_name) fts /\
      Eval_Spec filter_outer (Params_Q21 nation_name) db = Some rows /\
      bag_correspond_unordered slot_map_outer_cor fts rows.

(** The public statement is definitionally this obligation. *)
Lemma Q21_OuterOrdinarySpineCorrespond_statement_eq_Bag :
  Q21_OuterOrdinarySpineCorrespond_statement =
  Q21_CrossJoin_ConjList_JoinInner_Bag.
Proof. reflexivity. Qed.

(** * Milestone A — Project/rename transport ([scalar_select_list_4])

    FormalSQL exact Project of Dot-only [scalar_select_list_4] yields
    [project_row select_4 (native dots)]. Watched cor attrs then equal the
    corresponding native dots, so [row_correspond] transports
    [slot_map_outer] → [slot_map_outer_cor] on the same RelNode row. *)

Definition q21_select4_dot_values (ft : tuple TNull) : list value :=
  [ dot TNull ft (AttrInt32 "s_suppkey")
  ; dot TNull ft (AttrString "s_name" (StringChar 25))
  ; dot TNull ft (AttrString "s_address" (StringVarcharN 40))
  ; dot TNull ft (AttrInt32 "s_nationkey")
  ; dot TNull ft (AttrString "s_phone" (StringChar 15))
  ; dot TNull ft (AttrDecimal "s_acctbal" 15 2)
  ; dot TNull ft (AttrString "s_comment" (StringVarcharN 101))
  ; dot TNull ft (AttrInt32 "l_orderkey")
  ; dot TNull ft (AttrInt32 "l_partkey")
  ; dot TNull ft (AttrInt32 "l_suppkey")
  ; dot TNull ft (AttrInt32 "l_linenumber")
  ; dot TNull ft (AttrDecimal "l_quantity" 15 2)
  ; dot TNull ft (AttrDecimal "l_extendedprice" 15 2)
  ; dot TNull ft (AttrDecimal "l_discount" 15 2)
  ; dot TNull ft (AttrDecimal "l_tax" 15 2)
  ; dot TNull ft (AttrString "l_returnflag" (StringChar 1))
  ; dot TNull ft (AttrString "l_linestatus" (StringChar 1))
  ; dot TNull ft (AttrDate "l_shipdate")
  ; dot TNull ft (AttrDate "l_commitdate")
  ; dot TNull ft (AttrDate "l_receiptdate")
  ; dot TNull ft (AttrString "l_shipinstruct" (StringChar 25))
  ; dot TNull ft (AttrString "l_shipmode" (StringChar 10))
  ; dot TNull ft (AttrString "l_comment" (StringVarcharN 44))
  ; dot TNull ft (AttrInt32 "o_orderkey")
  ; dot TNull ft (AttrInt32 "o_custkey")
  ; dot TNull ft (AttrString "o_orderstatus" (StringChar 1))
  ; dot TNull ft (AttrDecimal "o_totalprice" 15 2)
  ; dot TNull ft (AttrDate "o_orderdate")
  ; dot TNull ft (AttrString "o_orderpriority" (StringChar 15))
  ; dot TNull ft (AttrString "o_clerk" (StringChar 15))
  ; dot TNull ft (AttrInt32 "o_shippriority")
  ; dot TNull ft (AttrString "o_comment" (StringVarcharN 79))
  ; dot TNull ft (AttrInt32 "n_nationkey")
  ; dot TNull ft (AttrString "n_name" (StringChar 25))
  ; dot TNull ft (AttrInt32 "n_regionkey")
  ; dot TNull ft (AttrString "n_comment" (StringVarcharN 152))
  ].

Definition q21_project_select4 (ft : tuple TNull) : tuple TNull :=
  project_row Queries.scalar_select_list_4 (q21_select4_dot_values ft).

(** Tiny reusable: project_row dot at a concrete output attr equals the
    paired value once [Fset.mem] is true (Q1-style discharge). *)
Ltac q21_discharge_project_row_dot :=
  unfold q21_project_select4, project_row, scalar_select_outputs,
    q21_select4_dot_values, Queries.scalar_select_list_4;
  cbn [map snd combine];
  rewrite dot_mk_tuple;
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end;
  cbn; reflexivity.

Lemma q21_select4_dot_s_suppkey :
  forall ft,
    dot TNull (q21_project_select4 ft) (Attr_int32 "__logos_cor_cor0_0") =
    dot TNull ft (Attr_int32 "s_suppkey").
Proof. intro ft; q21_discharge_project_row_dot. Qed.

Lemma q21_select4_dot_s_name :
  forall ft,
    dot TNull (q21_project_select4 ft)
      (Attr_string "__logos_cor_cor0_1" (StringChar 25)) =
    dot TNull ft (Attr_string "s_name" (StringChar 25)).
Proof. intro ft; q21_discharge_project_row_dot. Qed.

Lemma q21_select4_dot_s_nationkey :
  forall ft,
    dot TNull (q21_project_select4 ft) (Attr_int32 "__logos_cor_cor0_3") =
    dot TNull ft (Attr_int32 "s_nationkey").
Proof. intro ft; q21_discharge_project_row_dot. Qed.

Lemma q21_select4_dot_l_orderkey :
  forall ft,
    dot TNull (q21_project_select4 ft) (Attr_int32 "__logos_cor_cor0_7") =
    dot TNull ft (Attr_int32 "l_orderkey").
Proof. intro ft; q21_discharge_project_row_dot. Qed.

Lemma q21_select4_dot_l_suppkey :
  forall ft,
    dot TNull (q21_project_select4 ft) (Attr_int32 "__logos_cor_cor0_9") =
    dot TNull ft (Attr_int32 "l_suppkey").
Proof. intro ft; q21_discharge_project_row_dot. Qed.

Lemma q21_select4_dot_l_commitdate :
  forall ft,
    dot TNull (q21_project_select4 ft) (Attr_date "__logos_cor_cor0_18") =
    dot TNull ft (Attr_date "l_commitdate").
Proof. intro ft; q21_discharge_project_row_dot. Qed.

Lemma q21_select4_dot_l_receiptdate :
  forall ft,
    dot TNull (q21_project_select4 ft) (Attr_date "__logos_cor_cor0_19") =
    dot TNull ft (Attr_date "l_receiptdate").
Proof. intro ft; q21_discharge_project_row_dot. Qed.

Lemma q21_select4_dot_o_orderkey :
  forall ft,
    dot TNull (q21_project_select4 ft) (Attr_int32 "__logos_cor_cor0_23") =
    dot TNull ft (Attr_int32 "o_orderkey").
Proof. intro ft; q21_discharge_project_row_dot. Qed.

Lemma q21_select4_dot_o_orderstatus :
  forall ft,
    dot TNull (q21_project_select4 ft)
      (Attr_string "__logos_cor_cor0_25" (StringChar 1)) =
    dot TNull ft (Attr_string "o_orderstatus" (StringChar 1)).
Proof. intro ft; q21_discharge_project_row_dot. Qed.

Lemma q21_select4_dot_n_nationkey :
  forall ft,
    dot TNull (q21_project_select4 ft) (Attr_int32 "__logos_cor_cor0_32") =
    dot TNull ft (Attr_int32 "n_nationkey").
Proof. intro ft; q21_discharge_project_row_dot. Qed.

Lemma q21_select4_dot_n_name :
  forall ft,
    dot TNull (q21_project_select4 ft)
      (Attr_string "__logos_cor_cor0_33" (StringChar 25)) =
    dot TNull ft (Attr_string "n_name" (StringChar 25)).
Proof. intro ft; q21_discharge_project_row_dot. Qed.

Lemma q21_select4_watched_dots :
  forall ft,
    let ft' := q21_project_select4 ft in
    dot TNull ft' (Attr_int32 "__logos_cor_cor0_0") =
      dot TNull ft (Attr_int32 "s_suppkey") /\
    dot TNull ft' (Attr_string "__logos_cor_cor0_1" (StringChar 25)) =
      dot TNull ft (Attr_string "s_name" (StringChar 25)) /\
    dot TNull ft' (Attr_int32 "__logos_cor_cor0_3") =
      dot TNull ft (Attr_int32 "s_nationkey") /\
    dot TNull ft' (Attr_int32 "__logos_cor_cor0_7") =
      dot TNull ft (Attr_int32 "l_orderkey") /\
    dot TNull ft' (Attr_int32 "__logos_cor_cor0_9") =
      dot TNull ft (Attr_int32 "l_suppkey") /\
    dot TNull ft' (Attr_date "__logos_cor_cor0_18") =
      dot TNull ft (Attr_date "l_commitdate") /\
    dot TNull ft' (Attr_date "__logos_cor_cor0_19") =
      dot TNull ft (Attr_date "l_receiptdate") /\
    dot TNull ft' (Attr_int32 "__logos_cor_cor0_23") =
      dot TNull ft (Attr_int32 "o_orderkey") /\
    dot TNull ft' (Attr_string "__logos_cor_cor0_25" (StringChar 1)) =
      dot TNull ft (Attr_string "o_orderstatus" (StringChar 1)) /\
    dot TNull ft' (Attr_int32 "__logos_cor_cor0_32") =
      dot TNull ft (Attr_int32 "n_nationkey") /\
    dot TNull ft' (Attr_string "__logos_cor_cor0_33" (StringChar 25)) =
      dot TNull ft (Attr_string "n_name" (StringChar 25)).
Proof.
  intro ft.
  exact (conj (q21_select4_dot_s_suppkey ft)
        (conj (q21_select4_dot_s_name ft)
        (conj (q21_select4_dot_s_nationkey ft)
        (conj (q21_select4_dot_l_orderkey ft)
        (conj (q21_select4_dot_l_suppkey ft)
        (conj (q21_select4_dot_l_commitdate ft)
        (conj (q21_select4_dot_l_receiptdate ft)
        (conj (q21_select4_dot_o_orderkey ft)
        (conj (q21_select4_dot_o_orderstatus ft)
        (conj (q21_select4_dot_n_nationkey ft)
              (q21_select4_dot_n_name ft))))))))))).
Qed.

(** Milestone A target: native [row_correspond] + exact select_4 project
    (Dot values) ⇒ correlated [row_correspond] on the same RelNode row. *)
Lemma Q21_row_correspond_select4_rename :
  forall ft ft' rr,
    row_correspond slot_map_outer ft rr ->
    ft' = q21_project_select4 ft ->
    row_correspond slot_map_outer_cor ft' rr.
Proof.
  intros ft ft' rr [Hlen Hrc] Heq; subst ft'.
  split.
  - rewrite Hlen.
    change (List.length slot_map_outer) with (List.length slot_map_outer_cor).
    reflexivity.
  - intros i sb v Hsb Hv.
    assert (Hi : (i < 11)%nat).
    { pose proof (@nth_error_Some SlotBinding slot_map_outer_cor i) as Hne.
      apply Hne; clear Hne.
      intros Hnone; rewrite Hnone in Hsb; discriminate Hsb. }
    destruct (q21_select4_watched_dots ft) as
      [Hd0 [Hd1 [Hd3 [Hd7 [Hd9 [Hd18 [Hd19 [Hd23 [Hd25 [Hd32 Hd33]]]]]]]]]].
    unfold slot_map_outer_cor in Hsb.
    destruct i as [|i]; simpl in Hsb; [|simpl in Hi].
    { inversion Hsb; subst sb; clear Hsb; cbn [sb_formal slot_rel_type].
      rewrite Hd0.
      exact (Hrc 0%nat
        (mkSlotBinding (Attr_int32 "s_suppkey") (attr "s_suppkey" TInt))
        v eq_refl Hv). }
    destruct i as [|i]; simpl in Hsb; [|simpl in Hi].
    { inversion Hsb; subst sb; clear Hsb; cbn [sb_formal slot_rel_type].
      rewrite Hd1.
      exact (Hrc 1%nat
        (mkSlotBinding (Attr_string "s_name" (StringChar 25))
          (attr "s_name" TText))
        v eq_refl Hv). }
    destruct i as [|i]; simpl in Hsb; [|simpl in Hi].
    { inversion Hsb; subst sb; clear Hsb; cbn [sb_formal slot_rel_type].
      rewrite Hd3.
      exact (Hrc 2%nat
        (mkSlotBinding (Attr_int32 "s_nationkey") (attr "s_nationkey" TInt))
        v eq_refl Hv). }
    destruct i as [|i]; simpl in Hsb; [|simpl in Hi].
    { inversion Hsb; subst sb; clear Hsb; cbn [sb_formal slot_rel_type].
      rewrite Hd7.
      exact (Hrc 3%nat
        (mkSlotBinding (Attr_int32 "l_orderkey") (attr "l_orderkey" TInt))
        v eq_refl Hv). }
    destruct i as [|i]; simpl in Hsb; [|simpl in Hi].
    { inversion Hsb; subst sb; clear Hsb; cbn [sb_formal slot_rel_type].
      rewrite Hd9.
      exact (Hrc 4%nat
        (mkSlotBinding (Attr_int32 "l_suppkey") (attr "l_suppkey" TInt))
        v eq_refl Hv). }
    destruct i as [|i]; simpl in Hsb; [|simpl in Hi].
    { inversion Hsb; subst sb; clear Hsb; cbn [sb_formal slot_rel_type].
      rewrite Hd18.
      exact (Hrc 5%nat
        (mkSlotBinding (Attr_date "l_commitdate") (attr "l_commitdate" TDate))
        v eq_refl Hv). }
    destruct i as [|i]; simpl in Hsb; [|simpl in Hi].
    { inversion Hsb; subst sb; clear Hsb; cbn [sb_formal slot_rel_type].
      rewrite Hd19.
      exact (Hrc 6%nat
        (mkSlotBinding (Attr_date "l_receiptdate") (attr "l_receiptdate" TDate))
        v eq_refl Hv). }
    destruct i as [|i]; simpl in Hsb; [|simpl in Hi].
    { inversion Hsb; subst sb; clear Hsb; cbn [sb_formal slot_rel_type].
      rewrite Hd23.
      exact (Hrc 7%nat
        (mkSlotBinding (Attr_int32 "o_orderkey") (attr "o_orderkey" TInt))
        v eq_refl Hv). }
    destruct i as [|i]; simpl in Hsb; [|simpl in Hi].
    { inversion Hsb; subst sb; clear Hsb; cbn [sb_formal slot_rel_type].
      rewrite Hd25.
      exact (Hrc 8%nat
        (mkSlotBinding (Attr_string "o_orderstatus" (StringChar 1))
          (attr "o_orderstatus" TText))
        v eq_refl Hv). }
    destruct i as [|i]; simpl in Hsb; [|simpl in Hi].
    { inversion Hsb; subst sb; clear Hsb; cbn [sb_formal slot_rel_type].
      rewrite Hd32.
      exact (Hrc 9%nat
        (mkSlotBinding (Attr_int32 "n_nationkey") (attr "n_nationkey" TInt))
        v eq_refl Hv). }
    destruct i as [|i]; simpl in Hsb; [|simpl in Hi; lia].
    { inversion Hsb; subst sb; clear Hsb; cbn [sb_formal slot_rel_type].
      rewrite Hd33.
      exact (Hrc 10%nat
        (mkSlotBinding (Attr_string "n_name" (StringChar 25))
          (attr "n_name" TText))
        v eq_refl Hv). }
Qed.

(** * Milestone B — Ordinary ConjList acceptance ↔ [pred_ordinary_outer]

    Under [row_correspond slot_map_outer_cor], FormalSQL six-conjunct
    [ordinary_outer_conjlist] accepts a tuple (SQL TRUE only) iff RelNode
    [eval_pred_keep pred_ordinary_outer] is [Some true]. *)

(** Named FormalSQL leaves (exact lowered / nation-substituted shapes). *)
Definition q21_fsql_pred_suppkey :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateEq
    [@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_0");
     @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_9")].

Definition q21_fsql_pred_orderkey :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateEq
    [@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_23");
     @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_7")].

Definition q21_fsql_pred_status_F :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateEq
    [@SExpr_Leaf TNull relname type_string
       (DotString "__logos_cor_cor0_25" (StringChar 1));
     @SExpr_Leaf TNull relname type_string (CstString StringBpchar "F")].

Definition q21_fsql_pred_late :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateGt
    [@SExpr_Leaf TNull relname type_date (DotDate "__logos_cor_cor0_19");
     @SExpr_Leaf TNull relname type_date (DotDate "__logos_cor_cor0_18")].

Definition q21_fsql_pred_nationkey :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateEq
    [@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_3");
     @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_32")].

Definition q21_fsql_pred_nation (nation_name : string) :
    @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname PredicateEq
    [@SExpr_Leaf TNull relname type_string
       (DotString "__logos_cor_cor0_33" (StringChar 25));
     @SExpr_Leaf TNull relname type_string (CstString StringBpchar nation_name)].

Lemma ordinary_outer_conjlist_shape :
  forall nn,
    ordinary_outer_conjlist nn =
      SExpr_ConjList
        ([[]; ["b0"]; ["b1"; "b2"]; ["b3"; "b4"; "b5"];
          ["b6"; "b7"; "b8"; "b9"]; ["b10"; "b11"]])%list
        And_F
        [q21_fsql_pred_suppkey; q21_fsql_pred_orderkey; q21_fsql_pred_status_F;
         q21_fsql_pred_late; q21_fsql_pred_nationkey; q21_fsql_pred_nation nn].
Proof.
  intro nn.
  unfold ordinary_outer_conjlist, Queries.scalar_expr_predicate_1.
  simpl.
  unfold subst_nation_sexpr_bool; simpl.
  unfold subst_nation_sexpr_val, subst_nation_aggterm; simpl.
  reflexivity.
Qed.

(** Watched cor attrs present, with Attr-aligned FormalSQL carriers
    (int32 / date / CHAR) so leaf 3VL bridges apply without mixed-tag gaps. *)
Definition q21_outer_cor_labels_ok (ft : tuple TNull) : Prop :=
  Attr_int32 "__logos_cor_cor0_0" inS labels TNull ft /\
  Attr_int32 "__logos_cor_cor0_9" inS labels TNull ft /\
  Attr_int32 "__logos_cor_cor0_23" inS labels TNull ft /\
  Attr_int32 "__logos_cor_cor0_7" inS labels TNull ft /\
  Attr_string "__logos_cor_cor0_25" (StringChar 1) inS labels TNull ft /\
  Attr_date "__logos_cor_cor0_19" inS labels TNull ft /\
  Attr_date "__logos_cor_cor0_18" inS labels TNull ft /\
  Attr_int32 "__logos_cor_cor0_3" inS labels TNull ft /\
  Attr_int32 "__logos_cor_cor0_32" inS labels TNull ft /\
  Attr_string "__logos_cor_cor0_33" (StringChar 25) inS labels TNull ft /\
  match dot TNull ft (Attr_int32 "__logos_cor_cor0_0") with
  | Value_int32 _ => True | _ => False end /\
  match dot TNull ft (Attr_int32 "__logos_cor_cor0_9") with
  | Value_int32 _ => True | _ => False end /\
  match dot TNull ft (Attr_int32 "__logos_cor_cor0_23") with
  | Value_int32 _ => True | _ => False end /\
  match dot TNull ft (Attr_int32 "__logos_cor_cor0_7") with
  | Value_int32 _ => True | _ => False end /\
  match dot TNull ft (Attr_date "__logos_cor_cor0_19") with
  | Value_date _ => True | _ => False end /\
  match dot TNull ft (Attr_date "__logos_cor_cor0_18") with
  | Value_date _ => True | _ => False end /\
  match dot TNull ft (Attr_int32 "__logos_cor_cor0_3") with
  | Value_int32 _ => True | _ => False end /\
  match dot TNull ft (Attr_int32 "__logos_cor_cor0_32") with
  | Value_int32 _ => True | _ => False end.

(** Char carriers for text leaves — typmod pinned to the slot Attr
    ([StringChar 1] / [StringChar 25]). Same facts already justified by
    [DB_CharCanonical_Q21] on embed; required by [VC_status_F_3vl] /
    [VC_nation_name_3vl]. *)
Definition q21_outer_cor_text_canonical (ft : tuple TNull) : Prop :=
  match dot TNull ft (Attr_string "__logos_cor_cor0_25" (StringChar 1)) with
  | Value_string (StringChar 1, Some s) => Char1Canonical s
  | Value_string (StringChar 1, None) => True
  | _ => False
  end /\
  match dot TNull ft (Attr_string "__logos_cor_cor0_33" (StringChar 25)) with
  | Value_string (StringChar 25, Some s) => Char25Canonical s
  | Value_string (StringChar 25, None) => True
  | _ => False
  end.

Lemma formal_is_true_eq_Bool_is_true :
  forall b, formal_is_true b = Bool.is_true Bool3 b.
Proof. destruct b; reflexivity. Qed.

(** RelNode: [BAnd] keep bit is boolean AND of child keep bits (WHERE TRUE-only).
    Holds because keep-true requires [VBool true] on both sides. *)
Lemma eval_pred_keep_band :
  forall (p : Params) (r : Row) (a b : Pred) (ba bb : bool),
    eval_pred_keep a p r = Some ba ->
    eval_pred_keep b p r = Some bb ->
    eval_pred_keep (EBinary BAnd a b) p r = Some (andb ba bb).
Proof.
  intros p r a b ba bb Ha Hb.
  unfold eval_pred_keep, eval_pred in Ha, Hb |- *.
  destruct (eval_expr a p (EvRow r)) as [va|] eqn:Hea; try discriminate.
  destruct (eval_expr b p (EvRow r)) as [vb|] eqn:Heb; try discriminate.
  simpl; rewrite Hea, Heb; simpl.
  destruct va as [| |ba0| |]; try discriminate;
  destruct vb as [| |bb0| |]; try discriminate;
  simpl in Ha, Hb |- *.
  - (* Null, Null *) inversion Ha; inversion Hb; subst; reflexivity.
  - (* Null, Bool *) inversion Ha; subst; destruct bb0; inversion Hb; subst; reflexivity.
  - (* Bool, Null *)
    inversion Hb; subst.
    destruct ba0; simpl in Ha; inversion Ha; subst; reflexivity.
  - (* Bool, Bool *)
    destruct ba0; destruct bb0; simpl in Ha, Hb;
      inversion Ha; inversion Hb; subst; reflexivity.
Qed.

Lemma eval_pred_keep_ordinary_as_andb6 :
  forall (p : Params) (r : Row) b0 b1 b2 b3 b4 b5,
    eval_pred_keep pred_eq_supp_l1 p r = Some b0 ->
    eval_pred_keep pred_eq_ord_l1 p r = Some b1 ->
    eval_pred_keep pred_status_F p r = Some b2 ->
    eval_pred_keep pred_late_l1 p r = Some b3 ->
    eval_pred_keep pred_eq_nationkey p r = Some b4 ->
    eval_pred_keep pred_nation_param p r = Some b5 ->
    eval_pred_keep pred_ordinary_outer p r =
      Some (andb b0 (andb b1 (andb b2 (andb b3 (andb b4 b5))))).
Proof.
  intros p r b0 b1 b2 b3 b4 b5 H0 H1 H2 H3 H4 H5.
  unfold pred_ordinary_outer.
  eapply eq_trans.
  { eapply eval_pred_keep_band; [exact H0|].
    eapply eval_pred_keep_band; [exact H1|].
    eapply eval_pred_keep_band; [exact H2|].
    eapply eval_pred_keep_band; [exact H3|].
    eapply eval_pred_keep_band; [exact H4|exact H5]. }
  reflexivity.
Qed.

(** Extract RelNode slot values under [row_correspond]. *)
Lemma row_correspond_cor_nth :
  forall ft rr i sb v,
    row_correspond slot_map_outer_cor ft rr ->
    nth_error slot_map_outer_cor i = Some sb ->
    nth_error rr i = Some v ->
    value_correspond (slot_rel_type sb) (dot TNull ft (sb_formal sb)) v.
Proof.
  intros; eapply row_correspond_nth; eassumption.
Qed.

(** Local abbreviations for Milestone B FormalSQL exactness. *)
Section Q21OrdinaryConjAcceptance.
Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Local Abbreviation accept_at :=
  (@scalar_expr_acceptance_exact_at TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule).

Local Abbreviation values_exact :=
  (@scalar_value_list_exact_at TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule).

Lemma q21_dot_exact_int32 :
  forall env ft name,
    Attr_int32 name inS labels TNull ft ->
    env = Env.env_t TNull nil ft ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Leaf TNull relname type_int32 (DotInt32 name))
      (dot TNull ft (Attr_int32 name)).
Proof.
  intros env ft name Hin ->.
  apply tnull_dot_int32_exact.
  apply interp_dot_env_t; exact Hin.
Qed.

Lemma q21_dot_exact_date :
  forall env ft name,
    Attr_date name inS labels TNull ft ->
    env = Env.env_t TNull nil ft ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Leaf TNull relname type_date (DotDate name))
      (dot TNull ft (Attr_date name)).
Proof.
  intros env ft name Hin ->.
  apply tnull_dot_date_exact.
  apply interp_dot_env_t; exact Hin.
Qed.

Lemma q21_dot_exact_string :
  forall env ft name tm,
    Attr_string name tm inS labels TNull ft ->
    env = Env.env_t TNull nil ft ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Leaf TNull relname type_string (DotString name tm))
      (dot TNull ft (Attr_string name tm)).
Proof.
  intros env ft name tm Hin ->.
  apply tnull_dot_string_exact.
  apply interp_dot_env_t; exact Hin.
Qed.

(** RelNode leaf keep from typed slot values. *)
Lemma eval_pred_keep_beq_slots :
  forall (p : Params) (r : Row) (i j : nat) (vi vj : SqlValue),
    nth_error r i = Some vi ->
    nth_error r j = Some vj ->
    eval_pred_keep
      (EBinary BEq (EInputRef i) (EInputRef j)) p r =
    Some (truth3_is_true (cmp_int_eq vi vj)).
Proof.
  intros p r i j vi vj Hi Hj.
  unfold eval_pred_keep, eval_pred, eval_expr, ctx_lookup_input, row_nth.
  rewrite Hi, Hj; simpl.
  change (cmp_truth BEq vi vj) with (cmp_int_eq vi vj).
  destruct (cmp_int_eq vi vj); reflexivity.
Qed.

Lemma eval_pred_keep_bgt_slots :
  forall (p : Params) (r : Row) (i j : nat) (vi vj : SqlValue),
    nth_error r i = Some vi ->
    nth_error r j = Some vj ->
    eval_pred_keep
      (EBinary BGt (EInputRef i) (EInputRef j)) p r =
    Some (truth3_is_true (cmp_truth BGt vi vj)).
Proof.
  intros p r i j vi vj Hi Hj.
  unfold eval_pred_keep, eval_pred, eval_expr, ctx_lookup_input, row_nth.
  rewrite Hi, Hj; simpl.
  unfold cmp_truth; simpl.
  destruct (truth3_and (cmp_int_leb vj vi) (truth3_not (cmp_int_eq vi vj)));
    reflexivity.
Qed.

Lemma eval_pred_keep_beq_const_text :
  forall (p : Params) (r : Row) (i : nat) (vi : SqlValue) (s : string),
    nth_error r i = Some vi ->
    eval_pred_keep
      (EBinary BEq (EInputRef i) (EConst (VText s) TText)) p r =
    Some (truth3_is_true (cmp_int_eq vi (VText s))).
Proof.
  intros p r i vi s Hi.
  unfold eval_pred_keep, eval_pred, eval_expr, ctx_lookup_input, row_nth.
  rewrite Hi; simpl.
  change (cmp_truth BEq vi (VText s)) with (cmp_int_eq vi (VText s)).
  destruct (cmp_int_eq vi (VText s)); reflexivity.
Qed.

Lemma eval_pred_keep_beq_param_text_slots :
  forall nation_name (r : Row) (i : nat) (vi : SqlValue),
    nth_error r i = Some vi ->
    eval_pred_keep
      (EBinary BEq (EInputRef i) param_nation_name)
      (Params_Q21 nation_name) r =
    Some (truth3_is_true (cmp_int_eq vi (VText nation_name))).
Proof.
  intros nation_name r i vi Hi.
  unfold eval_pred_keep, eval_pred, eval_expr, ctx_lookup_input, row_nth,
    Params_Q21, params_nation, params_lookup, param_nation_name.
  rewrite Hi; cbn.
  change (cmp_truth BEq vi (VText nation_name)) with
    (cmp_int_eq vi (VText nation_name)).
  destruct (cmp_int_eq vi (VText nation_name)); reflexivity.
Qed.

(** ValueCorrespond lifts for Q21 int32 / date / text leaves (NULL → reject). *)
Lemma VC_int32_eq_keep_some :
  forall i j,
    Bool.is_true Bool3
      (interp_predicate PredicateEq
         [Value_int32 (Some i); Value_int32 (Some j)]) =
    truth3_is_true
      (cmp_int_eq (VInt (int32_value i)) (VInt (int32_value j))).
Proof.
  intros i j.
  rewrite <- formal_is_true_eq_Bool_is_true.
  apply VC_int32_eq_3vl.
Qed.

Lemma VC_int32_eq_keep_null_l :
  forall fv,
    Bool.is_true Bool3
      (interp_predicate PredicateEq [Value_int32 None; fv]) = false.
Proof. intro fv; destruct fv; reflexivity. Qed.

Lemma VC_int32_eq_keep_null_r :
  forall i,
    Bool.is_true Bool3
      (interp_predicate PredicateEq
         [Value_int32 (Some i); Value_int32 None]) = false.
Proof. intro; reflexivity. Qed.

Lemma VC_date_gt_keep_some :
  forall d1 d2,
    Bool.is_true Bool3
      (interp_predicate PredicateGt
         [Value_date (Some d1); Value_date (Some d2)]) =
    truth3_is_true (cmp_truth BGt (VInt d1) (VInt d2)).
Proof.
  intros d1 d2.
  rewrite <- formal_is_true_eq_Bool_is_true.
  apply VC_date_gt_3vl.
Qed.

Lemma VC_date_gt_keep_null_l :
  forall fv,
    Bool.is_true Bool3
      (interp_predicate PredicateGt [Value_date None; fv]) = false.
Proof. intro fv; destruct fv; reflexivity. Qed.

Lemma VC_date_gt_keep_null_r :
  forall d,
    Bool.is_true Bool3
      (interp_predicate PredicateGt
         [Value_date (Some d); Value_date None]) = false.
Proof. intro; reflexivity. Qed.

Lemma VC_status_F_keep_of_correspond :
  forall s rv,
    value_correspond TText
      (Value_string (StringValue (StringChar 1) (Some s))) rv ->
    Char1Canonical s ->
    Bool.is_true Bool3
      (interp_predicate PredicateEq
         [Value_string (StringValue (StringChar 1) (Some s));
          Value_string (StringValue StringBpchar
            (Some (string_explicit_cast StringBpchar "F")))]) =
    truth3_is_true (cmp_int_eq rv (VText "F")).
Proof.
  intros s rv Hvc Hcan.
  inversion Hvc; subst.
  rewrite <- formal_is_true_eq_Bool_is_true.
  apply VC_status_F_3vl; exact Hcan.
Qed.

Lemma VC_status_F_keep_null :
  Bool.is_true Bool3
    (interp_predicate PredicateEq
       [Value_string (StringValue (StringChar 1) None);
        Value_string (StringValue StringBpchar
          (Some (string_explicit_cast StringBpchar "F")))]) = false.
Proof. reflexivity. Qed.

Lemma VC_nation_name_keep_of_correspond :
  forall s nation_name rv,
    value_correspond TText
      (Value_string (StringValue (StringChar 25) (Some s))) rv ->
    Char25Canonical s ->
    NationNameOk nation_name ->
    Bool.is_true Bool3
      (interp_predicate PredicateEq
         [Value_string (StringValue (StringChar 25) (Some s));
          Value_string (StringValue StringBpchar
            (Some (string_explicit_cast StringBpchar nation_name)))]) =
    truth3_is_true (cmp_int_eq rv (VText nation_name)).
Proof.
  intros s nation_name rv Hvc Hcan Hnn.
  inversion Hvc; subst.
  rewrite <- formal_is_true_eq_Bool_is_true.
  apply VC_nation_name_3vl; assumption.
Qed.

Lemma VC_nation_name_keep_null :
  forall nation_name,
    Bool.is_true Bool3
      (interp_predicate PredicateEq
         [Value_string (StringValue (StringChar 25) None);
          Value_string (StringValue StringBpchar
            (Some (string_explicit_cast StringBpchar nation_name)))]) = false.
Proof. intro; reflexivity. Qed.

(** Helper: pred acceptance from exact Dot/Const values. *)
Lemma q21_pred_accept_of_values :
  forall env pred args values,
    values_exact env args values ->
    accept_at env (SExpr_Pred pred args)
      (Bool.is_true Bool3 (interp_predicate pred values)).
Proof.
  intros env pred args values Hex.
  eapply (@pred_acceptance_of_value_list TNull relname basesort instance
    unknown3 NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule); exact Hex.
Qed.

(** Row length under correspond. *)
Lemma row_correspond_cor_length :
  forall ft rr,
    row_correspond slot_map_outer_cor ft rr ->
    List.length rr = 11%nat.
Proof.
  intros ft rr [Hlen _].
  rewrite Hlen; reflexivity.
Qed.

Lemma nth_error_rr_cor :
  forall ft rr i,
    row_correspond slot_map_outer_cor ft rr ->
    (i < 11)%nat ->
    exists v, nth_error rr i = Some v.
Proof.
  intros ft rr i Hrc Hi.
  pose proof (row_correspond_cor_length _ _ Hrc) as Hlen.
  destruct (nth_error rr i) as [v|] eqn:Heq.
  - exists v; reflexivity.
  - apply nth_error_None in Heq.
    rewrite Hlen in Heq; lia.
Qed.

(** Reduce [slot_rel_type] on concrete outer-cor bindings. *)
Lemma vc_cor_int32 :
  forall name rel_name o v,
    value_correspond
      (slot_rel_type
         (mkSlotBinding (Attr_int32 name) (attr rel_name TInt)))
      (Value_int32 o) v ->
    value_correspond TInt (Value_int32 o) v.
Proof. intros; assumption. Qed.

Lemma vc_cor_date :
  forall name rel_name o v,
    value_correspond
      (slot_rel_type
         (mkSlotBinding (Attr_date name) (attr rel_name TDate)))
      (Value_date o) v ->
    value_correspond TDate (Value_date o) v.
Proof. intros; assumption. Qed.

Lemma vc_cor_text :
  forall name tm rel_name sv v,
    value_correspond
      (slot_rel_type
         (mkSlotBinding (Attr_string name tm) (attr rel_name TText)))
      (Value_string sv) v ->
    value_correspond TText (Value_string sv) v.
Proof. intros; assumption. Qed.

Lemma q21_int32_pair_keep :
  forall o1 o2 rv1 rv2,
    value_correspond TInt (Value_int32 o1) rv1 ->
    value_correspond TInt (Value_int32 o2) rv2 ->
    Bool.is_true Bool3
      (interp_predicate PredicateEq [Value_int32 o1; Value_int32 o2]) =
    truth3_is_true (cmp_int_eq rv1 rv2).
Proof.
  intros o1 o2 rv1 rv2 H1 H2.
  inversion H1; subst; inversion H2; subst;
    rewrite <- formal_is_true_eq_Bool_is_true;
    try apply VC_int32_eq_3vl; reflexivity.
Qed.

Lemma q21_date_pair_keep :
  forall o1 o2 rv1 rv2,
    value_correspond TDate (Value_date o1) rv1 ->
    value_correspond TDate (Value_date o2) rv2 ->
    Bool.is_true Bool3
      (interp_predicate PredicateGt [Value_date o1; Value_date o2]) =
    truth3_is_true (cmp_truth BGt rv1 rv2).
Proof.
  intros o1 o2 rv1 rv2 H1 H2.
  inversion H1; subst; inversion H2; subst;
    rewrite <- formal_is_true_eq_Bool_is_true;
    try apply VC_date_gt_3vl; reflexivity.
Qed.

(** Exact two-Dot int32 argument list. *)
Lemma q21_values_exact_int32_pair :
  forall env ft n1 n2,
    env = Env.env_t TNull nil ft ->
    Attr_int32 n1 inS labels TNull ft ->
    Attr_int32 n2 inS labels TNull ft ->
    values_exact env
      [@SExpr_Leaf TNull relname type_int32 (DotInt32 n1);
       @SExpr_Leaf TNull relname type_int32 (DotInt32 n2)]
      [dot TNull ft (Attr_int32 n1); dot TNull ft (Attr_int32 n2)].
Proof.
  intros env ft n1 n2 -> H1 H2.
  eapply scalar_value_list_exact_cons;
    [eapply q21_dot_exact_int32; [exact H1|reflexivity]|].
  eapply scalar_value_list_exact_cons;
    [eapply q21_dot_exact_int32; [exact H2|reflexivity]|].
  apply scalar_value_list_exact_nil.
Qed.

Lemma q21_values_exact_date_pair :
  forall env ft n1 n2,
    env = Env.env_t TNull nil ft ->
    Attr_date n1 inS labels TNull ft ->
    Attr_date n2 inS labels TNull ft ->
    values_exact env
      [@SExpr_Leaf TNull relname type_date (DotDate n1);
       @SExpr_Leaf TNull relname type_date (DotDate n2)]
      [dot TNull ft (Attr_date n1); dot TNull ft (Attr_date n2)].
Proof.
  intros env ft n1 n2 -> H1 H2.
  eapply scalar_value_list_exact_cons;
    [eapply q21_dot_exact_date; [exact H1|reflexivity]|].
  eapply scalar_value_list_exact_cons;
    [eapply q21_dot_exact_date; [exact H2|reflexivity]|].
  apply scalar_value_list_exact_nil.
Qed.

(** Projectors for [q21_outer_cor_labels_ok] (avoid brittle 18-way destruct). *)
Lemma labels_ok_suppkey :
  forall ft,
    q21_outer_cor_labels_ok ft ->
    Attr_int32 "__logos_cor_cor0_0" inS labels TNull ft /\
    Attr_int32 "__logos_cor_cor0_9" inS labels TNull ft /\
    match dot TNull ft (Attr_int32 "__logos_cor_cor0_0") with
    | Value_int32 _ => True | _ => False end /\
    match dot TNull ft (Attr_int32 "__logos_cor_cor0_9") with
    | Value_int32 _ => True | _ => False end.
Proof.
  intros ft H; unfold q21_outer_cor_labels_ok in H.
  destruct H as [A [B [_ [_ [_ [_ [_ [_ [_ [_ [C [D _]]]]]]]]]]]].
  repeat split; assumption.
Qed.

Lemma labels_ok_orderkey :
  forall ft,
    q21_outer_cor_labels_ok ft ->
    Attr_int32 "__logos_cor_cor0_23" inS labels TNull ft /\
    Attr_int32 "__logos_cor_cor0_7" inS labels TNull ft /\
    match dot TNull ft (Attr_int32 "__logos_cor_cor0_23") with
    | Value_int32 _ => True | _ => False end /\
    match dot TNull ft (Attr_int32 "__logos_cor_cor0_7") with
    | Value_int32 _ => True | _ => False end.
Proof.
  intros ft H; unfold q21_outer_cor_labels_ok in H.
  destruct H as [_ [_ [A [B [_ [_ [_ [_ [_ [_ [_ [_ [C [D _]]]]]]]]]]]]]].
  repeat split; assumption.
Qed.

Lemma labels_ok_nationkey :
  forall ft,
    q21_outer_cor_labels_ok ft ->
    Attr_int32 "__logos_cor_cor0_3" inS labels TNull ft /\
    Attr_int32 "__logos_cor_cor0_32" inS labels TNull ft /\
    match dot TNull ft (Attr_int32 "__logos_cor_cor0_3") with
    | Value_int32 _ => True | _ => False end /\
    match dot TNull ft (Attr_int32 "__logos_cor_cor0_32") with
    | Value_int32 _ => True | _ => False end.
Proof.
  intros ft H; unfold q21_outer_cor_labels_ok in H.
  destruct H as [_ [_ [_ [_ [_ [_ [_ [A [B [_ [_ [_ [_ [_ [_ [_ [C D]]]]]]]]]]]]]]]]].
  repeat split; assumption.
Qed.

Lemma labels_ok_late :
  forall ft,
    q21_outer_cor_labels_ok ft ->
    Attr_date "__logos_cor_cor0_19" inS labels TNull ft /\
    Attr_date "__logos_cor_cor0_18" inS labels TNull ft /\
    match dot TNull ft (Attr_date "__logos_cor_cor0_19") with
    | Value_date _ => True | _ => False end /\
    match dot TNull ft (Attr_date "__logos_cor_cor0_18") with
    | Value_date _ => True | _ => False end.
Proof.
  intros ft H; unfold q21_outer_cor_labels_ok in H.
  destruct H as [_ [_ [_ [_ [_ [A [B [_ [_ [_ [_ [_ [_ [_ [C [D _]]]]]]]]]]]]]]]].
  repeat split; assumption.
Qed.

Lemma labels_ok_status :
  forall ft,
    q21_outer_cor_labels_ok ft ->
    Attr_string "__logos_cor_cor0_25" (StringChar 1) inS labels TNull ft.
Proof.
  intros ft H; unfold q21_outer_cor_labels_ok in H.
  destruct H as [_ [_ [_ [_ [A _]]]]]; exact A.
Qed.

Lemma labels_ok_n_name :
  forall ft,
    q21_outer_cor_labels_ok ft ->
    Attr_string "__logos_cor_cor0_33" (StringChar 25) inS labels TNull ft.
Proof.
  intros ft H; unfold q21_outer_cor_labels_ok in H.
  destruct H as [_ [_ [_ [_ [_ [_ [_ [_ [_ [A _]]]]]]]]]]; exact A.
Qed.

(** * Six leaf same-bit bridges *)

Lemma Q21_leaf_suppkey_same_bit :
  forall ft rr (p : Params),
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    exists b,
      accept_at (Env.env_t TNull nil ft) q21_fsql_pred_suppkey b /\
      eval_pred_keep pred_eq_supp_l1 p rr = Some b.
Proof.
  intros ft rr p Hrc Hlab.
  destruct (labels_ok_suppkey ft Hlab) as [Hl0 [Hl9 [Hs0 Hs9]]].
  destruct (nth_error_rr_cor _ _ 0%nat Hrc ltac:(lia)) as [v0 Hv0].
  destruct (nth_error_rr_cor _ _ 4%nat Hrc ltac:(lia)) as [v4 Hv4].
  pose proof (row_correspond_cor_nth ft rr 0%nat _ v0 Hrc eq_refl Hv0) as Hc0.
  pose proof (row_correspond_cor_nth ft rr 4%nat _ v4 Hrc eq_refl Hv4) as Hc4.
  cbn [sb_formal] in Hc0, Hc4.
  set (env := Env.env_t TNull nil ft).
  destruct (dot TNull ft (Attr_int32 "__logos_cor_cor0_0"))
    as [ | | o0 | | | | | | | | | ] eqn:Ed0; try (exfalso; exact Hs0).
  destruct (dot TNull ft (Attr_int32 "__logos_cor_cor0_9"))
    as [ | | o9 | | | | | | | | | ] eqn:Ed9; try (exfalso; exact Hs9).
  pose proof (eval_pred_keep_beq_slots p rr 0%nat 4%nat v0 v4 Hv0 Hv4) as Hk.
  change (EBinary BEq (EInputRef 0) (EInputRef 4)) with pred_eq_supp_l1 in Hk.
  assert (Hex : values_exact env
    [@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_0");
     @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_9")]
    [Value_int32 o0; Value_int32 o9]).
  { rewrite <- Ed0, <- Ed9.
    eapply q21_values_exact_int32_pair; [reflexivity|exact Hl0|exact Hl9]. }
  pose proof (q21_pred_accept_of_values env PredicateEq _ _ Hex) as Ha.
  rewrite (q21_int32_pair_keep o0 o9 v0 v4 (vc_cor_int32 _ _ _ _ Hc0) (vc_cor_int32 _ _ _ _ Hc4)) in Ha.
  exists (truth3_is_true (cmp_int_eq v0 v4)); split; [exact Ha|exact Hk].
Qed.

Lemma Q21_leaf_orderkey_same_bit :
  forall ft rr (p : Params),
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    exists b,
      accept_at (Env.env_t TNull nil ft) q21_fsql_pred_orderkey b /\
      eval_pred_keep pred_eq_ord_l1 p rr = Some b.
Proof.
  intros ft rr p Hrc Hlab.
  destruct (labels_ok_orderkey ft Hlab) as [Hl23 [Hl7 [Hs23 Hs7]]].
  destruct (nth_error_rr_cor _ _ 7%nat Hrc ltac:(lia)) as [v7 Hv7].
  destruct (nth_error_rr_cor _ _ 3%nat Hrc ltac:(lia)) as [v3 Hv3].
  pose proof (row_correspond_cor_nth ft rr 7%nat _ v7 Hrc eq_refl Hv7) as Hc7.
  pose proof (row_correspond_cor_nth ft rr 3%nat _ v3 Hrc eq_refl Hv3) as Hc3.
  cbn [sb_formal] in Hc7, Hc3.
  set (env := Env.env_t TNull nil ft).
  destruct (dot TNull ft (Attr_int32 "__logos_cor_cor0_23"))
    as [ | | o23 | | | | | | | | | ] eqn:Ed23; try (exfalso; exact Hs23).
  destruct (dot TNull ft (Attr_int32 "__logos_cor_cor0_7"))
    as [ | | o7 | | | | | | | | | ] eqn:Ed7; try (exfalso; exact Hs7).
  pose proof (eval_pred_keep_beq_slots p rr 7%nat 3%nat v7 v3 Hv7 Hv3) as Hk.
  change (EBinary BEq (EInputRef 7) (EInputRef 3)) with pred_eq_ord_l1 in Hk.
  assert (Hex : values_exact env
    [@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_23");
     @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_7")]
    [Value_int32 o23; Value_int32 o7]).
  { rewrite <- Ed23, <- Ed7.
    eapply q21_values_exact_int32_pair; [reflexivity|exact Hl23|exact Hl7]. }
  pose proof (q21_pred_accept_of_values env PredicateEq _ _ Hex) as Ha.
  rewrite (q21_int32_pair_keep o23 o7 v7 v3 (vc_cor_int32 _ _ _ _ Hc7) (vc_cor_int32 _ _ _ _ Hc3)) in Ha.
  exists (truth3_is_true (cmp_int_eq v7 v3)); split; [exact Ha|exact Hk].
Qed.

Lemma Q21_leaf_nationkey_same_bit :
  forall ft rr (p : Params),
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    exists b,
      accept_at (Env.env_t TNull nil ft) q21_fsql_pred_nationkey b /\
      eval_pred_keep pred_eq_nationkey p rr = Some b.
Proof.
  intros ft rr p Hrc Hlab.
  destruct (labels_ok_nationkey ft Hlab) as [Hl3 [Hl32 [Hs3 Hs32]]].
  destruct (nth_error_rr_cor _ _ 2%nat Hrc ltac:(lia)) as [v2 Hv2].
  destruct (nth_error_rr_cor _ _ 9%nat Hrc ltac:(lia)) as [v9 Hv9].
  pose proof (row_correspond_cor_nth ft rr 2%nat _ v2 Hrc eq_refl Hv2) as Hc2.
  pose proof (row_correspond_cor_nth ft rr 9%nat _ v9 Hrc eq_refl Hv9) as Hc9.
  cbn [sb_formal] in Hc2, Hc9.
  set (env := Env.env_t TNull nil ft).
  destruct (dot TNull ft (Attr_int32 "__logos_cor_cor0_3"))
    as [ | | o3 | | | | | | | | | ] eqn:Ed3; try (exfalso; exact Hs3).
  destruct (dot TNull ft (Attr_int32 "__logos_cor_cor0_32"))
    as [ | | o32 | | | | | | | | | ] eqn:Ed32; try (exfalso; exact Hs32).
  pose proof (eval_pred_keep_beq_slots p rr 2%nat 9%nat v2 v9 Hv2 Hv9) as Hk.
  change (EBinary BEq (EInputRef 2) (EInputRef 9)) with pred_eq_nationkey in Hk.
  assert (Hex : values_exact env
    [@SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_3");
     @SExpr_Leaf TNull relname type_int32 (DotInt32 "__logos_cor_cor0_32")]
    [Value_int32 o3; Value_int32 o32]).
  { rewrite <- Ed3, <- Ed32.
    eapply q21_values_exact_int32_pair; [reflexivity|exact Hl3|exact Hl32]. }
  pose proof (q21_pred_accept_of_values env PredicateEq _ _ Hex) as Ha.
  rewrite (q21_int32_pair_keep o3 o32 v2 v9 (vc_cor_int32 _ _ _ _ Hc2) (vc_cor_int32 _ _ _ _ Hc9)) in Ha.
  exists (truth3_is_true (cmp_int_eq v2 v9)); split; [exact Ha|exact Hk].
Qed.

Lemma Q21_leaf_late_same_bit :
  forall ft rr (p : Params),
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    exists b,
      accept_at (Env.env_t TNull nil ft) q21_fsql_pred_late b /\
      eval_pred_keep pred_late_l1 p rr = Some b.
Proof.
  intros ft rr p Hrc Hlab.
  destruct (labels_ok_late ft Hlab) as [Hl19 [Hl18 [Hs19 Hs18]]].
  destruct (nth_error_rr_cor _ _ 6%nat Hrc ltac:(lia)) as [v6 Hv6].
  destruct (nth_error_rr_cor _ _ 5%nat Hrc ltac:(lia)) as [v5 Hv5].
  pose proof (row_correspond_cor_nth ft rr 6%nat _ v6 Hrc eq_refl Hv6) as Hc6.
  pose proof (row_correspond_cor_nth ft rr 5%nat _ v5 Hrc eq_refl Hv5) as Hc5.
  cbn [sb_formal] in Hc6, Hc5.
  set (env := Env.env_t TNull nil ft).
  destruct (dot TNull ft (Attr_date "__logos_cor_cor0_19"))
    as [ | | | | | | | | o19 | | | ] eqn:Ed19; try (exfalso; exact Hs19).
  destruct (dot TNull ft (Attr_date "__logos_cor_cor0_18"))
    as [ | | | | | | | | o18 | | | ] eqn:Ed18; try (exfalso; exact Hs18).
  pose proof (eval_pred_keep_bgt_slots p rr 6%nat 5%nat v6 v5 Hv6 Hv5) as Hk.
  change (EBinary BGt (EInputRef 6) (EInputRef 5)) with pred_late_l1 in Hk.
  assert (Hex : values_exact env
    [@SExpr_Leaf TNull relname type_date (DotDate "__logos_cor_cor0_19");
     @SExpr_Leaf TNull relname type_date (DotDate "__logos_cor_cor0_18")]
    [Value_date o19; Value_date o18]).
  { rewrite <- Ed19, <- Ed18.
    eapply q21_values_exact_date_pair; [reflexivity|exact Hl19|exact Hl18]. }
  pose proof (q21_pred_accept_of_values env PredicateGt _ _ Hex) as Ha.
  rewrite (q21_date_pair_keep o19 o18 v6 v5 (vc_cor_date _ _ _ _ Hc6) (vc_cor_date _ _ _ _ Hc5)) in Ha.
  exists (truth3_is_true (cmp_truth BGt v6 v5)); split; [exact Ha|exact Hk].
Qed.

Lemma Q21_leaf_status_F_same_bit :
  forall ft rr (p : Params),
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    q21_outer_cor_text_canonical ft ->
    exists b,
      accept_at (Env.env_t TNull nil ft) q21_fsql_pred_status_F b /\
      eval_pred_keep pred_status_F p rr = Some b.
Proof.
  intros ft rr p Hrc Hlab Htxt.
  pose proof (labels_ok_status ft Hlab) as Hl25.
  destruct Htxt as [Hst _].
  destruct (nth_error_rr_cor _ _ 8%nat Hrc ltac:(lia)) as [v8 Hv8].
  pose proof (row_correspond_cor_nth ft rr 8%nat _ v8 Hrc eq_refl Hv8) as Hc8.
  cbn [sb_formal] in Hc8.
  set (env := Env.env_t TNull nil ft).
  destruct (dot TNull ft (Attr_string "__logos_cor_cor0_25" (StringChar 1)))
    as [ sv | | | | | | | | | | | ] eqn:Ed25; try (exfalso; exact Hst).
  destruct sv as [tm os].
  destruct tm as [ | | | width | ]; try (exfalso; exact Hst).
  destruct (Nat.eq_dec width 1%nat) as [Heq|Hneq];
    [|exfalso; exact (match width as w
        return (w <> 1%nat ->
                match w with
                | 1%nat => match os with
                           | Some s => Char1Canonical s
                           | None => True
                           end
                | _ => False
                end -> False) with
      | 1%nat => fun Hneq _ => Hneq eq_refl
      | _ => fun _ Habs => Habs
      end Hneq Hst)].
  subst width.
  pose proof (eval_pred_keep_beq_const_text p rr 8%nat v8 "F" Hv8) as Hk.
  change (EBinary BEq (EInputRef 8) (EConst (VText "F") TText))
    with pred_status_F in Hk.
  destruct os as [s|].
  - assert (Hex : values_exact env
      [@SExpr_Leaf TNull relname type_string
         (DotString "__logos_cor_cor0_25" (StringChar 1));
       @SExpr_Leaf TNull relname type_string (CstString StringBpchar "F")]
      [Value_string (StringChar 1, Some s);
       Value_string (StringValue StringBpchar
         (Some (string_explicit_cast StringBpchar "F")))]).
    { eapply (@scalar_value_list_exact_cons TNull relname basesort instance
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        tnull_value_is_null boolean_schedule).
      - rewrite <- Ed25; eapply q21_dot_exact_string; [exact Hl25|reflexivity].
      - eapply (@scalar_value_list_exact_cons TNull relname basesort instance
          unknown3 NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error
          tnull_value_is_null boolean_schedule).
        + apply (tnull_leaf_constant_exact basesort instance boolean_schedule).
        + apply (@scalar_value_list_exact_nil TNull relname basesort instance
            unknown3 NullValues.interp_scalar_operator_runtime_error
            NullValues.interp_aggregate_runtime_error
            tnull_value_is_null boolean_schedule). }
    pose proof (q21_pred_accept_of_values env PredicateEq _ _ Hex) as Ha.
    pose proof (VC_status_F_keep_of_correspond s v8
      (vc_cor_text _ _ _ _ _ Hc8) Hst) as Hvc.
    unfold StringValue in Ha, Hvc.
    rewrite Hvc in Ha.
    exists (truth3_is_true (cmp_int_eq v8 (VText "F"))); split; [exact Ha|exact Hk].
  - inversion Hc8; subst.
    assert (Hex : values_exact env
      [@SExpr_Leaf TNull relname type_string
         (DotString "__logos_cor_cor0_25" (StringChar 1));
       @SExpr_Leaf TNull relname type_string (CstString StringBpchar "F")]
      [Value_string (StringChar 1, None);
       Value_string (StringValue StringBpchar
         (Some (string_explicit_cast StringBpchar "F")))]).
    { eapply (@scalar_value_list_exact_cons TNull relname basesort instance
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        tnull_value_is_null boolean_schedule).
      - rewrite <- Ed25; eapply q21_dot_exact_string; [exact Hl25|reflexivity].
      - eapply (@scalar_value_list_exact_cons TNull relname basesort instance
          unknown3 NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error
          tnull_value_is_null boolean_schedule).
        + apply (tnull_leaf_constant_exact basesort instance boolean_schedule).
        + apply (@scalar_value_list_exact_nil TNull relname basesort instance
            unknown3 NullValues.interp_scalar_operator_runtime_error
            NullValues.interp_aggregate_runtime_error
            tnull_value_is_null boolean_schedule). }
    pose proof (q21_pred_accept_of_values env PredicateEq _ _ Hex) as Ha.
    pose proof VC_status_F_keep_null as Hvc.
    unfold StringValue in Ha, Hvc.
    rewrite Hvc in Ha.
    exists false; split; [exact Ha|exact Hk].
Qed.

Lemma Q21_leaf_nation_name_same_bit :
  forall ft rr nation_name,
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    q21_outer_cor_text_canonical ft ->
    NationNameOk nation_name ->
    exists b,
      accept_at (Env.env_t TNull nil ft)
        (q21_fsql_pred_nation nation_name) b /\
      eval_pred_keep pred_nation_param (Params_Q21 nation_name) rr = Some b.
Proof.
  intros ft rr nation_name Hrc Hlab Htxt Hnn.
  pose proof (labels_ok_n_name ft Hlab) as Hl33.
  destruct Htxt as [_ Hnm].
  destruct (nth_error_rr_cor _ _ 10%nat Hrc ltac:(lia)) as [v10 Hv10].
  pose proof (row_correspond_cor_nth ft rr 10%nat _ v10 Hrc eq_refl Hv10) as Hc10.
  cbn [sb_formal] in Hc10.
  set (env := Env.env_t TNull nil ft).
  destruct (dot TNull ft (Attr_string "__logos_cor_cor0_33" (StringChar 25)))
    as [ sv | | | | | | | | | | | ] eqn:Ed33; try (exfalso; exact Hnm).
  destruct sv as [tm os].
  destruct tm as [ | | | width | ]; try (exfalso; exact Hnm).
  destruct (Nat.eq_dec width 25%nat) as [Heq|Hneq];
    [|exfalso; exact (match width as w
        return (w <> 25%nat ->
                match w with
                | 25%nat => match os with
                            | Some s => Char25Canonical s
                            | None => True
                            end
                | _ => False
                end -> False) with
      | 25%nat => fun Hneq _ => Hneq eq_refl
      | _ => fun _ Habs => Habs
      end Hneq Hnm)].
  subst width.
  pose proof (eval_pred_keep_beq_param_text_slots nation_name rr 10%nat v10 Hv10)
    as Hk.
  change (EBinary BEq (EInputRef 10) param_nation_name)
    with pred_nation_param in Hk.
  destruct os as [s|].
  - assert (Hex : values_exact env
      [@SExpr_Leaf TNull relname type_string
         (DotString "__logos_cor_cor0_33" (StringChar 25));
       @SExpr_Leaf TNull relname type_string (CstString StringBpchar nation_name)]
      [Value_string (StringChar 25, Some s);
       Value_string (StringValue StringBpchar
         (Some (string_explicit_cast StringBpchar nation_name)))]).
    { eapply (@scalar_value_list_exact_cons TNull relname basesort instance
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        tnull_value_is_null boolean_schedule).
      - rewrite <- Ed33; eapply q21_dot_exact_string; [exact Hl33|reflexivity].
      - eapply (@scalar_value_list_exact_cons TNull relname basesort instance
          unknown3 NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error
          tnull_value_is_null boolean_schedule).
        + apply (tnull_leaf_constant_exact basesort instance boolean_schedule).
        + apply (@scalar_value_list_exact_nil TNull relname basesort instance
            unknown3 NullValues.interp_scalar_operator_runtime_error
            NullValues.interp_aggregate_runtime_error
            tnull_value_is_null boolean_schedule). }
    pose proof (q21_pred_accept_of_values env PredicateEq _ _ Hex) as Ha.
    pose proof (VC_nation_name_keep_of_correspond s nation_name v10
      (vc_cor_text _ _ _ _ _ Hc10) Hnm Hnn) as Hvc.
    unfold StringValue in Ha, Hvc.
    rewrite Hvc in Ha.
    exists (truth3_is_true (cmp_int_eq v10 (VText nation_name)));
      split; [exact Ha|exact Hk].
  - inversion Hc10; subst.
    assert (Hex : values_exact env
      [@SExpr_Leaf TNull relname type_string
         (DotString "__logos_cor_cor0_33" (StringChar 25));
       @SExpr_Leaf TNull relname type_string (CstString StringBpchar nation_name)]
      [Value_string (StringChar 25, None);
       Value_string (StringValue StringBpchar
         (Some (string_explicit_cast StringBpchar nation_name)))]).
    { eapply (@scalar_value_list_exact_cons TNull relname basesort instance
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        tnull_value_is_null boolean_schedule).
      - rewrite <- Ed33; eapply q21_dot_exact_string; [exact Hl33|reflexivity].
      - eapply (@scalar_value_list_exact_cons TNull relname basesort instance
          unknown3 NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error
          tnull_value_is_null boolean_schedule).
        + apply (tnull_leaf_constant_exact basesort instance boolean_schedule).
        + apply (@scalar_value_list_exact_nil TNull relname basesort instance
            unknown3 NullValues.interp_scalar_operator_runtime_error
            NullValues.interp_aggregate_runtime_error
            tnull_value_is_null boolean_schedule). }
    pose proof (q21_pred_accept_of_values env PredicateEq _ _ Hex) as Ha.
    pose proof (VC_nation_name_keep_null nation_name) as Hvc.
    unfold StringValue in Ha, Hvc.
    rewrite Hvc in Ha.
    exists false; split; [exact Ha|exact Hk].
Qed.

Lemma accept_true_iff_keep_Some_true :
  forall env leaf (pred : Pred) (p : Params) rr b,
    accept_at env leaf b ->
    eval_pred_keep pred p rr = Some b ->
    (accept_at env leaf true <-> eval_pred_keep pred p rr = Some true).
Proof.
  intros env leaf pred p rr b Ha Hk.
  split; intro H.
  - destruct Ha as [[tr [Hev Htr]] [Huniq _]].
    destruct H as [[tr' [Hev' Htr']] _].
    pose proof (Huniq tr Hev) as Hb1.
    pose proof (Huniq tr' Hev') as Hb2.
    rewrite Hk.
    rewrite <- Hb1, Htr, <- Hb2, Htr'; reflexivity.
  - rewrite Hk in H; inversion H; subst b; exact Ha.
Qed.

Lemma Q21_leaf_suppkey_accept_iff :
  forall ft rr (p : Params),
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    accept_at (Env.env_t TNull nil ft) q21_fsql_pred_suppkey true
    <-> eval_pred_keep pred_eq_supp_l1 p rr = Some true.
Proof.
  intros ft rr p Hrc Hlab.
  destruct (Q21_leaf_suppkey_same_bit ft rr p Hrc Hlab) as [b [Ha Hk]].
  eapply accept_true_iff_keep_Some_true; eassumption.
Qed.

Lemma Q21_leaf_orderkey_accept_iff :
  forall ft rr (p : Params),
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    accept_at (Env.env_t TNull nil ft) q21_fsql_pred_orderkey true
    <-> eval_pred_keep pred_eq_ord_l1 p rr = Some true.
Proof.
  intros ft rr p Hrc Hlab.
  destruct (Q21_leaf_orderkey_same_bit ft rr p Hrc Hlab) as [b [Ha Hk]].
  eapply accept_true_iff_keep_Some_true; eassumption.
Qed.

Lemma Q21_leaf_nationkey_accept_iff :
  forall ft rr (p : Params),
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    accept_at (Env.env_t TNull nil ft) q21_fsql_pred_nationkey true
    <-> eval_pred_keep pred_eq_nationkey p rr = Some true.
Proof.
  intros ft rr p Hrc Hlab.
  destruct (Q21_leaf_nationkey_same_bit ft rr p Hrc Hlab) as [b [Ha Hk]].
  eapply accept_true_iff_keep_Some_true; eassumption.
Qed.

Lemma Q21_leaf_late_accept_iff :
  forall ft rr (p : Params),
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    accept_at (Env.env_t TNull nil ft) q21_fsql_pred_late true
    <-> eval_pred_keep pred_late_l1 p rr = Some true.
Proof.
  intros ft rr p Hrc Hlab.
  destruct (Q21_leaf_late_same_bit ft rr p Hrc Hlab) as [b [Ha Hk]].
  eapply accept_true_iff_keep_Some_true; eassumption.
Qed.

Lemma Q21_leaf_status_F_accept_iff :
  forall ft rr (p : Params),
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    q21_outer_cor_text_canonical ft ->
    accept_at (Env.env_t TNull nil ft) q21_fsql_pred_status_F true
    <-> eval_pred_keep pred_status_F p rr = Some true.
Proof.
  intros ft rr p Hrc Hlab Htxt.
  destruct (Q21_leaf_status_F_same_bit ft rr p Hrc Hlab Htxt) as [b [Ha Hk]].
  eapply accept_true_iff_keep_Some_true; eassumption.
Qed.

Lemma Q21_leaf_nation_name_accept_iff :
  forall ft rr nation_name,
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    q21_outer_cor_text_canonical ft ->
    NationNameOk nation_name ->
    accept_at (Env.env_t TNull nil ft) (q21_fsql_pred_nation nation_name) true
    <-> eval_pred_keep pred_nation_param (Params_Q21 nation_name) rr = Some true.
Proof.
  intros ft rr nation_name Hrc Hlab Htxt Hnn.
  destruct (Q21_leaf_nation_name_same_bit ft rr nation_name Hrc Hlab Htxt Hnn)
    as [b [Ha Hk]].
  eapply accept_true_iff_keep_Some_true; eassumption.
Qed.

(** * FormalSQL ConjList fold *)

(** Tag the six ordinary leaves by Dot names so [decide] is a closed Gallina
    function (needed by [scalar_expr_conj_list_acceptance_exact]). *)
Definition q21_ordinary_leaf_decide (nation_name : string)
    (b0 b1 b2 b3 b4 b5 : bool)
    (e : @scalar_expr TNull relname ScalarResultBoolean) : bool :=
  match e with
  | SExpr_Pred PredicateEq
      (SExpr_Leaf type_int32 (A_Expr _ (F_Dot _ (Attr_int32 n1))) ::
       SExpr_Leaf type_int32 (A_Expr _ (F_Dot _ (Attr_int32 n2))) :: nil) =>
      if (String.eqb n1 "__logos_cor_cor0_0" && String.eqb n2 "__logos_cor_cor0_9")%bool
      then b0
      else if (String.eqb n1 "__logos_cor_cor0_23" && String.eqb n2 "__logos_cor_cor0_7")%bool
      then b1
      else if (String.eqb n1 "__logos_cor_cor0_3" && String.eqb n2 "__logos_cor_cor0_32")%bool
      then b4
      else false
  | SExpr_Pred PredicateEq
      (SExpr_Leaf type_string (A_Expr _ (F_Dot _ (Attr_string n (StringChar 1)))) ::
       SExpr_Leaf type_string (A_Expr _ (F_Constant _ _)) :: nil) =>
      if String.eqb n "__logos_cor_cor0_25" then b2 else false
  | SExpr_Pred PredicateEq
      (SExpr_Leaf type_string (A_Expr _ (F_Dot _ (Attr_string n (StringChar 25)))) ::
       SExpr_Leaf type_string (A_Expr _ (F_Constant _ _)) :: nil) =>
      if String.eqb n "__logos_cor_cor0_33" then b5 else false
  | SExpr_Pred PredicateGt
      (SExpr_Leaf type_date (A_Expr _ (F_Dot _ (Attr_date n1))) ::
       SExpr_Leaf type_date (A_Expr _ (F_Dot _ (Attr_date n2))) :: nil) =>
      if (String.eqb n1 "__logos_cor_cor0_19" && String.eqb n2 "__logos_cor_cor0_18")%bool
      then b3 else false
  | _ => false
  end.

Lemma q21_ordinary_decide_map :
  forall nation_name b0 b1 b2 b3 b4 b5,
    map (q21_ordinary_leaf_decide nation_name b0 b1 b2 b3 b4 b5)
      [q21_fsql_pred_suppkey; q21_fsql_pred_orderkey; q21_fsql_pred_status_F;
       q21_fsql_pred_late; q21_fsql_pred_nationkey;
       q21_fsql_pred_nation nation_name] =
    [b0; b1; b2; b3; b4; b5].
Proof.
  intros; unfold q21_ordinary_leaf_decide, q21_fsql_pred_suppkey,
    q21_fsql_pred_orderkey, q21_fsql_pred_status_F, q21_fsql_pred_late,
    q21_fsql_pred_nationkey, q21_fsql_pred_nation,
    DotInt32, DotDate, DotString, CstString, AExpr, Dot, Constant,
    AttrInt32, AttrDate, AttrString; simpl; reflexivity.
Qed.

Lemma andb6_fold_And_F :
  forall b0 b1 b2 b3 b4 b5 : bool,
    andb b0 (andb b1 (andb b2 (andb b3 (andb b4 b5)))) =
    scalar_acceptance_fold And_F [b0; b1; b2; b3; b4; b5].
Proof.
  intros; simpl; rewrite Bool.andb_true_r; reflexivity.
Qed.

Lemma Q21_conjlist_accept_as_fold :
  forall ft nation_name b0 b1 b2 b3 b4 b5,
    accept_at (Env.env_t TNull nil ft) q21_fsql_pred_suppkey b0 ->
    accept_at (Env.env_t TNull nil ft) q21_fsql_pred_orderkey b1 ->
    accept_at (Env.env_t TNull nil ft) q21_fsql_pred_status_F b2 ->
    accept_at (Env.env_t TNull nil ft) q21_fsql_pred_late b3 ->
    accept_at (Env.env_t TNull nil ft) q21_fsql_pred_nationkey b4 ->
    accept_at (Env.env_t TNull nil ft) (q21_fsql_pred_nation nation_name) b5 ->
    accept_at (Env.env_t TNull nil ft) (ordinary_outer_conjlist nation_name)
      (andb b0 (andb b1 (andb b2 (andb b3 (andb b4 b5))))).
Proof.
  intros ft nation_name b0 b1 b2 b3 b4 b5 H0 H1 H2 H3 H4 H5.
  rewrite ordinary_outer_conjlist_shape.
  set (env := Env.env_t TNull nil ft).
  set (exprs :=
    [q21_fsql_pred_suppkey; q21_fsql_pred_orderkey; q21_fsql_pred_status_F;
     q21_fsql_pred_late; q21_fsql_pred_nationkey;
     q21_fsql_pred_nation nation_name]).
  set (decide := q21_ordinary_leaf_decide nation_name b0 b1 b2 b3 b4 b5).
  rewrite andb6_fold_And_F.
  rewrite <- (q21_ordinary_decide_map nation_name b0 b1 b2 b3 b4 b5).
  change (map (q21_ordinary_leaf_decide nation_name b0 b1 b2 b3 b4 b5) exprs)
    with (map decide exprs).
  eapply (@scalar_expr_conj_list_acceptance_exact TNull relname
    basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule
    ([[]; ["b0"]; ["b1"; "b2"]; ["b3"; "b4"; "b5"];
      ["b6"; "b7"; "b8"; "b9"]; ["b10"; "b11"]])%list
    env And_F exprs decide).
  intros e Hin; simpl in Hin.
  destruct Hin as [Heq|Hin]; [inversion Heq; subst e; exact H0|].
  destruct Hin as [Heq|Hin]; [inversion Heq; subst e; exact H1|].
  destruct Hin as [Heq|Hin]; [inversion Heq; subst e; exact H2|].
  destruct Hin as [Heq|Hin]; [inversion Heq; subst e; exact H3|].
  destruct Hin as [Heq|Hin]; [inversion Heq; subst e; exact H4|].
  destruct Hin as [Heq|Hin]; [inversion Heq; subst e; exact H5|].
  destruct Hin.
Qed.

(** * RelNode ordinary keep = all six leaves true *)

Lemma Q21_relnode_ordinary_keep_as_andb6 :
  forall rr nation_name b0 b1 b2 b3 b4 b5,
    eval_pred_keep pred_eq_supp_l1 (Params_Q21 nation_name) rr = Some b0 ->
    eval_pred_keep pred_eq_ord_l1 (Params_Q21 nation_name) rr = Some b1 ->
    eval_pred_keep pred_status_F (Params_Q21 nation_name) rr = Some b2 ->
    eval_pred_keep pred_late_l1 (Params_Q21 nation_name) rr = Some b3 ->
    eval_pred_keep pred_eq_nationkey (Params_Q21 nation_name) rr = Some b4 ->
    eval_pred_keep pred_nation_param (Params_Q21 nation_name) rr = Some b5 ->
    eval_pred_keep pred_ordinary_outer (Params_Q21 nation_name) rr =
      Some (andb b0 (andb b1 (andb b2 (andb b3 (andb b4 b5))))).
Proof.
  intros; eapply eval_pred_keep_ordinary_as_andb6; eassumption.
Qed.

Lemma andb6_true_iff :
  forall b0 b1 b2 b3 b4 b5 : bool,
    andb b0 (andb b1 (andb b2 (andb b3 (andb b4 b5)))) = true
    <->
    (b0 = true /\ b1 = true /\ b2 = true /\ b3 = true /\ b4 = true /\ b5 = true).
Proof.
  intros b0 b1 b2 b3 b4 b5; split.
  - intro H.
    apply andb_prop in H; destruct H as [H0 H].
    apply andb_prop in H; destruct H as [H1 H].
    apply andb_prop in H; destruct H as [H2 H].
    apply andb_prop in H; destruct H as [H3 H].
    apply andb_prop in H; destruct H as [H4 H5].
    repeat split; assumption.
  - intros [H0 [H1 [H2 [H3 [H4 H5]]]]].
    subst; reflexivity.
Qed.

(** * Compose: Milestone B target *)

Theorem Q21_ordinary_conjlist_acceptance_iff :
  forall ft rr nation_name,
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    q21_outer_cor_text_canonical ft ->
    NationNameOk nation_name ->
    accept_at (Env.env_t TNull nil ft)
      (ordinary_outer_conjlist nation_name) true
    <->
    eval_pred_keep pred_ordinary_outer (Params_Q21 nation_name) rr
      = Some true.
Proof.
  intros ft rr nation_name Hrc Hlab Htxt Hnn.
  destruct (Q21_leaf_suppkey_same_bit ft rr (Params_Q21 nation_name) Hrc Hlab)
    as [b0 [Ha0 Hk0]].
  destruct (Q21_leaf_orderkey_same_bit ft rr (Params_Q21 nation_name) Hrc Hlab)
    as [b1 [Ha1 Hk1]].
  destruct (Q21_leaf_status_F_same_bit ft rr (Params_Q21 nation_name)
    Hrc Hlab Htxt) as [b2 [Ha2 Hk2]].
  destruct (Q21_leaf_late_same_bit ft rr (Params_Q21 nation_name) Hrc Hlab)
    as [b3 [Ha3 Hk3]].
  destruct (Q21_leaf_nationkey_same_bit ft rr (Params_Q21 nation_name) Hrc Hlab)
    as [b4 [Ha4 Hk4]].
  destruct (Q21_leaf_nation_name_same_bit ft rr nation_name Hrc Hlab Htxt Hnn)
    as [b5 [Ha5 Hk5]].
  pose proof (Q21_conjlist_accept_as_fold ft nation_name b0 b1 b2 b3 b4 b5
    Ha0 Ha1 Ha2 Ha3 Ha4 Ha5) as Hconj.
  pose proof (Q21_relnode_ordinary_keep_as_andb6 rr nation_name
    b0 b1 b2 b3 b4 b5 Hk0 Hk1 Hk2 Hk3 Hk4 Hk5) as Hrn.
  split; intro H.
  - destruct Hconj as [[tr [Hev Htr]] [Huniq _]].
    destruct H as [[tr' [Hev' Htr']] _].
    pose proof (Huniq tr Hev) as Hb.
    pose proof (Huniq tr' Hev') as Hb'.
    rewrite <- Hb, Htr, <- Hb', Htr' in Hrn.
    exact Hrn.
  - rewrite Hrn in H; inversion H as [Hand]; clear H.
    apply andb6_true_iff in Hand.
    destruct Hand as [E0 [E1 [E2 [E3 [E4 E5]]]]].
    subst b0 b1 b2 b3 b4 b5.
    exact Hconj.
Qed.

End Q21OrdinaryConjAcceptance.

(** Milestone B target statement (closed by the section theorem). *)
Definition Q21_ordinary_conjlist_acceptance_iff_statement : Prop :=
  forall (basesort : relname -> Fset.set (A TNull))
         (instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)))
         (boolean_schedule : boolean_site -> boolean_evaluation_order)
         (ft : tuple TNull) (rr : Row) (nation_name : string),
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    q21_outer_cor_text_canonical ft ->
    NationNameOk nation_name ->
    @scalar_expr_acceptance_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      (Env.env_t TNull nil ft)
      (ordinary_outer_conjlist nation_name) true
    <->
    eval_pred_keep pred_ordinary_outer (Params_Q21 nation_name) rr
      = Some true.

Theorem Q21_ordinary_conjlist_acceptance_iff_statement_holds :
  Q21_ordinary_conjlist_acceptance_iff_statement.
Proof.
  unfold Q21_ordinary_conjlist_acceptance_iff_statement.
  intros basesort instance boolean_schedule.
  apply (@Q21_ordinary_conjlist_acceptance_iff basesort instance boolean_schedule).
Qed.

(** * Milestone C — Filter preserves [bag_correspond_unordered]

    Generic fact: [bag_correspond_unordered_filter] in SlotMap.
    RelNode bridge: [filter_rows_eq_List_filter] in Morphisms.
    Instantiate with Milestone B same-bit / acceptance uniqueness. *)

Definition q21_ordinary_rel_keep (nation_name : string) (rr : Row) : bool :=
  match eval_pred_keep pred_ordinary_outer (Params_Q21 nation_name) rr with
  | Some b => b
  | None => false
  end.

Section Q21OrdinaryFilterBag.
Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Local Abbreviation accept_at :=
  (@scalar_expr_acceptance_exact_at TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule).

Lemma accept_at_unique_bool :
  forall env e b1 b2,
    accept_at env e b1 ->
    accept_at env e b2 ->
    b1 = b2.
Proof.
  intros env e b1 b2 [[tr1 [He1 Ht1]] [Hu _]] [[tr2 [He2 Ht2]] _].
  rewrite <- (Hu tr1 He1), Ht1, <- (Hu tr2 He2), Ht2; reflexivity.
Qed.

(** Whole ordinary ConjList shares one keep bit with RelNode (Milestone B glue). *)
Lemma Q21_ordinary_same_bit :
  forall ft rr nation_name,
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    q21_outer_cor_text_canonical ft ->
    NationNameOk nation_name ->
    exists b,
      accept_at (Env.env_t TNull nil ft)
        (ordinary_outer_conjlist nation_name) b /\
      eval_pred_keep pred_ordinary_outer (Params_Q21 nation_name) rr = Some b.
Proof.
  intros ft rr nation_name Hrc Hlab Htxt Hnn.
  destruct (@Q21_leaf_suppkey_same_bit basesort instance boolean_schedule
    ft rr (Params_Q21 nation_name) Hrc Hlab) as [b0 [Ha0 Hk0]].
  destruct (@Q21_leaf_orderkey_same_bit basesort instance boolean_schedule
    ft rr (Params_Q21 nation_name) Hrc Hlab) as [b1 [Ha1 Hk1]].
  destruct (@Q21_leaf_status_F_same_bit basesort instance boolean_schedule
    ft rr (Params_Q21 nation_name) Hrc Hlab Htxt) as [b2 [Ha2 Hk2]].
  destruct (@Q21_leaf_late_same_bit basesort instance boolean_schedule
    ft rr (Params_Q21 nation_name) Hrc Hlab) as [b3 [Ha3 Hk3]].
  destruct (@Q21_leaf_nationkey_same_bit basesort instance boolean_schedule
    ft rr (Params_Q21 nation_name) Hrc Hlab) as [b4 [Ha4 Hk4]].
  destruct (@Q21_leaf_nation_name_same_bit basesort instance boolean_schedule
    ft rr nation_name Hrc Hlab Htxt Hnn) as [b5 [Ha5 Hk5]].
  exists (andb b0 (andb b1 (andb b2 (andb b3 (andb b4 b5))))).
  split.
  - eapply (@Q21_conjlist_accept_as_fold basesort instance boolean_schedule);
      eassumption.
  - eapply Q21_relnode_ordinary_keep_as_andb6; eassumption.
Qed.

Lemma Q21_ordinary_keep_bits_agree :
  forall ft rr nation_name (keep_f : tuple TNull -> bool),
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    q21_outer_cor_text_canonical ft ->
    NationNameOk nation_name ->
    accept_at (Env.env_t TNull nil ft)
      (ordinary_outer_conjlist nation_name) (keep_f ft) ->
    keep_f ft = q21_ordinary_rel_keep nation_name rr.
Proof.
  intros ft rr nation_name keep_f Hrc Hlab Htxt Hnn Ha.
  destruct (Q21_ordinary_same_bit ft rr nation_name Hrc Hlab Htxt Hnn)
    as [b [Hb Hk]].
  unfold q21_ordinary_rel_keep; rewrite Hk.
  eapply accept_at_unique_bool; eassumption.
Qed.

(** List.filter form — composes with FormalSQL [filter_rows_success_of_acceptance]. *)
Theorem Q21_ordinary_filter_preserves_bag_correspond_unordered :
  forall fts rrs nation_name (keep_f : tuple TNull -> bool),
    bag_correspond_unordered slot_map_outer_cor fts rrs ->
    NationNameOk nation_name ->
    (forall ft rr,
       row_correspond slot_map_outer_cor ft rr ->
       q21_outer_cor_labels_ok ft /\
       q21_outer_cor_text_canonical ft /\
       accept_at (Env.env_t TNull nil ft)
         (ordinary_outer_conjlist nation_name) (keep_f ft)) ->
    bag_correspond_unordered slot_map_outer_cor
      (List.filter keep_f fts)
      (List.filter (q21_ordinary_rel_keep nation_name) rrs).
Proof.
  intros fts rrs nation_name keep_f Hbag Hnn Hrow.
  eapply bag_correspond_unordered_filter; [exact Hbag|].
  intros ft rr Hrc.
  destruct (Hrow ft rr Hrc) as [Hlab [Htxt Ha]].
  eapply Q21_ordinary_keep_bits_agree; eassumption.
Qed.

(** Every RelNode row in an unordered corresponding bag decides under the
    ordinary predicate (via a FormalSQL partner + Milestone B same-bit). *)
Lemma Q21_ordinary_rel_keep_total_of_bag :
  forall fts rrs nation_name,
    bag_correspond_unordered slot_map_outer_cor fts rrs ->
    NationNameOk nation_name ->
    (forall ft rr,
       row_correspond slot_map_outer_cor ft rr ->
       q21_outer_cor_labels_ok ft /\
       q21_outer_cor_text_canonical ft) ->
    forall rr,
      List.In rr rrs ->
      eval_pred_keep pred_ordinary_outer (Params_Q21 nation_name) rr =
        Some (q21_ordinary_rel_keep nation_name rr).
Proof.
  intros fts rrs nation_name [rrs' [HP HF]] Hnn Hlabtxt rr Hin.
  apply (Permutation_in rr HP) in Hin.
  assert (Hpair : exists ft, List.In ft fts /\ row_correspond slot_map_outer_cor ft rr).
  { clear HP Hnn Hlabtxt.
    induction HF as [|ft rr0 fts0 rrs0 Hrc HFs IH].
    - contradiction Hin.
    - simpl in Hin; destruct Hin as [Heq|Hin].
      + subst rr0; exists ft; split; [left; reflexivity|exact Hrc].
      + destruct (IH Hin) as [ft' [Hin' Hrc']].
        exists ft'; split; [right; exact Hin'|exact Hrc']. }
  destruct Hpair as [ft [Hin_ft Hrc]].
  destruct (Hlabtxt ft rr Hrc) as [Hlab Htxt].
  destruct (Q21_ordinary_same_bit ft rr nation_name Hrc Hlab Htxt Hnn)
    as [b [_ Hk]].
  unfold q21_ordinary_rel_keep; rewrite Hk; reflexivity.
Qed.

(** RelNode [filter_rows] form. *)
Theorem Q21_ordinary_filter_rows_preserves_bag_correspond_unordered :
  forall fts rrs nation_name (keep_f : tuple TNull -> bool) rrs_f,
    bag_correspond_unordered slot_map_outer_cor fts rrs ->
    NationNameOk nation_name ->
    (forall ft rr,
       row_correspond slot_map_outer_cor ft rr ->
       q21_outer_cor_labels_ok ft /\
       q21_outer_cor_text_canonical ft /\
       accept_at (Env.env_t TNull nil ft)
         (ordinary_outer_conjlist nation_name) (keep_f ft)) ->
    filter_rows pred_ordinary_outer (Params_Q21 nation_name) rrs = Some rrs_f ->
    bag_correspond_unordered slot_map_outer_cor
      (List.filter keep_f fts) rrs_f.
Proof.
  intros fts rrs nation_name keep_f rrs_f Hbag Hnn Hrow Hfr.
  assert (Hlabtxt :
    forall ft rr,
      row_correspond slot_map_outer_cor ft rr ->
      q21_outer_cor_labels_ok ft /\ q21_outer_cor_text_canonical ft).
  { intros ft rr Hrc; destruct (Hrow ft rr Hrc) as [A [B _]]; split; assumption. }
  pose proof (Q21_ordinary_rel_keep_total_of_bag fts rrs nation_name
    Hbag Hnn Hlabtxt) as Htot.
  assert (Hfr' :
    filter_rows pred_ordinary_outer (Params_Q21 nation_name) rrs =
      Some (List.filter (q21_ordinary_rel_keep nation_name) rrs)).
  { apply filter_rows_eq_List_filter; exact Htot. }
  rewrite Hfr' in Hfr; inversion Hfr; subst rrs_f.
  eapply Q21_ordinary_filter_preserves_bag_correspond_unordered; eassumption.
Qed.

End Q21OrdinaryFilterBag.

(** * Milestone D — Four-way CrossJoin product under [bag_correspond_unordered]

    FormalSQL supplier × l1 × orders × nation  ↔  RelNode nested-loop product
    under native [slot_map_outer], before Project/rename and ordinary filter.

    Generic helpers: [row_correspond_cross_join],
    [bag_correspond_unordered_cross_product],
    [cross_join_tuples_eq_brute_left_join_list],
    [join_inner_true_eq_cross_join_rows].
    No ordinary 6-conjuncts. No EXISTS / NOT EXISTS. *)

Definition q21_cross4_tuples
    (fts_s fts_l fts_o fts_n : list (tuple TNull)) : list (tuple TNull) :=
  cross_join_tuples
    (cross_join_tuples
      (cross_join_tuples fts_s fts_l) fts_o) fts_n.

Definition q21_cross4_rows
    (rrs_s rrs_l rrs_o rrs_n : list Row) : list Row :=
  cross_join_rows
    (cross_join_rows
      (cross_join_rows rrs_s rrs_l) rrs_o) rrs_n.

Lemma slot_map_outer_as_nested_apps :
  slot_map_outer =
  List.app
    (List.app
      (List.app slot_map_supplier slot_map_lineitem)
      slot_map_orders)
    slot_map_nation.
Proof.
  unfold slot_map_outer.
  rewrite <- !app_assoc; reflexivity.
Qed.

(** Present / freshness side-conditions required by FormalSQL [join_tuple]
    for watched attrs (semantically necessary; not TCB). *)
Definition q21_cross4_present_fresh
    (fts_s fts_l fts_o fts_n : list (tuple TNull)) : Prop :=
  (forall ft, List.In ft fts_s -> slot_map_present slot_map_supplier ft) /\
  (forall ft, List.In ft fts_l -> slot_map_present slot_map_lineitem ft) /\
  (forall ft, List.In ft fts_o -> slot_map_present slot_map_orders ft) /\
  (forall ft, List.In ft fts_n -> slot_map_present slot_map_nation ft) /\
  (forall ft, List.In ft fts_s ->
     slot_map_fresh_from slot_map_lineitem ft) /\
  (forall ft, List.In ft (cross_join_tuples fts_s fts_l) ->
     slot_map_fresh_from slot_map_orders ft) /\
  (forall ft,
     List.In ft (cross_join_tuples (cross_join_tuples fts_s fts_l) fts_o) ->
     slot_map_fresh_from slot_map_nation ft).

Theorem Q21_four_way_cross_product_bag_correspond :
  forall fts_s fts_l fts_o fts_n rrs_s rrs_l rrs_o rrs_n,
    bag_correspond_unordered slot_map_supplier fts_s rrs_s ->
    bag_correspond_unordered slot_map_lineitem fts_l rrs_l ->
    bag_correspond_unordered slot_map_orders fts_o rrs_o ->
    bag_correspond_unordered slot_map_nation fts_n rrs_n ->
    q21_cross4_present_fresh fts_s fts_l fts_o fts_n ->
    bag_correspond_unordered slot_map_outer
      (q21_cross4_tuples fts_s fts_l fts_o fts_n)
      (q21_cross4_rows rrs_s rrs_l rrs_o rrs_n).
Proof.
  intros fts_s fts_l fts_o fts_n rrs_s rrs_l rrs_o rrs_n
    Hs Hl Ho Hn Hpf.
  destruct Hpf as [Hps [Hpl [Hpo [Hpn [Hfl [Hfo Hfn]]]]]].
  unfold q21_cross4_tuples, q21_cross4_rows.
  rewrite slot_map_outer_as_nested_apps.
  (* supplier × lineitem *)
  assert (Hsl : bag_correspond_unordered
                  (List.app slot_map_supplier slot_map_lineitem)
                  (cross_join_tuples fts_s fts_l)
                  (cross_join_rows rrs_s rrs_l)).
  {
    eapply bag_correspond_unordered_cross_product;
      try eassumption.
  }
  (* (S×L) × orders *)
  assert (Hslo : bag_correspond_unordered
                   (List.app
                      (List.app slot_map_supplier slot_map_lineitem)
                      slot_map_orders)
                   (cross_join_tuples (cross_join_tuples fts_s fts_l) fts_o)
                   (cross_join_rows (cross_join_rows rrs_s rrs_l) rrs_o)).
  {
    eapply bag_correspond_unordered_cross_product;
      try eassumption.
    intros ft Hin.
    eapply slot_map_present_cross_join_tuples; eassumption.
  }
  (* ((S×L)×O) × nation *)
  eapply bag_correspond_unordered_cross_product;
    try eassumption.
  intros ft Hin.
  eapply slot_map_present_cross_join_tuples; try eassumption.
  intros ft' Hin'.
  eapply slot_map_present_cross_join_tuples; eassumption.
Qed.

(** RelNode nested [JoinCross] / constantly-true [join_inner] enumerates
    the same nested-loop product as [q21_cross4_rows]. *)
Lemma q21_cross4_rows_join_inner_true :
  forall (p : Params) rrs_s rrs_l rrs_o rrs_n,
    join_inner (EConst (VBool true) TBool) p
      (match join_inner (EConst (VBool true) TBool) p
               (match join_inner (EConst (VBool true) TBool) p
                        rrs_s rrs_l with
                | Some sl => sl
                | None => nil
                end)
               rrs_o with
       | Some slo => slo
       | None => nil
       end)
      rrs_n =
    Some (q21_cross4_rows rrs_s rrs_l rrs_o rrs_n).
Proof.
  intros p rrs_s rrs_l rrs_o rrs_n.
  unfold q21_cross4_rows.
  rewrite !join_inner_true_eq_cross_join_rows.
  reflexivity.
Qed.

(** FormalSQL side: each pairwise product is [brute_left_join_list], hence
    same multiset as [query_cross_join_bag] of [query_rows_bag]s
    ([raw_cross_same_rows_as_bag]); [Febag.elements] order is absorbed by
    [bag_correspond_unordered] / Permutation on the RelNode product. *)
Lemma q21_cross4_tuples_as_brute_nested :
  forall fts_s fts_l fts_o fts_n,
    q21_cross4_tuples fts_s fts_l fts_o fts_n =
    brute_left_join_list (tuple TNull) (join_tuple TNull)
      (brute_left_join_list (tuple TNull) (join_tuple TNull)
         (brute_left_join_list (tuple TNull) (join_tuple TNull)
            fts_s fts_l)
         fts_o)
      fts_n.
Proof.
  intros.
  unfold q21_cross4_tuples.
  rewrite !cross_join_tuples_eq_brute_left_join_list.
  reflexivity.
Qed.

(** * Milestone E — Compose A–D into [Q21_OuterOrdinarySpineCorrespond_statement]

    Intended path:
      D: four-way CrossJoin product under [slot_map_outer]
      A: [Q21_bag_correspond_select4_rename] → [slot_map_outer_cor]
      B/C: ordinary ConjList keep ↔ [pred_ordinary_outer] + filter bags
      wrappers: FormalSQL_exec of [q_ordinary_outer] and Eval_Spec [filter_outer]
      conclude public statement.

    No EXISTS / NOT EXISTS. *)

(** Bag lift of Milestone A. *)
Lemma Q21_bag_correspond_select4_rename :
  forall fts rrs,
    bag_correspond_unordered slot_map_outer fts rrs ->
    bag_correspond_unordered slot_map_outer_cor
      (List.map q21_project_select4 fts) rrs.
Proof.
  intros fts rrs [rrs' [HP HF]].
  exists rrs'; split; [exact HP|].
  clear rrs HP.
  induction HF as [|ft rr fts0 rrs0 Hrc HFs IH]; simpl; [constructor|].
  constructor; [|exact IH].
  eapply Q21_row_correspond_select4_rename; [exact Hrc|reflexivity].
Qed.

(** Catalog nation: spine extraction is definitional. *)
Lemma q21_cross_spine_of_qhat_catalog :
  q21_cross_spine_of (qhat_Q21 catalog_nation_name) =
  q21_cross_spine_of Queries.source_query_expr.
Proof.
  rewrite qhat_Q21_catalog_instance; reflexivity.
Qed.

Lemma q_ordinary_outer_shape_catalog :
  exists spine,
    q_ordinary_outer catalog_nation_name =
      QExpr_Filter (ordinary_outer_conjlist catalog_nation_name)
        (QExpr_Project Queries.scalar_select_list_4
          (QExpr_Filter SExpr_True spine)) /\
    is_q21_cross_spine spine = true.
Proof.
  pose proof q21_cross_spine_catalog_Some as H.
  destruct (q21_cross_spine_of (qhat_Q21 catalog_nation_name)) as [spine|] eqn:Hs.
  - exists spine; split; [|exact H].
    apply q_ordinary_outer_shape_of_Some; exact Hs.
  - contradiction H.
Qed.

(** RelNode helper: [join_inner] of a total boolean keep is filter-of-cross
    ([join_inner_eq_filter_cross_join_rows] in Morphisms). *)

(** * STEP E1 — FormalSQL_exec wrapper

    Connect [FormalSQL_exec] of [q_ordinary_outer] to the concrete filtered
    projected CrossJoin list (or a same-bag representative). *)

(** Empty CrossJoin spine (catalog/source shape) evaluates to [nil]. *)
Lemma q21_empty_source_cross_spine_success :
  forall spine,
    q21_cross_spine_of Queries.source_query_expr = Some spine ->
    EQ spine (SqlSuccess nil).
Proof.
  intros spine Hs.
  unfold q21_cross_spine_of, Queries.source_query_expr,
    Queries.shared_query_expr_0 in Hs.
  simpl in Hs.
  inversion Hs; subst spine; clear Hs.
  eapply q21_empty_cross_join_success.
  - eapply q21_empty_project_success.
    eapply q21_empty_filter_success.
    eapply q21_empty_cross_join_success.
    + eapply q21_empty_project_success.
      eapply q21_empty_filter_success.
      eapply q21_empty_cross_join_success.
      * eapply q21_empty_project_success.
        apply q21_empty_table_success; left; reflexivity.
      * eapply q21_empty_project_success.
        apply q21_empty_table_success; right; left; reflexivity.
    + eapply q21_empty_project_success.
      apply q21_empty_table_success; right; right; left; reflexivity.
  - eapply q21_empty_project_success.
    apply q21_empty_table_success; right; right; right; reflexivity.
Qed.

Lemma q21_cross_spine_of_qhat_Some :
  forall nation_name,
    exists spine,
      q21_cross_spine_of (qhat_Q21 nation_name) = Some spine /\
      q21_cross_spine_of Queries.source_query_expr = Some spine.
Proof.
  intros nation_name.
  pose proof q21_cross_spine_source_Some as Hsrc.
  destruct (q21_cross_spine_of Queries.source_query_expr) as [spine|] eqn:Hs;
    [|contradiction Hsrc].
  exists spine; split; [|reflexivity].
  unfold qhat_Q21.
  destruct (String.eqb nation_name catalog_nation_name) eqn:Heq;
    (unfold q21_cross_spine_of, subst_nation_query,
       Queries.source_query_expr, Queries.shared_query_expr_0;
     simpl;
     unfold q21_cross_spine_of, Queries.source_query_expr,
       Queries.shared_query_expr_0 in Hs;
     simpl in Hs; inversion Hs; subst; reflexivity).
Qed.

(** Empty-DB FormalSQL_exec of [q_ordinary_outer] (any nation_name).
    Output is exactly [nil] (= filter/map/cross4 of empty embeds). *)
Lemma FormalSQL_exec_q_ordinary_outer_empty :
  forall nation_name,
    FormalSQL_exec D_empty_Q21 (q_ordinary_outer nation_name) nil.
Proof.
  intros nation_name.
  unfold FormalSQL_exec, eval_query_expr_possible_outcome.
  exists q21_boolean_schedule.
  destruct (q21_cross_spine_of_qhat_Some nation_name) as [spine [Hs Hsrc]].
  unfold q_ordinary_outer; rewrite Hs.
  eapply EQuery_FilterRows; [|apply q21_empty_filter_rows].
  eapply EQuery_ProjectRows; [|apply q21_empty_project_rows].
  eapply EQuery_FilterRows; [|apply q21_empty_filter_rows].
  eapply q21_empty_source_cross_spine_success; exact Hsrc.
Qed.

(** * E1 helpers — EmbedDB witness tables + Filter(True) + CrossJoin pick *)

Definition q21_embed_tables
    (ss' ls' os' ns' : list (tuple TNull)) : list witness_table :=
  [WitnessTable (Rel "supplier") ss';
   WitnessTable (Rel "lineitem") ls';
   WitnessTable (Rel "orders") os';
   WitnessTable (Rel "nation") ns'].

Definition D_of_embeds (ss' ls' os' ns' : list (tuple TNull)) : db_state :=
  witness_database Schema.generated_schema
    (q21_embed_tables ss' ls' os' ns').

Lemma EmbedDB_Q21_as_D_of_embeds :
  forall db D ss ls os ns ss' ls' os' ns',
    EmbedDB_Q21 db = Some D ->
    db_lookup db table_supplier = Some ss ->
    db_lookup db table_lineitem = Some ls ->
    db_lookup db table_orders = Some os ->
    db_lookup db table_nation = Some ns ->
    embed_rows EmbedRow_supplier ss = Some ss' ->
    embed_rows EmbedRow_lineitem ls = Some ls' ->
    embed_rows EmbedRow_orders os = Some os' ->
    embed_rows EmbedRow_nation ns = Some ns' ->
    D = D_of_embeds ss' ls' os' ns'.
Proof.
  intros db D ss ls os ns ss' ls' os' ns'
    Hemb Hs Hl Ho Hn Hes Hel Heo Hen.
  unfold EmbedDB_Q21 in Hemb.
  rewrite Hs, Hl, Ho, Hn, Hes, Hel, Heo, Hen in Hemb.
  inversion Hemb; subst; reflexivity.
Qed.

Lemma D_of_embeds_instance_supplier :
  forall ss' ls' os' ns',
    @_instance TNull (D_of_embeds ss' ls' os' ns') (Rel "supplier")
    = Febag.mk_bag (Fecol.CBag (CTuple TNull)) ss'.
Proof. intros; reflexivity. Qed.

Lemma D_of_embeds_instance_lineitem :
  forall ss' ls' os' ns',
    @_instance TNull (D_of_embeds ss' ls' os' ns') (Rel "lineitem")
    = Febag.mk_bag (Fecol.CBag (CTuple TNull)) ls'.
Proof. intros; reflexivity. Qed.

Lemma D_of_embeds_instance_orders :
  forall ss' ls' os' ns',
    @_instance TNull (D_of_embeds ss' ls' os' ns') (Rel "orders")
    = Febag.mk_bag (Fecol.CBag (CTuple TNull)) os'.
Proof. intros; reflexivity. Qed.

Lemma D_of_embeds_instance_nation :
  forall ss' ls' os' ns',
    @_instance TNull (D_of_embeds ss' ls' os' ns') (Rel "nation")
    = Febag.mk_bag (Fecol.CBag (CTuple TNull)) ns'.
Proof. intros; reflexivity. Qed.

(** Table outputs appearing in the extracted CrossJoin spine. *)
Definition q21_supplier_table_outputs : list (attribute TNull) :=
  [AttrInt32 "s_suppkey"; AttrString "s_name" (StringChar 25);
   AttrString "s_address" (StringVarcharN 40); AttrInt32 "s_nationkey";
   AttrString "s_phone" (StringChar 15); AttrDecimal "s_acctbal" 15 2;
   AttrString "s_comment" (StringVarcharN 101)].

Definition q21_lineitem_table_outputs : list (attribute TNull) :=
  [AttrInt32 "l_orderkey"; AttrInt32 "l_partkey"; AttrInt32 "l_suppkey";
   AttrInt32 "l_linenumber"; AttrDecimal "l_quantity" 15 2;
   AttrDecimal "l_extendedprice" 15 2; AttrDecimal "l_discount" 15 2;
   AttrDecimal "l_tax" 15 2; AttrString "l_returnflag" (StringChar 1);
   AttrString "l_linestatus" (StringChar 1); AttrDate "l_shipdate";
   AttrDate "l_commitdate"; AttrDate "l_receiptdate";
   AttrString "l_shipinstruct" (StringChar 25);
   AttrString "l_shipmode" (StringChar 10);
   AttrString "l_comment" (StringVarcharN 44)].

Definition q21_orders_table_outputs : list (attribute TNull) :=
  [AttrInt32 "o_orderkey"; AttrInt32 "o_custkey";
   AttrString "o_orderstatus" (StringChar 1); AttrDecimal "o_totalprice" 15 2;
   AttrDate "o_orderdate"; AttrString "o_orderpriority" (StringChar 15);
   AttrString "o_clerk" (StringChar 15); AttrInt32 "o_shippriority";
   AttrString "o_comment" (StringVarcharN 79)].

Definition q21_nation_table_outputs : list (attribute TNull) :=
  [AttrInt32 "n_nationkey"; AttrString "n_name" (StringChar 25);
   AttrInt32 "n_regionkey"; AttrString "n_comment" (StringVarcharN 152)].

Lemma q21_supplier_table_sort_ok :
  @query_outputs_sort TNull q21_supplier_table_outputs =S=
    @_basesort TNull Schema.generated_schema (Rel "supplier").
Proof. vm_compute; reflexivity. Qed.

Lemma q21_lineitem_table_sort_ok :
  @query_outputs_sort TNull q21_lineitem_table_outputs =S=
    @_basesort TNull Schema.generated_schema (Rel "lineitem").
Proof. vm_compute; reflexivity. Qed.

Lemma q21_orders_table_sort_ok :
  @query_outputs_sort TNull q21_orders_table_outputs =S=
    @_basesort TNull Schema.generated_schema (Rel "orders").
Proof. vm_compute; reflexivity. Qed.

Lemma q21_nation_table_sort_ok :
  @query_outputs_sort TNull q21_nation_table_outputs =S=
    @_basesort TNull Schema.generated_schema (Rel "nation").
Proof. vm_compute; reflexivity. Qed.

Lemma q21_embed_table_same_rows_supplier :
  forall ss' ls' os' ns',
    query_same_rows_as_bag (T:=TNull) ss'
      (@query_table_bag TNull relname
         (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
         (@_instance TNull (D_of_embeds ss' ls' os' ns'))
         q21_supplier_table_outputs (Rel "supplier")).
Proof.
  intros ss' ls' os' ns'.
  unfold query_same_rows_as_bag, query_table_bag, query_rows_bag.
  rewrite Febag.nb_occ_equal; intro x.
  assert (Hsort :
    @query_outputs_sort TNull q21_supplier_table_outputs =S?=
      @_basesort TNull (D_of_embeds ss' ls' os' ns') (Rel "supplier") = true).
  { change (@query_outputs_sort TNull q21_supplier_table_outputs =S?=
      @_basesort TNull Schema.generated_schema (Rel "supplier") = true).
    pose proof q21_supplier_table_sort_ok as H.
    unfold Fset.equal in H |- *; exact H. }
  rewrite Hsort.
  rewrite D_of_embeds_instance_supplier.
  rewrite !Febag.nb_occ_mk_bag; reflexivity.
Qed.

Lemma q21_embed_table_same_rows_lineitem :
  forall ss' ls' os' ns',
    query_same_rows_as_bag (T:=TNull) ls'
      (@query_table_bag TNull relname
         (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
         (@_instance TNull (D_of_embeds ss' ls' os' ns'))
         q21_lineitem_table_outputs (Rel "lineitem")).
Proof.
  intros ss' ls' os' ns'.
  unfold query_same_rows_as_bag, query_table_bag, query_rows_bag.
  rewrite Febag.nb_occ_equal; intro x.
  assert (Hsort :
    @query_outputs_sort TNull q21_lineitem_table_outputs =S?=
      @_basesort TNull (D_of_embeds ss' ls' os' ns') (Rel "lineitem") = true).
  { change (@query_outputs_sort TNull q21_lineitem_table_outputs =S?=
      @_basesort TNull Schema.generated_schema (Rel "lineitem") = true).
    pose proof q21_lineitem_table_sort_ok as H.
    unfold Fset.equal in H |- *; exact H. }
  rewrite Hsort.
  rewrite D_of_embeds_instance_lineitem.
  rewrite !Febag.nb_occ_mk_bag; reflexivity.
Qed.

Lemma q21_embed_table_same_rows_orders :
  forall ss' ls' os' ns',
    query_same_rows_as_bag (T:=TNull) os'
      (@query_table_bag TNull relname
         (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
         (@_instance TNull (D_of_embeds ss' ls' os' ns'))
         q21_orders_table_outputs (Rel "orders")).
Proof.
  intros ss' ls' os' ns'.
  unfold query_same_rows_as_bag, query_table_bag, query_rows_bag.
  rewrite Febag.nb_occ_equal; intro x.
  assert (Hsort :
    @query_outputs_sort TNull q21_orders_table_outputs =S?=
      @_basesort TNull (D_of_embeds ss' ls' os' ns') (Rel "orders") = true).
  { change (@query_outputs_sort TNull q21_orders_table_outputs =S?=
      @_basesort TNull Schema.generated_schema (Rel "orders") = true).
    pose proof q21_orders_table_sort_ok as H.
    unfold Fset.equal in H |- *; exact H. }
  rewrite Hsort.
  rewrite D_of_embeds_instance_orders.
  rewrite !Febag.nb_occ_mk_bag; reflexivity.
Qed.

Lemma q21_embed_table_same_rows_nation :
  forall ss' ls' os' ns',
    query_same_rows_as_bag (T:=TNull) ns'
      (@query_table_bag TNull relname
         (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
         (@_instance TNull (D_of_embeds ss' ls' os' ns'))
         q21_nation_table_outputs (Rel "nation")).
Proof.
  intros ss' ls' os' ns'.
  unfold query_same_rows_as_bag, query_table_bag, query_rows_bag.
  rewrite Febag.nb_occ_equal; intro x.
  assert (Hsort :
    @query_outputs_sort TNull q21_nation_table_outputs =S?=
      @_basesort TNull (D_of_embeds ss' ls' os' ns') (Rel "nation") = true).
  { change (@query_outputs_sort TNull q21_nation_table_outputs =S?=
      @_basesort TNull Schema.generated_schema (Rel "nation") = true).
    pose proof q21_nation_table_sort_ok as H.
    unfold Fset.equal in H |- *; exact H. }
  rewrite Hsort.
  rewrite D_of_embeds_instance_nation.
  rewrite !Febag.nb_occ_mk_bag; reflexivity.
Qed.

(** Filter(True) preserves the concrete input row list. *)
Lemma q21_filter_true_rows :
  forall basesort instance boolean_schedule env rows,
    @eval_filter_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule env SExpr_True rows (SqlSuccess rows).
Proof.
  intros basesort instance boolean_schedule env rows.
  assert (Hkeep :
    List.filter (fun _ : tuple TNull => true) rows = rows).
  { apply ListFacts.filter_true; intros; reflexivity. }
  pose proof (@tnull_filter_rows_success_of_acceptance
                basesort instance boolean_schedule env SExpr_True rows
                (fun _ : tuple TNull => true)
                (fun row (_ : List.In row rows) =>
                   @scalar_expr_true_acceptance_exact TNull relname
                     basesort instance unknown3
                     NullValues.interp_scalar_operator_runtime_error
                     NullValues.interp_aggregate_runtime_error
                     value_is_null boolean_schedule
                     (Env.env_t TNull env row))) as Hfilt.
  rewrite Hkeep in Hfilt; exact Hfilt.
Qed.

Lemma q21_filter_true_success :
  forall basesort instance boolean_schedule env input rows,
    @eval_query_expr_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule env input (SqlSuccess rows) ->
    @eval_query_expr_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule env (QExpr_Filter SExpr_True input)
      (SqlSuccess rows).
Proof.
  intros basesort instance boolean_schedule env input rows Hin.
  eapply EQuery_FilterRows; [exact Hin|].
  apply q21_filter_true_rows.
Qed.

(** CrossJoin success picking the concrete brute-force representative. *)
Lemma q21_cross_join_brute_success :
  forall basesort instance boolean_schedule env left right left_rows right_rows,
    @eval_query_expr_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule env left (SqlSuccess left_rows) ->
    @eval_query_expr_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule env right (SqlSuccess right_rows) ->
    @eval_query_expr_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule env (QExpr_CrossJoin left right)
      (SqlSuccess
         (brute_left_join_list (tuple TNull) (join_tuple TNull)
            left_rows right_rows)).
Proof.
  intros basesort instance boolean_schedule env left right left_rows right_rows
    Hl Hr.
  eapply EQuery_CrossJoinSuccess; [exact Hl|exact Hr|].
  apply raw_cross_same_rows_as_bag.
Qed.

(** Target E1 statement: [FormalSQL_exec] witnesses the concrete filtered
    projected CrossJoin list (Leibniz equality of the result list).

    Remaining discharge: intermediate Dot-only [Project] layers
    ([scalar_select_list_7/8/9/10/6/5]) via [ProjectExactFacade], then
    [Project(select_4)] + ordinary [Filter] via the same facades. *)
Definition Q21_FormalSQL_ordinary_outer_exec_statement : Prop :=
  forall (db : DB) (nation_name : string) (D : db_state)
         ss ls os ns ss' ls' os' ns'
         (keep_f : tuple TNull -> bool),
    Preconditions_Q21 db nation_name ->
    EmbedDB_Q21 db = Some D ->
    db_lookup db table_supplier = Some ss ->
    db_lookup db table_lineitem = Some ls ->
    db_lookup db table_orders = Some os ->
    db_lookup db table_nation = Some ns ->
    embed_rows EmbedRow_supplier ss = Some ss' ->
    embed_rows EmbedRow_lineitem ls = Some ls' ->
    embed_rows EmbedRow_orders os = Some os' ->
    embed_rows EmbedRow_nation ns = Some ns' ->
    (forall ft,
       List.In ft
         (List.map q21_project_select4
            (q21_cross4_tuples ss' ls' os' ns')) ->
       @scalar_expr_acceptance_exact_at TNull relname
         (@_basesort TNull D) (@_instance TNull D) unknown3
         NullValues.interp_scalar_operator_runtime_error
         NullValues.interp_aggregate_runtime_error value_is_null
         q21_boolean_schedule
         (Env.env_t TNull nil ft)
         (ordinary_outer_conjlist nation_name) (keep_f ft)) ->
    (forall ft,
       List.In ft (q21_cross4_tuples ss' ls' os' ns') ->
       @scalar_value_list_exact_at TNull relname
         (@_basesort TNull D) (@_instance TNull D) unknown3
         NullValues.interp_scalar_operator_runtime_error
         NullValues.interp_aggregate_runtime_error value_is_null
         q21_boolean_schedule
         (Env.env_t TNull nil ft)
         (map fst Queries.scalar_select_list_4)
         (q21_select4_dot_values ft)) ->
    FormalSQL_exec D (q_ordinary_outer nation_name)
      (List.filter keep_f
         (List.map q21_project_select4
            (q21_cross4_tuples ss' ls' os' ns'))).

(** Blocker status was: intermediate Dot-list exactness for spine Projects.
    Below: cheap shape lemmas (no vm_compute on select_5) + ProjectExact. *)

(** * STEP E1 — select_k shape lemmas (cheap; no vm_compute on select_5) *)

Lemma map_firstn_comm :
  forall {A B} (f : A -> B) n (l : list A),
    map f (firstn n l) = firstn n (map f l).
Proof.
  intros A B f n; induction n as [|n IH]; intros l; [reflexivity|].
  destruct l as [|x xs]; [reflexivity|simpl; f_equal; apply IH].
Qed.

Definition q21_sel7_exprs :
  list (@scalar_expr TNull relname ScalarResultValue) :=
  [@SExpr_Leaf TNull relname type_int32 (DotInt32 "s_suppkey");
   @SExpr_Leaf TNull relname type_string (DotString "s_name" (StringChar 25));
   @SExpr_Leaf TNull relname type_string
     (DotString "s_address" (StringVarcharN 40));
   @SExpr_Leaf TNull relname type_int32 (DotInt32 "s_nationkey");
   @SExpr_Leaf TNull relname type_string (DotString "s_phone" (StringChar 15));
   @SExpr_Leaf TNull relname type_numeric (DotDecimal "s_acctbal" 15 2);
   @SExpr_Leaf TNull relname type_string
     (DotString "s_comment" (StringVarcharN 101))].

Definition q21_sel7_attrs : list (attribute TNull) :=
  [AttrInt32 "s_suppkey";
   AttrString "s_name" (StringChar 25);
   AttrString "s_address" (StringVarcharN 40);
   AttrInt32 "s_nationkey";
   AttrString "s_phone" (StringChar 15);
   AttrDecimal "s_acctbal" 15 2;
   AttrString "s_comment" (StringVarcharN 101)].

Lemma firstn_7_map_fst_select5 :
  firstn 7 (map fst Queries.scalar_select_list_5) = q21_sel7_exprs.
Proof.
  unfold Queries.scalar_select_list_5, q21_sel7_exprs.
  cbn [firstn map fst]; reflexivity.
Qed.

Lemma firstn_7_map_snd_select5 :
  firstn 7 (map snd Queries.scalar_select_list_5) = q21_sel7_attrs.
Proof.
  unfold Queries.scalar_select_list_5, q21_sel7_attrs.
  cbn [firstn map snd]; reflexivity.
Qed.

Lemma map_fst_select7 :
  map fst Queries.scalar_select_list_7 = q21_sel7_exprs.
Proof.
  unfold Queries.scalar_select_list_7, Queries.scalar_select_list_6.
  rewrite !map_firstn_comm, firstn_firstn.
  change (Nat.min 7 23) with 7%nat.
  apply firstn_7_map_fst_select5.
Qed.

Lemma map_snd_select7 :
  map snd Queries.scalar_select_list_7 = q21_sel7_attrs.
Proof.
  unfold Queries.scalar_select_list_7, Queries.scalar_select_list_6.
  rewrite !map_firstn_comm, firstn_firstn.
  change (Nat.min 7 23) with 7%nat.
  apply firstn_7_map_snd_select5.
Qed.

Lemma sel7_attrs_eq_supplier_formal :
  q21_sel7_attrs = supplier_formal_attrs.
Proof. reflexivity. Qed.

Definition q21_sel8_exprs :
  list (@scalar_expr TNull relname ScalarResultValue) :=
  [@SExpr_Leaf TNull relname type_int32 (DotInt32 "l_orderkey");
   @SExpr_Leaf TNull relname type_int32 (DotInt32 "l_partkey");
   @SExpr_Leaf TNull relname type_int32 (DotInt32 "l_suppkey");
   @SExpr_Leaf TNull relname type_int32 (DotInt32 "l_linenumber");
   @SExpr_Leaf TNull relname type_numeric (DotDecimal "l_quantity" 15 2);
   @SExpr_Leaf TNull relname type_numeric (DotDecimal "l_extendedprice" 15 2);
   @SExpr_Leaf TNull relname type_numeric (DotDecimal "l_discount" 15 2);
   @SExpr_Leaf TNull relname type_numeric (DotDecimal "l_tax" 15 2);
   @SExpr_Leaf TNull relname type_string (DotString "l_returnflag" (StringChar 1));
   @SExpr_Leaf TNull relname type_string (DotString "l_linestatus" (StringChar 1));
   @SExpr_Leaf TNull relname type_date (DotDate "l_shipdate");
   @SExpr_Leaf TNull relname type_date (DotDate "l_commitdate");
   @SExpr_Leaf TNull relname type_date (DotDate "l_receiptdate");
   @SExpr_Leaf TNull relname type_string
     (DotString "l_shipinstruct" (StringChar 25));
   @SExpr_Leaf TNull relname type_string (DotString "l_shipmode" (StringChar 10));
   @SExpr_Leaf TNull relname type_string
     (DotString "l_comment" (StringVarcharN 44))].

Lemma map_fst_select8 :
  map fst Queries.scalar_select_list_8 = q21_sel8_exprs.
Proof.
  unfold Queries.scalar_select_list_8, q21_sel8_exprs.
  cbn [map fst]; reflexivity.
Qed.

Definition q21_sel8_attrs : list (attribute TNull) :=
  [AttrInt32 "l_orderkey"; AttrInt32 "l_partkey"; AttrInt32 "l_suppkey";
   AttrInt32 "l_linenumber"; AttrDecimal "l_quantity" 15 2;
   AttrDecimal "l_extendedprice" 15 2; AttrDecimal "l_discount" 15 2;
   AttrDecimal "l_tax" 15 2; AttrString "l_returnflag" (StringChar 1);
   AttrString "l_linestatus" (StringChar 1); AttrDate "l_shipdate";
   AttrDate "l_commitdate"; AttrDate "l_receiptdate";
   AttrString "l_shipinstruct" (StringChar 25);
   AttrString "l_shipmode" (StringChar 10);
   AttrString "l_comment" (StringVarcharN 44)].

Lemma map_snd_select8 :
  map snd Queries.scalar_select_list_8 = q21_sel8_attrs.
Proof.
  unfold Queries.scalar_select_list_8, q21_sel8_attrs.
  cbn [map snd]; reflexivity.
Qed.

Definition q21_sel9_exprs :
  list (@scalar_expr TNull relname ScalarResultValue) :=
  [@SExpr_Leaf TNull relname type_int32 (DotInt32 "o_orderkey");
   @SExpr_Leaf TNull relname type_int32 (DotInt32 "o_custkey");
   @SExpr_Leaf TNull relname type_string
     (DotString "o_orderstatus" (StringChar 1));
   @SExpr_Leaf TNull relname type_numeric (DotDecimal "o_totalprice" 15 2);
   @SExpr_Leaf TNull relname type_date (DotDate "o_orderdate");
   @SExpr_Leaf TNull relname type_string
     (DotString "o_orderpriority" (StringChar 15));
   @SExpr_Leaf TNull relname type_string (DotString "o_clerk" (StringChar 15));
   @SExpr_Leaf TNull relname type_int32 (DotInt32 "o_shippriority");
   @SExpr_Leaf TNull relname type_string
     (DotString "o_comment" (StringVarcharN 79))].

Lemma map_fst_select9 :
  map fst Queries.scalar_select_list_9 = q21_sel9_exprs.
Proof.
  unfold Queries.scalar_select_list_9, q21_sel9_exprs.
  cbn [map fst]; reflexivity.
Qed.

Definition q21_sel9_attrs : list (attribute TNull) :=
  [AttrInt32 "o_orderkey"; AttrInt32 "o_custkey";
   AttrString "o_orderstatus" (StringChar 1);
   AttrDecimal "o_totalprice" 15 2; AttrDate "o_orderdate";
   AttrString "o_orderpriority" (StringChar 15);
   AttrString "o_clerk" (StringChar 15); AttrInt32 "o_shippriority";
   AttrString "o_comment" (StringVarcharN 79)].

Lemma map_snd_select9 :
  map snd Queries.scalar_select_list_9 = q21_sel9_attrs.
Proof.
  unfold Queries.scalar_select_list_9, q21_sel9_attrs.
  cbn [map snd]; reflexivity.
Qed.

Definition q21_sel10_exprs :
  list (@scalar_expr TNull relname ScalarResultValue) :=
  [@SExpr_Leaf TNull relname type_int32 (DotInt32 "n_nationkey");
   @SExpr_Leaf TNull relname type_string (DotString "n_name" (StringChar 25));
   @SExpr_Leaf TNull relname type_int32 (DotInt32 "n_regionkey");
   @SExpr_Leaf TNull relname type_string
     (DotString "n_comment" (StringVarcharN 152))].

Lemma map_fst_select10 :
  map fst Queries.scalar_select_list_10 = q21_sel10_exprs.
Proof.
  unfold Queries.scalar_select_list_10, q21_sel10_exprs.
  cbn [map fst]; reflexivity.
Qed.

Definition q21_sel10_attrs : list (attribute TNull) :=
  [AttrInt32 "n_nationkey"; AttrString "n_name" (StringChar 25);
   AttrInt32 "n_regionkey"; AttrString "n_comment" (StringVarcharN 152)].

Lemma map_snd_select10 :
  map snd Queries.scalar_select_list_10 = q21_sel10_attrs.
Proof.
  unfold Queries.scalar_select_list_10, q21_sel10_attrs.
  cbn [map snd]; reflexivity.
Qed.

(** select_6 = firstn 23 select_5 = supplier (7) ++ lineitem (16). *)
Lemma firstn_23_map_fst_select5 :
  firstn 23 (map fst Queries.scalar_select_list_5) =
  List.app q21_sel7_exprs q21_sel8_exprs.
Proof.
  unfold Queries.scalar_select_list_5, q21_sel7_exprs, q21_sel8_exprs.
  cbn [firstn map fst List.app]; reflexivity.
Qed.

Lemma map_fst_select6 :
  map fst Queries.scalar_select_list_6 =
  List.app q21_sel7_exprs q21_sel8_exprs.
Proof.
  unfold Queries.scalar_select_list_6.
  rewrite map_firstn_comm.
  apply firstn_23_map_fst_select5.
Qed.

Lemma firstn_23_map_snd_select5 :
  firstn 23 (map snd Queries.scalar_select_list_5) =
  List.app q21_sel7_attrs q21_sel8_attrs.
Proof.
  unfold Queries.scalar_select_list_5, q21_sel7_attrs, q21_sel8_attrs.
  cbn [firstn map snd List.app]; reflexivity.
Qed.

Lemma map_snd_select6 :
  map snd Queries.scalar_select_list_6 =
  List.app q21_sel7_attrs q21_sel8_attrs.
Proof.
  unfold Queries.scalar_select_list_6.
  rewrite map_firstn_comm.
  apply firstn_23_map_snd_select5.
Qed.

Lemma map_snd_select5 :
  map snd Queries.scalar_select_list_5 =
  List.app (List.app q21_sel7_attrs q21_sel8_attrs) q21_sel9_attrs.
Proof.
  unfold Queries.scalar_select_list_5, q21_sel7_attrs, q21_sel8_attrs,
    q21_sel9_attrs.
  cbn [map snd List.app]; reflexivity.
Qed.

(** * Dot value lists + ProjectExact for table selects *)

Definition q21_sel7_values (ft : tuple TNull) : list value :=
  [ dot TNull ft (AttrInt32 "s_suppkey")
  ; dot TNull ft (AttrString "s_name" (StringChar 25))
  ; dot TNull ft (AttrString "s_address" (StringVarcharN 40))
  ; dot TNull ft (AttrInt32 "s_nationkey")
  ; dot TNull ft (AttrString "s_phone" (StringChar 15))
  ; dot TNull ft (AttrDecimal "s_acctbal" 15 2)
  ; dot TNull ft (AttrString "s_comment" (StringVarcharN 101))
  ].

Definition q21_sel8_values (ft : tuple TNull) : list value :=
  [ dot TNull ft (AttrInt32 "l_orderkey")
  ; dot TNull ft (AttrInt32 "l_partkey")
  ; dot TNull ft (AttrInt32 "l_suppkey")
  ; dot TNull ft (AttrInt32 "l_linenumber")
  ; dot TNull ft (AttrDecimal "l_quantity" 15 2)
  ; dot TNull ft (AttrDecimal "l_extendedprice" 15 2)
  ; dot TNull ft (AttrDecimal "l_discount" 15 2)
  ; dot TNull ft (AttrDecimal "l_tax" 15 2)
  ; dot TNull ft (AttrString "l_returnflag" (StringChar 1))
  ; dot TNull ft (AttrString "l_linestatus" (StringChar 1))
  ; dot TNull ft (AttrDate "l_shipdate")
  ; dot TNull ft (AttrDate "l_commitdate")
  ; dot TNull ft (AttrDate "l_receiptdate")
  ; dot TNull ft (AttrString "l_shipinstruct" (StringChar 25))
  ; dot TNull ft (AttrString "l_shipmode" (StringChar 10))
  ; dot TNull ft (AttrString "l_comment" (StringVarcharN 44))
  ].

Definition q21_sel9_values (ft : tuple TNull) : list value :=
  [ dot TNull ft (AttrInt32 "o_orderkey")
  ; dot TNull ft (AttrInt32 "o_custkey")
  ; dot TNull ft (AttrString "o_orderstatus" (StringChar 1))
  ; dot TNull ft (AttrDecimal "o_totalprice" 15 2)
  ; dot TNull ft (AttrDate "o_orderdate")
  ; dot TNull ft (AttrString "o_orderpriority" (StringChar 15))
  ; dot TNull ft (AttrString "o_clerk" (StringChar 15))
  ; dot TNull ft (AttrInt32 "o_shippriority")
  ; dot TNull ft (AttrString "o_comment" (StringVarcharN 79))
  ].

Definition q21_sel10_values (ft : tuple TNull) : list value :=
  [ dot TNull ft (AttrInt32 "n_nationkey")
  ; dot TNull ft (AttrString "n_name" (StringChar 25))
  ; dot TNull ft (AttrInt32 "n_regionkey")
  ; dot TNull ft (AttrString "n_comment" (StringVarcharN 152))
  ].

Ltac q21_dot_cons :=
  eapply (@scalar_value_list_exact_cons TNull relname);
  [ first
      [ eapply tnull_dot_int32_exact; apply interp_dot_env_t; eassumption
      | eapply tnull_dot_string_exact; apply interp_dot_env_t; eassumption
      | eapply tnull_dot_decimal_exact; apply interp_dot_env_t; eassumption
      | eapply tnull_dot_date_exact; apply interp_dot_env_t; eassumption ]
  | ].

Lemma q21_sel7_exact :
  forall basesort instance boolean_schedule env ft,
    labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs ->
    @scalar_value_list_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule (Env.env_t TNull env ft)
      (map fst Queries.scalar_select_list_7)
      (q21_sel7_values ft).
Proof.
  intros basesort instance boolean_schedule env ft Hl.
  rewrite map_fst_select7; unfold q21_sel7_exprs, q21_sel7_values.
  assert (Ha0 : AttrInt32 "s_suppkey" inS labels TNull ft)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl); apply supplier_attr_inS_suppkey).
  assert (Ha1 : AttrString "s_name" (StringChar 25) inS labels TNull ft)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl); apply supplier_attr_inS_s_name).
  assert (Ha2 : AttrString "s_address" (StringVarcharN 40) inS labels TNull ft).
  { rewrite (Fset.mem_eq_2 _ _ _ Hl).
    change ((AttrString "s_address" (StringVarcharN 40) inS?
               Fset.mk_set (A TNull) supplier_formal_attrs) = true).
    vm_compute; reflexivity. }
  assert (Ha3 : AttrInt32 "s_nationkey" inS labels TNull ft)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl); apply supplier_attr_inS_nationkey).
  assert (Ha4 : AttrString "s_phone" (StringChar 15) inS labels TNull ft).
  { rewrite (Fset.mem_eq_2 _ _ _ Hl).
    change ((AttrString "s_phone" (StringChar 15) inS?
               Fset.mk_set (A TNull) supplier_formal_attrs) = true).
    vm_compute; reflexivity. }
  assert (Ha5 : AttrDecimal "s_acctbal" 15 2 inS labels TNull ft).
  { rewrite (Fset.mem_eq_2 _ _ _ Hl).
    change ((AttrDecimal "s_acctbal" 15 2 inS?
               Fset.mk_set (A TNull) supplier_formal_attrs) = true).
    vm_compute; reflexivity. }
  assert (Ha6 : AttrString "s_comment" (StringVarcharN 101) inS labels TNull ft).
  { rewrite (Fset.mem_eq_2 _ _ _ Hl).
    change ((AttrString "s_comment" (StringVarcharN 101) inS?
               Fset.mk_set (A TNull) supplier_formal_attrs) = true).
    vm_compute; reflexivity. }
  q21_dot_cons. q21_dot_cons. q21_dot_cons. q21_dot_cons.
  q21_dot_cons. q21_dot_cons. q21_dot_cons.
  apply (@scalar_value_list_exact_nil TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule).
Qed.

Definition lineitem_formal_attrs : list (attribute TNull) :=
  [Attr_int32 "l_orderkey"; Attr_int32 "l_partkey";
   Attr_int32 "l_suppkey"; Attr_int32 "l_linenumber";
   Attr_decimal "l_quantity" 15 2; Attr_decimal "l_extendedprice" 15 2;
   Attr_decimal "l_discount" 15 2; Attr_decimal "l_tax" 15 2;
   Attr_string "l_returnflag" (StringChar 1);
   Attr_string "l_linestatus" (StringChar 1);
   Attr_date "l_shipdate"; Attr_date "l_commitdate";
   Attr_date "l_receiptdate";
   Attr_string "l_shipinstruct" (StringChar 25);
   Attr_string "l_shipmode" (StringChar 10);
   Attr_string "l_comment" (StringVarcharN 44)].

Lemma sel8_attrs_eq_lineitem_formal :
  q21_sel8_attrs = lineitem_formal_attrs.
Proof. reflexivity. Qed.

Definition orders_formal_attrs : list (attribute TNull) :=
  [Attr_int32 "o_orderkey"; Attr_int32 "o_custkey";
   Attr_string "o_orderstatus" (StringChar 1);
   Attr_decimal "o_totalprice" 15 2; Attr_date "o_orderdate";
   Attr_string "o_orderpriority" (StringChar 15);
   Attr_string "o_clerk" (StringChar 15);
   Attr_int32 "o_shippriority";
   Attr_string "o_comment" (StringVarcharN 79)].

Lemma sel9_attrs_eq_orders_formal :
  q21_sel9_attrs = orders_formal_attrs.
Proof. reflexivity. Qed.

Definition nation_formal_attrs : list (attribute TNull) :=
  [Attr_int32 "n_nationkey";
   Attr_string "n_name" (StringChar 25);
   Attr_int32 "n_regionkey";
   Attr_string "n_comment" (StringVarcharN 152)].

Lemma sel10_attrs_eq_nation_formal :
  q21_sel10_attrs = nation_formal_attrs.
Proof. reflexivity. Qed.

Ltac q21_attr_inS_of_labels Hl :=
  rewrite (Fset.mem_eq_2 _ _ _ Hl);
  match goal with
  | |- ?a inS Fset.mk_set _ ?attrs =>
      change ((a inS? Fset.mk_set (A TNull) attrs) = true);
      vm_compute; reflexivity
  end.

Lemma q21_sel8_exact :
  forall basesort instance boolean_schedule env ft,
    labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs ->
    @scalar_value_list_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule (Env.env_t TNull env ft)
      (map fst Queries.scalar_select_list_8)
      (q21_sel8_values ft).
Proof.
  intros basesort instance boolean_schedule env ft Hl.
  rewrite map_fst_select8; unfold q21_sel8_exprs, q21_sel8_values.
  assert (Ha0 : AttrInt32 "l_orderkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha1 : AttrInt32 "l_partkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha2 : AttrInt32 "l_suppkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha3 : AttrInt32 "l_linenumber" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha4 : AttrDecimal "l_quantity" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha5 : AttrDecimal "l_extendedprice" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha6 : AttrDecimal "l_discount" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha7 : AttrDecimal "l_tax" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha8 : AttrString "l_returnflag" (StringChar 1) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha9 : AttrString "l_linestatus" (StringChar 1) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha10 : AttrDate "l_shipdate" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha11 : AttrDate "l_commitdate" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha12 : AttrDate "l_receiptdate" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha13 : AttrString "l_shipinstruct" (StringChar 25) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha14 : AttrString "l_shipmode" (StringChar 10) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha15 : AttrString "l_comment" (StringVarcharN 44) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  do 16 q21_dot_cons.
  apply (@scalar_value_list_exact_nil TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule).
Qed.

Lemma q21_sel9_exact :
  forall basesort instance boolean_schedule env ft,
    labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs ->
    @scalar_value_list_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule (Env.env_t TNull env ft)
      (map fst Queries.scalar_select_list_9)
      (q21_sel9_values ft).
Proof.
  intros basesort instance boolean_schedule env ft Hl.
  rewrite map_fst_select9; unfold q21_sel9_exprs, q21_sel9_values.
  assert (Ha0 : AttrInt32 "o_orderkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha1 : AttrInt32 "o_custkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha2 : AttrString "o_orderstatus" (StringChar 1) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha3 : AttrDecimal "o_totalprice" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha4 : AttrDate "o_orderdate" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha5 : AttrString "o_orderpriority" (StringChar 15) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha6 : AttrString "o_clerk" (StringChar 15) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha7 : AttrInt32 "o_shippriority" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha8 : AttrString "o_comment" (StringVarcharN 79) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  do 9 q21_dot_cons.
  apply (@scalar_value_list_exact_nil TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule).
Qed.

Lemma q21_sel10_exact :
  forall basesort instance boolean_schedule env ft,
    labels TNull ft =S= Fset.mk_set (A TNull) nation_formal_attrs ->
    @scalar_value_list_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule (Env.env_t TNull env ft)
      (map fst Queries.scalar_select_list_10)
      (q21_sel10_values ft).
Proof.
  intros basesort instance boolean_schedule env ft Hl.
  rewrite map_fst_select10; unfold q21_sel10_exprs, q21_sel10_values.
  assert (Ha0 : AttrInt32 "n_nationkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha1 : AttrString "n_name" (StringChar 25) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha2 : AttrInt32 "n_regionkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha3 : AttrString "n_comment" (StringVarcharN 152) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  do 4 q21_dot_cons.
  apply (@scalar_value_list_exact_nil TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule).
Qed.

(** Embed rows carry full formal labels. *)
Lemma EmbedRow_supplier_labels :
  forall r ft,
    EmbedRow_supplier r = Some ft ->
    labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs.
Proof.
  intros r ft Hemb.
  revert ft Hemb.
  refine (match r with
          | [VInt _; VText _; VInt _] => _
          | _ => fun ft H => ltac:(discriminate H)
          end).
  intros ft Hemb.
  unfold EmbedRow_supplier in Hemb.
  destruct (int32_checked _) as [?|]; [|discriminate Hemb].
  destruct (int32_checked _) as [?|]; [|discriminate Hemb].
  inversion Hemb; apply mk_tuple_lists_labels.
Qed.

Lemma EmbedRow_lineitem_labels :
  forall r ft,
    EmbedRow_lineitem r = Some ft ->
    labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs.
Proof.
  intros r ft Hemb.
  revert ft Hemb.
  refine (match r with
          | [VInt _; VInt _; VInt _; VInt _] => _
          | _ => fun ft H => ltac:(discriminate H)
          end).
  intros ft Hemb.
  unfold EmbedRow_lineitem in Hemb.
  destruct (int32_checked _) as [?|]; [|discriminate Hemb].
  destruct (int32_checked _) as [?|]; [|discriminate Hemb].
  inversion Hemb; apply mk_tuple_lists_labels.
Qed.

Lemma EmbedRow_orders_labels :
  forall r ft,
    EmbedRow_orders r = Some ft ->
    labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs.
Proof.
  intros r ft Hemb.
  revert ft Hemb.
  refine (match r with
          | [VInt _; VText _] => _
          | _ => fun ft H => ltac:(discriminate H)
          end).
  intros ft Hemb.
  unfold EmbedRow_orders in Hemb.
  destruct (int32_checked _) as [?|]; [|discriminate Hemb].
  inversion Hemb; apply mk_tuple_lists_labels.
Qed.

Lemma EmbedRow_nation_labels :
  forall r ft,
    EmbedRow_nation r = Some ft ->
    labels TNull ft =S= Fset.mk_set (A TNull) nation_formal_attrs.
Proof.
  intros r ft Hemb.
  revert ft Hemb.
  refine (match r with
          | [VInt _; VText _] => _
          | _ => fun ft H => ltac:(discriminate H)
          end).
  intros ft Hemb.
  unfold EmbedRow_nation in Hemb.
  destruct (int32_checked _) as [?|]; [|discriminate Hemb].
  inversion Hemb; apply mk_tuple_lists_labels.
Qed.

Lemma embed_rows_Forall_labels_supplier :
  forall ss ss',
    embed_rows EmbedRow_supplier ss = Some ss' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs)
      ss'.
Proof.
  induction ss as [|r rs IH]; intros ss' H; simpl in H.
  - inversion H; subst; constructor.
  - destruct (EmbedRow_supplier r) as [ft|] eqn:Hft; [|discriminate H].
    case_eq (embed_rows EmbedRow_supplier rs).
    + intros fts Hfs; rewrite Hfs in H; inversion H; subst ss'; clear H.
      constructor.
      * eapply EmbedRow_supplier_labels; exact Hft.
      * apply IH; exact Hfs.
    + intros Hfs; rewrite Hfs in H; discriminate H.
Qed.

Lemma embed_rows_Forall_labels_lineitem :
  forall ls ls',
    embed_rows EmbedRow_lineitem ls = Some ls' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs)
      ls'.
Proof.
  induction ls as [|r rs IH]; intros ls' H; simpl in H.
  - inversion H; subst; constructor.
  - destruct (EmbedRow_lineitem r) as [ft|] eqn:Hft; [|discriminate H].
    case_eq (embed_rows EmbedRow_lineitem rs).
    + intros fts Hfs; rewrite Hfs in H; inversion H; subst ls'; clear H.
      constructor.
      * eapply EmbedRow_lineitem_labels; exact Hft.
      * apply IH; exact Hfs.
    + intros Hfs; rewrite Hfs in H; discriminate H.
Qed.

Lemma embed_rows_Forall_labels_orders :
  forall os os',
    embed_rows EmbedRow_orders os = Some os' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs)
      os'.
Proof.
  induction os as [|r rs IH]; intros os' H; simpl in H.
  - inversion H; subst; constructor.
  - destruct (EmbedRow_orders r) as [ft|] eqn:Hft; [|discriminate H].
    case_eq (embed_rows EmbedRow_orders rs).
    + intros fts Hfs; rewrite Hfs in H; inversion H; subst os'; clear H.
      constructor.
      * eapply EmbedRow_orders_labels; exact Hft.
      * apply IH; exact Hfs.
    + intros Hfs; rewrite Hfs in H; discriminate H.
Qed.

Lemma embed_rows_Forall_labels_nation :
  forall ns ns',
    embed_rows EmbedRow_nation ns = Some ns' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) nation_formal_attrs)
      ns'.
Proof.
  induction ns as [|r rs IH]; intros ns' H; simpl in H.
  - inversion H; subst; constructor.
  - destruct (EmbedRow_nation r) as [ft|] eqn:Hft; [|discriminate H].
    case_eq (embed_rows EmbedRow_nation rs).
    + intros fts Hfs; rewrite Hfs in H; inversion H; subst ns'; clear H.
      constructor.
      * eapply EmbedRow_nation_labels; exact Hft.
      * apply IH; exact Hfs.
    + intros Hfs; rewrite Hfs in H; discriminate H.
Qed.

(** ProjectRows success for identity table selects. *)
Lemma q21_project_sel7_success :
  forall basesort instance boolean_schedule env rows,
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs)
      rows ->
    @eval_project_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule env Queries.scalar_select_list_7 rows
      (SqlSuccess
         (List.map (fun ft => project_row Queries.scalar_select_list_7
                                  (q21_sel7_values ft)) rows)).
Proof.
  intros basesort instance boolean_schedule env rows Hlab.
  eapply (@tnull_project_rows_success_of_exact_map
            basesort instance boolean_schedule).
  intros row Hin.
  apply q21_sel7_exact.
  rewrite Forall_forall in Hlab; apply Hlab; exact Hin.
Qed.

Lemma q21_project_sel8_success :
  forall basesort instance boolean_schedule env rows,
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs)
      rows ->
    @eval_project_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule env Queries.scalar_select_list_8 rows
      (SqlSuccess
         (List.map (fun ft => project_row Queries.scalar_select_list_8
                                  (q21_sel8_values ft)) rows)).
Proof.
  intros basesort instance boolean_schedule env rows Hlab.
  eapply (@tnull_project_rows_success_of_exact_map
            basesort instance boolean_schedule).
  intros row Hin.
  apply q21_sel8_exact.
  rewrite Forall_forall in Hlab; apply Hlab; exact Hin.
Qed.

Lemma q21_project_sel9_success :
  forall basesort instance boolean_schedule env rows,
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs)
      rows ->
    @eval_project_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule env Queries.scalar_select_list_9 rows
      (SqlSuccess
         (List.map (fun ft => project_row Queries.scalar_select_list_9
                                  (q21_sel9_values ft)) rows)).
Proof.
  intros basesort instance boolean_schedule env rows Hlab.
  eapply (@tnull_project_rows_success_of_exact_map
            basesort instance boolean_schedule).
  intros row Hin.
  apply q21_sel9_exact.
  rewrite Forall_forall in Hlab; apply Hlab; exact Hin.
Qed.

Lemma q21_project_sel10_success :
  forall basesort instance boolean_schedule env rows,
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) nation_formal_attrs)
      rows ->
    @eval_project_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule env Queries.scalar_select_list_10 rows
      (SqlSuccess
         (List.map (fun ft => project_row Queries.scalar_select_list_10
                                  (q21_sel10_values ft)) rows)).
Proof.
  intros basesort instance boolean_schedule env rows Hlab.
  eapply (@tnull_project_rows_success_of_exact_map
            basesort instance boolean_schedule).
  intros row Hin.
  apply q21_sel10_exact.
  rewrite Forall_forall in Hlab; apply Hlab; exact Hin.
Qed.

(** select_6 values = supplier dots ++ lineitem dots on a joined tuple. *)
Definition q21_sel6_values (ft : tuple TNull) : list value :=
  List.app (q21_sel7_values ft) (q21_sel8_values ft).

Definition supplier_lineitem_formal_attrs : list (attribute TNull) :=
  List.app supplier_formal_attrs lineitem_formal_attrs.

Lemma q21_sel6_exact :
  forall basesort instance boolean_schedule env ft,
    labels TNull ft =S= Fset.mk_set (A TNull) supplier_lineitem_formal_attrs ->
    @scalar_value_list_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule (Env.env_t TNull env ft)
      (map fst Queries.scalar_select_list_6)
      (q21_sel6_values ft).
Proof.
  intros basesort instance boolean_schedule env ft Hl.
  rewrite map_fst_select6; unfold q21_sel7_exprs, q21_sel8_exprs, q21_sel6_values.
  unfold q21_sel7_values, q21_sel8_values.
  (* 23 attributes: 7 supplier + 16 lineitem *)
  assert (Ha00 : AttrInt32 "s_suppkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha01 : AttrString "s_name" (StringChar 25) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha02 : AttrString "s_address" (StringVarcharN 40) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha03 : AttrInt32 "s_nationkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha04 : AttrString "s_phone" (StringChar 15) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha05 : AttrDecimal "s_acctbal" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha06 : AttrString "s_comment" (StringVarcharN 101) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha07 : AttrInt32 "l_orderkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha08 : AttrInt32 "l_partkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha09 : AttrInt32 "l_suppkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha10 : AttrInt32 "l_linenumber" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha11 : AttrDecimal "l_quantity" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha12 : AttrDecimal "l_extendedprice" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha13 : AttrDecimal "l_discount" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha14 : AttrDecimal "l_tax" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha15 : AttrString "l_returnflag" (StringChar 1) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha16 : AttrString "l_linestatus" (StringChar 1) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha17 : AttrDate "l_shipdate" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha18 : AttrDate "l_commitdate" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha19 : AttrDate "l_receiptdate" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha20 : AttrString "l_shipinstruct" (StringChar 25) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha21 : AttrString "l_shipmode" (StringChar 10) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha22 : AttrString "l_comment" (StringVarcharN 44) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  do 23 q21_dot_cons.
  apply (@scalar_value_list_exact_nil TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule).
Qed.

Lemma q21_project_sel6_success :
  forall basesort instance boolean_schedule env rows,
    List.Forall
      (fun ft =>
         labels TNull ft =S= Fset.mk_set (A TNull) supplier_lineitem_formal_attrs)
      rows ->
    @eval_project_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule env Queries.scalar_select_list_6 rows
      (SqlSuccess
         (List.map (fun ft => project_row Queries.scalar_select_list_6
                                  (q21_sel6_values ft)) rows)).
Proof.
  intros basesort instance boolean_schedule env rows Hlab.
  eapply (@tnull_project_rows_success_of_exact_map
            basesort instance boolean_schedule).
  intros row Hin.
  apply q21_sel6_exact.
  rewrite Forall_forall in Hlab; apply Hlab; exact Hin.
Qed.

(** select_5 = supplier∥lineitem∥orders (32 attrs). *)
Definition q21_sel5_values (ft : tuple TNull) : list value :=
  List.app (q21_sel6_values ft) (q21_sel9_values ft).

Definition supplier_lineitem_orders_formal_attrs : list (attribute TNull) :=
  List.app supplier_lineitem_formal_attrs orders_formal_attrs.

Lemma map_fst_select5 :
  map fst Queries.scalar_select_list_5 =
  List.app (List.app q21_sel7_exprs q21_sel8_exprs) q21_sel9_exprs.
Proof.
  unfold Queries.scalar_select_list_5, q21_sel7_exprs, q21_sel8_exprs,
    q21_sel9_exprs.
  cbn [map fst List.app]; reflexivity.
Qed.

Lemma q21_sel5_exact :
  forall basesort instance boolean_schedule env ft,
    labels TNull ft =S=
      Fset.mk_set (A TNull) supplier_lineitem_orders_formal_attrs ->
    @scalar_value_list_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule (Env.env_t TNull env ft)
      (map fst Queries.scalar_select_list_5)
      (q21_sel5_values ft).
Proof.
  intros basesort instance boolean_schedule env ft Hl.
  rewrite map_fst_select5;
    unfold q21_sel7_exprs, q21_sel8_exprs, q21_sel9_exprs, q21_sel5_values,
      q21_sel6_values, q21_sel7_values, q21_sel8_values, q21_sel9_values.
  assert (Ha00 : AttrInt32 "s_suppkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha01 : AttrString "s_name" (StringChar 25) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha02 : AttrString "s_address" (StringVarcharN 40) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha03 : AttrInt32 "s_nationkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha04 : AttrString "s_phone" (StringChar 15) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha05 : AttrDecimal "s_acctbal" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha06 : AttrString "s_comment" (StringVarcharN 101) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha07 : AttrInt32 "l_orderkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha08 : AttrInt32 "l_partkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha09 : AttrInt32 "l_suppkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha10 : AttrInt32 "l_linenumber" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha11 : AttrDecimal "l_quantity" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha12 : AttrDecimal "l_extendedprice" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha13 : AttrDecimal "l_discount" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha14 : AttrDecimal "l_tax" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha15 : AttrString "l_returnflag" (StringChar 1) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha16 : AttrString "l_linestatus" (StringChar 1) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha17 : AttrDate "l_shipdate" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha18 : AttrDate "l_commitdate" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha19 : AttrDate "l_receiptdate" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha20 : AttrString "l_shipinstruct" (StringChar 25) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha21 : AttrString "l_shipmode" (StringChar 10) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha22 : AttrString "l_comment" (StringVarcharN 44) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha23 : AttrInt32 "o_orderkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha24 : AttrInt32 "o_custkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha25 : AttrString "o_orderstatus" (StringChar 1) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha26 : AttrDecimal "o_totalprice" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha27 : AttrDate "o_orderdate" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha28 : AttrString "o_orderpriority" (StringChar 15) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha29 : AttrString "o_clerk" (StringChar 15) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha30 : AttrInt32 "o_shippriority" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha31 : AttrString "o_comment" (StringVarcharN 79) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  do 32 q21_dot_cons.
  apply (@scalar_value_list_exact_nil TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule).
Qed.

Lemma q21_project_sel5_success :
  forall basesort instance boolean_schedule env rows,
    List.Forall
      (fun ft =>
         labels TNull ft =S=
           Fset.mk_set (A TNull) supplier_lineitem_orders_formal_attrs)
      rows ->
    @eval_project_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule env Queries.scalar_select_list_5 rows
      (SqlSuccess
         (List.map (fun ft => project_row Queries.scalar_select_list_5
                                  (q21_sel5_values ft)) rows)).
Proof.
  intros basesort instance boolean_schedule env rows Hlab.
  eapply (@tnull_project_rows_success_of_exact_map
            basesort instance boolean_schedule).
  intros row Hin.
  apply q21_sel5_exact.
  rewrite Forall_forall in Hlab; apply Hlab; exact Hin.
Qed.

(** * project_row labels + watched-dot characterizations (Q1 Dot-cons style) *)

Lemma q21_project_row_labels :
  forall select_list values,
    labels TNull (@project_row TNull relname select_list values) =S=
    Fset.mk_set (A TNull) (scalar_select_outputs select_list).
Proof.
  intros select_list values.
  unfold project_row, scalar_select_outputs.
  apply labels_mk_tuple.
Qed.

Lemma q21_project_sel7_labels :
  forall vs,
    labels TNull (project_row Queries.scalar_select_list_7 vs) =S=
    Fset.mk_set (A TNull) supplier_formal_attrs.
Proof.
  intros vs.
  rewrite (Fset.equal_eq_1 _ _ _ _
             (q21_project_row_labels Queries.scalar_select_list_7 vs)).
  unfold scalar_select_outputs.
  rewrite map_snd_select7, sel7_attrs_eq_supplier_formal.
  apply Fset.equal_refl.
Qed.

Lemma q21_project_sel8_labels :
  forall vs,
    labels TNull (project_row Queries.scalar_select_list_8 vs) =S=
    Fset.mk_set (A TNull) lineitem_formal_attrs.
Proof.
  intros vs.
  rewrite (Fset.equal_eq_1 _ _ _ _
             (q21_project_row_labels Queries.scalar_select_list_8 vs)).
  unfold scalar_select_outputs.
  rewrite map_snd_select8, sel8_attrs_eq_lineitem_formal.
  apply Fset.equal_refl.
Qed.

Lemma q21_project_sel9_labels :
  forall vs,
    labels TNull (project_row Queries.scalar_select_list_9 vs) =S=
    Fset.mk_set (A TNull) orders_formal_attrs.
Proof.
  intros vs.
  rewrite (Fset.equal_eq_1 _ _ _ _
             (q21_project_row_labels Queries.scalar_select_list_9 vs)).
  unfold scalar_select_outputs.
  rewrite map_snd_select9, sel9_attrs_eq_orders_formal.
  apply Fset.equal_refl.
Qed.

Lemma q21_project_sel10_labels :
  forall vs,
    labels TNull (project_row Queries.scalar_select_list_10 vs) =S=
    Fset.mk_set (A TNull) nation_formal_attrs.
Proof.
  intros vs.
  rewrite (Fset.equal_eq_1 _ _ _ _
             (q21_project_row_labels Queries.scalar_select_list_10 vs)).
  unfold scalar_select_outputs.
  rewrite map_snd_select10, sel10_attrs_eq_nation_formal.
  apply Fset.equal_refl.
Qed.

Lemma q21_project_sel6_labels :
  forall vs,
    labels TNull (project_row Queries.scalar_select_list_6 vs) =S=
    Fset.mk_set (A TNull) supplier_lineitem_formal_attrs.
Proof.
  intros vs.
  rewrite (Fset.equal_eq_1 _ _ _ _
             (q21_project_row_labels Queries.scalar_select_list_6 vs)).
  unfold scalar_select_outputs, supplier_lineitem_formal_attrs.
  rewrite map_snd_select6, sel7_attrs_eq_supplier_formal,
    sel8_attrs_eq_lineitem_formal.
  apply Fset.equal_refl.
Qed.

Lemma q21_project_sel5_labels :
  forall vs,
    labels TNull (project_row Queries.scalar_select_list_5 vs) =S=
    Fset.mk_set (A TNull) supplier_lineitem_orders_formal_attrs.
Proof.
  intros vs.
  rewrite (Fset.equal_eq_1 _ _ _ _
             (q21_project_row_labels Queries.scalar_select_list_5 vs)).
  unfold scalar_select_outputs, supplier_lineitem_orders_formal_attrs,
    supplier_lineitem_formal_attrs.
  rewrite map_snd_select5, sel7_attrs_eq_supplier_formal,
    sel8_attrs_eq_lineitem_formal, sel9_attrs_eq_orders_formal.
  apply Fset.equal_refl.
Qed.

(** Join of projected table rows has S∥L labels (needed by sel6 ProjectExact). *)
Lemma q21_join_project_sel7_sel8_labels :
  forall vs vs',
    labels TNull
      (join_tuple TNull
         (project_row Queries.scalar_select_list_7 vs)
         (project_row Queries.scalar_select_list_8 vs')) =S=
    Fset.mk_set (A TNull) supplier_lineitem_formal_attrs.
Proof.
  intros vs vs'.
  unfold join_tuple.
  rewrite (Fset.equal_eq_1 _ _ _ _ (labels_mk_tuple _ _ _)).
  rewrite (Fset.equal_eq_1 _ _ _ _
    (Fset.union_eq _ _ _ _ _
       (q21_project_sel7_labels vs) (q21_project_sel8_labels vs'))).
  unfold supplier_lineitem_formal_attrs.
  pose proof (Fset.mk_set_app (A TNull) supplier_formal_attrs
                lineitem_formal_attrs) as Happ.
  rewrite Fset.equal_spec in Happ |- *; intro a; rewrite Happ; reflexivity.
Qed.

Lemma q21_Forall_join_project_sel7_sel8_labels :
  forall ss' ls',
    List.Forall
      (fun ft =>
         labels TNull ft =S=
           Fset.mk_set (A TNull) supplier_lineitem_formal_attrs)
      (brute_left_join_list (tuple TNull) (join_tuple TNull)
         (List.map (fun ft => project_row Queries.scalar_select_list_7
                                  (q21_sel7_values ft)) ss')
         (List.map (fun ft => project_row Queries.scalar_select_list_8
                                  (q21_sel8_values ft)) ls')).
Proof.
  intros ss' ls'.
  rewrite Forall_forall; intros ft Hin.
  unfold brute_left_join_list, theta_join_list, d_join_list in Hin.
  rewrite in_flat_map in Hin.
  destruct Hin as [ft1 [Hin1 Hin2]].
  rewrite in_map_iff in Hin2.
  destruct Hin2 as [ft2 [Heq Hin2]]; subst ft.
  rewrite filter_In in Hin2; destruct Hin2 as [Hin2 _].
  rewrite in_map_iff in Hin1.
  destruct Hin1 as [s [Heq1 Hins]]; subst ft1.
  rewrite in_map_iff in Hin2.
  destruct Hin2 as [l [Heq2 Hinl]]; subst ft2.
  apply q21_join_project_sel7_sel8_labels.
Qed.

(** Join of (S∥L projected) × orders projected has S∥L∥O labels. *)
Lemma q21_join_sl_project_sel9_labels :
  forall vs_sl vs_o,
    labels TNull
      (join_tuple TNull
         (project_row Queries.scalar_select_list_6 vs_sl)
         (project_row Queries.scalar_select_list_9 vs_o)) =S=
    Fset.mk_set (A TNull) supplier_lineitem_orders_formal_attrs.
Proof.
  intros vs_sl vs_o.
  unfold join_tuple.
  rewrite (Fset.equal_eq_1 _ _ _ _ (labels_mk_tuple _ _ _)).
  rewrite (Fset.equal_eq_1 _ _ _ _
    (Fset.union_eq _ _ _ _ _
       (q21_project_sel6_labels vs_sl) (q21_project_sel9_labels vs_o))).
  unfold supplier_lineitem_orders_formal_attrs.
  pose proof (Fset.mk_set_app (A TNull) supplier_lineitem_formal_attrs
                orders_formal_attrs) as Happ.
  rewrite Fset.equal_spec in Happ |- *; intro a; rewrite Happ; reflexivity.
Qed.

(** Composition checkpoint: A–D bag path (no FormalSQL_exec / Eval_Spec yet). *)
Lemma Q21_ordinary_bag_path_of_cross_product :
  forall (basesort : relname -> Fset.set (A TNull))
         (instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)))
         (boolean_schedule : boolean_site -> boolean_evaluation_order)
         fts_s fts_l fts_o fts_n rrs_s rrs_l rrs_o rrs_n
         nation_name (keep_f : tuple TNull -> bool),
    bag_correspond_unordered slot_map_supplier fts_s rrs_s ->
    bag_correspond_unordered slot_map_lineitem fts_l rrs_l ->
    bag_correspond_unordered slot_map_orders fts_o rrs_o ->
    bag_correspond_unordered slot_map_nation fts_n rrs_n ->
    q21_cross4_present_fresh fts_s fts_l fts_o fts_n ->
    NationNameOk nation_name ->
    (forall ft rr,
       row_correspond slot_map_outer_cor ft rr ->
       q21_outer_cor_labels_ok ft /\
       q21_outer_cor_text_canonical ft /\
       @scalar_expr_acceptance_exact_at TNull relname basesort instance unknown3
         NullValues.interp_scalar_operator_runtime_error
         NullValues.interp_aggregate_runtime_error
         tnull_value_is_null boolean_schedule
         (Env.env_t TNull nil ft)
         (ordinary_outer_conjlist nation_name) (keep_f ft)) ->
    bag_correspond_unordered slot_map_outer_cor
      (List.filter keep_f
         (List.map q21_project_select4
            (q21_cross4_tuples fts_s fts_l fts_o fts_n)))
      (List.filter (q21_ordinary_rel_keep nation_name)
         (q21_cross4_rows rrs_s rrs_l rrs_o rrs_n)).
Proof.
  intros basesort instance boolean_schedule
    fts_s fts_l fts_o fts_n rrs_s rrs_l rrs_o rrs_n
    nation_name keep_f Hs Hl Ho Hn Hpf Hnn Hrow.
  pose proof (Q21_four_way_cross_product_bag_correspond
    fts_s fts_l fts_o fts_n rrs_s rrs_l rrs_o rrs_n
    Hs Hl Ho Hn Hpf) as Hprod.
  pose proof (Q21_bag_correspond_select4_rename _ _ Hprod) as Hcor.
  eapply (Q21_ordinary_filter_preserves_bag_correspond_unordered
            basesort instance boolean_schedule);
    eassumption.
Qed.

(** * STEP E1 — FormalSQL spine Project/CrossJoin layers → concrete lists *)

Definition q21_map_project_sel7 (ss' : list (tuple TNull)) : list (tuple TNull) :=
  List.map (fun ft => project_row Queries.scalar_select_list_7
                           (q21_sel7_values ft)) ss'.

Definition q21_map_project_sel8 (ls' : list (tuple TNull)) : list (tuple TNull) :=
  List.map (fun ft => project_row Queries.scalar_select_list_8
                           (q21_sel8_values ft)) ls'.

Definition q21_map_project_sel9 (os' : list (tuple TNull)) : list (tuple TNull) :=
  List.map (fun ft => project_row Queries.scalar_select_list_9
                           (q21_sel9_values ft)) os'.

Definition q21_map_project_sel10 (ns' : list (tuple TNull)) : list (tuple TNull) :=
  List.map (fun ft => project_row Queries.scalar_select_list_10
                           (q21_sel10_values ft)) ns'.

(** Nested Project/Filter(True)/CrossJoin spine output (before outer select_4). *)
Definition q21_spine_cross4_tuples
    (ss' ls' os' ns' : list (tuple TNull)) : list (tuple TNull) :=
  brute_left_join_list (tuple TNull) (join_tuple TNull)
    (List.map (fun ft => project_row Queries.scalar_select_list_5
                             (q21_sel5_values ft))
       (brute_left_join_list (tuple TNull) (join_tuple TNull)
          (List.map (fun ft => project_row Queries.scalar_select_list_6
                                   (q21_sel6_values ft))
             (brute_left_join_list (tuple TNull) (join_tuple TNull)
                (q21_map_project_sel7 ss')
                (q21_map_project_sel8 ls')))
          (q21_map_project_sel9 os')))
    (q21_map_project_sel10 ns').

Lemma q21_Forall_map_project_sel7_labels :
  forall ss',
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs)
      ss' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs)
      (q21_map_project_sel7 ss').
Proof.
  intros ss' H; rewrite Forall_forall in H |- *; intros ft Hin.
  unfold q21_map_project_sel7 in Hin; rewrite in_map_iff in Hin.
  destruct Hin as [s [Heq _]]; subst ft; apply q21_project_sel7_labels.
Qed.

Lemma q21_Forall_map_project_sel8_labels :
  forall ls',
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs)
      ls' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs)
      (q21_map_project_sel8 ls').
Proof.
  intros ls' H; rewrite Forall_forall in H |- *; intros ft Hin.
  unfold q21_map_project_sel8 in Hin; rewrite in_map_iff in Hin.
  destruct Hin as [l [Heq _]]; subst ft; apply q21_project_sel8_labels.
Qed.

Lemma q21_Forall_map_project_sel9_labels :
  forall os',
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs)
      os' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs)
      (q21_map_project_sel9 os').
Proof.
  intros os' H; rewrite Forall_forall in H |- *; intros ft Hin.
  unfold q21_map_project_sel9 in Hin; rewrite in_map_iff in Hin.
  destruct Hin as [o [Heq _]]; subst ft; apply q21_project_sel9_labels.
Qed.

Lemma q21_Forall_map_project_sel10_labels :
  forall ns',
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) nation_formal_attrs)
      ns' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) nation_formal_attrs)
      (q21_map_project_sel10 ns').
Proof.
  intros ns' H; rewrite Forall_forall in H |- *; intros ft Hin.
  unfold q21_map_project_sel10 in Hin; rewrite in_map_iff in Hin.
  destruct Hin as [n [Heq _]]; subst ft; apply q21_project_sel10_labels.
Qed.

Lemma q21_project_supplier_table_success :
  forall ss' ls' os' ns',
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs)
      ss' ->
    @eval_query_expr_outcome TNull relname
      (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
      (@_instance TNull (D_of_embeds ss' ls' os' ns'))
      unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      q21_boolean_schedule nil
      (QExpr_Project Queries.scalar_select_list_7
         (@QExpr_Table TNull relname q21_supplier_table_outputs (Rel "supplier")))
      (SqlSuccess (q21_map_project_sel7 ss')).
Proof.
  intros ss' ls' os' ns' Hlab.
  eapply EQuery_ProjectRows.
  - apply EQuery_Table.
    apply q21_embed_table_same_rows_supplier.
  - unfold q21_map_project_sel7.
    apply q21_project_sel7_success; exact Hlab.
Qed.

Lemma q21_project_lineitem_table_success :
  forall ss' ls' os' ns',
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs)
      ls' ->
    @eval_query_expr_outcome TNull relname
      (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
      (@_instance TNull (D_of_embeds ss' ls' os' ns'))
      unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      q21_boolean_schedule nil
      (QExpr_Project Queries.scalar_select_list_8
         (@QExpr_Table TNull relname q21_lineitem_table_outputs (Rel "lineitem")))
      (SqlSuccess (q21_map_project_sel8 ls')).
Proof.
  intros ss' ls' os' ns' Hlab.
  eapply EQuery_ProjectRows.
  - apply EQuery_Table.
    apply q21_embed_table_same_rows_lineitem.
  - unfold q21_map_project_sel8.
    apply q21_project_sel8_success; exact Hlab.
Qed.

Lemma q21_project_orders_table_success :
  forall ss' ls' os' ns',
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs)
      os' ->
    @eval_query_expr_outcome TNull relname
      (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
      (@_instance TNull (D_of_embeds ss' ls' os' ns'))
      unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      q21_boolean_schedule nil
      (QExpr_Project Queries.scalar_select_list_9
         (@QExpr_Table TNull relname q21_orders_table_outputs (Rel "orders")))
      (SqlSuccess (q21_map_project_sel9 os')).
Proof.
  intros ss' ls' os' ns' Hlab.
  eapply EQuery_ProjectRows.
  - apply EQuery_Table.
    apply q21_embed_table_same_rows_orders.
  - unfold q21_map_project_sel9.
    apply q21_project_sel9_success; exact Hlab.
Qed.

Lemma q21_project_nation_table_success :
  forall ss' ls' os' ns',
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) nation_formal_attrs)
      ns' ->
    @eval_query_expr_outcome TNull relname
      (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
      (@_instance TNull (D_of_embeds ss' ls' os' ns'))
      unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      q21_boolean_schedule nil
      (QExpr_Project Queries.scalar_select_list_10
         (@QExpr_Table TNull relname q21_nation_table_outputs (Rel "nation")))
      (SqlSuccess (q21_map_project_sel10 ns')).
Proof.
  intros ss' ls' os' ns' Hlab.
  eapply EQuery_ProjectRows.
  - apply EQuery_Table.
    apply q21_embed_table_same_rows_nation.
  - unfold q21_map_project_sel10.
    apply q21_project_sel10_success; exact Hlab.
Qed.

(** S×L CrossJoin under Project(sel7/8), then Filter(True), then Project(sel6). *)
Lemma q21_spine_sl_project6_success :
  forall ss' ls' os' ns',
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs)
      ss' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs)
      ls' ->
    @eval_query_expr_outcome TNull relname
      (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
      (@_instance TNull (D_of_embeds ss' ls' os' ns'))
      unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      q21_boolean_schedule nil
      (QExpr_Project Queries.scalar_select_list_6
         (QExpr_Filter SExpr_True
            (QExpr_CrossJoin
               (QExpr_Project Queries.scalar_select_list_7
                  (@QExpr_Table TNull relname q21_supplier_table_outputs
                     (Rel "supplier")))
               (QExpr_Project Queries.scalar_select_list_8
                  (@QExpr_Table TNull relname q21_lineitem_table_outputs
                     (Rel "lineitem"))))))
      (SqlSuccess
         (List.map (fun ft => project_row Queries.scalar_select_list_6
                                  (q21_sel6_values ft))
            (brute_left_join_list (tuple TNull) (join_tuple TNull)
               (q21_map_project_sel7 ss')
               (q21_map_project_sel8 ls')))).
Proof.
  intros ss' ls' os' ns' Hs Hl.
  eapply EQuery_ProjectRows.
  - eapply q21_filter_true_success.
    eapply q21_cross_join_brute_success.
    + eapply q21_project_supplier_table_success; exact Hs.
    + eapply q21_project_lineitem_table_success; exact Hl.
  - apply q21_project_sel6_success.
    apply q21_Forall_join_project_sel7_sel8_labels.
Qed.

Lemma q21_Forall_join_sl6_project_sel9_labels :
  forall ss' ls' os',
    List.Forall
      (fun ft =>
         labels TNull ft =S=
           Fset.mk_set (A TNull) supplier_lineitem_orders_formal_attrs)
      (brute_left_join_list (tuple TNull) (join_tuple TNull)
         (List.map (fun ft => project_row Queries.scalar_select_list_6
                                  (q21_sel6_values ft))
            (brute_left_join_list (tuple TNull) (join_tuple TNull)
               (q21_map_project_sel7 ss')
               (q21_map_project_sel8 ls')))
         (q21_map_project_sel9 os')).
Proof.
  intros ss' ls' os'.
  rewrite Forall_forall; intros ft Hin.
  unfold brute_left_join_list, theta_join_list, d_join_list in Hin.
  rewrite in_flat_map in Hin.
  destruct Hin as [ft1 [Hin1 Hin2]].
  rewrite in_map_iff in Hin2.
  destruct Hin2 as [ft2 [Heq Hin2]]; subst ft.
  rewrite filter_In in Hin2; destruct Hin2 as [Hin2 _].
  rewrite in_map_iff in Hin1.
  destruct Hin1 as [sl [Heq1 _]]; subst ft1.
  unfold q21_map_project_sel9 in Hin2; rewrite in_map_iff in Hin2.
  destruct Hin2 as [o [Heq2 _]]; subst ft2.
  (* labels of project_row sel6 _ and project_row sel9 _ are fixed *)
  apply q21_join_sl_project_sel9_labels.
Qed.

(** (S∥L)×O under Project(sel6/9), Filter(True), Project(sel5). *)
Lemma q21_spine_slo_project5_success :
  forall ss' ls' os' ns',
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs)
      ss' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs)
      ls' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs)
      os' ->
    @eval_query_expr_outcome TNull relname
      (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
      (@_instance TNull (D_of_embeds ss' ls' os' ns'))
      unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      q21_boolean_schedule nil
      (QExpr_Project Queries.scalar_select_list_5
         (QExpr_Filter SExpr_True
            (QExpr_CrossJoin
               (QExpr_Project Queries.scalar_select_list_6
                  (QExpr_Filter SExpr_True
                     (QExpr_CrossJoin
                        (QExpr_Project Queries.scalar_select_list_7
                           (@QExpr_Table TNull relname q21_supplier_table_outputs
                              (Rel "supplier")))
                        (QExpr_Project Queries.scalar_select_list_8
                           (@QExpr_Table TNull relname q21_lineitem_table_outputs
                              (Rel "lineitem"))))))
               (QExpr_Project Queries.scalar_select_list_9
                  (@QExpr_Table TNull relname q21_orders_table_outputs
                     (Rel "orders"))))))
      (SqlSuccess
         (List.map (fun ft => project_row Queries.scalar_select_list_5
                                  (q21_sel5_values ft))
            (brute_left_join_list (tuple TNull) (join_tuple TNull)
               (List.map (fun ft => project_row Queries.scalar_select_list_6
                                        (q21_sel6_values ft))
                  (brute_left_join_list (tuple TNull) (join_tuple TNull)
                     (q21_map_project_sel7 ss')
                     (q21_map_project_sel8 ls')))
               (q21_map_project_sel9 os')))).
Proof.
  intros ss' ls' os' ns' Hs Hl Ho.
  eapply EQuery_ProjectRows.
  - eapply q21_filter_true_success.
    eapply q21_cross_join_brute_success.
    + eapply q21_spine_sl_project6_success; [exact Hs|exact Hl].
    + eapply q21_project_orders_table_success; exact Ho.
  - apply q21_project_sel5_success.
    apply q21_Forall_join_sl6_project_sel9_labels.
Qed.

(** Full extracted CrossJoin spine → [q21_spine_cross4_tuples]. *)
Lemma q21_spine_cross4_success :
  forall ss' ls' os' ns',
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs)
      ss' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs)
      ls' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs)
      os' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) nation_formal_attrs)
      ns' ->
    @eval_query_expr_outcome TNull relname
      (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
      (@_instance TNull (D_of_embeds ss' ls' os' ns'))
      unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      q21_boolean_schedule nil
      (QExpr_CrossJoin
         (QExpr_Project Queries.scalar_select_list_5
            (QExpr_Filter SExpr_True
               (QExpr_CrossJoin
                  (QExpr_Project Queries.scalar_select_list_6
                     (QExpr_Filter SExpr_True
                        (QExpr_CrossJoin
                           (QExpr_Project Queries.scalar_select_list_7
                              (@QExpr_Table TNull relname q21_supplier_table_outputs
                                 (Rel "supplier")))
                           (QExpr_Project Queries.scalar_select_list_8
                              (@QExpr_Table TNull relname q21_lineitem_table_outputs
                                 (Rel "lineitem"))))))
                  (QExpr_Project Queries.scalar_select_list_9
                     (@QExpr_Table TNull relname q21_orders_table_outputs
                        (Rel "orders"))))))
         (QExpr_Project Queries.scalar_select_list_10
            (@QExpr_Table TNull relname q21_nation_table_outputs
               (Rel "nation"))))
      (SqlSuccess (q21_spine_cross4_tuples ss' ls' os' ns')).
Proof.
  intros ss' ls' os' ns' Hs Hl Ho Hn.
  unfold q21_spine_cross4_tuples.
  eapply q21_cross_join_brute_success.
  - eapply q21_spine_slo_project5_success; [exact Hs|exact Hl|exact Ho].
  - eapply q21_project_nation_table_success; exact Hn.
Qed.

(** Remaining re-concretize: Dot identity + join transport → select4 congruence. *)

Ltac q21_discharge_project_identity_dot map_snd_lem attrs_def values_def :=
  intro ft;
  unfold project_row, scalar_select_outputs, values_def;
  rewrite map_snd_lem; unfold attrs_def;
  cbn [combine map snd];
  rewrite dot_mk_tuple;
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end;
  cbn; reflexivity.

(** * Project(sel7) Dot identity — all supplier attrs *)

Lemma q21_project_sel7_dot_s_suppkey :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_7 (q21_sel7_values ft))
      (AttrInt32 "s_suppkey") = dot TNull ft (AttrInt32 "s_suppkey").
Proof. q21_discharge_project_identity_dot map_snd_select7 q21_sel7_attrs q21_sel7_values. Qed.

Lemma q21_project_sel7_dot_s_name :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_7 (q21_sel7_values ft))
      (AttrString "s_name" (StringChar 25)) =
    dot TNull ft (AttrString "s_name" (StringChar 25)).
Proof. q21_discharge_project_identity_dot map_snd_select7 q21_sel7_attrs q21_sel7_values. Qed.

Lemma q21_project_sel7_dot_s_address :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_7 (q21_sel7_values ft))
      (AttrString "s_address" (StringVarcharN 40)) =
    dot TNull ft (AttrString "s_address" (StringVarcharN 40)).
Proof. q21_discharge_project_identity_dot map_snd_select7 q21_sel7_attrs q21_sel7_values. Qed.

Lemma q21_project_sel7_dot_s_nationkey :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_7 (q21_sel7_values ft))
      (AttrInt32 "s_nationkey") = dot TNull ft (AttrInt32 "s_nationkey").
Proof. q21_discharge_project_identity_dot map_snd_select7 q21_sel7_attrs q21_sel7_values. Qed.

Lemma q21_project_sel7_dot_s_phone :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_7 (q21_sel7_values ft))
      (AttrString "s_phone" (StringChar 15)) =
    dot TNull ft (AttrString "s_phone" (StringChar 15)).
Proof. q21_discharge_project_identity_dot map_snd_select7 q21_sel7_attrs q21_sel7_values. Qed.

Lemma q21_project_sel7_dot_s_acctbal :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_7 (q21_sel7_values ft))
      (AttrDecimal "s_acctbal" 15 2) =
    dot TNull ft (AttrDecimal "s_acctbal" 15 2).
Proof. q21_discharge_project_identity_dot map_snd_select7 q21_sel7_attrs q21_sel7_values. Qed.

Lemma q21_project_sel7_dot_s_comment :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_7 (q21_sel7_values ft))
      (AttrString "s_comment" (StringVarcharN 101)) =
    dot TNull ft (AttrString "s_comment" (StringVarcharN 101)).
Proof. q21_discharge_project_identity_dot map_snd_select7 q21_sel7_attrs q21_sel7_values. Qed.

Lemma q21_sel7_values_project_id :
  forall ft,
    q21_sel7_values
      (project_row Queries.scalar_select_list_7 (q21_sel7_values ft)) =
    q21_sel7_values ft.
Proof.
  intro ft; unfold q21_sel7_values at 1.
  rewrite q21_project_sel7_dot_s_suppkey, q21_project_sel7_dot_s_name,
    q21_project_sel7_dot_s_address, q21_project_sel7_dot_s_nationkey,
    q21_project_sel7_dot_s_phone, q21_project_sel7_dot_s_acctbal,
    q21_project_sel7_dot_s_comment.
  reflexivity.
Qed.

(** * Project(sel8) Dot identity — watched lineitem attrs for select_4 *)

Lemma q21_project_sel8_dot_l_orderkey :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_8 (q21_sel8_values ft))
      (AttrInt32 "l_orderkey") = dot TNull ft (AttrInt32 "l_orderkey").
Proof. q21_discharge_project_identity_dot map_snd_select8 q21_sel8_attrs q21_sel8_values. Qed.

Lemma q21_project_sel8_dot_l_partkey :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_8 (q21_sel8_values ft))
      (AttrInt32 "l_partkey") = dot TNull ft (AttrInt32 "l_partkey").
Proof. q21_discharge_project_identity_dot map_snd_select8 q21_sel8_attrs q21_sel8_values. Qed.

Lemma q21_project_sel8_dot_l_suppkey :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_8 (q21_sel8_values ft))
      (AttrInt32 "l_suppkey") = dot TNull ft (AttrInt32 "l_suppkey").
Proof. q21_discharge_project_identity_dot map_snd_select8 q21_sel8_attrs q21_sel8_values. Qed.

Lemma q21_project_sel8_dot_l_linenumber :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_8 (q21_sel8_values ft))
      (AttrInt32 "l_linenumber") = dot TNull ft (AttrInt32 "l_linenumber").
Proof. q21_discharge_project_identity_dot map_snd_select8 q21_sel8_attrs q21_sel8_values. Qed.

Lemma q21_project_sel8_dot_l_quantity :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_8 (q21_sel8_values ft))
      (AttrDecimal "l_quantity" 15 2) = dot TNull ft (AttrDecimal "l_quantity" 15 2).
Proof. q21_discharge_project_identity_dot map_snd_select8 q21_sel8_attrs q21_sel8_values. Qed.

Lemma q21_project_sel8_dot_l_extendedprice :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_8 (q21_sel8_values ft))
      (AttrDecimal "l_extendedprice" 15 2) =
    dot TNull ft (AttrDecimal "l_extendedprice" 15 2).
Proof. q21_discharge_project_identity_dot map_snd_select8 q21_sel8_attrs q21_sel8_values. Qed.

Lemma q21_project_sel8_dot_l_discount :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_8 (q21_sel8_values ft))
      (AttrDecimal "l_discount" 15 2) = dot TNull ft (AttrDecimal "l_discount" 15 2).
Proof. q21_discharge_project_identity_dot map_snd_select8 q21_sel8_attrs q21_sel8_values. Qed.

Lemma q21_project_sel8_dot_l_tax :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_8 (q21_sel8_values ft))
      (AttrDecimal "l_tax" 15 2) = dot TNull ft (AttrDecimal "l_tax" 15 2).
Proof. q21_discharge_project_identity_dot map_snd_select8 q21_sel8_attrs q21_sel8_values. Qed.

Lemma q21_project_sel8_dot_l_returnflag :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_8 (q21_sel8_values ft))
      (AttrString "l_returnflag" (StringChar 1)) =
    dot TNull ft (AttrString "l_returnflag" (StringChar 1)).
Proof. q21_discharge_project_identity_dot map_snd_select8 q21_sel8_attrs q21_sel8_values. Qed.

Lemma q21_project_sel8_dot_l_linestatus :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_8 (q21_sel8_values ft))
      (AttrString "l_linestatus" (StringChar 1)) =
    dot TNull ft (AttrString "l_linestatus" (StringChar 1)).
Proof. q21_discharge_project_identity_dot map_snd_select8 q21_sel8_attrs q21_sel8_values. Qed.

Lemma q21_project_sel8_dot_l_shipdate :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_8 (q21_sel8_values ft))
      (AttrDate "l_shipdate") = dot TNull ft (AttrDate "l_shipdate").
Proof. q21_discharge_project_identity_dot map_snd_select8 q21_sel8_attrs q21_sel8_values. Qed.

Lemma q21_project_sel8_dot_l_commitdate :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_8 (q21_sel8_values ft))
      (AttrDate "l_commitdate") = dot TNull ft (AttrDate "l_commitdate").
Proof. q21_discharge_project_identity_dot map_snd_select8 q21_sel8_attrs q21_sel8_values. Qed.

Lemma q21_project_sel8_dot_l_receiptdate :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_8 (q21_sel8_values ft))
      (AttrDate "l_receiptdate") = dot TNull ft (AttrDate "l_receiptdate").
Proof. q21_discharge_project_identity_dot map_snd_select8 q21_sel8_attrs q21_sel8_values. Qed.

Lemma q21_project_sel8_dot_l_shipinstruct :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_8 (q21_sel8_values ft))
      (AttrString "l_shipinstruct" (StringChar 25)) =
    dot TNull ft (AttrString "l_shipinstruct" (StringChar 25)).
Proof. q21_discharge_project_identity_dot map_snd_select8 q21_sel8_attrs q21_sel8_values. Qed.

Lemma q21_project_sel8_dot_l_shipmode :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_8 (q21_sel8_values ft))
      (AttrString "l_shipmode" (StringChar 10)) =
    dot TNull ft (AttrString "l_shipmode" (StringChar 10)).
Proof. q21_discharge_project_identity_dot map_snd_select8 q21_sel8_attrs q21_sel8_values. Qed.

Lemma q21_project_sel8_dot_l_comment :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_8 (q21_sel8_values ft))
      (AttrString "l_comment" (StringVarcharN 44)) =
    dot TNull ft (AttrString "l_comment" (StringVarcharN 44)).
Proof. q21_discharge_project_identity_dot map_snd_select8 q21_sel8_attrs q21_sel8_values. Qed.

Lemma q21_sel8_values_project_id :
  forall ft,
    q21_sel8_values
      (project_row Queries.scalar_select_list_8 (q21_sel8_values ft)) =
    q21_sel8_values ft.
Proof.
  intro ft; unfold q21_sel8_values at 1.
  rewrite q21_project_sel8_dot_l_orderkey, q21_project_sel8_dot_l_partkey,
    q21_project_sel8_dot_l_suppkey, q21_project_sel8_dot_l_linenumber,
    q21_project_sel8_dot_l_quantity, q21_project_sel8_dot_l_extendedprice,
    q21_project_sel8_dot_l_discount, q21_project_sel8_dot_l_tax,
    q21_project_sel8_dot_l_returnflag, q21_project_sel8_dot_l_linestatus,
    q21_project_sel8_dot_l_shipdate, q21_project_sel8_dot_l_commitdate,
    q21_project_sel8_dot_l_receiptdate, q21_project_sel8_dot_l_shipinstruct,
    q21_project_sel8_dot_l_shipmode, q21_project_sel8_dot_l_comment.
  reflexivity.
Qed.

(** * Project(sel9) Dot identity *)

Lemma q21_project_sel9_dot_o_orderkey :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_9 (q21_sel9_values ft))
      (AttrInt32 "o_orderkey") = dot TNull ft (AttrInt32 "o_orderkey").
Proof. q21_discharge_project_identity_dot map_snd_select9 q21_sel9_attrs q21_sel9_values. Qed.

Lemma q21_project_sel9_dot_o_custkey :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_9 (q21_sel9_values ft))
      (AttrInt32 "o_custkey") = dot TNull ft (AttrInt32 "o_custkey").
Proof. q21_discharge_project_identity_dot map_snd_select9 q21_sel9_attrs q21_sel9_values. Qed.

Lemma q21_project_sel9_dot_o_orderstatus :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_9 (q21_sel9_values ft))
      (AttrString "o_orderstatus" (StringChar 1)) =
    dot TNull ft (AttrString "o_orderstatus" (StringChar 1)).
Proof. q21_discharge_project_identity_dot map_snd_select9 q21_sel9_attrs q21_sel9_values. Qed.

Lemma q21_project_sel9_dot_o_totalprice :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_9 (q21_sel9_values ft))
      (AttrDecimal "o_totalprice" 15 2) =
    dot TNull ft (AttrDecimal "o_totalprice" 15 2).
Proof. q21_discharge_project_identity_dot map_snd_select9 q21_sel9_attrs q21_sel9_values. Qed.

Lemma q21_project_sel9_dot_o_orderdate :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_9 (q21_sel9_values ft))
      (AttrDate "o_orderdate") = dot TNull ft (AttrDate "o_orderdate").
Proof. q21_discharge_project_identity_dot map_snd_select9 q21_sel9_attrs q21_sel9_values. Qed.

Lemma q21_project_sel9_dot_o_orderpriority :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_9 (q21_sel9_values ft))
      (AttrString "o_orderpriority" (StringChar 15)) =
    dot TNull ft (AttrString "o_orderpriority" (StringChar 15)).
Proof. q21_discharge_project_identity_dot map_snd_select9 q21_sel9_attrs q21_sel9_values. Qed.

Lemma q21_project_sel9_dot_o_clerk :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_9 (q21_sel9_values ft))
      (AttrString "o_clerk" (StringChar 15)) =
    dot TNull ft (AttrString "o_clerk" (StringChar 15)).
Proof. q21_discharge_project_identity_dot map_snd_select9 q21_sel9_attrs q21_sel9_values. Qed.

Lemma q21_project_sel9_dot_o_shippriority :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_9 (q21_sel9_values ft))
      (AttrInt32 "o_shippriority") = dot TNull ft (AttrInt32 "o_shippriority").
Proof. q21_discharge_project_identity_dot map_snd_select9 q21_sel9_attrs q21_sel9_values. Qed.

Lemma q21_project_sel9_dot_o_comment :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_9 (q21_sel9_values ft))
      (AttrString "o_comment" (StringVarcharN 79)) =
    dot TNull ft (AttrString "o_comment" (StringVarcharN 79)).
Proof. q21_discharge_project_identity_dot map_snd_select9 q21_sel9_attrs q21_sel9_values. Qed.

Lemma q21_sel9_values_project_id :
  forall ft,
    q21_sel9_values
      (project_row Queries.scalar_select_list_9 (q21_sel9_values ft)) =
    q21_sel9_values ft.
Proof.
  intro ft; unfold q21_sel9_values at 1.
  rewrite q21_project_sel9_dot_o_orderkey, q21_project_sel9_dot_o_custkey,
    q21_project_sel9_dot_o_orderstatus, q21_project_sel9_dot_o_totalprice,
    q21_project_sel9_dot_o_orderdate, q21_project_sel9_dot_o_orderpriority,
    q21_project_sel9_dot_o_clerk, q21_project_sel9_dot_o_shippriority,
    q21_project_sel9_dot_o_comment.
  reflexivity.
Qed.

(** * Project(sel10) Dot identity *)

Lemma q21_project_sel10_dot_n_nationkey :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_10 (q21_sel10_values ft))
      (AttrInt32 "n_nationkey") = dot TNull ft (AttrInt32 "n_nationkey").
Proof. q21_discharge_project_identity_dot map_snd_select10 q21_sel10_attrs q21_sel10_values. Qed.

Lemma q21_project_sel10_dot_n_name :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_10 (q21_sel10_values ft))
      (AttrString "n_name" (StringChar 25)) =
    dot TNull ft (AttrString "n_name" (StringChar 25)).
Proof. q21_discharge_project_identity_dot map_snd_select10 q21_sel10_attrs q21_sel10_values. Qed.

Lemma q21_project_sel10_dot_n_regionkey :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_10 (q21_sel10_values ft))
      (AttrInt32 "n_regionkey") = dot TNull ft (AttrInt32 "n_regionkey").
Proof. q21_discharge_project_identity_dot map_snd_select10 q21_sel10_attrs q21_sel10_values. Qed.

Lemma q21_project_sel10_dot_n_comment :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_10 (q21_sel10_values ft))
      (AttrString "n_comment" (StringVarcharN 152)) =
    dot TNull ft (AttrString "n_comment" (StringVarcharN 152)).
Proof. q21_discharge_project_identity_dot map_snd_select10 q21_sel10_attrs q21_sel10_values. Qed.

Lemma q21_sel10_values_project_id :
  forall ft,
    q21_sel10_values
      (project_row Queries.scalar_select_list_10 (q21_sel10_values ft)) =
    q21_sel10_values ft.
Proof.
  intro ft; unfold q21_sel10_values at 1.
  rewrite q21_project_sel10_dot_n_nationkey, q21_project_sel10_dot_n_name,
    q21_project_sel10_dot_n_regionkey, q21_project_sel10_dot_n_comment.
  reflexivity.
Qed.

(** * Status placeholder — continuing with sel6/5 + join transport below *)

(** * Project(sel6) Dot identity — recovers S∥L values from joined tuple dots *)

Ltac q21_discharge_sel6_project_dot :=
  intro ft;
  unfold project_row, scalar_select_outputs, q21_sel6_values,
    q21_sel7_values, q21_sel8_values;
  rewrite map_snd_select6; unfold q21_sel7_attrs, q21_sel8_attrs;
  cbn [combine map snd List.app];
  rewrite dot_mk_tuple;
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end;
  cbn; reflexivity.

Lemma q21_project_sel6_dot_s_suppkey :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrInt32 "s_suppkey") = dot TNull ft (AttrInt32 "s_suppkey").
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_s_name :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrString "s_name" (StringChar 25)) =
    dot TNull ft (AttrString "s_name" (StringChar 25)).
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_s_address :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrString "s_address" (StringVarcharN 40)) =
    dot TNull ft (AttrString "s_address" (StringVarcharN 40)).
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_s_nationkey :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrInt32 "s_nationkey") = dot TNull ft (AttrInt32 "s_nationkey").
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_s_phone :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrString "s_phone" (StringChar 15)) =
    dot TNull ft (AttrString "s_phone" (StringChar 15)).
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_s_acctbal :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrDecimal "s_acctbal" 15 2) =
    dot TNull ft (AttrDecimal "s_acctbal" 15 2).
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_s_comment :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrString "s_comment" (StringVarcharN 101)) =
    dot TNull ft (AttrString "s_comment" (StringVarcharN 101)).
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_l_orderkey :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrInt32 "l_orderkey") = dot TNull ft (AttrInt32 "l_orderkey").
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_l_partkey :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrInt32 "l_partkey") = dot TNull ft (AttrInt32 "l_partkey").
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_l_suppkey :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrInt32 "l_suppkey") = dot TNull ft (AttrInt32 "l_suppkey").
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_l_linenumber :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrInt32 "l_linenumber") = dot TNull ft (AttrInt32 "l_linenumber").
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_l_quantity :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrDecimal "l_quantity" 15 2) = dot TNull ft (AttrDecimal "l_quantity" 15 2).
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_l_extendedprice :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrDecimal "l_extendedprice" 15 2) =
    dot TNull ft (AttrDecimal "l_extendedprice" 15 2).
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_l_discount :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrDecimal "l_discount" 15 2) = dot TNull ft (AttrDecimal "l_discount" 15 2).
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_l_tax :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrDecimal "l_tax" 15 2) = dot TNull ft (AttrDecimal "l_tax" 15 2).
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_l_returnflag :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrString "l_returnflag" (StringChar 1)) =
    dot TNull ft (AttrString "l_returnflag" (StringChar 1)).
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_l_linestatus :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrString "l_linestatus" (StringChar 1)) =
    dot TNull ft (AttrString "l_linestatus" (StringChar 1)).
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_l_shipdate :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrDate "l_shipdate") = dot TNull ft (AttrDate "l_shipdate").
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_l_commitdate :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrDate "l_commitdate") = dot TNull ft (AttrDate "l_commitdate").
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_l_receiptdate :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrDate "l_receiptdate") = dot TNull ft (AttrDate "l_receiptdate").
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_l_shipinstruct :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrString "l_shipinstruct" (StringChar 25)) =
    dot TNull ft (AttrString "l_shipinstruct" (StringChar 25)).
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_l_shipmode :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrString "l_shipmode" (StringChar 10)) =
    dot TNull ft (AttrString "l_shipmode" (StringChar 10)).
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_project_sel6_dot_l_comment :
  forall ft,
    dot TNull (project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
      (AttrString "l_comment" (StringVarcharN 44)) =
    dot TNull ft (AttrString "l_comment" (StringVarcharN 44)).
Proof. q21_discharge_sel6_project_dot. Qed.

Lemma q21_sel6_values_project_id :
  forall ft,
    q21_sel6_values
      (project_row Queries.scalar_select_list_6 (q21_sel6_values ft)) =
    q21_sel6_values ft.
Proof.
  intro ft; unfold q21_sel6_values at 1; unfold q21_sel7_values at 1;
    unfold q21_sel8_values at 1.
  rewrite q21_project_sel6_dot_s_suppkey, q21_project_sel6_dot_s_name,
    q21_project_sel6_dot_s_address, q21_project_sel6_dot_s_nationkey,
    q21_project_sel6_dot_s_phone, q21_project_sel6_dot_s_acctbal,
    q21_project_sel6_dot_s_comment,
    q21_project_sel6_dot_l_orderkey, q21_project_sel6_dot_l_partkey,
    q21_project_sel6_dot_l_suppkey, q21_project_sel6_dot_l_linenumber,
    q21_project_sel6_dot_l_quantity, q21_project_sel6_dot_l_extendedprice,
    q21_project_sel6_dot_l_discount, q21_project_sel6_dot_l_tax,
    q21_project_sel6_dot_l_returnflag, q21_project_sel6_dot_l_linestatus,
    q21_project_sel6_dot_l_shipdate, q21_project_sel6_dot_l_commitdate,
    q21_project_sel6_dot_l_receiptdate, q21_project_sel6_dot_l_shipinstruct,
    q21_project_sel6_dot_l_shipmode, q21_project_sel6_dot_l_comment.
  reflexivity.
Qed.

(** * Project(sel5) Dot identity — recovers S∥L∥O values *)

Ltac q21_discharge_sel5_project_dot :=
  intro ft;
  unfold project_row, scalar_select_outputs, q21_sel5_values, q21_sel6_values,
    q21_sel7_values, q21_sel8_values, q21_sel9_values;
  rewrite map_snd_select5; unfold q21_sel7_attrs, q21_sel8_attrs, q21_sel9_attrs;
  cbn [combine map snd List.app];
  rewrite dot_mk_tuple;
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end;
  cbn; reflexivity.

Lemma q21_project_sel5_dot_s_suppkey : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrInt32 "s_suppkey") = dot TNull ft (AttrInt32 "s_suppkey").
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_s_name : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrString "s_name" (StringChar 25)) = dot TNull ft (AttrString "s_name" (StringChar 25)).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_s_address : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrString "s_address" (StringVarcharN 40)) = dot TNull ft (AttrString "s_address" (StringVarcharN 40)).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_s_nationkey : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrInt32 "s_nationkey") = dot TNull ft (AttrInt32 "s_nationkey").
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_s_phone : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrString "s_phone" (StringChar 15)) = dot TNull ft (AttrString "s_phone" (StringChar 15)).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_s_acctbal : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrDecimal "s_acctbal" 15 2) = dot TNull ft (AttrDecimal "s_acctbal" 15 2).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_s_comment : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrString "s_comment" (StringVarcharN 101)) = dot TNull ft (AttrString "s_comment" (StringVarcharN 101)).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_l_orderkey : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrInt32 "l_orderkey") = dot TNull ft (AttrInt32 "l_orderkey").
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_l_partkey : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrInt32 "l_partkey") = dot TNull ft (AttrInt32 "l_partkey").
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_l_suppkey : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrInt32 "l_suppkey") = dot TNull ft (AttrInt32 "l_suppkey").
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_l_linenumber : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrInt32 "l_linenumber") = dot TNull ft (AttrInt32 "l_linenumber").
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_l_quantity : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrDecimal "l_quantity" 15 2) = dot TNull ft (AttrDecimal "l_quantity" 15 2).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_l_extendedprice : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrDecimal "l_extendedprice" 15 2) = dot TNull ft (AttrDecimal "l_extendedprice" 15 2).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_l_discount : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrDecimal "l_discount" 15 2) = dot TNull ft (AttrDecimal "l_discount" 15 2).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_l_tax : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrDecimal "l_tax" 15 2) = dot TNull ft (AttrDecimal "l_tax" 15 2).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_l_returnflag : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrString "l_returnflag" (StringChar 1)) = dot TNull ft (AttrString "l_returnflag" (StringChar 1)).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_l_linestatus : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrString "l_linestatus" (StringChar 1)) = dot TNull ft (AttrString "l_linestatus" (StringChar 1)).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_l_shipdate : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrDate "l_shipdate") = dot TNull ft (AttrDate "l_shipdate").
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_l_commitdate : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrDate "l_commitdate") = dot TNull ft (AttrDate "l_commitdate").
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_l_receiptdate : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrDate "l_receiptdate") = dot TNull ft (AttrDate "l_receiptdate").
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_l_shipinstruct : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrString "l_shipinstruct" (StringChar 25)) = dot TNull ft (AttrString "l_shipinstruct" (StringChar 25)).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_l_shipmode : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrString "l_shipmode" (StringChar 10)) = dot TNull ft (AttrString "l_shipmode" (StringChar 10)).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_l_comment : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrString "l_comment" (StringVarcharN 44)) = dot TNull ft (AttrString "l_comment" (StringVarcharN 44)).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_o_orderkey : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrInt32 "o_orderkey") = dot TNull ft (AttrInt32 "o_orderkey").
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_o_custkey : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrInt32 "o_custkey") = dot TNull ft (AttrInt32 "o_custkey").
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_o_orderstatus : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrString "o_orderstatus" (StringChar 1)) = dot TNull ft (AttrString "o_orderstatus" (StringChar 1)).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_o_totalprice : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrDecimal "o_totalprice" 15 2) = dot TNull ft (AttrDecimal "o_totalprice" 15 2).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_o_orderdate : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrDate "o_orderdate") = dot TNull ft (AttrDate "o_orderdate").
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_o_orderpriority : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrString "o_orderpriority" (StringChar 15)) = dot TNull ft (AttrString "o_orderpriority" (StringChar 15)).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_o_clerk : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrString "o_clerk" (StringChar 15)) = dot TNull ft (AttrString "o_clerk" (StringChar 15)).
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_o_shippriority : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrInt32 "o_shippriority") = dot TNull ft (AttrInt32 "o_shippriority").
Proof. q21_discharge_sel5_project_dot. Qed.
Lemma q21_project_sel5_dot_o_comment : forall ft, dot TNull (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) (AttrString "o_comment" (StringVarcharN 79)) = dot TNull ft (AttrString "o_comment" (StringVarcharN 79)).
Proof. q21_discharge_sel5_project_dot. Qed.

Lemma q21_sel5_values_project_id :
  forall ft,
    q21_sel5_values
      (project_row Queries.scalar_select_list_5 (q21_sel5_values ft)) =
    q21_sel5_values ft.
Proof.
  intro ft; unfold q21_sel5_values at 1; unfold q21_sel6_values at 1;
    unfold q21_sel7_values at 1; unfold q21_sel8_values at 1;
    unfold q21_sel9_values at 1.
  rewrite
    q21_project_sel5_dot_s_suppkey, q21_project_sel5_dot_s_name,
    q21_project_sel5_dot_s_address, q21_project_sel5_dot_s_nationkey,
    q21_project_sel5_dot_s_phone, q21_project_sel5_dot_s_acctbal,
    q21_project_sel5_dot_s_comment,
    q21_project_sel5_dot_l_orderkey, q21_project_sel5_dot_l_partkey,
    q21_project_sel5_dot_l_suppkey, q21_project_sel5_dot_l_linenumber,
    q21_project_sel5_dot_l_quantity, q21_project_sel5_dot_l_extendedprice,
    q21_project_sel5_dot_l_discount, q21_project_sel5_dot_l_tax,
    q21_project_sel5_dot_l_returnflag, q21_project_sel5_dot_l_linestatus,
    q21_project_sel5_dot_l_shipdate, q21_project_sel5_dot_l_commitdate,
    q21_project_sel5_dot_l_receiptdate, q21_project_sel5_dot_l_shipinstruct,
    q21_project_sel5_dot_l_shipmode, q21_project_sel5_dot_l_comment,
    q21_project_sel5_dot_o_orderkey, q21_project_sel5_dot_o_custkey,
    q21_project_sel5_dot_o_orderstatus, q21_project_sel5_dot_o_totalprice,
    q21_project_sel5_dot_o_orderdate, q21_project_sel5_dot_o_orderpriority,
    q21_project_sel5_dot_o_clerk, q21_project_sel5_dot_o_shippriority,
    q21_project_sel5_dot_o_comment.
  reflexivity.
Qed.

(** * select_4 value list = sel5 ++ sel10 *)

Lemma q21_select4_dot_values_as_sel5_sel10 :
  forall ft,
    q21_select4_dot_values ft =
    List.app (q21_sel5_values ft) (q21_sel10_values ft).
Proof.
  intro ft.
  unfold q21_select4_dot_values, q21_sel5_values, q21_sel6_values,
    q21_sel7_values, q21_sel8_values, q21_sel9_values, q21_sel10_values.
  reflexivity.
Qed.

(** * Attr membership helpers for join transport *)

Ltac q21_attr_not_in_formal attrs :=
  change ((?a inS? Fset.mk_set (A TNull) attrs) = false);
  vm_compute; reflexivity.

Lemma q21_l_attrs_not_in_supplier :
  (AttrInt32 "l_orderkey" inS? Fset.mk_set (A TNull) supplier_formal_attrs) = false /\
  (AttrInt32 "l_partkey" inS? Fset.mk_set (A TNull) supplier_formal_attrs) = false /\
  (AttrInt32 "l_suppkey" inS? Fset.mk_set (A TNull) supplier_formal_attrs) = false /\
  (AttrInt32 "l_linenumber" inS? Fset.mk_set (A TNull) supplier_formal_attrs) = false /\
  (AttrDecimal "l_quantity" 15 2 inS? Fset.mk_set (A TNull) supplier_formal_attrs) = false /\
  (AttrDecimal "l_extendedprice" 15 2 inS? Fset.mk_set (A TNull) supplier_formal_attrs) = false /\
  (AttrDecimal "l_discount" 15 2 inS? Fset.mk_set (A TNull) supplier_formal_attrs) = false /\
  (AttrDecimal "l_tax" 15 2 inS? Fset.mk_set (A TNull) supplier_formal_attrs) = false /\
  (AttrString "l_returnflag" (StringChar 1) inS? Fset.mk_set (A TNull) supplier_formal_attrs) = false /\
  (AttrString "l_linestatus" (StringChar 1) inS? Fset.mk_set (A TNull) supplier_formal_attrs) = false /\
  (AttrDate "l_shipdate" inS? Fset.mk_set (A TNull) supplier_formal_attrs) = false /\
  (AttrDate "l_commitdate" inS? Fset.mk_set (A TNull) supplier_formal_attrs) = false /\
  (AttrDate "l_receiptdate" inS? Fset.mk_set (A TNull) supplier_formal_attrs) = false /\
  (AttrString "l_shipinstruct" (StringChar 25) inS? Fset.mk_set (A TNull) supplier_formal_attrs) = false /\
  (AttrString "l_shipmode" (StringChar 10) inS? Fset.mk_set (A TNull) supplier_formal_attrs) = false /\
  (AttrString "l_comment" (StringVarcharN 44) inS? Fset.mk_set (A TNull) supplier_formal_attrs) = false.
Proof. repeat split; vm_compute; reflexivity. Qed.

Lemma q21_o_attrs_not_in_sl :
  (AttrInt32 "o_orderkey" inS? Fset.mk_set (A TNull) supplier_lineitem_formal_attrs) = false /\
  (AttrInt32 "o_custkey" inS? Fset.mk_set (A TNull) supplier_lineitem_formal_attrs) = false /\
  (AttrString "o_orderstatus" (StringChar 1) inS? Fset.mk_set (A TNull) supplier_lineitem_formal_attrs) = false /\
  (AttrDecimal "o_totalprice" 15 2 inS? Fset.mk_set (A TNull) supplier_lineitem_formal_attrs) = false /\
  (AttrDate "o_orderdate" inS? Fset.mk_set (A TNull) supplier_lineitem_formal_attrs) = false /\
  (AttrString "o_orderpriority" (StringChar 15) inS? Fset.mk_set (A TNull) supplier_lineitem_formal_attrs) = false /\
  (AttrString "o_clerk" (StringChar 15) inS? Fset.mk_set (A TNull) supplier_lineitem_formal_attrs) = false /\
  (AttrInt32 "o_shippriority" inS? Fset.mk_set (A TNull) supplier_lineitem_formal_attrs) = false /\
  (AttrString "o_comment" (StringVarcharN 79) inS? Fset.mk_set (A TNull) supplier_lineitem_formal_attrs) = false.
Proof. repeat split; vm_compute; reflexivity. Qed.

Lemma q21_n_attrs_not_in_slo :
  (AttrInt32 "n_nationkey" inS? Fset.mk_set (A TNull) supplier_lineitem_orders_formal_attrs) = false /\
  (AttrString "n_name" (StringChar 25) inS? Fset.mk_set (A TNull) supplier_lineitem_orders_formal_attrs) = false /\
  (AttrInt32 "n_regionkey" inS? Fset.mk_set (A TNull) supplier_lineitem_orders_formal_attrs) = false /\
  (AttrString "n_comment" (StringVarcharN 152) inS? Fset.mk_set (A TNull) supplier_lineitem_orders_formal_attrs) = false.
Proof. repeat split; vm_compute; reflexivity. Qed.

(** * join_tuple transport of watched value lists *)

Lemma q21_sel7_values_join_left :
  forall t1 t2,
    labels TNull t1 =S= Fset.mk_set (A TNull) supplier_formal_attrs ->
    q21_sel7_values (join_tuple TNull t1 t2) = q21_sel7_values t1.
Proof.
  intros t1 t2 Hl; unfold q21_sel7_values.
  assert (Ha0 : AttrInt32 "s_suppkey" inS labels TNull t1)
    by q21_attr_inS_of_labels Hl.
  assert (Ha1 : AttrString "s_name" (StringChar 25) inS labels TNull t1)
    by q21_attr_inS_of_labels Hl.
  assert (Ha2 : AttrString "s_address" (StringVarcharN 40) inS labels TNull t1)
    by q21_attr_inS_of_labels Hl.
  assert (Ha3 : AttrInt32 "s_nationkey" inS labels TNull t1)
    by q21_attr_inS_of_labels Hl.
  assert (Ha4 : AttrString "s_phone" (StringChar 15) inS labels TNull t1)
    by q21_attr_inS_of_labels Hl.
  assert (Ha5 : AttrDecimal "s_acctbal" 15 2 inS labels TNull t1)
    by q21_attr_inS_of_labels Hl.
  assert (Ha6 : AttrString "s_comment" (StringVarcharN 101) inS labels TNull t1)
    by q21_attr_inS_of_labels Hl.
  rewrite !(dot_join_tuple_1 TNull t1 t2); [reflexivity|..]; assumption.
Qed.

Lemma q21_sel8_values_join_right :
  forall t1 t2,
    labels TNull t1 =S= Fset.mk_set (A TNull) supplier_formal_attrs ->
    labels TNull t2 =S= Fset.mk_set (A TNull) lineitem_formal_attrs ->
    q21_sel8_values (join_tuple TNull t1 t2) = q21_sel8_values t2.
Proof.
  intros t1 t2 Hl1 Hl2; unfold q21_sel8_values.
  pose proof q21_l_attrs_not_in_supplier as Hnin.
  repeat match goal with
         | H : _ /\ _ |- _ => destruct H
         end.
  assert (Hb0 : (AttrInt32 "l_orderkey" inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb1 : (AttrInt32 "l_partkey" inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb2 : (AttrInt32 "l_suppkey" inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb3 : (AttrInt32 "l_linenumber" inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb4 : (AttrDecimal "l_quantity" 15 2 inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb5 : (AttrDecimal "l_extendedprice" 15 2 inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb6 : (AttrDecimal "l_discount" 15 2 inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb7 : (AttrDecimal "l_tax" 15 2 inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb8 : (AttrString "l_returnflag" (StringChar 1) inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb9 : (AttrString "l_linestatus" (StringChar 1) inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb10 : (AttrDate "l_shipdate" inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb11 : (AttrDate "l_commitdate" inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb12 : (AttrDate "l_receiptdate" inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb13 : (AttrString "l_shipinstruct" (StringChar 25) inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb14 : (AttrString "l_shipmode" (StringChar 10) inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb15 : (AttrString "l_comment" (StringVarcharN 44) inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Ha0 : AttrInt32 "l_orderkey" inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha1 : AttrInt32 "l_partkey" inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha2 : AttrInt32 "l_suppkey" inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha3 : AttrInt32 "l_linenumber" inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha4 : AttrDecimal "l_quantity" 15 2 inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha5 : AttrDecimal "l_extendedprice" 15 2 inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha6 : AttrDecimal "l_discount" 15 2 inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha7 : AttrDecimal "l_tax" 15 2 inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha8 : AttrString "l_returnflag" (StringChar 1) inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha9 : AttrString "l_linestatus" (StringChar 1) inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha10 : AttrDate "l_shipdate" inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha11 : AttrDate "l_commitdate" inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha12 : AttrDate "l_receiptdate" inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha13 : AttrString "l_shipinstruct" (StringChar 25) inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha14 : AttrString "l_shipmode" (StringChar 10) inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha15 : AttrString "l_comment" (StringVarcharN 44) inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  rewrite !(dot_join_tuple_2 TNull t1 t2); [reflexivity|..]; assumption.
Qed.

Lemma q21_sel9_values_join_right :
  forall t1 t2,
    labels TNull t1 =S= Fset.mk_set (A TNull) supplier_lineitem_formal_attrs ->
    labels TNull t2 =S= Fset.mk_set (A TNull) orders_formal_attrs ->
    q21_sel9_values (join_tuple TNull t1 t2) = q21_sel9_values t2.
Proof.
  intros t1 t2 Hl1 Hl2; unfold q21_sel9_values.
  pose proof q21_o_attrs_not_in_sl as Hnin.
  repeat match goal with
         | H : _ /\ _ |- _ => destruct H
         end.
  assert (Hb0 : (AttrInt32 "o_orderkey" inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb1 : (AttrInt32 "o_custkey" inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb2 : (AttrString "o_orderstatus" (StringChar 1) inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb3 : (AttrDecimal "o_totalprice" 15 2 inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb4 : (AttrDate "o_orderdate" inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb5 : (AttrString "o_orderpriority" (StringChar 15) inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb6 : (AttrString "o_clerk" (StringChar 15) inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb7 : (AttrInt32 "o_shippriority" inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Hb8 : (AttrString "o_comment" (StringVarcharN 79) inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); assumption).
  assert (Ha0 : AttrInt32 "o_orderkey" inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha1 : AttrInt32 "o_custkey" inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha2 : AttrString "o_orderstatus" (StringChar 1) inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha3 : AttrDecimal "o_totalprice" 15 2 inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha4 : AttrDate "o_orderdate" inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha5 : AttrString "o_orderpriority" (StringChar 15) inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha6 : AttrString "o_clerk" (StringChar 15) inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha7 : AttrInt32 "o_shippriority" inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha8 : AttrString "o_comment" (StringVarcharN 79) inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  rewrite !(dot_join_tuple_2 TNull t1 t2); [reflexivity|..]; assumption.
Qed.

Lemma q21_sel6_values_join_left :
  forall t1 t2,
    labels TNull t1 =S= Fset.mk_set (A TNull) supplier_lineitem_formal_attrs ->
    q21_sel6_values (join_tuple TNull t1 t2) = q21_sel6_values t1.
Proof.
  intros t1 t2 Hl; unfold q21_sel6_values, q21_sel7_values, q21_sel8_values.
  (* all 23 attrs in t1 *)
  assert (Ha00 : AttrInt32 "s_suppkey" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha01 : AttrString "s_name" (StringChar 25) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha02 : AttrString "s_address" (StringVarcharN 40) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha03 : AttrInt32 "s_nationkey" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha04 : AttrString "s_phone" (StringChar 15) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha05 : AttrDecimal "s_acctbal" 15 2 inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha06 : AttrString "s_comment" (StringVarcharN 101) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha07 : AttrInt32 "l_orderkey" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha08 : AttrInt32 "l_partkey" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha09 : AttrInt32 "l_suppkey" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha10 : AttrInt32 "l_linenumber" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha11 : AttrDecimal "l_quantity" 15 2 inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha12 : AttrDecimal "l_extendedprice" 15 2 inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha13 : AttrDecimal "l_discount" 15 2 inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha14 : AttrDecimal "l_tax" 15 2 inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha15 : AttrString "l_returnflag" (StringChar 1) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha16 : AttrString "l_linestatus" (StringChar 1) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha17 : AttrDate "l_shipdate" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha18 : AttrDate "l_commitdate" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha19 : AttrDate "l_receiptdate" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha20 : AttrString "l_shipinstruct" (StringChar 25) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha21 : AttrString "l_shipmode" (StringChar 10) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha22 : AttrString "l_comment" (StringVarcharN 44) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  rewrite !(dot_join_tuple_1 TNull t1 t2); [reflexivity|..]; assumption.
Qed.

Lemma q21_sel5_values_join_left :
  forall t1 t2,
    labels TNull t1 =S= Fset.mk_set (A TNull) supplier_lineitem_orders_formal_attrs ->
    q21_sel5_values (join_tuple TNull t1 t2) = q21_sel5_values t1.
Proof.
  intros t1 t2 Hl.
  unfold q21_sel5_values, q21_sel6_values, q21_sel7_values, q21_sel8_values, q21_sel9_values.
  assert (Ha00 : AttrInt32 "s_suppkey" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha01 : AttrString "s_name" (StringChar 25) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha02 : AttrString "s_address" (StringVarcharN 40) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha03 : AttrInt32 "s_nationkey" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha04 : AttrString "s_phone" (StringChar 15) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha05 : AttrDecimal "s_acctbal" 15 2 inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha06 : AttrString "s_comment" (StringVarcharN 101) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha07 : AttrInt32 "l_orderkey" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha08 : AttrInt32 "l_partkey" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha09 : AttrInt32 "l_suppkey" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha10 : AttrInt32 "l_linenumber" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha11 : AttrDecimal "l_quantity" 15 2 inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha12 : AttrDecimal "l_extendedprice" 15 2 inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha13 : AttrDecimal "l_discount" 15 2 inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha14 : AttrDecimal "l_tax" 15 2 inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha15 : AttrString "l_returnflag" (StringChar 1) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha16 : AttrString "l_linestatus" (StringChar 1) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha17 : AttrDate "l_shipdate" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha18 : AttrDate "l_commitdate" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha19 : AttrDate "l_receiptdate" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha20 : AttrString "l_shipinstruct" (StringChar 25) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha21 : AttrString "l_shipmode" (StringChar 10) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha22 : AttrString "l_comment" (StringVarcharN 44) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha23 : AttrInt32 "o_orderkey" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha24 : AttrInt32 "o_custkey" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha25 : AttrString "o_orderstatus" (StringChar 1) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha26 : AttrDecimal "o_totalprice" 15 2 inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha27 : AttrDate "o_orderdate" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha28 : AttrString "o_orderpriority" (StringChar 15) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha29 : AttrString "o_clerk" (StringChar 15) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha30 : AttrInt32 "o_shippriority" inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  assert (Ha31 : AttrString "o_comment" (StringVarcharN 79) inS labels TNull t1) by q21_attr_inS_of_labels Hl.
  rewrite !(dot_join_tuple_1 TNull t1 t2); [reflexivity|..]; assumption.
Qed.

Lemma q21_sel10_values_join_right :
  forall t1 t2,
    labels TNull t1 =S= Fset.mk_set (A TNull) supplier_lineitem_orders_formal_attrs ->
    labels TNull t2 =S= Fset.mk_set (A TNull) nation_formal_attrs ->
    q21_sel10_values (join_tuple TNull t1 t2) = q21_sel10_values t2.
Proof.
  intros t1 t2 Hl1 Hl2; unfold q21_sel10_values.
  pose proof q21_n_attrs_not_in_slo as Hnin.
  destruct Hnin as [Hn0 [Hn1 [Hn2 Hn3]]].
  assert (Hb0 : (AttrInt32 "n_nationkey" inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); exact Hn0).
  assert (Hb1 : (AttrString "n_name" (StringChar 25) inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); exact Hn1).
  assert (Hb2 : (AttrInt32 "n_regionkey" inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); exact Hn2).
  assert (Hb3 : (AttrString "n_comment" (StringVarcharN 152) inS? labels TNull t1) = false)
    by (rewrite (Fset.mem_eq_2 _ _ _ Hl1); exact Hn3).
  assert (Ha0 : AttrInt32 "n_nationkey" inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha1 : AttrString "n_name" (StringChar 25) inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha2 : AttrInt32 "n_regionkey" inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  assert (Ha3 : AttrString "n_comment" (StringVarcharN 152) inS labels TNull t2)
    by q21_attr_inS_of_labels Hl2.
  rewrite !(dot_join_tuple_2 TNull t1 t2); [reflexivity|..]; assumption.
Qed.

(** * One-tuple select4 congruence through the projected spine *)

Definition q21_spine_one_tuple
    (s l o n : tuple TNull) : tuple TNull :=
  join_tuple TNull
    (project_row Queries.scalar_select_list_5
       (q21_sel5_values
          (join_tuple TNull
             (project_row Queries.scalar_select_list_6
                (q21_sel6_values
                   (join_tuple TNull
                      (project_row Queries.scalar_select_list_7
                         (q21_sel7_values s))
                      (project_row Queries.scalar_select_list_8
                         (q21_sel8_values l)))))
             (project_row Queries.scalar_select_list_9
                (q21_sel9_values o)))))
    (project_row Queries.scalar_select_list_10
       (q21_sel10_values n)).

Lemma q21_select4_dots_spine_one :
  forall s l o n,
    labels TNull s =S= Fset.mk_set (A TNull) supplier_formal_attrs ->
    labels TNull l =S= Fset.mk_set (A TNull) lineitem_formal_attrs ->
    labels TNull o =S= Fset.mk_set (A TNull) orders_formal_attrs ->
    labels TNull n =S= Fset.mk_set (A TNull) nation_formal_attrs ->
    q21_select4_dot_values (q21_spine_one_tuple s l o n) =
    q21_select4_dot_values
      (join_tuple TNull
         (join_tuple TNull (join_tuple TNull s l) o) n).
Proof.
  intros s l o n Hs Hl Ho Hn.
  unfold q21_spine_one_tuple.
  rewrite !q21_select4_dot_values_as_sel5_sel10.
  (* Outer join: sel5 from left project_sel5, sel10 from right project_sel10 *)
  set (ps := project_row Queries.scalar_select_list_7 (q21_sel7_values s)).
  set (pl := project_row Queries.scalar_select_list_8 (q21_sel8_values l)).
  set (po := project_row Queries.scalar_select_list_9 (q21_sel9_values o)).
  set (pn := project_row Queries.scalar_select_list_10 (q21_sel10_values n)).
  set (jsl := join_tuple TNull ps pl).
  set (psl := project_row Queries.scalar_select_list_6 (q21_sel6_values jsl)).
  set (jslo := join_tuple TNull psl po).
  set (pslo := project_row Queries.scalar_select_list_5 (q21_sel5_values jslo)).
  change (join_tuple TNull
            (project_row Queries.scalar_select_list_5
               (q21_sel5_values
                  (join_tuple TNull
                     (project_row Queries.scalar_select_list_6
                        (q21_sel6_values
                           (join_tuple TNull
                              (project_row Queries.scalar_select_list_7
                                 (q21_sel7_values s))
                              (project_row Queries.scalar_select_list_8
                                 (q21_sel8_values l)))))
                     (project_row Queries.scalar_select_list_9
                        (q21_sel9_values o)))))
            (project_row Queries.scalar_select_list_10
               (q21_sel10_values n)))
    with (join_tuple TNull pslo pn).
  assert (Hps : labels TNull ps =S= Fset.mk_set (A TNull) supplier_formal_attrs)
    by (subst ps; apply q21_project_sel7_labels).
  assert (Hpl : labels TNull pl =S= Fset.mk_set (A TNull) lineitem_formal_attrs)
    by (subst pl; apply q21_project_sel8_labels).
  assert (Hpo : labels TNull po =S= Fset.mk_set (A TNull) orders_formal_attrs)
    by (subst po; apply q21_project_sel9_labels).
  assert (Hpn : labels TNull pn =S= Fset.mk_set (A TNull) nation_formal_attrs)
    by (subst pn; apply q21_project_sel10_labels).
  assert (Hjsl : labels TNull jsl =S= Fset.mk_set (A TNull) supplier_lineitem_formal_attrs).
  { subst jsl ps pl; apply q21_join_project_sel7_sel8_labels. }
  assert (Hpsl : labels TNull psl =S= Fset.mk_set (A TNull) supplier_lineitem_formal_attrs)
    by (subst psl; apply q21_project_sel6_labels).
  assert (Hjslo : labels TNull jslo =S=
                    Fset.mk_set (A TNull) supplier_lineitem_orders_formal_attrs).
  { subst jslo psl po; apply q21_join_sl_project_sel9_labels. }
  assert (Hpslo : labels TNull pslo =S=
                    Fset.mk_set (A TNull) supplier_lineitem_orders_formal_attrs)
    by (subst pslo; apply q21_project_sel5_labels).
  rewrite (q21_sel5_values_join_left pslo pn Hpslo).
  rewrite (q21_sel10_values_join_right pslo pn Hpslo Hpn).
  unfold pslo; rewrite (q21_sel5_values_project_id jslo).
  unfold pn; rewrite (q21_sel10_values_project_id n).
  (* jslo = join psl po; sel5 = sel6 ++ sel9 *)
  unfold q21_sel5_values at 1.
  unfold jslo.
  rewrite (q21_sel6_values_join_left psl po Hpsl).
  rewrite (q21_sel9_values_join_right psl po Hpsl Hpo).
  unfold psl; rewrite (q21_sel6_values_project_id jsl).
  unfold po; rewrite (q21_sel9_values_project_id o).
  (* jsl = join ps pl; sel6 = sel7 ++ sel8 *)
  unfold q21_sel6_values at 1.
  unfold jsl.
  rewrite (q21_sel7_values_join_left ps pl Hps).
  rewrite (q21_sel8_values_join_right ps pl Hps Hpl).
  unfold ps; rewrite (q21_sel7_values_project_id s).
  unfold pl; rewrite (q21_sel8_values_project_id l).
  (* RHS: join (join (join s l) o) n — already as sel5++sel10 *)
  set (jsl0 := join_tuple TNull s l).
  set (jslo0 := join_tuple TNull jsl0 o).
  change (List.app (q21_sel5_values
                      (join_tuple TNull (join_tuple TNull (join_tuple TNull s l) o) n))
                   (q21_sel10_values
                      (join_tuple TNull (join_tuple TNull (join_tuple TNull s l) o) n)))
    with (List.app (q21_sel5_values (join_tuple TNull jslo0 n))
                   (q21_sel10_values (join_tuple TNull jslo0 n))).
  assert (Hjsl0 : labels TNull jsl0 =S= Fset.mk_set (A TNull) supplier_lineitem_formal_attrs).
  { subst jsl0; unfold join_tuple.
    rewrite (Fset.equal_eq_1 _ _ _ _ (labels_mk_tuple _ _ _)).
    rewrite (Fset.equal_eq_1 _ _ _ _
      (Fset.union_eq _ _ _ _ _ Hs Hl)).
    unfold supplier_lineitem_formal_attrs.
    pose proof (Fset.mk_set_app (A TNull) supplier_formal_attrs lineitem_formal_attrs) as Happ.
    rewrite Fset.equal_spec in Happ |- *; intro a; rewrite Happ; reflexivity. }
  assert (Hjslo0 : labels TNull jslo0 =S=
                     Fset.mk_set (A TNull) supplier_lineitem_orders_formal_attrs).
  { subst jslo0; unfold join_tuple.
    rewrite (Fset.equal_eq_1 _ _ _ _ (labels_mk_tuple _ _ _)).
    rewrite (Fset.equal_eq_1 _ _ _ _
      (Fset.union_eq _ _ _ _ _ Hjsl0 Ho)).
    unfold supplier_lineitem_orders_formal_attrs.
    pose proof (Fset.mk_set_app (A TNull) supplier_lineitem_formal_attrs
                  orders_formal_attrs) as Happ.
    rewrite Fset.equal_spec in Happ |- *; intro a; rewrite Happ; reflexivity. }
  rewrite (q21_sel5_values_join_left jslo0 n Hjslo0).
  rewrite (q21_sel10_values_join_right jslo0 n Hjslo0 Hn).
  unfold q21_sel5_values at 1.
  unfold jslo0.
  rewrite (q21_sel6_values_join_left jsl0 o Hjsl0).
  rewrite (q21_sel9_values_join_right jsl0 o Hjsl0 Ho).
  unfold q21_sel6_values at 1.
  unfold jsl0.
  rewrite (q21_sel7_values_join_left s l Hs).
  rewrite (q21_sel8_values_join_right s l Hs Hl).
  reflexivity.
Qed.

(** * List-lift of one-tuple congruence through nested brute products *)

Lemma q21_filter_true_id :
  forall (A : Type) (l : list A),
    List.filter (fun _ : A => true) l = l.
Proof.
  intros A l; induction l as [|x xs IH]; [reflexivity|simpl; rewrite IH; reflexivity].
Qed.

Lemma q21_brute_as_flat_map :
  forall A B,
    brute_left_join_list (tuple TNull) (join_tuple TNull) A B =
    List.flat_map
      (fun a => List.map (fun b => join_tuple TNull a b) B) A.
Proof.
  intros A B.
  unfold brute_left_join_list, theta_join_list, d_join_list.
  apply flat_map_ext; intros a.
  rewrite q21_filter_true_id; reflexivity.
Qed.

Lemma q21_brute_map_map :
  forall (g h : tuple TNull -> tuple TNull) A B,
    brute_left_join_list (tuple TNull) (join_tuple TNull)
      (List.map g A) (List.map h B) =
    List.flat_map
      (fun a => List.map (fun b => join_tuple TNull (g a) (h b)) B) A.
Proof.
  intros g h A B.
  rewrite q21_brute_as_flat_map.
  rewrite flat_map_concat_map, map_map, <- flat_map_concat_map.
  apply flat_map_ext; intros a; rewrite map_map; reflexivity.
Qed.

Lemma q21_flat_map_assoc :
  forall (A B C : Type) (f : B -> list C) (g : A -> list B) (xs : list A),
    List.flat_map f (List.flat_map g xs) =
    List.flat_map (fun x => List.flat_map f (g x)) xs.
Proof.
  intros A B C f g xs.
  induction xs as [|x xs IH]; [reflexivity|].
  simpl; rewrite flat_map_app, IH; reflexivity.
Qed.

Lemma q21_flat_map_map :
  forall (A B C : Type) (f : B -> list C) (g : A -> B) (xs : list A),
    List.flat_map f (List.map g xs) =
    List.flat_map (fun x => f (g x)) xs.
Proof.
  intros A B C f g xs.
  rewrite flat_map_concat_map, map_map, <- flat_map_concat_map; reflexivity.
Qed.

Lemma q21_map_flat_map :
  forall (A B C : Type) (f : B -> C) (g : A -> list B) (xs : list A),
    List.map f (List.flat_map g xs) =
    List.flat_map (fun x => List.map f (g x)) xs.
Proof.
  intros A B C f g xs.
  induction xs as [|x xs IH]; [reflexivity|].
  simpl; rewrite map_app, IH; reflexivity.
Qed.

Lemma q21_flat_map_ext_in :
  forall (A B : Type) (f g : A -> list B) (l : list A),
    (forall x, List.In x l -> f x = g x) ->
    List.flat_map f l = List.flat_map g l.
Proof.
  intros A B f g l H.
  induction l as [|x xs IH]; [reflexivity|].
  simpl; rewrite H by (left; reflexivity).
  rewrite IH; [reflexivity|].
  intros y Hy; apply H; right; exact Hy.
Qed.

Lemma q21_spine_cross4_as_flat_map :
  forall ss' ls' os' ns',
    q21_spine_cross4_tuples ss' ls' os' ns' =
    List.flat_map
      (fun s =>
         List.flat_map
           (fun l =>
              List.flat_map
                (fun o =>
                   List.map (fun n => q21_spine_one_tuple s l o n) ns')
                os')
           ls')
      ss'.
Proof.
  intros ss' ls' os' ns'.
  unfold q21_spine_cross4_tuples, q21_map_project_sel10,
    q21_map_project_sel9, q21_map_project_sel7, q21_map_project_sel8,
    q21_spine_one_tuple.
  rewrite (q21_brute_map_map
             (fun ft => project_row Queries.scalar_select_list_5
                          (q21_sel5_values ft))
             (fun ft => project_row Queries.scalar_select_list_10
                          (q21_sel10_values ft))).
  rewrite (q21_brute_map_map
             (fun ft => project_row Queries.scalar_select_list_6
                          (q21_sel6_values ft))
             (fun ft => project_row Queries.scalar_select_list_9
                          (q21_sel9_values ft))).
  rewrite q21_flat_map_assoc.
  rewrite (q21_brute_map_map
             (fun ft => project_row Queries.scalar_select_list_7
                          (q21_sel7_values ft))
             (fun ft => project_row Queries.scalar_select_list_8
                          (q21_sel8_values ft))).
  rewrite q21_flat_map_assoc.
  apply flat_map_ext; intros s.
  rewrite q21_flat_map_map.
  apply flat_map_ext; intros l.
  rewrite q21_flat_map_map.
  apply flat_map_ext; intros o.
  reflexivity.
Qed.

Lemma q21_cross4_as_flat_map :
  forall ss' ls' os' ns',
    q21_cross4_tuples ss' ls' os' ns' =
    List.flat_map
      (fun s =>
         List.flat_map
           (fun l =>
              List.flat_map
                (fun o =>
                   List.map
                     (fun n =>
                        join_tuple TNull
                          (join_tuple TNull (join_tuple TNull s l) o) n)
                     ns')
                os')
           ls')
      ss'.
Proof.
  intros ss' ls' os' ns'.
  unfold q21_cross4_tuples.
  rewrite !cross_join_tuples_eq_brute_left_join_list.
  rewrite !q21_brute_as_flat_map.
  rewrite q21_flat_map_assoc.
  rewrite q21_flat_map_assoc.
  apply flat_map_ext; intros s.
  rewrite q21_flat_map_map.
  apply flat_map_ext; intros l.
  rewrite q21_flat_map_map.
  apply flat_map_ext; intros o.
  reflexivity.
Qed.

Lemma q21_spine_select4_dots_eq_cross4 :
  forall ss' ls' os' ns',
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs)
      ss' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs)
      ls' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs)
      os' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) nation_formal_attrs)
      ns' ->
    List.map q21_select4_dot_values
      (q21_spine_cross4_tuples ss' ls' os' ns') =
    List.map q21_select4_dot_values
      (q21_cross4_tuples ss' ls' os' ns').
Proof.
  intros ss' ls' os' ns' Hs Hl Ho Hn.
  rewrite q21_spine_cross4_as_flat_map, q21_cross4_as_flat_map.
  rewrite !q21_map_flat_map.
  apply q21_flat_map_ext_in; intros s Hs_in.
  rewrite !q21_map_flat_map.
  apply q21_flat_map_ext_in; intros l Hl_in.
  rewrite !q21_map_flat_map.
  apply q21_flat_map_ext_in; intros o Ho_in.
  rewrite !map_map.
  apply map_ext_in; intros n Hn_in.
  rewrite Forall_forall in Hs, Hl, Ho, Hn.
  apply q21_select4_dots_spine_one;
    [apply Hs|apply Hl|apply Ho|apply Hn]; assumption.
Qed.

(** * Outer select_4 ProjectExact + map transport + E1 exec *)

Definition supplier_lineitem_orders_nation_formal_attrs : list (attribute TNull) :=
  List.app supplier_lineitem_orders_formal_attrs nation_formal_attrs.

Lemma map_fst_select4 :
  map fst Queries.scalar_select_list_4 =
  List.app
    (List.app (List.app q21_sel7_exprs q21_sel8_exprs) q21_sel9_exprs)
    q21_sel10_exprs.
Proof.
  unfold Queries.scalar_select_list_4, q21_sel7_exprs, q21_sel8_exprs,
    q21_sel9_exprs, q21_sel10_exprs.
  cbn [map fst List.app]; reflexivity.
Qed.

Lemma q21_select4_exact :
  forall basesort instance boolean_schedule env ft,
    labels TNull ft =S=
      Fset.mk_set (A TNull) supplier_lineitem_orders_nation_formal_attrs ->
    @scalar_value_list_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule (Env.env_t TNull env ft)
      (map fst Queries.scalar_select_list_4)
      (q21_select4_dot_values ft).
Proof.
  intros basesort instance boolean_schedule env ft Hl.
  rewrite map_fst_select4, q21_select4_dot_values_as_sel5_sel10.
  unfold q21_sel7_exprs, q21_sel8_exprs, q21_sel9_exprs, q21_sel10_exprs,
    q21_sel5_values, q21_sel6_values, q21_sel7_values, q21_sel8_values,
    q21_sel9_values, q21_sel10_values.
  assert (Ha00 : AttrInt32 "s_suppkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha01 : AttrString "s_name" (StringChar 25) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha02 : AttrString "s_address" (StringVarcharN 40) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha03 : AttrInt32 "s_nationkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha04 : AttrString "s_phone" (StringChar 15) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha05 : AttrDecimal "s_acctbal" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha06 : AttrString "s_comment" (StringVarcharN 101) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha07 : AttrInt32 "l_orderkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha08 : AttrInt32 "l_partkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha09 : AttrInt32 "l_suppkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha10 : AttrInt32 "l_linenumber" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha11 : AttrDecimal "l_quantity" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha12 : AttrDecimal "l_extendedprice" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha13 : AttrDecimal "l_discount" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha14 : AttrDecimal "l_tax" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha15 : AttrString "l_returnflag" (StringChar 1) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha16 : AttrString "l_linestatus" (StringChar 1) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha17 : AttrDate "l_shipdate" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha18 : AttrDate "l_commitdate" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha19 : AttrDate "l_receiptdate" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha20 : AttrString "l_shipinstruct" (StringChar 25) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha21 : AttrString "l_shipmode" (StringChar 10) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha22 : AttrString "l_comment" (StringVarcharN 44) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha23 : AttrInt32 "o_orderkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha24 : AttrInt32 "o_custkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha25 : AttrString "o_orderstatus" (StringChar 1) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha26 : AttrDecimal "o_totalprice" 15 2 inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha27 : AttrDate "o_orderdate" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha28 : AttrString "o_orderpriority" (StringChar 15) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha29 : AttrString "o_clerk" (StringChar 15) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha30 : AttrInt32 "o_shippriority" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha31 : AttrString "o_comment" (StringVarcharN 79) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha32 : AttrInt32 "n_nationkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha33 : AttrString "n_name" (StringChar 25) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha34 : AttrInt32 "n_regionkey" inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  assert (Ha35 : AttrString "n_comment" (StringVarcharN 152) inS labels TNull ft)
    by q21_attr_inS_of_labels Hl.
  do 36 q21_dot_cons.
  apply (@scalar_value_list_exact_nil TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule).
Qed.

Lemma q21_join_project_sel5_sel10_labels :
  forall vs vs',
    labels TNull
      (join_tuple TNull
         (project_row Queries.scalar_select_list_5 vs)
         (project_row Queries.scalar_select_list_10 vs')) =S=
    Fset.mk_set (A TNull) supplier_lineitem_orders_nation_formal_attrs.
Proof.
  intros vs vs'.
  unfold join_tuple.
  rewrite (Fset.equal_eq_1 _ _ _ _ (labels_mk_tuple _ _ _)).
  rewrite (Fset.equal_eq_1 _ _ _ _
    (Fset.union_eq _ _ _ _ _
       (q21_project_sel5_labels vs) (q21_project_sel10_labels vs'))).
  unfold supplier_lineitem_orders_nation_formal_attrs.
  pose proof (Fset.mk_set_app (A TNull) supplier_lineitem_orders_formal_attrs
                nation_formal_attrs) as Happ.
  rewrite Fset.equal_spec in Happ |- *; intro a; rewrite Happ; reflexivity.
Qed.

Lemma q21_Forall_spine_cross4_labels :
  forall ss' ls' os' ns',
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs)
      ss' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs)
      ls' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs)
      os' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) nation_formal_attrs)
      ns' ->
    List.Forall
      (fun ft =>
         labels TNull ft =S=
           Fset.mk_set (A TNull) supplier_lineitem_orders_nation_formal_attrs)
      (q21_spine_cross4_tuples ss' ls' os' ns').
Proof.
  intros ss' ls' os' ns' Hs Hl Ho Hn.
  rewrite q21_spine_cross4_as_flat_map.
  rewrite Forall_forall; intros ft Hin.
  rewrite in_flat_map in Hin; destruct Hin as [s [Hs_in Hin]].
  rewrite in_flat_map in Hin; destruct Hin as [l [Hl_in Hin]].
  rewrite in_flat_map in Hin; destruct Hin as [o [Ho_in Hin]].
  rewrite in_map_iff in Hin; destruct Hin as [n [Heq Hn_in]]; subst ft.
  unfold q21_spine_one_tuple.
  apply q21_join_project_sel5_sel10_labels.
Qed.

Lemma q21_project_select4_success :
  forall basesort instance boolean_schedule env rows,
    List.Forall
      (fun ft =>
         labels TNull ft =S=
           Fset.mk_set (A TNull) supplier_lineitem_orders_nation_formal_attrs)
      rows ->
    @eval_project_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null
      boolean_schedule env Queries.scalar_select_list_4 rows
      (SqlSuccess (List.map q21_project_select4 rows)).
Proof.
  intros basesort instance boolean_schedule env rows Hlab.
  unfold q21_project_select4.
  eapply (@tnull_project_rows_success_of_exact_map
            basesort instance boolean_schedule).
  intros row Hin.
  apply q21_select4_exact.
  rewrite Forall_forall in Hlab; apply Hlab; exact Hin.
Qed.

Lemma q21_map_project_select4_spine_eq_cross4 :
  forall ss' ls' os' ns',
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs)
      ss' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs)
      ls' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs)
      os' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) nation_formal_attrs)
      ns' ->
    List.map q21_project_select4 (q21_spine_cross4_tuples ss' ls' os' ns') =
    List.map q21_project_select4 (q21_cross4_tuples ss' ls' os' ns').
Proof.
  intros ss' ls' os' ns' Hs Hl Ho Hn.
  unfold q21_project_select4.
  transitivity
    (List.map (project_row Queries.scalar_select_list_4)
       (List.map q21_select4_dot_values
          (q21_spine_cross4_tuples ss' ls' os' ns'))).
  - symmetry; apply map_map.
  - rewrite (q21_spine_select4_dots_eq_cross4 ss' ls' os' ns' Hs Hl Ho Hn).
    apply map_map.
Qed.

Definition q21_expected_cross_spine : QueryExpr :=
  QExpr_CrossJoin
    (QExpr_Project Queries.scalar_select_list_5
       (QExpr_Filter SExpr_True
          (QExpr_CrossJoin
             (QExpr_Project Queries.scalar_select_list_6
                (QExpr_Filter SExpr_True
                   (QExpr_CrossJoin
                      (QExpr_Project Queries.scalar_select_list_7
                         (@QExpr_Table TNull relname q21_supplier_table_outputs
                            (Rel "supplier")))
                      (QExpr_Project Queries.scalar_select_list_8
                         (@QExpr_Table TNull relname q21_lineitem_table_outputs
                            (Rel "lineitem"))))))
             (QExpr_Project Queries.scalar_select_list_9
                (@QExpr_Table TNull relname q21_orders_table_outputs
                   (Rel "orders"))))))
    (QExpr_Project Queries.scalar_select_list_10
       (@QExpr_Table TNull relname q21_nation_table_outputs
          (Rel "nation"))).

Lemma q21_source_cross_spine_ast :
  q21_cross_spine_of Queries.source_query_expr = Some q21_expected_cross_spine.
Proof.
  unfold q21_cross_spine_of, Queries.source_query_expr, Queries.shared_query_expr_0.
  simpl.
  unfold q21_expected_cross_spine,
    q21_supplier_table_outputs, q21_lineitem_table_outputs,
    q21_orders_table_outputs, q21_nation_table_outputs.
  reflexivity.
Qed.

Theorem Q21_FormalSQL_ordinary_outer_exec :
  Q21_FormalSQL_ordinary_outer_exec_statement.
Proof.
  unfold Q21_FormalSQL_ordinary_outer_exec_statement.
  intros db nation_name D ss ls os ns ss' ls' os' ns' keep_f
    Hpre Hemb Hs Hl Ho Hn Hes Hel Heo Hen Hacc Hexact.
  clear Hexact. (* discharged via q21_select4_exact + spine labels *)
  pose proof (EmbedDB_Q21_as_D_of_embeds db D ss ls os ns ss' ls' os' ns'
    Hemb Hs Hl Ho Hn Hes Hel Heo Hen) as HD; subst D.
  pose proof (embed_rows_Forall_labels_supplier ss ss' Hes) as Hs_lab.
  pose proof (embed_rows_Forall_labels_lineitem ls ls' Hel) as Hl_lab.
  pose proof (embed_rows_Forall_labels_orders os os' Heo) as Ho_lab.
  pose proof (embed_rows_Forall_labels_nation ns ns' Hen) as Hn_lab.
  pose proof (q21_map_project_select4_spine_eq_cross4 ss' ls' os' ns'
    Hs_lab Hl_lab Ho_lab Hn_lab) as Hproj_eq.
  unfold FormalSQL_exec, eval_query_expr_possible_outcome.
  exists q21_boolean_schedule.
  destruct (q21_cross_spine_of_qhat_Some nation_name) as [spine [Hspine Hsrc]].
  unfold q_ordinary_outer; rewrite Hspine.
  assert (Hast : spine = q21_expected_cross_spine).
  { rewrite q21_source_cross_spine_ast in Hsrc; inversion Hsrc; reflexivity. }
  rewrite Hast.
  rewrite <- Hproj_eq.
  eapply EQuery_FilterRows.
  - eapply EQuery_ProjectRows.
    + eapply q21_filter_true_success.
      eapply q21_spine_cross4_success; eassumption.
    + apply q21_project_select4_success.
      apply q21_Forall_spine_cross4_labels; assumption.
  - eapply (@tnull_filter_rows_success_of_acceptance
              (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
              (@_instance TNull (D_of_embeds ss' ls' os' ns'))
              q21_boolean_schedule).
    intros row Hin.
    rewrite Hproj_eq in Hin.
    apply Hacc; exact Hin.
Qed.

(** * STEP E2 — RelNode [filter_outer] ↔ ordinary filter of [q21_cross4_rows]

    [filter_outer] = RelFilter (status ∧ late ∧ nation) over three nested
    RelJoin JoinInner equijoins (supp=l1 @0/4, l1=ord @3/7, s=n @2/9).
    All join conds reference only prefix slots, so each inner filter lifts
    through the outer products; the four filters fuse into
    [q21_ordinary_rel_keep] on the width-11 Cartesian product. *)

Definition q21_pkeep (pr : Pred) (nation_name : string) (r : Row) : bool :=
  match eval_pred_keep pr (Params_Q21 nation_name) r with
  | Some b => b
  | None => false
  end.

Definition q21_join_cond_sl : Pred :=
  EBinary BEq (EInputRef 0%nat) (EInputRef 4%nat).
Definition q21_join_cond_slo : Pred :=
  EBinary BEq (EInputRef 3%nat) (EInputRef 7%nat).
Definition q21_join_cond_slon : Pred :=
  EBinary BEq (EInputRef 2%nat) (EInputRef 9%nat).

Lemma cmp_int_eq_sym :
  forall a b, cmp_int_eq a b = cmp_int_eq b a.
Proof.
  intros [|x|x|x|x y] [|u|u|u|u v]; simpl; try reflexivity.
  - rewrite Z.eqb_sym; reflexivity.
  - destruct x, u; reflexivity.
  - rewrite String.eqb_sym; reflexivity.
Qed.

Lemma eval_pred_keep_beq_swap :
  forall (a b : Expr) (p : Params) (r : Row),
    eval_pred_keep (EBinary BEq a b) p r =
    eval_pred_keep (EBinary BEq b a) p r.
Proof.
  intros a b p r.
  unfold eval_pred_keep, eval_pred; cbn [eval_expr].
  destruct (eval_expr a p (EvRow r)), (eval_expr b p (EvRow r));
    try reflexivity.
  unfold cmp_truth; rewrite cmp_int_eq_sym; reflexivity.
Qed.

Lemma eval_pred_keep_cmp_Some :
  forall (op : BinaryOp) (a b : Expr) (p : Params) (r : Row) va vb,
    op = BEq \/ op = BGt ->
    eval_expr a p (EvRow r) = Some va ->
    eval_expr b p (EvRow r) = Some vb ->
    eval_pred_keep (EBinary op a b) p r =
      Some (truth3_is_true (cmp_truth op va vb)).
Proof.
  intros op a b p r va vb [-> | ->] Ha Hb;
    unfold eval_pred_keep, eval_pred; cbn [eval_expr]; rewrite Ha, Hb;
    destruct (cmp_truth _ va vb); reflexivity.
Qed.

Lemma eval_expr_inputref_Some :
  forall (p : Params) (r : Row) (i : nat),
    (i < List.length r)%nat ->
    exists v, eval_expr (EInputRef i) p (EvRow r) = Some v.
Proof.
  intros p r i Hi.
  destruct (nth_error r i) as [v|] eqn:E.
  - exists v; exact E.
  - apply nth_error_None in E; lia.
Qed.

Lemma q21_pkeep_of_Some :
  forall pr nation_name r b,
    eval_pred_keep pr (Params_Q21 nation_name) r = Some b ->
    eval_pred_keep pr (Params_Q21 nation_name) r =
      Some (q21_pkeep pr nation_name r).
Proof.
  intros pr nation_name r b H; unfold q21_pkeep; rewrite H; reflexivity.
Qed.

Lemma q21_cmp_refs_total :
  forall op i j nation_name r,
    op = BEq \/ op = BGt ->
    (i < List.length r)%nat -> (j < List.length r)%nat ->
    eval_pred_keep (EBinary op (EInputRef i) (EInputRef j))
      (Params_Q21 nation_name) r =
    Some (q21_pkeep (EBinary op (EInputRef i) (EInputRef j)) nation_name r).
Proof.
  intros op i j nation_name r Hop Hi Hj.
  destruct (eval_expr_inputref_Some (Params_Q21 nation_name) r i Hi) as [vi Hvi].
  destruct (eval_expr_inputref_Some (Params_Q21 nation_name) r j Hj) as [vj Hvj].
  eapply q21_pkeep_of_Some, eval_pred_keep_cmp_Some; eassumption.
Qed.

Lemma q21_status_total :
  forall nation_name r,
    (8 < List.length r)%nat ->
    eval_pred_keep pred_status_F (Params_Q21 nation_name) r =
      Some (q21_pkeep pred_status_F nation_name r).
Proof.
  intros nation_name r Hr.
  destruct (eval_expr_inputref_Some (Params_Q21 nation_name) r 8 Hr) as [v Hv].
  eapply q21_pkeep_of_Some, eval_pred_keep_cmp_Some;
    [left; reflexivity | exact Hv | reflexivity].
Qed.

Lemma q21_nation_total :
  forall nation_name r,
    (10 < List.length r)%nat ->
    eval_pred_keep pred_nation_param (Params_Q21 nation_name) r =
      Some (q21_pkeep pred_nation_param nation_name r).
Proof.
  intros nation_name r Hr.
  destruct (eval_expr_inputref_Some (Params_Q21 nation_name) r 10 Hr) as [v Hv].
  eapply q21_pkeep_of_Some, eval_pred_keep_cmp_Some;
    [left; reflexivity | exact Hv | reflexivity].
Qed.

(** Two-ref comparisons only look at prefix slots. *)
Lemma eval_pred_keep_cmp_refs_app :
  forall op i j (p : Params) (x y : Row),
    (i < List.length x)%nat -> (j < List.length x)%nat ->
    eval_pred_keep (EBinary op (EInputRef i) (EInputRef j)) p (List.app x y) =
    eval_pred_keep (EBinary op (EInputRef i) (EInputRef j)) p x.
Proof.
  intros op i j p x y Hi Hj.
  unfold eval_pred_keep, eval_pred; cbn [eval_expr ctx_lookup_input].
  unfold row_nth; rewrite !nth_error_app1 by assumption; reflexivity.
Qed.

Lemma q21_pkeep_cmp_refs_app :
  forall op i j nation_name (x y : Row),
    (i < List.length x)%nat -> (j < List.length x)%nat ->
    q21_pkeep (EBinary op (EInputRef i) (EInputRef j)) nation_name (List.app x y) =
    q21_pkeep (EBinary op (EInputRef i) (EInputRef j)) nation_name x.
Proof.
  intros; unfold q21_pkeep; rewrite eval_pred_keep_cmp_refs_app by assumption;
    reflexivity.
Qed.

(** Row widths from [DB_ok_Q21]. *)
Lemma rows_ok_In :
  forall ok rs r, rows_ok ok rs = true -> List.In r rs -> ok r = true.
Proof.
  intros ok rs r H Hin; unfold rows_ok in H.
  rewrite List.forallb_forall in H; exact (H r Hin).
Qed.

Lemma DB_ok_Q21_widths :
  forall db ss ls os ns,
    DB_ok_Q21 db = true ->
    db_lookup db table_supplier = Some ss ->
    db_lookup db table_lineitem = Some ls ->
    db_lookup db table_orders = Some os ->
    db_lookup db table_nation = Some ns ->
    (forall r, List.In r ss -> List.length r = 3%nat) /\
    (forall r, List.In r ls -> List.length r = 4%nat) /\
    (forall r, List.In r os -> List.length r = 2%nat) /\
    (forall r, List.In r ns -> List.length r = 2%nat).
Proof.
  intros db ss ls os ns Hok Hs Hl Ho Hn.
  unfold DB_ok_Q21 in Hok; rewrite Hs, Hl, Ho, Hn in Hok.
  apply andb_prop in Hok as [Hss Hok].
  apply andb_prop in Hok as [Hls Hok].
  apply andb_prop in Hok as [Hos Hns].
  repeat split; intros r Hin.
  - pose proof (rows_ok_In _ _ _ Hss Hin) as H.
    destruct r as [|[] [|[] [|[] [|]]]]; simpl in H; try discriminate; reflexivity.
  - pose proof (rows_ok_In _ _ _ Hls Hin) as H.
    destruct r as [|[] [|[] [|[] [|[] [|]]]]]; simpl in H; try discriminate;
      reflexivity.
  - pose proof (rows_ok_In _ _ _ Hos Hin) as H.
    destruct r as [|[] [|[] [|]]]; simpl in H; try discriminate; reflexivity.
  - pose proof (rows_ok_In _ _ _ Hns Hin) as H.
    destruct r as [|[] [|[] [|]]]; simpl in H; try discriminate; reflexivity.
Qed.

Lemma In_cross_join_rows_length :
  forall (L R : list Row) nl nr x,
    (forall l, List.In l L -> List.length l = nl) ->
    (forall r, List.In r R -> List.length r = nr) ->
    List.In x (cross_join_rows L R) ->
    List.length x = (nl + nr)%nat.
Proof.
  intros L R nl nr x HL HR Hin.
  destruct (In_cross_join_rows L R x Hin) as [l [r [Hl [Hr ->]]]].
  rewrite List.length_app, (HL l Hl), (HR r Hr); reflexivity.
Qed.

Lemma In_filter_length :
  forall (f : Row -> bool) (xs : list Row) n,
    (forall x, List.In x xs -> List.length x = n) ->
    forall x, List.In x (List.filter f xs) -> List.length x = n.
Proof.
  intros f xs n H x Hin; apply List.filter_In in Hin as [Hin _]; auto.
Qed.

Definition q21_keep_sl_slo (nation_name : string) (r : Row) : bool :=
  andb (q21_pkeep q21_join_cond_sl nation_name r)
       (q21_pkeep q21_join_cond_slo nation_name r).

(** The four RelNode keep bits fuse into the six-conjunct ordinary keep on
    every width-11 row. *)
Lemma q21_fused_keep_eq_ordinary :
  forall nation_name r,
    List.length r = 11%nat ->
    andb (andb (q21_keep_sl_slo nation_name r)
               (q21_pkeep q21_join_cond_slon nation_name r))
         (q21_pkeep pred_outer_filters nation_name r) =
    q21_ordinary_rel_keep nation_name r.
Proof.
  intros nation_name r Hlen.
  assert (Hlt : forall k, (k < 11)%nat -> (k < List.length r)%nat)
    by (intros; lia).
  pose proof (q21_cmp_refs_total BEq 0 4 nation_name r
    (or_introl eq_refl) (Hlt 0%nat ltac:(lia)) (Hlt 4%nat ltac:(lia))) as H0.
  pose proof (q21_cmp_refs_total BEq 7 3 nation_name r
    (or_introl eq_refl) (Hlt 7%nat ltac:(lia)) (Hlt 3%nat ltac:(lia))) as H1.
  pose proof (q21_status_total nation_name r (Hlt 8%nat ltac:(lia))) as H2.
  pose proof (q21_cmp_refs_total BGt 6 5 nation_name r
    (or_intror eq_refl) (Hlt 6%nat ltac:(lia)) (Hlt 5%nat ltac:(lia))) as H3.
  pose proof (q21_cmp_refs_total BEq 2 9 nation_name r
    (or_introl eq_refl) (Hlt 2%nat ltac:(lia)) (Hlt 9%nat ltac:(lia))) as H4.
  pose proof (q21_nation_total nation_name r (Hlt 10%nat ltac:(lia))) as H5.
  assert (Hswap :
    q21_pkeep q21_join_cond_slo nation_name r =
    q21_pkeep (EBinary BEq (EInputRef 7%nat) (EInputRef 3%nat)) nation_name r).
  { unfold q21_pkeep, q21_join_cond_slo; rewrite eval_pred_keep_beq_swap;
      reflexivity. }
  assert (HF : eval_pred_keep pred_outer_filters (Params_Q21 nation_name) r =
    Some (andb (q21_pkeep pred_status_F nation_name r)
            (andb (q21_pkeep
                     (EBinary BGt (EInputRef 6%nat) (EInputRef 5%nat))
                     nation_name r)
                  (q21_pkeep pred_nation_param nation_name r)))).
  { unfold pred_outer_filters.
    eapply eval_pred_keep_band; [exact H2|].
    eapply eval_pred_keep_band; [exact H3|exact H5]. }
  assert (HO := eval_pred_keep_ordinary_as_andb6
    (Params_Q21 nation_name) r _ _ _ _ _ _ H0 H1 H2 H3 H4 H5).
  unfold q21_ordinary_rel_keep; rewrite HO.
  assert (HF' : q21_pkeep pred_outer_filters nation_name r =
    andb (q21_pkeep pred_status_F nation_name r)
      (andb (q21_pkeep (EBinary BGt (EInputRef 6%nat) (EInputRef 5%nat))
               nation_name r)
            (q21_pkeep pred_nation_param nation_name r))).
  { unfold q21_pkeep at 1; rewrite HF; reflexivity. }
  rewrite HF'.
  unfold q21_keep_sl_slo; rewrite Hswap.
  unfold q21_join_cond_sl, q21_join_cond_slon.
  destruct (q21_pkeep (EBinary BEq (EInputRef 0%nat) (EInputRef 4%nat)) nation_name r),
           (q21_pkeep (EBinary BEq (EInputRef 7%nat) (EInputRef 3%nat)) nation_name r),
           (q21_pkeep pred_status_F nation_name r),
           (q21_pkeep (EBinary BGt (EInputRef 6%nat) (EInputRef 5%nat)) nation_name r),
           (q21_pkeep (EBinary BEq (EInputRef 2%nat) (EInputRef 9%nat)) nation_name r),
           (q21_pkeep pred_nation_param nation_name r);
    reflexivity.
Qed.

(** E2: exact RelNode execution of the ordinary outer spine. *)
Theorem Q21_filter_outer_eq_ordinary_filter_of_cross4 :
  forall db nation_name ss ls os ns,
    Preconditions_Q21 db nation_name ->
    db_lookup db table_supplier = Some ss ->
    db_lookup db table_lineitem = Some ls ->
    db_lookup db table_orders = Some os ->
    db_lookup db table_nation = Some ns ->
    Eval_Spec filter_outer (Params_Q21 nation_name) db =
    Some (List.filter (q21_ordinary_rel_keep nation_name)
            (q21_cross4_rows ss ls os ns)).
Proof.
  intros db nation_name ss ls os ns [Hok _] Hs Hl Ho Hn.
  destruct (DB_ok_Q21_widths db ss ls os ns Hok Hs Hl Ho Hn)
    as [Ws [Wl [Wo Wn]]].
  set (p := Params_Q21 nation_name).
  set (k1 := q21_pkeep q21_join_cond_sl nation_name).
  set (k2 := q21_pkeep q21_join_cond_slo nation_name).
  set (k3 := q21_pkeep q21_join_cond_slon nation_name).
  set (X := cross_join_rows ss ls).
  set (Y := cross_join_rows X os).
  set (Z4 := cross_join_rows Y ns).
  assert (WX : forall x, List.In x X -> List.length x = 7%nat)
    by (intros x Hx; exact (In_cross_join_rows_length _ _ 3 4 x Ws Wl Hx)).
  assert (WY : forall x, List.In x Y -> List.length x = 9%nat)
    by (intros x Hx; exact (In_cross_join_rows_length _ _ 7 2 x WX Wo Hx)).
  assert (WZ : forall x, List.In x Z4 -> List.length x = 11%nat)
    by (intros x Hx; exact (In_cross_join_rows_length _ _ 9 2 x WY Wn Hx)).
  (* supplier ⋈ lineitem *)
  assert (HJ1 : join_inner q21_join_cond_sl p ss ls = Some (List.filter k1 X)).
  { apply join_inner_eq_filter_cross_join_rows_in.
    intros s l Hs' Hl'.
    assert (Hw : List.length (List.app s l) = 7%nat)
      by (rewrite List.length_app, (Ws s Hs'), (Wl l Hl'); reflexivity).
    apply q21_cmp_refs_total; [left; reflexivity| |]; lia. }
  (* ⋈ orders; lift k1 through the orders product *)
  assert (HJ2 : join_inner q21_join_cond_slo p (List.filter k1 X) os =
                Some (List.filter (fun r => andb (k1 r) (k2 r)) Y)).
  { rewrite (join_inner_eq_filter_cross_join_rows_in _ _ _ _ k2).
    - f_equal.
      rewrite (cross_join_rows_filter_left k1 k1).
      + apply filter_filter_andb.
      + intros x o Hx _; unfold k1, q21_join_cond_sl.
        apply q21_pkeep_cmp_refs_app; rewrite (WX x Hx); lia.
    - intros x o Hx Ho'.
      apply (In_filter_length k1 X 7%nat WX) in Hx.
      assert (Hw : List.length (List.app x o) = 9%nat)
        by (rewrite List.length_app, Hx, (Wo o Ho'); reflexivity).
      apply q21_cmp_refs_total; [left; reflexivity| |]; lia. }
  (* ⋈ nation; lift k1 ∧ k2 through the nation product *)
  assert (HJ3 : join_inner q21_join_cond_slon p
                  (List.filter (fun r => andb (k1 r) (k2 r)) Y) ns =
                Some (List.filter
                        (fun r => andb (andb (k1 r) (k2 r)) (k3 r)) Z4)).
  { rewrite (join_inner_eq_filter_cross_join_rows_in _ _ _ _ k3).
    - f_equal.
      rewrite (cross_join_rows_filter_left
                 (fun r => andb (k1 r) (k2 r)) (fun r => andb (k1 r) (k2 r))).
      + apply filter_filter_andb.
      + intros y n Hy _; unfold k1, k2, q21_join_cond_sl, q21_join_cond_slo.
        rewrite !q21_pkeep_cmp_refs_app by (rewrite (WY y Hy); lia).
        reflexivity.
    - intros y n Hy Hn'.
      apply (In_filter_length _ Y 9%nat WY) in Hy.
      assert (Hw : List.length (List.app y n) = 11%nat)
        by (rewrite List.length_app, Hy, (Wn n Hn'); reflexivity).
      apply q21_cmp_refs_total; [left; reflexivity| |]; lia. }
  (* outer RelFilter (status ∧ late ∧ nation) *)
  assert (HF : filter_rows pred_outer_filters p
                 (List.filter (fun r => andb (andb (k1 r) (k2 r)) (k3 r)) Z4) =
               Some (List.filter (q21_pkeep pred_outer_filters nation_name)
                 (List.filter (fun r => andb (andb (k1 r) (k2 r)) (k3 r)) Z4))).
  { apply filter_rows_eq_List_filter.
    intros r Hr.
    apply (In_filter_length _ Z4 11%nat WZ) in Hr.
    assert (Hlt : forall k, (k < 11)%nat -> (k < List.length r)%nat)
      by (intros; lia).
    eapply q21_pkeep_of_Some.
    unfold pred_outer_filters.
    eapply eval_pred_keep_band; [apply (q21_status_total _ _ (Hlt 8%nat ltac:(lia)))|].
    eapply eval_pred_keep_band.
    - apply (q21_cmp_refs_total BGt 6 5 nation_name r (or_intror eq_refl));
        apply Hlt; lia.
    - apply (q21_nation_total _ _ (Hlt 10%nat ltac:(lia))). }
  unfold q21_join_cond_sl, q21_join_cond_slo, q21_join_cond_slon in HJ1, HJ2, HJ3.
  unfold Eval_Spec, filter_outer, join_outer_spine, join_supp_l1_ord,
    join_supp_l1, scan_supplier, scan_lineitem, scan_orders, scan_nation.
  cbn [eval_rel].
  rewrite Hs, Hl, Ho, Hn.
  rewrite HJ1, HJ2, HJ3, HF.
  f_equal.
  rewrite filter_filter_andb.
  apply List.filter_ext_in.
  intros r Hr.
  apply q21_fused_keep_eq_ordinary; exact (WZ r Hr).
Qed.

(** * STEP E3 — compose E1 + E2 + Milestones A/B/D + generic filter bag *)

(** ** E3 leaves: labels of the four-way FormalSQL product *)

Lemma q21_labels_join_mk_set :
  forall t1 t2 a1 a2,
    labels TNull t1 =S= Fset.mk_set (A TNull) a1 ->
    labels TNull t2 =S= Fset.mk_set (A TNull) a2 ->
    labels TNull (join_tuple TNull t1 t2) =S=
      Fset.mk_set (A TNull) (List.app a1 a2).
Proof.
  intros t1 t2 a1 a2 H1 H2.
  unfold join_tuple.
  rewrite (Fset.equal_eq_1 _ _ _ _ (labels_mk_tuple _ _ _)).
  rewrite (Fset.equal_eq_1 _ _ _ _ (Fset.union_eq _ _ _ _ _ H1 H2)).
  pose proof (Fset.mk_set_app (A TNull) a1 a2) as Happ.
  rewrite Fset.equal_spec in Happ |- *; intro a; rewrite Happ; reflexivity.
Qed.

Lemma q21_Forall_labels_cross :
  forall fts1 fts2 a1 a2,
    List.Forall (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) a1) fts1 ->
    List.Forall (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) a2) fts2 ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) (List.app a1 a2))
      (cross_join_tuples fts1 fts2).
Proof.
  intros fts1 fts2 a1 a2 H1 H2.
  rewrite Forall_forall; intros ft Hin.
  destruct (In_cross_join_tuples fts1 fts2 ft Hin) as [t1 [t2 [Ht1 [Ht2 ->]]]].
  rewrite Forall_forall in H1, H2.
  apply q21_labels_join_mk_set; auto.
Qed.

Lemma q21_Forall_cross4_labels :
  forall ss' ls' os' ns',
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs) ss' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs) ls' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs) os' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) nation_formal_attrs) ns' ->
    List.Forall
      (fun ft => labels TNull ft =S=
         Fset.mk_set (A TNull) supplier_lineitem_orders_nation_formal_attrs)
      (q21_cross4_tuples ss' ls' os' ns').
Proof.
  intros ss' ls' os' ns' Hs Hl Ho Hn.
  unfold q21_cross4_tuples, supplier_lineitem_orders_nation_formal_attrs,
    supplier_lineitem_orders_formal_attrs, supplier_lineitem_formal_attrs.
  repeat apply q21_Forall_labels_cross; assumption.
Qed.

(** ** E3 leaves: scan row correspondences (lineitem / orders / nation) *)

Ltac q21_embed_dot_rewrite :=
  match goal with
  | |- value_correspond _ (dot TNull (mk_tuple_lists ?atts ?vals) ?att) _ =>
      rewrite (mk_tuple_lists_dot atts vals att)
        by (change ((att inS? Fset.mk_set (A TNull) atts) = true);
            vm_compute; reflexivity)
  end;
  simpl mk_att.

Lemma EmbedRow_lineitem_row_correspond :
  forall ok sk cd rd ft,
    EmbedRow_lineitem [VInt ok; VInt sk; VInt cd; VInt rd] = Some ft ->
    row_correspond slot_map_lineitem ft [VInt ok; VInt sk; VInt cd; VInt rd].
Proof.
  intros ok sk cd rd ft Hemb.
  unfold EmbedRow_lineitem in Hemb.
  destruct (int32_checked ok) as [ok32|] eqn:Hok; try discriminate.
  destruct (int32_checked sk) as [sk32|] eqn:Hsk; try discriminate.
  injection Hemb as Hft; subst ft.
  unfold row_correspond, slot_map_lineitem; cbn [List.length].
  split; [reflexivity|].
  intros i sb v Hi Hv.
  destruct i as [|[|[|[|i]]]]; cbn [nth_error] in Hi, Hv;
    try (destruct i; discriminate Hi);
    injection Hi as <-; injection Hv as <-;
    unfold slot_rel_type, sb_formal; simpl attr_type;
    q21_embed_dot_rewrite.
  - rewrite <- (int32_checked_result_value_local _ _ Hok); apply VC_int32.
  - rewrite <- (int32_checked_result_value_local _ _ Hsk); apply VC_int32.
  - apply VC_date.
  - apply VC_date.
Qed.

Lemma EmbedRow_orders_row_correspond :
  forall ok st ft,
    Char1Canonical st ->
    EmbedRow_orders [VInt ok; VText st] = Some ft ->
    row_correspond slot_map_orders ft [VInt ok; VText st].
Proof.
  intros ok st ft Hcan Hemb.
  unfold EmbedRow_orders in Hemb.
  destruct (int32_checked ok) as [ok32|] eqn:Hok; try discriminate.
  injection Hemb as Hft; subst ft.
  unfold row_correspond, slot_map_orders; cbn [List.length].
  split; [reflexivity|].
  intros i sb v Hi Hv.
  destruct i as [|[|i]]; cbn [nth_error] in Hi, Hv;
    try (destruct i; discriminate Hi);
    injection Hi as <-; injection Hv as <-;
    unfold slot_rel_type, sb_formal; simpl attr_type;
    q21_embed_dot_rewrite.
  - rewrite <- (int32_checked_result_value_local _ _ Hok); apply VC_int32.
  - change (string_rtrim_spaces (string_take 1 st))
      with (string_explicit_cast (StringChar 1) st).
    unfold Char1Canonical in Hcan; rewrite Hcan.
    apply VC_text.
Qed.

Lemma EmbedRow_nation_row_correspond :
  forall nk nn ft,
    Char25Canonical nn ->
    EmbedRow_nation [VInt nk; VText nn] = Some ft ->
    row_correspond slot_map_nation ft [VInt nk; VText nn].
Proof.
  intros nk nn ft Hcan Hemb.
  unfold EmbedRow_nation in Hemb.
  destruct (int32_checked nk) as [nk32|] eqn:Hnk; try discriminate.
  injection Hemb as Hft; subst ft.
  unfold row_correspond, slot_map_nation; cbn [List.length].
  split; [reflexivity|].
  intros i sb v Hi Hv.
  destruct i as [|[|i]]; cbn [nth_error] in Hi, Hv;
    try (destruct i; discriminate Hi);
    injection Hi as <-; injection Hv as <-;
    unfold slot_rel_type, sb_formal; simpl attr_type;
    q21_embed_dot_rewrite.
  - rewrite <- (int32_checked_result_value_local _ _ Hnk); apply VC_int32.
  - change (string_rtrim_spaces (string_take 25 nn))
      with (string_explicit_cast (StringChar 25) nn).
    unfold Char25Canonical in Hcan; rewrite Hcan.
    apply VC_text.
Qed.

(** Table-level lift: [embed_rows] pairs each RelNode row with its embed. *)
Lemma embed_rows_Forall2_correspond :
  forall (E : Row -> option (tuple TNull)) (sm : SlotMap) (P : Row -> Prop)
         rs fts,
    (forall r ft, P r -> E r = Some ft -> row_correspond sm ft r) ->
    List.Forall P rs ->
    embed_rows E rs = Some fts ->
    List.Forall2 (row_correspond sm) fts rs.
Proof.
  intros E sm P rs; induction rs as [|r rs IH]; intros fts Hrow HP Hemb;
    simpl in Hemb.
  - injection Hemb as <-; constructor.
  - destruct (E r) as [ft|] eqn:Hr; [|discriminate].
    destruct (embed_rows E rs) as [fts0|] eqn:Hrs; [|discriminate].
    injection Hemb as <-.
    inversion HP; subst.
    constructor; [eapply Hrow; eassumption|].
    apply IH; auto.
Qed.

Lemma Forall2_bag_correspond_unordered :
  forall sm fts rrs,
    List.Forall2 (row_correspond sm) fts rrs ->
    bag_correspond_unordered sm fts rrs.
Proof.
  intros sm fts rrs H; exists rrs; split; [apply Permutation_refl|exact H].
Qed.

Lemma rows_ok_Forall :
  forall ok rs, rows_ok ok rs = true -> List.Forall (fun r => ok r = true) rs.
Proof.
  intros ok rs H; unfold rows_ok in H.
  apply Forall_forall; intros r Hin.
  rewrite List.forallb_forall in H; exact (H r Hin).
Qed.

(** Scan bags for all four tables from [Preconditions_Q21] + embeds. *)
Lemma Q21_scan_bags :
  forall db nation_name ss ls os ns ss' ls' os' ns',
    Preconditions_Q21 db nation_name ->
    db_lookup db table_supplier = Some ss ->
    db_lookup db table_lineitem = Some ls ->
    db_lookup db table_orders = Some os ->
    db_lookup db table_nation = Some ns ->
    embed_rows EmbedRow_supplier ss = Some ss' ->
    embed_rows EmbedRow_lineitem ls = Some ls' ->
    embed_rows EmbedRow_orders os = Some os' ->
    embed_rows EmbedRow_nation ns = Some ns' ->
    List.Forall2 (row_correspond slot_map_supplier) ss' ss /\
    List.Forall2 (row_correspond slot_map_lineitem) ls' ls /\
    List.Forall2 (row_correspond slot_map_orders) os' os /\
    List.Forall2 (row_correspond slot_map_nation) ns' ns.
Proof.
  intros db nation_name ss ls os ns ss' ls' os' ns'
    [Hok [_ [Hchar _]]] Hs Hl Ho Hn Hes Hel Heo Hen.
  unfold DB_ok_Q21 in Hok; rewrite Hs, Hl, Ho, Hn in Hok.
  apply andb_prop in Hok as [Hss Hok].
  apply andb_prop in Hok as [Hls Hok].
  apply andb_prop in Hok as [Hos Hns].
  unfold DB_CharCanonical_Q21 in Hchar; rewrite Hs, Ho, Hn in Hchar.
  destruct Hchar as [Cs [Co Cn]].
  repeat split.
  - eapply (embed_rows_Forall2_correspond EmbedRow_supplier slot_map_supplier
              row_supplier_char_ok); [|exact Cs|exact Hes].
    intros r ft Hc He.
    destruct r as [|[] [|[] [|[] [|]]]]; simpl in Hc; try contradiction.
    apply EmbedRow_supplier_row_correspond; assumption.
  - eapply (embed_rows_Forall2_correspond EmbedRow_lineitem slot_map_lineitem
              (fun r => row_ok_lineitem r = true));
      [|exact (rows_ok_Forall _ _ Hls)|exact Hel].
    intros r ft Hc He.
    destruct r as [|[] [|[] [|[] [|[] [|]]]]]; simpl in Hc; try discriminate.
    apply EmbedRow_lineitem_row_correspond; assumption.
  - eapply (embed_rows_Forall2_correspond EmbedRow_orders slot_map_orders
              row_orders_char_ok); [|exact Co|exact Heo].
    intros r ft Hc He.
    destruct r as [|[] [|[] [|]]]; simpl in Hc; try contradiction.
    apply EmbedRow_orders_row_correspond; assumption.
  - eapply (embed_rows_Forall2_correspond EmbedRow_nation slot_map_nation
              row_nation_char_ok); [|exact Cn|exact Hen].
    intros r ft Hc He.
    destruct r as [|[] [|[] [|]]]; simpl in Hc; try contradiction.
    apply EmbedRow_nation_row_correspond; assumption.
Qed.

(** ** E3 leaves: present / fresh side conditions for Milestone D *)

Ltac q21_fresh_of_labels Hl :=
  rewrite (Fset.mem_eq_2 _ _ _ Hl); vm_compute; reflexivity.

Lemma q21_cross4_present_fresh_of_labels :
  forall ss' ls' os' ns',
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs) ss' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs) ls' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs) os' ->
    List.Forall
      (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) nation_formal_attrs) ns' ->
    q21_cross4_present_fresh ss' ls' os' ns'.
Proof.
  intros ss' ls' os' ns' Hs Hl Ho Hn.
  pose proof (q21_Forall_labels_cross _ _ _ _ Hs Hl) as Hsl.
  pose proof (q21_Forall_labels_cross _ _ _ _ Hsl Ho) as Hslo.
  rewrite Forall_forall in Hs, Hl, Ho, Hn, Hsl, Hslo.
  unfold q21_cross4_present_fresh, slot_map_present, slot_map_fresh_from.
  repeat split; intros ft Hin.
  - pose proof (Hs ft Hin) as H; unfold slot_map_supplier;
      repeat constructor; simpl sb_formal; q21_attr_inS_of_labels H.
  - pose proof (Hl ft Hin) as H; unfold slot_map_lineitem;
      repeat constructor; simpl sb_formal; q21_attr_inS_of_labels H.
  - pose proof (Ho ft Hin) as H; unfold slot_map_orders;
      repeat constructor; simpl sb_formal; q21_attr_inS_of_labels H.
  - pose proof (Hn ft Hin) as H; unfold slot_map_nation;
      repeat constructor; simpl sb_formal; q21_attr_inS_of_labels H.
  - pose proof (Hs ft Hin) as H; unfold slot_map_lineitem;
      repeat constructor; simpl sb_formal; q21_fresh_of_labels H.
  - pose proof (Hsl ft Hin) as H; unfold slot_map_orders;
      repeat constructor; simpl sb_formal; q21_fresh_of_labels H.
  - pose proof (Hslo ft Hin) as H; unfold slot_map_nation;
      repeat constructor; simpl sb_formal; q21_fresh_of_labels H.
Qed.

(** ** E3 leaves: watched-value shapes of embedded tuples *)

Definition q21_is_int32 (v : value) : Prop :=
  match v with Value_int32 _ => True | _ => False end.

Definition q21_is_date (v : value) : Prop :=
  match v with Value_date _ => True | _ => False end.

Definition q21_char1_canonical_value (v : value) : Prop :=
  match v with
  | Value_string (StringChar 1, Some s) => Char1Canonical s
  | Value_string (StringChar 1, None) => True
  | _ => False
  end.

Definition q21_char25_canonical_value (v : value) : Prop :=
  match v with
  | Value_string (StringChar 25, Some s) => Char25Canonical s
  | Value_string (StringChar 25, None) => True
  | _ => False
  end.

Definition q21_supplier_member (ft : tuple TNull) : Prop :=
  labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs /\
  q21_is_int32 (dot TNull ft (Attr_int32 "s_suppkey")) /\
  q21_is_int32 (dot TNull ft (Attr_int32 "s_nationkey")).

Definition q21_lineitem_member (ft : tuple TNull) : Prop :=
  labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs /\
  q21_is_int32 (dot TNull ft (Attr_int32 "l_orderkey")) /\
  q21_is_int32 (dot TNull ft (Attr_int32 "l_suppkey")) /\
  q21_is_date (dot TNull ft (Attr_date "l_commitdate")) /\
  q21_is_date (dot TNull ft (Attr_date "l_receiptdate")).

Definition q21_orders_member (ft : tuple TNull) : Prop :=
  labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs /\
  q21_is_int32 (dot TNull ft (Attr_int32 "o_orderkey")) /\
  q21_char1_canonical_value
    (dot TNull ft (Attr_string "o_orderstatus" (StringChar 1))).

Definition q21_nation_member (ft : tuple TNull) : Prop :=
  labels TNull ft =S= Fset.mk_set (A TNull) nation_formal_attrs /\
  q21_is_int32 (dot TNull ft (Attr_int32 "n_nationkey")) /\
  q21_char25_canonical_value
    (dot TNull ft (Attr_string "n_name" (StringChar 25))).

Ltac q21_embed_dot_rewrite_any :=
  match goal with
  | |- context [dot TNull (mk_tuple_lists ?atts ?vals) ?att] =>
      rewrite (mk_tuple_lists_dot atts vals att)
        by (change ((att inS? Fset.mk_set (A TNull) atts) = true);
            vm_compute; reflexivity)
  end.

Lemma embed_rows_Forall_of :
  forall (E : Row -> option (tuple TNull)) (P : Row -> Prop)
         (Q : tuple TNull -> Prop) rs fts,
    (forall r ft, P r -> E r = Some ft -> Q ft) ->
    List.Forall P rs ->
    embed_rows E rs = Some fts ->
    List.Forall Q fts.
Proof.
  intros E P Q rs; induction rs as [|r rs IH]; intros fts HQ HP Hemb;
    simpl in Hemb.
  - injection Hemb as <-; constructor.
  - destruct (E r) as [ft|] eqn:Hr; [|discriminate].
    destruct (embed_rows E rs) as [fts0|] eqn:Hrs; [|discriminate].
    injection Hemb as <-.
    inversion HP; subst.
    constructor; [eapply HQ; eassumption|].
    apply IH; auto.
Qed.

Lemma EmbedRow_supplier_member :
  forall r ft, EmbedRow_supplier r = Some ft -> q21_supplier_member ft.
Proof.
  intros r ft Hemb.
  split; [eapply EmbedRow_supplier_labels; exact Hemb|].
  destruct r as [|[] [|[] [|[] [|]]]]; try discriminate.
  unfold EmbedRow_supplier in Hemb.
  destruct (int32_checked _); [|discriminate].
  destruct (int32_checked _); [|discriminate].
  injection Hemb as <-.
  split; q21_embed_dot_rewrite_any; simpl; exact I.
Qed.

Lemma EmbedRow_lineitem_member :
  forall r ft, EmbedRow_lineitem r = Some ft -> q21_lineitem_member ft.
Proof.
  intros r ft Hemb.
  split; [eapply EmbedRow_lineitem_labels; exact Hemb|].
  destruct r as [|[] [|[] [|[] [|[] [|]]]]]; try discriminate.
  unfold EmbedRow_lineitem in Hemb.
  destruct (int32_checked _); [|discriminate].
  destruct (int32_checked _); [|discriminate].
  injection Hemb as <-.
  repeat split; q21_embed_dot_rewrite_any; simpl; exact I.
Qed.

Lemma EmbedRow_orders_member :
  forall r ft,
    row_orders_char_ok r ->
    EmbedRow_orders r = Some ft -> q21_orders_member ft.
Proof.
  intros r ft Hc Hemb.
  split; [eapply EmbedRow_orders_labels; exact Hemb|].
  destruct r as [|[] [|[] [|]]]; simpl in Hc; try contradiction.
  unfold EmbedRow_orders in Hemb.
  destruct (int32_checked _); [|discriminate].
  injection Hemb as <-.
  split; q21_embed_dot_rewrite_any; simpl; [exact I|].
  change (string_rtrim_spaces (string_take 1 s))
    with (string_explicit_cast (StringChar 1) s).
  unfold Char1Canonical in *; rewrite Hc; exact Hc.
Qed.

Lemma EmbedRow_nation_member :
  forall r ft,
    row_nation_char_ok r ->
    EmbedRow_nation r = Some ft -> q21_nation_member ft.
Proof.
  intros r ft Hc Hemb.
  split; [eapply EmbedRow_nation_labels; exact Hemb|].
  destruct r as [|[] [|[] [|]]]; simpl in Hc; try contradiction.
  unfold EmbedRow_nation in Hemb.
  destruct (int32_checked _); [|discriminate].
  injection Hemb as <-.
  split; q21_embed_dot_rewrite_any; simpl; [exact I|].
  change (string_rtrim_spaces (string_take 25 s))
    with (string_explicit_cast (StringChar 25) s).
  unfold Char25Canonical in *; rewrite Hc; exact Hc.
Qed.

(** ** E3 leaves: Milestone B side conditions on projected product tuples *)

Ltac q21_memtac :=
  match goal with
  | H : labels TNull ?t =S= _ |- (_ inS? labels TNull ?t) = true =>
      q21_attr_inS_of_labels H
  | H : labels TNull ?t =S= _ |- (_ inS? labels TNull ?t) = false =>
      q21_fresh_of_labels H
  end.

Ltac q21_dot_path :=
  repeat first
    [ rewrite (dot_join_tuple_1 TNull _ _ _) by q21_memtac
    | rewrite (dot_join_tuple_2 TNull _ _ _) by q21_memtac ].

Lemma q21_In_cross4_tuples :
  forall ss' ls' os' ns' ft0,
    List.In ft0 (q21_cross4_tuples ss' ls' os' ns') ->
    exists s l o n,
      List.In s ss' /\ List.In l ls' /\ List.In o os' /\ List.In n ns' /\
      ft0 = join_tuple TNull (join_tuple TNull (join_tuple TNull s l) o) n.
Proof.
  intros ss' ls' os' ns' ft0 Hin.
  unfold q21_cross4_tuples in Hin.
  destruct (In_cross_join_tuples _ _ _ Hin) as [slo [n [Hslo [Hn ->]]]].
  destruct (In_cross_join_tuples _ _ _ Hslo) as [sl [o [Hsl [Ho ->]]]].
  destruct (In_cross_join_tuples _ _ _ Hsl) as [s [l [Hs [Hl ->]]]].
  exists s, l, o, n; repeat split; assumption.
Qed.

Lemma q21_cross4_member_cor_facts :
  forall s l o n,
    q21_supplier_member s ->
    q21_lineitem_member l ->
    q21_orders_member o ->
    q21_nation_member n ->
    q21_outer_cor_labels_ok
      (q21_project_select4
         (join_tuple TNull (join_tuple TNull (join_tuple TNull s l) o) n)) /\
    q21_outer_cor_text_canonical
      (q21_project_select4
         (join_tuple TNull (join_tuple TNull (join_tuple TNull s l) o) n)).
Proof.
  intros s l o n [Hs [Ss0 Ss1]] [Hl [Sl0 [Sl1 [Sl2 Sl3]]]]
    [Ho [So0 So1]] [Hn [Sn0 Sn1]].
  pose proof (q21_labels_join_mk_set _ _ _ _ Hs Hl) as Hsl.
  pose proof (q21_labels_join_mk_set _ _ _ _ Hsl Ho) as Hslo.
  set (ft0 := join_tuple TNull (join_tuple TNull (join_tuple TNull s l) o) n).
  destruct (q21_select4_watched_dots ft0)
    as (W0 & W1 & W3 & W7 & W9 & W18 & W19 & W23 & W25 & W32 & W33).
  unfold q21_outer_cor_labels_ok, q21_outer_cor_text_canonical.
  rewrite W0, W3, W7, W9, W18, W19, W23, W25, W32, W33.
  unfold ft0.
  unfold q21_project_select4.
  repeat split;
    first
      [ rewrite (Fset.mem_eq_2 _ _ _ (q21_project_row_labels _ _));
        vm_compute; reflexivity
      | q21_dot_path; assumption ].
Qed.

Lemma q21_Forall2_In_l :
  forall (X Y : Type) (R : X -> Y -> Prop) xs ys x,
    List.Forall2 R xs ys -> List.In x xs -> exists y, R x y.
Proof.
  intros X Y R xs ys x HF; induction HF as [|a b xs0 ys0 Hab HF IH];
    intros Hin; [contradiction|].
  destruct Hin as [<-|Hin]; [exists b; exact Hab|exact (IH Hin)].
Qed.

Lemma slot_map_outer_cor_non_numeric :
  slot_map_non_numeric slot_map_outer_cor.
Proof.
  unfold slot_map_non_numeric, slot_map_outer_cor.
  repeat constructor; intros scale H; discriminate H.
Qed.

(** FormalSQL keep bit: the RelNode ordinary keep of the (unique) RelNode
    row determined by the watched cor slots. *)
Definition q21_formal_ordinary_keep (nation_name : string)
    (ft : tuple TNull) : bool :=
  q21_ordinary_rel_keep nation_name (rel_row_of slot_map_outer_cor ft).

Lemma q21_formal_ordinary_keep_of_correspond :
  forall nation_name ft rr,
    row_correspond slot_map_outer_cor ft rr ->
    q21_formal_ordinary_keep nation_name ft = q21_ordinary_rel_keep nation_name rr.
Proof.
  intros nation_name ft rr Hrc.
  unfold q21_formal_ordinary_keep.
  rewrite <- (row_correspond_rel_row_of _ _ _
                slot_map_outer_cor_non_numeric Hrc).
  reflexivity.
Qed.

(** ** E3: the ordinary outer spine correspondence *)

(** Strengthened form: additionally exports the Milestone-B side condition
    on every FormalSQL outer tuple (needed by correlated layers above). *)
Definition Q21_OuterOrdinarySpineCorrespond_strong_statement : Prop :=
  forall (db : DB) (nation_name : string),
    Preconditions_Q21 db nation_name ->
    exists (D : db_state) (fts : list (tuple TNull)) (rows : list Row),
      EmbedDB_Q21 db = Some D /\
      FormalSQL_exec D (q_ordinary_outer nation_name) fts /\
      Eval_Spec filter_outer (Params_Q21 nation_name) db = Some rows /\
      bag_correspond_unordered slot_map_outer_cor fts rows /\
      List.Forall q21_outer_cor_labels_ok fts.

Theorem Q21_OuterOrdinarySpineCorrespond_strong :
  Q21_OuterOrdinarySpineCorrespond_strong_statement.
Proof.
  unfold Q21_OuterOrdinarySpineCorrespond_strong_statement.
  intros db nation_name Hpre.
  pose proof Hpre as [Hok [Hnn [Hchar Hemb]]].
  destruct (EmbedDB_Q21 db) as [D|] eqn:HD; [|contradiction].
  (* table lookups and embeds behind [EmbedDB_Q21] *)
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
  (* per-table FormalSQL facts *)
  pose proof (embed_rows_Forall_labels_supplier ss ss' Hes) as Ls.
  pose proof (embed_rows_Forall_labels_lineitem ls ls' Hel) as Ll.
  pose proof (embed_rows_Forall_labels_orders os os' Heo) as Lo.
  pose proof (embed_rows_Forall_labels_nation ns ns' Hen) as Ln.
  unfold DB_CharCanonical_Q21 in Hchar; rewrite Hs, Ho, Hn in Hchar.
  destruct Hchar as [Cs [Co Cn]].
  pose proof (embed_rows_Forall_of EmbedRow_supplier (fun _ => True)
    q21_supplier_member ss ss'
    (fun r ft _ He => EmbedRow_supplier_member r ft He)
    (proj2 (Forall_forall (fun _ => True) ss) (fun _ _ => I))
    Hes) as Ms.
  pose proof (embed_rows_Forall_of EmbedRow_lineitem (fun _ => True)
    q21_lineitem_member ls ls'
    (fun r ft _ He => EmbedRow_lineitem_member r ft He)
    (proj2 (Forall_forall (fun _ => True) ls) (fun _ _ => I))
    Hel) as Ml.
  pose proof (embed_rows_Forall_of EmbedRow_orders row_orders_char_ok
    q21_orders_member os os' EmbedRow_orders_member Co Heo) as Mo.
  pose proof (embed_rows_Forall_of EmbedRow_nation row_nation_char_ok
    q21_nation_member ns ns' EmbedRow_nation_member Cn Hen) as Mn.
  (* Milestone D + A: product bag under the cor SlotMap *)
  destruct (Q21_scan_bags db nation_name ss ls os ns ss' ls' os' ns'
              Hpre Hs Hl Ho Hn Hes Hel Heo Hen) as [Bs [Bl [Bo Bn]]].
  pose proof (Q21_four_way_cross_product_bag_correspond
    ss' ls' os' ns' ss ls os ns
    (Forall2_bag_correspond_unordered _ _ _ Bs)
    (Forall2_bag_correspond_unordered _ _ _ Bl)
    (Forall2_bag_correspond_unordered _ _ _ Bo)
    (Forall2_bag_correspond_unordered _ _ _ Bn)
    (q21_cross4_present_fresh_of_labels ss' ls' os' ns' Ls Ll Lo Ln))
    as Hprod.
  pose proof (Q21_bag_correspond_select4_rename _ _ Hprod) as Hcor.
  exists D,
    (List.filter (q21_formal_ordinary_keep nation_name)
       (List.map q21_project_select4 (q21_cross4_tuples ss' ls' os' ns'))),
    (List.filter (q21_ordinary_rel_keep nation_name)
       (q21_cross4_rows ss ls os ns)).
  split; [reflexivity|].
  split; [|split; [|split]].
  - (* FormalSQL side: E1 *)
    eapply (Q21_FormalSQL_ordinary_outer_exec db nation_name D
              ss ls os ns ss' ls' os' ns'
              (q21_formal_ordinary_keep nation_name));
      try eassumption.
    + (* ordinary ConjList acceptance at the chosen keep bit (Milestone B) *)
      intros ft Hin.
      pose proof Hin as Hin'.
      apply in_map_iff in Hin' as [ft0 [<- Hin0]].
      destruct Hcor as [rrs' [_ HF]].
      destruct (q21_Forall2_In_l _ _ _ _ _ _ HF Hin) as [rr Hrc].
      destruct (q21_In_cross4_tuples _ _ _ _ _ Hin0)
        as [s [l [o [n [Hs0 [Hl0 [Ho0 [Hn0 ->]]]]]]]].
      rewrite Forall_forall in Ms, Ml, Mo, Mn.
      destruct (q21_cross4_member_cor_facts s l o n
                  (Ms s Hs0) (Ml l Hl0) (Mo o Ho0) (Mn n Hn0))
        as [Hlab Htxt].
      destruct (Q21_ordinary_same_bit (@_basesort TNull D) (@_instance TNull D)
                  q21_boolean_schedule _ rr nation_name Hrc Hlab Htxt Hnn)
        as [b [Hacc Hk]].
      rewrite (q21_formal_ordinary_keep_of_correspond nation_name _ rr Hrc).
      unfold q21_ordinary_rel_keep; rewrite Hk.
      exact Hacc.
    + (* exact select_4 Dot list on every product tuple *)
      intros ft Hin.
      apply q21_select4_exact.
      pose proof (q21_Forall_cross4_labels ss' ls' os' ns' Ls Ll Lo Ln) as HL.
      rewrite Forall_forall in HL; exact (HL ft Hin).
  - (* RelNode side: E2 *)
    exact (Q21_filter_outer_eq_ordinary_filter_of_cross4
             db nation_name ss ls os ns Hpre Hs Hl Ho Hn).
  - (* bag: generic filter preservation with the determined keep bit *)
    apply bag_correspond_unordered_filter; [exact Hcor|].
    intros ft rr Hrc.
    apply q21_formal_ordinary_keep_of_correspond; exact Hrc.
  - (* exported side condition on every outer tuple *)
    apply Forall_forall; intros ft Hin.
    apply filter_In in Hin as [Hin _].
    apply in_map_iff in Hin as [ft0 [<- Hin0]].
    destruct (q21_In_cross4_tuples _ _ _ _ _ Hin0)
      as [s [l [o [n [Hs0 [Hl0 [Ho0 [Hn0 ->]]]]]]]].
    rewrite Forall_forall in Ms, Ml, Mo, Mn.
    exact (proj1 (q21_cross4_member_cor_facts s l o n
                    (Ms s Hs0) (Ml l Hl0) (Mo o Ho0) (Mn n Hn0))).
Qed.

Theorem Q21_OuterOrdinarySpineCorrespond :
  Q21_OuterOrdinarySpineCorrespond_statement.
Proof.
  intros db nation_name Hpre.
  destruct (Q21_OuterOrdinarySpineCorrespond_strong db nation_name Hpre)
    as [D [fts [rows [HD [Hx [He [Hb _]]]]]]].
  exists D, fts, rows; repeat split; assumption.
Qed.

(** * Status

    Milestone A–D Qed; bag path Qed; FormalSQL spine → spine_cross4 Qed.

    STEP E1 Qed:
      - Dot identity sel7/8/9/10/6/5 + values_project_id
      - join_tuple transport sel7/8/9/10/6/5
      - [q21_select4_dots_spine_one]
      - [q21_spine_select4_dots_eq_cross4]
      - [Q21_FormalSQL_ordinary_outer_exec]

    STEP E2 Qed:
      - [Q21_filter_outer_eq_ordinary_filter_of_cross4]
        (three RelJoin equijoin conds + RelFilter fuse into
         [q21_ordinary_rel_keep] over [q21_cross4_rows]; only DB_ok widths used)

    STEP E3 Qed:
      - [Q21_OuterOrdinarySpineCorrespond]
        (= [Q21_OuterOrdinarySpineCorrespond_statement]; E1 + E2 + D + A +
         generic [bag_correspond_unordered_filter] + B [Q21_ordinary_same_bit];
         FormalSQL keep bit [q21_formal_ordinary_keep] via [rel_row_of])

    No new preconditions / TCB. No Admit. No EXISTS / NOT EXISTS. *)
