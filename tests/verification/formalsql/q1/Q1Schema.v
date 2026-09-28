(* Q1Schema — lineitem catalog + Aux Row ↔ generic Row conversion.
   Query-specific. Generic LogicalSpec modules must not import this for
   interpreter semantics. *)

From Stdlib Require Import List ZArith String.
Import ListNotations.
Open Scope Z_scope.
Open Scope string_scope.

From FormalSQLLogical Require Import SqlTypes Schema Typing.
From JetStreamFS Require Import Q1AuxSpec.

Definition table_lineitem : TableId := "lineitem".

(** Positional schema for Scan(lineitem):
    0 shipdate, 1 qty, 2 price, 3 disc, 4 tax, 5 rf, 6 ls *)
Definition attr (name : string) (ty : SqlType) : Attr :=
  mkAttr name ty false.

Definition schema_lineitem : Schema :=
  [attr "l_shipdate" TDate;
   attr "l_quantity" (TNumeric 2);
   attr "l_extendedprice" (TNumeric 2);
   attr "l_discount" (TNumeric 2);
   attr "l_tax" (TNumeric 2);
   attr "l_returnflag" TInt;
   attr "l_linestatus" TInt].

Definition catalog_Q1 : Catalog :=
  [(table_lineitem, schema_lineitem)].

(** Slot indices *)
Definition slot_shipdate : nat := 0.
Definition slot_qty : nat := 1.
Definition slot_price : nat := 2.
Definition slot_disc : nat := 3.
Definition slot_tax : nat := 4.
Definition slot_rf : nat := 5.
Definition slot_ls : nat := 6.

Definition row_of_aux (r : Q1AuxSpec.Row) : Row :=
  [VInt (Q1AuxSpec.shipdate r);
   VInt (Q1AuxSpec.qty r);
   VInt (Q1AuxSpec.price r);
   VInt (Q1AuxSpec.disc r);
   VInt (Q1AuxSpec.tax r);
   VInt (Q1AuxSpec.rf r);
   VInt (Q1AuxSpec.ls r)].

Definition db_of_aux (rs : list Q1AuxSpec.Row) : DB :=
  db_singleton table_lineitem (map row_of_aux rs).

Definition param_cutoff (c : Z) : Params :=
  [("cutoff", VInt c)].
