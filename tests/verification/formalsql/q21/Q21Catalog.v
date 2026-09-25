(* Q21 catalog pin — filled only by Logos lower of exact Cat(Q21).
   catalog_sql_sha256 must match query_catalog.cpp tpch_q21 base_sql. *)

From Stdlib Require Import String.
Open Scope string_scope.

(** SHA-256 of tests/verification/formalsql/q21/catalog_base_sql.sql *)
Definition catalog_sql_sha256 : string :=
  "58f4982b93bd765675f5f9b4f97c9aaa1ee5e4a5f5c5bcd6e147fe915648318a".

(** Logos-lowered FormalSQL artifacts live in formalsql/q21/generated/
   (Schema.v / Queries.v / Witness.v), produced by scripts/logos_lower_catalog.sh. *)
