(* Generic FormalSQL ORDER BY observation facade.

   Packages FormalSQL [order_by_rows_has_observation] /
   [sort_ordered_rows] and Logos [order_by_rows_total_map] so query proofs
   obtain a canonical ordered observation without reinventing sort machinery. *)

From Stdlib Require Import List.
From SQLFS Require Import
  OrderedSet FiniteSet FiniteBag FiniteCollection FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts SqlListFacts Values GenericInstance.
From Logos.FormalSQL Require Import
  TNullSyntax QueryTNullSyntax OrderedObservationTransportFacts.

Import Tuple.
Import ListNotations.

Section OrderRowsFacade.

Context {T : Tuple.Rcd}.

Variable value_is_null : value T -> bool.

(** Canonical ordered representative always exists. *)
Theorem order_by_rows_canonical :
  forall keys input,
    @order_by_rows T value_is_null keys input
      (sort_ordered_rows value_is_null keys input).
Proof.
  intros keys input.
  split.
  - apply permutation_same_rows_as_bag, sort_ordered_rows_permutation.
  - apply sort_ordered_rows_is_ordered.
Qed.

Theorem order_by_rows_exists :
  forall keys input,
    exists output, @order_by_rows T value_is_null keys input output.
Proof.
  intros keys input.
  exists (sort_ordered_rows value_is_null keys input).
  apply order_by_rows_canonical.
Qed.

(** Transport ordered observation across an exact key-preserving map. *)
Theorem order_by_rows_of_total_map :
  forall (mapping : tuple T -> tuple T)
      left_keys right_keys input ordered,
    order_key_map_exact value_is_null mapping left_keys right_keys ->
    @order_by_rows T value_is_null left_keys input ordered ->
    @order_by_rows T value_is_null right_keys
      (map mapping input) (map mapping ordered).
Proof.
  intros; eapply order_by_rows_total_map; eassumption.
Qed.

End OrderRowsFacade.

Section OrderRowsFacade_TNull.

Definition tnull_value_is_null (v : value TNull) : bool :=
  NullValues.is_null_value v.

Theorem tnull_order_by_rows_canonical :
  forall keys input,
    @order_by_rows TNull tnull_value_is_null keys input
      (sort_ordered_rows tnull_value_is_null keys input).
Proof.
  exact (@order_by_rows_canonical TNull tnull_value_is_null).
Qed.

End OrderRowsFacade_TNull.
