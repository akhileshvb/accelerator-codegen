(* Schema / Row / Params / DB — generic logical substrate.
   No query-specific column names, Pack, or physical layout. *)

From Stdlib Require Import List ZArith String Bool Nat.
Import ListNotations.
Open Scope string_scope.

From FormalSQLLogical Require Import SqlTypes.

(** Opaque table identifier (catalog name). *)
Definition TableId : Type := string.

(** Attribute in a schema. *)
Record Attr : Type := mkAttr {
  attr_name : string;
  attr_type : SqlType;
  attr_nullable : bool
}.

Definition Schema : Type := list Attr.

(** Logical row = positional tuple of SqlValue (bag element). *)
Definition Row : Type := list SqlValue.

Definition row_nth (r : Row) (i : nat) : option SqlValue :=
  nth_error r i.

(** Generic parameter environment: named SqlValues. *)
Definition Params : Type := list (string * SqlValue).

Fixpoint params_lookup (p : Params) (id : string) : option SqlValue :=
  match p with
  | [] => None
  | (k, v) :: rest => if String.eqb k id then Some v else params_lookup rest id
  end.

(** Database: named bags of rows (logical tables). *)
Definition DB : Type := list (TableId * list Row).

Fixpoint db_lookup (db : DB) (t : TableId) : option (list Row) :=
  match db with
  | [] => None
  | (k, rows) :: rest =>
      if String.eqb k t then Some rows else db_lookup rest t
  end.

(** Single-table DB helper (common for one-scan queries). *)
Definition db_singleton (t : TableId) (rows : list Row) : DB :=
  [(t, rows)].

Definition schema_length (s : Schema) : nat := List.length s.

Definition row_matches_schema (s : Schema) (r : Row) : bool :=
  Nat.eqb (List.length s) (List.length r).
