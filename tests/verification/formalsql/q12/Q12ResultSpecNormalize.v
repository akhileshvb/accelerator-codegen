(* Q12ResultSpecNormalize — normalize(qhat_Q12) = Some(S_Q12_catalog). *)

From LogosGenerated Require Import Queries.
From Stdlib Require Import List ZArith String.
Import ListNotations.
Open Scope Z_scope.
Open Scope string_scope.

From FormalSQLResultSpec Require Import QueryResultSpecQ12 NormalizeQueryQ12.
Require Import Q12Catalog.

Lemma normalize_qhat_Q12_catalog :
  normalize_query_Q12 Queries.source_query_expr = Some S_Q12_catalog.
Proof.
  unfold normalize_query_Q12, Queries.source_query_expr.
  (* source_query_expr unfolds to shared_query_expr_0 in generated Queries.v *)
  unfold Queries.shared_query_expr_0.
  cbn.
  reflexivity.
Qed.
