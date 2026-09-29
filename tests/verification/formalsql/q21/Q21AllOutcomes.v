(* Q21AllOutcomes — every possible FormalSQL outcome of q̂ is the SAME success.

   NormalizeSound_Q21 shows the embedded accelerator result is ONE possible
   outcome of q̂ (FormalSQL_exec is existential over Boolean schedules and
   over bag / ORDER BY representatives).  This file closes the gap: under the
   same premises, every outcome under every schedule equals [SqlSuccess outs].

   Route (generic inversion lemmas in lib/PossibleOutcomeDeterminism.v):
     spine / WHERE / projections   outcomes_perm   (Stdlib Permutation)
     GROUP BY s_name + COUNT-star  outcomes_bag    (permut mod =t=)
     final Project over ORDER BY   outcomes_order_project
   ORDER BY (numwait DESC, s_name ASC) is total on the grouped rows because
   s_name is the group key, so the ordered representative is unique
   (pod_sorted_perm_unique on the key list).

   Results:
     Q21_ao_where_input                     spine + select_4 outcomes (perm)
     q21_ao_where / q21_ao_group_input      WHERE and select_3/select_2
     q21_group_values_respect               group values ignore group rep
     q21_ao_group / q21_ao_group_shape      Group stage outcomes (bag) + shape
     Q21_order_final_unique                 ORDER BY + final Project unique
     Q21_qhat_outcomes_unique               any two outcomes of q̂ are equal
     NormalizeSound_Q21_all_outcomes        forall-outcomes NormalizeSound
     NormalizeSound_Q21_catalog_all_outcomes  nation = "SAUDI ARABIA"

   No new premises beyond NormalizeSound_Q21 (Preconditions_Q21, Q21CountSafe).
   Fully proved; no new axioms. *)

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

Require Import Q21OuterOrdinarySpine Q21ExistsL2 Q21NotExistsL3 Q21WhereFusion Q21GroupCount Q21NormalizeSoundFinal.
From FormalSQLLib Require Import PossibleOutcomeDeterminism.
From Stdlib Require Import Sorting.Sorted.
From FormalSQLLogical Require Import Agg.

(** * Part 1: every outcome of the WHERE input (Project select_4 over the
    CrossJoin spine) is a permutation of the projected product P. *)

(** Tuple equivalence transports labels and dots. *)
Lemma q21_teq_labels :
  forall r r' X, Oeset.compare (OTuple TNull) r r' = Eq ->
    labels TNull r =S= X -> labels TNull r' =S= X.
Proof.
  intros r r' X Heq HX.
  rewrite (tuple_eq TNull) in Heq; destruct Heq as [Hl _].
  rewrite <- (Fset.equal_eq_1 _ _ _ _ Hl); exact HX.
Qed.

Lemma q21_teq_dot :
  forall r r' a, Oeset.compare (OTuple TNull) r r' = Eq ->
    a inS labels TNull r -> dot TNull r' a = dot TNull r a.
Proof.
  intros r r' a Heq Ha.
  rewrite (tuple_eq TNull) in Heq; destruct Heq as [_ Hd].
  symmetry; apply Hd; exact Ha.
Qed.

Ltac q21_vals_eq Heq Hlab :=
  repeat first
    [ reflexivity
    | (apply (q21_teq_dot _ _ _ Heq); q21_attr_inS_of_labels Hlab)
    | f_equal ].

Section Q21SpineOutcomes.
Variables ss' ls' os' ns' : list (tuple TNull).
Hypothesis Ls : List.Forall
  (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs) ss'.
Hypothesis Ll : List.Forall
  (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs) ls'.
Hypothesis Lo : List.Forall
  (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs) os'.
Hypothesis Ln : List.Forall
  (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) nation_formal_attrs) ns'.

Local Abbreviation bs := (@_basesort TNull (D_of_embeds ss' ls' os' ns')).
Local Abbreviation ins := (@_instance TNull (D_of_embeds ss' ls' os' ns')).
Local Abbreviation OB := (@outcomes_bag relname bs ins value_is_null).
Local Abbreviation OP := (@outcomes_perm relname bs ins value_is_null).

Lemma q21_ao_p7 :
  OP (QExpr_Project Queries.scalar_select_list_7
        (@QExpr_Table TNull relname q21_supplier_table_outputs (Rel "supplier")))
     (q21_map_project_sel7 ss').
Proof.
  apply (outcomes_bag_project bs ins value_is_null).
  - apply outcomes_bag_table; apply q21_embed_table_same_rows_supplier.
  - intros sched r r' Hr Heq; rewrite Forall_forall in Ls; pose proof (Ls r Hr) as Hl.
    split; [apply q21_sel7_exact; exact (q21_teq_labels _ _ _ Heq Hl)|].
    unfold q21_sel7_values; q21_vals_eq Heq Hl.
Qed.

Lemma q21_ao_p8 :
  OP (QExpr_Project Queries.scalar_select_list_8
        (@QExpr_Table TNull relname q21_lineitem_table_outputs (Rel "lineitem")))
     (q21_map_project_sel8 ls').
Proof.
  apply (outcomes_bag_project bs ins value_is_null).
  - apply outcomes_bag_table; apply q21_embed_table_same_rows_lineitem.
  - intros sched r r' Hr Heq; rewrite Forall_forall in Ll; pose proof (Ll r Hr) as Hl.
    split; [apply q21_sel8_exact; exact (q21_teq_labels _ _ _ Heq Hl)|].
    unfold q21_sel8_values; q21_vals_eq Heq Hl.
Qed.

Lemma q21_ao_p9 :
  OP (QExpr_Project Queries.scalar_select_list_9
        (@QExpr_Table TNull relname q21_orders_table_outputs (Rel "orders")))
     (q21_map_project_sel9 os').
Proof.
  apply (outcomes_bag_project bs ins value_is_null).
  - apply outcomes_bag_table; apply q21_embed_table_same_rows_orders.
  - intros sched r r' Hr Heq; rewrite Forall_forall in Lo; pose proof (Lo r Hr) as Hl.
    split; [apply q21_sel9_exact; exact (q21_teq_labels _ _ _ Heq Hl)|].
    unfold q21_sel9_values; q21_vals_eq Heq Hl.
Qed.

Lemma q21_ao_p10 :
  OP (QExpr_Project Queries.scalar_select_list_10
        (@QExpr_Table TNull relname q21_nation_table_outputs (Rel "nation")))
     (q21_map_project_sel10 ns').
Proof.
  apply (outcomes_bag_project bs ins value_is_null).
  - apply outcomes_bag_table; apply q21_embed_table_same_rows_nation.
  - intros sched r r' Hr Heq; rewrite Forall_forall in Ln; pose proof (Ln r Hr) as Hl.
    split; [apply q21_sel10_exact; exact (q21_teq_labels _ _ _ Heq Hl)|].
    unfold q21_sel10_values; q21_vals_eq Heq Hl.
Qed.

Lemma q21_ao_p6 :
  OP (QExpr_Project Queries.scalar_select_list_6
        (QExpr_Filter SExpr_True
           (QExpr_CrossJoin
              (QExpr_Project Queries.scalar_select_list_7
                 (@QExpr_Table TNull relname q21_supplier_table_outputs (Rel "supplier")))
              (QExpr_Project Queries.scalar_select_list_8
                 (@QExpr_Table TNull relname q21_lineitem_table_outputs (Rel "lineitem"))))))
     (List.map (fun ft => project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
        (brute_left_join_list (tuple TNull) (join_tuple TNull)
           (q21_map_project_sel7 ss') (q21_map_project_sel8 ls'))).
Proof.
  apply (outcomes_bag_project bs ins value_is_null).
  - apply outcomes_bag_filter_true; apply outcomes_bag_cross_join;
      [exact q21_ao_p7|exact q21_ao_p8].
  - intros sched r r' Hr Heq.
    pose proof (q21_Forall_join_project_sel7_sel8_labels ss' ls') as HL.
    rewrite Forall_forall in HL; pose proof (HL r Hr) as Hl.
    split; [apply q21_sel6_exact; exact (q21_teq_labels _ _ _ Heq Hl)|].
    unfold q21_sel6_values, q21_sel7_values, q21_sel8_values; cbn [List.app].
    q21_vals_eq Heq Hl.
Qed.

Lemma q21_ao_p5 :
  OP (QExpr_Project Queries.scalar_select_list_5
        (QExpr_Filter SExpr_True
           (QExpr_CrossJoin
              (QExpr_Project Queries.scalar_select_list_6
                 (QExpr_Filter SExpr_True
                    (QExpr_CrossJoin
                       (QExpr_Project Queries.scalar_select_list_7
                          (@QExpr_Table TNull relname q21_supplier_table_outputs (Rel "supplier")))
                       (QExpr_Project Queries.scalar_select_list_8
                          (@QExpr_Table TNull relname q21_lineitem_table_outputs (Rel "lineitem"))))))
              (QExpr_Project Queries.scalar_select_list_9
                 (@QExpr_Table TNull relname q21_orders_table_outputs (Rel "orders"))))))
     (List.map (fun ft => project_row Queries.scalar_select_list_5 (q21_sel5_values ft))
        (brute_left_join_list (tuple TNull) (join_tuple TNull)
           (List.map (fun ft => project_row Queries.scalar_select_list_6 (q21_sel6_values ft))
              (brute_left_join_list (tuple TNull) (join_tuple TNull)
                 (q21_map_project_sel7 ss') (q21_map_project_sel8 ls')))
           (q21_map_project_sel9 os'))).
Proof.
  apply (outcomes_bag_project bs ins value_is_null).
  - apply outcomes_bag_filter_true; apply outcomes_bag_cross_join;
      [exact q21_ao_p6|exact q21_ao_p9].
  - intros sched r r' Hr Heq.
    pose proof (q21_Forall_join_sl6_project_sel9_labels ss' ls' os') as HL.
    rewrite Forall_forall in HL; pose proof (HL r Hr) as Hl.
    split; [apply q21_sel5_exact; exact (q21_teq_labels _ _ _ Heq Hl)|].
    unfold q21_sel5_values, q21_sel6_values, q21_sel7_values, q21_sel8_values,
      q21_sel9_values; cbn [List.app].
    q21_vals_eq Heq Hl.
Qed.

(** Every outcome of [Project select_4 (Filter True spine)] is a permutation
    of the projected product P. *)
Theorem Q21_ao_where_input :
  OP (QExpr_Project Queries.scalar_select_list_4
        (QExpr_Filter SExpr_True q21_expected_cross_spine))
     (List.map q21_project_select4 (q21_cross4_tuples ss' ls' os' ns')).
Proof.
  rewrite <- (q21_map_project_select4_spine_eq_cross4 ss' ls' os' ns' Ls Ll Lo Ln).
  unfold q21_project_select4, q21_expected_cross_spine.
  apply (outcomes_bag_project bs ins value_is_null).
  - apply outcomes_bag_filter_true; unfold q21_spine_cross4_tuples.
    apply outcomes_bag_cross_join; [exact q21_ao_p5|exact q21_ao_p10].
  - intros sched r r' Hr Heq.
    pose proof (q21_Forall_spine_cross4_labels ss' ls' os' ns' Ls Ll Lo Ln) as HL.
    rewrite Forall_forall in HL; pose proof (HL r Hr) as Hl.
    split; [apply q21_select4_exact; exact (q21_teq_labels _ _ _ Heq Hl)|].
    unfold q21_select4_dot_values; q21_vals_eq Heq Hl.
Qed.

End Q21SpineOutcomes.

(** * Part 2: key order on grouped output rows *)

Definition q21_kz (o : tuple TNull) : Z * string :=
  match dot TNull o (AttrInt64 "numwait"), dot TNull o (AttrString "s_name" (StringChar 25)) with
  | Value_int64 (Some i), Value_string (_, Some s) => (int64_value i, s)
  | _, _ => (0%Z, EmptyString)
  end.

Definition q21_key_le (k1 k2 : Z * string) : Prop :=
  (fst k2 < fst k1)%Z \/ (fst k1 = fst k2 /\ string_compare (snd k1) (snd k2) <> Gt).

Lemma q21_string_le_trans :
  forall a b c, string_compare a b <> Gt -> string_compare b c <> Gt ->
    string_compare a c <> Gt.
Proof.
  intros a b c H1 H2.
  destruct (string_compare a b) eqn:E1; [|clear H1|contradiction H1; reflexivity].
  - apply (Oset.compare_eq_iff Ostring) in E1; subst b; exact H2.
  - destruct (string_compare b c) eqn:E2; [|clear H2|contradiction H2; reflexivity].
    + apply (Oset.compare_eq_iff Ostring) in E2; subst c; rewrite E1; discriminate.
    + change (Oset.compare Ostring a b = Lt) in E1.
      change (Oset.compare Ostring b c = Lt) in E2.
      change (Oset.compare Ostring a c <> Gt).
      rewrite (Oset.compare_lt_trans Ostring a b c E1 E2); discriminate.
Qed.

Lemma q21_key_le_trans :
  forall a b c, q21_key_le a b -> q21_key_le b c -> q21_key_le a c.
Proof.
  intros [n1 s1] [n2 s2] [n3 s3]; unfold q21_key_le; simpl.
  intros [H1|[E1 H1]] [H2|[E2 H2]]; subst; try (left; lia).
  right; split; [reflexivity|eapply q21_string_le_trans; eassumption].
Qed.

Lemma q21_key_le_antisym :
  forall a b, q21_key_le a b -> q21_key_le b a -> a = b.
Proof.
  intros [n1 s1] [n2 s2]; unfold q21_key_le; simpl.
  intros [H1|[E1 H1]] [H2|[E2 H2]]; try lia; subst.
  f_equal.
  apply (Oset.compare_eq_iff Ostring).
  change (string_compare s1 s2 = Eq).
  pose proof (Oset.compare_lt_gt Ostring s1 s2) as Hop; change (string_compare s1 s2 =
    CompOpp (string_compare s2 s1)) in Hop.
  destruct (string_compare s1 s2); [reflexivity| |contradiction H1; reflexivity].
  destruct (string_compare s2 s1); simpl in Hop; try discriminate; contradiction H2; reflexivity.
Qed.

(** Shapes of grouped output rows. *)
Definition q21_out_row_ok (o : tuple TNull) : Prop :=
  labels TNull o =S=
    Fset.mk_set (A TNull) (scalar_select_outputs Queries.scalar_select_list_1) /\
  (exists s, dot TNull o (AttrString "s_name" (StringChar 25)) =
             Value_string (StringValue (StringChar 25) (Some s))) /\
  (exists i, dot TNull o (AttrInt64 "numwait") = Value_int64 (Some i)).

Lemma q21_out_row_ok_teq :
  forall o o', Oeset.compare (OTuple TNull) o o' = Eq -> q21_out_row_ok o ->
    q21_out_row_ok o' /\ q21_kz o' = q21_kz o /\ q21_final_out o' = q21_final_out o.
Proof.
  intros o o' Heq [L [[s Hs] [i Hi]]].
  assert (Ds : dot TNull o' (AttrString "s_name" (StringChar 25)) =
               dot TNull o (AttrString "s_name" (StringChar 25)))
    by (apply (q21_teq_dot _ _ _ Heq); rewrite (Fset.mem_eq_2 _ _ _ L); vm_compute; reflexivity).
  assert (Dn : dot TNull o' (AttrInt64 "numwait") = dot TNull o (AttrInt64 "numwait"))
    by (apply (q21_teq_dot _ _ _ Heq); rewrite (Fset.mem_eq_2 _ _ _ L); vm_compute; reflexivity).
  split; [split; [exact (q21_teq_labels _ _ _ Heq L)|split; [exists s; rewrite Ds; exact Hs|exists i; rewrite Dn; exact Hi]]|].
  unfold q21_kz, q21_final_out; rewrite Ds, Dn; split; reflexivity.
Qed.

Lemma q21_ordered_pair_key_le :
  forall o1 o2, q21_out_row_ok o1 -> q21_out_row_ok o2 ->
    @ordered_pair TNull value_is_null q21_fsql_order_keys o1 o2 ->
    q21_key_le (q21_kz o1) (q21_kz o2).
Proof.
  intros o1 o2 [_ [[s1 Hs1] [i1 Hi1]]] [_ [[s2 Hs2] [i2 Hi2]]] Hp.
  unfold q21_kz; rewrite Hs1, Hi1, Hs2, Hi2; unfold q21_key_le; simpl.
  unfold ordered_pair, q21_fsql_order_keys, SortDescNullsFirst, SortAscNullsLast, SortKey in Hp.
  cbn [compare_order_keys] in Hp.
  unfold compare_order_key in Hp; cbn [sort_key_attribute sort_key_direction
    sort_key_null_direction sort_key_value_compare] in Hp.
  rewrite Hs1, Hs2, Hi1, Hi2 in Hp.
  unfold compare_order_value in Hp.
  change (value_is_null (Value_int64 (Some i1))) with false in Hp.
  change (value_is_null (Value_int64 (Some i2))) with false in Hp.
  change (value_is_null (Value_string (StringValue (StringChar 25) (Some s1)))) with false in Hp.
  change (value_is_null (Value_string (StringValue (StringChar 25) (Some s2)))) with false in Hp.
  rewrite q21_order_compare_int64, q21_order_compare_char25 in Hp.
  unfold TNullSyntax.SortDesc, TNullSyntax.SortAsc in Hp.
  destruct (Z.compare_spec (int64_value i1) (int64_value i2)) as [E|L|G];
    cbn [reverse_comparison] in Hp.
  - right; split; [exact E|].
    destruct (string_compare s1 s2); [discriminate|discriminate|contradiction].
  - contradiction.
  - left; lia.
Qed.

Lemma q21_ordered_rows_sorted_keys :
  forall O, List.Forall q21_out_row_ok O ->
    @ordered_rows TNull value_is_null q21_fsql_order_keys O ->
    LocallySorted q21_key_le (List.map q21_kz O).
Proof.
  induction O as [|o1 O IH]; intros HF Hord; [constructor|].
  destruct O as [|o2 O']; [constructor|].
  inversion HF as [|? ? H1 HF']; subst.
  inversion HF' as [|? ? H2 _]; subst.
  destruct Hord as [Hp Hord'].
  cbn [List.map]; constructor.
  - apply IH; assumption.
  - apply q21_ordered_pair_key_le; assumption.
Qed.

Definition q21_final_of_key (k : Z * string) : tuple TNull :=
  project_row Queries.scalar_select_list_0
    [Value_string (StringValue (StringChar 25) (Some (snd k)));
     value_int64_checked (fst k)].

Lemma q21_final_out_of_key :
  forall o, q21_out_row_ok o -> q21_final_out o = q21_final_of_key (q21_kz o).
Proof.
  intros o [_ [[s Hs] [i Hi]]].
  unfold q21_final_out, q21_final_of_key, q21_kz; rewrite Hs, Hi; simpl.
  unfold value_int64_checked; rewrite q21_int64_checked_value; reflexivity.
Qed.

(** Uniqueness: any two ordered representatives of the grouped bag give the
    same final rows. *)
Theorem Q21_order_final_unique :
  forall GO O1 O2,
    List.Forall q21_out_row_ok GO ->
    Oeset.permut (OTuple TNull) GO O1 -> Oeset.permut (OTuple TNull) GO O2 ->
    @ordered_rows TNull value_is_null q21_fsql_order_keys O1 ->
    @ordered_rows TNull value_is_null q21_fsql_order_keys O2 ->
    List.map q21_final_out O1 = List.map q21_final_out O2.
Proof.
  intros GO O1 O2 HGO HP1 HP2 Hord1 Hord2.
  assert (Hok : forall O, Oeset.permut (OTuple TNull) GO O -> List.Forall q21_out_row_ok O).
  { intros O HP; apply Forall_forall; intros o Ho.
    destruct (pod_permut_in_left _ _ _ _ o HP Ho) as [g [Hg Hgo]].
    rewrite Forall_forall in HGO.
    exact (proj1 (q21_out_row_ok_teq g o Hgo (HGO g Hg))). }
  pose proof (Hok O1 HP1) as H1. pose proof (Hok O2 HP2) as H2.
  assert (HP12 : Oeset.permut (OTuple TNull) O1 O2)
    by (eapply Oeset.permut_trans; [apply Oeset.permut_sym; exact HP1|exact HP2]).
  assert (HK : Permutation (List.map q21_kz O1) (List.map q21_kz O2)).
  { eapply pod_permut_map; [exact HP12|].
    intros a b Ha Hab; rewrite Forall_forall in H1.
    symmetry; exact (proj1 (proj2 (q21_out_row_ok_teq a b Hab (H1 a Ha)))). }
  assert (Heq : List.map q21_kz O1 = List.map q21_kz O2).
  { eapply (pod_sorted_perm_unique _ q21_key_le (fun _ => True)).
    - intros a b c _ _ _; apply q21_key_le_trans.
    - intros a b _ _; apply q21_key_le_antisym.
    - apply Forall_forall; intros; exact I.
    - exact HK.
    - apply q21_ordered_rows_sorted_keys; assumption.
    - apply q21_ordered_rows_sorted_keys; assumption. }
  rewrite (map_ext_in _ _ O1 (fun o Ho => q21_final_out_of_key o
             (proj1 (Forall_forall _ _) H1 o Ho))).
  rewrite (map_ext_in _ _ O2 (fun o Ho => q21_final_out_of_key o
             (proj1 (Forall_forall _ _) H2 o Ho))).
  rewrite <- (map_map q21_kz q21_final_of_key O1), <- (map_map q21_kz q21_final_of_key O2), Heq.
  reflexivity.
Qed.

(** * Part 3: group values do not depend on the group representative *)

Lemma q21_comparelA_cons_eq :
  forall (X : Type) (cmp : X -> X -> comparison) a l b m,
    comparelA cmp (a :: l) (b :: m) = Eq -> cmp a b = Eq.
Proof. intros X cmp a l b m H; simpl in H; destruct (cmp a b); congruence. Qed.

Lemma q21_group_values_respect :
  forall g g', Oeset.compare (OLTuple TNull) g g' = Eq ->
    q21_group_values g' = q21_group_values g.
Proof.
  intros g g' H.
  change (Oeset.compare (mk_oelists (OTuple TNull))
            (ListSort.quicksort (OTuple TNull) g)
            (ListSort.quicksort (OTuple TNull) g') = Eq) in H.
  pose proof (comparelA_eq_length_eq _ _ _ H) as Hlen.
  rewrite !(@ListSort.length_quicksort _ (OTuple TNull)) in Hlen.
  unfold q21_group_values, q21_group_env, Env.env_g.
  rewrite !(Interp.interp_dot_unfold TNull (_ :: nil) q21_sname_attr).
  rewrite Hlen.
  destruct (ListSort.quicksort (OTuple TNull) g) as [|t l];
  destruct (ListSort.quicksort (OTuple TNull) g') as [|t' l'];
    try discriminate H; [reflexivity|].
  pose proof (q21_comparelA_cons_eq _ (Oeset.compare (OTuple TNull)) t l t' l' H) as Ht.
  rewrite (Fset.mem_eq_2 _ _ _ (tuple_eq_labels TNull t t' Ht)).
  destruct (q21_sname_attr inS? labels TNull t') eqn:Hin; [|reflexivity].
  f_equal.
  apply (q21_teq_dot _ _ _ Ht).
  rewrite (Fset.mem_eq_2 _ _ _ (tuple_eq_labels TNull t t' Ht)); exact Hin.
Qed.

(** * Part 4: every outcome of the Group stage is the same bag *)

Section Q21GroupOutcomes.
Variables ss' ls' os' ns' : list (tuple TNull).
Hypothesis Ls : List.Forall
  (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) supplier_formal_attrs) ss'.
Hypothesis Ll : List.Forall
  (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) lineitem_formal_attrs) ls'.
Hypothesis Lo : List.Forall
  (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) orders_formal_attrs) os'.
Hypothesis Ln : List.Forall
  (fun ft => labels TNull ft =S= Fset.mk_set (A TNull) nation_formal_attrs) ns'.
Variable nation_name : string.
Hypothesis Ml : List.Forall q21_lineitem_member ls'.
Hypothesis Hnn : NationNameOk nation_name.

Local Abbreviation P := (List.map q21_project_select4 (q21_cross4_tuples ss' ls' os' ns')).

Hypothesis Pfacts : forall ft, List.In ft P ->
  (exists rr, row_correspond slot_map_outer_cor ft rr) /\
  q21_outer_cor_labels_ok ft /\ q21_outer_cor_text_canonical ft.
Hypothesis Psn : forall ft, List.In ft P -> q21_sname_ok ft.
Hypothesis Hcnt : (Z.of_nat (List.length P) <= int64_max)%Z.

Local Abbreviation bs := (@_basesort TNull (D_of_embeds ss' ls' os' ns')).
Local Abbreviation ins := (@_instance TNull (D_of_embeds ss' ls' os' ns')).
Local Abbreviation OB := (@outcomes_bag relname bs ins value_is_null).
Local Abbreviation OP := (@outcomes_perm relname bs ins value_is_null).

Local Abbreviation FW := (List.filter (q21_catalog_where_keep nation_name ls') P).

Lemma q21_ao_where :
  OP (q21_catalog_where_stage nation_name) FW.
Proof.
  unfold q21_catalog_where_stage.
  apply outcomes_perm_filter; [apply Q21_ao_where_input; assumption|].
  intros sched r Hr.
  destruct (Pfacts r Hr) as [[rr Hrc] [Hl Ht]].
  exact (Q21_catalog_where_row_acceptance ss' ls' os' ns' sched
           nation_name r rr Ml Hrc Hl Ht Hnn).
Qed.

Lemma q21_ao_group_input :
  OP (QExpr_Project Queries.scalar_select_list_2
        (QExpr_Project Queries.scalar_select_list_3
           (q21_catalog_where_stage nation_name)))
     (List.map q21_gin FW).
Proof.
  assert (H3 : OP (QExpr_Project Queries.scalar_select_list_3
                     (q21_catalog_where_stage nation_name))
                  (List.map q21_row3 FW)).
  { apply outcomes_perm_project; [exact q21_ao_where|].
    intros sched r Hr.
    apply filter_In in Hr as [Hr _].
    apply q21_sel3_exact.
    eapply q21_P_labels; exact Hr. }
  pose proof (@outcomes_perm_project relname bs ins value_is_null Queries.scalar_select_list_2 _ _
                (fun r3 => [dot TNull r3 q21_sname_attr]) H3) as H2.
  assert (Hmap : List.map q21_gin FW =
                 List.map (fun r3 => project_row Queries.scalar_select_list_2
                                       [dot TNull r3 q21_sname_attr])
                   (List.map q21_row3 FW)).
  { rewrite List.map_map; apply List.map_ext; intro ft.
    unfold q21_gin; rewrite q21_row3_dot_sname; reflexivity. }
  rewrite Hmap; apply H2.
  intros sched r3 Hr3.
  apply in_map_iff in Hr3 as [ft [<- _]].
  eapply scalar_value_list_exact_cons; [|apply scalar_value_list_exact_nil].
  apply tnull_leaf_dot_exact; apply interp_dot_env_t; apply q21_row3_sname_inS.
Qed.

Local Abbreviation G := (List.map q21_gin FW).
Local Abbreviation GO := (List.map (fun g => project_row Queries.scalar_select_list_1
                                               (q21_group_values g))
                            (query_make_groups nil G q21_group_terms)).

Lemma q21_ao_group :
  OB (q21_catalog_group_stage nation_name) GO.
Proof.
  unfold q21_catalog_group_stage.
  apply outcomes_bag_group.
  - exact q21_group_keys_terms.
  - discriminate.
  - exact q21_ao_group_input.
  - apply q21_group_keys_runtime_none.
  - intros sched rep Hrep g Hg.
    apply q21_group_filter_map_exact.
    apply q21_count_fits_of_le.
    rewrite q21_query_make_groups_eq in Hg.
    pose proof (q21_partition_group_length rep g Hg) as Hle.
    pose proof (ListPermut._permut_length Hrep) as HlenG.
    rewrite length_map in HlenG.
    pose proof (q21_filter_length_le _ (q21_catalog_where_keep nation_name ls') P) as Hf.
    eapply Z.le_trans; [|exact Hcnt]; lia.
  - intros g g' _ Heq; exact (q21_group_values_respect g g' Heq).
Qed.

Lemma q21_ao_group_shape :
  List.Forall q21_out_row_ok GO.
Proof.
  apply Forall_forall; intros o Ho.
  apply in_map_iff in Ho as [g [<- Hg]].
  pose proof Hg as Hg'.
  rewrite q21_query_make_groups_eq in Hg'.
  assert (Hmem : forall t, List.In t g -> exists ft, t = q21_gin ft /\ List.In ft P).
  { intros t Ht.
    pose proof (Partition.in_map_snd_partition _ _ _ _ _ Hg' Ht) as HtG.
    apply in_map_iff in HtG as [ft [<- Hft]].
    apply filter_In in Hft as [Hft _]; eauto. }
  destruct (q21_group_env_dot_member g
              (ltac:(eapply Partition.in_map_snd_partition_diff_nil; exact Hg')))
    as [t [Ht Hdot]].
  { apply Forall_forall; intros t Ht.
    destruct (Hmem t Ht) as [ft [-> _]]; apply q21_gin_sname_inS. }
  destruct (Hmem t Ht) as [ft [-> Hft]].
  destruct (Psn ft Hft) as [s [Hs _]].
  destruct (q21_sel1_dots (Interp.interp_dot TNull (q21_group_env g) q21_sname_attr)
              (value_int64_checked (Z.of_nat (List.length g)))) as [D1 D2].
  assert (Hfit : q21_count_fits (List.length g)).
  { apply q21_count_fits_of_le.
    pose proof (q21_partition_group_length G g Hg') as Hle.
    rewrite length_map in Hle.
    pose proof (q21_filter_length_le _ (q21_catalog_where_keep nation_name ls') P) as Hf.
    eapply Z.le_trans; [|exact Hcnt]; lia. }
  destruct (q21_value_int64_checked_fits _ Hfit) as [i [Hi _]].
  split; [apply q21_project_row_labels|split].
  - exists s. unfold q21_group_values. etransitivity; [exact D1|].
    rewrite Hdot, q21_gin_dot_sname; exact Hs.
  - exists i. unfold q21_group_values. etransitivity; [exact D2|exact Hi].
Qed.

End Q21GroupOutcomes.

(** * Part 5: every possible outcome of q̂ is the same success *)

Definition q21_final_vals (o : tuple TNull) : list value :=
  [dot TNull o (AttrString "s_name" (StringChar 25));
   dot TNull o (AttrInt64 "numwait")].

(** Any two possible outcomes of q̂ (under any schedules) coincide. *)
Theorem Q21_qhat_outcomes_unique :
  forall db nation_name D,
    Preconditions_Q21 db nation_name ->
    Q21CountSafe db ->
    EmbedDB_Q21 db = Some D ->
    forall sched1 o1 sched2 o2,
      @eval_query_expr_outcome TNull relname (@_basesort TNull D) (@_instance TNull D)
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error value_is_null sched1 nil
        (qhat_Q21 nation_name) o1 ->
      @eval_query_expr_outcome TNull relname (@_basesort TNull D) (@_instance TNull D)
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error value_is_null sched2 nil
        (qhat_Q21 nation_name) o2 ->
      o1 = o2.
Proof.
  intros db nation_name D Hpre Hsafe HD sched1 o1 sched2 o2 Hev1 Hev2.
  pose proof Hpre as [Hok [Hnn [Hchar Hemb]]].
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
  set (P := List.map q21_project_select4 (q21_cross4_tuples ss' ls' os' ns')) in *.
  destruct Hcor as [rrs' [HPr HFr]].
  assert (Pfacts : forall ft, List.In ft P ->
            (exists rr, row_correspond slot_map_outer_cor ft rr) /\
            q21_outer_cor_labels_ok ft /\ q21_outer_cor_text_canonical ft).
  { intros ft Hin; split; [exact (q21_Forall2_In_l _ _ _ _ _ _ HFr Hin)|].
    exact (q21_projected_member_facts ss' ls' os' ns' ft Ms Ml Mo Mn Hin). }
  assert (Psn : forall ft, List.In ft P -> q21_sname_ok ft)
    by (intros ft Hin; eapply q21_P_sname; eassumption).
  assert (Hcnt : (Z.of_nat (List.length P) <= int64_max)%Z).
  { pose proof (List.Forall2_length HFr) as H1.
    pose proof (Permutation_length HPr) as H2.
    rewrite q21_cross4_rows_length in H2.
    pose proof (Hsafe ss ls os ns Hs Hl Ho Hn) as H3.
    rewrite H1, <- H2; exact H3. }
  set (GO := List.map (fun g => project_row Queries.scalar_select_list_1
                                  (q21_group_values g))
               (query_make_groups nil
                  (List.map q21_gin
                     (List.filter (q21_catalog_where_keep nation_name ls') P))
                  q21_group_terms)).
  pose proof (q21_ao_group ss' ls' os' ns' Ls Ll Lo Ln nation_name Ml Hnn
                Pfacts Hcnt) as HOB.
  pose proof (q21_ao_group_shape ss' ls' os' ns' nation_name Psn Hcnt) as HSh.
  fold P GO in HOB, HSh.
  assert (Hex : forall sched r r', List.In r GO ->
            Oeset.compare (OTuple TNull) r r' = Eq ->
            @scalar_value_list_exact_at TNull relname
              (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
              (@_instance TNull (D_of_embeds ss' ls' os' ns')) unknown3
              NullValues.interp_scalar_operator_runtime_error
              NullValues.interp_aggregate_runtime_error value_is_null sched
              (Env.env_t TNull nil r') (map fst Queries.scalar_select_list_0)
              (q21_final_vals r')).
  { intros sched r r' Hr Heq.
    rewrite Forall_forall in HSh.
    destruct (q21_out_row_ok_teq r r' Heq (HSh r Hr)) as [[Hl' _] _].
    rewrite q21_select_list_0_shape; cbn [map fst].
    unfold q21_final_vals.
    eapply scalar_value_list_exact_cons;
      [|eapply scalar_value_list_exact_cons; [|apply scalar_value_list_exact_nil]];
      apply tnull_leaf_dot_exact; apply interp_dot_env_t;
      rewrite (Fset.mem_eq_2 _ _ _ Hl'); vm_compute; reflexivity. }
  assert (Hall : forall sched o,
    @eval_query_expr_outcome TNull relname
      (@_basesort TNull (D_of_embeds ss' ls' os' ns'))
      (@_instance TNull (D_of_embeds ss' ls' os' ns'))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched nil
      (qhat_Q21 nation_name) o ->
    exists O', Oeset.permut (OTuple TNull) GO O' /\
      @ordered_rows TNull value_is_null q21_fsql_order_keys O' /\
      o = SqlSuccess (List.map q21_final_out O')).
  { intros sched o Hev.
    rewrite Q21_catalog_top_shape in Hev; unfold q21_catalog_top in Hev.
    exact (outcomes_order_project _ _ _ Queries.scalar_select_list_0
             q21_fsql_order_keys _ GO q21_final_vals HOB Hex sched o Hev). }
  destruct (Hall sched1 o1 Hev1) as [O1 [HP1 [Hord1 ->]]].
  destruct (Hall sched2 o2 Hev2) as [O2 [HP2 [Hord2 ->]]].
  f_equal.
  exact (Q21_order_final_unique GO O1 O2 HSh HP1 HP2 Hord1 Hord2).
Qed.

(** Strengthened NormalizeSound_Q21: the embedded accelerator result is not
    merely one possible FormalSQL outcome of q̂ -- it is the ONLY one. Every
    outcome, under every Boolean schedule and every bag/ORDER BY
    representative choice, equals [SqlSuccess outs]. *)
Theorem NormalizeSound_Q21_all_outcomes :
  forall (db : DB) (nation_name : string) (rows : list Row),
    Preconditions_Q21 db nation_name ->
    Q21CountSafe db ->
    Eval_Spec_Q21 nation_name db = Some rows ->
    exists outs D,
      Embed_Q21_Result rows = Some outs /\
      EmbedDB_Q21 db = Some D /\
      FormalSQL_exec D (qhat_Q21 nation_name) outs /\
      (forall sched o,
         @eval_query_expr_outcome TNull relname (@_basesort TNull D) (@_instance TNull D)
           unknown3 NullValues.interp_scalar_operator_runtime_error
           NullValues.interp_aggregate_runtime_error value_is_null sched nil
           (qhat_Q21 nation_name) o ->
         o = SqlSuccess outs).
Proof.
  intros db nation_name rows Hpre Hsafe Hev.
  destruct (NormalizeSound_Q21 db nation_name rows Hpre Hsafe Hev)
    as [outs [D [He [HD Hx]]]].
  exists outs, D; split; [exact He|split; [exact HD|split; [exact Hx|]]].
  intros sched o Ho.
  pose proof Hx as Hx'.
  unfold FormalSQL_exec, eval_query_expr_possible_outcome in Hx'.
  destruct Hx' as [sched0 Hx0].
  exact (Q21_qhat_outcomes_unique db nation_name D Hpre Hsafe HD
           sched o sched0 (SqlSuccess outs) Ho Hx0).
Qed.

Corollary NormalizeSound_Q21_catalog_all_outcomes :
  forall (db : DB) (rows : list Row),
    Preconditions_Q21 db catalog_nation_name ->
    Q21CountSafe db ->
    Eval_Spec LogicalSpec_Q21_rel params_catalog_nation db = Some rows ->
    exists outs D,
      Embed_Q21_Result rows = Some outs /\
      EmbedDB_Q21 db = Some D /\
      FormalSQL_exec D Queries.source_query_expr outs /\
      (forall sched o,
         @eval_query_expr_outcome TNull relname (@_basesort TNull D) (@_instance TNull D)
           unknown3 NullValues.interp_scalar_operator_runtime_error
           NullValues.interp_aggregate_runtime_error value_is_null sched nil
           Queries.source_query_expr o ->
         o = SqlSuccess outs).
Proof.
  intros db rows Hpre Hsafe Hev.
  rewrite <- Params_Q21_catalog_instance in Hev.
  destruct (NormalizeSound_Q21_all_outcomes db catalog_nation_name rows Hpre Hsafe Hev)
    as [outs [D [He [HD [Hx Hall]]]]].
  rewrite qhat_Q21_catalog_instance in Hx, Hall.
  exists outs, D; repeat split; assumption.
Qed.
