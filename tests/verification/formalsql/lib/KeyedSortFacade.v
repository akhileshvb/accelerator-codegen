(* Generic keyed-sort uniqueness.

   Equal bags + Sorted under a transitive comparator with local (in-list)
   antisymmetry ⇒ the sorted lists are equal.  Query proofs reuse this for
   finalize/ORDER BY instead of re-proving uniqueness per query.

   Global antisymmetry is only required for [sorted_perm_eq], not for the
   bag-local [sorted_perm_eq_in] used when ORDER BY keys are unique in the
   observation but not for arbitrary [A] values. *)

From Stdlib Require Import List Sorting.Sorted Sorting.Permutation.
Import ListNotations.

Section KeyedSortFacade.

Context {A : Type}.

Variable leA : A -> A -> bool.

Definition LeA (x y : A) : Prop := leA x y = true.

(** List-local transitivity (enough for sorted uniqueness on a bag). *)
Definition leA_trans_on (l : list A) : Prop :=
  forall x y z,
    In x l -> In y l -> In z l ->
    leA x y = true -> leA y z = true -> leA x z = true.

Lemma Sorted_le_In :
  forall a l x,
    Sorted LeA (a :: l) ->
    In x l ->
    leA_trans_on (a :: l) ->
    LeA a x.
Proof.
  intros a l; revert a.
  induction l as [|b bs IH]; intros a x Hs Hx Htr; [contradiction|].
  destruct Hx as [Heq|Hin].
  - subst x.
    inversion Hs as [|aa ll Hs' Hd Heq1]; clear Hs Hs'; subst aa ll.
    inversion Hd as [|bb ll Hle Heq2]; subst bb ll; exact Hle.
  - inversion Hs as [|aa ll Hs_tail Hd Heq1]; subst aa ll.
    assert (Hab : leA a b = true).
    { inversion Hd as [|bb ll Hle Heq2]; subst bb ll; exact Hle. }
    assert (Hbx : leA b x = true).
    {
      apply IH; [exact Hs_tail|exact Hin|].
      intros u v w Hu Hv Hw; apply Htr; simpl; tauto.
    }
    change (LeA a x).
    unfold LeA.
    apply (Htr a b x); simpl; auto.
Qed.

Lemma Permutation_cons_head_eq :
  forall (a b : A) l1 l2,
    Permutation (a :: l1) (b :: l2) ->
    a = b ->
    Permutation l1 l2.
Proof.
  intros a b l1 l2 Hp Heq; subst b.
  apply Permutation_cons_inv in Hp; exact Hp.
Qed.

(** Unique sorted representative when antisym holds on list members only
    (e.g. ORDER BY keys unique in the bag, but not globally on [A]). *)
Theorem sorted_perm_eq_in :
  forall l1 l2,
    Sorted LeA l1 ->
    Sorted LeA l2 ->
    Permutation l1 l2 ->
    leA_trans_on l1 ->
    (forall x y, In x l1 -> In y l1 -> LeA x y -> LeA y x -> x = y) ->
    l1 = l2.
Proof.
  intros l1; induction l1 as [|a l1 IH]; intros l2 Hs1 Hs2 Hp Htr Hanti.
  - apply Permutation_nil in Hp; symmetry; exact Hp.
  - destruct l2 as [|b l2].
    + apply Permutation_sym, Permutation_nil in Hp; discriminate.
    + assert (Ha : In a (b :: l2)).
      { eapply Permutation_in; [exact Hp|left; reflexivity]. }
      assert (Hb : In b (a :: l1)).
      { eapply Permutation_in;
          [apply Permutation_sym; exact Hp|left; reflexivity]. }
      assert (Htr2 : leA_trans_on (b :: l2)).
      {
        intros u v w Hu Hv Hw Hab' Hbc'.
        apply Htr with v;
          try (eapply Permutation_in; [apply Permutation_sym; exact Hp|]; assumption);
          assumption.
      }
      assert (Heqab : a = b).
      { destruct Ha as [Heqa|Haina]; [symmetry; exact Heqa|].
        destruct Hb as [Heqb|Hainb]; [exact Heqb|].
        pose proof (Sorted_le_In a l1 b Hs1 Hainb Htr) as Hab.
        pose proof (Sorted_le_In b l2 a Hs2 Haina Htr2) as Hba.
        apply Hanti; [left; reflexivity|right; exact Hainb|exact Hab|exact Hba]. }
      subst b.
      inversion Hs1 as [|? ? Hs1' ?]; subst.
      inversion Hs2 as [|? ? Hs2' ?]; subst.
      f_equal.
      apply IH; [exact Hs1'|exact Hs2'| | |].
      * exact (Permutation_cons_head_eq _ _ _ _ Hp eq_refl).
      * intros u v w Hu Hv Hw; apply Htr; simpl; tauto.
      * intros x y Hx Hy; apply Hanti; right; assumption.
Qed.

Section WithGlobalAntisym.

Hypothesis leA_trans :
  forall x y z, leA x y = true -> leA y z = true -> leA x z = true.

Hypothesis leA_total :
  forall x y, leA x y = true \/ leA y x = true.

Hypothesis leA_antisym :
  forall x y, leA x y = true -> leA y x = true -> x = y.

(** Unique sorted representative of a multiset. *)
Theorem sorted_perm_eq :
  forall l1 l2,
    Sorted LeA l1 ->
    Sorted LeA l2 ->
    Permutation l1 l2 ->
    l1 = l2.
Proof.
  intros l1 l2 Hs1 Hs2 Hp.
  apply sorted_perm_eq_in; try assumption.
  - intros x y z _ _ _; apply leA_trans.
  - intros x y _ _ Hab Hba; exact (leA_antisym x y Hab Hba).
Qed.

(** Map of equal sorted lists is equal (outer embed / project bridge). *)
Theorem map_sorted_perm_eq :
  forall (B : Type) (f : A -> B) l1 l2,
    Sorted LeA l1 ->
    Sorted LeA l2 ->
    Permutation l1 l2 ->
    map f l1 = map f l2.
Proof.
  intros B f l1 l2 Hs1 Hs2 Hp.
  rewrite (sorted_perm_eq l1 l2 Hs1 Hs2 Hp); reflexivity.
Qed.

End WithGlobalAntisym.

End KeyedSortFacade.
