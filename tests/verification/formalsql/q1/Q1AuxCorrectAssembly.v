(* Q1 AuxSpec / Embed helpers supporting the fixed obligation interface.

   Public Q1 Box-1 surface is [Q1Obligations.v] (FilterEq / GroupEq / CountEq /
   Sum*Eq / AvgEq / ProjectionEq / OrderEq).  This file keeps SpecView↔FoldSpec
   and Embed column lemmas those obligations wrap.

   See STATUS.md.  Policy: fail-closed (no open proofs).
   Q1NumericSafe rules out SqlError only; value equalities are proved, not TCB. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueNumericTypmod
  ValueString ValueInteger Formula SchemaConstraints SqlOutcome SqlQuerySyntax
  SqlQuerySemantics SqlQueryFacts SqlErrorSemantics SqlOrder
  Env Interp Projection OrderedSet
  FiniteBag FiniteCollection FiniteSet FTuples Bool3.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax FormalSQL.WitnessFacts
  FormalSQL.AggregateOutcomeBridgeFacts
  FormalSQL.AggregateRuntimeFacts
  FormalSQL.GroupingRewriteFacts
  FormalSQL.NumericRegroupFacts
  FormalSQL.OrderedQueryFacts
  FormalSQL.OrderedObservationTransportFacts
  FormalSQL.ProofAgentFacade.
From LogosGenerated Require Import Schema Queries.
From JetStreamFS Require Import Q1AuxSpec.
From Stdlib Require Import String Ascii List ZArith Lia Ring Sorted Permutation.
Import ListNotations.
Import Tuple.
Import NullValues.
Open Scope string_scope.
Open Scope Z_scope.
Open Scope bool_scope.

Require Import Q1Catalog Q1FormalSQLProof Q1GroupingCorrespondence.

(** * Spec-side helpers *)

Definition row_passes_cutoff (theta : Z) (r : Q1AuxSpec.Row) : bool :=
  Z.leb (Q1AuxSpec.shipdate r) theta.

Definition filtered_rows (db : list Q1AuxSpec.Row) (theta : Z)
  : list Q1AuxSpec.Row :=
  filter (row_passes_cutoff theta) db.

Definition row_contrib (r : Q1AuxSpec.Row) : Q1AuxSpec.Stats :=
  Q1AuxSpec.SpecApply Q1AuxSpec.EmptyStats r 1.

Lemma SpecMerge_Empty_l :
  forall s, Q1AuxSpec.SpecMerge Q1AuxSpec.EmptyStats s = s.
Proof. intros []; reflexivity. Qed.

Lemma SpecMerge_Empty_r :
  forall s, Q1AuxSpec.SpecMerge s Q1AuxSpec.EmptyStats = s.
Proof.
  intros [c q b d ch di];
    unfold Q1AuxSpec.SpecMerge, Q1AuxSpec.EmptyStats; cbn;
    rewrite !Z.add_0_r; reflexivity.
Qed.

Lemma SpecMerge_assoc :
  forall a b c,
    Q1AuxSpec.SpecMerge a (Q1AuxSpec.SpecMerge b c) =
    Q1AuxSpec.SpecMerge (Q1AuxSpec.SpecMerge a b) c.
Proof.
  intros [c1 q1 b1 d1 ch1 di1] [c2 q2 b2 d2 ch2 di2]
         [c3 q3 b3 d3 ch3 di3];
    unfold Q1AuxSpec.SpecMerge; cbn; f_equal; ring.
Qed.

Lemma SpecMerge_comm :
  forall a b, Q1AuxSpec.SpecMerge a b = Q1AuxSpec.SpecMerge b a.
Proof.
  intros [c1 q1 b1 d1 ch1 di1] [c2 q2 b2 d2 ch2 di2];
    unfold Q1AuxSpec.SpecMerge; cbn; f_equal; ring.
Qed.

Lemma SpecApply_as_merge_contrib :
  forall s r,
    Q1AuxSpec.SpecApply s r 1 =
    Q1AuxSpec.SpecMerge s (row_contrib r).
Proof.
  intros [c q b d ch di] r;
    unfold row_contrib, Q1AuxSpec.SpecApply, Q1AuxSpec.SpecMerge,
      Q1AuxSpec.EmptyStats; cbn; f_equal; ring.
Qed.

Fixpoint keystore_get (acc : list (Z * Q1AuxSpec.Stats)) (gkey : Z)
  : Q1AuxSpec.Stats :=
  match acc with
  | [] => Q1AuxSpec.EmptyStats
  | (k, s) :: rest =>
      if Z.eqb k gkey then s else keystore_get rest gkey
  end.

Lemma keystore_put_get :
  forall acc gkey s,
    keystore_get (Q1AuxSpec.keystore_put acc gkey s) gkey =
    Q1AuxSpec.SpecMerge (keystore_get acc gkey) s.
Proof.
  induction acc as [|[k s0] rest IH]; intros gkey s; cbn.
  - rewrite Z.eqb_refl, SpecMerge_Empty_l. reflexivity.
  - destruct (Z.eqb k gkey) eqn:Ek.
    + cbn. rewrite Ek. reflexivity.
    + cbn. rewrite Ek. apply IH.
Qed.

Lemma keystore_put_get_other :
  forall acc gkey s gkey',
    gkey <> gkey' ->
    keystore_get (Q1AuxSpec.keystore_put acc gkey s) gkey' =
    keystore_get acc gkey'.
Proof.
  induction acc as [|[k s0] rest IH]; intros gkey s gkey' Hne; cbn.
  - apply Z.eqb_neq in Hne; rewrite Hne; reflexivity.
  - destruct (Z.eqb k gkey) eqn:Ek.
    + cbn. apply Z.eqb_eq in Ek; subst k.
      destruct (Z.eqb gkey gkey') eqn:Eg.
      * apply Z.eqb_eq in Eg; contradiction.
      * reflexivity.
    + cbn. destruct (Z.eqb k gkey') eqn:Ek'; [reflexivity|].
      apply IH; assumption.
Qed.

Fixpoint sum_contrib_matching (rows : list Q1AuxSpec.Row) (gkey : Z)
  : Q1AuxSpec.Stats :=
  match rows with
  | [] => Q1AuxSpec.EmptyStats
  | r :: rest =>
      let acc := sum_contrib_matching rest gkey in
      if Z.eqb (Q1AuxSpec.RowKey r) gkey
      then Q1AuxSpec.SpecMerge acc (row_contrib r)
      else acc
  end.

Lemma cellkey_eqb_sym :
  forall a b, Q1AuxSpec.cellkey_eqb a b = Q1AuxSpec.cellkey_eqb b a.
Proof.
  intros [d1 g1] [d2 g2]; unfold Q1AuxSpec.cellkey_eqb; cbn.
  rewrite (Z.eqb_sym d1 d2), (Z.eqb_sym g1 g2); reflexivity.
Qed.

Lemma cellkey_eqb_refl :
  forall k, Q1AuxSpec.cellkey_eqb k k = true.
Proof.
  intros [d g]; unfold Q1AuxSpec.cellkey_eqb; cbn; rewrite !Z.eqb_refl; reflexivity.
Qed.

Lemma store_get_remove_same :
  forall st k,
    Q1AuxSpec.store_get (Q1AuxSpec.store_remove st k) k = Q1AuxSpec.EmptyStats.
Proof.
  induction st as [|[k0 s0] st_rest IH]; intros k; [reflexivity|].
  cbn [Q1AuxSpec.store_remove filter].
  destruct (Q1AuxSpec.cellkey_eqb k0 k) eqn:E; cbn [negb].
  - (* removed head *) apply IH.
  - cbn [Q1AuxSpec.store_get].
    rewrite (cellkey_eqb_sym k k0), E.
    apply IH.
Qed.

Lemma store_get_remove_other :
  forall st k k',
    Q1AuxSpec.cellkey_eqb k k' = false ->
    Q1AuxSpec.store_get (Q1AuxSpec.store_remove st k) k' =
    Q1AuxSpec.store_get st k'.
Proof.
  induction st as [|[k0 s0] st_rest IH]; intros k k' Hne; [reflexivity|].
  cbn [Q1AuxSpec.store_remove filter].
  destruct (Q1AuxSpec.cellkey_eqb k0 k) eqn:E0; cbn [negb].
  - (* head removed: was k0=k, so store_get st k' skipped head if k0=k' would mean k=k' *)
    cbn [Q1AuxSpec.store_get].
    destruct (Q1AuxSpec.cellkey_eqb k' k0) eqn:E0k.
    + (* k'=k0=k, contradict Hne *)
      unfold Q1AuxSpec.cellkey_eqb in E0, E0k, Hne.
      destruct k0 as [a b]; destruct k as [c d]; destruct k' as [e f]; cbn in *.
      destruct (Z.eqb a c) eqn:A1; [|discriminate].
      destruct (Z.eqb b d) eqn:A2; [|discriminate].
      destruct (Z.eqb e a) eqn:B1; [|discriminate].
      destruct (Z.eqb f b) eqn:B2; [|discriminate].
      apply Z.eqb_eq in A1, A2, B1, B2; subst.
      rewrite !Z.eqb_refl in Hne; discriminate.
    + apply IH; assumption.
  - cbn [Q1AuxSpec.store_get].
    destruct (Q1AuxSpec.cellkey_eqb k' k0) eqn:E0k; [reflexivity|].
    apply IH; assumption.
Qed.

Lemma SpecApply_count_sign1 :
  forall s r,
    Q1AuxSpec.count (Q1AuxSpec.SpecApply s r 1) = Q1AuxSpec.count s + 1.
Proof. intros [] r; reflexivity. Qed.

Lemma FoldSpec_store_get_count_nonneg :
  forall db k,
    0 <= Q1AuxSpec.count (Q1AuxSpec.store_get (Q1AuxSpec.FoldSpec db) k).
Proof.
  induction db as [|r rest IH]; intros k; [cbn; lia|].
  cbn [Q1AuxSpec.FoldSpec].
  unfold Q1AuxSpec.SpecApplyDelta.
  pose proof (IH (Q1AuxSpec.shipdate r, Q1AuxSpec.RowKey r)) as Hnn.
  assert (Hpos :
    0 < Q1AuxSpec.count
          (Q1AuxSpec.SpecApply
             (Q1AuxSpec.store_get (Q1AuxSpec.FoldSpec rest)
                (Q1AuxSpec.shipdate r, Q1AuxSpec.RowKey r)) r 1)).
  { rewrite SpecApply_count_sign1; lia. }
  destruct (Z.eqb
    (Q1AuxSpec.count
      (Q1AuxSpec.SpecApply
        (Q1AuxSpec.store_get (Q1AuxSpec.FoldSpec rest)
           (Q1AuxSpec.shipdate r, Q1AuxSpec.RowKey r)) r 1)) 0) eqn:Ez.
  - apply Z.eqb_eq in Ez; lia.
  - cbn [Q1AuxSpec.store_get].
    (* store_get looks up with [cellkey_eqb lookup head] *)
    destruct (Q1AuxSpec.cellkey_eqb k
                (Q1AuxSpec.shipdate r, Q1AuxSpec.RowKey r)) eqn:Ekk.
    + apply Z.lt_le_incl; exact Hpos.
    + rewrite store_get_remove_other
        by (rewrite cellkey_eqb_sym; exact Ekk); apply IH.
Qed.

Lemma FoldSpec_cons_form :
  forall r rest,
    let k := (Q1AuxSpec.shipdate r, Q1AuxSpec.RowKey r) in
    let st := Q1AuxSpec.FoldSpec rest in
    let s2 := Q1AuxSpec.SpecApply (Q1AuxSpec.store_get st k) r 1 in
    Q1AuxSpec.count s2 <> 0 /\
    Q1AuxSpec.FoldSpec (r :: rest) =
      (k, s2) :: Q1AuxSpec.store_remove st k.
Proof.
  intros r rest.
  set (k := (Q1AuxSpec.shipdate r, Q1AuxSpec.RowKey r)).
  set (st := Q1AuxSpec.FoldSpec rest).
  set (s2 := Q1AuxSpec.SpecApply (Q1AuxSpec.store_get st k) r 1).
  split.
  - subst s2.
    unfold Q1AuxSpec.SpecApply.
    pose proof (FoldSpec_store_get_count_nonneg rest k) as Hnn.
    fold st in Hnn.
    destruct (Q1AuxSpec.store_get st k) as [c q b d ch di]; cbn in Hnn |- *; lia.
  - cbn [Q1AuxSpec.FoldSpec].
    unfold Q1AuxSpec.SpecApplyDelta.
    fold k; fold st; fold s2.
    destruct (Z.eqb (Q1AuxSpec.count s2) 0) eqn:Ez.
    + apply Z.eqb_eq in Ez.
      subst s2.
      unfold Q1AuxSpec.SpecApply in Ez.
      pose proof (FoldSpec_store_get_count_nonneg rest k) as Hnn.
      fold st in Hnn.
      destruct (Q1AuxSpec.store_get st k) as [c q b d ch di]; cbn in Ez, Hnn; lia.
    + reflexivity.
Qed.

(** Explicit cons equality without local [let] (easier to rewrite). *)
Lemma FoldSpec_cons_eq :
  forall r rest,
    Q1AuxSpec.FoldSpec (r :: rest) =
      ((Q1AuxSpec.shipdate r, Q1AuxSpec.RowKey r),
       Q1AuxSpec.SpecApply
         (Q1AuxSpec.store_get (Q1AuxSpec.FoldSpec rest)
            (Q1AuxSpec.shipdate r, Q1AuxSpec.RowKey r))
         r 1)
      :: Q1AuxSpec.store_remove (Q1AuxSpec.FoldSpec rest)
           (Q1AuxSpec.shipdate r, Q1AuxSpec.RowKey r).
Proof.
  intros r rest.
  exact (proj2 (FoldSpec_cons_form r rest)).
Qed.

(** * SpecView ↔ filter via sum_cells bridge (Qed below).

    [FoldSpec_cons_eq] is the remove-before-cons shape used by assembly.
    [SpecView_FoldSpec_key_stats] = [keystore_get_SpecView_sum]
      ∘ [sum_cells_FoldSpec_eq_contrib]. *)

Fixpoint sum_cells_matching (st : Q1AuxSpec.Store) (cutoff gkey : Z)
  : Q1AuxSpec.Stats :=
  match st with
  | [] => Q1AuxSpec.EmptyStats
  | ((day, gk), s) :: rest =>
      let acc := sum_cells_matching rest cutoff gkey in
      if Z.leb day cutoff && Z.eqb gk gkey
      then Q1AuxSpec.SpecMerge acc s
      else acc
  end.

Lemma keystore_get_SpecView_sum :
  forall st cutoff gkey,
    keystore_get (Q1AuxSpec.SpecView st cutoff) gkey =
    sum_cells_matching st cutoff gkey.
Proof.
  induction st as [|[k s] rest IH]; intros cutoff gkey; [reflexivity|].
  destruct k as [day gk].
  cbn [Q1AuxSpec.SpecView sum_cells_matching].
  destruct (Z.leb day cutoff) eqn:Ed.
  - destruct (Z.eqb_spec gk gkey) as [Hg|Hg].
    + subst. rewrite keystore_put_get, IH. reflexivity.
    + rewrite keystore_put_get_other by exact Hg.
      rewrite IH.
      destruct (Z.eqb gk gkey) eqn:E; [|reflexivity].
      apply Z.eqb_eq in E; contradiction.
  - rewrite IH. reflexivity.
Qed.

(** [sum_cells] of a FoldSpec store after remove-before-cons write. *)
Lemma sum_cells_FoldSpec_cons :
  forall r rest cutoff gkey,
    let day := Q1AuxSpec.shipdate r in
    let rk := Q1AuxSpec.RowKey r in
    let k := (day, rk) in
    let st := Q1AuxSpec.FoldSpec rest in
    let prior := Q1AuxSpec.store_get st k in
    let s2 := Q1AuxSpec.SpecApply prior r 1 in
    sum_cells_matching ((k, s2) :: Q1AuxSpec.store_remove st k) cutoff gkey =
    if Z.leb day cutoff && Z.eqb rk gkey
    then Q1AuxSpec.SpecMerge
           (sum_cells_matching (Q1AuxSpec.store_remove st k) cutoff gkey) s2
    else sum_cells_matching (Q1AuxSpec.store_remove st k) cutoff gkey.
Proof.
  intros r rest cutoff gkey day rk k st prior s2.
  cbn [sum_cells_matching]. reflexivity.
Qed.

(** [sum_cells] after removing write-key [k] of a FoldSpec store. *)
Lemma store_remove_idempotent :
  forall st k,
    Q1AuxSpec.store_remove (Q1AuxSpec.store_remove st k) k =
    Q1AuxSpec.store_remove st k.
Proof.
  induction st as [|[k0 s0] rest IH]; intros k; [reflexivity|].
  unfold Q1AuxSpec.store_remove; cbn [filter].
  destruct (Q1AuxSpec.cellkey_eqb k0 k) eqn:E; cbn [negb].
  - apply IH.
  - cbn [filter]. rewrite E; cbn [negb]. f_equal. apply IH.
Qed.

(** FoldSpec stores have unique cell keys (remove-before-cons). *)
Definition keys_NoDup (st : Q1AuxSpec.Store) : Prop :=
  NoDup (map fst st).

Lemma store_remove_notin_key :
  forall st k,
    ~ In k (map fst (Q1AuxSpec.store_remove st k)).
Proof.
  intros st k H.
  apply in_map_iff in H.
  destruct H as [[k' s'] [Hk Hin]].
  simpl in Hk; subst k'.
  unfold Q1AuxSpec.store_remove in Hin.
  apply filter_In in Hin.
  destruct Hin as [_ Hneg].
  rewrite cellkey_eqb_refl in Hneg.
  discriminate.
Qed.

Lemma in_map_fst_store_remove :
  forall st k k',
    In k' (map fst (Q1AuxSpec.store_remove st k)) ->
    In k' (map fst st).
Proof.
  induction st as [|[k0 s0] rest IH]; intros k k'; [simpl; tauto|].
  unfold Q1AuxSpec.store_remove; cbn [filter map].
  destruct (Q1AuxSpec.cellkey_eqb k0 k) eqn:E; cbn [negb].
  - intros Hin; right; eapply IH; exact Hin.
  - intros [Heq|Hin]; [left; exact Heq|right; eapply IH; exact Hin].
Qed.

Lemma store_remove_preserves_NoDup :
  forall st k,
    keys_NoDup st ->
    keys_NoDup (Q1AuxSpec.store_remove st k).
Proof.
  induction st as [|[k0 s0] rest IH]; intros k Hnd; [constructor|].
  unfold keys_NoDup in *.
  inversion Hnd as [|? ? Hnin Hnd']; subst; clear Hnd.
  unfold Q1AuxSpec.store_remove; cbn [filter map].
  destruct (Q1AuxSpec.cellkey_eqb k0 k) eqn:E; cbn [negb].
  - apply IH; exact Hnd'.
  - constructor.
    + intros Hin. apply Hnin.
      change (In k0 (map fst (Q1AuxSpec.store_remove rest k))) in Hin.
      eapply in_map_fst_store_remove; exact Hin.
    + apply IH; exact Hnd'.
Qed.

Lemma store_remove_absent :
  forall st k,
    ~ In k (map fst st) ->
    Q1AuxSpec.store_remove st k = st.
Proof.
  induction st as [|[k0 s0] rest IH]; intros k Hnin; [reflexivity|].
  unfold Q1AuxSpec.store_remove; cbn [filter].
  destruct (Q1AuxSpec.cellkey_eqb k0 k) eqn:E; cbn [negb].
  - (* k0 = k contradicts ~In *)
    unfold Q1AuxSpec.cellkey_eqb in E.
    destruct k0 as [a b]; destruct k as [c d]; cbn in E.
    destruct (Z.eqb a c) eqn:Ea; [|discriminate].
    destruct (Z.eqb b d) eqn:Eb; [|discriminate].
    apply Z.eqb_eq in Ea, Eb; subst.
    exfalso; apply Hnin; left; reflexivity.
  - f_equal. apply IH. intros Hin; apply Hnin; right; exact Hin.
Qed.

Lemma FoldSpec_keys_NoDup :
  forall db, keys_NoDup (Q1AuxSpec.FoldSpec db).
Proof.
  induction db as [|r rest IH].
  - constructor.
  - rewrite FoldSpec_cons_eq.
    unfold keys_NoDup; cbn [map].
    constructor.
    + apply store_remove_notin_key.
    + apply store_remove_preserves_NoDup; exact IH.
Qed.

(** [sum_cells] after removing an arbitrary cell key, under unique keys. *)
Lemma sum_cells_remove_key_NoDup :
  forall st (k : Q1AuxSpec.CellKey) cutoff gkey,
    keys_NoDup st ->
    sum_cells_matching st cutoff gkey =
    Q1AuxSpec.SpecMerge
      (sum_cells_matching (Q1AuxSpec.store_remove st k) cutoff gkey)
      (if Z.leb (fst k) cutoff && Z.eqb (snd k) gkey
       then Q1AuxSpec.store_get st k
       else Q1AuxSpec.EmptyStats).
Proof.
  induction st as [|[k0 s0] rest IH]; intros [day gk] cutoff gkey Hnd.
  - cbn. destruct (_ && _); rewrite SpecMerge_Empty_l; reflexivity.
  - unfold keys_NoDup in Hnd.
    inversion Hnd as [|? ? Hnin Hnd']; subst; clear Hnd.
    destruct k0 as [day0 gk0].
    cbn [fst snd] in Hnin |- *.
    destruct (Q1AuxSpec.cellkey_eqb (day0, gk0) (day, gk)) eqn:Ek.
    + (* head key equals [k] *)
      unfold Q1AuxSpec.cellkey_eqb in Ek; cbn in Ek.
      destruct (Z.eqb day0 day) eqn:Ed0; [|discriminate].
      destruct (Z.eqb gk0 gk) eqn:Eg0; [|discriminate].
      apply Z.eqb_eq in Ed0, Eg0; subst day0 gk0.
      match goal with
      | |- context [Q1AuxSpec.store_remove ?st (day, gk)] =>
          assert (Hrm : Q1AuxSpec.store_remove st (day, gk) = rest);
          [| rewrite Hrm]
      end.
      { unfold Q1AuxSpec.store_remove; cbn [filter].
        rewrite (cellkey_eqb_refl (day, gk)); cbn [negb].
        apply store_remove_absent; exact Hnin. }
      cbn [sum_cells_matching Q1AuxSpec.store_get].
      match goal with
      | |- context [Q1AuxSpec.cellkey_eqb ?k ?k] =>
          rewrite (cellkey_eqb_refl k)
      end.
      destruct (Z.leb day cutoff) eqn:Ed;
        destruct (Z.eqb gk gkey) eqn:Eg; cbn [andb];
        try rewrite SpecMerge_Empty_r; reflexivity.
    + (* head kept *)
      match goal with
      | |- context [Q1AuxSpec.store_remove ?st (day, gk)] =>
          assert (Hrm :
            Q1AuxSpec.store_remove st (day, gk) =
            ((day0, gk0), s0) :: Q1AuxSpec.store_remove rest (day, gk));
          [| rewrite Hrm]
      end.
      { unfold Q1AuxSpec.store_remove; cbn [filter]; rewrite Ek; reflexivity. }
      cbn [sum_cells_matching Q1AuxSpec.store_get].
      rewrite (cellkey_eqb_sym (day, gk) (day0, gk0)), Ek.
      assert (IH' := IH (day, gk) cutoff gkey Hnd').
      cbn [fst snd] in IH'.
      destruct (Z.leb day0 cutoff && Z.eqb gk0 gkey) eqn:Em0.
      * rewrite IH'.
        rewrite <- SpecMerge_assoc.
        rewrite (SpecMerge_comm
                   (if Z.leb day cutoff && Z.eqb gk gkey
                    then Q1AuxSpec.store_get rest (day, gk)
                    else Q1AuxSpec.EmptyStats) s0).
        rewrite SpecMerge_assoc.
        reflexivity.
      * rewrite IH'. reflexivity.
Qed.

Lemma sum_cells_FoldSpec_remove_key :
  forall rest r cutoff gkey,
    sum_cells_matching (Q1AuxSpec.FoldSpec rest) cutoff gkey =
    Q1AuxSpec.SpecMerge
      (sum_cells_matching
         (Q1AuxSpec.store_remove (Q1AuxSpec.FoldSpec rest)
            (Q1AuxSpec.shipdate r, Q1AuxSpec.RowKey r))
         cutoff gkey)
      (if Z.leb (Q1AuxSpec.shipdate r) cutoff &&
          Z.eqb (Q1AuxSpec.RowKey r) gkey
       then Q1AuxSpec.store_get (Q1AuxSpec.FoldSpec rest)
              (Q1AuxSpec.shipdate r, Q1AuxSpec.RowKey r)
       else Q1AuxSpec.EmptyStats).
Proof.
  intros rest r cutoff gkey.
  pose proof (sum_cells_remove_key_NoDup
                (Q1AuxSpec.FoldSpec rest)
                (Q1AuxSpec.shipdate r, Q1AuxSpec.RowKey r)
                cutoff gkey
                (FoldSpec_keys_NoDup rest)) as H.
  cbn [fst snd] in H. exact H.
Qed.

Lemma sum_cells_FoldSpec_eq_contrib :
  forall db cutoff gkey,
    sum_cells_matching (Q1AuxSpec.FoldSpec db) cutoff gkey =
    sum_contrib_matching
      (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) cutoff) db) gkey.
Proof.
  induction db as [|r rest IH]; intros cutoff gkey; [reflexivity|].
  rewrite FoldSpec_cons_eq.
  cbn [sum_cells_matching filter].
  pose proof (sum_cells_FoldSpec_remove_key rest r cutoff gkey) as Hrem.
  destruct (Z.leb (Q1AuxSpec.shipdate r) cutoff) eqn:Ed;
    destruct (Z.eqb (Q1AuxSpec.RowKey r) gkey) eqn:Eg; cbn [andb].
  - (* row contributes: destruct already reduced Hrem's guard to true&&true *)
    cbn [sum_contrib_matching]. rewrite Eg.
    rewrite SpecApply_as_merge_contrib.
    rewrite SpecMerge_assoc.
    assert (Htmp := Hrem).
    cbn [andb] in Htmp.
    rewrite (eq_sym Htmp).
    apply (f_equal (fun s => Q1AuxSpec.SpecMerge s (row_contrib r))).
    apply IH.
  - cbn [sum_contrib_matching]; rewrite Eg.
    assert (Htmp := Hrem).
    cbn [andb] in Htmp.
    rewrite SpecMerge_Empty_r in Htmp.
    rewrite <- Htmp.
    apply IH.
  - assert (Htmp := Hrem).
    cbn [andb] in Htmp.
    rewrite SpecMerge_Empty_r in Htmp.
    rewrite <- Htmp.
    apply IH.
  - assert (Htmp := Hrem).
    cbn [andb] in Htmp.
    rewrite SpecMerge_Empty_r in Htmp.
    rewrite <- Htmp.
    apply IH.
Qed.

Lemma SpecView_FoldSpec_key_stats :
  forall db cutoff gkey,
    keystore_get (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) cutoff) gkey =
    sum_contrib_matching
      (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) cutoff) db) gkey.
Proof.
  intros db cutoff gkey.
  rewrite keystore_get_SpecView_sum.
  apply sum_cells_FoldSpec_eq_contrib.
Qed.

Lemma SpecFinalizeOne_positive :
  forall gkey s,
    0 < Q1AuxSpec.count s ->
    Q1AuxSpec.SpecFinalizeOne gkey s =
    let '(rf, ls) := Q1AuxSpec.Unpack gkey in
    [Q1AuxSpec.mkOutRow rf ls
       (Q1AuxSpec.sum_qty s) (Q1AuxSpec.sum_base_price s)
       (Q1AuxSpec.sum_disc_price s) (Q1AuxSpec.sum_charge s)
       (Q1AuxSpec.sum_discount s)
       (Q1AuxSpec.AvgRound (Q1AuxSpec.sum_qty s) (Q1AuxSpec.count s))
       (Q1AuxSpec.AvgRound (Q1AuxSpec.sum_base_price s) (Q1AuxSpec.count s))
       (Q1AuxSpec.AvgRound (Q1AuxSpec.sum_discount s) (Q1AuxSpec.count s))
       (Q1AuxSpec.count s)].
Proof.
  intros gkey s Hpos.
  unfold Q1AuxSpec.SpecFinalizeOne.
  destruct (Z.leb (Q1AuxSpec.count s) 0) eqn:Ele.
  - apply Z.leb_le in Ele; lia.
  - reflexivity.
Qed.

Lemma sum_contrib_count :
  forall rows gkey,
    Q1AuxSpec.count (sum_contrib_matching rows gkey) =
    Z.of_nat (length (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey) rows)).
Proof.
  induction rows as [|r rest IH]; intros gkey; [reflexivity|].
  cbn [sum_contrib_matching filter length].
  destruct (Z.eqb (Q1AuxSpec.RowKey r) gkey) eqn:Ek.
  - unfold Q1AuxSpec.SpecMerge, row_contrib, Q1AuxSpec.SpecApply,
      Q1AuxSpec.EmptyStats; cbn.
    rewrite IH.
    destruct (sum_contrib_matching rest gkey); cbn; lia.
  - apply IH.
Qed.

(** SpecView count column = filtered key-bucket cardinality (COUNT bridge). *)
Lemma SpecView_FoldSpec_key_count :
  forall db cutoff gkey,
    Q1AuxSpec.count
      (keystore_get (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) cutoff) gkey) =
    Z.of_nat
      (length
        (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey)
          (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) cutoff) db))).
Proof.
  intros db cutoff gkey.
  rewrite SpecView_FoldSpec_key_stats.
  apply sum_contrib_count.
Qed.

(** SpecView sum_qty / sum_base_price = Z-sum of matching filtered rows. *)
Lemma row_contrib_sum_qty :
  forall r, Q1AuxSpec.sum_qty (row_contrib r) = Q1AuxSpec.qty r.
Proof.
  intro r.
  unfold row_contrib, Q1AuxSpec.SpecApply, Q1AuxSpec.EmptyStats.
  simpl.
  destruct (Q1AuxSpec.qty r); reflexivity.
Qed.

Lemma row_contrib_sum_base_price :
  forall r, Q1AuxSpec.sum_base_price (row_contrib r) = Q1AuxSpec.price r.
Proof.
  intro r.
  unfold row_contrib, Q1AuxSpec.SpecApply, Q1AuxSpec.EmptyStats.
  simpl.
  destruct (Q1AuxSpec.price r); reflexivity.
Qed.

Lemma SpecMerge_sum_qty :
  forall a b,
    Q1AuxSpec.sum_qty (Q1AuxSpec.SpecMerge a b) =
    Q1AuxSpec.sum_qty a + Q1AuxSpec.sum_qty b.
Proof. intros [] []; reflexivity. Qed.

Lemma SpecMerge_sum_base_price :
  forall a b,
    Q1AuxSpec.sum_base_price (Q1AuxSpec.SpecMerge a b) =
    Q1AuxSpec.sum_base_price a + Q1AuxSpec.sum_base_price b.
Proof. intros [] []; reflexivity. Qed.

Lemma sum_contrib_sum_qty :
  forall rows gkey,
    Q1AuxSpec.sum_qty (sum_contrib_matching rows gkey) =
    fold_right Z.add 0
      (map Q1AuxSpec.qty
        (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey) rows)).
Proof.
  induction rows as [|r rest IH]; intros gkey; [reflexivity|].
  cbn [sum_contrib_matching filter map fold_right].
  destruct (Z.eqb (Q1AuxSpec.RowKey r) gkey) eqn:Ek.
  - rewrite SpecMerge_sum_qty, row_contrib_sum_qty, IH.
    apply Z.add_comm.
  - apply IH.
Qed.

Lemma sum_contrib_sum_base_price :
  forall rows gkey,
    Q1AuxSpec.sum_base_price (sum_contrib_matching rows gkey) =
    fold_right Z.add 0
      (map Q1AuxSpec.price
        (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey) rows)).
Proof.
  induction rows as [|r rest IH]; intros gkey; [reflexivity|].
  cbn [sum_contrib_matching filter map fold_right].
  destruct (Z.eqb (Q1AuxSpec.RowKey r) gkey) eqn:Ek.
  - rewrite SpecMerge_sum_base_price, row_contrib_sum_base_price, IH.
    apply Z.add_comm.
  - apply IH.
Qed.

Lemma SpecView_FoldSpec_key_sum_qty :
  forall db cutoff gkey,
    Q1AuxSpec.sum_qty
      (keystore_get (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) cutoff) gkey) =
    fold_right Z.add 0
      (map Q1AuxSpec.qty
        (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey)
          (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) cutoff) db))).
Proof.
  intros db cutoff gkey.
  rewrite SpecView_FoldSpec_key_stats.
  apply sum_contrib_sum_qty.
Qed.

Lemma SpecView_FoldSpec_key_sum_base_price :
  forall db cutoff gkey,
    Q1AuxSpec.sum_base_price
      (keystore_get (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) cutoff) gkey) =
    fold_right Z.add 0
      (map Q1AuxSpec.price
        (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey)
          (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) cutoff) db))).
Proof.
  intros db cutoff gkey.
  rewrite SpecView_FoldSpec_key_stats.
  apply sum_contrib_sum_base_price.
Qed.

Lemma SpecMerge_sum_disc_price :
  forall a b,
    Q1AuxSpec.sum_disc_price (Q1AuxSpec.SpecMerge a b) =
    Q1AuxSpec.sum_disc_price a + Q1AuxSpec.sum_disc_price b.
Proof. intros [] []; reflexivity. Qed.

Lemma SpecMerge_sum_charge :
  forall a b,
    Q1AuxSpec.sum_charge (Q1AuxSpec.SpecMerge a b) =
    Q1AuxSpec.sum_charge a + Q1AuxSpec.sum_charge b.
Proof. intros [] []; reflexivity. Qed.

Lemma SpecMerge_sum_discount :
  forall a b,
    Q1AuxSpec.sum_discount (Q1AuxSpec.SpecMerge a b) =
    Q1AuxSpec.sum_discount a + Q1AuxSpec.sum_discount b.
Proof. intros [] []; reflexivity. Qed.

Lemma row_contrib_sum_disc_price :
  forall r,
    Q1AuxSpec.sum_disc_price (row_contrib r) =
    Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r).
Proof.
  intros r.
  unfold row_contrib, Q1AuxSpec.SpecApply, Q1AuxSpec.EmptyStats.
  cbn [Q1AuxSpec.sum_disc_price].
  rewrite Z.add_0_l, Z.mul_1_l.
  reflexivity.
Qed.

Lemma row_contrib_sum_charge :
  forall r,
    Q1AuxSpec.sum_charge (row_contrib r) =
    Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r) * (100 + Q1AuxSpec.tax r).
Proof.
  intros r.
  unfold row_contrib, Q1AuxSpec.SpecApply, Q1AuxSpec.EmptyStats.
  cbn [Q1AuxSpec.sum_charge].
  rewrite Z.add_0_l, Z.mul_1_l.
  reflexivity.
Qed.

Lemma row_contrib_sum_discount :
  forall r,
    Q1AuxSpec.sum_discount (row_contrib r) = Q1AuxSpec.disc r.
Proof.
  intros r.
  unfold row_contrib, Q1AuxSpec.SpecApply, Q1AuxSpec.EmptyStats.
  cbn [Q1AuxSpec.sum_discount].
  rewrite Z.add_0_l, Z.mul_1_l.
  reflexivity.
Qed.

Lemma sum_contrib_sum_disc_price :
  forall rows gkey,
    Q1AuxSpec.sum_disc_price (sum_contrib_matching rows gkey) =
    fold_right Z.add 0
      (map (fun r => Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r))
        (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey) rows)).
Proof.
  induction rows as [|r rest IH]; intros gkey; [reflexivity|].
  cbn [sum_contrib_matching filter map fold_right].
  destruct (Z.eqb (Q1AuxSpec.RowKey r) gkey) eqn:Ek.
  - rewrite SpecMerge_sum_disc_price, row_contrib_sum_disc_price, IH.
    apply Z.add_comm.
  - apply IH.
Qed.

Lemma sum_contrib_sum_charge :
  forall rows gkey,
    Q1AuxSpec.sum_charge (sum_contrib_matching rows gkey) =
    fold_right Z.add 0
      (map (fun r => Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r)
                       * (100 + Q1AuxSpec.tax r))
        (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey) rows)).
Proof.
  induction rows as [|r rest IH]; intros gkey; [reflexivity|].
  cbn [sum_contrib_matching filter map fold_right].
  destruct (Z.eqb (Q1AuxSpec.RowKey r) gkey) eqn:Ek.
  - rewrite SpecMerge_sum_charge, row_contrib_sum_charge, IH.
    apply Z.add_comm.
  - apply IH.
Qed.

Lemma sum_contrib_sum_discount :
  forall rows gkey,
    Q1AuxSpec.sum_discount (sum_contrib_matching rows gkey) =
    fold_right Z.add 0
      (map Q1AuxSpec.disc
        (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey) rows)).
Proof.
  induction rows as [|r rest IH]; intros gkey; [reflexivity|].
  cbn [sum_contrib_matching filter map fold_right].
  destruct (Z.eqb (Q1AuxSpec.RowKey r) gkey) eqn:Ek.
  - rewrite SpecMerge_sum_discount, row_contrib_sum_discount, IH.
    apply Z.add_comm.
  - apply IH.
Qed.

Lemma SpecView_FoldSpec_key_sum_disc_price :
  forall db cutoff gkey,
    Q1AuxSpec.sum_disc_price
      (keystore_get (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) cutoff) gkey) =
    fold_right Z.add 0
      (map (fun r => Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r))
        (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey)
          (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) cutoff) db))).
Proof.
  intros db cutoff gkey.
  rewrite SpecView_FoldSpec_key_stats.
  apply sum_contrib_sum_disc_price.
Qed.

Lemma SpecView_FoldSpec_key_sum_charge :
  forall db cutoff gkey,
    Q1AuxSpec.sum_charge
      (keystore_get (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) cutoff) gkey) =
    fold_right Z.add 0
      (map (fun r => Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r)
                       * (100 + Q1AuxSpec.tax r))
        (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey)
          (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) cutoff) db))).
Proof.
  intros db cutoff gkey.
  rewrite SpecView_FoldSpec_key_stats.
  apply sum_contrib_sum_charge.
Qed.

Lemma SpecView_FoldSpec_key_sum_discount :
  forall db cutoff gkey,
    Q1AuxSpec.sum_discount
      (keystore_get (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) cutoff) gkey) =
    fold_right Z.add 0
      (map Q1AuxSpec.disc
        (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey)
          (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) cutoff) db))).
Proof.
  intros db cutoff gkey.
  rewrite SpecView_FoldSpec_key_stats.
  apply sum_contrib_sum_discount.
Qed.

(** * Embed column reads *)

Lemma lineitem_attr_inS_qty :
  AttrDecimal "l_quantity" 15 2 inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrDecimal "l_quantity" 15 2 inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_price :
  AttrDecimal "l_extendedprice" 15 2 inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrDecimal "l_extendedprice" 15 2 inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_disc :
  AttrDecimal "l_discount" 15 2 inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrDecimal "l_discount" 15 2 inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_tax :
  AttrDecimal "l_tax" 15 2 inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrDecimal "l_tax" 15 2 inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_rf :
  AttrString "l_returnflag" (StringChar 1)
    inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrString "l_returnflag" (StringChar 1) inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_ls :
  AttrString "l_linestatus" (StringChar 1)
    inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrString "l_linestatus" (StringChar 1) inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_shipdate :
  AttrDate "l_shipdate" inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrDate "l_shipdate" inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma EmbedRow_dot_qty :
  forall r,
    dot TNull (EmbedRow_Q1 r) (AttrDecimal "l_quantity" 15 2) =
    embed_decimal2 (Q1AuxSpec.qty r).
Proof.
  intro r; unfold EmbedRow_Q1.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_qty.
  reflexivity.
Qed.

Lemma EmbedRow_dot_price :
  forall r,
    dot TNull (EmbedRow_Q1 r) (AttrDecimal "l_extendedprice" 15 2) =
    embed_decimal2 (Q1AuxSpec.price r).
Proof.
  intro r; unfold EmbedRow_Q1.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_price.
  reflexivity.
Qed.

Lemma EmbedRow_dot_disc :
  forall r,
    dot TNull (EmbedRow_Q1 r) (AttrDecimal "l_discount" 15 2) =
    embed_decimal2 (Q1AuxSpec.disc r).
Proof.
  intro r; unfold EmbedRow_Q1.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_disc.
  reflexivity.
Qed.

Lemma EmbedRow_dot_tax :
  forall r,
    dot TNull (EmbedRow_Q1 r) (AttrDecimal "l_tax" 15 2) =
    embed_decimal2 (Q1AuxSpec.tax r).
Proof.
  intro r; unfold EmbedRow_Q1.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_tax.
  reflexivity.
Qed.

Lemma EmbedRow_dot_rf :
  forall r,
    dot TNull (EmbedRow_Q1 r) (AttrString "l_returnflag" (StringChar 1)) =
    embed_flag_char (Q1AuxSpec.rf r).
Proof.
  intro r; unfold EmbedRow_Q1.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_rf.
  reflexivity.
Qed.

Lemma EmbedRow_dot_ls :
  forall r,
    dot TNull (EmbedRow_Q1 r) (AttrString "l_linestatus" (StringChar 1)) =
    embed_flag_char (Q1AuxSpec.ls r).
Proof.
  intro r; unfold EmbedRow_Q1.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_ls.
  reflexivity.
Qed.

Lemma EmbedRow_dot_shipdate :
  forall r,
    dot TNull (EmbedRow_Q1 r) (AttrDate "l_shipdate") =
    Value_date (Some (Q1AuxSpec.shipdate r)).
Proof.
  intro r; unfold EmbedRow_Q1.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_shipdate.
  reflexivity.
Qed.

(** * FormalSQLAuxCorrect_Q1 — nonempty via leaf Props + QuerySpineFacade.

    Empty DB: [FormalSQLAuxCorrect_Q1_nil] (facade + empty leaves).
    Spec/Embed helpers above feed [Q1Obligations.v].
    Nonempty: [FormalSQLAuxCorrect_Q1] / [of_holds] under SpecView equality;
    Safe (COUNT+SUM×4+AVG×3) discharges from Pre. SpecView count bridge Qed
    ([SpecView_FoldSpec_key_count]). Drop SpecView hyp for fully universal AuxCorrect.
    Do not nest EQuery_* here. *)

