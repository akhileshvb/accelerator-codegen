From SQLFS Require Import SqlSyntax GenericInstance ValueCore ValueInteger ValueString Formula SchemaConstraints.
From Logos Require Import FormalSQL.TNullSyntax FormalSQL.WitnessFacts.
From Stdlib Require Import String ZArith List Lia.
Import ListNotations.
Open Scope string_scope.
Open Scope Z_scope.

Definition generated_sql_default_collation : string := "C".
Definition generated_sql_character_classification : string := "C".
Definition generated_sql_locale_provider : string := "libc".
Definition generated_sql_server_encoding : string := "UTF8".

Definition generated_schema :=
  create_table
    (init_db)
    (Rel "lineitem")
    (Attr_int64 "l_orderkey" :: Attr_int64 "l_partkey" :: Attr_int64 "l_suppkey" :: Attr_int32 "l_linenumber" :: Attr_decimal "l_quantity" 15 2 :: Attr_decimal "l_extendedprice" 15 2 :: Attr_decimal "l_discount" 15 2 :: Attr_decimal "l_tax" 15 2 :: Attr_string "l_returnflag" (StringChar 1) :: Attr_string "l_linestatus" (StringChar 1) :: Attr_date "l_shipdate" :: Attr_date "l_commitdate" :: Attr_date "l_receiptdate" :: Attr_string "l_shipinstruct" (StringChar 25) :: Attr_string "l_shipmode" (StringChar 10) :: Attr_string "l_comment" (StringVarcharN 44) :: nil).

Definition generated_table_constraint_0 : table_constraint :=
  TableConstraint
    (Rel "lineitem")
    (Attr_int64 "l_orderkey" :: Attr_int64 "l_partkey" :: Attr_int64 "l_suppkey" :: Attr_int32 "l_linenumber" :: Attr_decimal "l_quantity" 15 2 :: Attr_decimal "l_extendedprice" 15 2 :: Attr_decimal "l_discount" 15 2 :: Attr_decimal "l_tax" 15 2 :: Attr_string "l_returnflag" (StringChar 1) :: Attr_string "l_linestatus" (StringChar 1) :: Attr_date "l_shipdate" :: Attr_date "l_commitdate" :: Attr_date "l_receiptdate" :: Attr_string "l_shipinstruct" (StringChar 25) :: Attr_string "l_shipmode" (StringChar 10) :: Attr_string "l_comment" (StringVarcharN 44) :: nil)
    (None)
    ([])
    ([])
    ([])
    ([]).

Definition generated_schema_constraints : list table_constraint :=
  [generated_table_constraint_0].

Lemma generated_table_constraint_0_primary_key :
  constraint_primary_key generated_table_constraint_0 = (None).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_0_unique_keys :
  constraint_unique_keys generated_table_constraint_0 = ([]).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_0_foreign_keys :
  constraint_foreign_keys generated_table_constraint_0 = ([]).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_0_checks :
  constraint_checks generated_table_constraint_0 = ([]).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_0_unique_indexes :
  constraint_unique_indexes generated_table_constraint_0 = ([]).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_0_declarations_well_formed :
table_constraint_declarations_well_formed
generated_schema_constraints generated_table_constraint_0.
Proof.
apply table_constraint_declarations_well_formedb_sound.
vm_compute.
reflexivity.
Qed.

Lemma generated_schema_constraints_well_formed :
schema_constraints_well_formed generated_schema_constraints.
Proof.
unfold schema_constraints_well_formed, generated_schema_constraints.
  constructor; [exact generated_table_constraint_0_declarations_well_formed|].
  constructor.
Qed.

Definition generated_schema_conforms (db : db_state) : Prop :=
  database_conforms_schema
    generated_schema generated_schema_constraints db.
