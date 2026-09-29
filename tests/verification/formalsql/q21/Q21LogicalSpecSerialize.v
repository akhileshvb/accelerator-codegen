(* Q21 RelNode LogicalSpec export — Rocq DATA authority for Dafny RelNode DATA. *)

From Stdlib Require Import List ZArith String.
Import ListNotations.
Local Open Scope string_scope.

From FormalSQLLogical Require Import Expr Agg RelNode LogicalSpecSerialize.
Require Import Q21Schema.
Require Import Q21LogicalSpecTree.

Definition serialize_LogicalSpec_Q21_rel_export_json : string :=
  "{""version"":""logical_spec_relnode_v1"""
  ++ ",""query"":""q21"""
  ++ ",""param_nation_name"":""nation_name"""
  ++ ",""catalog_nation_name"":""" ++ catalog_nation_name ++ """"
  ++ ",""outer_width"":" ++ nat_to_string outer_width
  ++ ",""slot_map"":{"
  ++ """s_suppkey"":" ++ nat_to_string slot_s_suppkey
  ++ ",""s_name"":" ++ nat_to_string slot_s_name
  ++ ",""s_nationkey"":" ++ nat_to_string slot_s_nationkey
  ++ ",""l1_orderkey"":" ++ nat_to_string slot_l1_orderkey
  ++ ",""l1_suppkey"":" ++ nat_to_string slot_l1_suppkey
  ++ ",""l1_commitdate"":" ++ nat_to_string slot_l1_commitdate
  ++ ",""l1_receiptdate"":" ++ nat_to_string slot_l1_receiptdate
  ++ ",""o_orderkey"":" ++ nat_to_string slot_o_orderkey
  ++ ",""o_orderstatus"":" ++ nat_to_string slot_o_orderstatus
  ++ ",""n_nationkey"":" ++ nat_to_string slot_n_nationkey
  ++ ",""n_name"":" ++ nat_to_string slot_n_name
  ++ ",""r_orderkey"":" ++ nat_to_string slot_r_orderkey
  ++ ",""r_suppkey"":" ++ nat_to_string slot_r_suppkey
  ++ "}"
  ++ ",""group_exprs"":" ++ list_to_json expr_to_json q21_group_exprs
  ++ ",""aggs"":" ++ list_to_json agg_to_json q21_aggs
  ++ ",""project_exprs"":" ++ list_to_json expr_to_json q21_project_exprs
  ++ ",""order_keys"":" ++ list_to_json order_key_to_json q21_order_keys
  ++ ",""tree"":" ++ rel_to_json LogicalSpec_Q21_rel
  ++ "}".
