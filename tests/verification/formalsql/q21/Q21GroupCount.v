(* Q21GroupCount — GROUP BY s_name + COUNT-star over the exact catalog WHERE.

   FormalSQL (from q̂):  Group [s_name ; COUNT-star AS numwait] BY s_name HAVING True
                         (Project select_2 (Project select_3 WHERE-stage))
   RelNode:              RelAggregate anti_not_exists_l3 [EInputRef 1] [AggCountStarSpec]

   Results:
     Q21_catalog_group_shape             exact lowered grouping shape
     Q21_group_key_correspond            group key ↔ RelNode slot 1
     q21_sim_run_aggregate               COUNT-star: RelNode count = group size
     Q21_group_count_is_occurrences      group size = key occurrences in input
     Q21_group_rows_correspond           grouped rows ↔ aggregate rows
     Q21_GroupCountStageCorrespond       composed aggregate stage

   Premise Q21CountSafe: FormalSQL COUNT-star is int64 (errors on overflow).
   Final Project / ORDER BY out of scope. No Admit/Axiom. *)

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

Require Import Q21OuterOrdinarySpine Q21ExistsL2 Q21NotExistsL3 Q21WhereFusion.

From SQLFS Require Partition ListSort SqlListFacts.
From FormalSQLLogical Require Import Agg.

(** * COUNT-star core: FormalSQL [partition_rec] ≈ RelNode [fold_groups]

    Both scan the input in order and maintain an association list keyed by
    the group key: FormalSQL [insert_in_partition] prepends the row to the
    first entry whose key is [Oset.eq_bool]-equal, else appends a singleton;
    RelNode [store_add_row] finds the first [list_sql_value_eqb]-equal entry,
    increments its COUNT-star slot, else appends a fresh slot.  On aligned
    inputs whose key equalities agree, they stay in lockstep: same entries,
    same order, and RelNode count = FormalSQL group length. *)

Lemma sql_value_eqb_true_eq :
  forall a b, sql_value_eqb a b = true -> a = b.
Proof.
  intros [|x|x|x|x y] [|u|u|u|u v]; simpl; intro H; try discriminate; try reflexivity.
  - apply Z.eqb_eq in H; subst; reflexivity.
  - apply Bool.eqb_prop in H; subst; reflexivity.
  - apply String.eqb_eq in H; subst; reflexivity.
  - apply andb_prop in H as [H1 H2]; apply Z.eqb_eq in H1; apply Z.eqb_eq in H2;
      subst; reflexivity.
Qed.

Lemma list_sql_value_eqb_true_eq :
  forall a b, list_sql_value_eqb a b = true -> a = b.
Proof.
  induction a as [|x xs IH]; intros [|y ys] H; simpl in H; try discriminate;
    [reflexivity|].
  apply andb_prop in H as [H1 H2].
  rewrite (sql_value_eqb_true_eq _ _ H1), (IH _ H2); reflexivity.
Qed.

Lemma sql_value_eqb_refl : forall a, sql_value_eqb a a = true.
Proof.
  intros [|x|x|x|x y]; simpl; try reflexivity.
  - apply Z.eqb_refl.
  - destruct x; reflexivity.
  - apply String.eqb_refl.
  - rewrite !Z.eqb_refl; reflexivity.
Qed.

Lemma list_sql_value_eqb_refl : forall a, list_sql_value_eqb a a = true.
Proof.
  induction a as [|x xs IH]; simpl; [reflexivity|].
  rewrite sql_value_eqb_refl, IH; reflexivity.
Qed.

Definition cs_slot (n : nat) : AccSlot :=
  mkAccSlot (Z.of_nat n) (Z.of_nat n) true.

Section Q21GroupCountSim.
Variable kf : tuple TNull -> list NullValues.value.
Variable kr : Row -> list SqlValue.
Variable R : tuple TNull -> Row -> Prop.
Variable gkeys : list Expr.
Variable p : Params.

Hypothesis Hkr : forall a b, R a b -> eval_exprs gkeys p (EvRow b) = Some (kr b).
Hypothesis Heq :
  forall a b x y, R a b -> R x y ->
    Oset.eq_bool (mk_olists (Tuple.OVal TNull)) (kf a) (kf x) =
    list_sql_value_eqb (kr b) (kr y).

Definition q21_entry_rel
    (e1 : list NullValues.value * list (tuple TNull))
    (e2 : list SqlValue * AggAcc) : Prop :=
  let (vf, g) := e1 in
  let (vr, acc) := e2 in
  g <> nil /\ acc = [cs_slot (List.length g)] /\
  List.Forall (fun x => kf x = vf) g /\
  List.Forall (fun x => exists y, R x y /\ kr y = vr) g.

Definition q21_cs_inc (acc : AggAcc) : AggAcc :=
  match acc with
  | [sl] => [mkAccSlot (acc_sum sl + 1) (acc_count sl + 1) true]
  | _ => acc
  end.

Lemma acc_add_row_count_star :
  forall r sl,
    acc_add_row [AggCountStarSpec] p r [sl] = Some (q21_cs_inc [sl]).
Proof. intros r sl; reflexivity. Qed.

Lemma q21_sim_insert :
  forall pf st a b,
    List.Forall2 q21_entry_rel pf st ->
    R a b ->
    List.Forall2 q21_entry_rel
      (Partition.insert_in_partition (mk_olists (Tuple.OVal TNull)) (kf a) a pf)
      (store_update st (kr b)
         (q21_cs_inc (match store_find st (kr b) with
                      | Some acc => acc
                      | None => empty_acc 1
                      end))).
Proof.
  intros pf st a b HF Hab.
  induction HF as [|[vf g] [vr acc] pf st He HF IH];
    cbn [Partition.insert_in_partition store_find store_update].
  - constructor; [|constructor].
    simpl; repeat split.
    + discriminate.
    + constructor; [reflexivity|constructor].
    + constructor; [exists b; split; [exact Hab|reflexivity]|constructor].
  - destruct He as [Hne [Hacc [Hkey Hpart]]].
    destruct g as [|x0 g0]; [contradiction Hne; reflexivity|].
    inversion Hkey as [|? ? Hk0 _]; subst.
    inversion Hpart as [|? ? [y0 [Hxy0 Hky0]] _]; subst.
    rewrite (Heq a b x0 y0 Hab Hxy0).
    destruct (list_sql_value_eqb (kr b) (kr y0)) eqn:E.
    + apply list_sql_value_eqb_true_eq in E.
      constructor; [|exact HF].
      simpl; repeat split.
      * discriminate.
      * unfold cs_slot; simpl; f_equal; f_equal; lia.
      * constructor; [|exact Hkey].
        pose proof (Heq a b x0 y0 Hab Hxy0) as E'.
        rewrite E, list_sql_value_eqb_refl in E'.
        apply Oset.eq_bool_true_iff in E'; exact E'.
      * constructor; [exists b; split; [exact Hab|reflexivity]|].
        eapply Forall_impl; [|exact Hpart].
        intros x [y [Hxy Hky]]; exists y; split; [exact Hxy|rewrite Hky, E; reflexivity].
    + constructor; [|exact IH].
      simpl; repeat split; assumption.
Qed.

Lemma q21_store_find_cases :
  forall pf st k,
    List.Forall2 q21_entry_rel pf st ->
    store_find st k = None \/ exists n, store_find st k = Some [cs_slot n].
Proof.
  intros pf st k HF.
  induction HF as [|[vf g] [vr acc] pf st He HF IH]; cbn [store_find]; [left; reflexivity|].
  destruct He as [_ [Hacc _]].
  destruct (list_sql_value_eqb k vr); [right; exists (List.length g); rewrite Hacc; reflexivity|].
  exact IH.
Qed.

Lemma q21_store_add_row_eq :
  forall pf st a b,
    List.Forall2 q21_entry_rel pf st ->
    R a b ->
    store_add_row [AggCountStarSpec] gkeys p b st =
    Some (store_update st (kr b)
            (q21_cs_inc (match store_find st (kr b) with
                         | Some acc => acc
                         | None => empty_acc 1
                         end))).
Proof.
  intros pf st a b HF Hab.
  unfold store_add_row; rewrite (Hkr a b Hab).
  destruct (q21_store_find_cases pf st (kr b) HF) as [Hn|[n Hs]];
    [rewrite Hn|rewrite Hs]; reflexivity.
Qed.

Lemma q21_sim_fold :
  forall lf lr pf st,
    List.Forall2 R lf lr ->
    List.Forall2 q21_entry_rel pf st ->
    exists st',
      fold_groups [AggCountStarSpec] gkeys p lr st = Some st' /\
      List.Forall2 q21_entry_rel
        (Partition.partition_rec (mk_olists (Tuple.OVal TNull)) kf pf lf) st'.
Proof.
  intros lf lr pf st HR; revert pf st.
  induction HR as [|a b lf lr Hab HR IH]; intros pf st HF.
  - exists st; split; [reflexivity|exact HF].
  - cbn [fold_groups Partition.partition_rec].
    rewrite (q21_store_add_row_eq pf st a b HF Hab).
    apply IH.
    apply q21_sim_insert; assumption.
Qed.

(** Final aggregate bags line up with FormalSQL groups. *)
Definition q21_group_bag_rel
    (g : list (tuple TNull)) (bag : list SqlValue * list SqlValue) : Prop :=
  let (k, vals) := bag in
  g <> nil /\ vals = [VInt (Z.of_nat (List.length g))] /\
  (exists vf, List.Forall (fun x => kf x = vf) g) /\
  List.Forall (fun x => exists y, R x y /\ kr y = k) g.

Lemma q21_store_to_agg_bags :
  forall pf st,
    List.Forall2 q21_entry_rel pf st ->
    exists bags,
      store_to_agg_bags [AggCountStarSpec] st = Some bags /\
      List.Forall2 q21_group_bag_rel (List.map snd pf) bags.
Proof.
  intros pf st HF.
  induction HF as [|[vf g] [vr acc] pf st He HF IH].
  - exists []; split; [reflexivity|constructor].
  - destruct IH as [bags [Hb HFb]].
    destruct He as [Hne [Hacc [Hkey Hpart]]]; subst acc.
    exists ((vr, [VInt (Z.of_nat (List.length g))]) :: bags); split.
    + cbn [store_to_agg_bags]; rewrite Hb; reflexivity.
    + constructor; [|exact HFb].
      simpl; repeat split; [exact Hne|exists vf; exact Hkey|exact Hpart].
Qed.

(** COUNT-star correspondence: on aligned inputs, RelNode [run_aggregate]
    emits one bag per FormalSQL group, in the same order, whose count is
    exactly the group's size (duplicate rows counted separately). *)
Theorem q21_sim_run_aggregate :
  forall lf lr,
    List.Forall2 R lf lr ->
    List.length gkeys <> 0%nat ->
    exists bags,
      run_aggregate [AggCountStarSpec] gkeys p lr = Some bags /\
      List.Forall2 q21_group_bag_rel
        (List.map snd (Partition.partition (mk_olists (Tuple.OVal TNull)) kf lf))
        bags.
Proof.
  intros lf lr HR Hk.
  destruct (q21_sim_fold lf lr [] [] HR (List.Forall2_nil _))
    as [st [Hfold HF]].
  destruct (q21_store_to_agg_bags _ _ HF) as [bags [Hb HFb]].
  exists bags; split; [|exact HFb].
  unfold run_aggregate; rewrite Hfold.
  destruct (List.length gkeys); [contradiction Hk; reflexivity|].
  simpl; exact Hb.
Qed.

End Q21GroupCountSim.

From FormalSQLLib Require Import GroupsExactFacade GroupBagExactFacade AggregateLeafExactFacade.
From Logos.FormalSQL Require Import AggregateRuntimeFacts.

(** * Part A: exact lowered GROUP BY shape *)

Definition q21_sname_attr : attribute TNull := AttrString "s_name" (StringChar 25).

Definition q21_group_keys : list (@scalar_expr TNull relname ScalarResultValue) :=
  [@SExpr_Leaf TNull relname type_string (DotString "s_name" (StringChar 25))].

Definition q21_group_terms : list (@aggterm TNull) :=
  [DotString "s_name" (StringChar 25)].

Lemma q21_group_keys_terms :
  scalar_group_key_terms q21_group_keys = Some q21_group_terms.
Proof. reflexivity. Qed.

(** The group select list is exactly [s_name ; COUNT-star AS numwait]. *)
Lemma q21_select_list_1_shape :
  Queries.scalar_select_list_1 =
    [(@SExpr_Leaf TNull relname type_string (DotString "s_name" (StringChar 25)),
      AttrString "s_name" (StringChar 25));
     (@SExpr_Leaf TNull relname type_int64 ACountStar, AttrInt64 "numwait")].
Proof. reflexivity. Qed.

Lemma q21_select_list_2_shape :
  Queries.scalar_select_list_2 =
    [(@SExpr_Leaf TNull relname type_string (DotString "s_name" (StringChar 25)),
      AttrString "s_name" (StringChar 25))].
Proof. reflexivity. Qed.

Definition q21_catalog_group_stage (nation_name : string) : QueryExpr :=
  QExpr_Group Queries.scalar_select_list_1 q21_group_keys SExpr_True
    (QExpr_Project Queries.scalar_select_list_2
       (QExpr_Project Queries.scalar_select_list_3
          (q21_catalog_where_stage nation_name))).

Definition q21_group_stage_of (q : QueryExpr) : option QueryExpr :=
  match q with
  | QExpr_Project _ (QExpr_OrderBy _ g) => Some g
  | _ => None
  end.

Theorem Q21_catalog_group_shape :
  forall nation_name,
    q21_group_stage_of (qhat_Q21 nation_name) =
      Some (q21_catalog_group_stage nation_name).
Proof.
  intros nation_name.
  unfold qhat_Q21.
  destruct (String.eqb nation_name catalog_nation_name) eqn:Heq.
  - apply String.eqb_eq in Heq; subst nation_name.
    unfold q21_group_stage_of, Queries.source_query_expr, Queries.shared_query_expr_0.
    simpl.
    unfold q21_catalog_group_stage, q21_group_keys,
      q21_catalog_where_stage, q21_catalog_where, q21_catalog_where_conjuncts,
      q21_where_sites, q21_expected_cross_spine,
      q21_supplier_table_outputs, q21_lineitem_table_outputs,
      q21_orders_table_outputs, q21_nation_table_outputs,
      q21_exists_l2_sexpr, q21_exists_l2_subquery,
      q21_not_exists_l3_sexpr, q21_not_exists_l3_subquery.
    reflexivity.
  - unfold q21_group_stage_of, subst_nation_query,
      Queries.source_query_expr, Queries.shared_query_expr_0.
    simpl.
    unfold subst_nation_sexpr_bool; simpl.
    unfold subst_nation_sexpr_val, subst_nation_aggterm; simpl.
    unfold q21_catalog_group_stage, q21_group_keys,
      q21_catalog_where_stage, q21_catalog_where, q21_catalog_where_conjuncts,
      q21_where_sites, q21_expected_cross_spine,
      q21_supplier_table_outputs, q21_lineitem_table_outputs,
      q21_orders_table_outputs, q21_nation_table_outputs,
      q21_exists_l2_sexpr, q21_exists_l2_subquery,
      q21_not_exists_l3_sexpr, q21_not_exists_l3_subquery.
    reflexivity.
Qed.

(** RelNode aggregate node (subterm of [LogicalSpec_Q21_rel]). *)
Definition q21_agg_node : RelNode :=
  RelAggregate anti_not_exists_l3 q21_group_exprs q21_aggs.

Lemma q21_agg_node_in_spec :
  LogicalSpec_Q21_rel =
    RelSort (RelProject q21_agg_node q21_project_exprs) q21_order_keys /\
  q21_group_exprs = [EInputRef 1%nat] /\ q21_aggs = [AggCountStarSpec].
Proof. repeat split; reflexivity. Qed.

(** * Part B: projections feeding the Group *)

(** [select_3] renames the 36 [__logos_cor_*] columns back to native names;
    every leaf is a Dot. *)
Definition q21_sel3_attrs : list (attribute TNull) :=
  [AttrInt32 "__logos_cor_cor0_0";
   AttrString "__logos_cor_cor0_1" (StringChar 25);
   AttrString "__logos_cor_cor0_2" (StringVarcharN 40);
   AttrInt32 "__logos_cor_cor0_3";
   AttrString "__logos_cor_cor0_4" (StringChar 15);
   AttrDecimal "__logos_cor_cor0_5" 15 2;
   AttrString "__logos_cor_cor0_6" (StringVarcharN 101);
   AttrInt32 "__logos_cor_cor0_7";
   AttrInt32 "__logos_cor_cor0_8";
   AttrInt32 "__logos_cor_cor0_9";
   AttrInt32 "__logos_cor_cor0_10";
   AttrDecimal "__logos_cor_cor0_11" 15 2;
   AttrDecimal "__logos_cor_cor0_12" 15 2;
   AttrDecimal "__logos_cor_cor0_13" 15 2;
   AttrDecimal "__logos_cor_cor0_14" 15 2;
   AttrString "__logos_cor_cor0_15" (StringChar 1);
   AttrString "__logos_cor_cor0_16" (StringChar 1);
   AttrDate "__logos_cor_cor0_17";
   AttrDate "__logos_cor_cor0_18";
   AttrDate "__logos_cor_cor0_19";
   AttrString "__logos_cor_cor0_20" (StringChar 25);
   AttrString "__logos_cor_cor0_21" (StringChar 10);
   AttrString "__logos_cor_cor0_22" (StringVarcharN 44);
   AttrInt32 "__logos_cor_cor0_23";
   AttrInt32 "__logos_cor_cor0_24";
   AttrString "__logos_cor_cor0_25" (StringChar 1);
   AttrDecimal "__logos_cor_cor0_26" 15 2;
   AttrDate "__logos_cor_cor0_27";
   AttrString "__logos_cor_cor0_28" (StringChar 15);
   AttrString "__logos_cor_cor0_29" (StringChar 15);
   AttrInt32 "__logos_cor_cor0_30";
   AttrString "__logos_cor_cor0_31" (StringVarcharN 79);
   AttrInt32 "__logos_cor_cor0_32";
   AttrString "__logos_cor_cor0_33" (StringChar 25);
   AttrInt32 "__logos_cor_cor0_34";
   AttrString "__logos_cor_cor0_35" (StringVarcharN 152)].

Definition q21_sel3_values (ft : tuple TNull) : list value :=
  map (dot TNull ft) q21_sel3_attrs.

Definition q21_row3 (ft : tuple TNull) : tuple TNull :=
  project_row Queries.scalar_select_list_3 (q21_sel3_values ft).

Definition q21_cor_sname_attr : attribute TNull :=
  AttrString "__logos_cor_cor0_1" (StringChar 25).

(** Group input row: [select_2] keeps only [s_name]. *)
Definition q21_gin (ft : tuple TNull) : tuple TNull :=
  project_row Queries.scalar_select_list_2 [dot TNull ft q21_cor_sname_attr].

Section Q21GroupInputProject.
Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Lemma q21_sel3_exact :
  forall env ft,
    labels TNull ft =S=
      Fset.mk_set (A TNull) (scalar_select_outputs Queries.scalar_select_list_4) ->
    @scalar_value_list_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule
      (Env.env_t TNull env ft) (map fst Queries.scalar_select_list_3)
      (q21_sel3_values ft).
Proof.
  intros env ft Hl.
  unfold q21_sel3_values, q21_sel3_attrs, Queries.scalar_select_list_3.
  cbn [map fst].
  repeat (eapply scalar_value_list_exact_cons;
          [apply tnull_leaf_dot_exact; apply interp_dot_env_t;
           q21_attr_inS_of_labels Hl|]).
  apply scalar_value_list_exact_nil.
Qed.

End Q21GroupInputProject.

Lemma q21_row3_dot_sname :
  forall ft, dot TNull (q21_row3 ft) q21_sname_attr = dot TNull ft q21_cor_sname_attr.
Proof.
  intro ft.
  unfold q21_row3, project_row, scalar_select_outputs, q21_sel3_values,
    q21_sel3_attrs, Queries.scalar_select_list_3, q21_sname_attr, q21_cor_sname_attr.
  cbn [map snd fst combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q21_gin_dot_sname :
  forall ft, dot TNull (q21_gin ft) q21_sname_attr = dot TNull ft q21_cor_sname_attr.
Proof.
  intro ft.
  unfold q21_gin, project_row, scalar_select_outputs,
    Queries.scalar_select_list_2, Queries.scalar_select_list_1, q21_sname_attr.
  cbn [map snd fst combine firstn].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q21_row3_sname_inS :
  forall ft, q21_sname_attr inS labels TNull (q21_row3 ft).
Proof.
  intro ft; unfold q21_row3.
  rewrite (Fset.mem_eq_2 _ _ _ (q21_project_row_labels _ _)).
  vm_compute; reflexivity.
Qed.

Lemma q21_gin_sname_inS :
  forall ft, q21_sname_attr inS labels TNull (q21_gin ft).
Proof.
  intro ft; unfold q21_gin.
  rewrite (Fset.mem_eq_2 _ _ _ (q21_project_row_labels _ _)).
  vm_compute; reflexivity.
Qed.

Section Q21GroupInputRows.
Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Local Abbreviation eval_query :=
  (@eval_query_expr_outcome TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule).

(** Project(select_2) ∘ Project(select_3) over the WHERE output. *)
Lemma q21_group_input_success :
  forall input fts,
    eval_query nil input (SqlSuccess fts) ->
    List.Forall (fun ft => labels TNull ft =S=
      Fset.mk_set (A TNull) (scalar_select_outputs Queries.scalar_select_list_4)) fts ->
    eval_query nil
      (QExpr_Project Queries.scalar_select_list_2
         (QExpr_Project Queries.scalar_select_list_3 input))
      (SqlSuccess (List.map q21_gin fts)).
Proof.
  intros input fts Hin Hlabs.
  assert (Hmap : List.map q21_gin fts =
                 List.map (fun r3 => project_row Queries.scalar_select_list_2
                                       [dot TNull r3 q21_sname_attr])
                   (List.map q21_row3 fts)).
  { rewrite List.map_map; apply List.map_ext; intro ft.
    unfold q21_gin; rewrite q21_row3_dot_sname; reflexivity. }
  rewrite Hmap.
  eapply EQuery_ProjectRows.
  - eapply EQuery_ProjectRows; [exact Hin|].
    unfold q21_row3.
    apply (tnull_project_rows_success_of_exact_map basesort instance boolean_schedule).
    intros ft Hft; rewrite Forall_forall in Hlabs.
    apply q21_sel3_exact; exact (Hlabs ft Hft).
  - apply (tnull_project_rows_success_of_exact_map basesort instance boolean_schedule).
    intros r3 Hr3.
    apply in_map_iff in Hr3 as [ft [<- _]].
    eapply scalar_value_list_exact_cons; [|apply scalar_value_list_exact_nil].
    apply tnull_leaf_dot_exact; apply interp_dot_env_t; apply q21_row3_sname_inS.
Qed.

End Q21GroupInputRows.

(** * Part C: group-level exactness *)

Definition q21_group_env (g : list (tuple TNull)) : Env.env TNull :=
  Env.env_g TNull nil (@Env.Group_By TNull q21_group_terms) g.

Definition q21_group_values (g : list (tuple TNull)) : list value :=
  [Interp.interp_dot TNull (q21_group_env g) q21_sname_attr;
   value_int64_checked (Z.of_nat (List.length g))].

Definition q21_count_fits (n : nat) : Prop :=
  int64_result_runtime_error (Z.of_nat n) = None.

Lemma q21_group_keys_runtime_none :
  forall rows,
    @group_keys_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      nil q21_group_terms rows = None.
Proof.
  induction rows as [|row rows IH]; [reflexivity|].
  exact IH.
Qed.

Section Q21GroupExact.
Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Lemma q21_group_filter_map_exact :
  forall g,
    q21_count_fits (List.length g) ->
    @group_filter_map_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule
      nil Queries.scalar_select_list_1 q21_group_terms SExpr_True
      (fun _ => true) q21_group_values g.
Proof.
  intros g Hfit.
  unfold group_filter_map_exact_at.
  split; [|split; [reflexivity|split]].
  - unfold eval_scalar_select_aggregate_runtime_error.
    apply (proj2 (first_runtime_error_none_iff _ _ _)).
    rewrite q21_select_list_1_shape.
    constructor; [reflexivity|].
    constructor; [|constructor].
    simpl fst.
    unfold eval_scalar_expr_aggregate_runtime_error.
    rewrite (tnull_count_star_aggregate_runtime_exact nil q21_group_terms g).
    exact Hfit.
  - exact (tnull_having_true_acceptance_exact basesort instance boolean_schedule _).
  - rewrite q21_select_list_1_shape; cbn [map fst].
    unfold q21_group_values.
    eapply (@scalar_value_list_exact_cons TNull relname basesort instance unknown3 NullValues.interp_scalar_operator_runtime_error NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule).
    + apply tnull_leaf_dot_exact; reflexivity.
    + eapply (@scalar_value_list_exact_cons TNull relname basesort instance unknown3 NullValues.interp_scalar_operator_runtime_error NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule); [|apply (@scalar_value_list_exact_nil TNull relname basesort instance unknown3 NullValues.interp_scalar_operator_runtime_error NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule)].
      apply tnull_count_star_leaf_exact; exact Hfit.
Qed.

End Q21GroupExact.

(** Group-env Dot reads the key of some member of a nonempty group. *)
Lemma q21_group_env_dot_member :
  forall (g : list (tuple TNull)),
    g <> nil ->
    List.Forall (fun row => q21_sname_attr inS labels TNull row) g ->
    exists t, List.In t g /\
      Interp.interp_dot TNull (q21_group_env g) q21_sname_attr =
      dot TNull t q21_sname_attr.
Proof.
  intros g Hne Hfa.
  unfold q21_group_env, Env.env_g.
  rewrite (Interp.interp_dot_unfold TNull (_ :: nil) q21_sname_attr).
  destruct (@ListSort.quicksort _ (OTuple TNull) g) as [|t rest] eqn:Q.
  - assert (Hlen : List.length (@ListSort.quicksort _ (OTuple TNull) g) = List.length g)
      by apply (@ListSort.length_quicksort _ (OTuple TNull)).
    rewrite Q in Hlen; cbn in Hlen.
    destruct g; [contradiction Hne; reflexivity|discriminate].
  - exists t; split.
    + apply (proj2 (@ListSort.In_quicksort _ (OTuple TNull) g t)).
      rewrite Q; left; reflexivity.
    + assert (HinS : q21_sname_attr inS? labels TNull t = true).
      { pose proof (proj2 (@ListSort.In_quicksort _ (OTuple TNull) g t)) as Hin.
        rewrite Q in Hin; specialize (Hin (or_introl eq_refl)).
        apply (proj1 (Forall_forall _ _) Hfa) in Hin; exact Hin. }
      rewrite HinS; reflexivity.
Qed.

(** * Part D: Q21 instance of the COUNT-star simulation *)

Definition q21_kf (t : tuple TNull) : list value :=
  map (fun f => interp_aggterm TNull (Env.env_t TNull nil t) f) q21_group_terms.

Definition q21_kr (rr : Row) : list SqlValue :=
  match nth_error rr 1 with Some v => [v] | None => [] end.

Definition q21_sname_ok (ft : tuple TNull) : Prop :=
  exists s, dot TNull ft q21_cor_sname_attr =
            Value_string (StringValue (StringChar 25) (Some s)) /\
            Char25Canonical s.

(** Aligned FormalSQL Group-input row ↔ RelNode anti-join row. *)
Definition q21_group_align (t : tuple TNull) (rr : Row) : Prop :=
  exists ft, t = q21_gin ft /\ row_correspond slot_map_outer_cor ft rr /\
             q21_sname_ok ft.

(** Group-key correspondence: FormalSQL key of the Group-input row is the
    outer [s_name] (via cor0_1); RelNode key is slot 1; they correspond. *)
Theorem Q21_group_key_correspond :
  forall ft rr s,
    row_correspond slot_map_outer_cor ft rr ->
    dot TNull ft q21_cor_sname_attr = Value_string (StringValue (StringChar 25) (Some s)) ->
    q21_kf (q21_gin ft) = [Value_string (StringValue (StringChar 25) (Some s))] /\
    q21_kr rr = [VText s] /\
    eval_exprs q21_group_exprs (Params_Q21 "") (EvRow rr) = Some [VText s].
Proof.
  intros ft rr s Hrc Hs.
  assert (H1 : nth_error rr 1 = Some (VText s)).
  { destruct Hrc as [Hlen Hdots].
    destruct (nth_error rr 1) as [v|] eqn:E.
    - pose proof (Hdots 1%nat _ v eq_refl E) as Hv.
      change (value_correspond TText (dot TNull ft q21_cor_sname_attr) v) in Hv.
      rewrite Hs in Hv; inversion Hv; reflexivity.
    - apply nth_error_None in E; rewrite Hlen in E; simpl in E; lia. }
  repeat split.
  - assert (K : q21_kf (q21_gin ft) =
      [Interp.interp_dot TNull (Env.env_t TNull nil (q21_gin ft)) q21_sname_attr])
      by reflexivity.
    rewrite K, interp_dot_env_t by apply q21_gin_sname_inS.
    rewrite q21_gin_dot_sname, Hs; reflexivity.
  - unfold q21_kr; rewrite H1; reflexivity.
  - unfold q21_group_exprs, eval_exprs; simpl.
    destruct rr as [|r0 [|r1 rest]]; simpl in H1; try discriminate.
    injection H1 as ->; reflexivity.
Qed.

Lemma q21_group_align_keys :
  forall t rr, q21_group_align t rr ->
    exists s, q21_kf t = [Value_string (StringValue (StringChar 25) (Some s))] /\
              q21_kr rr = [VText s].
Proof.
  intros t rr [ft [-> [Hrc [s [Hs _]]]]].
  destruct (Q21_group_key_correspond ft rr s Hrc Hs) as [A [B _]].
  exists s; split; assumption.
Qed.

Lemma q21_group_align_Hkr :
  forall p t rr, q21_group_align t rr ->
    eval_exprs q21_group_exprs p (EvRow rr) = Some (q21_kr rr).
Proof.
  intros p t rr [ft [-> [Hrc [s [Hs _]]]]].
  destruct (Q21_group_key_correspond ft rr s Hrc Hs) as [_ [B C]].
  rewrite B; exact C.
Qed.

Lemma q21_group_align_Heq :
  forall a b x y,
    q21_group_align a b -> q21_group_align x y ->
    Oset.eq_bool (mk_olists (Tuple.OVal TNull)) (q21_kf a) (q21_kf x) =
    list_sql_value_eqb (q21_kr b) (q21_kr y).
Proof.
  intros a b x y Hab Hxy.
  destruct (q21_group_align_keys _ _ Hab) as [s [Ka Kb]].
  destruct (q21_group_align_keys _ _ Hxy) as [s' [Kx Ky]].
  rewrite Ka, Kb, Kx, Ky; simpl; rewrite Bool.andb_true_r.
  destruct (String.eqb s s') eqn:E.
  - apply String.eqb_eq in E; subst s'.
    apply Oset.eq_bool_refl.
  - match goal with |- ?bb = false => destruct bb eqn:E2; [|reflexivity] end.
    apply Oset.eq_bool_true_iff in E2.
    injection E2 as E3; apply String.eqb_neq in E; contradiction.
Qed.

(** Transport alignment across a permutation of the RelNode side. *)
Lemma q21_Forall2_perm_r :
  forall (X Y : Type) (Rel : X -> Y -> Prop) xs ys ys',
    List.Forall2 Rel xs ys -> Permutation ys ys' ->
    exists xs', Permutation xs xs' /\ List.Forall2 Rel xs' ys'.
Proof.
  intros X Y Rel xs ys ys' HF HP; revert xs HF.
  induction HP as [|y ys ys' HP IH|y1 y2 ys|ys ys' ys'' HP1 IH1 HP2 IH2];
    intros xs HF.
  - exists xs; split; [apply Permutation_refl|exact HF].
  - inversion HF as [|x ? xs0 ? Hxy HF0]; subst.
    destruct (IH xs0 HF0) as [xs' [HPx HFx]].
    exists (x :: xs'); split; [apply perm_skip; exact HPx|constructor; assumption].
  - inversion HF as [|x1 ? xs1 ? H1 HF1]; subst.
    inversion HF1 as [|x2 ? xs2 ? H2 HF2]; subst.
    exists (x2 :: x1 :: xs2); split; [apply perm_swap|repeat constructor; assumption].
  - destruct (IH1 xs HF) as [xs1 [HP1' HF1]].
    destruct (IH2 xs1 HF1) as [xs2 [HP2' HF2]].
    exists xs2; split; [eapply Permutation_trans; eassumption|exact HF2].
Qed.

(** FormalSQL groups are exactly [partition] groups of the key. *)
Lemma q21_query_make_groups_eq :
  forall rows,
    query_make_groups nil rows q21_group_terms =
    map snd (Partition.partition (mk_olists (Tuple.OVal TNull)) q21_kf rows).
Proof. intro rows; reflexivity. Qed.

(** Group size is bounded by the input size (the count bound). *)
Lemma q21_partition_group_length :
  forall rows g,
    List.In g (map snd (Partition.partition (mk_olists (Tuple.OVal TNull)) q21_kf rows)) ->
    (List.length g <= List.length rows)%nat.
Proof.
  intros rows g Hin.
  pose proof (Partition.partition_permut (mk_olists (Tuple.OVal TNull)) q21_kf rows) as HP.
  apply ListPermut._permut_length in HP.
  rewrite HP.
  apply in_map_iff in Hin as [[v g'] [Heq Hin]]; simpl in Heq; subst g'.
  clear HP.
  induction (Partition.partition (mk_olists (Tuple.OVal TNull)) q21_kf rows)
    as [|[v' k] ps IH]; [contradiction|].
  simpl; rewrite List.length_app.
  destruct Hin as [Heq|Hin]; [injection Heq as -> ->; lia|].
  specialize (IH Hin).
  eapply Nat.le_trans; [exact IH|apply Nat.le_add_l].
Qed.

Lemma q21_count_fits_of_le :
  forall n, (Z.of_nat n <= int64_max)%Z -> q21_count_fits n.
Proof.
  intros n Hn.
  unfold q21_count_fits, int64_result_runtime_error, int64_checked.
  destruct (Z.leb_spec0 int64_min (Z.of_nat n)) as [Hlo|Hlo];
    [|exfalso; unfold int64_min in Hlo; lia].
  destruct (Z.leb_spec0 (Z.of_nat n) int64_max) as [Hhi|Hhi]; [reflexivity|lia].
Qed.

Lemma q21_value_int64_checked_fits :
  forall n, q21_count_fits n ->
    exists i, value_int64_checked (Z.of_nat n) = Value_int64 (Some i) /\
              int64_value i = Z.of_nat n.
Proof.
  intros n Hfit.
  unfold q21_count_fits, int64_result_runtime_error in Hfit.
  unfold value_int64_checked.
  destruct (int64_checked (Z.of_nat n)) as [i|] eqn:E; [|discriminate].
  exists i; split; [reflexivity|].
  unfold int64_checked in E.
  destruct (Z.leb_spec0 int64_min (Z.of_nat n)); [|discriminate].
  destruct (Z.leb_spec0 (Z.of_nat n) int64_max); [|discriminate].
  injection E as <-; reflexivity.
Qed.

(** * Part E: composition *)

Lemma q21_filter_length_le :
  forall (X : Type) (f : X -> bool) (l : list X),
    (List.length (List.filter f l) <= List.length l)%nat.
Proof. intros X f l; induction l as [|x l IH]; simpl; [lia|destruct (f x); simpl; lia]. Qed.

Lemma q21_cross_join_rows_length :
  forall l r, List.length (cross_join_rows l r) = (List.length l * List.length r)%nat.
Proof.
  intros l r; unfold cross_join_rows, Row in *; induction l as [|x l IH]; simpl; [reflexivity|].
  rewrite List.length_app, List.length_map, IH; reflexivity.
Qed.

Lemma q21_cross4_rows_length :
  forall ss ls os ns,
    List.length (q21_cross4_rows ss ls os ns) =
    (List.length ss * List.length ls * List.length os * List.length ns)%nat.
Proof. intros; unfold q21_cross4_rows; rewrite !q21_cross_join_rows_length; reflexivity. Qed.

(** Supplier [s_name] carrier from the embed: CHAR(25), non-NULL. *)
Lemma EmbedRow_supplier_sname :
  forall r s,
    row_supplier_char_ok r ->
    EmbedRow_supplier r = Some s ->
    exists x, dot TNull s (Attr_string "s_name" (StringChar 25)) =
              Value_string (StringValue (StringChar 25) (Some x)) /\
              Char25Canonical x.
Proof.
  intros r s Hc Hemb.
  destruct r as [|[] [|[] [|[] [|]]]]; simpl in Hc; try contradiction.
  unfold EmbedRow_supplier in Hemb.
  destruct (int32_checked _); [|discriminate].
  destruct (int32_checked _); [|discriminate].
  injection Hemb as <-.
  rewrite (mk_tuple_lists_dot _ _ _ supplier_attr_inS_s_name).
  exists s0; split; [|exact Hc].
  simpl mk_att.
  change (string_rtrim_spaces (string_take 25 s0))
    with (string_explicit_cast (StringChar 25) s0).
  unfold Char25Canonical in Hc; rewrite Hc; reflexivity.
Qed.

(** Facts on every projected product tuple needed by the Group stage. *)
Lemma q21_P_labels :
  forall ss' ls' os' ns' ft,
    List.In ft (List.map q21_project_select4 (q21_cross4_tuples ss' ls' os' ns')) ->
    labels TNull ft =S=
      Fset.mk_set (A TNull) (scalar_select_outputs Queries.scalar_select_list_4).
Proof.
  intros ss' ls' os' ns' ft Hin.
  apply in_map_iff in Hin as [ft0 [<- _]].
  unfold q21_project_select4; apply q21_project_row_labels.
Qed.

Lemma q21_P_sname :
  forall ss' ls' os' ns' ft,
    List.Forall q21_supplier_member ss' ->
    List.Forall q21_lineitem_member ls' ->
    List.Forall q21_orders_member os' ->
    List.Forall q21_nation_member ns' ->
    List.Forall (fun s => exists x, dot TNull s (Attr_string "s_name" (StringChar 25)) =
                  Value_string (StringValue (StringChar 25) (Some x)) /\
                  Char25Canonical x) ss' ->
    List.In ft (List.map q21_project_select4 (q21_cross4_tuples ss' ls' os' ns')) ->
    q21_sname_ok ft.
Proof.
  intros ss' ls' os' ns' ft Ms Ml Mo Mn Sn Hin.
  apply in_map_iff in Hin as [ft0 [<- Hin0]].
  destruct (q21_In_cross4_tuples _ _ _ _ _ Hin0)
    as [s [l [o [n [Hs0 [Hl0 [Ho0 [Hn0 ->]]]]]]]].
  rewrite Forall_forall in Ms, Ml, Mo, Mn, Sn.
  destruct (Ms s Hs0) as [Hs _]. destruct (Ml l Hl0) as [Hl _].
  destruct (Mo o Ho0) as [Ho _]. destruct (Mn n Hn0) as [Hn _].
  pose proof (q21_labels_join_mk_set _ _ _ _ Hs Hl) as Hsl.
  pose proof (q21_labels_join_mk_set _ _ _ _ Hsl Ho) as Hslo.
  destruct (Sn s Hs0) as [x [Hx Hcx]].
  exists x; split; [|exact Hcx].
  unfold q21_cor_sname_attr.
  change (AttrString "__logos_cor_cor0_1" (StringChar 25))
    with (Attr_string "__logos_cor_cor0_1" (StringChar 25)).
  rewrite q21_select4_dot_s_name.
  q21_dot_path; exact Hx.
Qed.

(** WHERE stage with explicit witnesses and the fixed schedule (reuses the
    Q21WhereFusion lemmas; no WHERE/EXISTS/NOT EXISTS semantics reopened). *)
Lemma q21_where_explicit :
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
    exists fts rows,
      @eval_query_expr_outcome TNull relname
        (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
        (@_instance TNull (D_of_embeds ss' ls' os' ns')) unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error value_is_null
        q21_boolean_schedule nil (q21_catalog_where_stage nation_name)
        (SqlSuccess fts) /\
      Eval_Spec anti_not_exists_l3 (Params_Q21 nation_name) db = Some rows /\
      bag_correspond_unordered slot_map_outer_cor fts rows /\
      (forall ft, List.In ft fts ->
         List.In ft (List.map q21_project_select4 (q21_cross4_tuples ss' ls' os' ns'))) /\
      (List.length rows <= List.length (q21_cross4_rows ss ls os ns))%nat.
Proof.
  intros db nation_name ss ls os ns ss' ls' os' ns'
    Hpre Hs Hl Ho Hn Hes Hel Heo Hen.
  pose proof Hpre as [Hok [Hnn [Hchar Hemb]]].
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
  exists (List.filter (q21_catalog_where_keep nation_name ls') P), rows2.
  split; [|split; [exact E2a|split; [|split]]].
  - unfold q21_catalog_where_stage.
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
  - rewrite Q21_stacked_where_filters_eq; exact B2.
  - intros ft Hin; apply filter_In in Hin as [Hin _]; exact Hin.
  - unfold rows2, rows1, rows0.
    eapply Nat.le_trans; [apply q21_filter_length_le|].
    eapply Nat.le_trans; [apply q21_filter_length_le|].
    apply q21_filter_length_le.
Qed.

(** Grouped output schema: [s_name] ↔ RelNode key, [numwait] ↔ RelNode count. *)
Definition slot_map_group : SlotMap :=
  [mkSlotBinding (AttrString "s_name" (StringChar 25)) (attr "s_name" TText);
   mkSlotBinding (AttrInt64 "numwait") (attr "numwait" TInt)].

Lemma q21_sel1_dots :
  forall a b,
    dot TNull (project_row Queries.scalar_select_list_1 [a; b])
      (AttrString "s_name" (StringChar 25)) = a /\
    dot TNull (project_row Queries.scalar_select_list_1 [a; b])
      (AttrInt64 "numwait") = b.
Proof.
  intros a b; split;
    unfold project_row, scalar_select_outputs, Queries.scalar_select_list_1;
    cbn [map snd fst combine];
    rewrite dot_mk_tuple;
    match goal with
    | |- context [if ?bb then _ else _] =>
        assert (Hb : bb = true) by (vm_compute; reflexivity);
        rewrite Hb; clear Hb
    end;
    cbn; reflexivity.
Qed.

(** Grouped-row correspondence from the COUNT-star simulation. *)
Theorem Q21_group_rows_correspond :
  forall groups bags,
    List.Forall2 (q21_group_bag_rel q21_kf q21_kr q21_group_align) groups bags ->
    (forall g, List.In g groups -> q21_count_fits (List.length g)) ->
    List.Forall2 (row_correspond slot_map_group)
      (List.map (fun g => project_row Queries.scalar_select_list_1 (q21_group_values g))
         groups)
      (agg_bags_to_rows bags).
Proof.
  intros groups bags HF Hfit.
  induction HF as [|g [k vals] groups bags Hgb HF IH]; [constructor|].
  cbn [List.map agg_bags_to_rows].
  constructor; [|apply IH; intros g' Hg'; apply Hfit; right; exact Hg'].
  destruct Hgb as [Hne [Hvals [[vf Hkey] Hpart]]]; subst vals.
  destruct g as [|x0 g0]; [contradiction Hne; reflexivity|].
  inversion Hpart as [|? ? [y0 [Hal0 Hky0]] _]; subst.
  destruct (q21_group_align_keys _ _ Hal0) as [s0 [Kx0 Ky0]].
  rewrite Ky0.
  (* the group-env Dot reads some member, whose key equals x0's *)
  assert (Hlab : List.Forall (fun row => q21_sname_attr inS labels TNull row) (x0 :: g0)).
  { eapply Forall_impl; [|exact Hpart].
    intros x [y [[ft [-> _]] _]]; apply q21_gin_sname_inS. }
  destruct (q21_group_env_dot_member (x0 :: g0) Hne Hlab) as [t [Ht Hdot]].
  assert (Hkt : q21_kf t = q21_kf x0).
  { rewrite Forall_forall in Hkey; rewrite (Hkey t Ht), (Hkey x0 (or_introl eq_refl)); reflexivity. }
  assert (Hsv : Interp.interp_dot TNull (q21_group_env (x0 :: g0)) q21_sname_attr =
                Value_string (StringValue (StringChar 25) (Some s0))).
  { rewrite Hdot.
    rewrite Forall_forall in Hpart.
    destruct (Hpart t Ht) as [yt [[ftt [-> [_ _]]] _]].
    assert (K : q21_kf (q21_gin ftt) =
      [Interp.interp_dot TNull (Env.env_t TNull nil (q21_gin ftt)) q21_sname_attr])
      by reflexivity.
    rewrite K, interp_dot_env_t in Hkt by apply q21_gin_sname_inS.
    rewrite Kx0 in Hkt; injection Hkt as Hkt; exact Hkt. }
  destruct (q21_value_int64_checked_fits _ (Hfit _ (or_introl eq_refl))) as [i [Hi Hiv]].
  destruct (q21_sel1_dots
              (Interp.interp_dot TNull (q21_group_env (x0 :: g0)) q21_sname_attr)
              (value_int64_checked (Z.of_nat (List.length (x0 :: g0)))))
    as [D1 D2].
  assert (E1 : dot TNull (project_row Queries.scalar_select_list_1 (q21_group_values (x0 :: g0)))
                 (AttrString "s_name" (StringChar 25)) =
               Value_string (StringValue (StringChar 25) (Some s0)))
    by (unfold q21_group_values; etransitivity; [exact D1|exact Hsv]).
  assert (E2 : dot TNull (project_row Queries.scalar_select_list_1 (q21_group_values (x0 :: g0)))
                 (AttrInt64 "numwait") = Value_int64 (Some i))
    by (unfold q21_group_values; etransitivity; [exact D2|exact Hi]).
  split; [reflexivity|].
  intros i0 sb v Hsb Hv.
  destruct i0 as [|[|i0]]; cbn [nth_error] in Hsb, Hv.
  - injection Hsb as <-; injection Hv as <-.
    change (value_correspond TText
      (dot TNull (project_row Queries.scalar_select_list_1 (q21_group_values (x0 :: g0)))
         (AttrString "s_name" (StringChar 25))) (VText s0)).
    rewrite E1; apply VC_text.
  - injection Hsb as <-; injection Hv as <-.
    change (value_correspond TInt
      (dot TNull (project_row Queries.scalar_select_list_1 (q21_group_values (x0 :: g0)))
         (AttrInt64 "numwait")) (VInt (Z.of_nat (List.length (x0 :: g0))))).
    rewrite E2, <- Hiv; apply VC_int64.
  - destruct i0; discriminate Hsb.
Qed.

(** Semantically required premise: FormalSQL COUNT-star is int64 and raises
    an out-of-range error on overflow, while RelNode counts in Z.  Bounding the
    four-way product bounds every group (group ⊆ WHERE output ⊆ product). *)
Definition Q21CountSafe (db : DB) : Prop :=
  forall ss ls os ns,
    db_lookup db table_supplier = Some ss ->
    db_lookup db table_lineitem = Some ls ->
    db_lookup db table_orders = Some os ->
    db_lookup db table_nation = Some ns ->
    (Z.of_nat (List.length ss * List.length ls * List.length os * List.length ns)
       <= int64_max)%Z.

Lemma eval_rel_relantijoin_rows :
  forall l r c p db v,
    eval_rel (RelAntiJoin l r c) p db = Some v -> exists rs, v = RVRows rs.
Proof.
  intros l r c p db v H; cbn [eval_rel] in H.
  destruct (eval_rel l p db) as [[ls|]|], (eval_rel r p db) as [[rs|]|];
    try discriminate.
  destruct (join_anti_rows c p ls rs); try discriminate.
  injection H as <-; eauto.
Qed.

Lemma q21_align_of_forall2 :
  forall fts rows,
    List.Forall2 (row_correspond slot_map_outer_cor) fts rows ->
    (forall ft, List.In ft fts -> q21_sname_ok ft) ->
    List.Forall2 q21_group_align (List.map q21_gin fts) rows.
Proof.
  intros fts rows HF Hs.
  induction HF as [|ft rr fts rows Hrc HF IH]; [constructor|].
  simpl; constructor.
  - exists ft; split; [reflexivity|split; [exact Hrc|apply Hs; left; reflexivity]].
  - apply IH; intros ft' Hft'; apply Hs; right; exact Hft'.
Qed.

(** Output-row facts exported for the final Project / ORDER BY. *)
Definition q21_group_out_shape (gt : tuple TNull) : Prop :=
  labels TNull gt =S=
    Fset.mk_set (A TNull) (scalar_select_outputs Queries.scalar_select_list_1) /\
  (exists s, dot TNull gt (AttrString "s_name" (StringChar 25)) =
             Value_string (StringValue (StringChar 25) (Some s)) /\ Char25Canonical s) /\
  (exists i, dot TNull gt (AttrInt64 "numwait") = Value_int64 (Some i)).

Definition q21_bag_shape (bag : list SqlValue * list SqlValue) : Prop :=
  exists s n, fst bag = [VText s] /\ snd bag = [VInt n].

Lemma q21_Forall2_right :
  forall (X Y : Type) (Rel : X -> Y -> Prop) (P : Y -> Prop) xs ys,
    List.Forall2 Rel xs ys -> (forall x y, Rel x y -> P y) -> List.Forall P ys.
Proof. intros X Y Rel P xs ys HF H; induction HF; constructor; eauto. Qed.

Definition Q21_GroupCountStageCorrespond_statement : Prop :=
  forall (db : DB) (nation_name : string),
    Preconditions_Q21 db nation_name ->
    Q21CountSafe db ->
    exists (D : db_state) (gts : list (tuple TNull))
           (bags : list (list SqlValue * list SqlValue)),
      EmbedDB_Q21 db = Some D /\
      q21_group_stage_of (qhat_Q21 nation_name) =
        Some (q21_catalog_group_stage nation_name) /\
      FormalSQL_exec D (q21_catalog_group_stage nation_name) gts /\
      eval_rel q21_agg_node (Params_Q21 nation_name) db = Some (RVAgg bags) /\
      Eval_Spec q21_agg_node (Params_Q21 nation_name) db =
        Some (agg_bags_to_rows bags) /\
      bag_correspond_unordered slot_map_group gts (agg_bags_to_rows bags).

Definition Q21_GroupCountStageCorrespond_strong_statement : Prop :=
  forall (db : DB) (nation_name : string),
    Preconditions_Q21 db nation_name ->
    Q21CountSafe db ->
    exists (D : db_state) (gts : list (tuple TNull))
           (bags : list (list SqlValue * list SqlValue)),
      EmbedDB_Q21 db = Some D /\
      q21_group_stage_of (qhat_Q21 nation_name) =
        Some (q21_catalog_group_stage nation_name) /\
      FormalSQL_exec D (q21_catalog_group_stage nation_name) gts /\
      eval_rel q21_agg_node (Params_Q21 nation_name) db = Some (RVAgg bags) /\
      Eval_Spec q21_agg_node (Params_Q21 nation_name) db =
        Some (agg_bags_to_rows bags) /\
      bag_correspond_unordered slot_map_group gts (agg_bags_to_rows bags) /\
      List.Forall q21_group_out_shape gts /\
      List.Forall q21_bag_shape bags.

Theorem Q21_GroupCountStageCorrespond_strong :
  Q21_GroupCountStageCorrespond_strong_statement.
Proof.
  unfold Q21_GroupCountStageCorrespond_strong_statement.
  intros db nation_name Hpre Hsafe.
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
  unfold DB_CharCanonical_Q21 in Hchar; rewrite Hs, Ho, Hn in Hchar.
  destruct Hchar as [Cs [Co Cn]].
  pose proof (embed_rows_Forall_of EmbedRow_supplier (fun _ => True)
    q21_supplier_member ss ss'
    (fun r ft _ He => EmbedRow_supplier_member r ft He)
    (proj2 (Forall_forall (fun _ => True) ss) (fun _ _ => I)) Hes) as Ms.
  pose proof (embed_rows_Forall_of EmbedRow_supplier row_supplier_char_ok
    (fun s => exists x, dot TNull s (Attr_string "s_name" (StringChar 25)) =
                  Value_string (StringValue (StringChar 25) (Some x)) /\
                  Char25Canonical x) ss ss'
    EmbedRow_supplier_sname Cs Hes) as Sn.
  pose proof (embed_rows_Forall_of EmbedRow_lineitem (fun _ => True)
    q21_lineitem_member ls ls'
    (fun r ft _ He => EmbedRow_lineitem_member r ft He)
    (proj2 (Forall_forall (fun _ => True) ls) (fun _ _ => I)) Hel) as Ml.
  pose proof (embed_rows_Forall_of EmbedRow_orders row_orders_char_ok
    q21_orders_member os os' EmbedRow_orders_member Co Heo) as Mo.
  pose proof (embed_rows_Forall_of EmbedRow_nation row_nation_char_ok
    q21_nation_member ns ns' EmbedRow_nation_member Cn Hen) as Mn.
  destruct (q21_where_explicit db nation_name ss ls os ns ss' ls' os' ns'
              Hpre Hs Hl Ho Hn Hes Hel Heo Hen)
    as [fts [rows [Hw [He [Hb [Hsub Hlen]]]]]].
  (* align FormalSQL outer rows with the RelNode anti-join order *)
  destruct Hb as [rrs' [HP HF]].
  destruct (q21_Forall2_perm_r _ _ _ fts rrs' rows HF (Permutation_sym HP))
    as [fts' [HPf HF']].
  set (rep := List.map q21_gin fts').
  assert (Halign : List.Forall2 q21_group_align rep rows).
  { apply q21_align_of_forall2; [exact HF'|].
    intros ft Hft.
    eapply q21_P_sname; [exact Ms|exact Ml|exact Mo|exact Mn|exact Sn|].
    apply Hsub; exact (Permutation_in ft (Permutation_sym HPf) Hft). }
  destruct (q21_sim_run_aggregate q21_kf q21_kr q21_group_align q21_group_exprs
              (Params_Q21 nation_name)
              (fun a b H => q21_group_align_Hkr _ a b H)
              q21_group_align_Heq rep rows Halign ltac:(discriminate))
    as [bags [Hrun Hrel]].
  set (groups := query_make_groups nil rep q21_group_terms).
  assert (Hgroups : groups =
    List.map snd (Partition.partition (mk_olists (Tuple.OVal TNull)) q21_kf rep))
    by apply q21_query_make_groups_eq.
  assert (Hfit : forall g, List.In g groups -> q21_count_fits (List.length g)).
  { intros g Hg; rewrite Hgroups in Hg.
    apply q21_count_fits_of_le.
    pose proof (q21_partition_group_length rep g Hg) as H1.
    pose proof (List.Forall2_length Halign) as H2.
    pose proof (Hsafe ss ls os ns Hs Hl Ho Hn) as H3.
    rewrite q21_cross4_rows_length in Hlen.
    unfold rep in H1, H2; rewrite List.length_map in H1, H2.
    assert (Hle : (List.length g <=
                   List.length ss * List.length ls * List.length os * List.length ns)%nat)
      by (eapply Nat.le_trans; [exact H1|]; rewrite H2; exact Hlen).
    eapply Z.le_trans; [apply Nat2Z.inj_le; exact Hle|exact H3]. }
  set (gts := List.map (fun g => project_row Queries.scalar_select_list_1
                                   (q21_group_values g)) groups).
  assert (Hanti : eval_rel anti_not_exists_l3 (Params_Q21 nation_name) db =
                  Some (RVRows rows)).
  { unfold Eval_Spec in He.
    destruct (eval_rel anti_not_exists_l3 (Params_Q21 nation_name) db) as [v|] eqn:E;
      [|discriminate].
    destruct (eval_rel_relantijoin_rows _ _ _ _ _ _ E) as [rs ->].
    injection He as ->; reflexivity. }
  assert (Hagg : eval_rel q21_agg_node (Params_Q21 nation_name) db = Some (RVAgg bags)).
  { unfold q21_agg_node; cbn [eval_rel]; rewrite Hanti.
    change q21_aggs with [AggCountStarSpec]; rewrite Hrun; reflexivity. }
  exists (D_of_embeds ss' ls' os' ns'), gts, bags.
  split; [reflexivity|].
  split; [exact (Q21_catalog_group_shape nation_name)|].
  split; [|split; [exact Hagg|split; [|split; [|split]]]].
  - (* FormalSQL: Group over Project select_2 ∘ Project select_3 ∘ WHERE *)
    unfold FormalSQL_exec, eval_query_expr_possible_outcome.
    exists q21_boolean_schedule.
    unfold q21_catalog_group_stage.
    eapply EQuery_GroupBagSuccess.
    + apply q21_group_input_success; [exact Hw|].
      apply Forall_forall; intros ft Hft; apply (q21_P_labels ss' ls' os' ns'); apply Hsub; exact Hft.
    + apply (tnull_group_bag_success_of_exact_map_having_true
               (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
               (@_instance TNull (D_of_embeds ss' ls' os' ns'))
               q21_boolean_schedule nil Queries.scalar_select_list_1
               q21_group_keys q21_group_terms
               (query_rows_bag (List.map q21_gin fts)) rep q21_group_values).
      * exact q21_group_keys_terms.
      * apply SqlListFacts.permutation_same_rows_as_bag; unfold rep; apply Permutation_map; exact HPf.
      * apply q21_group_keys_runtime_none.
      * intros g Hg.
        apply (q21_group_filter_map_exact
                 (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
                 (@_instance TNull (D_of_embeds ss' ls' os' ns'))
                 q21_boolean_schedule g (Hfit g Hg)).
    + apply SqlListFacts.permutation_same_rows_as_bag; apply Permutation_refl.
  - unfold Eval_Spec; rewrite Hagg; reflexivity.
  - exists (agg_bags_to_rows bags); split; [apply Permutation_refl|].
    unfold gts; rewrite Hgroups in Hfit |- *.
    apply Q21_group_rows_correspond; [exact Hrel|exact Hfit].
  - (* output-row shapes *)
    assert (Hrep : forall x, List.In x rep ->
              exists ft, x = q21_gin ft /\ q21_sname_ok ft).
    { intros x Hx; unfold rep in Hx; apply in_map_iff in Hx as [ft [<- Hft]].
      exists ft; split; [reflexivity|].
      eapply q21_P_sname; [exact Ms|exact Ml|exact Mo|exact Mn|exact Sn|].
      apply Hsub; exact (Permutation_in ft (Permutation_sym HPf) Hft). }
    apply Forall_forall; intros gt Hgt; unfold gts in Hgt.
    apply in_map_iff in Hgt as [g [<- Hg]].
    assert (Hterms : q21_group_terms <> nil) by (unfold q21_group_terms; discriminate).
    assert (Hne : g <> nil)
      by exact (query_make_groups_member_nonempty nil rep q21_group_terms g Hterms Hg).
    assert (Hmem : forall x, List.In x g -> List.In x rep)
      by (intros x Hx; eapply query_make_groups_member_in; eassumption).
    assert (Hlab : List.Forall (fun row => q21_sname_attr inS labels TNull row) g).
    { apply Forall_forall; intros x Hx.
      destruct (Hrep x (Hmem x Hx)) as [ft [-> _]]; apply q21_gin_sname_inS. }
    destruct (q21_group_env_dot_member g Hne Hlab) as [t [Ht Hdot]].
    destruct (Hrep t (Hmem t Ht)) as [ft [-> [s0 [Hs0 Hc0]]]].
    destruct (q21_sel1_dots
                (Interp.interp_dot TNull (q21_group_env g) q21_sname_attr)
                (value_int64_checked (Z.of_nat (List.length g)))) as [D1 D2].
    destruct (q21_value_int64_checked_fits _ (Hfit g Hg)) as [i [Hi _]].
    split; [unfold q21_group_values; apply q21_project_row_labels|split].
    + exists s0; split; [|exact Hc0].
      unfold q21_group_values; etransitivity; [exact D1|].
      rewrite Hdot, q21_gin_dot_sname; exact Hs0.
    + exists i; unfold q21_group_values; etransitivity; [exact D2|exact Hi].
  - (* aggregate bag shapes *)
    eapply q21_Forall2_right; [exact Hrel|].
    intros g [k vals] [Hne [Hvals [_ Hpart]]]; subst vals.
    destruct g as [|x0 g0]; [contradiction Hne; reflexivity|].
    inversion Hpart as [|? ? [y0 [Hal0 Hky0]] _]; subst.
    destruct (q21_group_align_keys _ _ Hal0) as [s0 [_ Ky0]].
    exists s0, (Z.of_nat (List.length (x0 :: g0))); split; [exact Ky0|reflexivity].
Qed.

Theorem Q21_GroupCountStageCorrespond :
  Q21_GroupCountStageCorrespond_statement.
Proof.
  intros db nation_name Hpre Hsafe.
  destruct (Q21_GroupCountStageCorrespond_strong db nation_name Hpre Hsafe)
    as [D [gts [bags [H1 [H2 [H3 [H4 [H5 [H6 _]]]]]]]]].
  exists D, gts, bags; repeat split; assumption.
Qed.

(** * COUNT-star = number of input occurrences with the key *)

Lemma q21_filter_permut_length :
  forall (p : tuple TNull -> bool) (l1 l2 : list (tuple TNull)),
    ListPermut._permut (@eq (tuple TNull)) l1 l2 ->
    List.length (List.filter p l1) = List.length (List.filter p l2).
Proof.
  intros p l1 l2 H.
  induction H as [|a b l k1 k2 Hab H IH]; [reflexivity|].
  subst b.
  rewrite List.filter_app in IH |- *; rewrite List.length_app in IH |- *.
  cbn [List.filter]; destruct (p a); cbn [List.length]; lia.
Qed.

Lemma q21_filter_zero_group :
  forall (v v' : list value) (k : list (tuple TNull)),
    v' <> v ->
    (forall x, List.In x k -> q21_kf x = v') ->
    List.filter (fun t => Oset.eq_bool (mk_olists (Tuple.OVal TNull)) (q21_kf t) v) k = [].
Proof.
  intros v v' k Hv' Hk; induction k as [|x k IHk]; [reflexivity|].
  cbn [List.filter]; rewrite (Hk x (or_introl eq_refl)).
  destruct (Oset.eq_bool (mk_olists (Tuple.OVal TNull)) v' v) eqn:E.
  - apply Oset.eq_bool_true_iff in E; contradiction.
  - apply IHk; intros y Hy; apply Hk; right; exact Hy.
Qed.

Lemma q21_filter_zero_rest :
  forall (Q : list (list value * list (tuple TNull))) v,
    (forall v' k, List.In (v', k) Q -> forall x, List.In x k -> q21_kf x = v') ->
    ~ List.In v (List.map fst Q) ->
    List.length
      (List.filter (fun t => Oset.eq_bool (mk_olists (Tuple.OVal TNull)) (q21_kf t) v)
         (List.flat_map (fun x => match x with (_, k) => k end) Q)) = 0%nat.
Proof.
  induction Q as [|[v2 k2] Q IHQ]; intros v Hhom Hnot; [reflexivity|].
  cbn [List.flat_map]; rewrite List.filter_app, List.length_app.
  rewrite (q21_filter_zero_group v v2 k2).
  - cbn [List.length Nat.add]; apply IHQ.
    + intros v' k H x Hx; eapply Hhom; [right; exact H|exact Hx].
    + intro H; apply Hnot; right; exact H.
  - intro E; subst v2; apply Hnot; left; reflexivity.
  - intros x Hx; eapply Hhom; [left; reflexivity|exact Hx].
Qed.

Lemma q21_filter_all_group :
  forall (v : list value) (g : list (tuple TNull)),
    (forall x, List.In x g -> q21_kf x = v) ->
    List.filter (fun t => Oset.eq_bool (mk_olists (Tuple.OVal TNull)) (q21_kf t) v) g = g.
Proof.
  intros v g Hk; induction g as [|x g IHg]; [reflexivity|].
  cbn [List.filter]; rewrite (Hk x (or_introl eq_refl)), Oset.eq_bool_refl, IHg; [reflexivity|].
  intros y Hy; apply Hk; right; exact Hy.
Qed.

Lemma q21_filter_flat_map_partition :
  forall (P : list (list value * list (tuple TNull))) v g,
    (forall v' k, List.In (v', k) P -> forall x, List.In x k -> q21_kf x = v') ->
    ListFacts.all_diff (List.map fst P) ->
    List.In (v, g) P ->
    List.length
      (List.filter (fun t => Oset.eq_bool (mk_olists (Tuple.OVal TNull)) (q21_kf t) v)
         (List.flat_map (fun x => match x with (_, k) => k end) P)) =
    List.length g.
Proof.
  induction P as [|[v1 k1] P IH]; intros v g Hhom Hdiff Hin; [contradiction|].
  apply ListFacts.all_diff_unfold in Hdiff; cbn [List.map fst] in Hdiff.
  destruct Hdiff as [Hne Hdiff'].
  cbn [List.flat_map]; rewrite List.filter_app, List.length_app.
  destruct Hin as [Heq|Hin].
  - injection Heq as -> ->.
    rewrite (q21_filter_all_group v g)
      by (intros x Hx; eapply Hhom; [left; reflexivity|exact Hx]).
    rewrite q21_filter_zero_rest; [lia| |].
    + intros v' k H x Hx; eapply Hhom; [right; exact H|exact Hx].
    + intro H; apply (Hne v H); reflexivity.
  - rewrite (q21_filter_zero_group v v1 k1).
    + cbn [List.length Nat.add]; apply IH; [|exact Hdiff'|exact Hin].
      intros v' k H x Hx; eapply Hhom; [right; exact H|exact Hx].
    + intro E; subst v1; apply (Hne v); [|reflexivity].
      apply in_map_iff; exists (v, g); split; [reflexivity|exact Hin].
    + intros x Hx; eapply Hhom; [left; reflexivity|exact Hx].
Qed.

(** COUNT-star is the number of Group-input occurrences carrying the key
    (duplicate rows counted separately). *)
Theorem Q21_group_count_is_occurrences :
  forall rows v g,
    List.In (v, g) (Partition.partition (mk_olists (Tuple.OVal TNull)) q21_kf rows) ->
    List.length g =
    List.length
      (List.filter (fun t => Oset.eq_bool (mk_olists (Tuple.OVal TNull)) (q21_kf t) v) rows).
Proof.
  intros rows v g Hin.
  rewrite (q21_filter_permut_length _ rows _
             (Partition.partition_permut (mk_olists (Tuple.OVal TNull)) q21_kf rows)).
  symmetry; apply q21_filter_flat_map_partition; [| |exact Hin].
  - apply Partition.partition_homogeneous_values.
  - apply Partition.partition_all_diff_values.
Qed.
