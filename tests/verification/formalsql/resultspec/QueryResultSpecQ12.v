(* QueryResultSpec — Q12 result-only IR (join + CASE counts).
   Contains NO accelerator auxiliary state. *)

From Stdlib Require Import List ZArith String.
Import ListNotations.
Open Scope Z_scope.
Open Scope string_scope.

(** Catalog DATE '1995-01-01' as days-from-epoch (Logos CstDate 9131). *)
Definition q12_catalog_start : Z := 9131.

(** Window end: start + 1 year (accelerator AddMonths(start, 12);
    FormalSQL uses TimestampAdd Year 1 on the cast start date). *)
Definition q12_window_end (start : Z) : Z := start + 365. (* approx marker; Eval uses AddMonths in Dafny *)

Inductive Q12AggKind : Type :=
| Q12HighLineCount
| Q12LowLineCount.

Record QueryResultSpec_Q12 : Type := mkQueryResultSpec_Q12 {
  qrs_start : Z;
  qrs_shipmode1 : string;
  qrs_shipmode2 : string;
  qrs_group_by_shipmode : bool;
  qrs_aggs : list Q12AggKind;
  qrs_order_by_shipmode : bool
}.

Definition S_Q12 (start : Z) (m1 m2 : string) : QueryResultSpec_Q12 :=
  mkQueryResultSpec_Q12
    start m1 m2
    true
    [Q12HighLineCount; Q12LowLineCount]
    true.

(** Catalog instance: MAIL/SHIP / DATE '1995-01-01'. *)
Definition S_Q12_catalog : QueryResultSpec_Q12 :=
  S_Q12 q12_catalog_start "MAIL" "SHIP".

Lemma S_Q12_catalog_start_eq : qrs_start S_Q12_catalog = 9131.
Proof. reflexivity. Qed.
