(* QueryResultSpec — shallow result-only IR for JetStream FormalSQL fragment.
   Q1 PoC: TFGOP (Table→Filter→Project→Group→Order→Project) only.
   Contains NO accelerator auxiliary state. *)

From Stdlib Require Import List ZArith.
Import ListNotations.
Open Scope Z_scope.

(** * Canonical Q1 result-spec (parameterized by shipdate cutoff day). *)

(** Fixed Q1 shape descriptors (mirrors Logos TFGOP leaves; not aux state). *)
Inductive Q1AggKind : Type :=
| Q1SumQty | Q1SumBase | Q1SumDiscPrice | Q1SumCharge
| Q1AvgQty | Q1AvgPrice | Q1AvgDisc
| Q1CountStar.

Record QueryResultSpec_Q1 : Type := mkQueryResultSpec_Q1 {
  (** Shipdate cutoff day: rows with l_shipdate <= qrs_cutoff contribute. *)
  qrs_cutoff : Z;
  (** Group keys: returnflag, linestatus (byte codes). *)
  qrs_group_by_rf_ls : bool;
  (** Aggregates in FormalSQL / Logos order. *)
  qrs_aggs : list Q1AggKind;
  (** ORDER BY (rf, ls) ascending. *)
  qrs_order_by_rf_ls : bool
}.

(** Standard Q1 Spec at a concrete cutoff. *)
Definition S_Q1 (cutoff : Z) : QueryResultSpec_Q1 :=
  mkQueryResultSpec_Q1
    cutoff
    true
    [Q1SumQty; Q1SumBase; Q1SumDiscPrice; Q1SumCharge;
     Q1AvgQty; Q1AvgPrice; Q1AvgDisc; Q1CountStar]
    true.

(** TPC-H Q1 base date DATE '1998-12-01' as days-from-epoch used by Logos. *)
Definition q1_base_date : Z := 10561.

(** Parameter bridge: catalog INTERVAL delta days → shipdate cutoff. *)
Definition cutoff_Q1 (delta : Z) : Z := q1_base_date - delta.

Lemma cutoff_Q1_catalog_90 : cutoff_Q1 90 = 10471.
Proof. reflexivity. Qed.
