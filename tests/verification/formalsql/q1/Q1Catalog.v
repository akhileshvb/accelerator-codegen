(* Q1 catalog pin — filled only by Logos lower of exact Cat(Q1).
   catalog_sql_sha256 must match query_catalog.cpp tpch_q1 base_sql. *)

From Stdlib Require Import String.
Open Scope string_scope.

(** SHA-256 of tests/verification/formalsql/q1/catalog_base_sql.sql *)
Definition catalog_sql_sha256 : string :=
  "25c5f92b4fb8cc4d63dbf09505868cf8c25a6dc9b6bf8638668f5c51af48340d".

(** Logos-lowered FormalSQL artifacts live in formalsql/q1/generated/
   (Schema.v / Queries.v / Witness.v), produced by scripts/logos_lower_catalog.sh. *)
