(* GENERIC list uniqueness / filter helpers used by group-bag correspondence.

   Extracted from Q1 FormalSQL↔Spec group proofs. Query-independent:
   injectivity → NoDup(map), filter-length uniqueness → NoDup(map key),
   filter extensionality, and Forall under Permutation. *)

From Stdlib Require Import List Permutation Lia.
Import ListNotations.

Lemma NoDup_map_inj :
  forall (A B : Type) (f : A -> B) (l : list A),
    (forall x y, In x l -> In y l -> f x = f y -> x = y) ->
    NoDup l ->
    NoDup (map f l).
Proof.
  intros A B f l Hinj.
  induction l as [|a l IH]; intros Hnd; [constructor|].
  inversion Hnd as [|? ? Hnin Hnd']; subst.
  cbn [map]; constructor.
  - intro Hin; apply in_map_iff in Hin; destruct Hin as [x [Heq Hinx]].
    apply Hnin.
    assert (Heqax : a = x).
    { apply Hinj; [left; reflexivity|right; exact Hinx|symmetry; exact Heq]. }
    rewrite Heqax; exact Hinx.
  - apply IH; [|exact Hnd'].
    intros x y Hx Hy; apply Hinj; right; assumption.
Qed.

Lemma NoDup_map_of_filter_unique :
  forall (A K : Type) (key : A -> K) (eqb : K -> K -> bool) (l : list A),
    (forall x y, eqb x y = true -> x = y) ->
    (forall x, eqb x x = true) ->
    (forall k, (length (filter (fun x => eqb (key x) k) l) <= 1)%nat) ->
    NoDup (map key l).
Proof.
  intros A K key eqb l Heq_of Heq_refl Hle.
  induction l as [|a l IH]; [constructor|].
  cbn [map]; constructor.
  - rewrite in_map_iff; intros [x [Hk Hinx]].
    assert (Hlen := Hle (key a)).
    cbn [filter] in Hlen.
    rewrite (Heq_refl (key a)) in Hlen.
    destruct (eqb (key x) (key a)) eqn:Exa.
    + assert (Hinx_f : In x (filter (fun z => eqb (key z) (key a)) l)).
      { apply filter_In; split; [exact Hinx|exact Exa]. }
      destruct (filter (fun z => eqb (key z) (key a)) l) as [|p ps];
        [contradiction Hinx_f|cbn in Hlen; lia].
    + rewrite Hk, (Heq_refl (key a)) in Exa; discriminate.
  - apply IH.
    intros k; specialize (Hle k); cbn [filter] in Hle.
    destruct (eqb (key a) k); [cbn in Hle; lia|exact Hle].
Qed.

Lemma filter_ext_In :
  forall (A : Type) (f g : A -> bool) (l : list A),
    (forall x, In x l -> f x = g x) ->
    filter f l = filter g l.
Proof.
  intros A f g l H.
  induction l as [|a l IH]; [reflexivity|].
  cbn [filter]; rewrite (H a (or_introl eq_refl)), IH.
  - reflexivity.
  - intros x Hx; apply H; right; exact Hx.
Qed.

Lemma filter_map_ext_In :
  forall (A B : Type) (f : A -> B) (p : B -> bool) (q : A -> bool) (l : list A),
    (forall x, In x l -> p (f x) = q x) ->
    filter p (map f l) = map f (filter q l).
Proof.
  intros A B f p q l Hpq.
  induction l as [|x xs IH]; [reflexivity|].
  cbn [map filter].
  rewrite (Hpq x (or_introl eq_refl)).
  destruct (q x); cbn.
  - f_equal; apply IH; intros y Hin; apply Hpq; right; exact Hin.
  - apply IH; intros y Hin; apply Hpq; right; exact Hin.
Qed.

Lemma Forall_permutation :
  forall (A : Type) (P : A -> Prop) l1 l2,
    Permutation l1 l2 -> Forall P l1 -> Forall P l2.
Proof.
  intros A P l1 l2 Hp Hfa.
  apply Forall_forall; intros x Hin.
  apply (Permutation_in x (Permutation_sym Hp)) in Hin.
  eapply Forall_forall; eassumption.
Qed.
