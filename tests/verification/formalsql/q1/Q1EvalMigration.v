(* Q1EvalMigration — nontrivial migration:
   generic Eval_Spec(LogicalSpec_Q1) ∘ Q1Adapt = AuxAnswerSpec.

   Does NOT close by unfolding an is_LogicalSpec_Q1 branch inside Eval_Spec
   (there is none). Mirrors the Dafny A–H bridge:
     filter ↔ FilterCutoff
     GroupStore ↔ SpecView/FoldSpec pointwise (Pack keys)
     acc ↔ Stats
     semantic tuple order ↔ OutRowSort
     Q1Adapt ↔ SpecFinalizeOne
     compose into Eval_Spec_Q1_eq_AuxAnswerSpec. *)

From Stdlib Require Import List ZArith Bool Lia Ring Relations_1
  Sorting.Permutation Sorting.Sorted.
Import ListNotations.
Open Scope Z_scope.

From FormalSQLResultSpec Require Import
  LogicalSpec EvalLogicalSpec QueryResultSpec EvalSpec.
From JetStreamFS Require Import Q1AuxSpec.
Require Import Q1AuxCorrectAssembly.
Require Import Q1FormalSQLProof.
Require Import Q1GroupsCorrespond.

(** *******************************************************************
    Byte-range keys
    ******************************************************************* *)

Definition Q1ByteKeys (db : list Q1AuxSpec.Row) : Prop :=
  Forall (fun r => 0 <= Q1AuxSpec.rf r < 256 /\ 0 <= Q1AuxSpec.ls r < 256) db.

Lemma Q1ByteKeys_filter :
  forall c db,
    Q1ByteKeys db ->
    Q1ByteKeys (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db).
Proof.
  intros c db H; induction db as [|r rest IH]; simpl in *; [constructor|].
  inversion H; subst.
  destruct (Z.leb (Q1AuxSpec.shipdate r) c); [constructor; auto|]; apply IH; assumption.
Qed.

Lemma Q1ByteKeys_RowOk :
  forall db delta, Preconditions_Q1 db delta -> Q1ByteKeys db.
Proof.
  intros db delta Hpre.
  destruct Hpre as [_ [_ [Hrows _]]].
  induction Hrows as [|r rest Hr Hrest IH]; [constructor|].
  constructor; [|exact IH].
  destruct Hr as [_ [Hrf [Hls _]]]; split; assumption.
Qed.

Lemma Q1ByteKeys_cons_inv :
  forall r rs,
    Q1ByteKeys (r :: rs) ->
    0 <= Q1AuxSpec.rf r < 256 /\ 0 <= Q1AuxSpec.ls r < 256 /\ Q1ByteKeys rs.
Proof. intros r rs H; inversion H; subst; destruct H2; auto. Qed.

(** ----- A. Pack ----- *)
Lemma Pack_inj_byte :
  forall rf1 ls1 rf2 ls2,
    0 <= ls1 < 256 -> 0 <= ls2 < 256 ->
    Q1AuxSpec.Pack rf1 ls1 = Q1AuxSpec.Pack rf2 ls2 ->
    rf1 = rf2 /\ ls1 = ls2.
Proof.
  intros rf1 ls1 rf2 ls2 H1 H2 Heq.
  unfold Q1AuxSpec.Pack in Heq.
  assert (Hmod :
    Z.modulo (rf1 * 256 + ls1) 256 = Z.modulo (rf2 * 256 + ls2) 256)
    by (rewrite Heq; reflexivity).
  rewrite Z.add_comm, Z.mod_add in Hmod by lia.
  rewrite (Z.add_comm (rf2 * 256) ls2), Z.mod_add in Hmod by lia.
  rewrite !Z.mod_small in Hmod by lia.
  subst ls2.
  assert (rf1 * 256 = rf2 * 256) by lia.
  assert (rf1 = rf2) by lia.
  split; [assumption|reflexivity].
Qed.

Lemma Unpack_Pack_byte :
  forall rf ls,
    0 <= rf ->
    0 <= ls < 256 ->
    Q1AuxSpec.Unpack (Q1AuxSpec.Pack rf ls) = (rf, ls).
Proof.
  intros rf ls Hrf Hls.
  unfold Q1AuxSpec.Unpack, Q1AuxSpec.Pack.
  f_equal.
  - symmetry; apply (Z.div_unique (rf * 256 + ls) 256 rf ls); lia.
  - symmetry; apply (Z.mod_unique (rf * 256 + ls) 256 rf ls); lia.
Qed.

(** *******************************************************************
    Store helpers
    ******************************************************************* *)

Lemma list_Z_eqb_refl : forall k, list_Z_eqb k k = true.
Proof.
  induction k as [|x xs IH]; simpl; [reflexivity|].
  rewrite Z.eqb_refl, IH; reflexivity.
Qed.

Lemma list_Z_eqb_eq : forall a b, list_Z_eqb a b = true -> a = b.
Proof.
  induction a as [|x xs IH]; intros [|y ys]; simpl; try discriminate; [reflexivity|].
  intros H; apply andb_prop in H; destruct H as [Hx Hxs].
  apply Z.eqb_eq in Hx; apply IH in Hxs; subst; reflexivity.
Qed.

Lemma list_Z_eqb_neq : forall a b, a <> b -> list_Z_eqb a b = false.
Proof.
  intros a b Hne.
  destruct (list_Z_eqb a b) eqn:E; [|reflexivity].
  apply list_Z_eqb_eq in E; contradiction.
Qed.

Definition NoDupKeys (st : GroupStore) : Prop := NoDup (map fst st).

Lemma store_get_put_same :
  forall st k a, store_get (store_put st k a) k = Some a.
Proof.
  intros st k a; induction st as [|[k' a'] rest IH]; simpl.
  - rewrite list_Z_eqb_refl; reflexivity.
  - destruct (list_Z_eqb k k') eqn:Ek; simpl.
    + rewrite list_Z_eqb_refl; reflexivity.
    + rewrite Ek; exact IH.
Qed.

Lemma list_Z_eqb_sym :
  forall a b, list_Z_eqb a b = list_Z_eqb b a.
Proof.
  induction a as [|x xs IH]; intros [|y ys]; simpl; try reflexivity.
  rewrite Z.eqb_sym, IH; reflexivity.
Qed.

Lemma store_get_put_other :
  forall st k k' a,
    k <> k' -> store_get (store_put st k a) k' = store_get st k'.
Proof.
  intros st k k' a Hne; induction st as [|[kk aa] rest IH]; simpl.
  - rewrite (list_Z_eqb_neq k' k); [reflexivity|intros Heq; apply Hne; symmetry; exact Heq].
  - destruct (list_Z_eqb k kk) eqn:Ek; simpl.
    + rewrite (list_Z_eqb_neq k' k); [|intros Heq; apply Hne; symmetry; exact Heq].
      destruct (list_Z_eqb k' kk) eqn:Ek2; [|reflexivity].
      apply list_Z_eqb_eq in Ek, Ek2; subst; contradiction.
    + destruct (list_Z_eqb k' kk) eqn:Ekq.
      * reflexivity.
      * exact IH.
Qed.

Lemma not_In_fst_store_put :
  forall st k a kk,
    k <> kk ->
    ~ In kk (map fst st) ->
    ~ In kk (map fst (store_put st k a)).
Proof.
  induction st as [|[x y] xs IH]; intros k a kk Hne Hnin Hin; simpl in *.
  - destruct Hin as [Heq|[]]. apply Hne; exact Heq.
  - destruct (list_Z_eqb k x) eqn:Ex; simpl in Hin.
    + apply list_Z_eqb_eq in Ex; subst x.
      destruct Hin as [Heq|Hin0].
      * apply Hnin; left; exact Heq.
      * apply Hnin; right; exact Hin0.
    + destruct Hin as [Heq|Hin0].
      * apply Hnin; left; exact Heq.
      * eapply IH; [| |exact Hin0].
        -- exact Hne.
        -- intros H; apply Hnin; right; exact H.
Qed.

Lemma store_put_NoDupKeys :
  forall st k a, NoDupKeys st -> NoDupKeys (store_put st k a).
Proof.
  intros st k a; induction st as [|[kk aa] rest IH]; intros Hnd;
    unfold NoDupKeys in *; simpl in *.
  - constructor; [intros []|constructor].
  - inversion Hnd as [|? ? Hnin Hnd']; subst.
    destruct (list_Z_eqb k kk) eqn:Ek; simpl.
    + apply list_Z_eqb_eq in Ek; subst kk. constructor; [exact Hnin|exact Hnd'].
    + constructor.
      * apply not_In_fst_store_put.
        -- intros Heq; subst; rewrite list_Z_eqb_refl in Ek; discriminate.
        -- exact Hnin.
      * apply IH; exact Hnd'.
Qed.

Lemma store_get_of_In :
  forall st k a,
    NoDupKeys st -> In (k, a) st -> store_get st k = Some a.
Proof.
  intros st k a Hnd Hin.
  induction st as [|[k' a'] rest IH]; simpl in *.
  - contradiction.
  - unfold NoDupKeys in Hnd; simpl in Hnd; inversion Hnd; subst.
    destruct Hin as [Heq|Hin].
    + inversion Heq; subst. rewrite list_Z_eqb_refl; reflexivity.
    + destruct (list_Z_eqb k k') eqn:Ek.
      * apply list_Z_eqb_eq in Ek; subst k'.
        exfalso; apply H1. apply in_map with (f := fst) in Hin; exact Hin.
      * apply IH; [exact H2|exact Hin].
Qed.

Lemma In_of_store_get :
  forall st k a, store_get st k = Some a -> In (k, a) st.
Proof.
  induction st as [|[k' a'] rest IH]; intros k a Hget; simpl in *.
  - discriminate.
  - destruct (list_Z_eqb k k') eqn:Ek.
    + inversion Hget; subst. apply list_Z_eqb_eq in Ek; subst. left; reflexivity.
    + right; apply IH; exact Hget.
Qed.

Lemma store_to_rows_In :
  forall st row,
    In row (store_to_rows st) <-> In (sr_key row, sr_acc row) st.
Proof.
  induction st as [|[k a] rest IH]; intros row; simpl.
  - split; contradiction.
  - unfold store_to_rows; simpl.
    split; intros H.
    + destruct H as [Heq|H].
      * left. rewrite <- Heq; reflexivity.
      * right. apply IH; exact H.
    + destruct H as [Heq|H].
      * left. inversion Heq; destruct row; simpl in *; subst; reflexivity.
      * right. apply IH; exact H.
Qed.

(** *******************************************************************
    Acc ↔ Stats
    ******************************************************************* *)

Lemma acc_of_stats_Empty :
  acc_of_stats Q1AuxSpec.EmptyStats = empty_acc 8.
Proof. reflexivity. Qed.

Lemma acc_add_row_Q1_SpecApply :
  forall p r s,
    acc_add_row q1_aggs p r (acc_of_stats s) =
    Some (acc_of_stats (Q1AuxSpec.SpecApply s r 1)).
Proof.
  intros p r [cnt sq sb sdp sch sd].
  unfold acc_add_row; rewrite aggs_contrib_Q1.
  unfold acc_add_one, acc_of_stats, Q1AuxSpec.SpecApply.
  cbn -[Z.mul Z.add].
  rewrite !Z.mul_1_l.
  reflexivity.
Qed.

Lemma acc_nonempty_of_stats_pos :
  forall s, 0 < Q1AuxSpec.count s -> acc_nonempty (acc_of_stats s) = true.
Proof.
  intros s Hpos. unfold acc_nonempty, acc_of_stats; simpl.
  destruct (Z.eqb (Q1AuxSpec.count s) 0) eqn:Ez; [|reflexivity].
  apply Z.eqb_eq in Ez; lia.
Qed.

Lemma SpecApply_count_pos :
  forall s r, 0 <= Q1AuxSpec.count s ->
    0 < Q1AuxSpec.count (Q1AuxSpec.SpecApply s r 1).
Proof. intros s r H; unfold Q1AuxSpec.SpecApply; simpl; lia. Qed.

Lemma sum_contrib_count_nonneg :
  forall rs gkey, 0 <= Q1AuxSpec.count (sum_contrib_matching rs gkey).
Proof.
  induction rs as [|r rest IH]; intros gkey; [simpl; lia|].
  cbn [sum_contrib_matching].
  destruct (Z.eqb (Q1AuxSpec.RowKey r) gkey).
  - unfold Q1AuxSpec.SpecMerge, row_contrib, Q1AuxSpec.SpecApply, Q1AuxSpec.EmptyStats.
    simpl. pose proof (IH gkey). destruct (sum_contrib_matching rest gkey); simpl in *; lia.
  - apply IH.
Qed.

Lemma sum_contrib_count0_Empty :
  forall rs gkey,
    Q1AuxSpec.count (sum_contrib_matching rs gkey) = 0 ->
    sum_contrib_matching rs gkey = Q1AuxSpec.EmptyStats.
Proof.
  induction rs as [|r rest IH]; intros gkey Hc; [reflexivity|].
  cbn [sum_contrib_matching] in *.
  destruct (Z.eqb (Q1AuxSpec.RowKey r) gkey) eqn:Eg.
  - unfold Q1AuxSpec.SpecMerge, row_contrib, Q1AuxSpec.SpecApply, Q1AuxSpec.EmptyStats in Hc.
    simpl in Hc.
    pose proof (sum_contrib_count_nonneg rest gkey).
    destruct (sum_contrib_matching rest gkey); simpl in *; lia.
  - apply IH; exact Hc.
Qed.

Lemma sum_contrib_app_one :
  forall rs r gkey,
    sum_contrib_matching (rs ++ [r]) gkey =
    if Z.eqb (Q1AuxSpec.RowKey r) gkey
    then Q1AuxSpec.SpecApply (sum_contrib_matching rs gkey) r 1
    else sum_contrib_matching rs gkey.
Proof.
  induction rs as [|x xs IH]; intros r gkey; simpl.
  - destruct (Z.eqb (Q1AuxSpec.RowKey r) gkey) eqn:Eg.
    + rewrite SpecApply_as_merge_contrib, SpecMerge_Empty_l; unfold row_contrib; reflexivity.
    + reflexivity.
  - cbn [sum_contrib_matching]. rewrite IH.
    destruct (Z.eqb (Q1AuxSpec.RowKey x) gkey) eqn:Ex;
      destruct (Z.eqb (Q1AuxSpec.RowKey r) gkey) eqn:Er; try reflexivity.
    rewrite !SpecApply_as_merge_contrib, <- SpecMerge_assoc.
    rewrite (SpecMerge_comm (row_contrib r) (row_contrib x)), SpecMerge_assoc.
    reflexivity.
Qed.

(** *******************************************************************
    StoreMirrors (C/D/E): shape + get ↔ sum_contrib
    ******************************************************************* *)

Definition KeyOk (k : list Z) : Prop :=
  exists rf ls, k = [rf; ls] /\ 0 <= rf < 256 /\ 0 <= ls < 256.

Definition StoreMirrors (st : GroupStore) (rs : list Q1AuxSpec.Row) : Prop :=
  NoDupKeys st /\
  Forall (fun '(k, _) => KeyOk k) st /\
  (forall rf ls,
     0 <= rf < 256 -> 0 <= ls < 256 ->
     store_get st [rf; ls] =
       let s := sum_contrib_matching rs (Q1AuxSpec.Pack rf ls) in
       if Z.eqb (Q1AuxSpec.count s) 0 then None else Some (acc_of_stats s)).

Lemma StoreMirrors_empty : StoreMirrors [] [].
Proof.
  split; [|split].
  - constructor.
  - constructor.
  - intros rf ls _ _; simpl; reflexivity.
Qed.

Lemma store_put_Forall_KeyOk :
  forall st k a,
    KeyOk k ->
    Forall (fun '(k', _) => KeyOk k') st ->
    Forall (fun '(k', _) => KeyOk k') (store_put st k a).
Proof.
  intros st k a Hk Hfa.
  induction st as [|[kk aa] rest IH]; simpl.
  - constructor; [exact Hk|constructor].
  - inversion Hfa; subst.
    destruct (list_Z_eqb k kk); constructor; auto.
Qed.

Lemma store_add_row_Q1_mirrors :
  forall p r st rs,
    0 <= Q1AuxSpec.rf r < 256 ->
    0 <= Q1AuxSpec.ls r < 256 ->
    StoreMirrors st rs ->
    exists st',
      store_add_row q1_aggs q1_ls_group_keys p r st = Some st' /\
      StoreMirrors st' (rs ++ [r]).
Proof.
  intros p r st rs Hrf Hls [Hnd [Hfa Hm]].
  unfold store_add_row. rewrite eval_exprs_Q1_group.
  set (k := [Q1AuxSpec.rf r; Q1AuxSpec.ls r]).
  set (g := Q1AuxSpec.Pack (Q1AuxSpec.rf r) (Q1AuxSpec.ls r)).
  pose proof (Hm (Q1AuxSpec.rf r) (Q1AuxSpec.ls r) Hrf Hls) as Hget.
  set (s0 := sum_contrib_matching rs g).
  assert (Hacc0 :
    match store_get st k with
    | Some a => a
    | None => empty_acc (List.length q1_aggs)
    end = acc_of_stats s0).
  {
    unfold s0. subst k g; cbn in Hget |- *.
    destruct (Z.eqb (Q1AuxSpec.count (sum_contrib_matching rs
              (Q1AuxSpec.Pack (Q1AuxSpec.rf r) (Q1AuxSpec.ls r)))) 0) eqn:Ez.
    - rewrite Hget.
      apply Z.eqb_eq in Ez.
      rewrite (sum_contrib_count0_Empty _ _ Ez).
      unfold empty_acc, acc_of_stats, Q1AuxSpec.EmptyStats; reflexivity.
    - rewrite Hget; reflexivity.
  }
  rewrite Hacc0, acc_add_row_Q1_SpecApply.
  eexists; split; [reflexivity|].
  split; [|split].
  - apply store_put_NoDupKeys; exact Hnd.
  - apply store_put_Forall_KeyOk; [|exact Hfa].
    exists (Q1AuxSpec.rf r), (Q1AuxSpec.ls r).
    split; [reflexivity|]. split; [exact Hrf|exact Hls].
  - intros rf' ls' Hrf' Hls'.
    set (k' := [rf'; ls']).
    set (g' := Q1AuxSpec.Pack rf' ls').
    destruct (list_Z_eqb k k') eqn:Ek.
    + apply list_Z_eqb_eq in Ek.
      assert (rf' = Q1AuxSpec.rf r /\ ls' = Q1AuxSpec.ls r) by (inversion Ek; auto).
      destruct H as [-> ->].
      rewrite store_get_put_same, sum_contrib_app_one.
      replace (Q1AuxSpec.RowKey r) with g by (unfold g, Q1AuxSpec.RowKey; reflexivity).
      rewrite Z.eqb_refl.
      unfold s0, g, g'.
      set (s1 := Q1AuxSpec.SpecApply
                   (sum_contrib_matching rs
                      (Q1AuxSpec.Pack (Q1AuxSpec.rf r) (Q1AuxSpec.ls r))) r 1).
      assert (Hpos : 0 < Q1AuxSpec.count s1)
        by (subst s1; apply SpecApply_count_pos, sum_contrib_count_nonneg).
      change (
        let s := Q1AuxSpec.SpecApply
                   (sum_contrib_matching rs
                      (Q1AuxSpec.Pack (Q1AuxSpec.rf r) (Q1AuxSpec.ls r))) r 1 in
        if Z.eqb (Q1AuxSpec.count s) 0 then None else Some (acc_of_stats s)
      ) with (
        if Z.eqb (Q1AuxSpec.count s1) 0 then None else Some (acc_of_stats s1)
      ).
      destruct (Z.eqb (Q1AuxSpec.count s1) 0) eqn:Ez.
      { apply Z.eqb_eq in Ez; lia. }
      reflexivity.
    + assert (Hne : k <> k').
      { intros Heq.
        assert (Et : list_Z_eqb k k' = true) by (rewrite Heq; apply list_Z_eqb_refl).
        rewrite Et in Ek; discriminate. }
      rewrite (store_get_put_other st k k' _ Hne).
      unfold k', g'.
      rewrite (Hm rf' ls' Hrf' Hls').
      rewrite sum_contrib_app_one.
      destruct (Z.eqb (Q1AuxSpec.RowKey r)
                      (Q1AuxSpec.Pack rf' ls')) eqn:Erg.
      * apply Z.eqb_eq in Erg.
        assert (Hpack : Q1AuxSpec.Pack (Q1AuxSpec.rf r) (Q1AuxSpec.ls r) =
                        Q1AuxSpec.Pack rf' ls')
          by (rewrite <- Erg; unfold Q1AuxSpec.RowKey; reflexivity).
        destruct (Pack_inj_byte _ _ _ _ Hls Hls' Hpack) as [Heq1 Heq2].
        subst rf' ls'.
        unfold k, k' in Ek; rewrite list_Z_eqb_refl in Ek; discriminate.
      * reflexivity.
Qed.

Lemma fold_groups_Q1_mirrors :
  forall p rs st0 done,
    Q1ByteKeys rs ->
    StoreMirrors st0 done ->
    exists st,
      fold_groups q1_aggs q1_ls_group_keys p rs st0 = Some st /\
      StoreMirrors st (done ++ rs).
Proof.
  intros p rs; induction rs as [|r rest IH]; intros st0 done Hbyte Hm.
  - exists st0; split; [reflexivity|rewrite app_nil_r; exact Hm].
  - apply Q1ByteKeys_cons_inv in Hbyte.
    destruct Hbyte as [Hrf [Hls Hrest]].
    destruct (store_add_row_Q1_mirrors p r st0 done Hrf Hls Hm) as [st1 [Hadd Hm1]].
    cbn [fold_groups]. rewrite Hadd.
    destruct (IH st1 (done ++ [r]) Hrest Hm1) as [st [Hfold Hm2]].
    exists st; split; [exact Hfold|rewrite <- app_assoc in Hm2; exact Hm2].
Qed.

Lemma fold_groups_Q1_from_empty :
  forall p rs,
    Q1ByteKeys rs ->
    exists st,
      fold_groups q1_aggs q1_ls_group_keys p rs [] = Some st /\
      StoreMirrors st rs.
Proof.
  intros p rs Hbyte.
  destruct (fold_groups_Q1_mirrors p rs [] [] Hbyte StoreMirrors_empty) as [st [Hf Hm]].
  exists st; split; [exact Hf|simpl in Hm; exact Hm].
Qed.

Lemma StoreMirrors_In :
  forall st rs k a,
    StoreMirrors st rs ->
    In (k, a) st ->
    exists rf ls,
      k = [rf; ls] /\ 0 <= rf < 256 /\ 0 <= ls < 256 /\
      a = acc_of_stats (sum_contrib_matching rs (Q1AuxSpec.Pack rf ls)) /\
      0 < Q1AuxSpec.count (sum_contrib_matching rs (Q1AuxSpec.Pack rf ls)).
Proof.
  intros st rs k a [Hnd [Hfa Hm]] Hin.
  assert (Hk : KeyOk k).
  { rewrite Forall_forall in Hfa. apply (Hfa (k, a)); exact Hin. }
  destruct Hk as [rf [ls [Hk [Hrf Hls]]]]; subst k.
  pose proof (store_get_of_In st [rf; ls] a Hnd Hin) as Hget.
  pose proof (Hm rf ls Hrf Hls) as Hget'.
  simpl in Hget'.
  destruct (Z.eqb (Q1AuxSpec.count (sum_contrib_matching rs (Q1AuxSpec.Pack rf ls))) 0)
    eqn:Ez.
  - rewrite Hget' in Hget; discriminate.
  - rewrite Hget' in Hget; inversion Hget; subst a.
    exists rf, ls.
    split; [reflexivity|].
    split; [exact Hrf|].
    split; [exact Hls|].
    split; [reflexivity|].
    apply Z.eqb_neq in Ez.
    pose proof (sum_contrib_count_nonneg rs (Q1AuxSpec.Pack rf ls)); lia.
Qed.


(** *******************************************************************
    Filter-nonempty is identity under StoreMirrors
    ******************************************************************* *)

Lemma filter_id_Forall :
  forall {A} (f : A -> bool) (l : list A),
    Forall (fun x => f x = true) l ->
    filter f l = l.
Proof.
  intros A f l H; induction H as [|x xs Hx Hxs IH]; [reflexivity|].
  simpl. rewrite Hx, IH; reflexivity.
Qed.

Lemma filter_nonempty_StoreMirrors :
  forall st rs,
    StoreMirrors st rs ->
    filter_nonempty (store_to_rows st) = store_to_rows st.
Proof.
  intros st rs Hm.
  unfold filter_nonempty.
  apply filter_id_Forall.
  rewrite Forall_forall; intros row Hin.
  apply (proj1 (store_to_rows_In st row)) in Hin.
  destruct row as [k a]; simpl in Hin.
  destruct (StoreMirrors_In st rs k a Hm Hin)
    as [rf [ls [_ [_ [_ [Ha Hpos]]]]]].
  simpl. rewrite Ha. apply acc_nonempty_of_stats_pos; exact Hpos.
Qed.

(** *******************************************************************
    F. Q1AdaptRow ↔ SpecFinalizeOne
    ******************************************************************* *)

Lemma Q1AdaptRow_FinalizeOne_eq :
  forall rf ls s,
    0 <= rf ->
    0 <= ls < 256 ->
    0 < Q1AuxSpec.count s ->
    Q1AdaptRow (mkSpecResultRow [rf; ls] (acc_of_stats s)) =
    match Q1AuxSpec.SpecFinalizeOne (Q1AuxSpec.Pack rf ls) s with
    | [o] => Some o
    | _ => None
    end.
Proof.
  intros rf ls s Hrf Hls Hpos.
  rewrite SpecFinalizeOne_positive by lia.
  rewrite Unpack_Pack_byte by lia.
  rewrite Q1AdaptRow_of_acc by lia.
  reflexivity.
Qed.

(** *******************************************************************
    SpecView helpers
    ******************************************************************* *)

Lemma keystore_get_pos_In :
  forall (acc : list (Z * Q1AuxSpec.Stats)) gkey,
    0 < Q1AuxSpec.count (keystore_get acc gkey) ->
    exists s, In (gkey, s) acc /\ keystore_get acc gkey = s.
Proof.
  induction acc as [|[k s] rest IH]; intros gkey Hpos; simpl in *.
  - (* EmptyStats count = 0 *)
    change (Q1AuxSpec.count Q1AuxSpec.EmptyStats) with 0 in Hpos; lia.
  - destruct (Z.eqb k gkey) eqn:Ek.
    + apply Z.eqb_eq in Ek; subst k.
      exists s; split; [left; reflexivity|reflexivity].
    + destruct (IH gkey Hpos) as [s' [Hin Hg]].
      exists s'; split; [right; exact Hin|exact Hg].
Qed.

(** *******************************************************************
    Entry well-formedness from StoreMirrors
    ******************************************************************* *)

Lemma StoreMirrors_Forall_entries :
  forall st rs,
    StoreMirrors st rs ->
    Forall
      (fun '(k, a) =>
         exists rf ls,
           k = [rf; ls] /\ 0 <= rf < 256 /\ 0 <= ls < 256 /\
           a = acc_of_stats (sum_contrib_matching rs (Q1AuxSpec.Pack rf ls)) /\
           0 < Q1AuxSpec.count (sum_contrib_matching rs (Q1AuxSpec.Pack rf ls)))
      st.
Proof.
  intros st rs Hm.
  rewrite Forall_forall; intros [k a] Hin.
  eapply StoreMirrors_In; eassumption.
Qed.

(** Q1Adapt succeeds on store_to_rows and produces SpecFinalizeOne outputs. *)
Lemma Q1Adapt_store_to_rows_ok :
  forall st rs,
    StoreMirrors st rs ->
    exists outs,
      Q1Adapt (store_to_rows st) = Some outs /\
      Permutation
        outs
        (flat_map
           (fun '(k, a) =>
              match k with
              | [rf; ls] =>
                  Q1AuxSpec.SpecFinalizeOne
                    (Q1AuxSpec.Pack rf ls)
                    (sum_contrib_matching rs (Q1AuxSpec.Pack rf ls))
              | _ => []
              end)
           st).
Proof.
  intros st rs Hm.
  pose proof (StoreMirrors_Forall_entries st rs Hm) as Hfa.
  pose proof (proj1 Hm) as Hnd.
  clear Hm.
  unfold store_to_rows.
  revert Hfa Hnd.
  induction st as [|[k a] rest IH]; intros Hfa Hnd.
  - exists []; split; [reflexivity|apply Permutation_refl].
  - inversion Hfa as [|? ? Hx Hfa']; subst.
    unfold NoDupKeys in Hnd; simpl in Hnd; inversion Hnd; subst.
    destruct Hx as [rf [ls [Hk [Hrf [Hls [Ha Hpos]]]]]].
    subst k a.
    destruct (IH Hfa' H2) as [outs [Hadapt Hperm]].
    cbn [Q1Adapt map].
    rewrite Q1AdaptRow_FinalizeOne_eq by lia.
    rewrite SpecFinalizeOne_positive by lia.
    rewrite Unpack_Pack_byte by lia.
    simpl.
    rewrite Hadapt.
    eexists; split; [reflexivity|].
    cbn [flat_map].
    rewrite SpecFinalizeOne_positive by lia.
    rewrite Unpack_Pack_byte by lia.
    simpl.
    apply perm_skip.
    exact Hperm.
Qed.

(** *******************************************************************
    SpecFinalize SpecView ↔ flat_map over store (bag)
    ******************************************************************* *)

Lemma SpecFinalize_In_pos :
  forall entries gkey s,
    In (gkey, s) entries ->
    0 < Q1AuxSpec.count s ->
    In (let '(rf, ls) := Q1AuxSpec.Unpack gkey in
        Q1AuxSpec.mkOutRow rf ls
          (Q1AuxSpec.sum_qty s) (Q1AuxSpec.sum_base_price s)
          (Q1AuxSpec.sum_disc_price s) (Q1AuxSpec.sum_charge s)
          (Q1AuxSpec.sum_discount s)
          (Q1AuxSpec.AvgRound (Q1AuxSpec.sum_qty s) (Q1AuxSpec.count s))
          (Q1AuxSpec.AvgRound (Q1AuxSpec.sum_base_price s) (Q1AuxSpec.count s))
          (Q1AuxSpec.AvgRound (Q1AuxSpec.sum_discount s) (Q1AuxSpec.count s))
          (Q1AuxSpec.count s))
       (Q1AuxSpec.SpecFinalize entries).
Proof.
  induction entries as [|[g s0] rest IH]; intros gkey s Hin Hpos; simpl in *.
  - contradiction.
  - destruct Hin as [Heq|Hin].
    + inversion Heq; subst.
      rewrite SpecFinalizeOne_positive by lia.
      simpl. left. reflexivity.
    + apply in_or_app; right; apply IH; assumption.
Qed.

(** *******************************************************************
    Positive contrib ⇒ witness row with that RowKey
    ******************************************************************* *)

Lemma sum_contrib_pos_witness :
  forall rs gkey,
    0 < Q1AuxSpec.count (sum_contrib_matching rs gkey) ->
    exists r, In r rs /\ Q1AuxSpec.RowKey r = gkey.
Proof.
  induction rs as [|r rest IH]; intros gkey Hpos; simpl in *.
  - change (Q1AuxSpec.count Q1AuxSpec.EmptyStats) with 0 in Hpos; lia.
  - cbn [sum_contrib_matching] in Hpos.
    destruct (Z.eqb (Q1AuxSpec.RowKey r) gkey) eqn:Ek.
    + apply Z.eqb_eq in Ek.
      exists r; split; [left; reflexivity|exact Ek].
    + destruct (IH gkey Hpos) as [r' [Hin Heq]].
      exists r'; split; [right; exact Hin|exact Heq].
Qed.

Lemma SpecView_entry_char :
  forall db c gkey s,
    Q1ByteKeys db ->
    In (gkey, s) (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) c) ->
    exists rf ls,
      gkey = Q1AuxSpec.Pack rf ls /\
      0 <= rf < 256 /\ 0 <= ls < 256 /\
      s = sum_contrib_matching
            (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db) gkey /\
      0 < Q1AuxSpec.count s.
Proof.
  intros db c gkey s Hbyte Hin.
  pose proof (SpecView_NoDup_fst (Q1AuxSpec.FoldSpec db) c) as Hnd.
  pose proof (keystore_get_of_In _ _ _ Hnd Hin) as Hg.
  pose proof (SpecView_FoldSpec_In_count_pos db c gkey s Hin) as Hpos.
  pose proof (SpecView_FoldSpec_key_stats db c gkey) as Hstats.
  rewrite Hg in Hstats.
  set (filtered := filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db).
  assert (Hs_filt : s = sum_contrib_matching filtered gkey).
  { unfold filtered; exact Hstats. }
  assert (Hpos' : 0 < Q1AuxSpec.count (sum_contrib_matching filtered gkey))
    by (rewrite <- Hs_filt; exact Hpos).
  destruct (sum_contrib_pos_witness filtered gkey Hpos') as [r [Hinr Hk]].
  assert (Hbytef : Q1ByteKeys filtered) by (apply Q1ByteKeys_filter; exact Hbyte).
  assert (Hr : 0 <= Q1AuxSpec.rf r < 256 /\ 0 <= Q1AuxSpec.ls r < 256).
  { unfold Q1ByteKeys in Hbytef; rewrite Forall_forall in Hbytef; apply Hbytef; exact Hinr. }
  destruct Hr as [Hrf Hls].
  exists (Q1AuxSpec.rf r), (Q1AuxSpec.ls r).
  split; [rewrite <- Hk; reflexivity|].
  split; [exact Hrf|].
  split; [exact Hls|].
  split; [exact Hs_filt|exact Hpos].
Qed.

(** flat_map finalize of store ↔ SpecFinalize SpecView as bags *)
Lemma store_finalize_In_SpecFinalize :
  forall c db st o,
    Q1ByteKeys db ->
    StoreMirrors st (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db) ->
    In o (flat_map
            (fun '(k, a) =>
               match k with
               | [rf; ls] =>
                   Q1AuxSpec.SpecFinalizeOne
                     (Q1AuxSpec.Pack rf ls)
                     (sum_contrib_matching
                        (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db)
                        (Q1AuxSpec.Pack rf ls))
               | _ => []
               end) st) ->
    In o (Q1AuxSpec.SpecFinalize
            (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) c)).
Proof.
  intros c db st o Hbyte Hm Hin.
  set (filtered := filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db) in Hm.
  apply in_flat_map in Hin.
  destruct Hin as [[k a] [Hin_st Hin_o]].
  destruct (StoreMirrors_In st filtered k a Hm Hin_st)
    as [rf [ls [Hk [Hrf [Hls [Ha Hpos]]]]]].
  subst k.
  unfold filtered in Hpos, Ha.
  rewrite SpecFinalizeOne_positive in Hin_o by lia.
  rewrite Unpack_Pack_byte in Hin_o by lia.
  simpl in Hin_o.
  destruct Hin_o as [Heq|[]]; subst o.
  set (gkey := Q1AuxSpec.Pack rf ls).
  assert (Hkc :
    keystore_get (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) c) gkey =
    sum_contrib_matching
      (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db) gkey)
    by (unfold gkey; apply SpecView_FoldSpec_key_stats).
  destruct (keystore_get_pos_In
              (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) c) gkey)
    as [s' [Hin_e Hs]].
  { rewrite Hkc; exact Hpos. }
  assert (Hs' : s' =
    sum_contrib_matching
      (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db) gkey)
    by (rewrite <- Hs, Hkc; reflexivity).
  pose proof (SpecFinalize_In_pos
                (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) c)
                gkey s' Hin_e ltac:(rewrite Hs'; exact Hpos)) as Hin_fin.
  rewrite Hs' in Hin_fin.
  unfold gkey in Hin_fin.
  rewrite Unpack_Pack_byte in Hin_fin by lia.
  exact Hin_fin.
Qed.

Lemma SpecFinalize_In_store_finalize :
  forall c db st o,
    Q1ByteKeys db ->
    StoreMirrors st (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db) ->
    In o (Q1AuxSpec.SpecFinalize
            (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) c)) ->
    In o (flat_map
            (fun '(k, a) =>
               match k with
               | [rf; ls] =>
                   Q1AuxSpec.SpecFinalizeOne
                     (Q1AuxSpec.Pack rf ls)
                     (sum_contrib_matching
                        (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db)
                        (Q1AuxSpec.Pack rf ls))
               | _ => []
               end) st).
Proof.
  intros c db st o Hbyte Hm Hin.
  set (filtered := filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db) in Hm.
  (* o comes from some SpecView entry *)
  assert (Hex : exists gkey s,
    In (gkey, s) (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) c) /\
    0 < Q1AuxSpec.count s /\
    Q1AuxSpec.SpecFinalizeOne gkey s = [o]).
  {
    clear Hm filtered.
    revert o Hin.
    induction (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) c) as [|[g s] rest IH];
      intros o Hin; [contradiction|].
    cbn [Q1AuxSpec.SpecFinalize] in Hin.
    apply in_app_or in Hin; destruct Hin as [Hin|Hin].
    - assert (Hpos0 : 0 < Q1AuxSpec.count s).
      { destruct (Z.leb (Q1AuxSpec.count s) 0) eqn:Ele.
        - unfold Q1AuxSpec.SpecFinalizeOne in Hin; rewrite Ele in Hin; contradiction.
        - apply Z.leb_gt in Ele; exact Ele. }
      pose proof (SpecFinalizeOne_positive g s Hpos0) as Hf.
      rewrite Hf in Hin; simpl in Hin.
      destruct Hin as [Heq|[]]; subst o.
      exists g, s. split; [left; reflexivity|].
      split; [exact Hpos0|exact Hf].
    - destruct (IH o Hin) as [g' [s' [Hin' [Hpos' Hf']]]].
      exists g', s'. split; [right; exact Hin'|split; assumption].
  }
  destruct Hex as [gkey [s [Hin_e [Hpos Hf]]]].
  destruct (SpecView_entry_char db c gkey s Hbyte Hin_e)
    as [rf [ls [Hgkey [Hrf [Hls [Hs Hpos']]]]]].
  destruct Hm as [Hnd [Hfa Hget]].
  pose proof (Hget rf ls Hrf Hls) as Hg_st.
  simpl in Hg_st.
  rewrite Hgkey in Hs.
  unfold filtered in Hs, Hg_st.
  rewrite <- Hs in Hg_st.
  destruct (Z.eqb (Q1AuxSpec.count s) 0) eqn:Ez; [apply Z.eqb_eq in Ez; lia|].
  assert (Hin_st : In ([rf; ls], acc_of_stats s) st)
    by (apply In_of_store_get; rewrite Hg_st; reflexivity).
  apply in_flat_map.
  exists ([rf; ls], acc_of_stats s).
  split; [exact Hin_st|].
  rewrite <- Hs, <- Hgkey.
  rewrite Hf.
  left; reflexivity.
Qed.

Lemma SpecFinalize_out_key :
  forall entries o,
    In o (Q1AuxSpec.SpecFinalize entries) ->
    exists gkey s,
      In (gkey, s) entries /\
      0 < Q1AuxSpec.count s /\
      Q1AuxSpec.SpecFinalizeOne gkey s = [o].
Proof.
  induction entries as [|[g s] rest IH]; intros o Hin; [contradiction|].
  cbn [Q1AuxSpec.SpecFinalize] in Hin.
  apply in_app_or in Hin; destruct Hin as [Hin|Hin].
  - assert (Hpos : 0 < Q1AuxSpec.count s).
    { destruct (Z.leb (Q1AuxSpec.count s) 0) eqn:Ele.
      - unfold Q1AuxSpec.SpecFinalizeOne in Hin; rewrite Ele in Hin; contradiction.
      - apply Z.leb_gt in Ele; exact Ele. }
    pose proof (SpecFinalizeOne_positive g s Hpos) as Hf.
    rewrite Hf in Hin; simpl in Hin.
    destruct Hin as [Heq|[]]; subst o.
    exists g, s; split; [left; reflexivity|split; [exact Hpos|exact Hf]].
  - destruct (IH o Hin) as [g' [s' [Hin' [Hpos' Hf']]]].
    exists g', s'; split; [right; exact Hin'|split; assumption].
Qed.

Lemma SpecFinalize_NoDup_Q1 :
  forall db c,
    Q1ByteKeys db ->
    NoDup (Q1AuxSpec.SpecFinalize
             (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) c)).
Proof.
  intros db c Hbyte.
  set (entries := Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) c).
  pose proof (SpecView_NoDup_fst (Q1AuxSpec.FoldSpec db) c) as Hnd.
  fold entries in Hnd |- *.
  assert (Hchar : forall g s, In (g, s) entries ->
      0 < Q1AuxSpec.count s /\
      exists rf ls, g = Q1AuxSpec.Pack rf ls /\ 0 <= rf < 256 /\ 0 <= ls < 256).
  { intros g s Hin.
    destruct (SpecView_entry_char db c g s Hbyte Hin)
      as [rf [ls [Hg [Hrf [Hls [_ Hpos]]]]]].
    split; [exact Hpos|].
    exists rf, ls. split; [exact Hg|]. split; [exact Hrf|exact Hls]. }
  clear Hbyte.
  revert Hchar Hnd.
  induction entries as [|[g s] rest IH]; intros Hchar Hnd; [constructor|].
  inversion Hnd as [|? ? Hnin Hnd']; subst.
  cbn [Q1AuxSpec.SpecFinalize].
  assert (Hx := Hchar g s ltac:(left; reflexivity)).
  destruct Hx as [Hpos [rf [ls [Hg [Hrf Hls]]]]].
  rewrite SpecFinalizeOne_positive by lia.
  rewrite Hg, Unpack_Pack_byte by lia; simpl.
  constructor.
  - intros Hin_rest.
    destruct (SpecFinalize_out_key rest _ Hin_rest)
      as [g' [s' [Hin_e [Hpos' Heq_fin]]]].
    assert (Hy := Hchar g' s' ltac:(right; exact Hin_e)).
    destruct Hy as [_ [rf' [ls' [Hg' [Hrf' Hls']]]]].
    rewrite Hg' in Heq_fin.
    rewrite SpecFinalizeOne_positive in Heq_fin by lia.
    rewrite Unpack_Pack_byte in Heq_fin by lia.
    inversion Heq_fin; subst rf' ls'.
    assert (Hg_eq : g' = g) by (rewrite Hg', Hg; reflexivity).
    rewrite Hg_eq in Hin_e.
    apply Hnin, in_map_iff.
    exists (g, s'); split; [reflexivity|exact Hin_e].
  - apply IH.
    + intros g0 s0 Hin0; apply Hchar; right; exact Hin0.
    + exact Hnd'.
Qed.

Lemma store_flat_NoDup :
  forall c db st,
    Q1ByteKeys db ->
    StoreMirrors st (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db) ->
    NoDup
      (flat_map
         (fun '(k, a) =>
            match k with
            | [rf; ls] =>
                Q1AuxSpec.SpecFinalizeOne
                  (Q1AuxSpec.Pack rf ls)
                  (sum_contrib_matching
                     (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db)
                     (Q1AuxSpec.Pack rf ls))
            | _ => []
            end) st).
Proof.
  intros c db st Hbyte Hm.
  (* Transfer NoDup from SpecFinalize via the In-iff already proved,
     once we have Permutation — circular. Prove directly like SpecFinalize_NoDup. *)
  pose proof (proj1 Hm) as Hnd.
  pose proof (StoreMirrors_Forall_entries _ _ Hm) as Hfa.
  clear Hm Hbyte.
  revert Hfa Hnd.
  induction st as [|[k a] rest IH]; intros Hfa Hnd; [constructor|].
  inversion Hfa as [|? ? Hx Hfa']; subst.
  inversion Hnd as [|? ? Hnin Hnd']; subst.
  destruct Hx as [rf [ls [Hk [Hrf [Hls [Ha Hpos]]]]]].
  subst k a.
  cbn [flat_map].
  rewrite SpecFinalizeOne_positive by lia.
  rewrite Unpack_Pack_byte by lia; simpl.
  constructor.
  - intros Hin_rest.
    (* Hin_rest : In (mkOutRow rf ls ...) (flat_map ... rest) *)
    apply in_flat_map in Hin_rest.
    destruct Hin_rest as [[k' a'] [Hin_st Hin_o]].
    assert (Hx' : exists rf' ls',
      k' = [rf'; ls'] /\ 0 <= rf' < 256 /\ 0 <= ls' < 256 /\
      a' = acc_of_stats (sum_contrib_matching
            (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db)
            (Q1AuxSpec.Pack rf' ls')) /\
      0 < Q1AuxSpec.count (sum_contrib_matching
            (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db)
            (Q1AuxSpec.Pack rf' ls'))).
    { (* From Hfa' *)
      rewrite Forall_forall in Hfa'.
      specialize (Hfa' (k', a') Hin_st).
      exact Hfa'. }
    destruct Hx' as [rf' [ls' [Hk' [Hrf' [Hls' [Ha' Hpos']]]]]].
    subst k'.
    rewrite SpecFinalizeOne_positive in Hin_o by lia.
    rewrite Unpack_Pack_byte in Hin_o by lia.
    simpl in Hin_o.
    destruct Hin_o as [Heq|[]].
    inversion Heq; subst rf' ls'.
    apply Hnin, in_map_iff.
    exists ([rf; ls], a'); split; [reflexivity|exact Hin_st].
  - apply IH; [exact Hfa'|exact Hnd'].
Qed.

Lemma store_finalize_perm_SpecFinalize :
  forall c db st,
    Q1ByteKeys db ->
    StoreMirrors st (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db) ->
    Permutation
      (flat_map
         (fun '(k, a) =>
            match k with
            | [rf; ls] =>
                Q1AuxSpec.SpecFinalizeOne
                  (Q1AuxSpec.Pack rf ls)
                  (sum_contrib_matching
                     (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db)
                     (Q1AuxSpec.Pack rf ls))
            | _ => []
            end) st)
      (Q1AuxSpec.SpecFinalize
         (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) c)).
Proof.
  intros c db st Hbyte Hm.
  apply NoDup_Permutation.
  - apply store_flat_NoDup; assumption.
  - apply SpecFinalize_NoDup_Q1; exact Hbyte.
  - intros o; split.
    + apply store_finalize_In_SpecFinalize; assumption.
    + apply SpecFinalize_In_store_finalize; assumption.
Qed.
(** *******************************************************************
    G. OutRowSort uniqueness under unique (rf,ls) keys
    ******************************************************************* *)

Definition out_key (o : Q1AuxSpec.OutRow) : Z * Z :=
  (Q1AuxSpec.out_rf o, Q1AuxSpec.out_ls o).

Lemma OutRle_transitive : Transitive OutRle.
Proof.
  intros x y z Hxy Hyz.
  unfold OutRle in *.
  eapply OutRowLe_trans; eassumption.
Qed.

Lemma OutRle_antisym_keys :
  forall a b, OutRle a b -> OutRle b a -> out_key a = out_key b.
Proof.
  intros a b Hab Hba.
  destruct (OutRowLe_antisym a b Hab Hba) as [Erf Els].
  unfold out_key; f_equal; assumption.
Qed.

Lemma NoDup_out_key_inj :
  forall l a b,
    NoDup (map out_key l) ->
    In a l -> In b l ->
    out_key a = out_key b ->
    a = b.
Proof.
  induction l as [|x xs IH]; intros a b Hnd Hin_a Hin_b Heq; [contradiction|].
  simpl in Hnd; inversion Hnd as [|? ? Hnin Hnd']; subst.
  destruct Hin_a as [Ha|Ha]; destruct Hin_b as [Hb|Hb].
  - subst; reflexivity.
  - subst a. exfalso. apply Hnin. apply in_map_iff.
    exists b; split; [symmetry; exact Heq|exact Hb].
  - subst b. exfalso. apply Hnin. apply in_map_iff.
    exists a; split; [exact Heq|exact Ha].
  - apply (IH a b Hnd' Ha Hb Heq).
Qed.

Lemma Sorted_OutRle_forall :
  forall a l, Sorted OutRle (a :: l) -> Forall (OutRle a) l.
Proof.
  intros a l Hs.
  apply Sorted_extends; [exact OutRle_transitive|exact Hs].
Qed.

Lemma Sorted_OutRle_perm_eq :
  forall l1 l2,
    Sorted OutRle l1 ->
    Sorted OutRle l2 ->
    Permutation l1 l2 ->
    NoDup (map out_key l1) ->
    l1 = l2.
Proof.
  intros l1; induction l1 as [|a l1' IH]; intros l2 Hs1 Hs2 HP Hnd.
  - apply Permutation_nil in HP; symmetry; exact HP.
  - destruct l2 as [|b l2'].
    + apply Permutation_sym, Permutation_nil in HP; discriminate.
    + assert (Hin_a : In a (b :: l2'))
        by (apply (Permutation_in a HP); left; reflexivity).
      assert (Hin_b : In b (a :: l1'))
        by (apply Permutation_sym in HP;
            apply (Permutation_in b HP); left; reflexivity).
      assert (Ha_eq_b : a = b).
      { destruct Hin_a as [Heq|Hin_a']; [symmetry; exact Heq|].
        destruct Hin_b as [Heq|Hin_b']; [exact Heq|].
        pose proof (Sorted_OutRle_forall a l1' Hs1) as Hfa.
        pose proof (Sorted_OutRle_forall b l2' Hs2) as Hfb.
        rewrite Forall_forall in Hfa, Hfb.
        pose proof (OutRle_antisym_keys a b (Hfa b Hin_b') (Hfb a Hin_a')) as Hk.
        exact (NoDup_out_key_inj (a :: l1') a b Hnd
                 ltac:(left; reflexivity) ltac:(right; exact Hin_b') Hk). }
      subst b.
      f_equal.
      apply IH.
      * inversion Hs1; assumption.
      * inversion Hs2; assumption.
      * apply Permutation_cons_inv in HP; exact HP.
      * simpl in Hnd; inversion Hnd; assumption.
Qed.

Lemma OutRowSort_perm_unique :
  forall l1 l2,
    Permutation l1 l2 ->
    NoDup (map out_key l1) ->
    Q1AuxSpec.OutRowSort l1 = Q1AuxSpec.OutRowSort l2.
Proof.
  intros l1 l2 HP Hnd.
  apply Sorted_OutRle_perm_eq.
  - apply OutRowSort_sorted.
  - apply OutRowSort_sorted.
  - apply Permutation_trans with l1.
    + apply Permutation_sym, OutRowSort_perm.
    + apply Permutation_trans with l2; [exact HP|apply OutRowSort_perm].
  - apply Permutation_NoDup with (l := map out_key l1); [|exact Hnd].
    apply Permutation_map, OutRowSort_perm.
Qed.

(** SpecFinalize has unique (rf,ls) keys under Q1ByteKeys. *)
Lemma NoDup_fst_unique_snd :
  forall {A B} (l : list (A * B)) k (v1 v2 : B),
    NoDup (map fst l) ->
    In (k, v1) l ->
    In (k, v2) l ->
    v1 = v2.
Proof.
  intros A B l k v1 v2.
  induction l as [|[g s] rest IH]; intros Hnd Hin1 Hin2; [contradiction|].
  inversion Hnd as [|? ? Hnin Hnd']; subst.
  destruct Hin1 as [Heq1|Hin1]; destruct Hin2 as [Heq2|Hin2].
  - inversion Heq1; inversion Heq2; subst; reflexivity.
  - inversion Heq1; subst g s.
    exfalso. apply Hnin. apply in_map_iff.
    exists (k, v2); split; [reflexivity|exact Hin2].
  - inversion Heq2; subst g s.
    exfalso. apply Hnin. apply in_map_iff.
    exists (k, v1); split; [reflexivity|exact Hin1].
  - apply (IH Hnd' Hin1 Hin2).
Qed.

Lemma SpecFinalize_key_inj :
  forall db c o1 o2,
    Q1ByteKeys db ->
    In o1 (Q1AuxSpec.SpecFinalize
             (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) c)) ->
    In o2 (Q1AuxSpec.SpecFinalize
             (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) c)) ->
    out_key o1 = out_key o2 ->
    o1 = o2.
Proof.
  intros db c o1 o2 Hbyte Hin1 Hin2 Hk.
  set (entries := Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) c).
  destruct (SpecFinalize_out_key entries o1 Hin1)
    as [g1 [s1 [Hin_e1 [Hpos1 Hf1]]]].
  destruct (SpecFinalize_out_key entries o2 Hin2)
    as [g2 [s2 [Hin_e2 [Hpos2 Hf2]]]].
  destruct (SpecView_entry_char db c g1 s1 Hbyte Hin_e1)
    as [rf1 [ls1 [Hg1 [Hrf1 [Hls1 [Hs1 _]]]]]].
  destruct (SpecView_entry_char db c g2 s2 Hbyte Hin_e2)
    as [rf2 [ls2 [Hg2 [Hrf2 [Hls2 [Hs2 _]]]]]].
  rewrite Hg1 in Hf1, Hin_e1; rewrite Hg2 in Hf2, Hin_e2.
  rewrite SpecFinalizeOne_positive in Hf1 by lia.
  rewrite SpecFinalizeOne_positive in Hf2 by lia.
  rewrite Unpack_Pack_byte in Hf1 by lia.
  rewrite Unpack_Pack_byte in Hf2 by lia.
  inversion Hf1; inversion Hf2; subst o1 o2; clear Hf1 Hf2.
  unfold out_key in Hk; simpl in Hk.
  inversion Hk; subst rf2 ls2.
  pose proof (SpecView_NoDup_fst (Q1AuxSpec.FoldSpec db) c) as Hnd.
  fold entries in Hnd.
  assert (Hs_eq : s1 = s2)
    by (apply (NoDup_fst_unique_snd entries (Q1AuxSpec.Pack rf1 ls1) s1 s2 Hnd Hin_e1 Hin_e2)).
  clear Hs1 Hs2 Hpos1 Hpos2 Hg1 Hg2 Hrf1 Hls1 Hrf2 Hls2 Hbyte Hin1 Hin2.
  rewrite Hs_eq; reflexivity.
Qed.

Lemma SpecFinalize_NoDup_keys :
  forall db c,
    Q1ByteKeys db ->
    NoDup (map out_key
      (Q1AuxSpec.SpecFinalize
         (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) c))).
Proof.
  intros db c Hbyte.
  apply NoDup_map_NoDup_ForallPairs.
  - intros o1 o2 Hin1 Hin2 Heq.
    apply (SpecFinalize_key_inj db c o1 o2 Hbyte Hin1 Hin2 Heq).
  - apply SpecFinalize_NoDup_Q1; exact Hbyte.
Qed.

(** *******************************************************************
    H. Q1Adapt ↔ result_row_sort ↔ OutRowSort homomorphism
    ******************************************************************* *)

Lemma Q1AdaptRow_key :
  forall row o,
    Q1AdaptRow row = Some o ->
    exists rf ls,
      sr_key row = [rf; ls] /\
      Q1AuxSpec.out_rf o = rf /\
      Q1AuxSpec.out_ls o = ls.
Proof.
  intros [k a] o H.
  unfold Q1AdaptRow in H; simpl in H.
  destruct k as [|rf [|ls ktl]]; try discriminate.
  destruct ktl; try discriminate.
  destruct a as
    [|[sq cq] [|[sb cb] [|[sdp cdp] [|[sc cc]
     [|[saq caq] [|[sap cap] [|[sad cad] [|[cst cct] tl]]]]]]]];
    try discriminate.
  destruct tl; try discriminate.
  destruct (Z.leb cct 0); [discriminate|].
  inversion H; subst o; simpl.
  exists rf, ls; split; [reflexivity|split; reflexivity].
Qed.

Lemma result_row_le_OutRowLe :
  forall a b oa ob,
    Q1AdaptRow a = Some oa ->
    Q1AdaptRow b = Some ob ->
    result_row_le q1_ls_order_keys q1_ls_group_keys a b =
    Some (Q1AuxSpec.OutRowLe oa ob).
Proof.
  intros a b oa ob Ha Hb.
  destruct (Q1AdaptRow_key a oa Ha) as [rf1 [ls1 [Hk1 [Hr1 Hl1]]]].
  destruct (Q1AdaptRow_key b ob Hb) as [rf2 [ls2 [Hk2 [Hr2 Hl2]]]].
  rewrite (result_row_le_Q1_keys a b rf1 ls1 rf2 ls2 Hk1 Hk2).
  assert (Hoale :
    Q1AuxSpec.OutRowLe oa ob =
      if Z.eqb rf1 rf2 then Z.leb ls1 ls2 else Z.leb rf1 rf2).
  { unfold Q1AuxSpec.OutRowLe. rewrite Hr1, Hl1, Hr2, Hl2. reflexivity. }
  rewrite Hoale.
  destruct (Z.eqb rf1 rf2).
  - destruct (Z.eqb ls1 ls2) eqn:Els.
    + apply Z.eqb_eq in Els.
      clear Hr1 Hl1 Hr2 Hl2 Ha Hb Hk1 Hk2.
      rewrite Els.
      replace (Z.leb ls2 ls2) with true by (symmetry; apply Z.leb_le; lia).
      reflexivity.
    + reflexivity.
  - reflexivity.
Qed.

Lemma Q1Adapt_cons_inv :
  forall r rest outs,
    Q1Adapt (r :: rest) = Some outs ->
    exists o os,
      Q1AdaptRow r = Some o /\
      Q1Adapt rest = Some os /\
      outs = o :: os.
Proof.
  intros r rest outs H.
  cbn [Q1Adapt] in H.
  destruct (Q1AdaptRow r) as [o|] eqn:Ho; [|discriminate].
  destruct (Q1Adapt rest) as [os|] eqn:Hos; [|discriminate].
  inversion H; subst outs.
  exists o, os; split; [reflexivity|split; reflexivity].
Qed.

Lemma Q1Adapt_result_row_insert :
  forall x xs o os,
    Q1AdaptRow x = Some o ->
    Q1Adapt xs = Some os ->
    exists xs',
      result_row_insert q1_ls_order_keys q1_ls_group_keys x xs = Some xs' /\
      Q1Adapt xs' = Some (Q1AuxSpec.OutRowInsert o os).
Proof.
  intros x xs o os Hx; revert os.
  induction xs as [|y ys IH]; intros os Hos.
  - inversion Hos; subst os.
    exists [x]; split; [reflexivity|].
    cbn [Q1Adapt]. rewrite Hx; reflexivity.
  - destruct (Q1Adapt_cons_inv y ys os Hos)
      as [oy [oys [Hy [Hys Hos_eq]]]].
    subst os.
    cbn [result_row_insert].
    rewrite (result_row_le_OutRowLe x y o oy Hx Hy).
    destruct (Q1AuxSpec.OutRowLe o oy) eqn:Ele.
    + exists (x :: y :: ys); split; [reflexivity|].
      simpl. rewrite Hx, Hy, Hys.
      cbn [Q1AuxSpec.OutRowInsert]. rewrite Ele. reflexivity.
    + destruct (IH oys Hys) as [ys' [Hins Hadapt]].
      rewrite Hins.
      exists (y :: ys'); split; [reflexivity|].
      simpl. rewrite Hy, Hadapt.
      cbn [Q1AuxSpec.OutRowInsert]. rewrite Ele. reflexivity.
Qed.

Lemma Q1Adapt_result_row_sort :
  forall rows outs,
    Q1Adapt rows = Some outs ->
    exists rows',
      result_row_sort q1_ls_order_keys q1_ls_group_keys rows = Some rows' /\
      Q1Adapt rows' = Some (Q1AuxSpec.OutRowSort outs).
Proof.
  induction rows as [|r rest IH]; intros outs Hadapt.
  - inversion Hadapt; subst outs.
    exists []; split; reflexivity.
  - destruct (Q1Adapt_cons_inv r rest outs Hadapt)
      as [o [os [Hr [Hrest Heq]]]].
    subst outs.
    destruct (IH os Hrest) as [rest' [Hsort Hadapt']].
    cbn [result_row_sort].
    rewrite Hsort.
    destruct (Q1Adapt_result_row_insert r rest' o (Q1AuxSpec.OutRowSort os)
                Hr Hadapt') as [rows' [Hins Hadapt'']].
    rewrite Hins.
    exists rows'; split; [reflexivity|exact Hadapt''].
Qed.

(** *******************************************************************
    Compose: Eval_Spec_Q1_eq_AuxAnswerSpec
    ******************************************************************* *)

Lemma Q1Adapt_store_eq_OutRowSort_SpecFinalize :
  forall c db st,
    Q1ByteKeys db ->
    StoreMirrors st (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db) ->
    exists outs,
      Q1Adapt (store_to_rows st) = Some outs /\
      Q1AuxSpec.OutRowSort outs =
        Q1AuxSpec.AuxAnswerSpec db c.
Proof.
  intros c db st Hbyte Hm.
  destruct (Q1Adapt_store_to_rows_ok st _ Hm) as [outs [Hadapt Hperm_flat]].
  exists outs; split; [exact Hadapt|].
  unfold Q1AuxSpec.AuxAnswerSpec.
  (* outs ~ flat_map ~ SpecFinalize, so OutRowSort outs = OutRowSort SpecFinalize *)
  assert (HP : Permutation outs
    (Q1AuxSpec.SpecFinalize
       (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) c))).
  { eapply Permutation_trans; [exact Hperm_flat|].
    apply store_finalize_perm_SpecFinalize; assumption. }
  apply OutRowSort_perm_unique.
  - exact HP.
  - eapply Permutation_NoDup; [apply Permutation_map, Permutation_sym, HP|].
    apply SpecFinalize_NoDup_keys; exact Hbyte.
Qed.

Lemma Q1_IR_eval_eq_AuxAnswerSpec :
  forall c db,
    Q1ByteKeys db ->
    Q1_IR_eval c db = Some (Q1AuxSpec.AuxAnswerSpec db c).
Proof.
  intros c db Hbyte.
  unfold Q1_IR_eval.
  rewrite filter_rows_Q1.
  set (filtered := filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db).
  assert (Hbyte_f : Q1ByteKeys filtered) by (apply Q1ByteKeys_filter; exact Hbyte).
  destruct (fold_groups_Q1_from_empty (params_of_cutoff c) filtered Hbyte_f)
    as [st [Hfold Hm]].
  rewrite Hfold.
  rewrite (filter_nonempty_StoreMirrors st filtered Hm).
  destruct (Q1Adapt_store_eq_OutRowSort_SpecFinalize c db st Hbyte Hm)
    as [outs [Hadapt Hsort]].
  destruct (Q1Adapt_result_row_sort (store_to_rows st) outs Hadapt)
    as [rows' [Hrs Hadapt']].
  rewrite Hrs, Hadapt', Hsort.
  reflexivity.
Qed.

Theorem Eval_Spec_Q1_eq_AuxAnswerSpec :
  forall c db,
    Q1ByteKeys db ->
    Eval_Spec_Q1_rows LogicalSpec_Q1 (params_of_cutoff c) db =
    Some (Q1AuxSpec.AuxAnswerSpec db c).
Proof.
  intros c db Hbyte.
  rewrite Eval_Spec_Q1_eq_IR.
  apply Q1_IR_eval_eq_AuxAnswerSpec; exact Hbyte.
Qed.

Theorem Eval_Spec_LogicalSpec_Q1 :
  forall c db,
    Q1ByteKeys db ->
    Eval_Spec_Q1_rows LogicalSpec_Q1 (params_of_cutoff c) db =
    Some (EvalSpec_Q1 (S_Q1 c) db).
Proof.
  intros c db Hbyte.
  rewrite Eval_Spec_Q1_eq_AuxAnswerSpec by exact Hbyte.
  rewrite EvalSpec_S_Q1.
  reflexivity.
Qed.

Corollary Eval_Spec_Q1_eq_AuxAnswerSpec_pre :
  forall c db delta,
    Preconditions_Q1 db delta ->
    Eval_Spec_Q1_rows LogicalSpec_Q1 (params_of_cutoff c) db =
    Some (Q1AuxSpec.AuxAnswerSpec db c).
Proof.
  intros c db delta Hpre.
  apply Eval_Spec_Q1_eq_AuxAnswerSpec, (Q1ByteKeys_RowOk db delta Hpre).
Qed.
