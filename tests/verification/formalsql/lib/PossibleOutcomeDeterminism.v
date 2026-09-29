(* Possible-outcome determinism facade.

   FormalSQL query semantics is a relation: every Boolean schedule and every
   representative of an intermediate bag is a legal evaluation.  This facade
   proves, operator by operator, that ALL possible outcomes of a query succeed
   and agree:
     - [outcomes_perm q L] : every outcome is a success whose rows are a
       (Leibniz) permutation of [L]  (Project / Filter outputs);
     - [outcomes_bag  q L] : every outcome is a success whose rows are a
       permutation of [L] up to tuple equivalence (Table / CrossJoin / Group).
   Projects erase representation differences, so bag-level results collapse
   back to [outcomes_perm] at every Project.  Fully proved; no new axioms. *)

From Stdlib Require Import List Permutation Sorting.Sorted.
From SQLFS Require Import
  OrderedSet FiniteSet FiniteBag FiniteCollection FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts Values GenericInstance Projection ListPermut Join.
From Logos.FormalSQL Require Import
  TNullSyntax QueryTNullSyntax GroupedFilterOutcomeFacts QueryCardinality
  GroupingRewriteFacts.
From FormalSQLLib Require Import ProjectExactFacade GroupsExactFacade.
Import Tuple.
Import ListNotations.

(** * Generic _permut / Permutation bridges *)

Lemma pod_permut_in_left :
  forall (X : Type) (R : X -> X -> Prop) l1 l2 b,
    _permut R l1 l2 -> In b l2 -> exists a, In a l1 /\ R a b.
Proof.
  intros X R l1 l2 b H; induction H as [|a b0 l k1 k2 Hab H IH]; intros Hin;
    [contradiction|].
  apply in_app_iff in Hin; destruct Hin as [Hin|[<-|Hin]].
  - destruct (IH (proj2 (in_app_iff k1 k2 b) (or_introl Hin))) as [a' [Ha' Hr]].
    exists a'; split; [right; exact Ha'|exact Hr].
  - exists a; split; [left; reflexivity|exact Hab].
  - destruct (IH (proj2 (in_app_iff k1 k2 b) (or_intror Hin))) as [a' [Ha' Hr]].
    exists a'; split; [right; exact Ha'|exact Hr].
Qed.

Lemma pod_permut_map :
  forall (X Y : Type) (R : X -> X -> Prop) (f : X -> Y) l1 l2,
    _permut R l1 l2 ->
    (forall a b, In a l1 -> R a b -> f a = f b) ->
    Permutation (map f l1) (map f l2).
Proof.
  intros X Y R f l1 l2 H; induction H as [|a b l k1 k2 Hab H IH]; intros Hf;
    [constructor|].
  rewrite map_app; cbn [map].
  rewrite (Hf a b (or_introl eq_refl) Hab).
  apply Permutation_cons_app.
  rewrite <- map_app; apply IH.
  intros x y Hx Hxy; apply Hf; [right; exact Hx|exact Hxy].
Qed.

Lemma pod_permut_filter :
  forall (X : Type) (R : X -> X -> Prop) (keep : X -> bool) l1 l2,
    _permut R l1 l2 ->
    (forall a b, In a l1 -> R a b -> keep a = keep b) ->
    _permut R (filter keep l1) (filter keep l2).
Proof.
  intros X R keep l1 l2 H; induction H as [|a b l k1 k2 Hab H IH]; intros Hk;
    [constructor|].
  assert (IH' : _permut R (filter keep l) (filter keep k1 ++ filter keep k2)).
  { rewrite <- filter_app; apply IH; intros x y Hx Hxy; apply Hk; [right|]; assumption. }
  rewrite filter_app; cbn [filter].
  rewrite <- (Hk a b (or_introl eq_refl) Hab).
  destruct (keep a); [apply Pcons; assumption|exact IH'].
Qed.

Lemma pod_Permutation_permut :
  forall (X : Type) (R : X -> X -> Prop) l1 l2,
    (forall a, R a a) ->
    (forall a b c, R a b -> R b c -> R a c) ->
    Permutation l1 l2 -> _permut R l1 l2.
Proof.
  intros X R l1 l2 Hrefl Htrans H.
  induction H as [|x l l' H IH|x y l|l l' l'' H1 IH1 H2 IH2].
  - constructor.
  - apply (@Pcons X X R x x l [] l'); [apply Hrefl|exact IH].
  - apply (@Pcons X X R y y (x :: l) [x] l); [apply Hrefl|].
    apply (@Pcons X X R x x l [] l); [apply Hrefl|].
    apply _permut_refl; intros; apply Hrefl.
  - apply _permut_trans with l'; [|exact IH1|exact IH2].
    intros a b c _ _ _; apply Htrans.
Qed.

Lemma pod_Permutation_filter :
  forall (X : Type) (keep : X -> bool) l1 l2,
    Permutation l1 l2 -> Permutation (filter keep l1) (filter keep l2).
Proof.
  intros X keep l1 l2 H.
  induction H as [|x l l' H IH|x y l|l l' l'' H1 IH1 H2 IH2]; simpl.
  - constructor.
  - destruct (keep x); [constructor; exact IH|exact IH].
  - destruct (keep x), (keep y); try constructor; try apply Permutation_refl.
  - eapply Permutation_trans; eassumption.
Qed.

(** Sorted permutations under a total order (on the elements present) coincide. *)
Lemma pod_sorted_head_le :
  forall (K : Type) (le : K -> K -> Prop) (P : K -> Prop),
    (forall a b c, P a -> P b -> P c -> le a b -> le b c -> le a c) ->
    forall a l, List.Forall P (a :: l) -> LocallySorted le (a :: l) ->
      forall x, In x l -> le a x.
Proof.
  intros K le P Htr a l; revert a; induction l as [|b l IH]; intros a HP HS x Hx;
    [contradiction|].
  inversion HS as [| |? ? ? HS' Hab]; subst.
  inversion HP as [|? ? Pa HP']; subst.
  inversion HP' as [|? ? Pb HPl]; subst.
  destruct Hx as [<-|Hx]; [exact Hab|].
  assert (Hbx : le b x) by (eapply IH; eassumption).
  eapply Htr; [exact Pa|exact Pb| |exact Hab|exact Hbx].
  rewrite Forall_forall in HPl; apply HPl; exact Hx.
Qed.

Lemma pod_sorted_perm_unique :
  forall (K : Type) (le : K -> K -> Prop) (P : K -> Prop),
    (forall a b c, P a -> P b -> P c -> le a b -> le b c -> le a c) ->
    (forall a b, P a -> P b -> le a b -> le b a -> a = b) ->
    forall l1 l2, List.Forall P l1 -> Permutation l1 l2 ->
      LocallySorted le l1 -> LocallySorted le l2 -> l1 = l2.
Proof.
  intros K le P Htr Hanti l1; induction l1 as [|a l1 IH]; intros l2 HP HPerm HS1 HS2.
  - apply Permutation_nil in HPerm; subst; reflexivity.
  - destruct l2 as [|b l2]; [apply Permutation_sym, Permutation_nil in HPerm; discriminate|].
    assert (HP2 : List.Forall P (b :: l2)).
    { apply Forall_forall; intros x Hx; rewrite Forall_forall in HP; apply HP.
      exact (Permutation_in x (Permutation_sym HPerm) Hx). }
    assert (Pa : P a) by (inversion HP; assumption).
    assert (Pb : P b) by (inversion HP2; assumption).
    assert (Hab : a = b).
    { assert (Ha2 : In a (b :: l2)) by (apply (Permutation_in a HPerm); left; reflexivity).
      assert (Hb1 : In b (a :: l1))
        by (apply (Permutation_in b (Permutation_sym HPerm)); left; reflexivity).
      destruct Ha2 as [->|Ha2]; [reflexivity|].
      destruct Hb1 as [->|Hb1]; [reflexivity|].
      apply Hanti; [exact Pa|exact Pb| |].
      - eapply pod_sorted_head_le; [exact Htr|exact HP|exact HS1|exact Hb1].
      - eapply pod_sorted_head_le; [exact Htr|exact HP2|exact HS2|exact Ha2]. }
    subst b; f_equal.
    apply IH.
    + inversion HP; assumption.
    + exact (Permutation_cons_inv HPerm).
    + inversion HS1; subst; [constructor|assumption].
    + inversion HS2; subst; [constructor|assumption].
Qed.

(** * Outcome invariants *)

Section PossibleOutcomeDet.
Context {relname : Type}.
Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable value_is_null : value TNull -> bool.

Local Abbreviation eval sched :=
  (@eval_query_expr_outcome TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null sched).

Definition outcomes_perm (q : query_expr TNull relname) (L : list (tuple TNull)) : Prop :=
  forall sched o, eval sched nil q o ->
    exists L', o = SqlSuccess L' /\ Permutation L L'.

Definition outcomes_bag (q : query_expr TNull relname) (L : list (tuple TNull)) : Prop :=
  forall sched o, eval sched nil q o ->
    exists L', o = SqlSuccess L' /\ Oeset.permut (OTuple TNull) L L'.

Lemma outcomes_perm_bag :
  forall q L, outcomes_perm q L -> outcomes_bag q L.
Proof.
  intros q L H sched o Ho; destruct (H sched o Ho) as [L' [-> HP]].
  exists L'; split; [reflexivity|].
  apply pod_Permutation_permut;
    [intro; apply Oeset.compare_eq_refl|intros a b c; apply Oeset.compare_eq_trans|exact HP].
Qed.

Lemma outcomes_perm_no_error :
  forall q L, outcomes_perm q L -> forall sched e, ~ eval sched nil q (SqlError e).
Proof. intros q L H sched e He; destruct (H sched _ He) as [L' [E _]]; discriminate. Qed.

Lemma outcomes_perm_success :
  forall q L, outcomes_perm q L ->
    forall sched R, eval sched nil q (SqlSuccess R) -> Permutation L R.
Proof. intros q L H sched R He; destruct (H sched _ He) as [L' [E HP]]; injection E as ->; exact HP. Qed.

Lemma outcomes_bag_no_error :
  forall q L, outcomes_bag q L -> forall sched e, ~ eval sched nil q (SqlError e).
Proof. intros q L H sched e He; destruct (H sched _ He) as [L' [E _]]; discriminate. Qed.

Lemma outcomes_bag_success :
  forall q L, outcomes_bag q L ->
    forall sched R, eval sched nil q (SqlSuccess R) -> Oeset.permut (OTuple TNull) L R.
Proof. intros q L H sched R He; destruct (H sched _ He) as [L' [E HP]]; injection E as ->; exact HP. Qed.

Local Abbreviation exact_values sched :=
  (@scalar_value_list_exact_at TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null sched).

Local Abbreviation accept sched :=
  (@scalar_expr_acceptance_exact_at TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null sched).

(** ** Table *)
Lemma outcomes_bag_table :
  forall outputs table L,
    query_same_rows_as_bag L (@query_table_bag TNull relname basesort instance outputs table) ->
    outcomes_bag (QExpr_Table outputs table) L.
Proof.
  intros outputs table L HL sched o H; inversion H; subst.
  eexists; split; [reflexivity|].
  eapply (query_same_rows_as_bag_permut_between TNull); eassumption.
Qed.

(** ** Project over a permutation-determined input *)
Lemma outcomes_perm_project :
  forall sel q L (vf : tuple TNull -> list (value TNull)),
    outcomes_perm q L ->
    (forall sched r, In r L -> exact_values sched (env_t TNull nil r) (map fst sel) (vf r)) ->
    outcomes_perm (QExpr_Project sel q) (map (fun r => project_row sel (vf r)) L).
Proof.
  intros sel q L vf Hq Hex sched o H; inversion H; subst.
  - exfalso; eapply outcomes_perm_no_error; eassumption.
  - match goal with
    | Hc : eval_query_expr_outcome _ _ _ _ _ _ _ _ q (SqlSuccess ?R),
      Hp : eval_project_rows_outcome _ _ _ _ _ _ _ _ _ _ _ |- _ =>
        pose proof (outcomes_perm_success q L Hq sched R Hc) as HP;
        rewrite (proj1 (project_rows_outcome_iff_exact_map basesort instance unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error value_is_null sched
          nil sel R vf _
          (fun r Hr => Hex sched r (Permutation_in r (Permutation_sym HP) Hr))) Hp)
    end.
    eexists; split; [reflexivity|]; apply Permutation_map; exact HP.
Qed.

(** ** Project over a bag-determined input (representatives up to =t=) *)
Lemma outcomes_bag_project :
  forall sel q L (vf : tuple TNull -> list (value TNull)),
    outcomes_bag q L ->
    (forall sched r r', In r L -> Oeset.compare (OTuple TNull) r r' = Eq ->
       exact_values sched (env_t TNull nil r') (map fst sel) (vf r') /\ vf r' = vf r) ->
    outcomes_perm (QExpr_Project sel q) (map (fun r => project_row sel (vf r)) L).
Proof.
  intros sel q L vf Hq Hex sched o H; inversion H; subst.
  - exfalso; eapply outcomes_bag_no_error; eassumption.
  - match goal with
    | Hc : eval_query_expr_outcome _ _ _ _ _ _ _ _ q (SqlSuccess ?R),
      Hp : eval_project_rows_outcome _ _ _ _ _ _ _ _ _ _ _ |- _ =>
        pose proof (outcomes_bag_success q L Hq sched R Hc) as HP;
        rewrite (proj1 (project_rows_outcome_iff_exact_map basesort instance unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error value_is_null sched
          nil sel R vf _
          (fun r' Hr' =>
             match pod_permut_in_left _ _ _ _ r' HP Hr' with
             | ex_intro _ r (conj Hr Hrr) => proj1 (Hex sched r r' Hr Hrr)
             end)) Hp)
    end.
    eexists; split; [reflexivity|].
    eapply pod_permut_map; [exact HP|].
    intros a b Ha Hab; rewrite (proj2 (Hex sched a b Ha Hab)); reflexivity.
Qed.

(** ** Filter over a permutation-determined input *)
Lemma outcomes_perm_filter :
  forall phi q L (keep : tuple TNull -> bool),
    outcomes_perm q L ->
    (forall sched r, In r L -> accept sched (env_t TNull nil r) phi (keep r)) ->
    outcomes_perm (QExpr_Filter phi q) (filter keep L).
Proof.
  intros phi q L keep Hq Hacc sched o H; inversion H; subst.
  - exfalso; eapply outcomes_perm_no_error; eassumption.
  - match goal with
    | Hc : eval_query_expr_outcome _ _ _ _ _ _ _ _ q (SqlSuccess ?R),
      Hf : eval_filter_rows_outcome _ _ _ _ _ _ _ _ _ _ _ |- _ =>
        pose proof (outcomes_perm_success q L Hq sched R Hc) as HP;
        rewrite (proj1 (eval_filter_rows_acceptance_exact basesort instance unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error value_is_null sched
          nil phi R keep
          (fun r Hr => Hacc sched r (Permutation_in r (Permutation_sym HP) Hr)) _) Hf)
    end.
    eexists; split; [reflexivity|].
    apply pod_Permutation_filter; exact HP.
Qed.

(** ** Filter over a bag-determined input *)
Lemma outcomes_bag_filter :
  forall phi q L (keep : tuple TNull -> bool),
    outcomes_bag q L ->
    (forall sched r r', In r L -> Oeset.compare (OTuple TNull) r r' = Eq ->
       accept sched (env_t TNull nil r') phi (keep r') /\ keep r' = keep r) ->
    outcomes_bag (QExpr_Filter phi q) (filter keep L).
Proof.
  intros phi q L keep Hq Hacc sched o H; inversion H; subst.
  - exfalso; eapply outcomes_bag_no_error; eassumption.
  - match goal with
    | Hc : eval_query_expr_outcome _ _ _ _ _ _ _ _ q (SqlSuccess ?R),
      Hf : eval_filter_rows_outcome _ _ _ _ _ _ _ _ _ _ _ |- _ =>
        pose proof (outcomes_bag_success q L Hq sched R Hc) as HP;
        rewrite (proj1 (eval_filter_rows_acceptance_exact basesort instance unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error value_is_null sched
          nil phi R keep
          (fun r' Hr' =>
             match pod_permut_in_left _ _ _ _ r' HP Hr' with
             | ex_intro _ r (conj Hr Hrr) => proj1 (Hacc sched r r' Hr Hrr)
             end) _) Hf)
    end.
    eexists; split; [reflexivity|].
    apply pod_permut_filter; [exact HP|].
    intros a b Ha Hab; rewrite (proj2 (Hacc sched a b Ha Hab)); reflexivity.
Qed.

(** ** Filter True *)
Lemma pod_filter_true_id :
  forall (X : Type) (l : list X), filter (fun _ => true) l = l.
Proof. intros X l; induction l as [|x l IH]; simpl; [reflexivity|rewrite IH; reflexivity]. Qed.

Lemma outcomes_perm_filter_true :
  forall q L, outcomes_perm q L -> outcomes_perm (QExpr_Filter SExpr_True q) L.
Proof.
  intros q L Hq.
  pose proof (outcomes_perm_filter SExpr_True q L (fun _ => true) Hq
    (fun sched r _ => having_true_acceptance_exact basesort instance unknown3
       NullValues.interp_scalar_operator_runtime_error
       NullValues.interp_aggregate_runtime_error value_is_null sched
       (env_t TNull nil r))) as H.
  rewrite pod_filter_true_id in H; exact H.
Qed.

Lemma outcomes_bag_filter_true :
  forall q L, outcomes_bag q L -> outcomes_bag (QExpr_Filter SExpr_True q) L.
Proof.
  intros q L Hq.
  pose proof (outcomes_bag_filter SExpr_True q L (fun _ => true) Hq
    (fun sched r r' _ _ => conj (having_true_acceptance_exact basesort instance unknown3
       NullValues.interp_scalar_operator_runtime_error
       NullValues.interp_aggregate_runtime_error value_is_null sched
       (env_t TNull nil r')) eq_refl)) as H.
  rewrite pod_filter_true_id in H; exact H.
Qed.

(** ** CrossJoin *)
Lemma pod_brute_perm :
  forall L L' R R',
    Permutation L L' -> Permutation R R' ->
    Permutation (brute_left_join_list (tuple TNull) (join_tuple TNull) L R)
                (brute_left_join_list (tuple TNull) (join_tuple TNull) L' R').
Proof.
  intros L L' R R' HL HR.
  unfold brute_left_join_list, theta_join_list, d_join_list.
  eapply Permutation_trans.
  - apply Permutation_flat_map; exact HL.
  - clear HL L; induction L' as [|x L' IH]; simpl; [constructor|].
    apply Permutation_app; [|exact IH].
    apply Permutation_map; apply pod_Permutation_filter; exact HR.
Qed.

Lemma outcomes_bag_cross_join :
  forall l r L R,
    outcomes_perm l L -> outcomes_perm r R ->
    outcomes_bag (QExpr_CrossJoin l r)
      (brute_left_join_list (tuple TNull) (join_tuple TNull) L R).
Proof.
  intros l r L R Hl Hr sched o H; inversion H; subst.
  - exfalso; eapply outcomes_perm_no_error; [exact Hl|eassumption].
  - exfalso; eapply outcomes_perm_no_error; [exact Hr|eassumption].
  - match goal with
    | Hcl : eval_query_expr_outcome _ _ _ _ _ _ _ _ l (SqlSuccess ?Ll),
      Hcr : eval_query_expr_outcome _ _ _ _ _ _ _ _ r (SqlSuccess ?Rr),
      Hs : query_same_rows_as_bag ?O _ |- _ =>
        pose proof (outcomes_perm_success l L Hl sched Ll Hcl) as HPL;
        pose proof (outcomes_perm_success r R Hr sched Rr Hcr) as HPR;
        pose proof (query_same_rows_as_bag_permut_between TNull _ O _
          (raw_cross_same_rows_as_bag Ll Rr) Hs) as HB
    end.
    eexists; split; [reflexivity|].
    eapply Oeset.permut_trans; [|exact HB].
    apply pod_Permutation_permut;
      [intro; apply Oeset.compare_eq_refl|intros a b c; apply Oeset.compare_eq_trans|].
    apply pod_brute_perm; assumption.
Qed.

(** ** Group (HAVING True, keep-all), output bag determined *)
Lemma outcomes_bag_group :
  forall sel keys terms q G (values : list (tuple TNull) -> list (value TNull)),
    scalar_group_key_terms keys = Some terms ->
    terms <> nil ->
    outcomes_perm q G ->
    (forall rows, @group_keys_runtime_error TNull
       NullValues.interp_scalar_operator_runtime_error
       NullValues.interp_aggregate_runtime_error nil terms rows = None) ->
    (forall sched rep, Oeset.permut (OTuple TNull) G rep ->
       forall g, In g (query_make_groups nil rep terms) ->
       @group_filter_map_exact_at TNull relname basesort instance unknown3
         NullValues.interp_scalar_operator_runtime_error
         NullValues.interp_aggregate_runtime_error value_is_null sched
         nil sel terms SExpr_True (fun _ => true) values g) ->
    (forall g g', In g (query_make_groups nil G terms) ->
       Oeset.compare (OLTuple TNull) g g' = Eq -> values g' = values g) ->
    outcomes_bag (QExpr_Group sel keys SExpr_True q)
      (map (fun g => project_row sel (values g)) (query_make_groups nil G terms)).
Proof.
  intros sel keys terms q G values Hkeys Hne Hq Hrt Hex Hval sched o H.
  assert (Hrep : forall Gr rep,
            eval sched nil q (SqlSuccess Gr) ->
            query_same_rows_as_bag rep (query_rows_bag Gr) ->
            Oeset.permut (OTuple TNull) G rep).
  { intros Gr rep HGr Hs.
    pose proof (outcomes_perm_success q G Hq sched Gr HGr) as HP.
    eapply Oeset.permut_trans.
    - apply pod_Permutation_permut;
        [intro; apply Oeset.compare_eq_refl|intros a b c; apply Oeset.compare_eq_trans|exact HP].
    - eapply (query_same_rows_as_bag_permut_between TNull);
        [apply SqlListFacts.permutation_same_rows_as_bag; apply Permutation_refl|exact Hs]. }
  inversion H; subst.
  - exfalso; eapply outcomes_perm_no_error; eassumption.
  - (* group bag error: impossible *)
    exfalso.
    match goal with
    | HGr : eval_query_expr_outcome _ _ _ _ _ _ _ _ q (SqlSuccess ?Gr),
      Hb : eval_group_bag_outcome _ _ _ _ _ _ _ _ _ _ _ _ (SqlError _) |- _ =>
        inversion Hb; subst
    end.
    + match goal with
      | Ht : scalar_group_key_terms _ = Some ?t, He : group_keys_runtime_error _ _ _ _ _ = Some _ |- _ =>
          rewrite Hkeys in Ht; injection Ht as <-; rewrite Hrt in He; discriminate
      end.
    + match goal with
      | Ht : scalar_group_key_terms _ = Some _ |- _ => rewrite Hkeys in Ht; injection Ht as <-
      end.
      match goal with
      | HGr : eval_query_expr_outcome _ _ _ _ _ _ _ _ q (SqlSuccess ?Gr),
        Hs : query_same_rows_as_bag ?rep (query_rows_bag ?Gr),
        Hg : eval_groups_outcome _ _ _ _ _ _ _ _ _ _ _ _ (SqlError _) |- _ =>
          pose proof (Hrep Gr rep HGr Hs) as HPr;
          pose proof (proj1 (eval_groups_outcome_exact_filter_map basesort instance unknown3
            NullValues.interp_scalar_operator_runtime_error
            NullValues.interp_aggregate_runtime_error value_is_null sched
            nil sel terms SExpr_True _ (fun _ => true) values
            (Hex sched rep HPr) _) Hg) as E;
          discriminate E
      end.
  - (* success *)
    match goal with
    | HGr : eval_query_expr_outcome _ _ _ _ _ _ _ _ q (SqlSuccess ?Gr),
      Hb : eval_group_bag_outcome _ _ _ _ _ _ _ _ _ _ _ _ (SqlSuccess ?ob),
      Hout : query_same_rows_as_bag ?O ?ob |- _ =>
        inversion Hb; subst
    end.
    match goal with
    | Ht : scalar_group_key_terms _ = Some _ |- _ => rewrite Hkeys in Ht; injection Ht as <-
    end.
    match goal with
    | HGr : eval_query_expr_outcome _ _ _ _ _ _ _ _ q (SqlSuccess ?Gr),
      Hs : query_same_rows_as_bag ?rep (query_rows_bag ?Gr),
      Hg : eval_groups_outcome _ _ _ _ _ _ _ _ _ _ _ _ (SqlSuccess ?grs),
      Hgo : query_same_rows_as_bag ?grs ?ob,
      Hout : query_same_rows_as_bag ?O ?ob |- _ =>
        pose proof (Hrep Gr rep HGr Hs) as HPr;
        pose proof (proj1 (eval_groups_outcome_exact_filter_map basesort instance unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error value_is_null sched
          nil sel terms SExpr_True _ (fun _ => true) values
          (Hex sched rep HPr) _) Hg) as Egrs;
        pose proof (query_same_rows_as_bag_permut_between TNull _ O _ Hgo Hout) as HB
    end.
    injection Egrs as Egrs.
    rewrite pod_filter_true_id in Egrs; subst.
    eexists; split; [reflexivity|].
    eapply Oeset.permut_trans; [|exact HB].
    apply pod_Permutation_permut;
      [intro; apply Oeset.compare_eq_refl|intros a b c; apply Oeset.compare_eq_trans|].
    eapply pod_permut_map;
      [exact (query_make_groups_permut_nonempty TNull nil terms G _ Hne HPr)|].
    intros a b Ha Hab; rewrite (Hval a b Ha Hab); reflexivity.
Qed.

(** ** ORDER BY followed by a final Project: every outcome is the projection
    of SOME ordered representative of the input bag (query-specific
    uniqueness of ordered representatives then fixes the result). *)
Lemma outcomes_order_project :
  forall sel0 okeys q GO (vf0 : tuple TNull -> list (value TNull)),
    outcomes_bag q GO ->
    (forall sched r r', In r GO -> Oeset.compare (OTuple TNull) r r' = Eq ->
       exact_values sched (env_t TNull nil r') (map fst sel0) (vf0 r')) ->
    forall sched o,
      eval sched nil (QExpr_Project sel0 (QExpr_OrderBy okeys q)) o ->
      exists O', Oeset.permut (OTuple TNull) GO O' /\
                 ordered_rows value_is_null okeys O' /\
                 o = SqlSuccess (map (fun r => project_row sel0 (vf0 r)) O').
Proof.
  intros sel0 okeys q GO vf0 Hq Hex sched o H.
  inversion H; subst.
  - match goal with
    | He : eval_query_expr_outcome _ _ _ _ _ _ _ _ (QExpr_OrderBy _ _) (SqlError _) |- _ =>
        inversion He; subst; exfalso; eapply outcomes_bag_no_error; eassumption
    end.
  - match goal with
    | Ho : eval_query_expr_outcome _ _ _ _ _ _ _ _ (QExpr_OrderBy _ _) (SqlSuccess ?O') |- _ =>
        inversion Ho; subst
    end.
    match goal with
    | Hc : eval_query_expr_outcome _ _ _ _ _ _ _ _ q (SqlSuccess ?In),
      Hob : order_by_rows _ _ ?In ?O',
      Hp : eval_project_rows_outcome _ _ _ _ _ _ _ _ _ _ _ |- _ =>
        pose proof (outcomes_bag_success q GO Hq sched In Hc) as HPin;
        destruct Hob as [Hsame Hord];
        assert (HPO : Oeset.permut (OTuple TNull) GO O')
          by (eapply Oeset.permut_trans; [exact HPin|];
              eapply (query_same_rows_as_bag_permut_between TNull);
                [apply SqlListFacts.permutation_same_rows_as_bag; apply Permutation_refl
                |exact Hsame]);
        exists O'; split; [exact HPO|split; [exact Hord|]];
        exact (proj1 (project_rows_outcome_iff_exact_map basesort instance unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error value_is_null sched
          nil sel0 O' vf0 _
          (fun r' Hr' =>
             match pod_permut_in_left _ _ _ _ r' HPO Hr' with
             | ex_intro _ r (conj Hr Hrr) => Hex sched r r' Hr Hrr
             end)) Hp)
    end.
Qed.

End PossibleOutcomeDet.

(** * Generic helpers for all-outcomes uniqueness *)

Lemma pod_teq_labels :
  forall r r' X, Oeset.compare (OTuple TNull) r r' = Eq ->
    labels TNull r =S= X -> labels TNull r' =S= X.
Proof.
  intros r r' X Heq HX.
  rewrite (tuple_eq TNull) in Heq; destruct Heq as [Hl _].
  rewrite <- (Fset.equal_eq_1 _ _ _ _ Hl); exact HX.
Qed.

Lemma pod_teq_inS :
  forall r r' a, Oeset.compare (OTuple TNull) r r' = Eq ->
    a inS labels TNull r -> a inS labels TNull r'.
Proof.
  intros r r' a Heq Ha.
  rewrite (tuple_eq TNull) in Heq; destruct Heq as [Hl _].
  rewrite <- (Fset.mem_eq_2 _ _ _ Hl); exact Ha.
Qed.

Lemma pod_teq_dot :
  forall r r' a, Oeset.compare (OTuple TNull) r r' = Eq ->
    a inS labels TNull r -> dot TNull r' a = dot TNull r a.
Proof.
  intros r r' a Heq Ha.
  rewrite (tuple_eq TNull) in Heq; destruct Heq as [_ Hd].
  symmetry; apply Hd; exact Ha.
Qed.

(** A permutation up to [R] factors as a Leibniz permutation followed by a
    pointwise [R]-relation. *)
Lemma pod_permut_Forall2 :
  forall (X : Type) (R : X -> X -> Prop) l1 l2,
    _permut R l1 l2 -> exists l1', Permutation l1 l1' /\ Forall2 R l1' l2.
Proof.
  intros X R l1 l2 H.
  induction H as [|a b l k1 k2 Hab H IH].
  - exists nil; split; constructor.
  - destruct IH as [l' [HP HF]].
    apply Forall2_app_inv_r in HF as [x [y [Hx [Hy ->]]]].
    exists (x ++ a :: y); split.
    + eapply Permutation_trans; [apply perm_skip; exact HP|].
      apply Permutation_middle.
    + apply Forall2_app; [exact Hx|constructor; assumption].
Qed.

Lemma pod_sorted_Forall2_transfer :
  forall (X : Type) (R le : X -> X -> Prop) (P : X -> Prop),
    (forall a a' b b', P a -> P b -> R a a' -> R b b' -> le a' b' -> le a b) ->
    forall C O, Forall2 R C O -> Forall P C -> LocallySorted le O -> LocallySorted le C.
Proof.
  intros X R le P Hresp C O HF.
  induction HF as [|c o C O Hco HF IH]; intros HP HS; [constructor|].
  inversion HP as [|? ? Pc HP']; subst.
  destruct HF as [|c2 o2 C2 O2 Hco2 HF2].
  - constructor.
  - inversion HS as [| |? ? ? HS' Hle]; subst.
    inversion HP' as [|? ? Pc2 _]; subst.
    constructor.
    + apply IH; [exact HP'|exact HS'].
    + exact (Hresp c o c2 o2 Pc Pc2 Hco Hco2 Hle).
Qed.

(** ORDER BY uniqueness: if [le] is a total preorder that is antisymmetric
    (Leibniz) on the representative bag [GO] and respects tuple equivalence,
    then any two ordered representatives observe the same list through any
    [fin] that respects tuple equivalence. *)
Lemma pod_order_unique :
  forall (X : Type) (le : tuple TNull -> tuple TNull -> Prop) (fin : tuple TNull -> X)
         GO O1 O2,
    (forall a b c, In a GO -> In b GO -> In c GO -> le a b -> le b c -> le a c) ->
    (forall a b, In a GO -> In b GO -> le a b -> le b a -> a = b) ->
    (forall a a' b b', In a GO -> In b GO ->
       Oeset.compare (OTuple TNull) a a' = Eq -> Oeset.compare (OTuple TNull) b b' = Eq ->
       le a' b' -> le a b) ->
    (forall a a', In a GO -> Oeset.compare (OTuple TNull) a a' = Eq -> fin a' = fin a) ->
    Oeset.permut (OTuple TNull) GO O1 -> Oeset.permut (OTuple TNull) GO O2 ->
    LocallySorted le O1 -> LocallySorted le O2 ->
    map fin O1 = map fin O2.
Proof.
  intros X le fin GO O1 O2 Htr Hanti Hresp Hfin HP1 HP2 HS1 HS2.
  destruct (pod_permut_Forall2 _ _ _ _ HP1) as [C1 [HC1 HF1]].
  destruct (pod_permut_Forall2 _ _ _ _ HP2) as [C2 [HC2 HF2]].
  assert (HinC : forall C, Permutation GO C -> Forall (fun a => In a GO) C).
  { intros C HC; apply Forall_forall; intros a Ha.
    exact (Permutation_in a (Permutation_sym HC) Ha). }
  assert (HS1' : LocallySorted le C1)
    by (eapply (pod_sorted_Forall2_transfer _ _ le (fun a => In a GO));
        [exact Hresp|exact HF1|exact (HinC C1 HC1)|exact HS1]).
  assert (HS2' : LocallySorted le C2)
    by (eapply (pod_sorted_Forall2_transfer _ _ le (fun a => In a GO));
        [exact Hresp|exact HF2|exact (HinC C2 HC2)|exact HS2]).
  assert (HC : C1 = C2).
  { eapply (pod_sorted_perm_unique _ le (fun a => In a GO)).
    - intros a b c Ha Hb Hc; apply Htr; assumption.
    - intros a b Ha Hb; apply Hanti; assumption.
    - exact (HinC C1 HC1).
    - eapply Permutation_trans; [apply Permutation_sym; exact HC1|exact HC2].
    - exact HS1'.
    - exact HS2'. }
  assert (Hmap : forall C O, Forall2 (fun a b => Oeset.compare (OTuple TNull) a b = Eq) C O ->
                   Forall (fun a => In a GO) C -> map fin O = map fin C).
  { intros C O HF; induction HF as [|c o C O Hco HF IH]; intros HP; [reflexivity|].
    inversion HP as [|? ? Pc HP']; subst; cbn [map].
    rewrite (Hfin c o Pc Hco), (IH HP'); reflexivity. }
  rewrite (Hmap C1 O1 HF1 (HinC C1 HC1)), (Hmap C2 O2 HF2 (HinC C2 HC2)), HC.
  reflexivity.
Qed.

(** Groups formed from any representative of the input bag are, one by one,
    permutations (up to tuple equivalence) of the canonical groups. *)
Lemma pod_group_of_rep :
  forall terms G rep,
    terms <> nil ->
    Oeset.permut (OTuple TNull) G rep ->
    forall g', In g' (query_make_groups nil rep terms) ->
      exists g, In g (query_make_groups nil G terms) /\
        Oeset.compare (OLTuple TNull) g g' = Eq.
Proof.
  intros terms G rep Hne HP g' Hg'.
  pose proof (query_make_groups_permut_nonempty TNull nil terms G rep Hne HP) as HG.
  exact (pod_permut_in_left _ _ _ _ g' HG Hg').
Qed.

Lemma pod_OLTuple_permut :
  forall g g', Oeset.compare (OLTuple TNull) g g' = Eq -> Oeset.permut (OTuple TNull) g g'.
Proof.
  intros g g' H.
  apply (proj2 (Partition.compare_list_t (OTuple TNull) g g')); exact H.
Qed.
