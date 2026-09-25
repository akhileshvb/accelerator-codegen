(* Reusable NUMERIC overflow packaging (SUM digit-budget + AVG finalize).

   Query proofs discharge per-bucket [sum_numeric_runtime_error] /
   [avg_numeric_fixed_runtime_error] via cardinality + coefficient bounds.
   Shape-based SUM covers AttrNumeric carriers that are already
   [numeric_of_scaled] (e.g. projected $f4/$f5).  DECIMAL SUM/AVG use Logos
   [fixed_decimal_*_of_cardinality] plus [decimal15_2_avg_div_runtime_safe]. *)

From Stdlib Require Import List ZArith Lia QArith Qcanon Morphisms Setoid.
From SQLFS Require Import Values ValueCore ValueNumeric ValueNumericTypmod ValueInteger.
From Logos.FormalSQL Require Import NumericFacts NumericDerivedFacts NumericRegroupFacts.
From FormalSQLLib Require Import NumericScaledFits.

Import ListNotations.
Import NullValues.
Local Open Scope Z_scope.

Lemma fixed_decimal_shapes_numeric_values :
  forall precision scale observations,
    Forall (fixed_decimal_value_shape precision scale) observations ->
    Forall (fixed_decimal_numeric_shape precision scale)
      (numeric_values observations).
Proof.
  intros precision scale observations Hshapes.
  induction Hshapes as [|obs obs_rest Hhead Htail IH]; [constructor|].
  destruct obs; cbn in Hhead |- *; try contradiction; try exact IH.
  destruct o as [number|]; cbn; [|exact IH].
  constructor; [|exact IH].
  exact Hhead.
Qed.

Theorem scaled_numeric_sum_runtime_safe_of_cardinality :
  forall precision scale observations max_count accumulator_bound,
    0 <= precision ->
    0 <= max_count ->
    Z.of_nat (List.length observations) <= max_count ->
    max_count * Z.pow 10 precision <= accumulator_bound ->
    (forall coefficient,
      Z.abs coefficient <= accumulator_bound ->
      numeric_runtime_fits_bool
        (numeric_of_scaled coefficient scale) = true) ->
    Forall (fixed_decimal_value_shape precision scale) observations ->
    sum_numeric_runtime_error observations = None.
Proof.
  intros precision scale observations max_count accumulator_bound
    Hprecision Hcount_nonnegative Hlength Haccumulator Hfits Hshapes.
  pose proof (@fixed_decimal_shapes_are_numeric
    precision scale observations Hshapes) as Htyped.
  pose proof (@fixed_decimal_shapes_numeric_values
    precision scale observations Hshapes) as Hnumbers.
  pose proof (@fixed_decimal_sum_fold_shape
    precision scale (numeric_values observations) None 0
    Hprecision Hnumbers ltac:(lia) ltac:(left; reflexivity)) as Hfold.
  unfold sum_numeric_runtime_error; rewrite Htyped.
  unfold numeric_sum_state_runtime_error.
  rewrite numeric_sum_fold_from_initial.
  destruct Hfold as
    [Hnone | [Hnan | [coefficient [Hsum Hcoefficient]]]].
  - now rewrite Hnone.
  - rewrite Hnan.
    apply (proj2 (numeric_result_runtime_error_none_iff NumericNaN)).
    reflexivity.
  - rewrite Hsum.
    apply (proj2 (numeric_result_runtime_error_none_iff _)).
    apply Hfits.
    eapply Z.le_trans; [exact Hcoefficient|].
    replace
      (0 + Z.of_nat (List.length (numeric_values observations)) *
        Z.pow 10 precision)
      with
      (Z.of_nat (List.length (numeric_values observations)) *
        Z.pow 10 precision) by ring.
    eapply Z.le_trans; [|exact Haccumulator].
    apply Z.mul_le_mono_nonneg_r; [apply Z.pow_nonneg; lia|].
    eapply Z.le_trans; [|exact Hlength].
    apply Nat2Z.inj_le.
    apply numeric_values_length_le.
Qed.

Lemma fixed_decimal_value_shape_of_scaled :
  forall precision scale c,
    0 <= scale ->
    Z.abs c < Z.pow 10 precision ->
    fixed_decimal_value_shape precision scale
      (Value_numeric (Some (numeric_of_scaled c scale))).
Proof.
  intros precision scale c Hs Hc.
  set (n := numeric_of_scaled c scale).
  pose proof (numeric_finite_rounded_coeff_of_scaled c scale Hs) as Hr.
  change (match n with
          | NumericFinite q => numeric_finite_rounded_coeff q scale = c
          | _ => False
          end) in Hr.
  unfold fixed_decimal_value_shape.
  subst n.
  destruct (numeric_of_scaled c scale) as [|q| |] eqn:Hn; try contradiction.
  exists c.
  split.
  - rewrite <- Hn. reflexivity.
  - split; [exact Hr|exact Hc].
Qed.

Lemma Forall_fixed_decimal_value_shape_of_scaled :
  forall precision scale coeffs,
    0 <= scale ->
    Forall (fun c => Z.abs c < Z.pow 10 precision) coeffs ->
    Forall (fixed_decimal_value_shape precision scale)
      (map (fun c => Value_numeric (Some (numeric_of_scaled c scale))) coeffs).
Proof.
  intros precision scale coeffs Hs Hbound.
  induction Hbound as [|c cs Hc Hcs IH]; [constructor|].
  constructor; [|exact IH].
  apply fixed_decimal_value_shape_of_scaled; assumption.
Qed.

Lemma numeric_typmod_valid_15_2 :
  numeric_typmod_valid_bool 15 2 = true.
Proof. vm_compute; reflexivity. Qed.

Lemma numeric_is_nan_of_scaled_false :
  forall c s, numeric_is_nan (numeric_of_scaled c s) = false.
Proof.
  intros c s.
  unfold numeric_of_scaled, numeric_is_nan.
  destruct (0 <=? s); reflexivity.
Qed.

Lemma numeric_is_nan_of_Z_false :
  forall n, numeric_is_nan (numeric_of_Z n) = false.
Proof. intros; reflexivity. Qed.

(** * AVG finalize digit-budget (DECIMAL(15,2) / positive count) *)

Lemma numeric_round_quot_abs_le :
  forall numerator denominator,
    0 < denominator ->
    Z.abs (numeric_round_quot numerator denominator) <= Z.abs numerator + 1.
Proof.
  intros num den Hden.
  assert (Hden_ne : den <> 0) by lia.
  unfold numeric_round_quot.
  set (q := Z.quot num den).
  set (r := Z.rem num den).
  assert (Hq : Z.abs q <= Z.abs num).
  { destruct (Z.eq_dec num 0) as [Hz|Hnz].
    { subst num. unfold q. rewrite Z.quot_0_l by exact Hden_ne. lia. }
    destruct (Z.le_gt_cases 0 num) as [Hn0|Hn0].
    - unfold q.
      rewrite Z.quot_div_nonneg by lia.
      assert (Hdiv : 0 <= num / den) by (apply Z.div_pos; lia).
      rewrite (Z.abs_eq (num / den) Hdiv), (Z.abs_eq num Hn0).
      apply Z.div_le_upper_bound; [exact Hden|nia].
    - unfold q.
      rewrite <- (Z.opp_involutive (Z.quot num den)).
      rewrite <- Z.quot_opp_l by exact Hden_ne.
      rewrite Z.abs_opp.
      assert (Hnpos : 0 <= - num) by lia.
      rewrite Z.quot_div_nonneg by lia.
      assert (Hdiv : 0 <= (- num) / den) by (apply Z.div_pos; lia).
      rewrite (Z.abs_eq ((- num) / den) Hdiv), (Z.abs_neq num) by lia.
      apply Z.div_le_upper_bound; [exact Hden|nia].
  }
  destruct ((2 * Z.abs r >=? Z.abs den)%Z) eqn:Hge.
  - destruct ((num * den <? 0)%Z) eqn:Hsign; lia.
  - lia.
Qed.

Lemma Qred_num_abs_le_frac :
  forall a (b : positive),
    Z.abs (Qnum (Qred (a # b))) <= Z.abs a.
Proof.
  intros a b.
  unfold Qred; cbn [Qnum Qden Qred].
  destruct (Z.ggcd a (Z.pos b)) as [g [r1 r2]] eqn:Hgg.
  cbn [fst snd Qnum].
  pose proof (Z.ggcd_correct_divisors a (Z.pos b)) as Hdiv.
  rewrite Hgg in Hdiv; cbn in Hdiv.
  destruct Hdiv as [Ha _].
  pose proof (Z.ggcd_gcd a (Z.pos b)) as Hgcd.
  rewrite Hgg in Hgcd; cbn in Hgcd.
  assert (Hg0 : 0 <= g) by (rewrite Hgcd; apply Z.gcd_nonneg).
  rewrite Ha, Z.abs_mul, (Z.abs_eq g Hg0).
  destruct (Z.eq_dec r1 0) as [Hr0|Hr0]; [lia|].
  assert (Hgone : 1 <= g).
  { rewrite Hgcd.
    destruct (Z.eq_dec (Z.gcd a (Z.pos b)) 0) as [Hz|Hnz]; [|lia].
    apply (proj1 (Z.gcd_eq_0 a (Z.pos b))) in Hz; lia. }
  rewrite Z.mul_comm.
  rewrite <- (Z.mul_1_r (Z.abs r1)) at 1.
  apply Z.mul_le_mono_nonneg_l; [apply Z.abs_nonneg|exact Hgone].
Qed.

Lemma inject_Z_div_Qeq :
  forall a b, 0 < b ->
    (inject_Z a / inject_Z b)%Q == (a # Z.to_pos b)%Q.
Proof.
  intros a b Hb.
  unfold Qdiv, inject_Z, Qmult, Qinv, Qeq.
  cbn [Qnum Qden].
  destruct b as [|p|p]; try lia.
  cbn [Qnum Qden Z.to_pos].
  lia.
Qed.

Lemma this_Q2Qc_num_abs_le_inject_div :
  forall a b,
    0 < b ->
    Z.abs (Qnum (this (Q2Qc (inject_Z a / inject_Z b)))) <= Z.abs a.
Proof.
  intros a b Hb.
  change (Z.abs (Qnum (Qred (inject_Z a / inject_Z b))) <= Z.abs a).
  assert (Heq : (inject_Z a / inject_Z b)%Q == (a # Z.to_pos b)%Q)
    by (apply inject_Z_div_Qeq; exact Hb).
  rewrite (Qred_complete _ _ Heq).
  apply Qred_num_abs_le_frac.
Qed.

Lemma Qc_is_reduced : forall (q : Qc), Qred (this q) = this q.
Proof. intros [q Hq]; exact Hq. Qed.

Lemma this_Qcmult_Qeq : forall a b,
  this (Qcmult a b) == (this a * this b)%Q.
Proof. intros; apply Qred_correct. Qed.

Lemma this_Qcinv_Qeq : forall b,
  b <> 0%Qc ->
  this (Qcinv b) == (/ this b)%Q.
Proof. intros b Hb; unfold Qcinv; apply Qred_correct. Qed.

Lemma this_Qcdiv_Qeq : forall a b,
  b <> 0%Qc ->
  this (Qcdiv a b) == (this a / this b)%Q.
Proof.
  intros a b Hb.
  unfold Qcdiv, Qdiv.
  eapply Qeq_trans; [apply this_Qcmult_Qeq|].
  apply Qmult_comp; [reflexivity|].
  apply this_Qcinv_Qeq; exact Hb.
Qed.

Lemma Q_mul_div_inject_scale :
  forall c n rs,
    0 < n ->
    0 <= rs ->
    ((inject_Z c / inject_Z 100 / inject_Z n) * inject_Z (10 ^ rs))%Q ==
    (inject_Z (c * 10 ^ rs) / inject_Z (100 * n))%Q.
Proof.
  intros c n rs Hn Hrs.
  unfold Qdiv.
  rewrite !inject_Z_mult.
  field; unfold inject_Z; cbn [Qnum Qden]; intros Heq; unfold Qeq in Heq;
    cbn [Qnum Qden] in Heq; lia.
Qed.

Lemma avg_scaled_quot_Qeq :
  forall c n rs,
    0 < n ->
    0 <= rs ->
    let ql := Qcdiv (Q2Qc (inject_Z c)) (Q2Qc (inject_Z 100)) in
    let qr := Q2Qc (inject_Z n) in
    this (Qcmult (Qcdiv ql qr) (Q2Qc (inject_Z (10 ^ rs)))) ==
    (inject_Z (c * 10 ^ rs) / inject_Z (100 * n))%Q.
Proof.
  intros c n rs Hn Hrs ql qr.
  assert (H100 : Q2Qc (inject_Z 100) <> 0%Qc)
    by (apply Q2Qc_inject_Z_neq0; lia).
  assert (Hn0 : Q2Qc (inject_Z n) <> 0%Qc)
    by (apply Q2Qc_inject_Z_neq0; lia).
  eapply Qeq_trans; [apply this_Qcmult_Qeq|].
  eapply Qeq_trans.
  { apply Qmult_comp.
    - eapply Qeq_trans; [apply this_Qcdiv_Qeq; exact Hn0|].
      apply Qdiv_comp.
      + eapply Qeq_trans; [apply this_Qcdiv_Qeq; exact H100|].
        apply Qdiv_comp; apply this_Q2Qc_inject_Z.
      + apply this_Q2Qc_inject_Z.
    - apply this_Q2Qc_inject_Z. }
  apply Q_mul_div_inject_scale; assumption.
Qed.

Lemma avg_scaled_quot_num_abs_le :
  forall c n rs,
    0 < n ->
    0 <= rs ->
    let ql := Qcdiv (Q2Qc (inject_Z c)) (Q2Qc (inject_Z 100)) in
    let qr := Q2Qc (inject_Z n) in
    let scaled :=
      this (Qcmult (Qcdiv ql qr) (Q2Qc (inject_Z (10 ^ rs)))) in
    Z.abs (Qnum scaled) <= Z.abs (c * 10 ^ rs).
Proof.
  intros c n rs Hn Hrs ql qr scaled.
  pose proof (avg_scaled_quot_Qeq c n rs Hn Hrs) as Heq.
  fold ql qr in Heq.
  change (Z.abs (Qnum scaled) <= Z.abs (c * 10 ^ rs)).
  assert (Hred : scaled = Qred (inject_Z (c * 10 ^ rs) / inject_Z (100 * n))).
  { unfold scaled.
    rewrite <- (Qc_is_reduced
      (Qcmult (Qcdiv ql qr) (Q2Qc (inject_Z (10 ^ rs))))).
    apply Qred_complete.
    exact Heq. }
  rewrite Hred.
  assert (Heq' : (inject_Z (c * 10 ^ rs) / inject_Z (100 * n))%Q ==
                 ((c * 10 ^ rs) # Z.to_pos (100 * n))%Q).
  { apply inject_Z_div_Qeq; nia. }
  rewrite (Qred_complete _ _ Heq').
  apply Qred_num_abs_le_frac.
Qed.

Lemma numeric_display_scale_valid_2 :
  numeric_display_scale_valid_bool 2 = true.
Proof. vm_compute; reflexivity. Qed.

Lemma numeric_display_scale_valid_0 :
  numeric_display_scale_valid_bool 0 = true.
Proof. vm_compute; reflexivity. Qed.

Lemma numeric_of_Z_eqb_zero_false :
  forall n, 0 < n -> numeric_eqb (numeric_of_Z n) numeric_zero = false.
Proof.
  intros n Hn.
  apply numeric_positive_is_nonzero.
  cbn [numeric_compare numeric_zero numeric_of_Z].
  apply (proj1 (Qclt_alt _ _)).
  unfold Qclt, Q2Qc; cbn [this].
  apply (proj2 (Qred_lt _ _)).
  replace 0%Q with (inject_Z 0) by reflexivity.
  rewrite <- Zlt_Qlt.
  lia.
Qed.

Lemma Qred_inject_Z : forall n, Qred (inject_Z n) = inject_Z n.
Proof.
  intros n.
  unfold Qred, inject_Z; cbn.
  destruct (Z.ggcd n 1) as [g [aa bb]] eqn:Hgg.
  pose proof (Z.ggcd_correct_divisors n 1) as H; rewrite Hgg in H; cbn in H.
  destruct H as [Hn H1].
  pose proof (Z.ggcd_gcd n 1) as Hg; rewrite Hgg in Hg; cbn in Hg.
  assert (Hg1 : g = 1) by (rewrite Hg; apply Z.gcd_1_r).
  subst g.
  assert (Hbb : bb = 1) by lia.
  subst bb.
  assert (Haa : aa = n) by lia.
  subst aa.
  reflexivity.
Qed.

Lemma numeric_remove_factor_1 :
  forall fuel factor,
    1 < factor ->
    numeric_remove_factor fuel factor 1 = (0%nat, 1).
Proof.
  intros fuel factor Hf.
  destruct fuel; cbn; [reflexivity|].
  assert (Hmod : 1 mod factor <> 0).
  { rewrite Z.mod_1_l by lia. lia. }
  destruct (Z.eqb_spec (1 mod factor) 0); [lia|reflexivity].
Qed.

Lemma numeric_decimal_parts_of_Z :
  forall n, numeric_decimal_parts (numeric_of_Z n) = Some (n, 0).
Proof.
  intros n.
  unfold numeric_of_Z, numeric_decimal_parts.
  cbn [this Q2Qc].
  rewrite Qred_inject_Z.
  unfold inject_Z; cbn [Qnum Qden].
  rewrite !numeric_remove_factor_1 by lia.
  change (1 =? 1) with true.
  cbn [andb].
  rewrite Z.max_id.
  rewrite !Z.sub_diag, !Z.pow_0_r, !Z.mul_1_r.
  reflexivity.
Qed.

Lemma numeric_pg_div_scale_le_max_display :
  forall n1 d1 n2 d2 rs,
    numeric_pg_div_scale n1 d1 n2 d2 = Some rs ->
    rs <= postgres_numeric_max_display_scale.
Proof.
  intros n1 d1 n2 d2 rs Hrs.
  unfold numeric_pg_div_scale in Hrs.
  destruct (numeric_decimal_parts n1) as [[c1 s1]|]; [|discriminate].
  destruct (numeric_decimal_parts n2) as [[c2 s2]|]; [|discriminate].
  injection Hrs as <-.
  apply Z.le_min_r.
Qed.

Lemma numeric_pg_div_scale_of_scaled_of_Z :
  forall c s n,
    0 <= s ->
    0 < n ->
    exists result_scale,
      numeric_pg_div_scale
        (numeric_of_scaled c s) s (numeric_of_Z n) 0 = Some result_scale.
Proof.
  intros c s n Hs Hn.
  unfold numeric_pg_div_scale.
  destruct (numeric_of_scaled_decimal_parts_some c s Hs)
    as [cl [sl Hpl]].
  rewrite Hpl, numeric_decimal_parts_of_Z.
  eexists; reflexivity.
Qed.

(** AVG finalize: PG div of DECIMAL(15,2) sum coeff by finite count fits. *)
Lemma decimal15_2_avg_div_runtime_safe :
  forall finite_count coefficient,
    1 <= finite_count <= int64_max ->
    Z.abs coefficient <= finite_count * Z.pow 10 15 ->
    numeric_div_runtime_error
      [Value_numeric (Some (numeric_of_scaled coefficient 2));
       Value_Z (Some 2);
       Value_numeric (Some (numeric_of_Z finite_count));
       Value_Z (Some 0)] = None.
Proof.
  intros finite_count coefficient [Hc0 Hc1] Habs.
  assert (Hnz : numeric_eqb (numeric_of_Z finite_count) numeric_zero = false)
    by (apply numeric_of_Z_eqb_zero_false; lia).
  assert (Hleft_sc := numeric_display_scale_valid_2).
  assert (Hright_sc := numeric_display_scale_valid_0).
  destruct (numeric_pg_div_scale_of_scaled_of_Z coefficient 2 finite_count
              ltac:(lia) ltac:(lia)) as [result_scale Hrs].
  pose proof (numeric_pg_div_scale_display_valid
    (numeric_of_scaled coefficient 2) 2
    (numeric_of_Z finite_count) 0 result_scale Hrs) as Hrs_valid.
  set (left_n := numeric_of_scaled coefficient 2) in *.
  set (right_n := numeric_of_Z finite_count) in *.
  assert (Hdiv_round :
    exists ql qr,
      left_n = NumericFinite ql /\
      right_n = NumericFinite qr /\
      numeric_div_at_scales left_n 2 right_n 0 =
        Some (numeric_round_to_scale (NumericFinite (Qcdiv ql qr))
                result_scale)).
  { exists (Qcdiv (Q2Qc (inject_Z coefficient)) (Q2Qc (inject_Z (10 ^ 2)))),
      (Q2Qc (inject_Z finite_count)).
    unfold left_n, right_n, numeric_of_scaled, numeric_of_Z.
    rewrite (proj2 (Z.leb_le 0 2) ltac:(lia)).
    split; [reflexivity|].
    split; [reflexivity|].
    apply finite_numeric_division_result_rounding; [exact Hnz|].
    unfold left_n, right_n, numeric_of_scaled, numeric_of_Z in Hrs.
    rewrite (proj2 (Z.leb_le 0 2) ltac:(lia)) in Hrs.
    exact Hrs. }
  destruct Hdiv_round as [ql [qr [Hl [Hr Hdiv]]]].
  set (result := numeric_round_to_scale
                   (NumericFinite (Qcdiv ql qr)) result_scale).
  assert (Hrs0 : 0 <= result_scale).
  { unfold numeric_display_scale_valid_bool in Hrs_valid.
    apply andb_true_iff in Hrs_valid; destruct Hrs_valid as [Ha _].
    apply Z.leb_le in Ha; exact Ha. }
  assert (Hrs1 : result_scale <= postgres_numeric_max_display_scale).
  { eapply numeric_pg_div_scale_le_max_display; exact Hrs. }
  assert (Hfits : numeric_runtime_fits_bool result = true).
  { unfold result, numeric_round_to_scale, numeric_rounded_coeff,
      numeric_finite_rounded_coeff.
    assert (Hql : ql =
      Qcdiv (Q2Qc (inject_Z coefficient)) (Q2Qc (inject_Z 100))).
    { assert (Hleft : left_n =
        NumericFinite
          (Qcdiv (Q2Qc (inject_Z coefficient)) (Q2Qc (inject_Z (10 ^ 2))))).
      { unfold left_n, numeric_of_scaled.
        rewrite (proj2 (Z.leb_le 0 2) ltac:(lia)). reflexivity. }
      rewrite Hleft in Hl; injection Hl as <-.
      replace (10 ^ 2) with 100 by reflexivity.
      reflexivity. }
    assert (Hqr : qr = Q2Qc (inject_Z finite_count)).
    { assert (Hright : right_n = NumericFinite (Q2Qc (inject_Z finite_count))).
      { unfold right_n, numeric_of_Z. reflexivity. }
      rewrite Hright in Hr; injection Hr as <-; reflexivity. }
    rewrite Hql, Hqr.
    assert (Hsf : numeric_scale_factor result_scale =
      Q2Qc (inject_Z (10 ^ result_scale))).
    { unfold numeric_scale_factor.
      rewrite (proj2 (Z.leb_le 0 result_scale) Hrs0). reflexivity. }
    set (scaled := this
      (Qcmult
        (Qcdiv
          (Qcdiv (Q2Qc (inject_Z coefficient)) (Q2Qc (inject_Z 100)))
          (Q2Qc (inject_Z finite_count)))
        (Q2Qc (inject_Z (10 ^ result_scale))))).
    set (rc := numeric_round_quot (Qnum scaled) (Z.pos (Qden scaled))).
    (* Match goal's round_quot with rc after Hsf *)
    rewrite Hsf.
    change (numeric_runtime_fits_bool
      (numeric_of_scaled
        (numeric_round_quot (Qnum scaled) (Z.pos (Qden scaled)))
        result_scale) = true).
    fold rc.
    apply numeric_of_scaled_runtime_fits_of_digit_bound.
    - split; [exact Hrs0|].
      eapply Z.le_trans; [exact Hrs1|].
      unfold postgres_numeric_max_display_scale,
        postgres_numeric_max_fractional_digits; lia.
    - assert (Hrc : Z.abs rc <= Z.abs (Qnum scaled) + 1).
      { unfold rc; apply numeric_round_quot_abs_le; lia. }
      assert (Hnum : Z.abs (Qnum scaled) <=
        Z.abs (coefficient * 10 ^ result_scale)).
      { apply avg_scaled_quot_num_abs_le; lia. }
      assert (Hbound : Z.abs rc <=
        int64_max * Z.pow 10 15 *
          Z.pow 10 postgres_numeric_max_display_scale + 1).
      { eapply Z.le_trans; [exact Hrc|].
        eapply Z.le_trans; [apply Z.add_le_mono_r; exact Hnum|].
        assert (H1 : Z.abs (coefficient * 10 ^ result_scale) <=
          finite_count * Z.pow 10 15 * Z.pow 10 result_scale).
        { rewrite Z.abs_mul.
          eapply Z.le_trans.
          - apply Z.mul_le_mono_nonneg_r; [apply Z.abs_nonneg|exact Habs].
          - rewrite Z.abs_eq by (apply Z.pow_nonneg; lia).
            apply Z.mul_le_mono_nonneg_l; [nia|].
            apply Z.pow_le_mono_r; lia. }
        eapply Z.le_trans.
        { apply Z.add_le_mono_r. eapply Z.le_trans; [exact H1|].
          apply Z.mul_le_mono_nonneg_r; [apply Z.pow_nonneg; lia|].
          apply Z.mul_le_mono_nonneg_r; [apply Z.pow_nonneg; lia|exact Hc1]. }
        apply Z.add_le_mono_r.
        apply Z.mul_le_mono_nonneg_l.
        { apply Z.mul_nonneg_nonneg; [unfold int64_max; lia|apply Z.pow_nonneg; lia]. }
        apply Z.pow_le_mono_r; lia. }
      set (N := 19 + 15 + postgres_numeric_max_display_scale).
      assert (Hpow : Z.abs rc <= Z.pow 10 N).
      { assert (Hint : int64_max < Z.pow 10 19).
        { unfold int64_max.
          change (10 ^ 19) with 10000000000000000000.
          lia. }
        assert (Hprod' :
          int64_max * Z.pow 10 15 *
            Z.pow 10 postgres_numeric_max_display_scale + 1 <=
          Z.pow 10 N).
        { unfold N.
          set (A := int64_max * Z.pow 10 15).
          set (B := Z.pow 10 postgres_numeric_max_display_scale).
          set (A' := Z.pow 10 19 * Z.pow 10 15).
          assert (HltA : A < A').
          { unfold A, A'.
            apply Z.mul_lt_mono_pos_r; [apply Z.pow_pos_nonneg; lia|exact Hint]. }
          assert (HB_pos : 0 < B).
          { unfold B; apply Z.pow_pos_nonneg;
              unfold postgres_numeric_max_display_scale; lia. }
          assert (Hlt : A * B < A' * B).
          { apply Z.mul_lt_mono_pos_r; [exact HB_pos|exact HltA]. }
          replace (Z.pow 10 (19 + 15 + postgres_numeric_max_display_scale))
            with (A' * B).
          2:{ unfold A', B; rewrite !Z.pow_add_r;
                unfold postgres_numeric_max_display_scale; lia. }
          lia. }
        eapply Z.le_trans; [exact Hbound|exact Hprod']. }
      pose proof (numeric_digit_count_le_of_le_pow rc N
        ltac:(unfold N, postgres_numeric_max_display_scale; lia) Hpow)
        as Hd.
      eapply Z.le_trans; [exact Hd|].
      unfold N, postgres_numeric_max_integer_digits,
        postgres_numeric_max_display_scale; lia. }
  change (numeric_of_scaled coefficient 2) with left_n.
  change (numeric_of_Z finite_count) with right_n.
  rewrite Hl in Hdiv |- *.
  rewrite Hr in Hdiv |- *.
  eapply finite_numeric_division_runtime_error_none.
  - rewrite <- Hr; exact Hnz.
  - exact Hleft_sc.
  - exact Hright_sc.
  - exact Hdiv.
  - exact Hfits.
Qed.
