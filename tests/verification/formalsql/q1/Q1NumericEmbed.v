(* Q1 numeric embedding: Z aggregate coefficients <-> FormalSQL NUMERIC cells.

   [NumericEmbedFaithful] is PROVED ([NumericEmbedFaithful_holds]) from the
   query-generic lemmas of FormalSQLLib.NumericAggExact (exact SUM over scaled
   decimals; exact fixed-typmod AVG).  It is no longer a premise of the Q1
   theorems.

   History: it used to be a theorem hypothesis stated for EVERY row list,
   including the empty one.  That statement is false (SUM over no rows is SQL
   NULL, not 0): see [NumericEmbedFaithful_unrestricted_false].  Every Q1
   theorem that assumed it was therefore vacuous.  The corrected statement
   quantifies over the non-empty, well-formed row lists a GROUP BY group can
   contain, which is all the proofs ever use.

   Does NOT choose group membership or which rows contribute; that is
   [Q1_GroupsCorrespond]. *)

From SQLFS Require Import Values ValueNumeric.
From JetStreamFS Require Import Q1AuxSpec.
From Stdlib Require Import List ZArith Lia Bool.
Import ListNotations.
Open Scope Z_scope.

Require Import Q1FormalSQLProof.
From FormalSQLLib Require Import NumericScaledFits NumericPackagingFacade NumericAggExact.
From Logos.FormalSQL Require Import NumericFacts.
From Logos.FormalSQL Require Import NumericRegroupFacts.

(** Disc-price / charge Z coefficients (AuxSpec SpecApply shapes). *)
Definition q1_row_disc_price (r : Q1AuxSpec.Row) : Z :=
  Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r).

Definition q1_row_charge (r : Q1AuxSpec.Row) : Z :=
  q1_row_disc_price r * (100 + Q1AuxSpec.tax r).

(** FormalSQL [$f4]/[$f5] numeric trees (same shape as spine [q1_f4_num]). *)
Definition q1_embed_f4 (r : Q1AuxSpec.Row) : NullValues.value :=
  NullValues.Value_numeric
    (Some (numeric_mul (numeric_of_scaled (Q1AuxSpec.price r) 2)
             (numeric_sub (numeric_of_Z 1)
                (numeric_of_scaled (Q1AuxSpec.disc r) 2)))).

Definition q1_embed_f5 (r : Q1AuxSpec.Row) : NullValues.value :=
  NullValues.Value_numeric
    (Some (numeric_mul
             (numeric_mul (numeric_of_scaled (Q1AuxSpec.price r) 2)
                (numeric_sub (numeric_of_Z 1)
                   (numeric_of_scaled (Q1AuxSpec.disc r) 2)))
             (numeric_add (numeric_of_Z 1)
                (numeric_of_scaled (Q1AuxSpec.tax r) 2)))).

(** Per-row $f4/$f5 trees equal of_scaled of Aux Z coefficients (no SUM yet). *)
Lemma q1_embed_f4_of_scaled :
  forall r,
    numeric_mul (numeric_of_scaled (Q1AuxSpec.price r) 2)
      (numeric_sub (numeric_of_Z 1) (numeric_of_scaled (Q1AuxSpec.disc r) 2)) =
    numeric_of_scaled (q1_row_disc_price r) 4.
Proof.
  intro r.
  rewrite numeric_one_minus_scaled2.
  rewrite (numeric_mul_of_scaled_nonneg _ 2 _ 2 ltac:(lia) ltac:(lia)).
  unfold q1_row_disc_price; reflexivity.
Qed.

Lemma q1_embed_f5_of_scaled :
  forall r,
    numeric_mul
      (numeric_mul (numeric_of_scaled (Q1AuxSpec.price r) 2)
         (numeric_sub (numeric_of_Z 1) (numeric_of_scaled (Q1AuxSpec.disc r) 2)))
      (numeric_add (numeric_of_Z 1) (numeric_of_scaled (Q1AuxSpec.tax r) 2)) =
    numeric_of_scaled (q1_row_charge r) 6.
Proof.
  intro r.
  rewrite q1_embed_f4_of_scaled.
  rewrite numeric_one_plus_scaled2.
  rewrite (numeric_mul_of_scaled_nonneg _ 4 _ 2 ltac:(lia) ltac:(lia)).
  unfold q1_row_charge, q1_row_disc_price; reflexivity.
Qed.

(** The SUM / AVG clauses for a list of rows. *)
Definition NumericEmbedClauses (rs : list Q1AuxSpec.Row) : Prop :=
    NullValues.interp_sum_numeric
      (map (fun r => embed_decimal2 (Q1AuxSpec.qty r)) rs) =
      embed_decimal2 (fold_right Z.add 0 (map Q1AuxSpec.qty rs)) /\
    NullValues.interp_sum_numeric
      (map (fun r => embed_decimal2 (Q1AuxSpec.price r)) rs) =
      embed_decimal2 (fold_right Z.add 0 (map Q1AuxSpec.price rs)) /\
    NullValues.interp_sum_numeric (map q1_embed_f4 rs) =
      embed_decimal_at (fold_right Z.add 0 (map q1_row_disc_price rs)) 4 /\
    NullValues.interp_sum_numeric (map q1_embed_f5 rs) =
      embed_decimal_at (fold_right Z.add 0 (map q1_row_charge rs)) 6 /\
    NullValues.interp_avg_numeric_fixed 15 2
      (map (fun r => embed_decimal2 (Q1AuxSpec.qty r)) rs) =
      embed_avg_from_sum_scale2
        (fold_right Z.add 0 (map Q1AuxSpec.qty rs))
        (Z.of_nat (List.length rs)) /\
    NullValues.interp_avg_numeric_fixed 15 2
      (map (fun r => embed_decimal2 (Q1AuxSpec.price r)) rs) =
      embed_avg_from_sum_scale2
        (fold_right Z.add 0 (map Q1AuxSpec.price rs))
        (Z.of_nat (List.length rs)) /\
    NullValues.interp_avg_numeric_fixed 15 2
      (map (fun r => embed_decimal2 (Q1AuxSpec.disc r)) rs) =
      embed_avg_from_sum_scale2
        (fold_right Z.add 0 (map Q1AuxSpec.disc rs))
        (Z.of_nat (List.length rs)).

(** The former premise, over every row list.  False (empty list). *)
Definition NumericEmbedFaithful_unrestricted : Prop :=
  forall (rs : list Q1AuxSpec.Row), NumericEmbedClauses rs.

Lemma NumericEmbedFaithful_unrestricted_false : ~ NumericEmbedFaithful_unrestricted.
Proof.
  intros H. destruct (H nil) as [Hsum _].
  cbn in Hsum. unfold embed_decimal2 in Hsum. discriminate Hsum.
Qed.

(** Representation of the numeric cells of a GROUP BY group (non-empty,
    well-formed rows).  Scale 2 for qty/price/disc; scale 4 for disc_price
    ($f4); scale 6 for charge ($f5); AVG as [embed_avg_from_sum_scale2]. *)
Definition NumericEmbedFaithful : Prop :=
  forall (rs : list Q1AuxSpec.Row),
    rs <> nil -> Forall RowOk_Q1 rs -> NumericEmbedClauses rs.

Lemma embed_decimal2_scaled : forall z, embed_decimal2 z = scaled_value 2 z.
Proof. reflexivity. Qed.

Lemma map_embed_decimal2 : forall (f : Q1AuxSpec.Row -> Z) rs,
  map (fun r => embed_decimal2 (f r)) rs = map (scaled_value 2) (map f rs).
Proof. intros f rs; induction rs as [|r rs IH]; cbn; [reflexivity|now rewrite IH]. Qed.

Lemma map_embed_f4 : forall rs,
  map q1_embed_f4 rs = map (scaled_value 4) (map q1_row_disc_price rs).
Proof.
  intros rs; induction rs as [|r rs IH]; cbn; [reflexivity|].
  rewrite IH. unfold q1_embed_f4, scaled_value. now rewrite q1_embed_f4_of_scaled.
Qed.

Lemma map_embed_f5 : forall rs,
  map q1_embed_f5 rs = map (scaled_value 6) (map q1_row_charge rs).
Proof.
  intros rs; induction rs as [|r rs IH]; cbn; [reflexivity|].
  rewrite IH. unfold q1_embed_f5, scaled_value. now rewrite q1_embed_f5_of_scaled.
Qed.

Lemma sum_scaled_nonempty : forall s (cs : list Z),
  cs <> nil ->
  NullValues.interp_sum_numeric (map (scaled_value s) cs) = scaled_value s (zsum cs).
Proof.
  intros s cs Hne. rewrite interp_sum_numeric_scaled. destruct cs; [congruence|reflexivity].
Qed.

Lemma conforms_15_2 : forall (f : Q1AuxSpec.Row -> Z) rs,
  Forall (fun r => Z.abs (f r) < 10 ^ 15) rs ->
  forallb (NullValues.numeric_conforms_typmod_bool 15 2)
    (map (fun c => numeric_of_scaled c 2) (map f rs)) = true.
Proof.
  intros f rs H; induction H as [|r rs Hr _ IH]; cbn; [reflexivity|].
  rewrite IH, andb_true_r.
  unfold NullValues.numeric_conforms_typmod_bool.
  rewrite numeric_cast_typmod_of_scaled_15_2 by exact Hr.
  apply numeric_eqb_refl.
Qed.

Lemma avg_scaled_15_2 : forall (f : Q1AuxSpec.Row -> Z) rs,
  Forall (fun r => Z.abs (f r) < 10 ^ 15) rs ->
  NullValues.interp_avg_numeric_fixed 15 2 (map (fun r => embed_decimal2 (f r)) rs) =
  embed_avg_from_sum_scale2 (fold_right Z.add 0 (map f rs)) (Z.of_nat (List.length rs)).
Proof.
  intros f rs H.
  rewrite map_embed_decimal2.
  rewrite interp_avg_numeric_fixed_scaled.
  - rewrite length_map. reflexivity.
  - lia.
  - exact numeric_typmod_valid_15_2.
  - exact (conforms_15_2 f rs H).
Qed.

Theorem NumericEmbedFaithful_holds : NumericEmbedFaithful.
Proof.
  intros rs Hne Hok.
  assert (Hne' : forall (f : Q1AuxSpec.Row -> Z), map f rs <> nil)
    by (intros f E; apply Hne; destruct rs; [reflexivity|discriminate]).
  assert (Hb : forall (f : Q1AuxSpec.Row -> Z),
            (forall r, RowOk_Q1 r -> 0 <= f r <= 1000000000000) ->
            Forall (fun r => Z.abs (f r) < 10 ^ 15) rs).
  { intros f Hf. eapply Forall_impl; [|exact Hok].
    intros r Hr. pose proof (Hf r Hr). rewrite Z.abs_eq by lia. cbn. lia. }
  assert (Hq : forall r, RowOk_Q1 r -> 0 <= Q1AuxSpec.qty r <= 1000000000000)
    by (intros r Hr; unfold RowOk_Q1 in Hr; lia).
  assert (Hp : forall r, RowOk_Q1 r -> 0 <= Q1AuxSpec.price r <= 1000000000000)
    by (intros r Hr; unfold RowOk_Q1 in Hr; lia).
  assert (Hd : forall r, RowOk_Q1 r -> 0 <= Q1AuxSpec.disc r <= 1000000000000)
    by (intros r Hr; unfold RowOk_Q1 in Hr; lia).
  unfold NumericEmbedClauses.
  split; [|split; [|split; [|split; [|split; [|split]]]]].
  - rewrite map_embed_decimal2, sum_scaled_nonempty by apply Hne'. reflexivity.
  - rewrite map_embed_decimal2, sum_scaled_nonempty by apply Hne'. reflexivity.
  - rewrite map_embed_f4, sum_scaled_nonempty by apply Hne'. reflexivity.
  - rewrite map_embed_f5, sum_scaled_nonempty by apply Hne'. reflexivity.
  - exact (avg_scaled_15_2 Q1AuxSpec.qty rs (Hb _ Hq)).
  - exact (avg_scaled_15_2 Q1AuxSpec.price rs (Hb _ Hp)).
  - exact (avg_scaled_15_2 Q1AuxSpec.disc rs (Hb _ Hd)).
Qed.
