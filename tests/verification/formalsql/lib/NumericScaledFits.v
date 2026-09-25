(* Digit-budget / of_scaled fits helpers for TPC-H-scale NUMERIC trees. *)

From Stdlib Require Import ZArith Lia QArith Qcanon Bool Morphisms Nat PArith Zwf.
From SQLFS Require Import Values ValueNumeric ValueNumericTypmod.
From Logos.FormalSQL Require Import NumericFacts NumericDerivedFacts NumericRegroupFacts.

Open Scope Z_scope.

(** * Qc / of_scaled algebra *)

Lemma Q2Qc_inject_Z_mul :
  forall (a b : Z),
    Q2Qc (inject_Z (a * b)) = Qcmult (Q2Qc (inject_Z a)) (Q2Qc (inject_Z b)).
Proof.
  intros a b.
  unfold Qcmult.
  apply (proj2 (Qceq_alt _ _)).
  unfold Qccompare.
  setoid_rewrite Qred_correct.
  rewrite inject_Z_mult.
  pose proof (Qred_correct (inject_Z a)) as Ha.
  pose proof (Qred_correct (inject_Z b)) as Hb.
  setoid_rewrite Ha.
  setoid_rewrite Hb.
  destruct (Qcompare_spec (inject_Z a * inject_Z b) (inject_Z a * inject_Z b))
    as [Heq | Hlt | Hgt].
  - reflexivity.
  - exfalso; exact (Qlt_irrefl _ Hlt).
  - exfalso; exact (Qlt_irrefl _ Hgt).
Qed.

Lemma Q2Qc_pow10_add :
  forall (sa sb : Z),
    0 <= sa -> 0 <= sb ->
    Q2Qc (inject_Z (10 ^ (sa + sb))) =
    Qcmult (Q2Qc (inject_Z (10 ^ sa))) (Q2Qc (inject_Z (10 ^ sb))).
Proof.
  intros sa sb Hsa Hsb.
  replace (10 ^ (sa + sb)) with (10 ^ sa * 10 ^ sb).
  2:{ rewrite Z.pow_add_r by lia. reflexivity. }
  apply Q2Qc_inject_Z_mul.
Qed.

Lemma numeric_mul_of_scaled_nonneg :
  forall (a sa b sb : Z),
    0 <= sa ->
    0 <= sb ->
    numeric_mul (numeric_of_scaled a sa) (numeric_of_scaled b sb) =
    numeric_of_scaled (a * b) (sa + sb).
Proof.
  intros a sa b sb Hsa Hsb.
  unfold numeric_mul, numeric_of_scaled.
  rewrite (proj2 (Z.leb_le 0 sa) Hsa).
  rewrite (proj2 (Z.leb_le 0 sb) Hsb).
  assert (Hsum : 0 <= sa + sb) by lia.
  rewrite (proj2 (Z.leb_le 0 (sa + sb)) Hsum).
  f_equal.
  unfold Qcdiv.
  rewrite Q2Qc_inject_Z_mul.
  rewrite Q2Qc_pow10_add by lia.
  rewrite Qcinv_mult_distr.
  ring.
Qed.

Lemma Q2Qc_inject_Z_opp :
  forall (c : Z),
    Qcopp (Q2Qc (inject_Z c)) = Q2Qc (inject_Z (- c)).
Proof.
  intro c.
  unfold Qcopp.
  apply (proj2 (Qceq_alt _ _)).
  unfold Qccompare.
  setoid_rewrite Qred_correct.
  pose proof (Qred_correct (inject_Z c)) as Hc.
  setoid_rewrite Hc.
  rewrite inject_Z_opp.
  destruct (Qcompare_spec (- inject_Z c) (- inject_Z c))
    as [Heq | Hlt | Hgt].
  - reflexivity.
  - exfalso; exact (Qlt_irrefl _ Hlt).
  - exfalso; exact (Qlt_irrefl _ Hgt).
Qed.

Lemma numeric_opp_of_scaled :
  forall (c s : Z),
    numeric_opp (numeric_of_scaled c s) = numeric_of_scaled (- c) s.
Proof.
  intros c s.
  unfold numeric_opp, numeric_of_scaled.
  destruct (Z.leb_spec0 0 s); f_equal; unfold Qcdiv;
    rewrite <- Q2Qc_inject_Z_opp; ring.
Qed.

Lemma Q2Qc_inject_Z_neq0 :
  forall (z : Z), z <> 0 -> Q2Qc (inject_Z z) <> 0%Qc.
Proof.
  intros z Hz Heq.
  assert (Heq' : Q2Qc (inject_Z z) == 0%Qc) by (rewrite Heq; reflexivity).
  change (this (Q2Qc (inject_Z z)) == this 0%Qc) in Heq'.
  cbn [this Q2Qc] in Heq'.
  assert (Hinj : inject_Z z == 0%Q).
  { eapply Qeq_trans; [apply Qeq_sym; apply Qred_correct|exact Heq']. }
  unfold inject_Z, Qeq in Hinj. cbn in Hinj. lia.
Qed.

Lemma numeric_of_Z_as_scaled2 :
  forall (z : Z),
    numeric_of_Z z = numeric_of_scaled (z * 100) 2.
Proof.
  intro z.
  unfold numeric_of_Z, numeric_of_scaled.
  cbn. f_equal.
  unfold Qcdiv.
  rewrite Q2Qc_inject_Z_mul.
  field.
  apply Q2Qc_inject_Z_neq0; lia.
Qed.

Lemma numeric_one_minus_scaled2 :
  forall (d : Z),
    numeric_sub (numeric_of_Z 1) (numeric_of_scaled d 2) =
    numeric_of_scaled (100 - d) 2.
Proof.
  intro d.
  unfold numeric_sub.
  rewrite numeric_of_Z_as_scaled2.
  rewrite numeric_opp_of_scaled.
  replace (1 * 100)%Z with 100%Z by lia.
  rewrite numeric_add_same_scale.
  f_equal.
Qed.

Lemma numeric_one_plus_scaled2 :
  forall (t : Z),
    numeric_add (numeric_of_Z 1) (numeric_of_scaled t 2) =
    numeric_of_scaled (100 + t) 2.
Proof.
  intro t.
  rewrite numeric_of_Z_as_scaled2.
  replace (1 * 100)%Z with 100%Z by lia.
  rewrite numeric_add_same_scale.
  f_equal.
Qed.

(** * Digit-count bounds *)

Lemma numeric_digit_count_fuel_le :
  forall (fuel : nat) (w n : Z),
    0 <= w ->
    1 <= n ->
    w < 10 ^ n ->
    numeric_digit_count_fuel fuel w <= n.
Proof.
  induction fuel as [|fuel IH]; intros w n Hw Hn Hlt.
  - cbn. lia.
  - cbn [numeric_digit_count_fuel].
    destruct (Z.ltb_spec0 w 10) as [Hsmall|Hbig]; [lia|].
    destruct (Z.eq_dec n 1) as [Hn1|Hn1].
    + subst n. change (10 ^ 1) with 10 in Hlt. lia.
    + assert (Hw' : 0 <= w / 10) by (apply Z.div_pos; lia).
      assert (Hn' : 1 <= n - 1) by lia.
      assert (Hpow : 10 ^ n = 10 * 10 ^ (n - 1)).
      { assert (Hn0 : 0 <= n - 1) by lia.
        rewrite <- (Z.pow_succ_r 10 (n - 1) Hn0).
        replace (Z.succ (n - 1)) with n by lia.
        reflexivity. }
      assert (Hlt' : w / 10 < 10 ^ (n - 1)).
      { rewrite Hpow in Hlt.
        apply Z.div_lt_upper_bound; lia. }
      pose proof (IH (w / 10) (n - 1) Hw' Hn' Hlt') as IH'.
      lia.
Qed.

Lemma numeric_digit_count_lt_pow :
  forall (z n : Z),
    1 <= n ->
    Z.abs z < 10 ^ n ->
    numeric_digit_count z <= n.
Proof.
  intros z n Hn Hlt.
  unfold numeric_digit_count.
  apply numeric_digit_count_fuel_le.
  - apply Z.abs_nonneg.
  - exact Hn.
  - exact Hlt.
Qed.

Lemma numeric_digit_count_le_of_le_pow :
  forall (z n : Z),
    1 <= n ->
    Z.abs z <= 10 ^ n ->
    numeric_digit_count z <= n + 1.
Proof.
  intros z n Hn Hle.
  assert (Hlt : Z.abs z < 10 ^ (n + 1)).
  { assert (10 ^ n < 10 ^ (n + 1)) by (apply Z.pow_lt_mono_r; lia).
    lia. }
  apply numeric_digit_count_lt_pow; lia.
Qed.

(** * remove_factor + pow10 divides *)

Lemma numeric_remove_factor_spec :
  forall (fuel : nat) (factor value : Z),
    1 < factor ->
    0 < value ->
    exists (count : nat) (rest : Z),
      numeric_remove_factor fuel factor value = (count, rest) /\
      value = factor ^ Z.of_nat count * rest /\
      0 < rest /\
      (rest mod factor <> 0 \/ (count = fuel)%nat).
Proof.
  induction fuel as [|fuel IH]; intros factor value Hf Hv.
  - cbn. exists O, value.
    split; [reflexivity|].
    split; [cbn [Z.of_nat]; rewrite Z.pow_0_r; lia|].
    split; [exact Hv|].
    right; reflexivity.
  - cbn [numeric_remove_factor].
    destruct (Z.eqb_spec (value mod factor) 0) as [Hmod|Hmod].
    + assert (Hdiv : 0 < value / factor).
      { assert (Hle : factor <= value).
        { pose proof (Z_div_mod_eq_full value factor) as Hde.
          rewrite Hmod, Z.add_0_r in Hde.
          assert (Hq : 0 <= value / factor) by (apply Z.div_pos; lia).
          destruct (Z.eq_dec (value / factor) 0) as [Hz|Hnz].
          - rewrite Hz in Hde; lia.
          - nia. }
        apply Z.div_str_pos; lia. }
      destruct (IH factor (value / factor) Hf Hdiv)
        as [count [rest [Heq [Hval [Hrest Hstop]]]]].
      exists (S count), rest.
      rewrite Heq.
      split; [reflexivity|].
      split.
      * rewrite Nat2Z.inj_succ, Z.pow_succ_r by lia.
        rewrite <- Z.mul_assoc, <- Hval.
        pose proof (Z_div_mod_eq_full value factor) as Hde.
        rewrite Hmod, Z.add_0_r in Hde.
        lia.
      * split; [exact Hrest|].
        destruct Hstop as [Hnm|Hcnt]; [left; exact Hnm|right; lia].
    + exists O, value.
      split; [reflexivity|].
      split; [cbn [Z.of_nat]; rewrite Z.pow_0_r; lia|].
      split; [exact Hv|now left].
Qed.

Lemma positive_divisors_of_10 :
  forall g,
    0 < g ->
    g <= 10 ->
    (g | 10)%Z ->
    g = 1 \/ g = 2 \/ g = 5 \/ g = 10.
Proof.
  intros g Hg Hle [k Hk].
  assert (Hkpos : 0 < k) by nia.
  assert (H : g = 1 \/ g = 2 \/ g = 3 \/ g = 4 \/ g = 5 \/
              g = 6 \/ g = 7 \/ g = 8 \/ g = 9 \/ g = 10) by lia.
  destruct H as [H|[H|[H|[H|[H|[H|[H|[H|[H|H]]]]]]]]];
    try (left; exact H);
    try (right; left; exact H);
    try (right; right; left; exact H);
    try (right; right; right; exact H);
    rewrite H in Hk; lia.
Qed.

Lemma divides_pow10_no_2_5_is_1 :
  forall (d s : Z),
    0 <= s ->
    0 < d ->
    (d | 10 ^ s)%Z ->
    d mod 2 <> 0 ->
    d mod 5 <> 0 ->
    d = 1.
Proof.
  intros d s Hs Hd Hdiv H2 H5.
  revert d Hd Hdiv H2 H5.
  refine ((natlike_ind
    (fun s => forall d, 0 < d -> (d | 10 ^ s)%Z ->
      d mod 2 <> 0 -> d mod 5 <> 0 -> d = 1) _ _ s Hs)); clear s Hs.
  - intros d Hd Hdiv H2 H5.
    cbn in Hdiv.
    destruct Hdiv as [k Hk].
    pose proof Hk as Hk'.
    apply eq_sym, Z.mul_eq_1 in Hk.
    destruct Hk as [Hk | Hk]; rewrite Hk in Hk'; lia.
  - intros s Hs IH d Hd Hdiv H2 H5.
    assert (Hdiv' : (d | 10 * 10 ^ s)%Z).
    { rewrite <- (Z.pow_succ_r 10 s Hs). exact Hdiv. }
    clear Hdiv.
    assert (Hgcd : Z.gcd d 10 = 1).
    { assert (Hdiv10 : (Z.gcd d 10 | 10)%Z) by apply Z.gcd_divide_r.
      assert (Hdivd : (Z.gcd d 10 | d)%Z) by apply Z.gcd_divide_l.
      pose proof (Z.gcd_nonneg d 10) as Hg0.
      destruct (Z.eq_dec (Z.gcd d 10) 0) as [Hz|Hnz].
      { destruct Hdiv10 as [k Hk]; rewrite Hz in Hk; lia. }
      assert (Hgpos : 0 < Z.gcd d 10) by lia.
      assert (Hgle : Z.gcd d 10 <= 10).
      { destruct Hdiv10 as [k Hk]; assert (0 < k) by nia; nia. }
      assert (H2d : ~(2 | d)%Z).
      { intro H. apply Z.mod_divide in H; [|lia]. congruence. }
      assert (H5d : ~(5 | d)%Z).
      { intro H. apply Z.mod_divide in H; [|lia]. congruence. }
      pose proof (positive_divisors_of_10 (Z.gcd d 10) Hgpos Hgle Hdiv10) as Hc.
      destruct Hc as [H|[H|[H|H]]]; try exact H.
      - exfalso; apply H2d; rewrite <- H; exact Hdivd.
      - exfalso; apply H5d; rewrite <- H; exact Hdivd.
      - exfalso; apply H2d.
        apply Z.divide_trans with (m := 10).
        + exists 5; lia.
        + rewrite <- H; exact Hdivd. }
    assert (Hdiv10s : (d | 10 ^ s)%Z).
    { eapply Z.gauss; [exact Hdiv'|exact Hgcd]. }
    exact (IH d Hd Hdiv10s H2 H5).
Qed.

Lemma inject_Z_div_pow10 :
  forall (c s : Z),
    0 <= s ->
    inject_Z c / inject_Z (10 ^ s) == c # Z.to_pos (10 ^ s).
Proof.
  intros c s Hs.
  assert (Hpos : 0 < 10 ^ s) by (apply Z.pow_pos_nonneg; lia).
  unfold Qdiv, inject_Z, Qinv.
  cbn.
  destruct (10 ^ s) as [|p|p] eqn:Heq; try lia.
  replace (Z.to_pos (10 ^ s)) with p by (rewrite Heq; reflexivity).
  unfold Qeq; cbn.
  lia.
Qed.

Lemma Qred_Qmake_den_divides :
  forall (c n : Z),
    0 < n ->
    (Z.pos (Qden (Qred (c # Z.to_pos n))) | n)%Z.
Proof.
  intros c n Hn.
  unfold Qred.
  rewrite Z2Pos.id by lia.
  cbn [Qnum Qden].
  pose proof (Z.ggcd_gcd c n) as Hgcd.
  pose proof (Z.ggcd_correct_divisors c n) as Hd.
  destruct (Z.ggcd c n) as [g [c' n']].
  cbn in *.
  destruct Hd as [Hc Hn'].
  assert (Hgpos : 0 < g).
  { rewrite Hgcd.
    pose proof (Z.gcd_nonneg c n) as Hg0.
    pose proof (Z.gcd_divide_r c n) as [k Hk].
    destruct (Z.eq_dec (Z.gcd c n) 0) as [Hz|Hnz]; [|lia].
    rewrite Hz in Hk. lia. }
  assert (Hn'pos : 0 < n') by nia.
  rewrite Z2Pos.id by lia.
  exists g. lia.
Qed.

Lemma Qred_inject_Z : forall z, Qred (inject_Z z) = inject_Z z.
Proof.
  intro z. apply Qred_iff. unfold inject_Z. cbn. apply Z.gcd_1_r.
Qed.

Lemma numeric_of_scaled_finite_this :
  forall (c s : Z),
    0 <= s ->
    exists q,
      numeric_of_scaled c s = NumericFinite q /\
      this q == (inject_Z c / inject_Z (10 ^ s)).
Proof.
  intros c s Hs.
  unfold numeric_of_scaled.
  rewrite (proj2 (Z.leb_le 0 s) Hs).
  eexists; split; [reflexivity|].
  unfold Qcdiv, Qcmult, Qcinv.
  cbn [this Q2Qc].
  rewrite !Qred_inject_Z.
  change (Qred (inject_Z c * Qred (/ inject_Z (10 ^ s))) ==
          inject_Z c * / inject_Z (10 ^ s)).
  eapply Qeq_trans; [apply Qred_correct|].
  apply Qmult_comp; [reflexivity|].
  apply Qred_correct.
Qed.

(** * Pos.size / fuel for decimal_parts *)

Lemma size_to_nat :
  forall p, Pos.size_nat p = Pos.to_nat (Pos.size p).
Proof.
  induction p as [p IH|p IH|]; simpl; try rewrite IH; lia.
Qed.

Lemma Zpos_lt_pow2_size_nat :
  forall p : positive,
    Z.pos p < 2 ^ Z.of_nat (Pos.size_nat p).
Proof.
  intro p.
  pose proof (Pos.size_gt p) as H.
  apply Pos2Z.pos_lt_pos in H.
  rewrite size_to_nat.
  rewrite Pos2Z.inj_pow in H.
  change (Z.pos 2) with 2%Z in H.
  replace (Z.pos (Pos.size p)) with (Z.of_nat (Pos.to_nat (Pos.size p))) in H.
  2:{ symmetry. apply Zpos_eq_Z_of_nat_o_nat_of_P. }
  exact H.
Qed.

Lemma numeric_of_scaled_this_Qred :
  forall (c s : Z),
    0 <= s ->
    exists q,
      numeric_of_scaled c s = NumericFinite q /\
      this q = Qred (c # Z.to_pos (10 ^ s)).
Proof.
  intros c s Hs.
  unfold numeric_of_scaled.
  rewrite (proj2 (Z.leb_le 0 s) Hs).
  eexists; split; [reflexivity|].
  unfold Qcdiv, Qcmult, Qcinv.
  cbn [this Q2Qc].
  rewrite !Qred_inject_Z.
  apply Qred_complete.
  eapply Qeq_trans.
  { apply Qmult_comp; [reflexivity|apply Qred_correct]. }
  change (inject_Z c * / inject_Z (10 ^ s) == c # Z.to_pos (10 ^ s)).
  rewrite <- inject_Z_div_pow10 by exact Hs.
  unfold Qdiv. reflexivity.
Qed.

Lemma Z_pow2_le_pow5 :
  forall n, 0 <= n -> 2 ^ n <= 5 ^ n.
Proof.
  intros n Hn. apply Z.pow_le_mono_l; lia.
Qed.

Lemma numeric_remove_factor_2_fuel_enough :
  forall (p : positive),
    let den := Z.pos p in
    let fuel := S (Pos.size_nat p) in
    exists (count : nat) (rest : Z),
      numeric_remove_factor fuel 2 den = (count, rest) /\
      den = 2 ^ Z.of_nat count * rest /\
      0 < rest /\
      rest mod 2 <> 0.
Proof.
  intro p.
  set (den := Z.pos p).
  set (fuel := S (Pos.size_nat p)).
  assert (Hd : 0 < den) by (unfold den; lia).
  destruct (numeric_remove_factor_spec fuel 2 den ltac:(lia) Hd)
    as [count [rest [Heq [Hval [Hrest Hstop]]]]].
  exists count, rest.
  split; [exact Heq|]. split; [exact Hval|]. split; [exact Hrest|].
  destruct Hstop as [Hnm|Hcnt]; [exact Hnm|].
  intro Hmod.
  assert (Hrest_ge : 2 <= rest).
  { pose proof (Z_div_mod_eq_full rest 2) as Hde.
    rewrite Hmod, Z.add_0_r in Hde.
    assert (Hq : 0 <= rest / 2) by (apply Z.div_pos; lia).
    destruct (Z.eq_dec (rest / 2) 0) as [Hz|Hnz].
    - rewrite Hz in Hde; lia.
    - nia. }
  assert (Hden_ge : 2 ^ Z.of_nat (S fuel) <= den).
  { rewrite Hval, Hcnt. rewrite Nat2Z.inj_succ, Z.pow_succ_r by lia. nia. }
  assert (Hden_lt : den < 2 ^ Z.of_nat (Pos.size_nat p)).
  { unfold den. apply Zpos_lt_pow2_size_nat. }
  replace (Z.of_nat (Pos.size_nat p)) with (Z.of_nat fuel - 1) in Hden_lt by lia.
  assert (Hlt_pow : 2 ^ (Z.of_nat fuel - 1) < 2 ^ Z.of_nat (S fuel))
    by (apply Z.pow_lt_mono_r; lia).
  lia.
Qed.

Lemma numeric_remove_factor_5_fuel_enough :
  forall (p : positive) (after_twos : Z),
    0 < after_twos -> after_twos <= Z.pos p ->
    let fuel := S (Pos.size_nat p) in
    exists (count : nat) (rest : Z),
      numeric_remove_factor fuel 5 after_twos = (count, rest) /\
      after_twos = 5 ^ Z.of_nat count * rest /\
      0 < rest /\ rest mod 5 <> 0.
Proof.
  intros p after_twos Hat Hle.
  set (fuel := S (Pos.size_nat p)).
  destruct (numeric_remove_factor_spec fuel 5 after_twos ltac:(lia) Hat)
    as [count [rest [Heq [Hval [Hrest Hstop]]]]].
  exists count, rest.
  split; [exact Heq|]. split; [exact Hval|]. split; [exact Hrest|].
  destruct Hstop as [Hnm|Hcnt]; [exact Hnm|].
  intro Hmod.
  assert (Hrest_ge : 5 <= rest).
  { pose proof (Z_div_mod_eq_full rest 5) as Hde.
    rewrite Hmod, Z.add_0_r in Hde.
    assert (Hq : 0 <= rest / 5) by (apply Z.div_pos; lia).
    destruct (Z.eq_dec (rest / 5) 0) as [Hz|Hnz].
    - rewrite Hz in Hde; lia.
    - nia. }
  assert (Hat_ge : 5 ^ Z.of_nat (S fuel) <= after_twos).
  { rewrite Hval, Hcnt. rewrite Nat2Z.inj_succ, Z.pow_succ_r by lia. nia. }
  assert (Hp_lt : Z.pos p < 2 ^ Z.of_nat (Pos.size_nat p)) by apply Zpos_lt_pow2_size_nat.
  assert (Hat_lt : after_twos < 5 ^ Z.of_nat (Pos.size_nat p)).
  { assert (H2 : after_twos < 2 ^ Z.of_nat (Pos.size_nat p)) by lia.
    assert (H25 : 2 ^ Z.of_nat (Pos.size_nat p) <= 5 ^ Z.of_nat (Pos.size_nat p))
      by (apply Z_pow2_le_pow5; lia).
    lia. }
  replace (Z.of_nat (Pos.size_nat p)) with (Z.of_nat fuel - 1) in Hat_lt by lia.
  assert (Hlt_pow : 5 ^ (Z.of_nat fuel - 1) < 5 ^ Z.of_nat (S fuel))
    by (apply Z.pow_lt_mono_r; lia).
  lia.
Qed.

Lemma odd_of_mul_pow5 :
  forall (k rest : Z),
    (5 ^ k * rest) mod 2 <> 0 -> rest mod 2 <> 0.
Proof.
  intros k rest H Hmod.
  apply H.
  rewrite <- Z.mul_mod_idemp_r by lia.
  rewrite Hmod, Z.mul_0_r, Z.mod_0_l; lia.
Qed.

Lemma pow5_mod2_neq0 :
  forall s, 0 <= s -> (5 ^ s) mod 2 <> 0.
Proof.
  apply (natlike_ind (fun s => (5 ^ s) mod 2 <> 0)).
  - cbn. lia.
  - intros s Hs IH.
    rewrite Z.pow_succ_r by lia.
    replace (5 * 5 ^ s) with (5 ^ s * 5) by lia.
    rewrite Z.mul_mod by lia.
    replace (5 mod 2) with 1 by reflexivity.
    rewrite Z.mul_1_r, Z.mod_mod by lia.
    exact IH.
Qed.

Lemma pow2_mod5_cycle :
  forall s, 0 <= s ->
    (2 ^ s) mod 5 = 1 \/ (2 ^ s) mod 5 = 2 \/
    (2 ^ s) mod 5 = 3 \/ (2 ^ s) mod 5 = 4.
Proof.
  apply (natlike_ind (fun s =>
    (2 ^ s) mod 5 = 1 \/ (2 ^ s) mod 5 = 2 \/
    (2 ^ s) mod 5 = 3 \/ (2 ^ s) mod 5 = 4)).
  - cbn. lia.
  - intros s Hs [H|[H|[H|H]]];
      rewrite Z.pow_succ_r by lia;
      rewrite Z.mul_mod by lia;
      rewrite H; cbn; lia.
Qed.

Lemma pow2_mod5_neq0 :
  forall s, 0 <= s -> (2 ^ s) mod 5 <> 0.
Proof.
  intros s Hs.
  destruct (pow2_mod5_cycle s Hs) as [H|[H|[H|H]]]; rewrite H; lia.
Qed.

Lemma pow2_pow5_divides_pow10_bounds :
  forall twos fives s,
    0 <= s -> 0 <= twos -> 0 <= fives ->
    (2 ^ twos * 5 ^ fives | 10 ^ s)%Z ->
    twos <= s /\ fives <= s.
Proof.
  intros twos fives s Hs Ht Hf [k Hk].
  assert (H10 : 10 ^ s = 2 ^ s * 5 ^ s).
  { replace 10 with (2 * 5) by lia. rewrite Z.pow_mul_l. reflexivity. }
  rewrite H10 in Hk.
  split.
  - destruct (Z.le_gt_cases twos s) as [Hle|Hgt]; [exact Hle|].
    assert (Heven : (2 ^ (twos - s) * 5 ^ fives * k = 5 ^ s)).
    { assert (Htwos : twos = s + (twos - s)) by lia.
      rewrite Htwos, Z.pow_add_r in Hk by lia.
      assert (H2s_pos : 2 ^ s <> 0) by (apply Z.pow_nonzero; lia).
      apply (Z.mul_reg_l _ _ (2 ^ s)); [exact H2s_pos|]. lia. }
    assert (Hlhs_even : (2 ^ (twos - s) * 5 ^ fives * k) mod 2 = 0).
    { assert (Hge : 1 <= twos - s) by lia.
      assert (Hpow : 2 ^ (twos - s) = 2 * 2 ^ (twos - s - 1)).
      { rewrite <- (Z.pow_succ_r 2 (twos - s - 1)) by lia.
        f_equal; lia. }
      rewrite Hpow.
      replace (2 * 2 ^ (twos - s - 1) * 5 ^ fives * k)
        with ((2 ^ (twos - s - 1) * 5 ^ fives * k) * 2) by lia.
      apply Z_mod_mult. }
    pose proof (pow5_mod2_neq0 s Hs) as Hrhs_odd.
    rewrite Heven in Hlhs_even. congruence.
  - destruct (Z.le_gt_cases fives s) as [Hle|Hgt]; [exact Hle|].
    assert (Heq : 2 ^ twos * 5 ^ (fives - s) * k = 2 ^ s).
    { assert (Hfives : fives = s + (fives - s)) by lia.
      rewrite Hfives, Z.pow_add_r in Hk by lia.
      assert (H5s_pos : 5 ^ s <> 0) by (apply Z.pow_nonzero; lia).
      apply (Z.mul_reg_l _ _ (5 ^ s)); [exact H5s_pos|]. lia. }
    assert (Hlhs5 : (2 ^ twos * 5 ^ (fives - s) * k) mod 5 = 0).
    { assert (Hge : 1 <= fives - s) by lia.
      assert (Hpow : 5 ^ (fives - s) = 5 * 5 ^ (fives - s - 1)).
      { rewrite <- (Z.pow_succ_r 5 (fives - s - 1)) by lia.
        f_equal; lia. }
      rewrite Hpow.
      replace (2 ^ twos * (5 * 5 ^ (fives - s - 1)) * k)
        with ((2 ^ twos * 5 ^ (fives - s - 1) * k) * 5) by lia.
      apply Z_mod_mult. }
    pose proof (pow2_mod5_neq0 s Hs) as Hrhs5.
    rewrite Heq in Hlhs5. congruence.
Qed.

Lemma numeric_digit_count_fuel_pos :
  forall fuel w, 1 <= numeric_digit_count_fuel fuel w.
Proof.
  induction fuel as [|fuel IH]; intros w; [cbn; lia|].
  cbn [numeric_digit_count_fuel].
  destruct (Z.ltb_spec0 w 10); [lia|].
  pose proof (IH (w / 10)). lia.
Qed.

Lemma numeric_digit_count_fuel_mono_val :
  forall fuel w1 w2,
    0 <= w1 <= w2 ->
    numeric_digit_count_fuel fuel w1 <= numeric_digit_count_fuel fuel w2.
Proof.
  induction fuel as [|fuel IH]; intros w1 w2 Hw; [cbn; lia|].
  cbn [numeric_digit_count_fuel].
  destruct (Z.ltb_spec0 w1 10) as [H1|H1];
  destruct (Z.ltb_spec0 w2 10) as [H2|H2].
  - lia.
  - pose proof (numeric_digit_count_fuel_pos fuel (w2 / 10)). lia.
  - lia.
  - assert (H10pos : (0 < 10)%Z) by reflexivity.
    assert (Hd : 0 <= w1 / 10 <= w2 / 10).
    { split.
      - apply (Z.div_pos w1 10); [lia|exact H10pos].
      - apply (Z.div_le_mono w1 w2 10); [exact H10pos|lia]. }
    pose proof (IH (w1 / 10) (w2 / 10) Hd) as IHw.
    lia.
Qed.

Lemma numeric_digit_count_fuel_mono_fuel :
  forall fuel1 fuel2 w,
    0 <= w ->
    (fuel1 <= fuel2)%nat ->
    numeric_digit_count_fuel fuel1 w <= numeric_digit_count_fuel fuel2 w.
Proof.
  intros fuel1 fuel2.
  revert fuel1.
  induction fuel2 as [|fuel2 IH]; intros fuel1 w Hw Hle.
  - assert (fuel1 = 0%nat) by lia. subst. lia.
  - destruct fuel1 as [|fuel1].
    + pose proof (numeric_digit_count_fuel_pos (S fuel2) w). cbn in *. lia.
    + cbn [numeric_digit_count_fuel].
      destruct (Z.ltb_spec0 w 10); [lia|].
      assert (H10pos : (0 < 10)%Z) by reflexivity.
      assert (Hw' : 0 <= w / 10) by (apply (Z.div_pos w 10); [lia|exact H10pos]).
      pose proof (IH fuel1 (w / 10) Hw' ltac:(lia)) as IHf.
      apply Z.add_le_mono_l. exact IHf.
Qed.

Lemma numeric_digit_count_mono_nonneg :
  forall a b,
    0 <= a <= b ->
    numeric_digit_count a <= numeric_digit_count b.
Proof.
  intros a b Hab.
  unfold numeric_digit_count.
  rewrite !Z.abs_eq by lia.
  set (fa := S (Z.to_nat (Z.log2 (a + 1)))).
  set (fb := S (Z.to_nat (Z.log2 (b + 1)))).
  assert (Hfuel : (fa <= fb)%nat).
  { unfold fa, fb. apply le_n_S, Nat2Z.inj_le.
    rewrite !Z2Nat.id by apply Z.log2_nonneg.
    apply Z.log2_le_mono; lia. }
  eapply Z.le_trans.
  - apply numeric_digit_count_fuel_mono_fuel with (fuel2 := fb); [lia|exact Hfuel].
  - apply numeric_digit_count_fuel_mono_val; lia.
Qed.

(** * digit_count vs powers of 10 *)

Lemma log2_div10_fuel :
  forall z, 10 <= z ->
    (S (Z.to_nat (Z.log2 (z / 10 + 1))) <= Z.to_nat (Z.log2 (z + 1)))%nat.
Proof.
  intros z Hz.
  apply Nat2Z.inj_le.
  rewrite Nat2Z.inj_succ.
  rewrite !Z2Nat.id by apply Z.log2_nonneg.
  assert (Hpos_z1 : 0 < z + 1) by lia.
  assert (Hpos_d : 0 < z / 10 + 1).
  { assert (0 <= z / 10) by (apply Z.div_pos; lia). lia. }
  (* goal: Z.succ (log2 (z/10+1)) <= log2 (z+1) *)
  apply (proj1 (Z.log2_le_pow2 (z + 1) (Z.succ (Z.log2 (z / 10 + 1))) Hpos_z1)).
  rewrite Z.pow_succ_r by (pose proof (Z.log2_nonneg (z / 10 + 1)); lia).
  pose proof (Z.log2_spec (z / 10 + 1) Hpos_d) as [Hlo _].
  assert (Hmul : 2 * 2 ^ Z.log2 (z / 10 + 1) <= 2 * (z / 10 + 1)) by nia.
  eapply Z.le_trans; [exact Hmul|].
  (* 2 * (z/10 + 1) = 2*(z/10) + 2 <= z + 2? Need <= z+1, so need 2*(z/10)+2 <= z+1
     i.e. 2*(z/10) + 1 <= z. From z = 10*q+r, 2q+1 <= 10q+r. For q >= 1 (since z>=10), OK if r >= 0.
     2q+1 <= 10q + r <=> 1 <= 8q + r. True for q >= 1. *)
  pose proof (Z_div_mod_eq_full z 10) as Hde.
  pose proof (Z.mod_pos_bound z 10 ltac:(lia)) as Hm.
  assert (Hq : 1 <= z / 10).
  { apply Z.div_le_lower_bound; lia. }
  nia.
Qed.

Lemma numeric_digit_count_ge_succ_div10 :
  forall z,
    10 <= z ->
    1 + numeric_digit_count (z / 10) <= numeric_digit_count z.
Proof.
  intros z Hz.
  assert (H10pos : (0 < 10)%Z) by reflexivity.
  assert (Hz10 : 0 <= z / 10) by (apply (Z.div_pos z 10); [lia|exact H10pos]).
  unfold numeric_digit_count.
  rewrite (Z.abs_eq z) by lia.
  rewrite (Z.abs_eq (z / 10)) by exact Hz10.
  assert (Hstep :
    numeric_digit_count_fuel (S (Z.to_nat (Z.log2 (z + 1)))) z =
    1 + numeric_digit_count_fuel (Z.to_nat (Z.log2 (z + 1))) (z / 10)).
  { cbn [numeric_digit_count_fuel].
    destruct (Z.ltb_spec0 z 10); [lia|]. reflexivity. }
  rewrite Hstep.
  apply Z.add_le_mono_l.
  apply numeric_digit_count_fuel_mono_fuel; [exact Hz10|].
  apply log2_div10_fuel; exact Hz.
Qed.

Lemma numeric_digit_count_abs_lt_pow :
  forall z,
    0 < z ->
    z < 10 ^ (numeric_digit_count z).
Proof.
  intros z Hz.
  induction z as [z IH] using (well_founded_induction (Zwf_well_founded 0)).
  destruct (Z.ltb_spec0 z 10) as [Hsmall|Hbig].
  - unfold numeric_digit_count.
    rewrite Z.abs_eq by lia.
    assert (Hdig : numeric_digit_count_fuel (S (Z.to_nat (Z.log2 (z + 1)))) z = 1).
    { cbn [numeric_digit_count_fuel].
      destruct (Z.ltb_spec0 z 10); [reflexivity|lia]. }
    rewrite Hdig.
    replace (10 ^ 1) with 10 by reflexivity.
    lia.
  - assert (Hz10pos : 0 < z / 10).
    { assert (H10pos : (0 < 10)%Z) by reflexivity.
      apply Z.div_str_pos; lia. }
    assert (Hwf : Zwf 0 (z / 10) z).
    { unfold Zwf. split; [lia|].
      apply Z.div_lt; lia. }
    pose proof (IH (z / 10) Hwf Hz10pos) as IH'.
    pose proof (numeric_digit_count_ge_succ_div10 z ltac:(lia)) as Hge.
    assert (Hdpos : 0 <= numeric_digit_count (z / 10)).
    { pose proof (numeric_digit_count_fuel_pos
        (S (Z.to_nat (Z.log2 (Z.abs (z / 10) + 1)))) (Z.abs (z / 10))).
      unfold numeric_digit_count. lia. }
    assert (Hpow : 10 * 10 ^ (numeric_digit_count (z / 10)) <=
                   10 ^ (numeric_digit_count z)).
    { replace (10 * 10 ^ (numeric_digit_count (z / 10)))
        with (10 ^ (1 + numeric_digit_count (z / 10))).
      2:{ rewrite Z.pow_add_r by lia.
          replace (10 ^ 1) with 10 by reflexivity.
          lia. }
      apply Z.pow_le_mono_r; lia. }
    assert (Hzbound : z <= 10 * (z / 10) + 9).
    { pose proof (Z_div_mod_eq_full z 10) as Hde.
      pose proof (Z.mod_pos_bound z 10 ltac:(lia)) as Hm.
      nia. }
    assert (Hdiv_le : z / 10 <= 10 ^ (numeric_digit_count (z / 10)) - 1) by lia.
    nia.
Qed.

Lemma numeric_digit_count_pos :
  forall z, 1 <= numeric_digit_count z.
Proof.
  intro z. unfold numeric_digit_count. apply numeric_digit_count_fuel_pos.
Qed.

Lemma numeric_digit_count_ge_of_pow :
  forall z m,
    0 <= m ->
    10 ^ m <= z ->
    m + 1 <= numeric_digit_count z.
Proof.
  intros z m Hm.
  revert z.
  refine (natlike_ind (fun m => forall z, 10 ^ m <= z -> m + 1 <= numeric_digit_count z) _ _ m Hm).
  - intros z Hz. cbn in Hz. pose proof (numeric_digit_count_pos z). lia.
  - intros m' Hm' IH z Hz.
    assert (H10 : 10 <= z).
    { assert (10 ^ 1 <= 10 ^ (Z.succ m')).
      { apply Z.pow_le_mono_r; lia. }
      change (10 ^ 1) with 10 in H.
      lia. }
    assert (Hdiv : 10 ^ m' <= z / 10).
    { assert (H10pos : (0 < 10)%Z) by reflexivity.
      apply Z.div_le_lower_bound; [exact H10pos|].
      eapply Z.le_trans; [|exact Hz].
      rewrite <- (Z.pow_succ_r 10 m' Hm').
      replace (Z.succ m') with (m' + 1) by lia.
      rewrite Z.pow_add_r by lia.
      replace (10 ^ 1) with 10 by reflexivity.
      lia. }
    pose proof (IH (z / 10) Hdiv) as IH'.
    pose proof (numeric_digit_count_ge_succ_div10 z H10) as Hge.
    lia.
Qed.

Lemma numeric_digit_count_le_implies_lt_pow :
  forall z n,
    0 <= z ->
    1 <= n ->
    numeric_digit_count z <= n ->
    z < 10 ^ n.
Proof.
  intros z n Hz Hn Hdig.
  destruct (Z.lt_ge_cases z (10 ^ n)) as [Hlt|Hge]; [exact Hlt|].
  pose proof (numeric_digit_count_ge_of_pow z n ltac:(lia) Hge).
  lia.
Qed.


Lemma numeric_digit_count_mono_abs :
  forall a b,
    Z.abs a <= Z.abs b ->
    numeric_digit_count a <= numeric_digit_count b.
Proof.
  intros a b H.
  replace (numeric_digit_count a) with (numeric_digit_count (Z.abs a)).
  2:{ unfold numeric_digit_count. rewrite Z.abs_idemp. reflexivity. }
  replace (numeric_digit_count b) with (numeric_digit_count (Z.abs b)).
  2:{ unfold numeric_digit_count. rewrite Z.abs_idemp. reflexivity. }
  apply numeric_digit_count_mono_nonneg.
  pose proof (Z.abs_nonneg a); pose proof (Z.abs_nonneg b); lia.
Qed.

Lemma numeric_of_scaled_decimal_parts_bounds :
  forall (c s : Z),
    0 <= s ->
    exists coeff scale,
      numeric_decimal_parts (numeric_of_scaled c s) = Some (coeff, scale) /\
      0 <= scale <= s /\
      Z.abs coeff <= Z.abs c /\
      coeff * 10 ^ s = c * 10 ^ scale.
Proof.
  intros c s Hs.
  destruct (numeric_of_scaled_this_Qred c s Hs) as [q [Hn Heq]].
  rewrite Hn.
  unfold numeric_decimal_parts.
  rewrite Heq.
  set (p := Qden (Qred (c # Z.to_pos (10 ^ s)))).
  pose proof (numeric_remove_factor_2_fuel_enough p) as H2ex.
  cbn zeta in H2ex.
  destruct H2ex as [twos [after_twos [H2 [H2val [H2rest H2mod]]]]].
  rewrite H2.
  assert (Hden_div : (Z.pos p | 10 ^ s)%Z).
  { unfold p. apply Qred_Qmake_den_divides. apply Z.pow_pos_nonneg; lia. }
  assert (Hafter_le : after_twos <= Z.pos p).
  { rewrite H2val.
    assert (0 < 2 ^ Z.of_nat twos) by (apply Z.pow_pos_nonneg; lia). nia. }
  assert (Hafter_div : (after_twos | 10 ^ s)%Z).
  { destruct Hden_div as [k Hk]. rewrite H2val in Hk.
    exists (k * 2 ^ Z.of_nat twos). lia. }
  pose proof (numeric_remove_factor_5_fuel_enough p after_twos H2rest Hafter_le) as H5ex.
  cbn zeta in H5ex.
  destruct H5ex as [fives [rest [H5 [H5val [H5rest H5mod]]]]].
  rewrite H5.
  assert (Hrest_div : (rest | 10 ^ s)%Z).
  { destruct Hafter_div as [k Hk]. rewrite H5val in Hk.
    exists (k * 5 ^ Z.of_nat fives). lia. }
  assert (Hrest_odd : rest mod 2 <> 0).
  { apply (odd_of_mul_pow5 (Z.of_nat fives) rest).
    rewrite <- H5val. exact H2mod. }
  assert (Hrest1 : rest = 1).
  { apply (divides_pow10_no_2_5_is_1 rest s Hs H5rest Hrest_div Hrest_odd H5mod). }
  rewrite Hrest1, Z.eqb_refl.
  set (twos_z := Z.of_nat twos).
  set (fives_z := Z.of_nat fives).
  set (scale := Z.max twos_z fives_z).
  set (qnum := Qnum (Qred (c # Z.to_pos (10 ^ s)))).
  set (coeff := qnum * 2 ^ (scale - twos_z) * 5 ^ (scale - fives_z)).
  exists coeff, scale.
  split; [reflexivity|].
  assert (Hden_fact : Z.pos p = 2 ^ twos_z * 5 ^ fives_z).
  { unfold twos_z, fives_z. rewrite H2val, H5val, Hrest1. lia. }
  assert (Hden_div' : (2 ^ twos_z * 5 ^ fives_z | 10 ^ s)%Z).
  { rewrite <- Hden_fact. exact Hden_div. }
  assert (Hbounds : twos_z <= s /\ fives_z <= s).
  { apply pow2_pow5_divides_pow10_bounds; try lia; exact Hden_div'. }
  assert (Hscale : 0 <= scale <= s).
  { unfold scale. destruct Hbounds. lia. }
  split; [exact Hscale|].
  assert (Hqeq : qnum * 10 ^ s = c * Z.pos p).
  { unfold qnum, p.
    assert (Hpos : 0 < 10 ^ s) by (apply Z.pow_pos_nonneg; lia).
    set (qr := Qred (c # Z.to_pos (10 ^ s))).
    assert (Hr0 : qr == (c # Z.to_pos (10 ^ s))) by (unfold qr; apply Qred_correct).
    assert (Hr : Qnum qr * Z.pos (Qden (c # Z.to_pos (10 ^ s))) =
                 c * Z.pos (Qden qr)).
    { unfold Qeq in Hr0.
      change (Qnum qr * Z.pos (Qden (c # Z.to_pos (10 ^ s))) =
              Qnum (c # Z.to_pos (10 ^ s)) * Z.pos (Qden qr)) in Hr0.
      cbn [Qnum Qden] in Hr0.
      exact Hr0. }
    cbn [Qden] in Hr.
    rewrite Z2Pos.id in Hr by lia.
    unfold qr in Hr.
    exact Hr. }
  assert (Hcoeff_eq : coeff * 10 ^ s = c * 10 ^ scale).
  { unfold coeff.
    assert (H10sc : 10 ^ scale = 2 ^ scale * 5 ^ scale).
    { replace 10 with (2 * 5) by lia. rewrite Z.pow_mul_l. reflexivity. }
    rewrite H10sc.
    assert (Htw : 0 <= scale - twos_z) by lia.
    assert (Hfv : 0 <= scale - fives_z) by lia.
    assert (Hrearr :
      qnum * 2 ^ (scale - twos_z) * 5 ^ (scale - fives_z) * 10 ^ s =
      (qnum * 10 ^ s) * (2 ^ (scale - twos_z) * 5 ^ (scale - fives_z))) by ring.
    rewrite Hrearr, Hqeq, Hden_fact.
    replace (twos_z + (scale - twos_z)) with scale by lia.
    replace (fives_z + (scale - fives_z)) with scale by lia.
    assert (H2split : 2 ^ scale = 2 ^ twos_z * 2 ^ (scale - twos_z)).
    { rewrite <- Z.pow_add_r by lia. f_equal; lia. }
    assert (H5split : 5 ^ scale = 5 ^ fives_z * 5 ^ (scale - fives_z)).
    { rewrite <- Z.pow_add_r by lia. f_equal; lia. }
    rewrite H2split, H5split.
    ring. }
  split; [|exact Hcoeff_eq].
  assert (Habs : Z.abs coeff * 10 ^ s = Z.abs c * 10 ^ scale).
  { apply (f_equal Z.abs) in Hcoeff_eq.
    rewrite !Z.abs_mul in Hcoeff_eq.
    rewrite (Z.abs_eq (10 ^ s)) in Hcoeff_eq by (apply Z.pow_nonneg; lia).
    rewrite (Z.abs_eq (10 ^ scale)) in Hcoeff_eq by (apply Z.pow_nonneg; lia).
    exact Hcoeff_eq. }
  assert (Hpow_le : 10 ^ scale <= 10 ^ s) by (apply Z.pow_le_mono_r; lia).
  assert (Hpos10 : 0 < 10 ^ s) by (apply Z.pow_pos_nonneg; lia).
  assert (Hle : Z.abs coeff * 10 ^ s <= Z.abs c * 10 ^ s).
  { rewrite Habs.
    apply Z.mul_le_mono_nonneg_l; [apply Z.abs_nonneg|exact Hpow_le]. }
  apply (proj2 (Z.mul_le_mono_pos_r (Z.abs coeff) (Z.abs c) (10 ^ s) Hpos10)).
  exact Hle.
Qed.

Lemma numeric_of_scaled_decimal_parts_some :
  forall (c s : Z),
    0 <= s ->
    exists coeff scale,
      numeric_decimal_parts (numeric_of_scaled c s) = Some (coeff, scale).
Proof.
  intros c s Hs.
  destruct (numeric_of_scaled_decimal_parts_bounds c s Hs) as [coeff [scale [Hp _]]].
  exists coeff, scale. exact Hp.
Qed.

Lemma numeric_integer_digit_count_le_digit_count :
  forall coeff scale,
    0 <= scale ->
    numeric_integer_digit_count coeff scale <= numeric_digit_count coeff.
Proof.
  intros coeff scale Hs.
  unfold numeric_integer_digit_count.
  destruct (Z.eqb_spec coeff 0) as [Hz|Hnz].
  - subst. pose proof (numeric_digit_count_pos 0). lia.
  - pose proof (numeric_digit_count_pos coeff) as Hp.
    destruct (Z.max_spec_le 0 (numeric_digit_count coeff - scale)) as [Hm|Hm].
    + lia.
    + lia.
Qed.

Lemma numeric_of_scaled_runtime_fits_of_digit_bound :
  forall c s,
    0 <= s <= postgres_numeric_max_fractional_digits ->
    numeric_digit_count c <= postgres_numeric_max_integer_digits ->
    numeric_runtime_fits_bool (numeric_of_scaled c s) = true.
Proof.
  intros c s Hs Hd.
  destruct Hs as [Hs0 Hs1].
  destruct (numeric_of_scaled_decimal_parts_bounds c s Hs0)
    as [coeff [scale [Hp [Hsc [Habs _]]]]].
  eapply numeric_runtime_fits_from_decimal_parts; [exact Hp| |].
  - unfold numeric_display_scale_valid_bool.
    apply andb_true_intro; split; apply Z.leb_le; lia.
  - eapply Z.le_trans; [|exact Hd].
    eapply Z.le_trans.
    + apply numeric_integer_digit_count_le_digit_count; lia.
    + apply numeric_digit_count_mono_abs; exact Habs.
Qed.


Lemma numeric_of_scaled_coeff_nonneg :
  forall c s coeff scale,
    0 <= c ->
    0 <= s ->
    numeric_decimal_parts (numeric_of_scaled c s) = Some (coeff, scale) ->
    0 <= coeff /\ coeff * 10 ^ s = c * 10 ^ scale /\ 0 <= scale <= s.
Proof.
  intros c s coeff scale Hc Hs Hparts.
  destruct (numeric_of_scaled_decimal_parts_bounds c s Hs)
    as [coeff' [scale' [Hp [Hsc [Habs Heq]]]]].
  rewrite Hp in Hparts. injection Hparts as Hc0 Hs0; subst.
  split.
  - pose proof (Z.abs_eq coeff).
    destruct (Z.le_gt_cases 0 coeff) as [Hle|Hlt]; [exact Hle|].
    (* coeff < 0 contradicts Habs and Heq with c >= 0 *)
    assert (Habs' : Z.abs coeff <= c).
    { rewrite (Z.abs_eq c) in Habs by lia. exact Habs. }
    assert (Heq' : coeff * 10 ^ s = c * 10 ^ scale) by exact Heq.
    assert (Hrhs : 0 <= c * 10 ^ scale).
    { apply Z.mul_nonneg_nonneg; [lia|apply Z.pow_nonneg; lia]. }
    rewrite <- Heq' in Hrhs.
    assert (Hpos10 : 0 < 10 ^ s) by (apply Z.pow_pos_nonneg; lia).
    nia.
  - split; [exact Heq|exact Hsc].
Qed.

Lemma numeric_of_scaled_int_digits_of_lt_pow :
  forall c s n coeff scale,
    0 <= c ->
    0 <= s ->
    0 <= n ->
    numeric_decimal_parts (numeric_of_scaled c s) = Some (coeff, scale) ->
    c < 10 ^ (n + s) ->
    numeric_integer_digit_count coeff scale <= n.
Proof.
  intros c s n coeff scale Hc Hs Hn Hparts Hmag.
  destruct (numeric_of_scaled_coeff_nonneg c s coeff scale Hc Hs Hparts)
    as [Hcn [Heq Hsc]].
  unfold numeric_integer_digit_count.
  destruct (Z.eqb_spec coeff 0); [lia|].
  assert (Hpos10s : 0 < 10 ^ s) by (apply Z.pow_pos_nonneg; lia).
  assert (Hcoeff_lt : coeff < 10 ^ (n + scale)).
  { assert (Hprod : coeff * 10 ^ s < 10 ^ (n + scale) * 10 ^ s).
    { rewrite Heq.
      replace (10 ^ (n + scale) * 10 ^ s) with (10 ^ (n + s) * 10 ^ scale).
      2:{ rewrite !Z.pow_add_r by lia. ring. }
      apply (proj1 (Z.mul_lt_mono_pos_r (10 ^ scale) c (10 ^ (n + s))
        ltac:(apply Z.pow_pos_nonneg; lia))); exact Hmag. }
    apply (proj2 (Z.mul_lt_mono_pos_r (10 ^ s) coeff (10 ^ (n + scale)) Hpos10s)).
    exact Hprod. }
  destruct (Z.eq_dec (n + scale) 0) as [Hz|Hnz].
  - assert (Hn0 : n = 0) by lia. assert (Hs0 : scale = 0) by lia.
    subst. change (10 ^ 0) with 1 in Hcoeff_lt. lia.
  - assert (Hnp : 1 <= n + scale) by lia.
    assert (Hdig : numeric_digit_count coeff <= n + scale).
    { apply numeric_digit_count_lt_pow; [exact Hnp|].
      rewrite Z.abs_eq by lia. exact Hcoeff_lt. }
    lia.
Qed.

Lemma numeric_of_scaled_lt_pow_of_int_digits :
  forall c s n coeff scale,
    0 <= c ->
    0 <= s ->
    0 <= n ->
    numeric_decimal_parts (numeric_of_scaled c s) = Some (coeff, scale) ->
    numeric_integer_digit_count coeff scale <= n ->
    c < 10 ^ (n + s).
Proof.
  intros c s n coeff scale Hc Hs Hn Hparts Hdig.
  destruct (numeric_of_scaled_coeff_nonneg c s coeff scale Hc Hs Hparts)
    as [Hcn [Heq Hsc]].
  unfold numeric_integer_digit_count in Hdig.
  destruct (Z.eqb_spec coeff 0) as [Hz|Hnz].
  - subst.
    assert (Hc0 : c = 0).
    { assert (H0 : 0 = c * 10 ^ scale) by (rewrite <- Heq; lia).
      assert (Hpos : 0 < 10 ^ scale) by (apply Z.pow_pos_nonneg; lia).
      nia. }
    subst. apply Z.pow_pos_nonneg; lia.
  - assert (Hd' : numeric_digit_count coeff - scale <= n) by lia.
    assert (Hd'' : numeric_digit_count coeff <= n + scale) by lia.
    assert (Hnp : 1 <= n + scale \/ n + scale = 0) by lia.
    destruct Hnp as [Hnp|Hzsum].
    + assert (Hcoeff_lt : coeff < 10 ^ (n + scale)).
      { apply numeric_digit_count_le_implies_lt_pow; lia. }
      assert (Hprod : c * 10 ^ scale < 10 ^ (n + s) * 10 ^ scale).
      { rewrite <- Heq.
        replace (10 ^ (n + s) * 10 ^ scale) with (10 ^ (n + scale) * 10 ^ s).
        2:{ rewrite !Z.pow_add_r by lia. ring. }
        apply (proj1 (Z.mul_lt_mono_pos_r (10 ^ s) coeff (10 ^ (n + scale))
          ltac:(apply Z.pow_pos_nonneg; lia))); exact Hcoeff_lt. }
      apply (proj2 (Z.mul_lt_mono_pos_r (10 ^ scale) c (10 ^ (n + s))
        ltac:(apply Z.pow_pos_nonneg; lia))).
      exact Hprod.
    + pose proof (numeric_digit_count_pos coeff). lia.
Qed.

Lemma numeric_of_scaled_fits_mono :
  forall c1 c2 s,
    0 <= s <= postgres_numeric_max_fractional_digits ->
    0 <= c1 <= c2 ->
    numeric_runtime_fits_bool (numeric_of_scaled c2 s) = true ->
    numeric_runtime_fits_bool (numeric_of_scaled c1 s) = true.
Proof.
  intros c1 c2 s Hs Hc Hfit.
  destruct Hs as [Hs0 Hs1].
  destruct (numeric_of_scaled_decimal_parts_bounds c2 s Hs0)
    as [coeff2 [scale2 [Hp2 [Hsc2 [Habs2 Heq2]]]]].
  assert (Hfit' : numeric_runtime_fits_bool (numeric_of_scaled c2 s) = true) by exact Hfit.
  unfold numeric_runtime_fits_bool in Hfit'.
  rewrite Hp2 in Hfit'.
  apply andb_prop in Hfit'.
  destruct Hfit' as [Hscale_valid Hdigits_b].
  apply Z.leb_le in Hdigits_b.
  destruct (numeric_of_scaled_decimal_parts_bounds c1 s Hs0)
    as [coeff1 [scale1 [Hp1 [Hsc1 [Habs1 Heq1]]]]].
  eapply numeric_runtime_fits_from_decimal_parts; [exact Hp1| |].
  - unfold numeric_display_scale_valid_bool.
    apply andb_true_intro; split; apply Z.leb_le; lia.
  - apply (numeric_of_scaled_int_digits_of_lt_pow c1 s
        postgres_numeric_max_integer_digits coeff1 scale1).
    + lia.
    + lia.
    + unfold postgres_numeric_max_integer_digits; lia.
    + exact Hp1.
    + eapply Z.le_lt_trans with (m := c2); [lia|].
      apply (numeric_of_scaled_lt_pow_of_int_digits c2 s
          postgres_numeric_max_integer_digits coeff2 scale2).
      * lia.
      * lia.
      * unfold postgres_numeric_max_integer_digits; lia.
      * exact Hp2.
      * exact Hdigits_b.
Qed.

Lemma numeric_mul_scaled2_fits_mono :
  forall price Pmax factor_c,
    0 <= price <= Pmax ->
    0 <= factor_c ->
    numeric_runtime_fits_bool
      (numeric_mul (numeric_of_scaled Pmax 2) (numeric_of_scaled factor_c 2)) = true ->
    numeric_runtime_fits_bool
      (numeric_mul (numeric_of_scaled price 2) (numeric_of_scaled factor_c 2)) = true.
Proof.
  intros price Pmax factor_c Hp Hf Hfit.
  rewrite !numeric_mul_of_scaled_nonneg in Hfit |- * by lia.
  apply (numeric_of_scaled_fits_mono (price * factor_c) (Pmax * factor_c) 4);
    [unfold postgres_numeric_max_fractional_digits; lia|nia|exact Hfit].
Qed.

Lemma numeric_mul_scaled2_charge_fits_mono :
  forall price Pmax disc_c tax_c,
    0 <= price <= Pmax ->
    0 <= disc_c ->
    0 <= tax_c ->
    numeric_runtime_fits_bool
      (numeric_mul
        (numeric_mul (numeric_of_scaled Pmax 2) (numeric_of_scaled disc_c 2))
        (numeric_of_scaled tax_c 2)) = true ->
    numeric_runtime_fits_bool
      (numeric_mul
        (numeric_mul (numeric_of_scaled price 2) (numeric_of_scaled disc_c 2))
        (numeric_of_scaled tax_c 2)) = true.
Proof.
  intros price Pmax disc_c tax_c Hp Hd Ht Hfit.
  replace (numeric_mul
      (numeric_mul (numeric_of_scaled price 2) (numeric_of_scaled disc_c 2))
      (numeric_of_scaled tax_c 2))
    with (numeric_of_scaled (price * disc_c * tax_c) 6).
  2:{
    assert (H1 : 0 <= (2:Z)) by lia.
    rewrite (numeric_mul_of_scaled_nonneg price 2 disc_c 2 H1 H1).
    change (2 + 2) with 4.
    assert (H4 : 0 <= (4:Z)) by lia.
    rewrite (numeric_mul_of_scaled_nonneg (price * disc_c) 4 tax_c 2 H4 H1).
    change (4 + 2) with 6.
    replace ((price * disc_c) * tax_c) with (price * disc_c * tax_c) by lia.
    reflexivity. }
  revert Hfit.
  replace (numeric_mul
      (numeric_mul (numeric_of_scaled Pmax 2) (numeric_of_scaled disc_c 2))
      (numeric_of_scaled tax_c 2))
    with (numeric_of_scaled (Pmax * disc_c * tax_c) 6).
  2:{
    assert (H1 : 0 <= (2:Z)) by lia.
    rewrite (numeric_mul_of_scaled_nonneg Pmax 2 disc_c 2 H1 H1).
    change (2 + 2) with 4.
    assert (H4 : 0 <= (4:Z)) by lia.
    rewrite (numeric_mul_of_scaled_nonneg (Pmax * disc_c) 4 tax_c 2 H4 H1).
    change (4 + 2) with 6.
    replace ((Pmax * disc_c) * tax_c) with (Pmax * disc_c * tax_c) by lia.
    reflexivity. }
  intro Hfit.
  apply (numeric_of_scaled_fits_mono
      (price * disc_c * tax_c) (Pmax * disc_c * tax_c) 6).
  - unfold postgres_numeric_max_fractional_digits; lia.
  - split.
    + apply Z.mul_nonneg_nonneg; [apply Z.mul_nonneg_nonneg; lia|lia].
    + apply Z.mul_le_mono_nonneg_r; [lia|].
      apply Z.mul_le_mono_nonneg_r; lia.
  - exact Hfit.
Qed.

(** * of_scaled typmod / DECIMAL conforms (ROUND recovers coefficient) *)

Lemma Qcdiv_Qcmult_cancel_r :
  forall a b,
    b <> 0%Qc ->
    Qcmult (Qcdiv a b) b == a.
Proof.
  intros a b Hb.
  unfold Qcdiv.
  rewrite <- Qcmult_assoc.
  rewrite (Qcmult_comm (Qcinv b) b).
  rewrite Qcmult_inv_r by exact Hb.
  rewrite Qcmult_1_r.
  reflexivity.
Qed.

Lemma this_Q2Qc_inject_Z :
  forall z, this (Q2Qc (inject_Z z)) == inject_Z z.
Proof.
  intro z. cbn [this Q2Qc]. apply Qred_correct.
Qed.

Lemma numeric_finite_rounded_coeff_of_scaled :
  forall c s,
    0 <= s ->
    match numeric_of_scaled c s with
    | NumericFinite q => numeric_finite_rounded_coeff q s = c
    | _ => False
    end.
Proof.
  intros c s Hs.
  unfold numeric_of_scaled.
  rewrite (proj2 (Z.leb_le 0 s) Hs).
  set (denZ := 10 ^ s).
  assert (HdenZ : 0 < denZ) by (apply Z.pow_pos_nonneg; lia).
  assert (HdenQc : Q2Qc (inject_Z denZ) <> 0%Qc)
    by (apply Q2Qc_inject_Z_neq0; lia).
  unfold numeric_finite_rounded_coeff, numeric_scale_factor.
  rewrite (proj2 (Z.leb_le 0 s) Hs).
  set (q := Qcdiv (Q2Qc (inject_Z c)) (Q2Qc (inject_Z denZ))).
  set (scaledQc := Qcmult q (Q2Qc (inject_Z denZ))).
  assert (HeqQc : scaledQc == Q2Qc (inject_Z c)).
  { unfold scaledQc, q. apply Qcdiv_Qcmult_cancel_r; exact HdenQc. }
  assert (Heq : this scaledQc == inject_Z c).
  { eapply Qeq_trans with (y := this (Q2Qc (inject_Z c))).
    - exact HeqQc.
    - apply this_Q2Qc_inject_Z. }
  unfold q in scaledQc |- *.
  match goal with
  | |- context [this (Qcmult ?a ?b)] =>
      change (Qcmult a b) with scaledQc
  end.
  unfold numeric_round_quot.
  set (scaledQ := this scaledQc) in *.
  assert (Hn : Qnum scaledQ * 1 = c * Z.pos (Qden scaledQ)).
  { unfold Qeq, inject_Z in Heq. cbn [Qnum Qden] in Heq. exact Heq. }
  assert (Hn' : Qnum scaledQ = c * Z.pos (Qden scaledQ)) by lia.
  set (g := Z.pos (Qden scaledQ)).
  assert (Hg : 0 < g) by (unfold g; lia).
  rewrite Hn'.
  replace (Z.pos (Qden scaledQ)) with g by reflexivity.
  rewrite (Z.quot_mul c g) by lia.
  rewrite (Z.rem_mul c g) by lia.
  assert (Hgeb : (2 * Z.abs 0 >=? Z.abs g) = false).
  { rewrite Z.geb_leb. apply Z.leb_gt. rewrite Z.abs_eq by lia. lia. }
  rewrite Hgeb. reflexivity.
Qed.

Lemma numeric_round_to_scale_of_scaled :
  forall c s,
    0 <= s ->
    numeric_round_to_scale (numeric_of_scaled c s) s = numeric_of_scaled c s.
Proof.
  intros c s Hs.
  unfold numeric_round_to_scale.
  pose proof (numeric_finite_rounded_coeff_of_scaled c s Hs) as Hr.
  destruct (numeric_of_scaled c s) as [|q| |] eqn:Hn; try contradiction.
  cbn [numeric_rounded_coeff].
  rewrite Hr, <- Hn. reflexivity.
Qed.

Lemma numeric_fits_typmod_of_scaled_15_2 :
  forall c,
    Z.abs c < 10 ^ 15 ->
    numeric_fits_typmod_bool (numeric_of_scaled c 2) 15 2 = true.
Proof.
  intros c Hc.
  unfold numeric_fits_typmod_bool, numeric_typmod_valid_bool.
  apply andb_true_intro; split.
  - vm_compute; reflexivity.
  - unfold numeric_rounded_coeff.
    pose proof (numeric_finite_rounded_coeff_of_scaled c 2 ltac:(lia)) as Hr.
    destruct (numeric_of_scaled c 2) as [|q| |] eqn:Hn; try contradiction.
    cbn. rewrite Hr. apply Z.ltb_lt. exact Hc.
Qed.

Lemma numeric_cast_typmod_of_scaled_15_2 :
  forall c,
    Z.abs c < 10 ^ 15 ->
    numeric_cast_typmod (numeric_of_scaled c 2) 15 2 =
    Some (numeric_of_scaled c 2).
Proof.
  intros c Hc.
  unfold numeric_cast_typmod.
  rewrite (numeric_fits_typmod_of_scaled_15_2 c Hc).
  f_equal.
  apply numeric_round_to_scale_of_scaled; lia.
Qed.
