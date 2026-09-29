(* Exact NUMERIC aggregates over scaled decimals (query-generic).

   A.  SUM:  interp_sum_numeric over values c_i / 10^s is the scaled integer
       sum (NULL on empty input).
   B.  AVG:  interp_avg_numeric_fixed p s over values c_i / 10^s (conforming
       to NUMERIC(p,s)) is numeric_div_at_scales of the scaled integer sum by
       the count -- i.e. [avg_value (sum c_i) s (count)].
   C.  PostgreSQL's avg division in closed integer form:
         numeric_div_at_scales (numeric_of_scaled sum s) s (numeric_of_Z n) 0
           = Some (numeric_of_scaled (pg_avg_coeff sum s n) (pg_avg_scale sum s n))
       [pg_avg_scale] / [pg_avg_coeff] are transliterated by the Dafny module
       tests/verification/logical/dafny/PgNumeric.dfy (DivScale /
       AvgOfScaledSum) and the C++ helper stateful/core/pg_numeric.hpp.

   Together these discharge a query's "numeric embedding" obligation (e.g.
   Q1's former NumericEmbedFaithful premise) instead of assuming it. *)

From Stdlib Require Import List ZArith Lia QArith Qcanon Qreduction.
From SQLFS Require Import Values ValueCore ValueNumeric ValueNumericTypmod.
From Logos.FormalSQL Require Import NumericFacts NumericDerivedFacts NumericRegroupFacts.
From FormalSQLLib Require Import NumericScaledFits NumericPackagingFacade.

Import ListNotations.
Import NullValues.
Local Open Scope Z_scope.

Definition scaled_value (s c : Z) : value := Value_numeric (Some (numeric_of_scaled c s)).

Definition zsum (cs : list Z) : Z := fold_right Z.add 0 cs.

(** [avg_value sum s n]: the SQL value of avg(numeric) whose finite inputs
    have display scale [s], coefficient sum [sum] and count [n]. *)
Definition avg_value (sum s n : Z) : value :=
  match numeric_div_at_scales (numeric_of_scaled sum s) s (numeric_of_Z n) 0 with
  | Some r => Value_numeric (Some r)
  | None => Value_numeric None
  end.

(* ------------------------------------------------------------------------ *)
(** * A. SUM *)

Lemma numeric_values_scaled : forall s cs,
  numeric_values (map (scaled_value s) cs) = map (fun c => numeric_of_scaled c s) cs.
Proof. intros s cs; induction cs as [|c cs IH]; cbn; [reflexivity|now rewrite IH]. Qed.

Lemma forallb_numeric_scaled : forall s cs,
  forallb is_numeric_value (map (scaled_value s) cs) = true.
Proof. intros s cs; induction cs as [|c cs IH]; cbn; [reflexivity|exact IH]. Qed.

Lemma fold_sum_option_scaled : forall s cs acc,
  fold_left numeric_sum_option_add (map (fun c => numeric_of_scaled c s) cs)
    (Some (numeric_of_scaled acc s)) =
  Some (numeric_of_scaled (acc + zsum cs) s).
Proof.
  intros s cs; induction cs as [|c cs IH]; intros acc;
    cbn [map fold_left numeric_sum_option_add zsum fold_right].
  - now rewrite Z.add_0_r.
  - rewrite numeric_add_same_scale. fold (zsum cs). rewrite IH. f_equal. f_equal. lia.
Qed.

Theorem interp_sum_numeric_scaled : forall s cs,
  interp_sum_numeric (map (scaled_value s) cs) =
  match cs with
  | [] => Value_numeric None
  | _ => scaled_value s (zsum cs)
  end.
Proof.
  intros s cs.
  unfold interp_sum_numeric.
  rewrite forallb_numeric_scaled, numeric_values_scaled, numeric_sum_fold_from_initial.
  destruct cs as [|c cs]; [reflexivity|].
  cbn [map fold_left numeric_sum_option_add].
  rewrite fold_sum_option_scaled. reflexivity.
Qed.

(* ------------------------------------------------------------------------ *)
(** * B. AVG over a fixed-typmod column *)

Lemma numeric_of_scaled_finite : forall c s,
  exists q, numeric_of_scaled c s = NumericFinite q.
Proof. intros; eexists; reflexivity. Qed.

Lemma avg_fold_scaled : forall s cs st,
  0 <= s ->
  fold_left (numeric_avg_scale_transition s) (map (fun c => numeric_of_scaled c s) cs) st =
  NumericAvgScaleState
    (numeric_avg_finite_count st + Z.of_nat (length cs))
    (numeric_avg_nan_count st)
    (numeric_avg_pos_inf_count st)
    (numeric_avg_neg_inf_count st)
    (numeric_avg_sum_coeff st + zsum cs).
Proof.
  intros s cs; induction cs as [|c cs IH]; intros st Hs; cbn [map fold_left length].
  - destruct st; cbn. f_equal; lia.
  - rewrite IH by exact Hs.
    pose proof (numeric_finite_rounded_coeff_of_scaled c s Hs) as Hc.
    destruct (numeric_of_scaled c s) as [|q| |] eqn:Hq; try contradiction.
    cbn [numeric_avg_scale_transition numeric_avg_finite_count numeric_avg_nan_count
         numeric_avg_pos_inf_count numeric_avg_neg_inf_count numeric_avg_sum_coeff
         zsum fold_right].
    rewrite Hc. fold (zsum cs). rewrite Nat2Z.inj_succ. f_equal; lia.
Qed.

Theorem interp_avg_numeric_fixed_scaled : forall p s cs,
  0 <= s ->
  numeric_typmod_valid_bool p s = true ->
  forallb (numeric_conforms_typmod_bool p s) (map (fun c => numeric_of_scaled c s) cs) = true ->
  interp_avg_numeric_fixed p s (map (scaled_value s) cs) =
  avg_value (zsum cs) s (Z.of_nat (length cs)).
Proof.
  intros p s cs Hs Hvalid Hconf.
  unfold interp_avg_numeric_fixed.
  rewrite Hvalid, forallb_numeric_scaled; cbn [andb].
  rewrite numeric_values_scaled, Hconf.
  rewrite avg_fold_scaled by exact Hs.
  unfold numeric_avg_scale_initial, avg_value; cbn [numeric_avg_finite_count numeric_avg_nan_count
    numeric_avg_pos_inf_count numeric_avg_neg_inf_count numeric_avg_sum_coeff].
  unfold numeric_avg_from_scale_state, numeric_avg_scale_total_count, numeric_agg_total_count;
    cbn [numeric_avg_finite_count numeric_avg_nan_count numeric_avg_pos_inf_count
         numeric_avg_neg_inf_count numeric_avg_sum_coeff].
  destruct cs as [|c cs].
  - cbn. reflexivity.
  - cbn [length zsum].
    replace (0 + Z.of_nat (S (length cs)) + 0 + 0 + 0 =? 0) with false
      by (symmetry; apply Z.eqb_neq; lia).
    cbn [numeric_agg_special_result Z.ltb Z.compare].
    replace (0 + Z.of_nat (S (length cs))) with (Z.of_nat (S (length cs))) by lia.
    replace (0 + zsum (c :: cs)) with (zsum (c :: cs)) by lia.
    destruct (numeric_div_at_scales _ _ _ _); reflexivity.
Qed.

(* ------------------------------------------------------------------------ *)
(** * C. PostgreSQL avg division in closed integer form *)

(** ** Digit counts *)

Lemma digit_count_fuel_le_of_lt_pow : forall fuel w m,
  1 <= m -> 0 <= w < 10 ^ m -> numeric_digit_count_fuel fuel w <= m.
Proof.
  induction fuel as [|fuel IH]; intros w m Hm Hw; cbn [numeric_digit_count_fuel]; [lia|].
  destruct (Z.ltb_spec0 w 10) as [Hlt|Hge]; [lia|].
  assert (Hm2 : 2 <= m).
  { destruct (Z.eq_dec m 1) as [->|]; [cbn in Hw; lia|lia]. }
  assert (Hw10 : 0 <= w / 10 < 10 ^ (m - 1)).
  { split; [apply Z.div_pos; lia|].
    apply Z.div_lt_upper_bound; [lia|].
    replace (10 * 10 ^ (m - 1)) with (10 ^ m); [lia|].
    rewrite <- Z.pow_succ_r by lia. f_equal. lia. }
  pose proof (IH (w / 10) (m - 1) ltac:(lia) Hw10). lia.
Qed.

Lemma digit_count_le_of_lt_pow : forall z m,
  1 <= m -> Z.abs z < 10 ^ m -> numeric_digit_count z <= m.
Proof.
  intros z m Hm Hz. unfold numeric_digit_count.
  apply digit_count_fuel_le_of_lt_pow; lia.
Qed.

Lemma digit_count_abs : forall z, numeric_digit_count (Z.abs z) = numeric_digit_count z.
Proof. intros z. unfold numeric_digit_count. now rewrite Z.abs_idemp. Qed.

(** For z <> 0: 10^(dc-1) <= |z| < 10^dc. *)
Lemma digit_count_bounds : forall z,
  z <> 0 ->
  10 ^ (numeric_digit_count z - 1) <= Z.abs z < 10 ^ numeric_digit_count z.
Proof.
  intros z Hz.
  pose proof (numeric_digit_count_pos z) as Hpos.
  split.
  - destruct (Z_lt_le_dec (Z.abs z) (10 ^ (numeric_digit_count z - 1))) as [Hlt|Hle]; [|exact Hle].
    destruct (Z.eq_dec (numeric_digit_count z) 1) as [H1|H1].
    + rewrite H1 in Hlt. cbn in Hlt. lia.
    + pose proof (digit_count_le_of_lt_pow z (numeric_digit_count z - 1) ltac:(lia) Hlt). lia.
  - rewrite <- digit_count_abs. apply numeric_digit_count_abs_lt_pow. lia.
Qed.

Lemma digit_count_of_bounds : forall z k,
  0 <= k -> 10 ^ k <= Z.abs z < 10 ^ (k + 1) -> numeric_digit_count z = k + 1.
Proof.
  intros z k Hk [Hlo Hhi].
  pose proof (digit_count_le_of_lt_pow z (k + 1) ltac:(lia) Hhi).
  pose proof (numeric_digit_count_ge_of_pow (Z.abs z) k Hk Hlo).
  rewrite digit_count_abs in *. lia.
Qed.

Lemma digit_count_mul_pow10 : forall c j,
  c <> 0 -> 0 <= j ->
  numeric_digit_count (c * 10 ^ j) = numeric_digit_count c + j.
Proof.
  intros c j Hc Hj.
  pose proof (digit_count_bounds c Hc) as [Hlo Hhi].
  pose proof (numeric_digit_count_pos c) as Hpos.
  assert (Hp : 0 < 10 ^ j) by (apply Z.pow_pos_nonneg; lia).
  replace (numeric_digit_count c + j) with ((numeric_digit_count c - 1 + j) + 1) by lia.
  apply digit_count_of_bounds; [lia|].
  rewrite Z.abs_mul, (Z.abs_eq (10 ^ j)) by lia.
  rewrite !Z.pow_add_r by lia.
  assert (Hd : 10 ^ numeric_digit_count c = 10 ^ (numeric_digit_count c - 1) * 10 ^ 1)
    by (rewrite <- Z.pow_add_r by lia; f_equal; lia).
  rewrite Hd in Hhi.
  split; nia.
Qed.

(** ** Weight / first digit depend only on the value *)

Lemma pg_weight_mul_pow10 : forall c t j,
  0 <= j ->
  numeric_pg_weight (c * 10 ^ j) (t + j) = numeric_pg_weight c t.
Proof.
  intros c t j Hj. unfold numeric_pg_weight.
  assert (Hp : 0 < 10 ^ j) by (apply Z.pow_pos_nonneg; lia).
  destruct (Z.eq_dec c 0) as [->|Hc]; [reflexivity|].
  replace (c * 10 ^ j =? 0) with false by (symmetry; apply Z.eqb_neq; nia).
  replace (c =? 0) with false by (symmetry; apply Z.eqb_neq; exact Hc).
  rewrite digit_count_mul_pow10 by assumption.
  f_equal. lia.
Qed.

Lemma shift_by_pow10_mul : forall a e j,
  0 <= a -> 0 <= j ->
  numeric_shift_by_pow10 (a * 10 ^ j) (e + j) = numeric_shift_by_pow10 a e.
Proof.
  intros a e j Ha Hj. unfold numeric_shift_by_pow10.
  assert (Hp : 0 < 10 ^ j) by (apply Z.pow_pos_nonneg; lia).
  destruct (Z.leb_spec0 0 (e + j)) as [Hej|Hej];
  destruct (Z.leb_spec0 0 e) as [He|He].
  - rewrite Z.pow_add_r by lia.
    rewrite (Z.mul_comm (10 ^ e) (10 ^ j)).
    rewrite <- Z.div_div by (try apply Z.pow_pos_nonneg; lia).
    rewrite Z.div_mul by lia. reflexivity.
  - (* e < 0 <= e + j: exact division *)
    replace (10 ^ j) with (10 ^ (e + j) * 10 ^ (- e))
      by (rewrite <- Z.pow_add_r by lia; f_equal; lia).
    replace (a * (10 ^ (e + j) * 10 ^ (- e))) with ((a * 10 ^ (- e)) * 10 ^ (e + j)) by ring.
    rewrite Z.div_mul; [reflexivity|].
    pose proof (Z.pow_pos_nonneg 10 (e + j) ltac:(lia) ltac:(lia)). lia.
  - lia.
  - rewrite <- Z.mul_assoc, <- Z.pow_add_r by lia. f_equal. f_equal. lia.
Qed.

Lemma pg_first_digit_mul_pow10 : forall c t j,
  0 <= j ->
  numeric_pg_first_digit (c * 10 ^ j) (t + j) = numeric_pg_first_digit c t.
Proof.
  intros c t j Hj. unfold numeric_pg_first_digit.
  assert (Hp : 0 < 10 ^ j) by (apply Z.pow_pos_nonneg; lia).
  destruct (Z.eq_dec c 0) as [->|Hc]; [reflexivity|].
  replace (c * 10 ^ j =? 0) with false by (symmetry; apply Z.eqb_neq; nia).
  replace (c =? 0) with false by (symmetry; apply Z.eqb_neq; exact Hc).
  rewrite pg_weight_mul_pow10 by exact Hj.
  rewrite Z.abs_mul, (Z.abs_eq (10 ^ j)) by lia.
  replace (t + j + numeric_pg_weight c t * postgres_numeric_dec_digits)
    with ((t + numeric_pg_weight c t * postgres_numeric_dec_digits) + j) by lia.
  apply shift_by_pow10_mul; lia.
Qed.

(** The minimal representation of c/10^s has the same weight / first digit. *)
Lemma decimal_parts_scaled_invariants : forall c s coeff scale,
  0 <= s ->
  numeric_decimal_parts (numeric_of_scaled c s) = Some (coeff, scale) ->
  numeric_pg_weight coeff scale = numeric_pg_weight c s /\
  numeric_pg_first_digit coeff scale = numeric_pg_first_digit c s.
Proof.
  intros c s coeff scale Hs Hparts.
  destruct (numeric_of_scaled_decimal_parts_bounds c s Hs)
    as [coeff' [scale' [Hparts' [Hsc [_ Heq]]]]].
  rewrite Hparts in Hparts'. injection Hparts' as <- <-.
  (* coeff * 10^s = c * 10^scale, 0 <= scale <= s  ==>  c = coeff * 10^(s - scale) *)
  assert (Hc : c = coeff * 10 ^ (s - scale)).
  { assert (Hpos : 0 < 10 ^ scale) by (apply Z.pow_pos_nonneg; lia).
    apply (Z.mul_reg_r _ _ (10 ^ scale)); [lia|].
    rewrite <- Z.mul_assoc, <- Z.pow_add_r by lia.
    replace (s - scale + scale) with s by lia. lia. }
  rewrite Hc.
  replace (numeric_pg_weight (coeff * 10 ^ (s - scale)) s)
    with (numeric_pg_weight (coeff * 10 ^ (s - scale)) (scale + (s - scale)))
    by (f_equal; lia).
  replace (numeric_pg_first_digit (coeff * 10 ^ (s - scale)) s)
    with (numeric_pg_first_digit (coeff * 10 ^ (s - scale)) (scale + (s - scale)))
    by (f_equal; lia).
  rewrite pg_weight_mul_pow10, pg_first_digit_mul_pow10 by lia.
  split; reflexivity.
Qed.

(** ** Closed forms (Dafny twin: PgNumeric.DivScale / AvgOfScaledSum) *)

Definition pg_div_scale_Z (c1 s1 c2 s2 : Z) : Z :=
  let weight1 := numeric_pg_weight c1 s1 in
  let weight2 := numeric_pg_weight c2 s2 in
  let firstdigit1 := numeric_pg_first_digit c1 s1 in
  let firstdigit2 := numeric_pg_first_digit c2 s2 in
  let qweight :=
    if firstdigit1 <=? firstdigit2 then weight1 - weight2 - 1 else weight1 - weight2 in
  let rscale := postgres_numeric_min_sig_digits - qweight * postgres_numeric_dec_digits in
  let rscale := Z.max rscale s1 in
  let rscale := Z.max rscale s2 in
  let rscale := Z.max rscale postgres_numeric_min_display_scale in
  Z.min rscale postgres_numeric_max_display_scale.

Definition pg_avg_scale (sum s n : Z) : Z := pg_div_scale_Z sum s n 0.

Definition pg_avg_coeff (sum s n : Z) : Z :=
  numeric_round_quot (sum * 10 ^ (pg_avg_scale sum s n - s)) n.

Lemma numeric_of_Z_as_scaled0 : forall n, numeric_of_Z n = numeric_of_scaled n 0.
Proof.
  intros n. unfold numeric_of_Z, numeric_of_scaled.
  replace (0 <=? 0) with true by reflexivity.
  rewrite Z.pow_0_r. f_equal.
  unfold Qcdiv.
  replace (/ Q2Qc (inject_Z 1))%Qc with 1%Qc by (apply Qc_is_canon; reflexivity).
  now rewrite Qcmult_1_r.
Qed.

Theorem pg_div_scale_of_scaled_of_Z : forall sum s n,
  0 <= s ->
  numeric_pg_div_scale (numeric_of_scaled sum s) s (numeric_of_Z n) 0 =
  Some (pg_avg_scale sum s n).
Proof.
  intros sum s n Hs.
  destruct (numeric_of_scaled_decimal_parts_bounds sum s Hs)
    as [c1 [t1 [H1 _]]].
  rewrite numeric_of_Z_as_scaled0.
  destruct (numeric_of_scaled_decimal_parts_bounds n 0 ltac:(lia))
    as [c2 [t2 [H2 _]]].
  unfold numeric_pg_div_scale. rewrite H1, H2.
  destruct (decimal_parts_scaled_invariants sum s c1 t1 Hs H1) as [Hw1 Hf1].
  destruct (decimal_parts_scaled_invariants n 0 c2 t2 ltac:(lia) H2) as [Hw2 Hf2].
  rewrite Hw1, Hf1, Hw2, Hf2. reflexivity.
Qed.

Lemma pg_avg_scale_ge : forall sum s n,
  s <= postgres_numeric_max_display_scale ->
  s <= pg_avg_scale sum s n <= postgres_numeric_max_display_scale.
Proof.
  intros sum s n Hs. unfold pg_avg_scale, pg_div_scale_Z.
  unfold postgres_numeric_max_display_scale, postgres_numeric_min_display_scale in *. lia.
Qed.


(** ** Rounding is invariant under scaling numerator and denominator *)

Lemma round_quot_scale : forall a b g,
  0 < g -> b <> 0 ->
  numeric_round_quot (a * g) (b * g) = numeric_round_quot a b.
Proof.
  intros a b g Hg Hb. unfold numeric_round_quot.
  rewrite Z.quot_mul_cancel_r by lia.
  assert (Hr : Z.rem (a * g) (b * g) = Z.rem a b * g).
  { rewrite !Z.rem_eq by nia. rewrite Z.quot_mul_cancel_r by lia. ring. }
  rewrite Hr.
  assert (Hcmp : (2 * Z.abs (Z.rem a b * g) >=? Z.abs (b * g)) =
                 (2 * Z.abs (Z.rem a b) >=? Z.abs b)).
  { rewrite !Z.abs_mul, (Z.abs_eq g) by lia.
    destruct (Z.geb_spec (2 * (Z.abs (Z.rem a b) * g)) (Z.abs b * g));
    destruct (Z.geb_spec (2 * Z.abs (Z.rem a b)) (Z.abs b)); nia. }
  rewrite Hcmp.
  assert (Hsign : (a * g * (b * g) <? 0) = (a * b <? 0)).
  { replace (a * g * (b * g)) with ((a * b) * (g * g)) by ring.
    assert (0 < g * g) by nia.
    destruct (Z.ltb_spec ((a * b) * (g * g)) 0); destruct (Z.ltb_spec (a * b) 0); nia. }
  rewrite Hsign. reflexivity.
Qed.

(** The reduced form of a # p shares a positive common factor with it. *)
Lemma Qred_common_factor : forall a p,
  exists g, 0 < g /\ a = Qnum (Qred (a # p)) * g /\ Z.pos p = Z.pos (Qden (Qred (a # p))) * g.
Proof.
  intros a p. unfold Qred.
  pose proof (Z.ggcd_correct_divisors a (Z.pos p)) as Hdiv.
  pose proof (Z.ggcd_gcd a (Z.pos p)) as Hgcd.
  destruct (Z.ggcd a (Z.pos p)) as [g [aa bb]]. cbn [fst snd Qnum Qden] in Hdiv, Hgcd |- *.
  destruct Hdiv as [Ha Hp].
  assert (Hg : 0 < g).
  { rewrite Hgcd. pose proof (Z.gcd_nonneg a (Z.pos p)).
    destruct (Z.eq_dec (Z.gcd a (Z.pos p)) 0) as [H0|]; [|lia].
    apply Z.gcd_eq_0_r in H0. lia. }
  assert (Hbb : 0 < bb) by nia.
  exists g. split; [exact Hg|]. split.
  - rewrite Ha. ring.
  - rewrite Z2Pos.id by exact Hbb. rewrite Hp. ring.
Qed.

(** ** The characterization *)

Lemma this_Q2Qc_inject_Z : forall z, this (Q2Qc (inject_Z z)) == inject_Z z.
Proof. intros z. apply Qred_correct. Qed.

Theorem pg_avg_exact : forall sum s n,
  0 <= s <= postgres_numeric_max_display_scale -> 0 < n ->
  numeric_div_at_scales (numeric_of_scaled sum s) s (numeric_of_Z n) 0 =
  Some (numeric_of_scaled (pg_avg_coeff sum s n) (pg_avg_scale sum s n)).
Proof.
  intros sum s n Hs Hn.
  set (rs := pg_avg_scale sum s n).
  pose proof (pg_avg_scale_ge sum s n ltac:(lia)) as Hrs. fold rs in Hrs.
  pose proof (pg_div_scale_of_scaled_of_Z sum s n ltac:(lia)) as Hscale. fold rs in Hscale.
  destruct (numeric_of_scaled_finite sum s) as [q1 Hq1].
  set (q2 := Q2Qc (inject_Z n)).
  assert (Hq2 : numeric_of_Z n = NumericFinite q2) by reflexivity.
  rewrite Hq1, Hq2 in Hscale |- *.
  assert (Hnz : numeric_eqb (NumericFinite q2) numeric_zero = false)
    by (rewrite <- Hq2; apply numeric_of_Z_eqb_zero_false; exact Hn).
  rewrite (finite_numeric_division_result_rounding q1 q2 s 0 rs Hnz Hscale).
  unfold numeric_round_to_scale, numeric_rounded_coeff. f_equal. f_equal.
  (* numeric_finite_rounded_coeff (q1 / q2) rs = round_quot (sum*10^(rs-s)) n *)
  unfold numeric_finite_rounded_coeff, numeric_scale_factor.
  replace (0 <=? rs) with true by (symmetry; apply Z.leb_le; lia).
  set (x := Qcmult (Qcdiv q1 q2) (Q2Qc (inject_Z (10 ^ rs)))).
  set (a := sum * 10 ^ rs). set (b := 10 ^ s * n).
  assert (Hb : 0 < b) by (unfold b; pose proof (Z.pow_pos_nonneg 10 s ltac:(lia) ltac:(lia)); nia).
  (* Q value of x *)
  assert (Hx : this x == a # Z.to_pos b).
  { unfold x.
    assert (H10s : Q2Qc (inject_Z (10 ^ s)) <> 0%Qc).
    { apply Q2Qc_inject_Z_neq0. pose proof (Z.pow_pos_nonneg 10 s ltac:(lia) ltac:(lia)). lia. }
    assert (Hq2nz : q2 <> 0%Qc) by (apply Q2Qc_inject_Z_neq0; lia).
    unfold numeric_of_scaled in Hq1. replace (0 <=? s) with true in Hq1 by (symmetry; apply Z.leb_le; lia).
    injection Hq1 as Hq1. rewrite <- Hq1.
    eapply Qeq_trans; [apply this_Qcmult_Qeq|].
    eapply Qeq_trans.
    { apply Qmult_comp.
      - eapply Qeq_trans; [apply this_Qcdiv_Qeq; exact Hq2nz|].
        apply Qdiv_comp.
        + eapply Qeq_trans; [apply this_Qcdiv_Qeq; exact H10s|].
          apply Qdiv_comp; apply this_Q2Qc_inject_Z.
        + apply this_Q2Qc_inject_Z.
      - apply this_Q2Qc_inject_Z. }
    unfold a, b. rewrite Qmake_Qdiv, Z2Pos.id by lia.
    unfold Qdiv. rewrite !inject_Z_mult.
    assert (Hp10 : 0 < 10 ^ s) by (apply Z.pow_pos_nonneg; lia).
    assert (Hz1 : ~ inject_Z (10 ^ s) == 0) by (intro E; unfold Qeq in E; cbn in E; lia).
    assert (Hz2 : ~ inject_Z n == 0) by (intro E; unfold Qeq in E; cbn in E; lia).
    field. split; assumption. }
  (* this x is reduced *)
  assert (Hred : this x = Qred (a # Z.to_pos b)).
  { rewrite <- (canon x). apply Qred_complete. exact Hx. }
  rewrite Hred.
  destruct (Qred_common_factor a (Z.to_pos b)) as [g [Hg [Ha Hbeq]]].
  rewrite Z2Pos.id in Hbeq by lia.
  rewrite <- (round_quot_scale _ _ g Hg) by lia.
  rewrite <- Ha, <- Hbeq.
  (* round_quot (sum*10^rs) (10^s*n) = round_quot (sum*10^(rs-s)) n *)
  unfold pg_avg_coeff. fold rs.
  unfold a, b.
  replace (sum * 10 ^ rs) with ((sum * 10 ^ (rs - s)) * 10 ^ s)
    by (rewrite <- Z.mul_assoc, <- Z.pow_add_r by lia; f_equal; f_equal; lia).
  rewrite (Z.mul_comm (10 ^ s) n).
  apply round_quot_scale; [apply Z.pow_pos_nonneg; lia|lia].
Qed.

(** The SQL value of avg(numeric) in closed form. *)
Corollary avg_value_exact : forall sum s n,
  0 <= s <= postgres_numeric_max_display_scale -> 0 < n ->
  avg_value sum s n =
  Value_numeric (Some (numeric_of_scaled (pg_avg_coeff sum s n) (pg_avg_scale sum s n))).
Proof.
  intros sum s n Hs Hn. unfold avg_value. rewrite pg_avg_exact by assumption. reflexivity.
Qed.
