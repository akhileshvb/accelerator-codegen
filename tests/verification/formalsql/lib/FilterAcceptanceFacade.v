(* Generic FormalSQL filter-acceptance facade.

   Packages Logos [eval_filter_rows_acceptance_exact] so query proofs discharge
   per-row [scalar_expr_acceptance_exact_at] and obtain filter success + exact
   [List.filter] observation — without stepping [EFilterRows_Cons] in query files.

   Also bridges finite DATE ≤ TIMESTAMP observations to [Z.leb] under an
   in-range midnight carrier (TPC-H style cutoffs), using StringTemporalFacts. *)

From Stdlib Require Import List ZArith Lia.
From SQLFS Require Import
  OrderedSet FiniteSet FiniteBag FiniteCollection FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts Values ValueCore ValueTemporal GenericInstance FTuples.
From Logos.FormalSQL Require Import
  TNullSyntax QueryTNullSyntax GroupedFilterOutcomeFacts StringTemporalFacts.

Import Tuple.
Import ListNotations.
Open Scope Z_scope.

(** * Finite DATE whose midnight TIMESTAMP is also in range *)

Definition finite_date_timestamp_ok (d : Z) : Prop :=
  postgres_date_min <= d < postgres_date_end /\
  postgres_timestamp_min <= cast_date_to_timestamp d < postgres_timestamp_end /\
  date_is_neg_infinity_bool d = false /\
  date_is_pos_infinity_bool d = false.

Lemma micros_per_day_pos : 0 < micros_per_day.
Proof. unfold micros_per_day, micros_per_hour, micros_per_minute, micros_per_second. lia. Qed.

Lemma Z_compare_mul_pos_r :
  forall a b m, 0 < m -> Z.compare (a * m) (b * m) = Z.compare a b.
Proof.
  intros a b m Hm.
  destruct (Z.compare_spec a b) as [Hab|Hab|Hab];
    rewrite ?Hab; try rewrite Z.compare_refl;
    try (apply Z.compare_lt_iff; nia);
    try (apply Z.compare_gt_iff; nia);
    reflexivity.
Qed.

Lemma date_cmp_timestamp_internal_cast_finite :
  forall date cutoff,
    finite_date_timestamp_ok date ->
    finite_date_timestamp_ok cutoff ->
    date_cmp_timestamp_internal date (cast_date_to_timestamp cutoff) =
    Z.compare date cutoff.
Proof.
  intros date cutoff
    [Hdate_range [Hdate_ts [Hdate_ninf Hdate_pinf]]]
    [Hcut_range [Hcut_ts [Hcut_ninf Hcut_pinf]]].
  unfold date_cmp_timestamp_internal.
  rewrite Hdate_ninf, Hdate_pinf.
  set (midnight := cast_date_to_timestamp date).
  assert (Hlo : (midnight <? postgres_timestamp_min) = false).
  { apply Z.ltb_ge. unfold midnight in *. lia. }
  assert (Hhi : (midnight <? postgres_timestamp_end) = true).
  { apply Z.ltb_lt. unfold midnight in *. lia. }
  rewrite Hlo, Hhi.
  unfold midnight, cast_date_to_timestamp.
  apply Z_compare_mul_pos_r, micros_per_day_pos.
Qed.

Lemma bool3_is_true_true3 : Bool.is_true Bool3 true3 = true.
Proof. reflexivity. Qed.

Lemma bool3_is_true_false3 : Bool.is_true Bool3 false3 = false.
Proof. reflexivity. Qed.

Lemma date_lte_cast_cutoff_leb :
  forall date cutoff,
    finite_date_timestamp_ok date ->
    finite_date_timestamp_ok cutoff ->
    Bool.is_true Bool3
      (NullValues.interp_predicate PredicateDateLteTimestamp
        [NullValues.Value_date (Some date);
         NullValues.Value_timestamp (Some (cast_date_to_timestamp cutoff))]) =
    Z.leb date cutoff.
Proof.
  intros date cutoff Hdate Hcut.
  pose proof (date_cmp_timestamp_internal_cast_finite date cutoff Hdate Hcut)
    as Hcmp.
  destruct (Z.leb date cutoff) eqn:Hleb.
  - apply Z.leb_le in Hleb.
    assert (Hne : date_cmp_timestamp_internal date
                    (cast_date_to_timestamp cutoff) <> Gt).
    { rewrite Hcmp. intro Hg.
      apply Z.compare_gt_iff in Hg. lia. }
    pose proof (proj2 (interp_date_lte_timestamp_true_iff date
                         (cast_date_to_timestamp cutoff)) Hne) as Htrue.
    rewrite Htrue. exact bool3_is_true_true3.
  - apply Z.leb_gt in Hleb.
    assert (Hg : date_cmp_timestamp_internal date
                   (cast_date_to_timestamp cutoff) = Gt).
    { rewrite Hcmp. apply Z.compare_gt_iff. exact Hleb. }
    pose proof (proj2 (interp_date_lte_timestamp_false_iff date
                         (cast_date_to_timestamp cutoff)) Hg) as Hfalse.
    rewrite Hfalse. exact bool3_is_true_false3.
Qed.

Lemma date_lt_cast_cutoff_ltb :
  forall date cutoff,
    finite_date_timestamp_ok date ->
    finite_date_timestamp_ok cutoff ->
    Bool.is_true Bool3
      (NullValues.interp_predicate PredicateDateLtTimestamp
        [NullValues.Value_date (Some date);
         NullValues.Value_timestamp (Some (cast_date_to_timestamp cutoff))]) =
    Z.ltb date cutoff.
Proof.
  intros date cutoff Hdate Hcut.
  pose proof (date_cmp_timestamp_internal_cast_finite date cutoff Hdate Hcut)
    as Hcmp.
  destruct (Z.ltb date cutoff) eqn:Hltb.
  - apply Z.ltb_lt in Hltb.
    assert (Hltc : date_cmp_timestamp_internal date
                     (cast_date_to_timestamp cutoff) = Lt).
    { rewrite Hcmp. apply Z.compare_lt_iff. exact Hltb. }
    pose proof (proj2 (interp_date_lt_timestamp_true_iff date
                         (cast_date_to_timestamp cutoff)) Hltc) as Htrue.
    rewrite Htrue. exact bool3_is_true_true3.
  - assert (Hnlt : ~ (date < cutoff)%Z).
    { intro Hlt. apply Z.ltb_lt in Hlt. rewrite Hlt in Hltb. discriminate. }
    assert (Hne : date_cmp_timestamp_internal date
                    (cast_date_to_timestamp cutoff) <> Lt).
    { rewrite Hcmp. intro Hltcmp.
      apply Z.compare_lt_iff in Hltcmp. apply Hnlt. exact Hltcmp. }
    pose proof (proj2 (interp_date_lt_timestamp_false_iff date
                         (cast_date_to_timestamp cutoff)) Hne) as Hfalse.
    rewrite Hfalse. exact bool3_is_true_false3.
Qed.

Lemma timestamp_infinity_false_of_in_range :
  forall ts,
    postgres_timestamp_min <= ts < postgres_timestamp_end ->
    timestamp_is_infinity_bool ts = false.
Proof.
  intros ts Hts.
  unfold timestamp_is_infinity_bool, timestamp_is_neg_infinity_bool,
    timestamp_is_pos_infinity_bool,
    postgres_timestamp_neg_infinity, postgres_timestamp_pos_infinity.
  apply Bool.orb_false_intro; apply Z.eqb_neq; lia.
Qed.

Lemma timestamp_add_days_cast_date :
  forall base delta,
    finite_date_timestamp_ok base ->
    finite_date_timestamp_ok (base + delta) ->
    timestamp_add_days_checked (cast_date_to_timestamp base) delta =
      Some (cast_date_to_timestamp (base + delta)).
Proof.
  intros base delta [_ [Hbase_ts _]] [_ [Hsum_ts _]].
  unfold timestamp_add_days_checked.
  rewrite (timestamp_infinity_false_of_in_range _ Hbase_ts).
  unfold timestamp_checked, timestamp_add_days, timestamp_add_microseconds,
    cast_date_to_timestamp.
  rewrite <- Z.mul_add_distr_r.
  apply timestamp_in_range_bool_true_iff in Hsum_ts.
  unfold cast_date_to_timestamp in Hsum_ts.
  rewrite Hsum_ts.
  reflexivity.
Qed.

Lemma timestamp_add_neg_days_cast_cutoff :
  forall base delta,
    finite_date_timestamp_ok base ->
    finite_date_timestamp_ok (base - delta) ->
    timestamp_add_days_checked (cast_date_to_timestamp base) (-delta) =
      Some (cast_date_to_timestamp (base - delta)).
Proof.
  intros base delta Hbase Hcut.
  replace (base - delta) with (base + (-delta)) in Hcut |- * by lia.
  apply timestamp_add_days_cast_date; assumption.
Qed.

Lemma date_lte_timestamp_add_cutoff_leb :
  forall date base delta,
    finite_date_timestamp_ok date ->
    finite_date_timestamp_ok base ->
    finite_date_timestamp_ok (base - delta) ->
    Bool.is_true Bool3
      (NullValues.interp_predicate PredicateDateLteTimestamp
        [NullValues.Value_date (Some date);
         NullValues.Value_timestamp
           (timestamp_add_days_checked
              (cast_date_to_timestamp base) (-delta))]) =
    Z.leb date (base - delta).
Proof.
  intros date base delta Hdate Hbase Hcut.
  rewrite (timestamp_add_neg_days_cast_cutoff base delta Hbase Hcut).
  apply date_lte_cast_cutoff_leb; assumption.
Qed.

(** * Filter row-list bridge (Logos [eval_filter_rows_acceptance_exact]) *)

Section FilterRowsAcceptance.

Context {T : Tuple.Rcd} {relname : Type}.

Variable basesort : relname -> Fset.set (A T).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple T)).
Variable unknown : Bool.b (B T).
Variable symbol_runtime_error :
  scalar_operator T -> list (option sql_runtime_error * value T) ->
  option sql_runtime_error.
Variable aggregate_runtime_error :
  aggregate T -> list (option sql_runtime_error * value T) ->
  option sql_runtime_error.
Variable value_is_null : value T -> bool.
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Local Abbreviation eval_filter :=
  (@eval_filter_rows_outcome T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

Local Abbreviation acceptance_at :=
  (@scalar_expr_acceptance_exact_at T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

(** Per-row acceptance ⇒ filter succeeds with exactly [List.filter keep rows]. *)
Theorem filter_rows_success_of_acceptance :
  forall env formula rows keep,
    (forall row,
      In row rows ->
      acceptance_at (env_t T env row) formula (keep row)) ->
    eval_filter env formula rows (SqlSuccess (List.filter keep rows)).
Proof.
  intros env formula rows keep Hacc.
  apply (proj2
    (@eval_filter_rows_acceptance_exact T relname basesort instance unknown
      symbol_runtime_error aggregate_runtime_error value_is_null
      boolean_schedule env formula rows keep Hacc
      (SqlSuccess (List.filter keep rows)))).
  reflexivity.
Qed.

Theorem filter_rows_outcome_iff_filter :
  forall env formula rows keep outcome,
    (forall row,
      In row rows ->
      acceptance_at (env_t T env row) formula (keep row)) ->
    eval_filter env formula rows outcome <->
    outcome = SqlSuccess (List.filter keep rows).
Proof.
  intros; eapply eval_filter_rows_acceptance_exact; eassumption.
Qed.

(** Predicate-form acceptance from exact argument values
    (Logos [scalar_expr_pred_acceptance_exact_safe]). *)
Theorem pred_acceptance_of_value_list :
  forall env predicate arguments values,
    @scalar_value_list_exact_at T relname basesort instance unknown
      symbol_runtime_error aggregate_runtime_error value_is_null
      boolean_schedule env arguments values ->
    acceptance_at env (SExpr_Pred predicate arguments)
      (Bool.is_true (B T) (interp_predicate T predicate values)).
Proof.
  intros; eapply scalar_expr_pred_acceptance_exact_safe; eassumption.
Qed.

End FilterRowsAcceptance.

(** * TNull specialization *)

Section FilterRowsAcceptance_TNull.

Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Definition tnull_value_is_null (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

Theorem tnull_filter_rows_success_of_acceptance :
  forall env formula rows keep,
    (forall row,
      In row rows ->
      @scalar_expr_acceptance_exact_at TNull relname basesort instance unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        tnull_value_is_null boolean_schedule
        (env_t TNull env row) formula (keep row)) ->
    @eval_filter_rows_outcome TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      env formula rows (SqlSuccess (List.filter keep rows)).
Proof.
  intros; eapply (@filter_rows_success_of_acceptance TNull relname
    basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule); eassumption.
Qed.

End FilterRowsAcceptance_TNull.
