(* Q1 Stage 2 final theorem, Q21 recipe. *)
From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueTemporal
  ValueNumericTypmod SchemaConstraints ValueString
  SqlOutcome SqlQuerySyntax SqlQuerySemantics SqlQueryFacts SqlErrorSemantics
  SqlOrder SqlListFacts FiniteBag FiniteCollection FiniteSet FTuples Bool3
  OrderedSet Env Projection Interp ListSort.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax
  FormalSQL.GroupedFilterOutcomeFacts FormalSQL.AggregateRuntimeFacts
  FormalSQL.QueryCardinality FormalSQL.NumericRegroupFacts
  FormalSQL.GroupingRewriteFacts FormalSQL.ProofAgentFacade.
From FormalSQLLib Require Import
  QuerySpineFacade FilterAcceptanceFacade ProjectExactFacade
  OrderRowsFacade GroupBagExactFacade GroupsExactFacade
  AggregateLeafExactFacade TemporalPredAcceptanceFacade
  NumericCallExactFacade NumericScaledFits NumericPackagingFacade
  KeyedSortFacade ListNoDupFacade NumericFoldFacade SortedGroupResultFacade
  PossibleOutcomeDeterminism.
From JetStreamFS Require Import Q1AuxSpec.
From Stdlib Require Import String List ZArith Lia Permutation Ascii
  Sorting.Sorted Bool NArith.
Import ListNotations.
Import Tuple.
Open Scope Z_scope.

Require Import Q1Catalog Q1FormalSQLProof Q1GroupingCorrespondence
  Q1AuxCorrectAssembly Q1Obligations Q1FormalSQLSpine Q1NumericEmbed
  Q1GroupsCorrespond Q1TCBAuxCorrect Q1NormalizeSound.
From FormalSQLResultSpec Require Import
  QueryResultSpec NormalizeQuery EvalSpec LogicalSpec NormalizeCompositional
  EvalLogicalSpec NormalizeSoundGeneric.
Require Import Q1EvalMigration Q1ResultSpecNormalize Q1LogicalNormalize Q1Adapt
  Q1RelNodeBridge.
Require Import Q1AllOutcomes Q1Schema Q1LogicalSpecTree.
From FormalSQLLib Require Import NumericAggExact.
From FormalSQLLogical Require Import SqlTypes Eval.

(* Q1FinalTheorem -- Q1 Stage 2 in the same shape as Q21 (Q21AllOutcomes):

     Eval_Spec(LogicalSpec_Q1_rel, cutoff, db) = Some rows  ->
     every FormalSQL outcome of the catalog query is SqlSuccess (Embed(rows)).

   [Embed_Q1_Result] embeds the RelNode rows directly: sums as scaled decimals,
   each AVG from its own VAvgExact(sum, count) as PostgreSQL avg(numeric)
   ([NumericAggExact.avg_value]).  No Q1Adapt / AvgRound / AuxAnswerSpec in
   the statement, and no numeric premise: [NumericEmbedFaithful] is proved
   ([Q1NumericEmbed.NumericEmbedFaithful_holds]).  The group-model /
   AuxAnswerSpec lemmas remain an internal, fully proved step of the proof. *)

Module F := FormalSQLResultSpec.EvalLogicalSpec.
Module FlatLS := FormalSQLResultSpec.LogicalSpec.

(** ** Shape of the RelNode rows *)

Definition FlatShape (row : F.SpecResultRow) : Prop :=
  exists rf ls s, 0 < Q1AuxSpec.count s /\ row = F.mkSpecResultRow [rf; ls] (F.acc_of_stats s).

Definition RelShape (r : list SqlValue) : Prop :=
  exists rf ls sq sb sdp sc sd cnt,
    0 < cnt /\
    r = [VInt rf; VInt ls; VInt sq; VInt sb; VInt sdp; VInt sc;
         VAvgExact sq cnt; VAvgExact sb cnt; VAvgExact sd cnt; VInt cnt].

Lemma result_row_insert_Forall :
  forall (P : F.SpecResultRow -> Prop) oks gks x l l',
    P x -> Forall P l -> F.result_row_insert oks gks x l = Some l' -> Forall P l'.
Proof.
  intros P oks gks x l; induction l as [|y ys IH]; intros l' Hx Hl H; cbn in H.
  - injection H as <-. constructor; [exact Hx|constructor].
  - inversion Hl as [|? ? Hy Hys]; subst.
    destruct (F.result_row_le oks gks x y) as [[|]|]; [| |discriminate].
    + injection H as <-. constructor; [exact Hx|constructor; assumption].
    + destruct (F.result_row_insert oks gks x ys) as [ys'|] eqn:E; [|discriminate].
      injection H as <-. constructor; [exact Hy|exact (IH ys' Hx Hys eq_refl)].
Qed.

Lemma result_row_sort_Forall :
  forall (P : F.SpecResultRow -> Prop) oks gks l l',
    Forall P l -> F.result_row_sort oks gks l = Some l' -> Forall P l'.
Proof.
  intros P oks gks l; induction l as [|x xs IH]; intros l' Hl H; cbn in H.
  - injection H as <-. constructor.
  - inversion Hl as [|? ? Hx Hxs]; subst.
    destruct (F.result_row_sort oks gks xs) as [xs'|] eqn:E; [|discriminate].
    exact (result_row_insert_Forall P oks gks x xs' l' Hx (IH xs' Hxs eq_refl) H).
Qed.

Lemma flat_eval_shape :
  forall c db flat,
    Q1ByteKeys db ->
    F.Eval_Spec FlatLS.LogicalSpec_Q1 (FlatLS.params_of_cutoff c) db = Some flat ->
    Forall FlatShape flat.
Proof.
  intros c db flat Hbyte Hev.
  rewrite F.Eval_Spec_Q1_unfold in Hev.
  rewrite F.filter_rows_Q1 in Hev.
  cbn in Hev.
  assert (Hbyte_f : Q1ByteKeys (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db))
    by (apply Q1ByteKeys_filter; exact Hbyte).
  destruct (fold_groups_Q1_from_empty (FlatLS.params_of_cutoff c) _ Hbyte_f) as [st [Hfold Hm]].
  rewrite Hfold in Hev.
  rewrite (filter_nonempty_StoreMirrors st _ Hm) in Hev.
  eapply result_row_sort_Forall; [|exact Hev].
  pose proof (StoreMirrors_Forall_entries st _ Hm) as HF.
  unfold F.store_to_rows. apply Forall_map.
  eapply Forall_impl; [|exact HF].
  intros [k a] [rf [ls [Hk [_ [_ [Ha Hpos]]]]]].
  exists rf, ls. eexists. split; [exact Hpos|]. subst k a. reflexivity.
Qed.

Lemma flat_rows_to_rel_shape :
  forall flat rows,
    Forall FlatShape flat -> flat_rows_to_rel flat = Some rows -> Forall RelShape rows.
Proof.
  intros flat; induction flat as [|x xs IH]; intros rows Hf H; cbn in H.
  - injection H as <-. constructor.
  - inversion Hf as [|? ? Hx Hxs]; subst.
    destruct Hx as [rf [ls [s [Hpos ->]]]].
    rewrite flat_row_to_rel_acc_of_stats in H.
    destruct (flat_rows_to_rel xs) as [rs|] eqn:E; [|discriminate].
    injection H as <-. constructor; [|exact (IH rs Hxs eq_refl)].
    exists rf, ls, (Q1AuxSpec.sum_qty s), (Q1AuxSpec.sum_base_price s),
      (Q1AuxSpec.sum_disc_price s), (Q1AuxSpec.sum_charge s),
      (Q1AuxSpec.sum_discount s), (Q1AuxSpec.count s).
    split; [exact Hpos|reflexivity].
Qed.

Theorem Eval_Spec_Q1_rel_shape :
  forall c db rows,
    Q1ByteKeys db ->
    Eval_Spec LogicalSpec_Q1_rel (param_cutoff c) (db_of_aux db) = Some rows ->
    Forall RelShape rows.
Proof.
  intros c db rows Hbyte H.
  rewrite (Eval_Spec_rel_eq_flat_convert c db Hbyte) in H.
  destruct (F.Eval_Spec FlatLS.LogicalSpec_Q1 (FlatLS.params_of_cutoff c) db) as [flat|] eqn:E;
    [|discriminate].
  exact (flat_rows_to_rel_shape flat rows (flat_eval_shape c db flat Hbyte E) H).
Qed.

(** ** Embedding of the RelNode rows (Q21-style) *)

Definition Embed_Q1_Row (r : list SqlValue) : option (tuple TNull) :=
  match r with
  | [VInt rf; VInt ls; VInt sq; VInt sb; VInt sdp; VInt sc;
     VAvgExact aq_s aq_n; VAvgExact ap_s ap_n; VAvgExact ad_s ad_n; VInt cnt] =>
      Some (project_row Queries.scalar_select_list_0
        [ embed_flag_char rf; embed_flag_char ls
        ; embed_decimal2 sq; embed_decimal2 sb
        ; embed_decimal_at sdp 4; embed_decimal_at sc 6
        ; avg_value aq_s 2 aq_n; avg_value ap_s 2 ap_n; avg_value ad_s 2 ad_n
        ; NullValues.value_int64_checked cnt ])
  | _ => None
  end.

Fixpoint Embed_Q1_Result (rows : list (list SqlValue)) : option (list (tuple TNull)) :=
  match rows with
  | [] => Some []
  | r :: rest =>
      match Embed_Q1_Row r, Embed_Q1_Result rest with
      | Some t, Some ts => Some (t :: ts)
      | _, _ => None
      end
  end.

Lemma Embed_Q1_Result_adapt :
  forall rows outs,
    Forall RelShape rows ->
    Q1Adapt rows = Some outs ->
    Embed_Q1_Result rows = Some (EmbedResult_Q1 outs).
Proof.
  intros rows; induction rows as [|r rest IH]; intros outs Hs H; cbn in H.
  - injection H as <-. reflexivity.
  - inversion Hs as [|? ? Hr Hrest]; subst.
    destruct Hr as [rf [ls [sq [sb [sdp [sc [sd [cnt [Hpos ->]]]]]]]]].
    cbn [Q1AdaptRow] in H.
    replace (Z.leb cnt 0) with false in H by (symmetry; apply Z.leb_gt; lia).
    destruct (Q1Adapt rest) as [os|] eqn:E; [|discriminate].
    injection H as <-.
    cbn [Embed_Q1_Result Embed_Q1_Row].
    rewrite (IH os Hrest eq_refl).
    reflexivity.
Qed.

(** ** Top-level theorems (no numeric premise) *)

Theorem NormalizeSound_Q1_rel_all_outcomes :
  forall db delta rows,
    Preconditions_Q1 db delta ->
    Q1NumericSafe db ->
    Eval_Spec LogicalSpec_Q1_rel (param_cutoff (cutoff_Q1 delta)) (db_of_aux db) = Some rows ->
    exists outs,
      Embed_Q1_Result rows = Some outs /\
      FormalSQL_exec (EmbedDB_Q1 db) delta outs /\
      (forall sched o,
         @eval_query_expr_outcome TNull relname
           (@_basesort TNull (EmbedDB_Q1 db)) (@_instance TNull (EmbedDB_Q1 db))
           unknown3 NullValues.interp_scalar_operator_runtime_error
           NullValues.interp_aggregate_runtime_error value_is_null sched nil
           (qhat_Q1 delta) o ->
         o = SqlSuccess outs).
Proof.
  intros db delta rows Hpre Hsafe Hrows.
  destruct (NormalizeSound_Q1_all_outcomes db delta Hpre Hsafe NumericEmbedFaithful_holds)
    as [_ [Hexec Hall]].
  exists (EmbedResult_Q1 (Eval_Spec_Q1_outs (cutoff_Q1 delta) db)).
  split; [|split; [exact Hexec|exact Hall]].
  pose proof (Eval_Spec_Q1_outs_Some (cutoff_Q1 delta) db delta Hpre) as Hsome.
  unfold Eval_Spec_Q1_rows_rel in Hsome. rewrite Hrows in Hsome.
  apply Embed_Q1_Result_adapt; [|exact Hsome].
  exact (Eval_Spec_Q1_rel_shape _ db rows (Q1ByteKeys_RowOk db delta Hpre) Hrows).
Qed.

Corollary NormalizeSound_Q1_rel_catalog_all_outcomes :
  forall db rows,
    Preconditions_Q1 db q1_catalog_delta ->
    Q1NumericSafe db ->
    Eval_Spec LogicalSpec_Q1_rel (param_cutoff (cutoff_Q1 q1_catalog_delta)) (db_of_aux db) =
      Some rows ->
    exists outs,
      Embed_Q1_Result rows = Some outs /\
      FormalSQL_exec (EmbedDB_Q1 db) q1_catalog_delta outs /\
      (forall sched o,
         @eval_query_expr_outcome TNull relname
           (@_basesort TNull (EmbedDB_Q1 db)) (@_instance TNull (EmbedDB_Q1 db))
           unknown3 NullValues.interp_scalar_operator_runtime_error
           NullValues.interp_aggregate_runtime_error value_is_null sched nil
           Queries.source_query_expr o ->
         o = SqlSuccess outs).
Proof.
  intros db rows Hpre Hsafe Hrows.
  destruct (NormalizeSound_Q1_rel_all_outcomes db q1_catalog_delta rows Hpre Hsafe Hrows)
    as [outs [He [Hx _]]].
  destruct (NormalizeSound_Q1_catalog_all_outcomes db Hpre Hsafe NumericEmbedFaithful_holds)
    as [_ Hall].
  exists outs. split; [exact He|split; [exact Hx|]].
  intros sched o Ho.
  rewrite (Hall sched o Ho). f_equal.
  (* both are the embedding of the same RelNode rows *)
  pose proof (Eval_Spec_Q1_outs_Some (cutoff_Q1 q1_catalog_delta) db q1_catalog_delta Hpre) as Hsome.
  unfold Eval_Spec_Q1_rows_rel in Hsome. rewrite Hrows in Hsome.
  pose proof (Embed_Q1_Result_adapt rows _
    (Eval_Spec_Q1_rel_shape _ db rows (Q1ByteKeys_RowOk db q1_catalog_delta Hpre) Hrows) Hsome)
    as Hemb.
  rewrite Hemb in He. injection He as <-. reflexivity.
Qed.
