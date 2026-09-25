(* Q12EmbedDots — EmbedOrderRow / EmbedLineRow attr membership + Dot recovery.

   Shared by JoinSpine leaf discharge (identity projects, WHERE acceptance).
   Pattern mirrors Q1AuxCorrectAssembly EmbedRow_dot_*. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueInteger
  ValueTemporal ValueString FiniteSet FTuples OrderedSet FlatData.
From Logos Require Import FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax.
From FormalSQLResultSpec Require Import QueryResultSpecQ12 Q12EvalSpec.
From Stdlib Require Import String List ZArith.
Import ListNotations.
Import Tuple.
Open Scope string_scope.
Open Scope Z_scope.

Require Import Q12NormalizeSound.

Import Q12EvalSpec.

(** * orders attrs inS *)

Lemma orders_attr_inS_orderkey :
  AttrInt32 "o_orderkey" inS Fset.mk_set (A TNull) orders_attrs.
Proof.
  change (AttrInt32 "o_orderkey" inS?
            Fset.mk_set (A TNull) orders_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma orders_attr_inS_priority :
  AttrString "o_orderpriority" (StringChar 15)
    inS Fset.mk_set (A TNull) orders_attrs.
Proof.
  change (AttrString "o_orderpriority" (StringChar 15) inS?
            Fset.mk_set (A TNull) orders_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma orders_attr_inS_custkey :
  AttrInt32 "o_custkey" inS Fset.mk_set (A TNull) orders_attrs.
Proof.
  change (AttrInt32 "o_custkey" inS?
            Fset.mk_set (A TNull) orders_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma orders_attr_inS_orderstatus :
  AttrString "o_orderstatus" (StringChar 1)
    inS Fset.mk_set (A TNull) orders_attrs.
Proof.
  change (AttrString "o_orderstatus" (StringChar 1) inS?
            Fset.mk_set (A TNull) orders_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma orders_attr_inS_totalprice :
  AttrDecimal "o_totalprice" 15 2 inS Fset.mk_set (A TNull) orders_attrs.
Proof.
  change (AttrDecimal "o_totalprice" 15 2 inS?
            Fset.mk_set (A TNull) orders_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma orders_attr_inS_orderdate :
  AttrDate "o_orderdate" inS Fset.mk_set (A TNull) orders_attrs.
Proof.
  change (AttrDate "o_orderdate" inS?
            Fset.mk_set (A TNull) orders_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma orders_attr_inS_clerk :
  AttrString "o_clerk" (StringChar 15)
    inS Fset.mk_set (A TNull) orders_attrs.
Proof.
  change (AttrString "o_clerk" (StringChar 15) inS?
            Fset.mk_set (A TNull) orders_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma orders_attr_inS_shippriority :
  AttrInt32 "o_shippriority" inS Fset.mk_set (A TNull) orders_attrs.
Proof.
  change (AttrInt32 "o_shippriority" inS?
            Fset.mk_set (A TNull) orders_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma orders_attr_inS_comment :
  AttrString "o_comment" (StringVarcharN 79)
    inS Fset.mk_set (A TNull) orders_attrs.
Proof.
  change (AttrString "o_comment" (StringVarcharN 79) inS?
            Fset.mk_set (A TNull) orders_attrs = true).
  vm_compute; reflexivity.
Qed.

(** * lineitem attrs inS *)

Lemma lineitem_attr_inS_orderkey :
  AttrInt32 "l_orderkey" inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrInt32 "l_orderkey" inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_shipmode :
  AttrString "l_shipmode" (StringChar 10)
    inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrString "l_shipmode" (StringChar 10) inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_shipdate :
  AttrDate "l_shipdate" inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrDate "l_shipdate" inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_commitdate :
  AttrDate "l_commitdate" inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrDate "l_commitdate" inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_receiptdate :
  AttrDate "l_receiptdate" inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrDate "l_receiptdate" inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_partkey :
  AttrInt32 "l_partkey" inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrInt32 "l_partkey" inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_suppkey :
  AttrInt32 "l_suppkey" inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrInt32 "l_suppkey" inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_linenumber :
  AttrInt32 "l_linenumber" inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrInt32 "l_linenumber" inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_quantity :
  AttrDecimal "l_quantity" 15 2 inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrDecimal "l_quantity" 15 2 inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_extendedprice :
  AttrDecimal "l_extendedprice" 15 2 inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrDecimal "l_extendedprice" 15 2 inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_discount :
  AttrDecimal "l_discount" 15 2 inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrDecimal "l_discount" 15 2 inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_tax :
  AttrDecimal "l_tax" 15 2 inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrDecimal "l_tax" 15 2 inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_returnflag :
  AttrString "l_returnflag" (StringChar 1)
    inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrString "l_returnflag" (StringChar 1) inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_linestatus :
  AttrString "l_linestatus" (StringChar 1)
    inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrString "l_linestatus" (StringChar 1) inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_shipinstruct :
  AttrString "l_shipinstruct" (StringChar 25)
    inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrString "l_shipinstruct" (StringChar 25) inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

Lemma lineitem_attr_inS_comment :
  AttrString "l_comment" (StringVarcharN 44)
    inS Fset.mk_set (A TNull) lineitem_attrs.
Proof.
  change (AttrString "l_comment" (StringVarcharN 44) inS?
            Fset.mk_set (A TNull) lineitem_attrs = true).
  vm_compute; reflexivity.
Qed.

(** * Labels of embedded rows *)

Lemma EmbedOrderRow_attr_inS :
  forall o (attr : attribute TNull),
    attr inS Fset.mk_set (A TNull) orders_attrs ->
    attr inS labels TNull (EmbedOrderRow o).
Proof.
  intros o attr Hin.
  unfold EmbedOrderRow.
  match goal with
  | |- context [mk_tuple_lists ?atts ?vals] =>
      rewrite (Fset.mem_eq_2 _ _ _
        (mk_tuple_lists_labels atts vals))
  end.
  exact Hin.
Qed.

Lemma EmbedLineRow_attr_inS :
  forall l (attr : attribute TNull),
    attr inS Fset.mk_set (A TNull) lineitem_attrs ->
    attr inS labels TNull (EmbedLineRow l).
Proof.
  intros l attr Hin.
  unfold EmbedLineRow.
  match goal with
  | |- context [mk_tuple_lists ?atts ?vals] =>
      rewrite (Fset.mem_eq_2 _ _ _
        (mk_tuple_lists_labels atts vals))
  end.
  exact Hin.
Qed.

(** * Dot recovery — orders *)

Lemma EmbedOrderRow_dot_orderkey :
  forall o,
    dot TNull (EmbedOrderRow o) (AttrInt32 "o_orderkey") =
    Value_int32 (Some (int32_from_twos_complement (o_orderkey o))).
Proof.
  intro o; unfold EmbedOrderRow.
  rewrite mk_tuple_lists_dot by apply orders_attr_inS_orderkey.
  reflexivity.
Qed.

Lemma EmbedOrderRow_dot_priority :
  forall o,
    dot TNull (EmbedOrderRow o) (AttrString "o_orderpriority" (StringChar 15)) =
    embed_string_char15 (o_priority o).
Proof.
  intro o; unfold EmbedOrderRow.
  rewrite mk_tuple_lists_dot by apply orders_attr_inS_priority.
  reflexivity.
Qed.

Lemma EmbedOrderRow_dot_custkey :
  forall o,
    dot TNull (EmbedOrderRow o) (AttrInt32 "o_custkey") =
    Value_int32 (Some zero_int32).
Proof.
  intro o; unfold EmbedOrderRow.
  rewrite mk_tuple_lists_dot by apply orders_attr_inS_custkey.
  reflexivity.
Qed.

Lemma EmbedOrderRow_dot_orderstatus :
  forall o,
    dot TNull (EmbedOrderRow o) (AttrString "o_orderstatus" (StringChar 1)) =
    embed_empty_char1.
Proof.
  intro o; unfold EmbedOrderRow.
  rewrite mk_tuple_lists_dot by apply orders_attr_inS_orderstatus.
  reflexivity.
Qed.

Lemma EmbedOrderRow_dot_totalprice :
  forall o,
    dot TNull (EmbedOrderRow o) (AttrDecimal "o_totalprice" 15 2) =
    embed_decimal0.
Proof.
  intro o; unfold EmbedOrderRow.
  rewrite mk_tuple_lists_dot by apply orders_attr_inS_totalprice.
  reflexivity.
Qed.

Lemma EmbedOrderRow_dot_orderdate :
  forall o,
    dot TNull (EmbedOrderRow o) (AttrDate "o_orderdate") =
    Value_date (Some 0).
Proof.
  intro o; unfold EmbedOrderRow.
  rewrite mk_tuple_lists_dot by apply orders_attr_inS_orderdate.
  reflexivity.
Qed.

Lemma EmbedOrderRow_dot_clerk :
  forall o,
    dot TNull (EmbedOrderRow o) (AttrString "o_clerk" (StringChar 15)) =
    embed_string_char15 "".
Proof.
  intro o; unfold EmbedOrderRow.
  rewrite mk_tuple_lists_dot by apply orders_attr_inS_clerk.
  reflexivity.
Qed.

Lemma EmbedOrderRow_dot_shippriority :
  forall o,
    dot TNull (EmbedOrderRow o) (AttrInt32 "o_shippriority") =
    Value_int32 (Some zero_int32).
Proof.
  intro o; unfold EmbedOrderRow.
  rewrite mk_tuple_lists_dot by apply orders_attr_inS_shippriority.
  reflexivity.
Qed.

Lemma EmbedOrderRow_dot_comment :
  forall o,
    dot TNull (EmbedOrderRow o) (AttrString "o_comment" (StringVarcharN 79)) =
    embed_empty_varchar79.
Proof.
  intro o; unfold EmbedOrderRow.
  rewrite mk_tuple_lists_dot by apply orders_attr_inS_comment.
  reflexivity.
Qed.

(** * Dot recovery — lineitem *)

Lemma EmbedLineRow_dot_orderkey :
  forall l,
    dot TNull (EmbedLineRow l) (AttrInt32 "l_orderkey") =
    Value_int32 (Some (int32_from_twos_complement (l_orderkey l))).
Proof.
  intro l; unfold EmbedLineRow.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_orderkey.
  reflexivity.
Qed.

Lemma EmbedLineRow_dot_shipmode :
  forall l,
    dot TNull (EmbedLineRow l) (AttrString "l_shipmode" (StringChar 10)) =
    embed_string_char10 (l_shipmode l).
Proof.
  intro l; unfold EmbedLineRow.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_shipmode.
  reflexivity.
Qed.

Lemma EmbedLineRow_dot_shipdate :
  forall l,
    dot TNull (EmbedLineRow l) (AttrDate "l_shipdate") =
    Value_date (Some (l_shipdate l)).
Proof.
  intro l; unfold EmbedLineRow.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_shipdate.
  reflexivity.
Qed.

Lemma EmbedLineRow_dot_commitdate :
  forall l,
    dot TNull (EmbedLineRow l) (AttrDate "l_commitdate") =
    Value_date (Some (l_commitdate l)).
Proof.
  intro l; unfold EmbedLineRow.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_commitdate.
  reflexivity.
Qed.

Lemma EmbedLineRow_dot_receiptdate :
  forall l,
    dot TNull (EmbedLineRow l) (AttrDate "l_receiptdate") =
    Value_date (Some (l_receiptdate l)).
Proof.
  intro l; unfold EmbedLineRow.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_receiptdate.
  reflexivity.
Qed.

Lemma EmbedLineRow_dot_partkey :
  forall l,
    dot TNull (EmbedLineRow l) (AttrInt32 "l_partkey") =
    Value_int32 (Some zero_int32).
Proof.
  intro l; unfold EmbedLineRow.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_partkey.
  reflexivity.
Qed.

Lemma EmbedLineRow_dot_suppkey :
  forall l,
    dot TNull (EmbedLineRow l) (AttrInt32 "l_suppkey") =
    Value_int32 (Some zero_int32).
Proof.
  intro l; unfold EmbedLineRow.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_suppkey.
  reflexivity.
Qed.

Lemma EmbedLineRow_dot_linenumber :
  forall l,
    dot TNull (EmbedLineRow l) (AttrInt32 "l_linenumber") =
    Value_int32 (Some zero_int32).
Proof.
  intro l; unfold EmbedLineRow.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_linenumber.
  reflexivity.
Qed.

Lemma EmbedLineRow_dot_quantity :
  forall l,
    dot TNull (EmbedLineRow l) (AttrDecimal "l_quantity" 15 2) =
    embed_decimal0.
Proof.
  intro l; unfold EmbedLineRow.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_quantity.
  reflexivity.
Qed.

Lemma EmbedLineRow_dot_extendedprice :
  forall l,
    dot TNull (EmbedLineRow l) (AttrDecimal "l_extendedprice" 15 2) =
    embed_decimal0.
Proof.
  intro l; unfold EmbedLineRow.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_extendedprice.
  reflexivity.
Qed.

Lemma EmbedLineRow_dot_discount :
  forall l,
    dot TNull (EmbedLineRow l) (AttrDecimal "l_discount" 15 2) =
    embed_decimal0.
Proof.
  intro l; unfold EmbedLineRow.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_discount.
  reflexivity.
Qed.

Lemma EmbedLineRow_dot_tax :
  forall l,
    dot TNull (EmbedLineRow l) (AttrDecimal "l_tax" 15 2) =
    embed_decimal0.
Proof.
  intro l; unfold EmbedLineRow.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_tax.
  reflexivity.
Qed.

Lemma EmbedLineRow_dot_returnflag :
  forall l,
    dot TNull (EmbedLineRow l) (AttrString "l_returnflag" (StringChar 1)) =
    Value_string (StringValue (StringChar 1) (Some "N")).
Proof.
  intro l; unfold EmbedLineRow.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_returnflag.
  reflexivity.
Qed.

Lemma EmbedLineRow_dot_linestatus :
  forall l,
    dot TNull (EmbedLineRow l) (AttrString "l_linestatus" (StringChar 1)) =
    Value_string (StringValue (StringChar 1) (Some "O")).
Proof.
  intro l; unfold EmbedLineRow.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_linestatus.
  reflexivity.
Qed.

Lemma EmbedLineRow_dot_shipinstruct :
  forall l,
    dot TNull (EmbedLineRow l) (AttrString "l_shipinstruct" (StringChar 25)) =
    embed_empty_char25.
Proof.
  intro l; unfold EmbedLineRow.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_shipinstruct.
  reflexivity.
Qed.

Lemma EmbedLineRow_dot_comment :
  forall l,
    dot TNull (EmbedLineRow l) (AttrString "l_comment" (StringVarcharN 44)) =
    embed_empty_varchar44.
Proof.
  intro l; unfold EmbedLineRow.
  rewrite mk_tuple_lists_dot by apply lineitem_attr_inS_comment.
  reflexivity.
Qed.
