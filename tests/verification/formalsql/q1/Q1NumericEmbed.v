(* Q1 NumericEmbedFaithful — representation TCB (theorem hypothesis, not Axiom).

   Z aggregate coefficients ↔ FormalSQL NUMERIC cells.  Does NOT choose group
   membership or which rows contribute; that is [Q1_GroupsCorrespond].

   AVG runtime overflow packaging is NOT here: [q1_group_bucket_safe_of_pre]
   already Qed from Pre (including AVG).

   Assessment (TCB reduce pass):
   - SUM of [embed_decimal2] / scaled [numeric_of_scaled] is near-Qed via
     [numeric_add_same_scale] + [numeric_mul_of_scaled_nonneg] +
     [numeric_one_minus_scaled2] (NumericScaledFits / NumericRegroupFacts).
   - AVG half uses [interp_avg_numeric_fixed] (typmod + avg_scale_state) vs
     [embed_avg_from_sum_scale2] ([numeric_div_at_scales] / PG select_div_scale).
     Closing that bridge is concrete PostgreSQL NUMERIC display-scale semantics
     — leave [NumericEmbedFaithful] TCB; do not claim NUMERIC_EMBED_PROVED. *)

From SQLFS Require Import Values ValueNumeric.
From JetStreamFS Require Import Q1AuxSpec.
From Stdlib Require Import List ZArith Lia.
Import ListNotations.
Open Scope Z_scope.

Require Import Q1FormalSQLProof.
From FormalSQLLib Require Import NumericScaledFits.
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

(** Representation only: SUM/AVG of embedded row cells = embed of Z folds.
    Scale 2 for qty/price/disc; scale 4 for disc_price ($f4); scale 6 for charge ($f5).
    AVG uses [embed_avg_from_sum_scale2], matching [EmbedOutRow_Q1]. *)
Definition NumericEmbedFaithful : Prop :=
  forall (rs : list Q1AuxSpec.Row),
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
