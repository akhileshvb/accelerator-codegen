(* Q1 RelNode LogicalSpec export — sole query-specific logical-query Rocq DATA.
   serialize_LogicalSpec_Q1_rel_export_json is the authority for Dafny RelNode DATA. *)

From Stdlib Require Import List ZArith String.
Import ListNotations.
Local Open Scope string_scope.
Local Open Scope Z_scope.

From FormalSQLLogical Require Import Expr Agg RelNode LogicalSpecSerialize.
From FormalSQLResultSpec Require Import QueryResultSpec.
Require Import Q1Schema.
Require Import Q1LogicalSpecTree.

(** Full dump: named pieces + tree + catalog Params (δ default 90). *)
Definition serialize_LogicalSpec_Q1_rel_export_json (catalog_delta : Z) : string :=
  let c := cutoff_Q1 catalog_delta in
  "{""version"":""logical_spec_relnode_v1"""
  ++ ",""table_lineitem"":""" ++ table_lineitem ++ """"
  ++ ",""pred_shipdate"":" ++ expr_to_json q1_pred_shipdate
  ++ ",""group_exprs"":" ++ list_to_json expr_to_json q1_group_exprs
  ++ ",""aggs"":" ++ list_to_json agg_to_json q1_aggs
  ++ ",""project_exprs"":" ++ list_to_json expr_to_json q1_project_exprs
  ++ ",""order_keys"":" ++ list_to_json order_key_to_json q1_order_keys
  ++ ",""tree"":" ++ rel_to_json LogicalSpec_Q1_rel
  ++ ",""catalog_params"":{""cutoff"":" ++ Z_to_string c
  ++ ",""base_date"":" ++ Z_to_string q1_base_date ++ "}"
  ++ ",""catalog_delta"":" ++ Z_to_string catalog_delta
  ++ "}".
