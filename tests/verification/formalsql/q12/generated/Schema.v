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
    (create_table
       (create_table
          (create_table
             (init_db)
             (Rel "nation")
             (Attr_int32 "n_nationkey" :: Attr_string "n_name" (StringChar 25) :: Attr_int32 "n_regionkey" :: Attr_string "n_comment" (StringVarcharN 152) :: nil))
          (Rel "supplier")
          (Attr_int32 "s_suppkey" :: Attr_string "s_name" (StringChar 25) :: Attr_string "s_address" (StringVarcharN 40) :: Attr_int32 "s_nationkey" :: Attr_string "s_phone" (StringChar 15) :: Attr_decimal "s_acctbal" 15 2 :: Attr_string "s_comment" (StringVarcharN 101) :: nil))
       (Rel "orders")
       (Attr_int32 "o_orderkey" :: Attr_int32 "o_custkey" :: Attr_string "o_orderstatus" (StringChar 1) :: Attr_decimal "o_totalprice" 15 2 :: Attr_date "o_orderdate" :: Attr_string "o_orderpriority" (StringChar 15) :: Attr_string "o_clerk" (StringChar 15) :: Attr_int32 "o_shippriority" :: Attr_string "o_comment" (StringVarcharN 79) :: nil))
    (Rel "lineitem")
    (Attr_int32 "l_orderkey" :: Attr_int32 "l_partkey" :: Attr_int32 "l_suppkey" :: Attr_int32 "l_linenumber" :: Attr_decimal "l_quantity" 15 2 :: Attr_decimal "l_extendedprice" 15 2 :: Attr_decimal "l_discount" 15 2 :: Attr_decimal "l_tax" 15 2 :: Attr_string "l_returnflag" (StringChar 1) :: Attr_string "l_linestatus" (StringChar 1) :: Attr_date "l_shipdate" :: Attr_date "l_commitdate" :: Attr_date "l_receiptdate" :: Attr_string "l_shipinstruct" (StringChar 25) :: Attr_string "l_shipmode" (StringChar 10) :: Attr_string "l_comment" (StringVarcharN 44) :: nil).

Definition generated_table_constraint_0 : table_constraint :=
  TableConstraint
    (Rel "nation")
    (Attr_int32 "n_nationkey" :: Attr_string "n_name" (StringChar 25) :: Attr_int32 "n_regionkey" :: Attr_string "n_comment" (StringVarcharN 152) :: nil)
    (None)
    ([])
    ([])
    ([])
    ([]).

Definition generated_table_constraint_1 : table_constraint :=
  TableConstraint
    (Rel "supplier")
    (Attr_int32 "s_suppkey" :: Attr_string "s_name" (StringChar 25) :: Attr_string "s_address" (StringVarcharN 40) :: Attr_int32 "s_nationkey" :: Attr_string "s_phone" (StringChar 15) :: Attr_decimal "s_acctbal" 15 2 :: Attr_string "s_comment" (StringVarcharN 101) :: nil)
    (None)
    ([])
    ([])
    ([])
    ([]).

Definition generated_table_constraint_2 : table_constraint :=
  TableConstraint
    (Rel "orders")
    (Attr_int32 "o_orderkey" :: Attr_int32 "o_custkey" :: Attr_string "o_orderstatus" (StringChar 1) :: Attr_decimal "o_totalprice" 15 2 :: Attr_date "o_orderdate" :: Attr_string "o_orderpriority" (StringChar 15) :: Attr_string "o_clerk" (StringChar 15) :: Attr_int32 "o_shippriority" :: Attr_string "o_comment" (StringVarcharN 79) :: nil)
    (None)
    ([])
    ([])
    ([])
    ([]).

Definition generated_table_constraint_3 : table_constraint :=
  TableConstraint
    (Rel "lineitem")
    (Attr_int32 "l_orderkey" :: Attr_int32 "l_partkey" :: Attr_int32 "l_suppkey" :: Attr_int32 "l_linenumber" :: Attr_decimal "l_quantity" 15 2 :: Attr_decimal "l_extendedprice" 15 2 :: Attr_decimal "l_discount" 15 2 :: Attr_decimal "l_tax" 15 2 :: Attr_string "l_returnflag" (StringChar 1) :: Attr_string "l_linestatus" (StringChar 1) :: Attr_date "l_shipdate" :: Attr_date "l_commitdate" :: Attr_date "l_receiptdate" :: Attr_string "l_shipinstruct" (StringChar 25) :: Attr_string "l_shipmode" (StringChar 10) :: Attr_string "l_comment" (StringVarcharN 44) :: nil)
    (None)
    ([])
    ([])
    ([])
    ([]).

Definition generated_schema_constraints : list table_constraint :=
  [generated_table_constraint_0; generated_table_constraint_1; generated_table_constraint_2; generated_table_constraint_3].

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

Lemma generated_table_constraint_1_primary_key :
  constraint_primary_key generated_table_constraint_1 = (None).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_1_unique_keys :
  constraint_unique_keys generated_table_constraint_1 = ([]).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_1_foreign_keys :
  constraint_foreign_keys generated_table_constraint_1 = ([]).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_1_checks :
  constraint_checks generated_table_constraint_1 = ([]).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_1_unique_indexes :
  constraint_unique_indexes generated_table_constraint_1 = ([]).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_2_primary_key :
  constraint_primary_key generated_table_constraint_2 = (None).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_2_unique_keys :
  constraint_unique_keys generated_table_constraint_2 = ([]).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_2_foreign_keys :
  constraint_foreign_keys generated_table_constraint_2 = ([]).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_2_checks :
  constraint_checks generated_table_constraint_2 = ([]).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_2_unique_indexes :
  constraint_unique_indexes generated_table_constraint_2 = ([]).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_3_primary_key :
  constraint_primary_key generated_table_constraint_3 = (None).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_3_unique_keys :
  constraint_unique_keys generated_table_constraint_3 = ([]).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_3_foreign_keys :
  constraint_foreign_keys generated_table_constraint_3 = ([]).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_3_checks :
  constraint_checks generated_table_constraint_3 = ([]).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_3_unique_indexes :
  constraint_unique_indexes generated_table_constraint_3 = ([]).
Proof. reflexivity. Qed.

Lemma generated_table_constraint_0_declarations_well_formed :
table_constraint_declarations_well_formed
generated_schema_constraints generated_table_constraint_0.
Proof.
apply table_constraint_declarations_well_formedb_sound.
vm_compute.
reflexivity.
Qed.

Lemma generated_table_constraint_1_declarations_well_formed :
table_constraint_declarations_well_formed
generated_schema_constraints generated_table_constraint_1.
Proof.
apply table_constraint_declarations_well_formedb_sound.
vm_compute.
reflexivity.
Qed.

Lemma generated_table_constraint_2_declarations_well_formed :
table_constraint_declarations_well_formed
generated_schema_constraints generated_table_constraint_2.
Proof.
apply table_constraint_declarations_well_formedb_sound.
vm_compute.
reflexivity.
Qed.

Lemma generated_table_constraint_3_declarations_well_formed :
table_constraint_declarations_well_formed
generated_schema_constraints generated_table_constraint_3.
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
  constructor; [exact generated_table_constraint_1_declarations_well_formed|].
  constructor; [exact generated_table_constraint_2_declarations_well_formed|].
  constructor; [exact generated_table_constraint_3_declarations_well_formed|].
  constructor.
Qed.

Definition generated_schema_conforms (db : db_state) : Prop :=
  database_conforms_schema
    generated_schema generated_schema_constraints db.
