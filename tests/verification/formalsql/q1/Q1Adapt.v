(* Q1Adapt — query-specific representation bridge (NOT part of generic Eval_Spec).
   Maps generic Eval_Spec output Rows for LogicalSpec_Q1_rel → Aux OutRows. *)

From Stdlib Require Import List ZArith.
Import ListNotations.
Open Scope Z_scope.

From FormalSQLLogical Require Import SqlTypes Schema Eval.
From JetStreamFS Require Import Q1AuxSpec.
Require Import Q1Schema.
Require Import Q1LogicalSpecTree.

(** Expected projected row layout after Q1 Project:
    [rf; ls; sum_qty; sum_price; sum_disc_price; sum_charge;
     avg_qty; avg_price; avg_disc; count_star]
    where Avg slots are VAvgExact. *)

Definition Q1AdaptRow (r : Row) : option Q1AuxSpec.OutRow :=
  match r with
  | [VInt rf; VInt ls;
     VInt sum_qty; VInt sum_base; VInt sum_disc_price; VInt sum_charge;
     VAvgExact sum_avg_qty c_qty;
     VAvgExact sum_avg_price c_price;
     VAvgExact sum_avg_disc c_disc;
     VInt c_count] =>
      if Z.leb c_count 0 then None
      else
        Some (Q1AuxSpec.mkOutRow rf ls
          sum_qty sum_base sum_disc_price sum_charge
          sum_avg_disc
          (Q1AuxSpec.AvgRound sum_avg_qty c_count)
          (Q1AuxSpec.AvgRound sum_avg_price c_count)
          (Q1AuxSpec.AvgRound sum_avg_disc c_count)
          c_count)
  | _ => None
  end.

Fixpoint Q1Adapt (rows : list Row) : option (list Q1AuxSpec.OutRow) :=
  match rows with
  | [] => Some []
  | r :: rest =>
      match Q1AdaptRow r, Q1Adapt rest with
      | Some o, Some os => Some (o :: os)
      | _, _ => None
      end
  end.

(** Canonical Q1 OutRow view via RelNode Eval_Spec ∘ Q1Adapt. *)
Definition Eval_Spec_Q1_rows_rel (c : Z) (db : list Q1AuxSpec.Row)
  : option (list Q1AuxSpec.OutRow) :=
  match Eval_Spec LogicalSpec_Q1_rel (param_cutoff c) (db_of_aux db) with
  | Some rows => Q1Adapt rows
  | None => None
  end.
