(* Generic sorted group-result uniqueness facade.

   Packages: same bag (Permutation) + Sorted under a list-local transitive
   ORDER BY comparator with local (in-list) antisymmetry ⇒ unique sorted
   representative.

   Used to discharge Spec↔FormalSQL group-bag residuals of the form

     sort_ordered_rows keys L_FS = sort_ordered_rows keys L_Spec

   once Permutation L_FS L_Spec and key uniqueness are proved.  Sibling of
   KeyedSortFacade; lifts it to FormalSQL [sort_ordered_rows]. *)

From Stdlib Require Import List Sorting.Sorted Sorting.Permutation.
From SQLFS Require Import
  OrderedSet FlatData SqlOrder SqlListFacts Values GenericInstance.
From FormalSQLLib Require Import KeyedSortFacade.
From Logos.FormalSQL Require Import TNullSyntax.

Import Tuple.
Import ListNotations.

Section SortedGroupResult.

Context {T : Tuple.Rcd}.

Variable value_is_null : value T -> bool.
Variable keys : list (sort_key T).

(** ORDER BY ≤ from FormalSQL [compare_order_keys] (Eq|Lt ⇒ true). *)
Definition order_keys_le (a b : tuple T) : bool :=
  match @compare_order_keys T value_is_null keys a b with
  | Gt => false
  | Eq | Lt => true
  end.

Definition OrderKeysLe (a b : tuple T) : Prop := order_keys_le a b = true.

Lemma ordered_pair_OrderKeysLe :
  forall a b,
    @ordered_pair T value_is_null keys a b <-> OrderKeysLe a b.
Proof.
  intros a b; unfold ordered_pair, OrderKeysLe, order_keys_le.
  destruct (@compare_order_keys T value_is_null keys a b);
    split; intros H; try exact I; try reflexivity; try contradiction; discriminate.
Qed.

Lemma ordered_rows_Sorted_OrderKeysLe :
  forall rows,
    @ordered_rows T value_is_null keys rows ->
    Sorted OrderKeysLe rows.
Proof.
  induction rows as [|a rows IH]; intros Hord; [constructor|].
  destruct rows as [|b bs]; [repeat constructor|].
  destruct Hord as [Hab Htail].
  constructor.
  - apply IH; exact Htail.
  - constructor; apply ordered_pair_OrderKeysLe; exact Hab.
Qed.

Lemma sort_ordered_rows_Sorted_OrderKeysLe :
  forall rows,
    Sorted OrderKeysLe
      (@sort_ordered_rows T value_is_null keys rows).
Proof.
  intros rows.
  apply ordered_rows_Sorted_OrderKeysLe, sort_ordered_rows_is_ordered.
Qed.

(** Core uniqueness: sorted perms of the same bag coincide under local
    transitivity + local antisym (both only required on list members). *)
Theorem sorted_order_keys_perm_eq_in :
  forall l1 l2,
    Sorted OrderKeysLe l1 ->
    Sorted OrderKeysLe l2 ->
    Permutation l1 l2 ->
    @leA_trans_on (tuple T) order_keys_le l1 ->
    (forall x y, In x l1 -> In y l1 ->
      OrderKeysLe x y -> OrderKeysLe y x -> x = y) ->
    l1 = l2.
Proof.
  intros l1 l2 Hs1 Hs2 Hp Htr Hanti.
  eapply (@sorted_perm_eq_in (tuple T) order_keys_le);
    eassumption.
Qed.

(** Main facade theorem: same bag ⇒ same FormalSQL sorted representative. *)
Theorem sort_ordered_rows_eq_of_perm :
  forall l1 l2,
    Permutation l1 l2 ->
    @leA_trans_on (tuple T) order_keys_le l1 ->
    (forall x y,
      In x l1 -> In y l1 ->
      OrderKeysLe x y -> OrderKeysLe y x -> x = y) ->
    @sort_ordered_rows T value_is_null keys l1 =
    @sort_ordered_rows T value_is_null keys l2.
Proof.
  intros l1 l2 Hp Htr Hanti.
  set (s1 := @sort_ordered_rows T value_is_null keys l1).
  set (s2 := @sort_ordered_rows T value_is_null keys l2).
  assert (Hs1 : Sorted OrderKeysLe s1) by apply sort_ordered_rows_Sorted_OrderKeysLe.
  assert (Hs2 : Sorted OrderKeysLe s2) by apply sort_ordered_rows_Sorted_OrderKeysLe.
  assert (Hp12 : Permutation s1 s2).
  {
    apply Permutation_trans with l1.
    - apply Permutation_sym, sort_ordered_rows_permutation.
    - apply Permutation_trans with l2; [exact Hp|].
      apply sort_ordered_rows_permutation.
  }
  assert (Htr_s1 : @leA_trans_on (tuple T) order_keys_le s1).
  {
    intros x y z Hx Hy Hz Hab Hba.
    assert (Hx1 : In x l1)
      by (eapply Permutation_in; [apply Permutation_sym, sort_ordered_rows_permutation|exact Hx]).
    assert (Hy1 : In y l1)
      by (eapply Permutation_in; [apply Permutation_sym, sort_ordered_rows_permutation|exact Hy]).
    assert (Hz1 : In z l1)
      by (eapply Permutation_in; [apply Permutation_sym, sort_ordered_rows_permutation|exact Hz]).
    exact (Htr x y z Hx1 Hy1 Hz1 Hab Hba).
  }
  assert (Hanti_s1 :
    forall x y, In x s1 -> In y s1 ->
      OrderKeysLe x y -> OrderKeysLe y x -> x = y).
  {
    intros x y Hx Hy Hab Hba.
    assert (Hx1 : In x l1)
      by (eapply Permutation_in; [apply Permutation_sym, sort_ordered_rows_permutation|exact Hx]).
    assert (Hy1 : In y l1)
      by (eapply Permutation_in; [apply Permutation_sym, sort_ordered_rows_permutation|exact Hy]).
    exact (Hanti x y Hx1 Hy1 Hab Hba).
  }
  exact (sorted_order_keys_perm_eq_in s1 s2 Hs1 Hs2 Hp12 Htr_s1 Hanti_s1).
Qed.

(** Convenience: antisym via injectivity of an order-key projection. *)
Theorem sort_ordered_rows_eq_of_perm_key_inj :
  forall (K : Type) (key_of : tuple T -> K) l1 l2,
    Permutation l1 l2 ->
    @leA_trans_on (tuple T) order_keys_le l1 ->
    (forall x y, In x l1 -> In y l1 ->
      key_of x = key_of y -> x = y) ->
    (forall x y, OrderKeysLe x y -> OrderKeysLe y x -> key_of x = key_of y) ->
    @sort_ordered_rows T value_is_null keys l1 =
    @sort_ordered_rows T value_is_null keys l2.
Proof.
  intros K key_of l1 l2 Hp Htr Hinj Hle_key.
  apply sort_ordered_rows_eq_of_perm; [exact Hp|exact Htr|].
  intros x y Hx Hy Hab Hba.
  apply Hinj; [exact Hx|exact Hy|].
  exact (Hle_key x y Hab Hba).
Qed.

End SortedGroupResult.

(** * TNull specialization *)

Section SortedGroupResult_TNull.

Variable keys : list (sort_key TNull).

Definition tnull_value_is_null (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

Definition tnull_order_keys_le :=
  @order_keys_le TNull tnull_value_is_null keys.

Definition TNullOrderKeysLe :=
  @OrderKeysLe TNull tnull_value_is_null keys.

Theorem tnull_sort_ordered_rows_eq_of_perm :
  forall l1 l2,
    Permutation l1 l2 ->
    @leA_trans_on (tuple TNull) tnull_order_keys_le l1 ->
    (forall x y,
      In x l1 -> In y l1 ->
      TNullOrderKeysLe x y -> TNullOrderKeysLe y x -> x = y) ->
    @sort_ordered_rows TNull tnull_value_is_null keys l1 =
    @sort_ordered_rows TNull tnull_value_is_null keys l2.
Proof.
  intros; eapply (@sort_ordered_rows_eq_of_perm TNull tnull_value_is_null keys);
    eassumption.
Qed.

End SortedGroupResult_TNull.
