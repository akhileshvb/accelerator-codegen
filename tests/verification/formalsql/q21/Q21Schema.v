(* Q21Schema — multi-table catalog + nation_name params.
   Watched logical attributes only (exactness of keys; no physical packing). *)

From Stdlib Require Import List ZArith String.
Import ListNotations.
Open Scope string_scope.

From FormalSQLLogical Require Import SqlTypes Schema Expr Typing.

Definition table_supplier : TableId := "supplier".
Definition table_lineitem : TableId := "lineitem".
Definition table_orders : TableId := "orders".
Definition table_nation : TableId := "nation".

Definition attr (name : string) (ty : SqlType) : Attr :=
  mkAttr name ty false.

(** supplier: 0 s_suppkey, 1 s_name, 2 s_nationkey *)
Definition schema_supplier : Schema :=
  [attr "s_suppkey" TInt;
   attr "s_name" TText;
   attr "s_nationkey" TInt].

(** lineitem (l1/l2/l3): 0 l_orderkey, 1 l_suppkey, 2 l_commitdate, 3 l_receiptdate *)
Definition schema_lineitem : Schema :=
  [attr "l_orderkey" TInt;
   attr "l_suppkey" TInt;
   attr "l_commitdate" TDate;
   attr "l_receiptdate" TDate].

(** orders: 0 o_orderkey, 1 o_orderstatus *)
Definition schema_orders : Schema :=
  [attr "o_orderkey" TInt;
   attr "o_orderstatus" TText].

(** nation: 0 n_nationkey, 1 n_name *)
Definition schema_nation : Schema :=
  [attr "n_nationkey" TInt;
   attr "n_name" TText].

Definition catalog_Q21 : Catalog :=
  [(table_supplier, schema_supplier);
   (table_lineitem, schema_lineitem);
   (table_orders, schema_orders);
   (table_nation, schema_nation)].

(** Parameterized nation filter (accelerator interface). *)
Definition param_nation_name : Expr := EParam "nation_name" TText.

Definition params_nation (name : string) : Params :=
  [("nation_name", VText name)].

(** Catalog SQL instance nation literal. *)
Definition catalog_nation_name : string := "SAUDI ARABIA".

Definition params_catalog_nation : Params :=
  params_nation catalog_nation_name.

(** ----- Slot map after outer spine joins (supplier ⋈ l1 ⋈ orders ⋈ nation) -----
    supplier: 0..2
    l1:       3 l_orderkey, 4 l_suppkey, 5 l_commitdate, 6 l_receiptdate
    orders:   7 o_orderkey, 8 o_orderstatus
    nation:   9 n_nationkey, 10 n_name
    Total outer width = 11.
    Semi/Anti right (lineitem): +0..+3 at offsets starting at 11. *)

Definition slot_s_suppkey : nat := 0.
Definition slot_s_name : nat := 1.
Definition slot_s_nationkey : nat := 2.
Definition slot_l1_orderkey : nat := 3.
Definition slot_l1_suppkey : nat := 4.
Definition slot_l1_commitdate : nat := 5.
Definition slot_l1_receiptdate : nat := 6.
Definition slot_o_orderkey : nat := 7.
Definition slot_o_orderstatus : nat := 8.
Definition slot_n_nationkey : nat := 9.
Definition slot_n_name : nat := 10.

Definition outer_width : nat := 11.

(** Right-side lineitem slots in Semi/Anti cond (concatenated layout). *)
Definition slot_r_orderkey : nat := 11.
Definition slot_r_suppkey : nat := 12.
Definition slot_r_commitdate : nat := 13.
Definition slot_r_receiptdate : nat := 14.
