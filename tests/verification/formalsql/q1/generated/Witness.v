From SQLFS Require Import SqlSyntax GenericInstance SchemaConstraints.
From LogosGenerated Require Import Schema.

(** No PostgreSQL candidate was losslessly frozen for this proof run.  Keeping
a fixed, unavailable witness makes the countermodel branch fail closed; an
agent may still prove the independent equivalence branch. *)
Definition generated_witness_available : bool := false.
Definition generated_witness_db : db_state := Schema.generated_schema.

Lemma generated_witness_schema_conforms :
generated_witness_available = true ->
Schema.generated_schema_conforms generated_witness_db.
Proof. discriminate. Qed.
