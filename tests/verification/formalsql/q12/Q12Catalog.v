(* Q12 catalog pin — filled only by Logos lower of exact Cat(Q12).
   catalog_sql_sha256 must match query_catalog.cpp tpch_q12 base_sql. *)

From Stdlib Require Import String.
Open Scope string_scope.

(** SHA-256 of tests/verification/formalsql/q12/catalog_base_sql.sql *)
Definition catalog_sql_sha256 : string :=
  "4d783e0eeb8050b8d083f505a6ee8bd6aa96a382531e33108c539bbd6e2f3f42".

(** Logos-lowered FormalSQL artifacts live in formalsql/q12/generated/
   (Schema.v / Queries.v / Witness.v), produced by scripts/logos_lower_catalog.sh. *)
