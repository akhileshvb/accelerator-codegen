(* Q21NormalizeSoundFinal — final Project, ORDER BY, and NormalizeSound_Q21.

   FormalSQL (from q̂):  Project select_0 (OrderBy [numwait DESC NULLS FIRST;
                          s_name ASC NULLS LAST] (Group stage))
   RelNode:              RelSort (RelProject q21_agg_node [EGroupRef 0; EAggRef 0])
                                 q21_order_keys

   Results:
     Q21_catalog_top_shape              exact top-level shape of q̂ (every nation)
     q21_project_agg_rows               RelNode final Project
     q21_final_project_success          FormalSQL final Project (exact)
     Q21_text_c_leb_string_compare      text order bridge, proved (no collation TCB)
     q21_final_keys_not_null            ORDER BY keys never NULL
     q21_ordered_pair_of_le             two-key lexicographic order correspondence
     q21_result_row_sort_spec           RelNode sort = sorting permutation
     q21_embed_final                    embedded RelNode rows = FormalSQL tuples
     NormalizeSound_Q21                 fail-closed normalization soundness
     NormalizeSound_Q21_catalog         nation = "SAUDI ARABIA" instance

   Stage 3 out of scope. No Admit/Axiom. *)

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

Require Import Q21OuterOrdinarySpine Q21ExistsL2 Q21NotExistsL3 Q21WhereFusion Q21GroupCount.
From Stdlib Require Import Sorting.Sorted Ascii NArith.
From SQLFS Require SqlListFacts.
From FormalSQLLogical Require Import Agg.

(** * Part A: exact top-level shape (final Project over ORDER BY) *)

Definition q21_fsql_order_keys :=
  [SortDescNullsFirst (AttrInt64 "numwait");
   SortAscNullsLast (AttrString "s_name" (StringChar 25))].

Definition q21_catalog_top (nation_name : string) : QueryExpr :=
  QExpr_Project Queries.scalar_select_list_0
    (QExpr_OrderBy q21_fsql_order_keys (q21_catalog_group_stage nation_name)).

Lemma q21_select_list_0_shape :
  Queries.scalar_select_list_0 =
    [(@SExpr_Leaf TNull relname type_string (DotString "s_name" (StringChar 25)),
      AttrString "__logos_output_0" (StringChar 25));
     (@SExpr_Leaf TNull relname type_int64 (DotInt64 "numwait"),
      AttrInt64 "__logos_output_1")].
Proof. reflexivity. Qed.

Theorem Q21_catalog_top_shape :
  forall nation_name, qhat_Q21 nation_name = q21_catalog_top nation_name.
Proof.
  intros nation_name.
  unfold qhat_Q21.
  destruct (String.eqb nation_name catalog_nation_name) eqn:Heq.
  - apply String.eqb_eq in Heq; subst nation_name.
    unfold Queries.source_query_expr, Queries.shared_query_expr_0.
    unfold q21_catalog_top, q21_fsql_order_keys, q21_catalog_group_stage, q21_group_keys,
      q21_catalog_where_stage, q21_catalog_where, q21_catalog_where_conjuncts,
      q21_where_sites, q21_expected_cross_spine,
      q21_supplier_table_outputs, q21_lineitem_table_outputs,
      q21_orders_table_outputs, q21_nation_table_outputs,
      q21_exists_l2_sexpr, q21_exists_l2_subquery,
      q21_not_exists_l3_sexpr, q21_not_exists_l3_subquery.
    reflexivity.
  - unfold subst_nation_query, Queries.source_query_expr, Queries.shared_query_expr_0.
    simpl.
    unfold subst_nation_sexpr_bool; simpl.
    unfold subst_nation_sexpr_val, subst_nation_aggterm; simpl.
    unfold q21_catalog_top, q21_fsql_order_keys, q21_catalog_group_stage, q21_group_keys,
      q21_catalog_where_stage, q21_catalog_where, q21_catalog_where_conjuncts,
      q21_where_sites, q21_expected_cross_spine,
      q21_supplier_table_outputs, q21_lineitem_table_outputs,
      q21_orders_table_outputs, q21_nation_table_outputs,
      q21_exists_l2_sexpr, q21_exists_l2_subquery,
      q21_not_exists_l3_sexpr, q21_not_exists_l3_subquery.
    reflexivity.
Qed.

(** RelNode: [RelProject q21_agg_node [EGroupRef 0; EAggRef 0]] on
    single-key / single-aggregate bags is exactly [agg_bags_to_rows]. *)
Lemma q21_project_agg_rows :
  forall p bags,
    List.Forall q21_bag_shape bags ->
    project_agg_rows q21_project_exprs p bags = Some (agg_bags_to_rows bags).
Proof.
  intros p bags H; induction H as [|[k v] bags [s [n [Hk Hv]]] H IH]; [reflexivity|].
  simpl in Hk, Hv; subst k v.
  cbn [project_agg_rows]; rewrite IH; reflexivity.
Qed.

(** * Part B: ORDER BY *)

(** ** Text order: RelNode [text_c_leb] vs FormalSQL [string_compare]
    (both byte-lexicographic; proved here, not assumed) *)

Lemma q21_text_c_leb_total :
  forall a b, text_c_leb a b = false -> text_c_leb b a = true.
Proof.
  induction a as [|ca a IH]; intros [|cb b] H; simpl in H |- *; try discriminate;
    try reflexivity.
  rewrite Nat.eqb_sym.
  destruct (Nat.eqb (nat_of_ascii ca) (nat_of_ascii cb)) eqn:E; [apply IH; exact H|].
  apply Nat.eqb_neq in E; apply Nat.ltb_ge in H; apply Nat.ltb_lt; lia.
Qed.

Theorem Q21_text_c_leb_string_compare :
  forall a b, text_c_leb a b = true <-> string_compare a b <> Gt.
Proof.
  induction a as [|ca a IH]; intros [|cb b]; cbn [text_c_leb string_compare].
  - split; [discriminate|reflexivity].
  - split; [discriminate|reflexivity].
  - split; [discriminate|intro H; contradiction H; reflexivity].
  - unfold nat_of_ascii.
    rewrite N2Nat.inj_compare.
    destruct (Nat.compare_spec (N.to_nat (N_of_ascii ca)) (N.to_nat (N_of_ascii cb)))
      as [E|L|G].
    + rewrite E, Nat.eqb_refl; apply IH.
    + assert (Hne : Nat.eqb (N.to_nat (N_of_ascii ca)) (N.to_nat (N_of_ascii cb)) = false)
        by (apply Nat.eqb_neq; lia).
      rewrite Hne; apply Nat.ltb_lt in L; rewrite L.
      split; [discriminate|reflexivity].
    + assert (Hne : Nat.eqb (N.to_nat (N_of_ascii ca)) (N.to_nat (N_of_ascii cb)) = false)
        by (apply Nat.eqb_neq; lia).
      assert (Hlt : Nat.ltb (N.to_nat (N_of_ascii ca)) (N.to_nat (N_of_ascii cb)) = false)
        by (apply Nat.ltb_ge; lia).
      rewrite Hne, Hlt.
      split; [discriminate|intro H; contradiction H; reflexivity].
Qed.

(** ** RelNode order on [s_name, numwait] result rows *)

Definition q21_rel_le (s1 : string) (n1 : Z) (s2 : string) (n2 : Z) : bool :=
  if Z.ltb n2 n1 then true
  else if Z.eqb n1 n2 then text_c_leb s1 s2 else false.

Lemma q21_order_keys_le_eq :
  forall p s1 n1 s2 n2,
    order_keys_le p q21_order_keys [VText s1; VInt n1] [VText s2; VInt n2] =
    Some (q21_rel_le s1 n1 s2 n2).
Proof.
  intros p s1 n1 s2 n2.
  unfold q21_order_keys, q21_rel_le; cbn.
  destruct (Z.leb_spec n2 n1) as [H21|H21]; destruct (Z.leb_spec n1 n2) as [H12|H12].
  - assert (n1 = n2) as <- by lia.
    rewrite Z.ltb_irrefl, Z.eqb_refl.
    destruct (text_c_leb s1 s2) eqn:E1; destruct (text_c_leb s2 s1) eqn:E2; try reflexivity.
    apply q21_text_c_leb_total in E1; congruence.
  - assert (Z.ltb n2 n1 = true) as -> by (apply Z.ltb_lt; lia); reflexivity.
  - assert (Z.ltb n2 n1 = false) as -> by (apply Z.ltb_ge; lia).
    assert (Z.eqb n1 n2 = false) as -> by (apply Z.eqb_neq; lia); reflexivity.
  - lia.
Qed.

Definition q21_result_row (r : Row) : Prop :=
  exists s n, r = [VText s; VInt n].

(** ** Generic: insertion sort is a sorting permutation when the order is
    total on the list's rows. *)
Section Q21RowSort.
Variable p : Params.
Variable oks : list OrderKey.
Variable Rowok : Row -> Prop.
Hypothesis Htot :
  forall a b, Rowok a -> Rowok b ->
    exists c, order_keys_le p oks a b = Some c /\
              (c = false -> order_keys_le p oks b a = Some true).

Definition q21_le (a b : Row) : Prop := order_keys_le p oks a b = Some true.

Lemma q21_row_insert_spec :
  forall x xs,
    Rowok x -> List.Forall Rowok xs -> LocallySorted q21_le xs ->
    exists ys,
      row_insert p oks x xs = Some ys /\ Permutation (x :: xs) ys /\
      LocallySorted q21_le ys /\
      (match ys with
       | [] => False
       | y :: _ => y = x \/ (exists xs', xs = y :: xs')
       end).
Proof.
  intros x xs Hx; induction xs as [|y ys IH]; intros Hall Hs.
  - exists [x]; repeat split; [apply Permutation_refl|constructor|left; reflexivity].
  - inversion Hall as [|? ? Hy Hys]; subst.
    destruct (Htot x y Hx Hy) as [c [Hc Hback]].
    cbn [row_insert]; rewrite Hc.
    destruct c.
    + exists (x :: y :: ys); repeat split.
      * apply Permutation_refl.
      * constructor; [exact Hs|exact Hc].
      * left; reflexivity.
    + assert (Hyx : q21_le y x) by (apply Hback; reflexivity).
      assert (Hsys : LocallySorted q21_le ys)
        by (inversion Hs; subst; [constructor|assumption]).
      destruct (IH Hys Hsys) as [zs [Hz [HPz [HSz Hhd]]]].
      rewrite Hz.
      exists (y :: zs); repeat split.
      * eapply Permutation_trans; [apply perm_swap|apply perm_skip; exact HPz].
      * destruct zs as [|z zs']; [contradiction|].
        constructor; [exact HSz|].
        destruct Hhd as [->|[xs' Heq]]; [exact Hyx|subst ys].
        inversion Hs; subst; assumption.
      * right; exists ys; reflexivity.
Qed.

Lemma q21_result_row_sort_spec :
  forall xs,
    List.Forall Rowok xs ->
    exists ys,
      result_row_sort p oks xs = Some ys /\ Permutation xs ys /\
      LocallySorted q21_le ys.
Proof.
  induction xs as [|x xs IH]; intros Hall.
  - exists []; repeat split; [apply Permutation_refl|constructor].
  - inversion Hall as [|? ? Hx Hxs]; subst.
    destruct (IH Hxs) as [ys [Hys [HP HS]]].
    assert (Hyok : List.Forall Rowok ys).
    { apply Forall_forall; intros y Hy; rewrite Forall_forall in Hxs;
        apply Hxs; exact (Permutation_in y (Permutation_sym HP) Hy). }
    destruct (q21_row_insert_spec x ys Hx Hyok HS) as [zs [Hz [HPz [HSz _]]]].
    exists zs; split; [cbn [result_row_sort]; rewrite Hys; exact Hz|split; [|exact HSz]].
    eapply Permutation_trans; [apply perm_skip; exact HP|exact HPz].
Qed.

End Q21RowSort.

Lemma q21_order_total :
  forall p a b, q21_result_row a -> q21_result_row b ->
    exists c, order_keys_le p q21_order_keys a b = Some c /\
              (c = false -> order_keys_le p q21_order_keys b a = Some true).
Proof.
  intros p a b [s1 [n1 ->]] [s2 [n2 ->]].
  rewrite !q21_order_keys_le_eq.
  exists (q21_rel_le s1 n1 s2 n2); split; [reflexivity|].
  intro H; f_equal; unfold q21_rel_le in *.
  destruct (Z.ltb_spec n2 n1); [discriminate|].
  destruct (Z.eqb_spec n1 n2) as [->|Hne].
  - rewrite Z.ltb_irrefl, Z.eqb_refl; apply q21_text_c_leb_total; exact H.
  - assert (Z.ltb n1 n2 = true) as -> by (apply Z.ltb_lt; lia); reflexivity.
Qed.

(** ** FormalSQL comparator on the grouped output columns *)

Lemma q21_order_compare_int64 :
  forall i1 i2,
    NullValues.sql_order_value_compare (Value_int64 (Some i1)) (Value_int64 (Some i2)) =
    Z.compare (int64_value i1) (int64_value i2).
Proof. intros; reflexivity. Qed.

Lemma q21_order_compare_char25 :
  forall s1 s2,
    NullValues.sql_order_value_compare
      (Value_string (StringValue (StringChar 25) (Some s1)))
      (Value_string (StringValue (StringChar 25) (Some s2))) =
    string_compare s1 s2.
Proof. intros; reflexivity. Qed.

(** ** Corresponding final rows *)

Definition q21_final_corr (o : tuple TNull) (r : Row) : Prop :=
  exists s n i,
    r = [VText s; VInt n] /\
    dot TNull o (AttrString "s_name" (StringChar 25)) =
      Value_string (StringValue (StringChar 25) (Some s)) /\
    Char25Canonical s /\
    dot TNull o (AttrInt64 "numwait") = Value_int64 (Some i) /\
    int64_value i = n /\
    labels TNull o =S=
      Fset.mk_set (A TNull) (scalar_select_outputs Queries.scalar_select_list_1).

Lemma q21_final_corr_of :
  forall o r,
    row_correspond slot_map_group o r -> q21_group_out_shape o -> q21_result_row r ->
    q21_final_corr o r.
Proof.
  intros o r [_ Hdots] [L [[s [Hs Hc]] [i Hi]]] [s' [n ->]].
  pose proof (Hdots 0%nat _ (VText s') eq_refl eq_refl) as H0.
  change (value_correspond TText (dot TNull o (AttrString "s_name" (StringChar 25)))
            (VText s')) in H0.
  pose proof (Hdots 1%nat _ (VInt n) eq_refl eq_refl) as H1.
  change (value_correspond TInt (dot TNull o (AttrInt64 "numwait")) (VInt n)) in H1.
  rewrite Hs in H0; inversion H0; subst s'.
  rewrite Hi in H1; inversion H1; subst n.
  exists s, (int64_value i), i; repeat split; assumption.
Qed.

(** NULL handling: COUNT-star output and the grouped [s_name] are never NULL,
    so neither NULLS FIRST (numwait) nor NULLS LAST (s_name) ever fires. *)
Lemma q21_final_keys_not_null :
  forall o r, q21_final_corr o r ->
    value_is_null (dot TNull o (AttrInt64 "numwait")) = false /\
    value_is_null (dot TNull o (AttrString "s_name" (StringChar 25))) = false.
Proof.
  intros o r [s [n [i [_ [Hs [_ [Hi _]]]]]]].
  rewrite Hs, Hi; split; reflexivity.
Qed.

(** Two-key lexicographic ORDER BY: RelNode ≤ implies FormalSQL ordered pair
    (numwait DESC, then s_name ASC). *)
Lemma q21_ordered_pair_of_le :
  forall p o1 r1 o2 r2,
    q21_final_corr o1 r1 -> q21_final_corr o2 r2 ->
    q21_le p q21_order_keys r1 r2 ->
    @ordered_pair TNull value_is_null q21_fsql_order_keys o1 o2.
Proof.
  intros p o1 r1 o2 r2 [s1 [n1 [i1 [-> [Hs1 [_ [Hi1 [Hv1 _]]]]]]]]
    [s2 [n2 [i2 [-> [Hs2 [_ [Hi2 [Hv2 _]]]]]]]] Hle.
  unfold q21_le in Hle; rewrite q21_order_keys_le_eq in Hle.
  injection Hle as Hle.
  unfold ordered_pair, q21_fsql_order_keys, SortDescNullsFirst, SortAscNullsLast, SortKey.
  cbn [compare_order_keys].
  unfold compare_order_key; cbn [sort_key_attribute sort_key_direction
    sort_key_null_direction sort_key_value_compare].
  rewrite Hs1, Hs2, Hi1, Hi2.
  unfold compare_order_value.
  change (value_is_null (Value_int64 (Some i1))) with false.
  change (value_is_null (Value_int64 (Some i2))) with false.
  change (value_is_null (Value_string (StringValue (StringChar 25) (Some s1)))) with false.
  change (value_is_null (Value_string (StringValue (StringChar 25) (Some s2)))) with false.
  rewrite q21_order_compare_int64, q21_order_compare_char25, Hv1, Hv2.
  unfold q21_rel_le in Hle.
  unfold TNullSyntax.SortDesc, TNullSyntax.SortAsc, TNullSyntax.NullsFirst,
    TNullSyntax.NullsLast.
  destruct (Z.compare_spec n1 n2) as [E|L|G]; cbn [reverse_comparison].
  - rewrite <- E in Hle; rewrite Z.ltb_irrefl, Z.eqb_refl in Hle.
    apply Q21_text_c_leb_string_compare in Hle.
    destruct (string_compare s1 s2); [exact I|exact I|contradiction Hle; reflexivity].
  - assert (Z.ltb n2 n1 = false) as E1 by (apply Z.ltb_ge; lia).
    assert (Z.eqb n1 n2 = false) as E2 by (apply Z.eqb_neq; lia).
    rewrite E1, E2 in Hle; discriminate.
  - exact I.
Qed.

Lemma q21_ordered_rows_of_sorted :
  forall p O R,
    List.Forall2 q21_final_corr O R ->
    LocallySorted (q21_le p q21_order_keys) R ->
    @ordered_rows TNull value_is_null q21_fsql_order_keys O.
Proof.
  intros p O R HF; induction HF as [|o r O R Hc HF IH]; intros Hs; [exact I|].
  destruct HF as [|o2 r2 O' R' Hc2 HF'].
  - exact I.
  - inversion Hs as [| |? ? ? Hs' Hle]; subst.
    split; [eapply q21_ordered_pair_of_le; eassumption|].
    apply IH; exact Hs'.
Qed.

(** ** Final observed tuples = embedded RelNode result rows *)

Definition q21_final_out (o : tuple TNull) : tuple TNull :=
  project_row Queries.scalar_select_list_0
    [dot TNull o (AttrString "s_name" (StringChar 25));
     dot TNull o (AttrInt64 "numwait")].

Lemma q21_int64_checked_value :
  forall i, int64_checked (int64_value i) = Some i.
Proof.
  intros i; destruct (int64_range i) as [Hlo Hhi].
  unfold int64_checked.
  destruct (Z.leb_spec0 int64_min (int64_value i)); [|contradiction].
  destruct (Z.leb_spec0 (int64_value i) int64_max); [|contradiction].
  f_equal; apply int64_ext; reflexivity.
Qed.

Lemma q21_out_tuple_eq :
  forall v1 v2,
    mk_tuple_lists q21_out_attrs [v1; v2] =
    project_row Queries.scalar_select_list_0 [v1; v2].
Proof. intros; reflexivity. Qed.

Lemma q21_embed_final :
  forall O R,
    List.Forall2 q21_final_corr O R ->
    Embed_Q21_Result R = Some (List.map q21_final_out O).
Proof.
  intros O R HF; induction HF as [|o r O R Hc HF IH]; [reflexivity|].
  destruct Hc as [s [n [i [-> [Hs [Hcan [Hi [Hv _]]]]]]]].
  cbn [Embed_Q21_Result List.map]; rewrite IH.
  unfold Embed_Q21_Result_row; rewrite <- Hv, q21_int64_checked_value.
  unfold Char25Canonical in Hcan; rewrite Hcan.
  unfold q21_final_out; rewrite q21_out_tuple_eq.
  do 2 f_equal.
  change (dot TNull o (AttrString "s_name" (StringChar 25))) with
         (dot TNull o (AttrString "s_name" (StringChar 25))) in Hs.
  rewrite Hs, Hi; reflexivity.
Qed.

(** * Part C: NormalizeSound_Q21 *)

Section Q21FinalProject.
Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

(** Final Project: exact at the two dot values of each ordered group row. *)
Lemma q21_final_project_success :
  forall O,
    List.Forall (fun o => labels TNull o =S=
      Fset.mk_set (A TNull) (scalar_select_outputs Queries.scalar_select_list_1)) O ->
    @eval_project_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule
      nil Queries.scalar_select_list_0 O (SqlSuccess (List.map q21_final_out O)).
Proof.
  intros O HL.
  unfold q21_final_out.
  apply (tnull_project_rows_success_of_exact_map basesort instance boolean_schedule).
  intros o Ho; rewrite Forall_forall in HL; pose proof (HL o Ho) as Hl.
  rewrite q21_select_list_0_shape; cbn [map fst].
  eapply (@scalar_value_list_exact_cons TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule).
  - apply tnull_leaf_dot_exact; apply interp_dot_env_t.
    rewrite (Fset.mem_eq_2 _ _ _ Hl); vm_compute; reflexivity.
  - eapply (@scalar_value_list_exact_cons TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule).
    + apply tnull_leaf_dot_exact; apply interp_dot_env_t.
      rewrite (Fset.mem_eq_2 _ _ _ Hl); vm_compute; reflexivity.
    + apply (@scalar_value_list_exact_nil TNull relname basesort instance unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error value_is_null boolean_schedule).
Qed.

End Q21FinalProject.

(** RelNode evaluation of the full LogicalSpec: Sort ∘ Project ∘ Aggregate. *)
Lemma q21_eval_spec_final :
  forall db nation_name bags,
    eval_rel q21_agg_node (Params_Q21 nation_name) db = Some (RVAgg bags) ->
    List.Forall q21_bag_shape bags ->
    Eval_Spec_Q21 nation_name db =
      result_row_sort (Params_Q21 nation_name) q21_order_keys (agg_bags_to_rows bags).
Proof.
  intros db nation_name bags Hagg Hsh.
  unfold Eval_Spec_Q21, Eval_Spec.
  rewrite (proj1 q21_agg_node_in_spec).
  cbn [eval_rel]; rewrite Hagg.
  rewrite q21_project_agg_rows by exact Hsh.
  destruct (result_row_sort _ _ _); reflexivity.
Qed.

Lemma q21_agg_rows_result_rows :
  forall bags,
    List.Forall q21_bag_shape bags ->
    List.Forall q21_result_row (agg_bags_to_rows bags).
Proof.
  intros bags H; induction H as [|[k v] bags [s [n [Hk Hv]]] H IH]; [constructor|].
  simpl in Hk, Hv; subst k v.
  constructor; [exists s, n; reflexivity|exact IH].
Qed.

(** Target shape (fail-closed): a successful RelNode evaluation of the
    canonical LogicalSpec embeds to a possible FormalSQL outcome of q̂. *)
Definition NormalizeSound_Q21_counted_statement : Prop :=
  forall (db : DB) (nation_name : string) (rows : list Row),
    Preconditions_Q21 db nation_name ->
    Q21CountSafe db ->
    Eval_Spec_Q21 nation_name db = Some rows ->
    exists outs D,
      Embed_Q21_Result rows = Some outs /\
      EmbedDB_Q21 db = Some D /\
      FormalSQL_exec D (qhat_Q21 nation_name) outs.

Theorem NormalizeSound_Q21 : NormalizeSound_Q21_counted_statement.
Proof.
  unfold NormalizeSound_Q21_counted_statement.
  intros db nation_name rows Hpre Hsafe Hev.
  destruct (Q21_GroupCountStageCorrespond_strong db nation_name Hpre Hsafe)
    as [D [gts [bags [HD [_ [Hx [Hagg [_ [Hbag [Hshape Hbsh]]]]]]]]]].
  pose proof (q21_agg_rows_result_rows bags Hbsh) as HPRok.
  destruct (q21_result_row_sort_spec (Params_Q21 nation_name) q21_order_keys
              q21_result_row (q21_order_total (Params_Q21 nation_name)) (agg_bags_to_rows bags) HPRok)
    as [SR [Hsort [HPs HLs]]].
  rewrite (q21_eval_spec_final db nation_name bags Hagg Hbsh), Hsort in Hev.
  injection Hev as <-.
  (* align FormalSQL group rows with the RelNode sorted rows *)
  destruct Hbag as [PR' [HP HF]].
  destruct (q21_Forall2_perm_r _ _ _ gts PR' SR HF
              (Permutation_trans (Permutation_sym HP) HPs)) as [O [HPO HFO]].
  assert (Hcorr : List.Forall2 q21_final_corr O SR).
  { assert (HO : forall o, List.In o O -> q21_group_out_shape o).
    { intros o Ho; rewrite Forall_forall in Hshape; apply Hshape.
      exact (Permutation_in o (Permutation_sym HPO) Ho). }
    assert (HR : forall r, List.In r SR -> q21_result_row r).
    { intros r Hr; rewrite Forall_forall in HPRok; apply HPRok.
      exact (Permutation_in r (Permutation_sym HPs) Hr). }
    clear HF HP HPO HPs HLs Hsort.
    induction HFO as [|o r O SR Hrc HFO IH]; [constructor|].
    constructor.
    - apply q21_final_corr_of; [exact Hrc|apply HO; left; reflexivity|apply HR; left; reflexivity].
    - apply IH; intros; [apply HO|apply HR]; right; assumption. }
  exists (List.map q21_final_out O), D.
  split; [exact (q21_embed_final O SR Hcorr)|].
  split; [exact HD|].
  unfold FormalSQL_exec, eval_query_expr_possible_outcome in *.
  destruct Hx as [sched Hg].
  exists sched.
  rewrite Q21_catalog_top_shape; unfold q21_catalog_top.
  eapply EQuery_ProjectRows.
  - eapply EQuery_OrderBySuccess; [exact Hg|].
    split.
    + apply SqlListFacts.permutation_same_rows_as_bag; exact HPO.
    + exact (q21_ordered_rows_of_sorted (Params_Q21 nation_name) O SR Hcorr HLs).
  - apply q21_final_project_success.
    apply Forall_forall; intros o Ho.
    rewrite Forall_forall in Hshape.
    destruct (Hshape o (Permutation_in o (Permutation_sym HPO) Ho)) as [L _]; exact L.
Qed.

(** Catalog instance: nation_name = "SAUDI ARABIA" and q̂ = the catalog query. *)
Corollary NormalizeSound_Q21_catalog :
  forall (db : DB) (rows : list Row),
    Preconditions_Q21 db catalog_nation_name ->
    Q21CountSafe db ->
    Eval_Spec LogicalSpec_Q21_rel params_catalog_nation db = Some rows ->
    exists outs D,
      Embed_Q21_Result rows = Some outs /\
      EmbedDB_Q21 db = Some D /\
      FormalSQL_exec D Queries.source_query_expr outs.
Proof.
  intros db rows Hpre Hsafe Hev.
  rewrite <- Params_Q21_catalog_instance in Hev.
  destruct (NormalizeSound_Q21 db catalog_nation_name rows Hpre Hsafe Hev)
    as [outs [D [He [HD Hx]]]].
  rewrite qhat_Q21_catalog_instance in Hx.
  exists outs, D; repeat split; assumption.
Qed.
