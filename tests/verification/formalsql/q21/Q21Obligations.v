(* Q21 FormalSQL ↔ AuxSpec: fixed semantic obligations (LLM proof-agent interface).

   Architecture (see STATUS.md / PIPELINE.md):
     exact Cat(Q21) --Logos--> qhat_Q21 --[these Qed obligations]--> AuxAnswerSpec
     AuxAnswerSpec --Dafny Twin≡Spec--> Twin --generic runtime--> accelerator

   Design rule: each obligation is a thin Q21 instantiation of a reusable
   FormalSQL facade / AuxSpec fact.  Do NOT hand-author FormalSQL query terms. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore
  SqlOutcome SqlQuerySyntax SqlQuerySemantics
  FiniteBag FiniteCollection FiniteSet FTuples Bool3.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax.
From LogosGenerated Require Import Schema Queries.
From JetStreamFS Require Import Q21AuxSpec.
From Stdlib Require Import String List ZArith Lia.
Import ListNotations.
Import Tuple.
Open Scope string_scope.
Open Scope Z_scope.

Require Import Q21Catalog Q21FormalSQLProof.

(** * Outcome membership *)

Definition FormalSQLOutcomes (D : db_state) (outs : list (tuple TNull)) : Prop :=
  FormalSQL_exec D outs.

(** * 1. ContribEq_Q21 — positive contribution ⇒ sole-late ∧ multi-supplier ∧ F *)

Lemma ContribEq_Q21 :
  forall summary sk,
    Q21AuxSpec.ComputeOrderContributionsSpec summary sk > 0 ->
    Q21AuxSpec.orderstatus summary = Q21AuxSpec.ORDER_STATUS_F /\
    Q21AuxSpec.supplier_count summary > 1 /\
    Q21AuxSpec.late_supplier_count summary = 1 /\
    Q21AuxSpec.sole_late_suppkey summary = sk.
Proof.
  exact Q21ContributionBridge.
Qed.

(** * 2. FoldSpec_nil — empty units ⇒ empty numwait map *)

Lemma FoldSpec_nil_Q21 :
  Q21AuxSpec.FoldSpec nil = nil.
Proof. reflexivity. Qed.

Lemma AuxAnswerSpec_nil_Q21 :
  forall theta, Q21AuxSpec.AuxAnswerSpec nil theta = nil.
Proof. exact AuxAnswerSpec_nil. Qed.

(** * 3. CatalogHashEq — pin matches extracted base_sql *)

Lemma CatalogHashEq_Q21 : catalog_sql_sha256_ok.
Proof. exact catalog_sql_sha256_matches. Qed.

(** * 4. qhat shape — join-spine facade equals Logos source *)

Lemma QhatShapeEq_Q21 :
  qhat_Q21 = Queries.source_query_expr.
Proof. exact qhat_Q21_catalog_instance. Qed.

(** * Open (nonempty) — documented leaf gaps (no trusted holes)

    - ExistsEq_Q21: FormalSQL EXISTS/NOT EXISTS ↔ sole-late on EmbedDB
    - NameJoinEq_Q21: AuxSpec (suppkey,numwait) ↔ FormalSQL (s_name,numwait)
    - Multi-table EmbedDB packing from units + θ
*)
