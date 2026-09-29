(* Q21WhereFusion — fuse the staged filters back into the exact lowered
   8-conjunct Q21 WHERE.

   Catalog WHERE (generated order):
     [suppkey; orderkey; status='F'; late(l1); EXISTS l2; NOT EXISTS l3;
      nationkey; n_name = nation_name]  under one SExpr_ConjList And_F.

   Results:
     Q21_catalog_where_shape            recognition from q̂ (every nation)
     Q21_catalog_where_row_acceptance   exact 8-way acceptance bit
     Q21_catalog_where_accept_true_iff  boolean form
     Q21_catalog_where_filter_rows      Filter(8-way WHERE) over outer rows
     Q21_stacked_where_filters_eq       filter fusion (exact list equality)
     Q21_catalog_where_fusion           catalog stage ≡ staged query, same
                                        outcome, bag-corresponds to AntiJoin

   GROUP BY / COUNT star / Project / Sort out of scope. No Admit/Axiom. *)

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

Require Import Q21OuterOrdinarySpine Q21ExistsL2 Q21NotExistsL3.

From Logos.FormalSQL Require Import SubqueryFacts MembershipJoinCompositionFacts.

(** * Exact catalog WHERE: recognition *)

Definition q21_where_sites : list (list string) :=
  ([[]; ["b0"]; ["b1"; "b2"]; ["b3"; "b4"; "b5"]; ["b6"; "b7"; "b8"; "b9"];
    ["b10"; "b11"; "b12"; "b13"; "b14"];
    ["b15"; "b16"; "b17"; "b18"; "b19"; "b20"];
    ["b21"; "b22"; "b23"; "b24"; "b25"; "b26"; "b27"]])%list.

(** The eight lowered conjuncts, in the generated order. *)
Definition q21_catalog_where_conjuncts (nation_name : string)
    : list (@scalar_expr TNull relname ScalarResultBoolean) :=
  [q21_fsql_pred_suppkey; q21_fsql_pred_orderkey; q21_fsql_pred_status_F;
   q21_fsql_pred_late; q21_exists_l2_sexpr; q21_not_exists_l3_sexpr;
   q21_fsql_pred_nationkey; q21_fsql_pred_nation nation_name].

Definition q21_catalog_where (nation_name : string)
    : @scalar_expr TNull relname ScalarResultBoolean :=
  SExpr_ConjList q21_where_sites And_F (q21_catalog_where_conjuncts nation_name).

(** The WHERE stage under Group/Project/Project in a lowered Q21 query. *)
Definition q21_where_stage_of (q : QueryExpr) : option QueryExpr :=
  match q with
  | QExpr_Project _
      (QExpr_OrderBy _
        (QExpr_Group _ _ _
          (QExpr_Project _
            (QExpr_Project _ w)))) => Some w
  | _ => None
  end.

Definition q21_catalog_where_stage (nation_name : string) : QueryExpr :=
  QExpr_Filter (q21_catalog_where nation_name)
    (QExpr_Project Queries.scalar_select_list_4
       (QExpr_Filter SExpr_True q21_expected_cross_spine)).

(** Recognition: for every nation parameter, the WHERE stage of q̂ is exactly
    one 8-conjunct [SExpr_ConjList] (this order) over the outer projected
    CrossJoin spine. *)
Theorem Q21_catalog_where_shape :
  forall nation_name,
    q21_where_stage_of (qhat_Q21 nation_name) =
      Some (q21_catalog_where_stage nation_name).
Proof.
  intros nation_name.
  unfold qhat_Q21.
  destruct (String.eqb nation_name catalog_nation_name) eqn:Heq.
  - apply String.eqb_eq in Heq; subst nation_name.
    unfold q21_where_stage_of, Queries.source_query_expr, Queries.shared_query_expr_0.
    simpl.
    unfold q21_catalog_where_stage, q21_catalog_where, q21_catalog_where_conjuncts,
      q21_where_sites, q21_expected_cross_spine,
      q21_supplier_table_outputs, q21_lineitem_table_outputs,
      q21_orders_table_outputs, q21_nation_table_outputs,
      q21_exists_l2_sexpr, q21_exists_l2_subquery,
      q21_not_exists_l3_sexpr, q21_not_exists_l3_subquery.
    reflexivity.
  - unfold q21_where_stage_of, subst_nation_query,
      Queries.source_query_expr, Queries.shared_query_expr_0.
    simpl.
    unfold subst_nation_sexpr_bool; simpl.
    unfold subst_nation_sexpr_val, subst_nation_aggterm; simpl.
    unfold q21_catalog_where_stage, q21_catalog_where, q21_catalog_where_conjuncts,
      q21_where_sites, q21_expected_cross_spine,
      q21_supplier_table_outputs, q21_lineitem_table_outputs,
      q21_orders_table_outputs, q21_nation_table_outputs,
      q21_exists_l2_sexpr, q21_exists_l2_subquery,
      q21_not_exists_l3_sexpr, q21_not_exists_l3_subquery.
    reflexivity.
Qed.

(** The staged ordinary six are exactly conjuncts 1–4 and 7–8. *)
Lemma Q21_catalog_where_ordinary_positions :
  forall nation_name,
    ordinary_outer_conjlist nation_name =
      SExpr_ConjList
        ([[]; ["b0"]; ["b1"; "b2"]; ["b3"; "b4"; "b5"];
          ["b6"; "b7"; "b8"; "b9"]; ["b10"; "b11"]])%list And_F
        [nth 0 (q21_catalog_where_conjuncts nation_name) SExpr_True;
         nth 1 (q21_catalog_where_conjuncts nation_name) SExpr_True;
         nth 2 (q21_catalog_where_conjuncts nation_name) SExpr_True;
         nth 3 (q21_catalog_where_conjuncts nation_name) SExpr_True;
         nth 6 (q21_catalog_where_conjuncts nation_name) SExpr_True;
         nth 7 (q21_catalog_where_conjuncts nation_name) SExpr_True] /\
    nth 4 (q21_catalog_where_conjuncts nation_name) SExpr_True = q21_exists_l2_sexpr /\
    nth 5 (q21_catalog_where_conjuncts nation_name) SExpr_True = q21_not_exists_l3_sexpr.
Proof.
  intros nation_name; split; [|split; reflexivity].
  rewrite ordinary_outer_conjlist_shape; reflexivity.
Qed.

(** * Row level: exact 8-conjunct acceptance *)

(** Decide function tagging the eight conjuncts (EXISTS / NOT / ordinary). *)
Definition q21_where_leaf_decide (nation_name : string)
    (b0 b1 b2 b3 be bn b4 b5 : bool)
    (e : @scalar_expr TNull relname ScalarResultBoolean) : bool :=
  match e with
  | SExpr_Exists _ => be
  | SExpr_Not _ => bn
  | _ => q21_ordinary_leaf_decide nation_name b0 b1 b2 b3 b4 b5 e
  end.

Lemma q21_andb8_fold :
  forall b0 b1 b2 b3 x y b4 b5 : bool,
    andb (andb (andb b0 (andb b1 (andb b2 (andb b3 (andb b4 b5))))) x) (negb y) =
    scalar_acceptance_fold And_F [b0; b1; b2; b3; x; negb y; b4; b5].
Proof. intros; destruct b0, b1, b2, b3, x, y, b4, b5; reflexivity. Qed.

Lemma q21_where_decide_map :
  forall nation_name b0 b1 b2 b3 be bn b4 b5,
    map (q21_where_leaf_decide nation_name b0 b1 b2 b3 be bn b4 b5)
      (q21_catalog_where_conjuncts nation_name) =
    [b0; b1; b2; b3; be; bn; b4; b5].
Proof.
  intros; unfold q21_catalog_where_conjuncts; simpl; reflexivity.
Qed.

(** FormalSQL keep bit of the catalog WHERE on an outer tuple. *)
Definition q21_catalog_where_keep (nation_name : string)
    (ls' : list (tuple TNull)) (ft : tuple TNull) : bool :=
  andb (andb (q21_formal_ordinary_keep nation_name ft)
             (existsb (q21_l2_match nation_name ft) ls'))
       (negb (existsb (q21_l3_match nation_name ft) ls')).

Section Q21CatalogWhere.
Variables ss' ls' os' ns' : list (tuple TNull).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Local Abbreviation D := (D_of_embeds ss' ls' os' ns').

Local Abbreviation accept_at :=
  (@scalar_expr_acceptance_exact_at TNull relname
    (@_basesort TNull D) (@_instance TNull D) unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null boolean_schedule).

(** Exact acceptance of the lowered 8-conjunct WHERE at
    [(ordinary ∧ EXISTS l2) ∧ ¬EXISTS l3]. *)
Theorem Q21_catalog_where_row_acceptance :
  forall nation_name ft rr,
    List.Forall q21_lineitem_member ls' ->
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    q21_outer_cor_text_canonical ft ->
    NationNameOk nation_name ->
    accept_at (Env.env_t TNull nil ft) (q21_catalog_where nation_name)
      (q21_catalog_where_keep nation_name ls' ft).
Proof.
  intros nation_name ft rr Hmem Hrc Hlab Htxt Hnn.
  set (bs := @_basesort TNull D). set (ins := @_instance TNull D).
  destruct (Q21_leaf_suppkey_same_bit bs ins boolean_schedule ft rr
              (Params_Q21 nation_name) Hrc Hlab) as [b0 [A0 K0]].
  destruct (Q21_leaf_orderkey_same_bit bs ins boolean_schedule ft rr
              (Params_Q21 nation_name) Hrc Hlab) as [b1 [A1 K1]].
  destruct (Q21_leaf_status_F_same_bit bs ins boolean_schedule ft rr
              (Params_Q21 nation_name) Hrc Hlab Htxt) as [b2 [A2 K2]].
  destruct (Q21_leaf_late_same_bit bs ins boolean_schedule ft rr
              (Params_Q21 nation_name) Hrc Hlab) as [b3 [A3 K3]].
  destruct (Q21_leaf_nationkey_same_bit bs ins boolean_schedule ft rr
              (Params_Q21 nation_name) Hrc Hlab) as [b4 [A4 K4]].
  destruct (Q21_leaf_nation_name_same_bit bs ins boolean_schedule ft rr
              nation_name Hrc Hlab Htxt Hnn) as [b5 [A5 K5]].
  pose proof (Q21_exists_l2_acceptance ss' ls' os' ns' boolean_schedule
                nation_name ft Hmem Hlab) as AE.
  pose proof (Q21_not_exists_l3_acceptance ss' ls' os' ns' boolean_schedule
                nation_name ft Hmem Hlab) as AN.
  assert (Hord : q21_formal_ordinary_keep nation_name ft =
                 andb b0 (andb b1 (andb b2 (andb b3 (andb b4 b5))))).
  { rewrite (q21_formal_ordinary_keep_of_correspond nation_name ft rr Hrc).
    unfold q21_ordinary_rel_keep.
    rewrite (Q21_relnode_ordinary_keep_as_andb6 rr nation_name
               b0 b1 b2 b3 b4 b5 K0 K1 K2 K3 K4 K5).
    reflexivity. }
  assert (Hacc8 : accept_at (Env.env_t TNull nil ft) (q21_catalog_where nation_name)
    (scalar_acceptance_fold And_F
       (map (q21_where_leaf_decide nation_name b0 b1 b2 b3
               (existsb (q21_l2_match nation_name ft) ls')
               (negb (existsb (q21_l3_match nation_name ft) ls')) b4 b5)
            (q21_catalog_where_conjuncts nation_name)))).
  { unfold q21_catalog_where.
    eapply (@scalar_expr_conj_list_acceptance_exact TNull relname
      bs ins unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      value_is_null boolean_schedule q21_where_sites (Env.env_t TNull nil ft) And_F).
    intros e Hin; unfold q21_catalog_where_conjuncts in Hin; simpl in Hin.
    destruct Hin as [<-|[<-|[<-|[<-|[<-|[<-|[<-|[<-|[]]]]]]]]];
      [exact A0|exact A1|exact A2|exact A3|exact AE|exact AN|exact A4|exact A5]. }
  rewrite q21_where_decide_map in Hacc8.
  unfold q21_catalog_where_keep; rewrite Hord, q21_andb8_fold.
  exact Hacc8.
Qed.

(** Boolean form: the WHERE accepts iff ordinary ∧ EXISTS(l2) ∧ ¬EXISTS(l3). *)
Corollary Q21_catalog_where_accept_true_iff :
  forall nation_name ft rr,
    List.Forall q21_lineitem_member ls' ->
    row_correspond slot_map_outer_cor ft rr ->
    q21_outer_cor_labels_ok ft ->
    q21_outer_cor_text_canonical ft ->
    NationNameOk nation_name ->
    accept_at (Env.env_t TNull nil ft) (q21_catalog_where nation_name) true
    <->
    (q21_ordinary_rel_keep nation_name rr = true /\
     existsb (q21_l2_match nation_name ft) ls' = true /\
     existsb (q21_l3_match nation_name ft) ls' = false).
Proof.
  intros nation_name ft rr Hmem Hrc Hlab Htxt Hnn.
  pose proof (Q21_catalog_where_row_acceptance nation_name ft rr
                Hmem Hrc Hlab Htxt Hnn) as Hk.
  rewrite <- (q21_formal_ordinary_keep_of_correspond nation_name ft rr Hrc).
  split.
  - intro Ht.
    pose proof (accept_at_unique_bool _ _ _ _ _ _ _ Hk Ht) as E.
    unfold q21_catalog_where_keep in E.
    destruct (q21_formal_ordinary_keep nation_name ft),
             (existsb (q21_l2_match nation_name ft) ls'),
             (existsb (q21_l3_match nation_name ft) ls');
      simpl in E; try discriminate; repeat split.
  - intros [H1 [H2 H3]].
    unfold q21_catalog_where_keep in Hk; rewrite H1, H2, H3 in Hk; exact Hk.
Qed.

(** * Filter level: the catalog WHERE over outer rows *)

Theorem Q21_catalog_where_filter_rows :
  forall nation_name fts outcome,
    List.Forall q21_lineitem_member ls' ->
    (forall ft, List.In ft fts -> exists rr, row_correspond slot_map_outer_cor ft rr) ->
    List.Forall q21_outer_cor_labels_ok fts ->
    List.Forall q21_outer_cor_text_canonical fts ->
    NationNameOk nation_name ->
    @eval_filter_rows_outcome TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      value_is_null boolean_schedule nil (q21_catalog_where nation_name) fts outcome
    <->
    outcome = SqlSuccess
      (List.filter (q21_catalog_where_keep nation_name ls') fts).
Proof.
  intros nation_name fts outcome Hmem Hpart Hlabs Htxts Hnn.
  apply eval_filter_rows_acceptance_exact.
  intros ft Hin.
  destruct (Hpart ft Hin) as [rr Hrc].
  rewrite Forall_forall in Hlabs, Htxts.
  exact (Q21_catalog_where_row_acceptance nation_name ft rr Hmem Hrc
           (Hlabs ft Hin) (Htxts ft Hin) Hnn).
Qed.

End Q21CatalogWhere.

(** * Stacked-filter equivalence (exact list equality: same order,
      same multiplicities) *)
Theorem Q21_stacked_where_filters_eq :
  forall nation_name ls' (xs : list (tuple TNull)),
    List.filter (q21_catalog_where_keep nation_name ls') xs =
    List.filter (fun ft => negb (existsb (q21_l3_match nation_name ft) ls'))
      (List.filter (fun ft => existsb (q21_l2_match nation_name ft) ls')
         (List.filter (q21_formal_ordinary_keep nation_name) xs)).
Proof.
  intros nation_name ls' xs.
  rewrite !filter_filter_andb.
  apply List.filter_ext; intro x.
  unfold q21_catalog_where_keep; symmetry; apply Stdlib.Bool.Bool.andb_assoc.
Qed.

(** * Exact-catalog fusion *)

(** Side conditions on every projected product tuple. *)
Lemma q21_projected_member_facts :
  forall ss' ls' os' ns' ft,
    List.Forall q21_supplier_member ss' ->
    List.Forall q21_lineitem_member ls' ->
    List.Forall q21_orders_member os' ->
    List.Forall q21_nation_member ns' ->
    List.In ft (List.map q21_project_select4 (q21_cross4_tuples ss' ls' os' ns')) ->
    q21_outer_cor_labels_ok ft /\ q21_outer_cor_text_canonical ft.
Proof.
  intros ss' ls' os' ns' ft Ms Ml Mo Mn Hin.
  apply in_map_iff in Hin as [ft0 [<- Hin0]].
  destruct (q21_In_cross4_tuples _ _ _ _ _ Hin0)
    as [s [l [o [n [Hs0 [Hl0 [Ho0 [Hn0 ->]]]]]]]].
  rewrite Forall_forall in Ms, Ml, Mo, Mn.
  exact (q21_cross4_member_cor_facts s l o n
           (Ms s Hs0) (Ml l Hl0) (Mo o Ho0) (Mn n Hn0)).
Qed.

Lemma q21_Forall_filter :
  forall (X : Type) (P : X -> Prop) (f : X -> bool) xs,
    (forall x, List.In x xs -> P x) -> List.Forall P (List.filter f xs).
Proof.
  intros X P f xs H; apply Forall_forall; intros x Hx.
  apply filter_In in Hx as [Hx _]; exact (H x Hx).
Qed.

(** The exact catalog WHERE stage and the staged
    NOT EXISTS ∘ EXISTS ∘ ordinary query yield the SAME success outcome
    [fts], which corresponds (unordered bag, exact multiplicity) to the
    RelNode anti-join rows. *)
Definition Q21_catalog_where_fusion_statement : Prop :=
  forall (db : DB) (nation_name : string),
    Preconditions_Q21 db nation_name ->
    exists (D : db_state) (fts : list (tuple TNull)) (rows : list Row),
      EmbedDB_Q21 db = Some D /\
      q21_where_stage_of (qhat_Q21 nation_name) =
        Some (q21_catalog_where_stage nation_name) /\
      FormalSQL_exec D (q21_catalog_where_stage nation_name) fts /\
      FormalSQL_exec D (q_not_exists_outer nation_name) fts /\
      Eval_Spec anti_not_exists_l3 (Params_Q21 nation_name) db = Some rows /\
      bag_correspond_unordered slot_map_outer_cor fts rows /\
      List.Forall q21_outer_cor_labels_ok fts /\
      List.Forall q21_outer_cor_text_canonical fts.

Theorem Q21_catalog_where_fusion : Q21_catalog_where_fusion_statement.
Proof.
  unfold Q21_catalog_where_fusion_statement.
  intros db nation_name Hpre.
  pose proof Hpre as [Hok [Hnn [Hchar Hemb]]].
  destruct (EmbedDB_Q21 db) as [D|] eqn:HD; [|contradiction].
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
  (* per-table facts *)
  pose proof (embed_rows_Forall_labels_supplier ss ss' Hes) as Ls.
  pose proof (embed_rows_Forall_labels_lineitem ls ls' Hel) as Ll.
  pose proof (embed_rows_Forall_labels_orders os os' Heo) as Lo.
  pose proof (embed_rows_Forall_labels_nation ns ns' Hen) as Ln.
  unfold DB_CharCanonical_Q21 in Hchar; rewrite Hs, Ho, Hn in Hchar.
  destruct Hchar as [Cs [Co Cn]].
  pose proof (embed_rows_Forall_of EmbedRow_supplier (fun _ => True)
    q21_supplier_member ss ss'
    (fun r ft _ He => EmbedRow_supplier_member r ft He)
    (proj2 (Forall_forall (fun _ => True) ss) (fun _ _ => I)) Hes) as Ms.
  pose proof (embed_rows_Forall_of EmbedRow_lineitem (fun _ => True)
    q21_lineitem_member ls ls'
    (fun r ft _ He => EmbedRow_lineitem_member r ft He)
    (proj2 (Forall_forall (fun _ => True) ls) (fun _ _ => I)) Hel) as Ml.
  pose proof (embed_rows_Forall_of EmbedRow_orders row_orders_char_ok
    q21_orders_member os os' EmbedRow_orders_member Co Heo) as Mo.
  pose proof (embed_rows_Forall_of EmbedRow_nation row_nation_char_ok
    q21_nation_member ns ns' EmbedRow_nation_member Cn Hen) as Mn.
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
  set (P := List.map q21_project_select4 (q21_cross4_tuples ss' ls' os' ns')).
  set (R := q21_cross4_rows ss ls os ns).
  assert (Pfacts : forall ft, List.In ft P ->
            q21_outer_cor_labels_ok ft /\ q21_outer_cor_text_canonical ft)
    by (intros ft Hin; exact (q21_projected_member_facts ss' ls' os' ns' ft
                                Ms Ml Mo Mn Hin)).
  assert (Ppart : forall ft, List.In ft P ->
            exists rr, row_correspond slot_map_outer_cor ft rr).
  { intros ft Hin; destruct Hcor as [rrs' [_ HF]].
    exact (q21_Forall2_In_l _ _ _ _ _ _ HF Hin). }
  (* RelNode stack: ordinary → semi → anti *)
  set (rows0 := List.filter (q21_ordinary_rel_keep nation_name) R).
  set (rows1 := List.filter (q21_rel_exists_l2_keep nation_name ls) rows0).
  set (rows2 := List.filter (q21_rel_not_exists_l3_keep nation_name ls) rows1).
  assert (B0 : bag_correspond_unordered slot_map_outer_cor
                 (List.filter (q21_formal_ordinary_keep nation_name) P) rows0).
  { apply bag_correspond_unordered_filter; [exact Hcor|].
    intros ft rr Hrc; apply q21_formal_ordinary_keep_of_correspond; exact Hrc. }
  pose proof (Q21_exists_l2_bag nation_name _ _ ls' ls B0 Bl) as B1.
  pose proof (Q21_not_exists_l3_bag nation_name _ _ ls' ls B1 Bl) as B2.
  assert (E0 : Eval_Spec filter_outer (Params_Q21 nation_name) db = Some rows0)
    by exact (Q21_filter_outer_eq_ordinary_filter_of_cross4
                db nation_name ss ls os ns Hpre Hs Hl Ho Hn).
  assert (E1s : Eval_Spec semi_exists_l2 (Params_Q21 nation_name) db = Some rows1)
    by exact (Q21_semi_exists_l2_eval db nation_name ss ls os ns rows0
                Hpre Hs Hl Ho Hn E0 (q21_bag_rows_width _ _ B0)).
  assert (E2a : Eval_Spec anti_not_exists_l3 (Params_Q21 nation_name) db = Some rows2)
    by exact (Q21_anti_not_exists_l3_eval db nation_name ss ls os ns rows1
                Hpre Hs Hl Ho Hn E1s (q21_bag_rows_width _ _ B1)).
  (* FormalSQL witness: the catalog WHERE filter over the projected spine *)
  exists (D_of_embeds ss' ls' os' ns'),
    (List.filter (q21_catalog_where_keep nation_name ls') P), rows2.
  split; [reflexivity|].
  split; [exact (Q21_catalog_where_shape nation_name)|].
  split; [|split; [|split; [|split; [|split]]]].
  - (* exact catalog WHERE stage *)
    unfold FormalSQL_exec, eval_query_expr_possible_outcome.
    exists q21_boolean_schedule.
    unfold q21_catalog_where_stage.
    assert (Hinner :
      @eval_query_expr_outcome TNull relname
        (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
        (@_instance TNull (D_of_embeds ss' ls' os' ns')) unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error value_is_null
        q21_boolean_schedule nil
        (QExpr_Project Queries.scalar_select_list_4
           (QExpr_Filter SExpr_True q21_expected_cross_spine))
        (SqlSuccess P)).
    { unfold P; rewrite <- (q21_map_project_select4_spine_eq_cross4 ss' ls' os' ns'
                              Ls Ll Lo Ln).
      eapply EQuery_ProjectRows.
      - eapply q21_filter_true_success; eapply q21_spine_cross4_success; eassumption.
      - apply q21_project_select4_success.
        apply q21_Forall_spine_cross4_labels; assumption. }
    eapply EQuery_FilterRows; [exact Hinner|].
    apply (proj2 (Q21_catalog_where_filter_rows ss' ls' os' ns'
                      q21_boolean_schedule nation_name P _ Ml Ppart
                      (proj2 (Forall_forall _ P) (fun ft H => proj1 (Pfacts ft H)))
                      (proj2 (Forall_forall _ P) (fun ft H => proj2 (Pfacts ft H)))
                      Hnn)).
      reflexivity.
  - (* staged NOT EXISTS ∘ EXISTS ∘ ordinary query, same outcome *)
    rewrite Q21_stacked_where_filters_eq.
    assert (Hord : FormalSQL_exec (D_of_embeds ss' ls' os' ns')
                     (q_ordinary_outer nation_name)
                     (List.filter (q21_formal_ordinary_keep nation_name) P)).
    { eapply (Q21_FormalSQL_ordinary_outer_exec db nation_name _
                ss ls os ns ss' ls' os' ns'
                (q21_formal_ordinary_keep nation_name)); try eassumption.
      - intros ft Hin.
        destruct (Pfacts ft Hin) as [Hlab Htxt].
        destruct (Ppart ft Hin) as [rr Hrc].
        destruct (Q21_ordinary_same_bit
                    (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
                    (@_instance TNull (D_of_embeds ss' ls' os' ns'))
                    q21_boolean_schedule _ rr nation_name Hrc Hlab Htxt Hnn)
          as [b [Hacc Hk]].
        rewrite (q21_formal_ordinary_keep_of_correspond nation_name _ rr Hrc).
        unfold q21_ordinary_rel_keep; rewrite Hk.
        exact Hacc.
      - intros ft Hin.
        apply q21_select4_exact.
        pose proof (q21_Forall_cross4_labels ss' ls' os' ns' Ls Ll Lo Ln) as HL.
        rewrite Forall_forall in HL; exact (HL ft Hin). }
    unfold FormalSQL_exec, eval_query_expr_possible_outcome in *.
    destruct Hord as [sched Hq].
    exists sched.
    unfold q_not_exists_outer, q_exists_outer.
    assert (Lab0 : List.Forall q21_outer_cor_labels_ok
                     (List.filter (q21_formal_ordinary_keep nation_name) P))
      by (apply q21_Forall_filter; intros ft Hin; exact (proj1 (Pfacts ft Hin))).
    eapply EQuery_FilterRows.
    + eapply EQuery_FilterRows; [exact Hq|].
      apply (proj2 (Q21_exists_l2_filter_rows ss' ls' os' ns' sched
                      nation_name _ _ Ml Lab0)).
      reflexivity.
    + assert (Lab1 : List.Forall q21_outer_cor_labels_ok
                (List.filter (fun ft => existsb (q21_l2_match nation_name ft) ls')
                   (List.filter (q21_formal_ordinary_keep nation_name) P))).
      { apply q21_Forall_filter; intros ft Hin.
        apply filter_In in Hin as [Hin _].
        exact (proj1 (Pfacts ft Hin)). }
      apply (proj2 (Q21_not_exists_l3_filter_rows ss' ls' os' ns' sched
                      nation_name _ _ Ml Lab1)).
      reflexivity.
  - (* RelNode anti-join *)
    exact E2a.
  - (* bag / multiplicity *)
    rewrite Q21_stacked_where_filters_eq; exact B2.
  - apply q21_Forall_filter; intros ft Hin; exact (proj1 (Pfacts ft Hin)).
  - apply q21_Forall_filter; intros ft Hin; exact (proj2 (Pfacts ft Hin)).
Qed.
