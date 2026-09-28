(* Q1RelNodeBridge — RelNode ↔ flat Q1 observational bridge.

   Under Q1ByteKeys:
     Eval_Spec LogicalSpec_Q1_rel (param_cutoff c) (db_of_aux db)
       = flat_rows_to_rel
           (F.Eval_Spec LogicalSpec_Q1 (params_of_cutoff c) db)
     and therefore
       Eval_Spec_Q1_rows_rel c db
         = F.Eval_Spec_Q1_rows LogicalSpec_Q1 (params_of_cutoff c) db
         = Some (AuxAnswerSpec db c).

   Compositional lemmas:
     Q1GroupStore_rel_eq_flat
     Q1Finalize_rel_eq_flat
     Q1Project_rel_eq_flat
     Q1Sort_rel_eq_flat
     Eval_Spec_rel_eq_flat_convert *)

From Stdlib Require Import List ZArith Bool Lia.
Import ListNotations.
Open Scope Z_scope.

From FormalSQLLogical Require Import SqlTypes Schema Expr Agg RelNode Eval.
From FormalSQLResultSpec Require Import LogicalSpec.
From FormalSQLResultSpec Require EvalLogicalSpec.
From JetStreamFS Require Import Q1AuxSpec.

Require Import Q1Schema.
Require Import Q1LogicalSpecTree.
Require Import Q1Adapt.
Require Import Q1EvalMigration.
Require Q1FormalSQLProof.

Module F := EvalLogicalSpec.
Module FlatLS := LogicalSpec.

(** * Flat SpecResultRow → RelNode projected Row *)

Definition flat_row_to_rel (row : F.SpecResultRow) : option Row :=
  match F.sr_key row, F.sr_acc row with
  | [rf; ls],
    [(sq, _); (sb, _); (sdp, _); (sc, _);
     (saq, caq); (sap, cap); (sad, cad); (_, cct)] =>
      Some [VInt rf; VInt ls;
            VInt sq; VInt sb; VInt sdp; VInt sc;
            VAvgExact saq caq; VAvgExact sap cap; VAvgExact sad cad;
            VInt cct]
  | _, _ => None
  end.

Fixpoint flat_rows_to_rel (rows : list F.SpecResultRow) : option (list Row) :=
  match rows with
  | [] => Some []
  | r :: rest =>
      match flat_row_to_rel r, flat_rows_to_rel rest with
      | Some x, Some xs => Some (x :: xs)
      | _, _ => None
      end
  end.

Lemma Q1AdaptRow_rel_of_flat :
  forall row o,
    F.Q1AdaptRow row = Some o ->
    match flat_row_to_rel row with
    | Some r => Q1AdaptRow r = Some o
    | None => False
    end.
Proof.
  intros row o H.
  unfold F.Q1AdaptRow, flat_row_to_rel, Q1AdaptRow in *.
  destruct (F.sr_key row) as [|rf k1]; try discriminate.
  destruct k1 as [|ls k2]; try discriminate.
  destruct k2; try discriminate.
  destruct (F.sr_acc row) as [|a0 t0]; try discriminate.
  destruct a0 as [sq cq].
  destruct t0 as [|a1 t1]; try discriminate.
  destruct a1 as [sb cb].
  destruct t1 as [|a2 t2]; try discriminate.
  destruct a2 as [sdp cdp].
  destruct t2 as [|a3 t3]; try discriminate.
  destruct a3 as [sc cc].
  destruct t3 as [|a4 t4]; try discriminate.
  destruct a4 as [saq caq].
  destruct t4 as [|a5 t5]; try discriminate.
  destruct a5 as [sap cap].
  destruct t5 as [|a6 t6]; try discriminate.
  destruct a6 as [sad cad].
  destruct t6 as [|a7 t7]; try discriminate.
  destruct a7 as [sct cct].
  destruct t7; try discriminate.
  simpl in *.
  destruct (Z.leb cct 0) eqn:Ele; try discriminate.
  inversion H; subst; reflexivity.
Qed.

Lemma Q1Adapt_rel_of_flat :
  forall rows outs,
    F.Q1Adapt rows = Some outs ->
    exists rs, flat_rows_to_rel rows = Some rs /\ Q1Adapt rs = Some outs.
Proof.
  induction rows as [|r rest IH]; intros outs H.
  - exists []; simpl in H; inversion H; subst; split; reflexivity.
  - simpl in H.
    destruct (F.Q1AdaptRow r) as [o|] eqn:Ho; [|discriminate].
    set (ar := F.Q1Adapt rest) in H.
    assert (Har : ar = F.Q1Adapt rest) by reflexivity.
    destruct ar as [os|]; [|discriminate].
    inversion H; subst; clear H.
    assert (Hos : F.Q1Adapt rest = Some os) by (symmetry; exact Har).
    pose proof (Q1AdaptRow_rel_of_flat r o Ho) as Hr.
    destruct (flat_row_to_rel r) as [rr|] eqn:Hfr; [|exfalso; exact Hr].
    destruct (IH os Hos) as [rrs [Hrest Heq]].
    exists (rr :: rrs). simpl.
    rewrite Hfr, Hrest, Hr, Heq. split; reflexivity.
Qed.

(** * Filter / leaf evaluation *)

Lemma eval_pred_keep_shipdate :
  forall c r,
    eval_pred_keep q1_pred_shipdate (param_cutoff c) (row_of_aux r) =
    Some (Z.leb (Q1AuxSpec.shipdate r) c).
Proof.
  intros c r.
  unfold eval_pred_keep, eval_pred, eval_expr, q1_pred_shipdate.
  unfold e_shipdate, e_cutoff, param_cutoff, row_of_aux.
  unfold params_lookup, ctx_lookup_input, row_nth, nth_error.
  simpl.
  unfold cmp_truth, cmp_int_leb, truth3_of_bool, truth3_is_true.
  simpl.
  destruct (Z.leb (Q1AuxSpec.shipdate r) c); reflexivity.
Qed.

Lemma filter_rel_Q1 :
  forall c db,
    filter_rows q1_pred_shipdate (param_cutoff c) (map row_of_aux db) =
    Some (map row_of_aux
            (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db)).
Proof.
  intros c db; induction db as [|r rest IH]; [reflexivity|].
  simpl.
  rewrite eval_pred_keep_shipdate, IH.
  destruct (Z.leb (Q1AuxSpec.shipdate r) c); reflexivity.
Qed.

Lemma filter_rel_eq_flat_filter :
  forall c db,
    filter_rows q1_pred_shipdate (param_cutoff c) (map row_of_aux db) =
    match F.filter_rows FlatLS.q1_filters (FlatLS.params_of_cutoff c) db with
    | Some rs => Some (map row_of_aux rs)
    | None => None
    end.
Proof.
  intros c db.
  rewrite filter_rel_Q1, F.filter_rows_Q1.
  reflexivity.
Qed.

Lemma eval_exprs_group_rel :
  forall c r,
    eval_exprs q1_group_exprs (param_cutoff c) (EvRow (row_of_aux r)) =
    Some [VInt (Q1AuxSpec.rf r); VInt (Q1AuxSpec.ls r)].
Proof. intros; simpl; reflexivity. Qed.

Lemma aggs_contrib_rel_Q1 :
  forall c r,
    aggs_contrib q1_aggs (param_cutoff c) (row_of_aux r) =
    Some [Some (VInt (Q1AuxSpec.qty r));
          Some (VInt (Q1AuxSpec.price r));
          Some (VInt (Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r)));
          Some (VInt (Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r)
                        * (100 + Q1AuxSpec.tax r)));
          Some (VInt (Q1AuxSpec.qty r));
          Some (VInt (Q1AuxSpec.price r));
          Some (VInt (Q1AuxSpec.disc r));
          Some (VInt 1)].
Proof. intros; simpl; reflexivity. Qed.

(** *******************************************************************
    AccSlot ↔ flat (sum, count)
    ******************************************************************* *)

Definition slot_of_pair (sc : Z * Z) : AccSlot :=
  let '(s, c) := sc in mkAccSlot s c (negb (Z.eqb c 0)).

Definition acc_of_flat (a : F.AggAcc) : AggAcc := List.map slot_of_pair a.
Definition keys_of_flat (k : list Z) : list SqlValue := List.map VInt k.
Definition store_of_flat (st : F.GroupStore) : GroupStore :=
  List.map (fun '(k, a) => (keys_of_flat k, acc_of_flat a)) st.

Definition AccCountsNonneg (a : F.AggAcc) : Prop :=
  Forall (fun '(_, c) => 0 <= c) a.

Definition StoreAccOk (st : F.GroupStore) : Prop :=
  Forall (fun '(_, a) => AccCountsNonneg a /\ List.length a = 8%nat) st.

Lemma empty_acc_of_flat :
  forall n, empty_acc n = acc_of_flat (F.empty_acc n).
Proof.
  induction n as [|n IH]; [reflexivity|].
  simpl. rewrite IH. reflexivity.
Qed.

Lemma AccCountsNonneg_empty :
  forall n, AccCountsNonneg (F.empty_acc n).
Proof.
  induction n as [|n IH]; [constructor|].
  unfold F.empty_acc; simpl. constructor; [lia|exact IH].
Qed.

Lemma list_Z_eqb_keys_of_flat :
  forall k k',
    list_sql_value_eqb (keys_of_flat k) (keys_of_flat k') = F.list_Z_eqb k k'.
Proof.
  induction k as [|x xs IH]; intros k'; simpl.
  - destruct k'; reflexivity.
  - destruct k' as [|y ys]; [reflexivity|].
    simpl. rewrite IH. reflexivity.
Qed.

Lemma store_find_of_flat :
  forall st k,
    store_find (store_of_flat st) (keys_of_flat k) =
    match F.store_get st k with
    | Some a => Some (acc_of_flat a)
    | None => None
    end.
Proof.
  induction st as [|[k' a'] rest IH]; intros k; simpl; [reflexivity|].
  rewrite list_Z_eqb_keys_of_flat.
  destruct (F.list_Z_eqb k k'); [reflexivity|apply IH].
Qed.

Lemma store_update_of_flat :
  forall st k a,
    store_update (store_of_flat st) (keys_of_flat k) (acc_of_flat a) =
    store_of_flat (F.store_put st k a).
Proof.
  induction st as [|[k' a'] rest IH]; intros k a; simpl; [reflexivity|].
  rewrite list_Z_eqb_keys_of_flat.
  destruct (F.list_Z_eqb k k'); [reflexivity|].
  simpl. rewrite IH. reflexivity.
Qed.

Lemma acc_nonempty_of_flat :
  forall a, acc_nonempty (acc_of_flat a) = F.acc_nonempty a.
Proof.
  induction a as [|[s c] rest IH]; [reflexivity|].
  change (acc_nonempty (slot_of_pair (s, c) :: acc_of_flat rest) =
          F.acc_nonempty ((s, c) :: rest)).
  unfold acc_nonempty, F.acc_nonempty; simpl.
  unfold slot_of_pair; simpl.
  destruct (Z.eqb c 0); simpl; [exact IH|reflexivity].
Qed.

Lemma acc_add_slot_sum_pair_nonneg :
  forall s c z, 0 <= c ->
    acc_add_slot Agg.AggSum (slot_of_pair (s, c)) (Some (VInt z)) =
    Some (slot_of_pair (s + z, c + 1)).
Proof.
  intros s c z Hc. unfold acc_add_slot, slot_of_pair; simpl.
  destruct (Z.eqb (c + 1) 0) eqn:Ez; [apply Z.eqb_eq in Ez; lia|reflexivity].
Qed.

Lemma acc_add_slot_avg_pair_nonneg :
  forall s c z, 0 <= c ->
    acc_add_slot Agg.AggAvg (slot_of_pair (s, c)) (Some (VInt z)) =
    Some (slot_of_pair (s + z, c + 1)).
Proof.
  intros s c z Hc. unfold acc_add_slot, slot_of_pair; simpl.
  destruct (Z.eqb (c + 1) 0) eqn:Ez; [apply Z.eqb_eq in Ez; lia|reflexivity].
Qed.

Lemma acc_add_slot_countstar_pair_nonneg :
  forall s c, 0 <= c ->
    acc_add_slot Agg.AggCountStar (slot_of_pair (s, c)) (Some (VInt 1)) =
    Some (slot_of_pair (s + 1, c + 1)).
Proof.
  intros s c Hc. unfold acc_add_slot, slot_of_pair; simpl.
  destruct (Z.eqb (c + 1) 0) eqn:Ez; [apply Z.eqb_eq in Ez; lia|reflexivity].
Qed.

Lemma F_acc_add_one_Q1_len_nonneg :
  forall a v0 v1 v2 v3 v4 v5 v6 v7 a',
    AccCountsNonneg a ->
    List.length a = 8%nat ->
    F.acc_add_one a [v0; v1; v2; v3; v4; v5; v6; v7] = Some a' ->
    AccCountsNonneg a' /\ List.length a' = 8%nat.
Proof.
  intros a v0 v1 v2 v3 v4 v5 v6 v7 a' Hnn Hlen Hadd.
  destruct a as [|p0 a]; [discriminate Hlen|].
  destruct a as [|p1 a]; [discriminate Hlen|].
  destruct a as [|p2 a]; [discriminate Hlen|].
  destruct a as [|p3 a]; [discriminate Hlen|].
  destruct a as [|p4 a]; [discriminate Hlen|].
  destruct a as [|p5 a]; [discriminate Hlen|].
  destruct a as [|p6 a]; [discriminate Hlen|].
  destruct a as [|p7 a]; [discriminate Hlen|].
  destruct a; [|discriminate Hlen].
  destruct p0 as [s0 c0]; destruct p1 as [s1 c1]; destruct p2 as [s2 c2];
    destruct p3 as [s3 c3]; destruct p4 as [s4 c4]; destruct p5 as [s5 c5];
    destruct p6 as [s6 c6]; destruct p7 as [s7 c7].
  inversion Hnn as [|? ? Hc0 H1]; subst.
  inversion H1 as [|? ? Hc1 H2]; subst.
  inversion H2 as [|? ? Hc2 H3]; subst.
  inversion H3 as [|? ? Hc3 H4]; subst.
  inversion H4 as [|? ? Hc4 H5]; subst.
  inversion H5 as [|? ? Hc5 H6]; subst.
  inversion H6 as [|? ? Hc6 H7]; subst.
  inversion H7 as [|? ? Hc7 _]; subst.
  unfold F.acc_add_one in Hadd; simpl in Hadd.
  inversion Hadd; subst; clear Hadd.
  split; [|reflexivity].
  repeat (constructor; [lia|]); constructor.
Qed.

Lemma F_acc_add_row_Q1_len_nonneg :
  forall p r a a',
    AccCountsNonneg a ->
    List.length a = 8%nat ->
    F.acc_add_row FlatLS.q1_aggs p r a = Some a' ->
    AccCountsNonneg a' /\ List.length a' = 8%nat.
Proof.
  intros p r a a' Hnn Hlen Hadd.
  unfold F.acc_add_row in Hadd.
  rewrite F.aggs_contrib_Q1 in Hadd.
  eapply F_acc_add_one_Q1_len_nonneg; eassumption.
Qed.

Lemma StoreAccOk_empty : StoreAccOk [].
Proof. constructor. Qed.

Lemma StoreAccOk_put :
  forall st k a,
    StoreAccOk st -> AccCountsNonneg a -> List.length a = 8%nat ->
    StoreAccOk (F.store_put st k a).
Proof.
  intros st k a Hok Hnn Hlen.
  induction st as [|[k' a'] rest IH]; simpl.
  - constructor; [split; assumption|constructor].
  - inversion Hok; subst.
    destruct (F.list_Z_eqb k k').
    + constructor; [split; assumption|assumption].
    + constructor; [assumption|apply IH; assumption].
Qed.

Lemma StoreAccOk_get :
  forall st k a,
    StoreAccOk st -> F.store_get st k = Some a ->
    AccCountsNonneg a /\ List.length a = 8%nat.
Proof.
  intros st k a Hok Hget.
  induction st as [|[k' a'] rest IH]; [discriminate Hget|].
  simpl in Hget. inversion Hok; subst.
  destruct (F.list_Z_eqb k k').
  - inversion Hget; subst; exact H1.
  - apply IH; assumption.
Qed.

Lemma acc_add_one_Q1_of_flat :
  forall a v0 v1 v2 v3 v4 v5 v6,
    AccCountsNonneg a -> List.length a = 8%nat ->
    acc_add_one q1_aggs (acc_of_flat a)
      [Some (VInt v0); Some (VInt v1); Some (VInt v2); Some (VInt v3);
       Some (VInt v4); Some (VInt v5); Some (VInt v6); Some (VInt 1)] =
    match F.acc_add_one a [v0; v1; v2; v3; v4; v5; v6; 1] with
    | Some a' => Some (acc_of_flat a')
    | None => None
    end.
Proof.
  intros a v0 v1 v2 v3 v4 v5 v6 Hnn Hlen.
  destruct a as [|p0 a]; [discriminate Hlen|].
  destruct a as [|p1 a]; [discriminate Hlen|].
  destruct a as [|p2 a]; [discriminate Hlen|].
  destruct a as [|p3 a]; [discriminate Hlen|].
  destruct a as [|p4 a]; [discriminate Hlen|].
  destruct a as [|p5 a]; [discriminate Hlen|].
  destruct a as [|p6 a]; [discriminate Hlen|].
  destruct a as [|p7 a]; [discriminate Hlen|].
  destruct a; [|discriminate Hlen].
  destruct p0 as [s0 c0]; destruct p1 as [s1 c1]; destruct p2 as [s2 c2];
    destruct p3 as [s3 c3]; destruct p4 as [s4 c4]; destruct p5 as [s5 c5];
    destruct p6 as [s6 c6]; destruct p7 as [s7 c7].
  inversion Hnn as [|? ? Hc0 H1]; subst.
  inversion H1 as [|? ? Hc1 H2]; subst.
  inversion H2 as [|? ? Hc2 H3]; subst.
  inversion H3 as [|? ? Hc3 H4]; subst.
  inversion H4 as [|? ? Hc4 H5]; subst.
  inversion H5 as [|? ? Hc5 H6]; subst.
  inversion H6 as [|? ? Hc6 H7]; subst.
  inversion H7 as [|? ? Hc7 _]; subst.
  unfold acc_of_flat, slot_of_pair; simpl.
  unfold F.acc_add_one; simpl.
  unfold acc_add_one, q1_aggs, AggSumOf, AggAvgOf, AggCountStarSpec,
         agg_kind, acc_add_slot; simpl.
  assert (E0 : Z.eqb (c0 + 1) 0 = false) by (apply Z.eqb_neq; lia).
  assert (E1 : Z.eqb (c1 + 1) 0 = false) by (apply Z.eqb_neq; lia).
  assert (E2 : Z.eqb (c2 + 1) 0 = false) by (apply Z.eqb_neq; lia).
  assert (E3 : Z.eqb (c3 + 1) 0 = false) by (apply Z.eqb_neq; lia).
  assert (E4 : Z.eqb (c4 + 1) 0 = false) by (apply Z.eqb_neq; lia).
  assert (E5 : Z.eqb (c5 + 1) 0 = false) by (apply Z.eqb_neq; lia).
  assert (E6 : Z.eqb (c6 + 1) 0 = false) by (apply Z.eqb_neq; lia).
  assert (E7 : Z.eqb (c7 + 1) 0 = false) by (apply Z.eqb_neq; lia).
  rewrite E0, E1, E2, E3, E4, E5, E6, E7.
  reflexivity.
Qed.

Lemma acc_add_row_rel_eq_flat_Q1 :
  forall c r a,
    AccCountsNonneg a -> List.length a = 8%nat ->
    acc_add_row q1_aggs (param_cutoff c) (row_of_aux r) (acc_of_flat a) =
    match F.acc_add_row FlatLS.q1_aggs (FlatLS.params_of_cutoff c) r a with
    | Some a' => Some (acc_of_flat a')
    | None => None
    end.
Proof.
  intros c r a Hnn Hlen.
  unfold acc_add_row, F.acc_add_row.
  rewrite aggs_contrib_rel_Q1, F.aggs_contrib_Q1.
  apply acc_add_one_Q1_of_flat; assumption.
Qed.

Lemma store_add_row_preserves_StoreAccOk :
  forall p r st st',
    StoreAccOk st ->
    F.store_add_row FlatLS.q1_aggs FlatLS.q1_ls_group_keys p r st = Some st' ->
    StoreAccOk st'.
Proof.
  intros p r st st' Hok Hadd.
  unfold F.store_add_row in Hadd.
  rewrite F.eval_exprs_Q1_group in Hadd.
  cbn beta iota zeta in Hadd.
  set (kz := [Q1AuxSpec.rf r; Q1AuxSpec.ls r]).
  change [Q1AuxSpec.rf r; Q1AuxSpec.ls r] with kz in Hadd.
  destruct (F.store_get st kz) as [a_found|] eqn:Hg;
    cbn beta iota zeta in Hadd.
  - destruct (StoreAccOk_get st kz a_found Hok Hg) as [Hnn Hlen].
    destruct (F.acc_add_row FlatLS.q1_aggs p r a_found) as [a1|] eqn:Ha;
      [|discriminate Hadd].
    inversion Hadd; subst st'; clear Hadd.
    destruct (F_acc_add_row_Q1_len_nonneg p r a_found a1 Hnn Hlen Ha) as [Hnn1 Hlen1].
    apply StoreAccOk_put; assumption.
  - assert (Hnn : AccCountsNonneg (F.empty_acc 8)) by apply AccCountsNonneg_empty.
    assert (Hlen : List.length (F.empty_acc 8) = 8%nat) by reflexivity.
    change (F.empty_acc (List.length FlatLS.q1_aggs)) with (F.empty_acc 8) in Hadd.
    destruct (F.acc_add_row FlatLS.q1_aggs p r (F.empty_acc 8)) as [a1|] eqn:Ha;
      [|discriminate Hadd].
    inversion Hadd; subst st'; clear Hadd.
    destruct (F_acc_add_row_Q1_len_nonneg p r (F.empty_acc 8) a1 Hnn Hlen Ha)
      as [Hnn1 Hlen1].
    apply StoreAccOk_put; assumption.
Qed.

Lemma store_add_row_rel_eq_flat_Q1 :
  forall c r st,
    StoreAccOk st ->
    store_add_row q1_aggs q1_group_exprs (param_cutoff c)
      (row_of_aux r) (store_of_flat st) =
    match F.store_add_row FlatLS.q1_aggs FlatLS.q1_ls_group_keys
            (FlatLS.params_of_cutoff c) r st with
    | Some st' => Some (store_of_flat st')
    | None => None
    end.
Proof.
  intros c r st Hok.
  unfold store_add_row, F.store_add_row.
  rewrite eval_exprs_group_rel, F.eval_exprs_Q1_group.
  cbn beta iota zeta.
  set (kz := [Q1AuxSpec.rf r; Q1AuxSpec.ls r]).
  change [VInt (Q1AuxSpec.rf r); VInt (Q1AuxSpec.ls r)] with (keys_of_flat kz).
  change [Q1AuxSpec.rf r; Q1AuxSpec.ls r] with kz.
  rewrite store_find_of_flat.
  destruct (F.store_get st kz) as [a_found|] eqn:Hg; cbn beta iota zeta.
  - destruct (StoreAccOk_get st kz a_found Hok Hg) as [Hnn Hlen].
    rewrite (acc_add_row_rel_eq_flat_Q1 c r a_found Hnn Hlen).
    destruct (F.acc_add_row FlatLS.q1_aggs (FlatLS.params_of_cutoff c) r a_found)
      as [a1|]; [|reflexivity].
    cbn beta iota zeta.
    rewrite store_update_of_flat. reflexivity.
  - rewrite empty_acc_of_flat.
    change (List.length q1_aggs) with 8%nat.
    change (List.length FlatLS.q1_aggs) with 8%nat.
    assert (Hnn : AccCountsNonneg (F.empty_acc 8)) by apply AccCountsNonneg_empty.
    assert (Hlen : List.length (F.empty_acc 8) = 8%nat) by reflexivity.
    rewrite (acc_add_row_rel_eq_flat_Q1 c r (F.empty_acc 8) Hnn Hlen).
    destruct (F.acc_add_row FlatLS.q1_aggs (FlatLS.params_of_cutoff c) r
                (F.empty_acc 8)) as [a1|]; [|reflexivity].
    cbn beta iota zeta.
    rewrite store_update_of_flat. reflexivity.
Qed.

Lemma Q1GroupStore_rel_eq_flat :
  forall c rs st,
    StoreAccOk st ->
    fold_groups q1_aggs q1_group_exprs (param_cutoff c)
      (map row_of_aux rs) (store_of_flat st) =
    match F.fold_groups FlatLS.q1_aggs FlatLS.q1_ls_group_keys
            (FlatLS.params_of_cutoff c) rs st with
    | Some st' => Some (store_of_flat st')
    | None => None
    end.
Proof.
  intros c rs; induction rs as [|r rest IH]; intros st Hok; [reflexivity|].
  simpl.
  rewrite (store_add_row_rel_eq_flat_Q1 c r st Hok).
  destruct (F.store_add_row FlatLS.q1_aggs FlatLS.q1_ls_group_keys
              (FlatLS.params_of_cutoff c) r st) as [st1|] eqn:Hadd;
    [|reflexivity].
  apply IH. eapply store_add_row_preserves_StoreAccOk; eassumption.
Qed.

Lemma Q1GroupStore_rel_eq_flat_from_empty :
  forall c rs,
    fold_groups q1_aggs q1_group_exprs (param_cutoff c)
      (map row_of_aux rs) [] =
    match F.fold_groups FlatLS.q1_aggs FlatLS.q1_ls_group_keys
            (FlatLS.params_of_cutoff c) rs [] with
    | Some st' => Some (store_of_flat st')
    | None => None
    end.
Proof.
  intros c rs.
  change (@nil (list SqlValue * AggAcc)) with (store_of_flat []).
  apply Q1GroupStore_rel_eq_flat, StoreAccOk_empty.
Qed.


(* Phase-1 compile checkpoint: Aggregate/GroupStore correspondence Qed. *)

(** *******************************************************************
    Finalize — via acc_of_stats (StoreMirrors entries)
    ******************************************************************* *)

Lemma Q1Finalize_acc_of_stats :
  forall s,
    0 < Q1AuxSpec.count s ->
    finalize_acc q1_aggs (acc_of_flat (F.acc_of_stats s)) =
    Some [VInt (Q1AuxSpec.sum_qty s);
          VInt (Q1AuxSpec.sum_base_price s);
          VInt (Q1AuxSpec.sum_disc_price s);
          VInt (Q1AuxSpec.sum_charge s);
          VAvgExact (Q1AuxSpec.sum_qty s) (Q1AuxSpec.count s);
          VAvgExact (Q1AuxSpec.sum_base_price s) (Q1AuxSpec.count s);
          VAvgExact (Q1AuxSpec.sum_discount s) (Q1AuxSpec.count s);
          VInt (Q1AuxSpec.count s)].
Proof.
  intros s Hpos.
  unfold finalize_acc, acc_of_flat, F.acc_of_stats, slot_of_pair,
         q1_aggs, AggSumOf, AggAvgOf, AggCountStarSpec, agg_kind,
         finalize_slot; simpl.
  assert (Ez : Z.eqb (Q1AuxSpec.count s) 0 = false)
    by (apply Z.eqb_neq; lia).
  rewrite !Ez. reflexivity.
Qed.

Lemma flat_row_to_rel_acc_of_stats :
  forall rf ls s,
    flat_row_to_rel (F.mkSpecResultRow [rf; ls] (F.acc_of_stats s)) =
    Some [VInt rf; VInt ls;
          VInt (Q1AuxSpec.sum_qty s);
          VInt (Q1AuxSpec.sum_base_price s);
          VInt (Q1AuxSpec.sum_disc_price s);
          VInt (Q1AuxSpec.sum_charge s);
          VAvgExact (Q1AuxSpec.sum_qty s) (Q1AuxSpec.count s);
          VAvgExact (Q1AuxSpec.sum_base_price s) (Q1AuxSpec.count s);
          VAvgExact (Q1AuxSpec.sum_discount s) (Q1AuxSpec.count s);
          VInt (Q1AuxSpec.count s)].
Proof. intros; reflexivity. Qed.

Lemma eval_exprs_q1_project :
  forall c rf ls v0 v1 v2 v3 v4 v5 v6 v7,
    eval_exprs q1_project_exprs (param_cutoff c)
      (EvAgg [VInt rf; VInt ls]
             [v0; v1; v2; v3; v4; v5; v6; v7]) =
    Some [VInt rf; VInt ls; v0; v1; v2; v3; v4; v5; v6; v7].
Proof. intros; reflexivity. Qed.


Lemma store_to_agg_bags_StoreMirrors :
  forall st rs,
    StoreMirrors st rs ->
    store_to_agg_bags q1_aggs (store_of_flat st) =
    Some (map (fun '(k, a) =>
      match k with
      | [rf; ls] =>
          ([VInt rf; VInt ls],
           match a with
           | _ =>
             (* filled from acc_of_stats shape *)
             match finalize_acc q1_aggs (acc_of_flat a) with
             | Some vals => vals
             | None => []
             end
           end)
      | _ => ([], [])
      end) st).
Proof.
  intros st rs Hm.
  pose proof (StoreMirrors_Forall_entries st rs Hm) as Hfa.
  clear Hm.
  induction st as [|[k a] rest IH]; [reflexivity|].
  inversion Hfa as [|? ? Hx Hfa']; subst; clear Hfa.
  destruct Hx as [rf [ls [Hk [_ [_ [Ha Hpos]]]]]].
  subst k.
  set (s := Q1AuxCorrectAssembly.sum_contrib_matching rs (Q1AuxSpec.Pack rf ls)).
  fold s in Ha; fold s in Hpos.
  rewrite Ha; clear a Ha.
  change (store_of_flat (([rf; ls], F.acc_of_stats s) :: rest))
    with (([VInt rf; VInt ls], acc_of_flat (F.acc_of_stats s))
          :: store_of_flat rest).
  unfold store_to_agg_bags;
    fold (store_to_agg_bags q1_aggs (store_of_flat rest)).
  assert (Hn : acc_nonempty (acc_of_flat (F.acc_of_stats s)) = true).
  { rewrite acc_nonempty_of_flat. apply acc_nonempty_of_stats_pos; exact Hpos. }
  rewrite Hn.
  rewrite (Q1Finalize_acc_of_stats s Hpos).
  simpl.
  rewrite (IH Hfa').
  cbn [map].
  assert (Hz : Z.eqb (Q1AuxSpec.count s) 0 = false)
    by (apply Z.eqb_neq; lia).
  rewrite Hz.
  reflexivity.
Qed.

Lemma project_bags_StoreMirrors :
  forall c st rs bags,
    StoreMirrors st rs ->
    store_to_agg_bags q1_aggs (store_of_flat st) = Some bags ->
    project_agg_rows q1_project_exprs (param_cutoff c) bags =
    flat_rows_to_rel (F.store_to_rows st).
Proof.
  intros c st rs bags Hm Hbags.
  rewrite (store_to_agg_bags_StoreMirrors st rs Hm) in Hbags.
  inversion Hbags; subst bags; clear Hbags.
  pose proof (StoreMirrors_Forall_entries st rs Hm) as Hfa.
  clear Hm.
  unfold F.store_to_rows.
  induction st as [|[k a] rest IH]; [reflexivity|].
  inversion Hfa as [|? ? Hx Hfa']; subst; clear Hfa.
  destruct Hx as [rf [ls [Hk [_ [_ [Ha Hpos]]]]]].
  subst k.
  set (s := Q1AuxCorrectAssembly.sum_contrib_matching rs (Q1AuxSpec.Pack rf ls)).
  fold s in Ha; fold s in Hpos.
  rewrite Ha; clear a Ha.
  assert (Hz : Z.eqb (Q1AuxSpec.count s) 0 = false)
    by (apply Z.eqb_neq; lia).
  simpl.
  rewrite Hz.
  simpl.
  rewrite (IH Hfa').
  reflexivity.
Qed.

Lemma Q1Finalize_Project_of_store :
  forall c st rs,
    StoreMirrors st rs ->
    match store_to_agg_bags q1_aggs (store_of_flat st) with
    | Some bags => project_agg_rows q1_project_exprs (param_cutoff c) bags
    | None => None
    end =
    flat_rows_to_rel (F.store_to_rows st).
Proof.
  intros c st rs Hm.
  destruct (store_to_agg_bags q1_aggs (store_of_flat st)) as [bags|] eqn:Hb.
  - eapply project_bags_StoreMirrors; eassumption.
  - rewrite (store_to_agg_bags_StoreMirrors st rs Hm) in Hb; discriminate.
Qed.

Definition Q1Finalize_rel_eq_flat := Q1Finalize_acc_of_stats.
Definition Q1Project_rel_eq_flat := Q1Finalize_Project_of_store.


(** *******************************************************************
    Sort
    ******************************************************************* *)

Lemma order_keys_le_Q1 :
  forall c rf1 ls1 rf2 ls2 ta tb,
    order_keys_le (param_cutoff c) q1_order_keys
      (VInt rf1 :: VInt ls1 :: ta)
      (VInt rf2 :: VInt ls2 :: tb) =
    if Z.eqb rf1 rf2
    then if Z.eqb ls1 ls2 then Some true else Some (Z.leb ls1 ls2)
    else Some (Z.leb rf1 rf2).
Proof.
  intros c rf1 ls1 rf2 ls2 ta tb.
  unfold order_keys_le, order_key_le, q1_order_keys, param_cutoff; simpl.
  unfold value_le; simpl.
  destruct (Z.eqb rf1 rf2) eqn:Erf.
  - apply Z.eqb_eq in Erf; subst rf2. rewrite !Z.leb_refl; simpl.
    destruct (Z.eqb ls1 ls2) eqn:Els.
    + apply Z.eqb_eq in Els; subst ls2. rewrite !Z.leb_refl; reflexivity.
    + destruct (Z.leb ls1 ls2) eqn:L1.
      * destruct (Z.leb ls2 ls1) eqn:L2.
        -- apply Z.leb_le in L1; apply Z.leb_le in L2.
           assert (ls1 = ls2) by lia. subst. rewrite Z.eqb_refl in Els; discriminate.
        -- reflexivity.
      * destruct (Z.leb ls2 ls1) eqn:L2.
        -- reflexivity.
        -- assert (ls1 <= ls2 \/ ls2 <= ls1) by lia.
           destruct H as [H|H]; apply Z.leb_le in H; congruence.
  - destruct (Z.leb rf1 rf2) eqn:L1.
    + destruct (Z.leb rf2 rf1) eqn:L2.
      * apply Z.leb_le in L1; apply Z.leb_le in L2.
        assert (rf1 = rf2) by lia. subst. rewrite Z.eqb_refl in Erf; discriminate.
      * reflexivity.
    + destruct (Z.leb rf2 rf1) eqn:L2.
      * reflexivity.
      * assert (rf1 <= rf2 \/ rf2 <= rf1) by lia.
        destruct H as [H|H]; apply Z.leb_le in H; congruence.
Qed.

Lemma result_row_le_flat_eq_rel :
  forall c a b ra rb,
    flat_row_to_rel a = Some ra ->
    flat_row_to_rel b = Some rb ->
    F.result_row_le FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys a b =
    order_keys_le (param_cutoff c) q1_order_keys ra rb.
Proof.
  intros c [ka aa] [kb ab] ra rb Ha Hb.
  unfold flat_row_to_rel in Ha, Hb; simpl in Ha, Hb.
  destruct ka as [|rf1 k1]; try discriminate.
  destruct k1 as [|ls1 k1']; try discriminate.
  destruct k1'; try discriminate.
  destruct aa as [|p0 t0]; try discriminate.
  destruct p0 as [sq1 cq1].
  destruct t0 as [|p1 t1]; try discriminate.
  destruct p1 as [sb1 cb1].
  destruct t1 as [|p2 t2]; try discriminate.
  destruct p2 as [sdp1 cdp1].
  destruct t2 as [|p3 t3]; try discriminate.
  destruct p3 as [sc1 cc1].
  destruct t3 as [|p4 t4]; try discriminate.
  destruct p4 as [saq1 caq1].
  destruct t4 as [|p5 t5]; try discriminate.
  destruct p5 as [sap1 cap1].
  destruct t5 as [|p6 t6]; try discriminate.
  destruct p6 as [sad1 cad1].
  destruct t6 as [|p7 t7]; try discriminate.
  destruct p7 as [sct1 cct1].
  destruct t7; try discriminate.
  destruct kb as [|rf2 k2]; try discriminate.
  destruct k2 as [|ls2 k2']; try discriminate.
  destruct k2'; try discriminate.
  destruct ab as [|q0 u0]; try discriminate.
  destruct q0 as [sq2 cq2].
  destruct u0 as [|q1 u1]; try discriminate.
  destruct q1 as [sb2 cb2].
  destruct u1 as [|q2 u2]; try discriminate.
  destruct q2 as [sdp2 cdp2].
  destruct u2 as [|q3 u3]; try discriminate.
  destruct q3 as [sc2 cc2].
  destruct u3 as [|q4 u4]; try discriminate.
  destruct q4 as [saq2 caq2].
  destruct u4 as [|q5 u5]; try discriminate.
  destruct q5 as [sap2 cap2].
  destruct u5 as [|q6 u6]; try discriminate.
  destruct q6 as [sad2 cad2].
  destruct u6 as [|q7 u7]; try discriminate.
  destruct q7 as [sct2 cct2].
  destruct u7; try discriminate.
  inversion Ha; inversion Hb; subst; clear Ha Hb.
  rewrite (F.result_row_le_Q1_keys _ _ rf1 ls1 rf2 ls2) by reflexivity.
  rewrite (order_keys_le_Q1 c rf1 ls1 rf2 ls2).
  reflexivity.
Qed.

Lemma row_insert_cons_true :
  forall p oks x y ys,
    order_keys_le p oks x y = Some true ->
    row_insert p oks x (y :: ys) = Some (x :: y :: ys).
Proof. intros p oks x y ys H; simpl; rewrite H; reflexivity. Qed.

Lemma row_insert_cons_false :
  forall p oks x y ys,
    order_keys_le p oks x y = Some false ->
    row_insert p oks x (y :: ys) =
    match row_insert p oks x ys with
    | Some ys' => Some (y :: ys')
    | None => None
    end.
Proof. intros p oks x y ys H; simpl; rewrite H; reflexivity. Qed.

Lemma row_insert_cons_none :
  forall p oks x y ys,
    order_keys_le p oks x y = None ->
    row_insert p oks x (y :: ys) = None.
Proof. intros p oks x y ys H; simpl; rewrite H; reflexivity. Qed.

Lemma row_insert_rel_eq_flat :
  forall c x xs rx rxs,
    flat_row_to_rel x = Some rx ->
    flat_rows_to_rel xs = Some rxs ->
    match F.result_row_insert FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys
            x xs with
    | Some ys =>
        exists rys,
          row_insert (param_cutoff c) q1_order_keys rx rxs = Some rys /\
          flat_rows_to_rel ys = Some rys
    | None =>
        row_insert (param_cutoff c) q1_order_keys rx rxs = None
    end.
Proof.
  intros c x xs rx rxs Hx.
  revert rx Hx rxs.
  induction xs as [|y ys IH]; intros rx Hx rxs Hrs.
  - simpl in Hrs; inversion Hrs; subst rxs.
    simpl. eexists; split; [reflexivity|].
    simpl. rewrite Hx; reflexivity.
  - simpl in Hrs.
    destruct (flat_row_to_rel y) as [ry|] eqn:Hy; [|discriminate].
    destruct (flat_rows_to_rel ys) as [rys|] eqn:Hys; [|discriminate].
    inversion Hrs; subst rxs; clear Hrs.
    change (F.result_row_insert FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys
              x (y :: ys)) with
      (match F.result_row_le FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys x y with
       | Some true => Some (x :: y :: ys)
       | Some false =>
           match F.result_row_insert FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys
                   x ys with
           | Some ys' => Some (y :: ys')
           | None => None
           end
       | None => None
       end).
    destruct (F.result_row_le FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys x y)
      as [ble|] eqn:Hble.
    + pose proof (result_row_le_flat_eq_rel c x y rx ry Hx Hy) as Hle.
      rewrite Hble in Hle.
      destruct ble.
      * rewrite (row_insert_cons_true _ _ _ _ _ (eq_sym Hle)).
        eexists; split; [reflexivity|].
        simpl. rewrite Hx, Hy, Hys; reflexivity.
      * specialize (IH rx Hx rys eq_refl).
        destruct (F.result_row_insert FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys
                    x ys) as [ys'|].
        -- destruct IH as [rys' [Hins Hfr]].
           rewrite (row_insert_cons_false _ _ _ _ _ (eq_sym Hle)).
           rewrite Hins.
           eexists; split; [reflexivity|].
           simpl. rewrite Hy, Hfr; reflexivity.
        -- rewrite (row_insert_cons_false _ _ _ _ _ (eq_sym Hle)).
           rewrite IH; reflexivity.
    + pose proof (result_row_le_flat_eq_rel c x y rx ry Hx Hy) as Hle.
      rewrite Hble in Hle.
      rewrite (row_insert_cons_none _ _ _ _ _ (eq_sym Hle)).
      reflexivity.
Qed.


Lemma flat_rows_to_rel_insert_preserves :
  forall x xs ys,
    (exists rx, flat_row_to_rel x = Some rx) ->
    (exists rxs, flat_rows_to_rel xs = Some rxs) ->
    F.result_row_insert FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys x xs
      = Some ys ->
    exists rys, flat_rows_to_rel ys = Some rys.
Proof.
  intros x xs; induction xs as [|y ys IH]; intros ys' Hx Hxs Hins.
  - simpl in Hins; inversion Hins; subst ys'.
    destruct Hx as [rx Hx].
    eexists. simpl. rewrite Hx; reflexivity.
  - change ((match F.result_row_le FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys x y with
             | Some true => Some (x :: y :: ys)
             | Some false =>
                 match F.result_row_insert FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys
                         x ys with
                 | Some ys1 => Some (y :: ys1)
                 | None => None
                 end
             | None => None
             end) = Some ys') in Hins.
    destruct Hxs as [rxs Hxs].
    simpl in Hxs.
    destruct (flat_row_to_rel y) as [ry|] eqn:Hy; [|discriminate Hxs].
    destruct (flat_rows_to_rel ys) as [rys|] eqn:Hys; [|discriminate Hxs].
    destruct (F.result_row_le FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys x y)
      as [ble|] eqn:Hble; [|discriminate Hins].
    destruct ble.
    + inversion Hins; subst ys'.
      destruct Hx as [rx Hx].
      eexists. simpl. rewrite Hx, Hy, Hys; reflexivity.
    + destruct (F.result_row_insert FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys
                  x ys) as [ys1|] eqn:Hins1; [|discriminate Hins].
      inversion Hins; subst ys'.
      destruct (IH ys1 Hx ltac:(exists rys; reflexivity) eq_refl) as [rys1 Hr1].
      eexists. simpl. rewrite Hy, Hr1; reflexivity.
Qed.

Lemma flat_rows_to_rel_sort_preserves :
  forall rows rrows sorted,
    flat_rows_to_rel rows = Some rrows ->
    F.result_row_sort FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys rows
      = Some sorted ->
    exists rsorted, flat_rows_to_rel sorted = Some rsorted.
Proof.
  intros rows; induction rows as [|x xs IH]; intros rrows sorted Hr Hs.
  - simpl in Hr, Hs; inversion Hr; inversion Hs; subst.
    eexists; reflexivity.
  - simpl in Hr, Hs.
    destruct (flat_row_to_rel x) as [rx|] eqn:Hrx; [|discriminate Hr].
    destruct (flat_rows_to_rel xs) as [rxs|] eqn:Hrxs; [|discriminate Hr].
    destruct (F.result_row_sort FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys xs)
      as [xs'|] eqn:Hsortxs; [|discriminate Hs].
    destruct (IH rxs xs' eq_refl eq_refl) as [rxs' Hfr'].
    eapply flat_rows_to_rel_insert_preserves; [eexists; exact Hrx|eexists; exact Hfr'|exact Hs].
Qed.

Lemma Q1Sort_rel_eq_flat :
  forall c rows rrows sorted,
    flat_rows_to_rel rows = Some rrows ->
    F.result_row_sort FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys rows
      = Some sorted ->
    result_row_sort (param_cutoff c) q1_order_keys rrows =
    flat_rows_to_rel sorted.
Proof.
  intros c rows; induction rows as [|x xs IH]; intros rrows sorted Hr Hs.
  - simpl in Hr, Hs. inversion Hr; inversion Hs; subst; reflexivity.
  - simpl in Hr, Hs.
    destruct (flat_row_to_rel x) as [rx|] eqn:Hrx; [|discriminate Hr].
    destruct (flat_rows_to_rel xs) as [rxs|] eqn:Hrxs; [|discriminate Hr].
    inversion Hr; subst rrows; clear Hr.
    destruct (F.result_row_sort FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys xs)
      as [xs'|] eqn:Hsortxs; [|discriminate Hs].
    (* destruct eqn rewrites IH premises to Some _ = Some _ *)
    pose proof (IH rxs xs' eq_refl eq_refl) as IHs.
    destruct (flat_rows_to_rel_sort_preserves xs rxs xs' Hrxs Hsortxs)
      as [rxs' Hfr'].
    rewrite Hfr' in IHs.
    simpl.
    rewrite IHs.
    pose proof (row_insert_rel_eq_flat c x xs' rx rxs' Hrx Hfr') as Hins.
    destruct (F.result_row_insert FlatLS.q1_ls_order_keys FlatLS.q1_ls_group_keys
                x xs') as [ys|] eqn:Hinsf; [|discriminate Hs].
    inversion Hs; subst sorted; clear Hs.
    destruct Hins as [rys [Hinr Hfrys]].
    rewrite Hinr. symmetry; exact Hfrys.
Qed.


(** *******************************************************************
    Compose: RelNode Eval_Spec ↔ flat Eval_Spec (convert)
    ******************************************************************* *)

Lemma eval_rel_scan_lineitem :
  forall p db,
    eval_rel (RelScan table_lineitem) p (db_of_aux db) =
    Some (RVRows (map row_of_aux db)).
Proof.
  intros p db. unfold db_of_aux, db_singleton, eval_rel, db_lookup.
  rewrite String.eqb_refl. reflexivity.
Qed.

Lemma eval_rel_filter_Q1 :
  forall c db,
    eval_rel (RelFilter (RelScan table_lineitem) q1_pred_shipdate)
      (param_cutoff c) (db_of_aux db) =
    Some (RVRows (map row_of_aux
                    (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db))).
Proof.
  intros c db.
  change (
    match eval_rel (RelScan table_lineitem) (param_cutoff c) (db_of_aux db) with
    | Some (RVRows rs) =>
        match filter_rows q1_pred_shipdate (param_cutoff c) rs with
        | Some rs' => Some (RVRows rs')
        | None => None
        end
    | _ => None
    end =
    Some (RVRows (map row_of_aux
                    (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db)))).
  rewrite eval_rel_scan_lineitem, filter_rel_Q1. reflexivity.
Qed.

Lemma flat_rows_to_rel_store_to_rows_StoreMirrors :
  forall st rs,
    StoreMirrors st rs ->
    exists rrows, flat_rows_to_rel (F.store_to_rows st) = Some rrows.
Proof.
  intros st rs Hm.
  pose proof (StoreMirrors_Forall_entries st rs Hm) as Hfa.
  clear Hm.
  induction st as [|[k a] rest IH].
  - exists []; reflexivity.
  - inversion Hfa as [|? ? Hx Hfa']; subst; clear Hfa.
    destruct Hx as [rf [ls [Hk [_ [_ [Ha Hpos]]]]]].
    subst k. rewrite Ha.
    destruct (IH Hfa') as [rrest Hrrest].
    eexists.
    change (F.store_to_rows (([rf; ls], F.acc_of_stats
              (Q1AuxCorrectAssembly.sum_contrib_matching rs (Q1AuxSpec.Pack rf ls))) :: rest))
      with (F.mkSpecResultRow [rf; ls] (F.acc_of_stats
              (Q1AuxCorrectAssembly.sum_contrib_matching rs (Q1AuxSpec.Pack rf ls)))
           :: F.store_to_rows rest).
    simpl. rewrite Hrrest. reflexivity.
Qed.

Lemma run_aggregate_Q1_of_store :
  forall c filtered st,
    F.fold_groups FlatLS.q1_aggs FlatLS.q1_ls_group_keys
      (FlatLS.params_of_cutoff c) filtered [] = Some st ->
    run_aggregate q1_aggs q1_group_exprs (param_cutoff c)
      (map row_of_aux filtered) =
    store_to_agg_bags q1_aggs (store_of_flat st).
Proof.
  intros c filtered st Hfold.
  unfold run_aggregate.
  rewrite (Q1GroupStore_rel_eq_flat_from_empty c filtered), Hfold.
  simpl. reflexivity.
Qed.

Lemma eval_rel_aggregate_Q1 :
  forall c db st,
    let filtered := filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db in
    F.fold_groups FlatLS.q1_aggs FlatLS.q1_ls_group_keys
      (FlatLS.params_of_cutoff c) filtered [] = Some st ->
    eval_rel
      (RelAggregate
        (RelFilter (RelScan table_lineitem) q1_pred_shipdate)
        q1_group_exprs q1_aggs)
      (param_cutoff c) (db_of_aux db) =
    match store_to_agg_bags q1_aggs (store_of_flat st) with
    | Some bags => Some (RVAgg bags)
    | None => None
    end.
Proof.
  intros c db st filtered Hfold.
  change (
    match eval_rel (RelFilter (RelScan table_lineitem) q1_pred_shipdate)
            (param_cutoff c) (db_of_aux db) with
    | Some (RVRows rs) =>
        match run_aggregate q1_aggs q1_group_exprs (param_cutoff c) rs with
        | Some bags => Some (RVAgg bags)
        | None => None
        end
    | _ => None
    end =
    match store_to_agg_bags q1_aggs (store_of_flat st) with
    | Some bags => Some (RVAgg bags)
    | None => None
    end).
  rewrite eval_rel_filter_Q1.
  fold filtered.
  rewrite (run_aggregate_Q1_of_store c filtered st Hfold).
  reflexivity.
Qed.

Lemma eval_rel_project_Q1 :
  forall c db st bags,
    let filtered := filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db in
    F.fold_groups FlatLS.q1_aggs FlatLS.q1_ls_group_keys
      (FlatLS.params_of_cutoff c) filtered [] = Some st ->
    store_to_agg_bags q1_aggs (store_of_flat st) = Some bags ->
    eval_rel
      (RelProject
        (RelAggregate
          (RelFilter (RelScan table_lineitem) q1_pred_shipdate)
          q1_group_exprs q1_aggs)
        q1_project_exprs)
      (param_cutoff c) (db_of_aux db) =
    match project_agg_rows q1_project_exprs (param_cutoff c) bags with
    | Some rs => Some (RVRows rs)
    | None => None
    end.
Proof.
  intros c db st bags filtered Hfold Hbags.
  change (
    match eval_rel
            (RelAggregate
              (RelFilter (RelScan table_lineitem) q1_pred_shipdate)
              q1_group_exprs q1_aggs)
            (param_cutoff c) (db_of_aux db) with
    | Some (RVAgg bags') =>
        match project_agg_rows q1_project_exprs (param_cutoff c) bags' with
        | Some rs => Some (RVRows rs)
        | None => None
        end
    | Some (RVRows rs) =>
        match project_rows q1_project_exprs (param_cutoff c) rs with
        | Some rs' => Some (RVRows rs')
        | None => None
        end
    | None => None
    end =
    match project_agg_rows q1_project_exprs (param_cutoff c) bags with
    | Some rs => Some (RVRows rs)
    | None => None
    end).
  rewrite (eval_rel_aggregate_Q1 c db st Hfold), Hbags.
  reflexivity.
Qed.

Theorem Eval_Spec_rel_eq_flat_convert :
  forall c db,
    Q1ByteKeys db ->
    Eval_Spec LogicalSpec_Q1_rel (param_cutoff c) (db_of_aux db) =
    match F.Eval_Spec FlatLS.LogicalSpec_Q1 (FlatLS.params_of_cutoff c) db with
    | Some rows => flat_rows_to_rel rows
    | None => None
    end.
Proof.
  intros c db Hbyte.
  rewrite F.Eval_Spec_Q1_unfold.
  set (filtered := filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db).
  rewrite F.filter_rows_Q1.
  replace (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db)
    with filtered by (subst filtered; reflexivity).
  cbn.
  assert (Hbyte_f : Q1ByteKeys filtered) by (apply Q1ByteKeys_filter; exact Hbyte).
  destruct (fold_groups_Q1_from_empty (FlatLS.params_of_cutoff c) filtered Hbyte_f)
    as [st [Hfold Hm]].
  rewrite Hfold.
  rewrite (filter_nonempty_StoreMirrors st filtered Hm).
  destruct (store_to_agg_bags q1_aggs (store_of_flat st)) as [bags|] eqn:Hbags.
  2: {
    rewrite (store_to_agg_bags_StoreMirrors st filtered Hm) in Hbags; discriminate.
  }
  pose proof (project_bags_StoreMirrors c st filtered bags Hm Hbags) as Hproj.
  destruct (flat_rows_to_rel_store_to_rows_StoreMirrors st filtered Hm)
    as [rrows Hfr].
  rewrite Hfr in Hproj.
  destruct (Q1Adapt_store_to_rows_ok st filtered Hm) as [outs [Hadapt _]].
  destruct (Q1Adapt_result_row_sort (F.store_to_rows st) outs Hadapt)
    as [sorted [Hsort _]].
  rewrite Hsort.
  pose proof (Q1Sort_rel_eq_flat c (F.store_to_rows st) rrows sorted Hfr Hsort)
    as Hsorteq.
  unfold Eval_Spec, LogicalSpec_Q1_rel.
  change (
    match
      match
        eval_rel
          (RelProject
            (RelAggregate
              (RelFilter (RelScan table_lineitem) q1_pred_shipdate)
              q1_group_exprs q1_aggs)
            q1_project_exprs)
          (param_cutoff c) (db_of_aux db)
      with
      | Some (RVRows rs2) =>
          match result_row_sort (param_cutoff c) q1_order_keys rs2 with
          | Some rs3 => Some (RVRows rs3)
          | None => None
          end
      | Some (RVAgg bags') =>
          match result_row_sort (param_cutoff c) q1_order_keys
                  (agg_bags_to_rows bags') with
          | Some rs3 => Some (RVRows rs3)
          | None => None
          end
      | None => None
      end
    with
    | Some (RVRows rs) => Some rs
    | Some (RVAgg bags') => Some (agg_bags_to_rows bags')
    | None => None
    end =
    flat_rows_to_rel sorted).
  rewrite (eval_rel_project_Q1 c db st bags Hfold Hbags).
  rewrite Hproj.
  rewrite Hsorteq.
  destruct (flat_rows_to_rel sorted); reflexivity.
Qed.

Theorem Eval_Spec_Q1_rows_rel_eq_AuxAnswerSpec :
  forall c db,
    Q1ByteKeys db ->
    Eval_Spec_Q1_rows_rel c db =
    Some (Q1AuxSpec.AuxAnswerSpec db c).
Proof.
  intros c db Hbyte.
  unfold Eval_Spec_Q1_rows_rel.
  rewrite (Eval_Spec_rel_eq_flat_convert c db Hbyte).
  pose proof (Eval_Spec_Q1_eq_AuxAnswerSpec c db Hbyte) as Hflat.
  unfold F.Eval_Spec_Q1_rows in Hflat.
  destruct (F.Eval_Spec FlatLS.LogicalSpec_Q1 (FlatLS.params_of_cutoff c) db)
    as [rows|] eqn:Heval; [|discriminate Hflat].
  destruct (F.Q1Adapt rows) as [outs|] eqn:Hadapt; [|discriminate Hflat].
  inversion Hflat; subst outs; clear Hflat.
  destruct (Q1Adapt_rel_of_flat rows _ Hadapt) as [rs [Hfr Hadaptr]].
  rewrite Hfr, Hadaptr. reflexivity.
Qed.

Theorem Eval_Spec_Q1_rows_rel_eq_flat :
  forall c db,
    Q1ByteKeys db ->
    Eval_Spec_Q1_rows_rel c db =
    F.Eval_Spec_Q1_rows FlatLS.LogicalSpec_Q1 (FlatLS.params_of_cutoff c) db.
Proof.
  intros c db Hbyte.
  rewrite (Eval_Spec_Q1_rows_rel_eq_AuxAnswerSpec c db Hbyte).
  rewrite (Eval_Spec_Q1_eq_AuxAnswerSpec c db Hbyte).
  reflexivity.
Qed.

Corollary Eval_Spec_Q1_rows_rel_eq_AuxAnswerSpec_pre :
  forall c db delta,
    Q1FormalSQLProof.Preconditions_Q1 db delta ->
    Eval_Spec_Q1_rows_rel c db =
    Some (Q1AuxSpec.AuxAnswerSpec db c).
Proof.
  intros c db delta Hpre.
  apply Eval_Spec_Q1_rows_rel_eq_AuxAnswerSpec, (Q1ByteKeys_RowOk db delta Hpre).
Qed.
