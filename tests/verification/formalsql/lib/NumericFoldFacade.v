(* GENERIC Z-fold helpers for SUM/COUNT bag aggregates over rev/app.

   Extracted from Q1 group-bag sum correspondence (COUNT/SUM over reversed
   FormalSQL group members vs Aux buckets). *)

From Stdlib Require Import List ZArith Lia.
Import ListNotations.
Open Scope Z_scope.

Lemma fold_right_Z_add_app :
  forall l1 l2,
    fold_right Z.add 0 (l1 ++ l2) =
    fold_right Z.add 0 l1 + fold_right Z.add 0 l2.
Proof.
  induction l1 as [|x xs IH]; intros l2; cbn; [lia|].
  rewrite IH; lia.
Qed.

Lemma fold_right_Z_add_rev_list :
  forall l : list Z,
    fold_right Z.add 0 (rev l) = fold_right Z.add 0 l.
Proof.
  induction l as [|x xs IH]; [reflexivity|].
  cbn [rev].
  rewrite fold_right_Z_add_app.
  cbn [fold_right].
  lia.
Qed.

Lemma fold_right_Z_add_rev :
  forall (A : Type) (f : A -> Z) (l : list A),
    fold_right Z.add 0 (map f (rev l)) = fold_right Z.add 0 (map f l).
Proof.
  intros A f l.
  rewrite map_rev.
  apply fold_right_Z_add_rev_list.
Qed.
