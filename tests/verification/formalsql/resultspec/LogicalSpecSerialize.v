(* Deterministic JSON serialization of expression-IR LogicalSpec.
   Pure function — dump scripts Compute this string; no hardcoded catalog bodies.
   Template is parameterized (EParam); catalog Params are serialized separately. *)

From Stdlib Require Import List ZArith String Nat.
Import ListNotations.
Local Open Scope string_scope.
Local Open Scope Z_scope.

From FormalSQLResultSpec Require Import LogicalSpec QueryResultSpec.

Definition digit_to_string (d : nat) : string :=
  match d with
  | 0%nat => "0" | 1%nat => "1" | 2%nat => "2" | 3%nat => "3" | 4%nat => "4"
  | 5%nat => "5" | 6%nat => "6" | 7%nat => "7" | 8%nat => "8" | _ => "9"
  end.

Fixpoint nat_to_string_aux (n fuel : nat) (acc : string) : string :=
  match fuel with
  | 0%nat => acc
  | S fuel' =>
      let d := Nat.modulo n 10 in
      let n' := Nat.div n 10 in
      let acc' := digit_to_string d ++ acc in
      if Nat.eqb n' 0 then acc' else nat_to_string_aux n' fuel' acc'
  end.

Definition nat_to_string (n : nat) : string :=
  if Nat.eqb n 0 then "0" else nat_to_string_aux n (S n) "".

Definition Z_to_string (z : Z) : string :=
  if Z.ltb z 0 then "-" ++ nat_to_string (Z.to_nat (- z))
  else nat_to_string (Z.to_nat z).

Definition comma_join (xs : list string) : string :=
  match xs with
  | [] => ""
  | x :: xs' => fold_left (fun acc y => acc ++ "," ++ y) xs' x
  end.

Definition scale_to_json (s : Scale) : string :=
  match s with
  | Scale_0 => "0" | Scale_2 => "2" | Scale_4 => "4" | Scale_6 => "6"
  end.

Definition column_to_json (c : ColumnId) : string :=
  """" ++ column_name c ++ """".

Definition param_to_json (p : ParamId) : string :=
  """" ++ param_name p ++ """".

Fixpoint expr_to_json (e : Expr) : string :=
  match e with
  | EColumn c =>
      "{""kind"":""column"",""name"":" ++ column_to_json c
      ++ ",""scale"":" ++ scale_to_json (column_scale c) ++ "}"
  | EConst z s =>
      "{""kind"":""const"",""value"":" ++ Z_to_string z
      ++ ",""scale"":" ++ scale_to_json s ++ "}"
  | EParam id s =>
      "{""kind"":""param"",""id"":" ++ param_to_json id
      ++ ",""scale"":" ++ scale_to_json s ++ "}"
  | EAdd a b =>
      "{""kind"":""add"",""left"":" ++ expr_to_json a
      ++ ",""right"":" ++ expr_to_json b ++ "}"
  | ESub a b =>
      "{""kind"":""sub"",""left"":" ++ expr_to_json a
      ++ ",""right"":" ++ expr_to_json b ++ "}"
  | EMul a b =>
      "{""kind"":""mul"",""left"":" ++ expr_to_json a
      ++ ",""right"":" ++ expr_to_json b ++ "}"
  end.

Definition pred_to_json (p : Pred) : string :=
  match p with
  | PLe a b =>
      "{""kind"":""le"",""left"":" ++ expr_to_json a
      ++ ",""right"":" ++ expr_to_json b ++ "}"
  end.

Definition agg_to_json (a : Agg) : string :=
  match a with
  | AggSum e => "{""kind"":""sum"",""expr"":" ++ expr_to_json e ++ "}"
  | AggAvg e => "{""kind"":""avg"",""expr"":" ++ expr_to_json e ++ "}"
  | AggCountStar => "{""kind"":""count_star""}"
  end.

Definition sort_dir_to_json (d : SortDir) : string :=
  match d with SortAsc => """asc""" | SortDesc => """desc""" end.

Definition order_key_to_json (k : OrderKey) : string :=
  "{""expr"":" ++ expr_to_json (ok_expr k)
  ++ ",""dir"":" ++ sort_dir_to_json (ok_dir k) ++ "}".

Definition table_to_json (t : SpecTable) : string :=
  match t with
  | SpecTable_lineitem => """lineitem"""
  | SpecTable_orders => """orders"""
  | SpecTable_unknown n => """unknown:" ++ n ++ """"
  end.

Definition join_to_json (j : SpecJoin) : string :=
  match j with
  | SpecJoin_none => "null"
  | SpecJoin_inner_eq l r lk rk =>
      "{""kind"":""inner_eq"",""left"":" ++ table_to_json l
      ++ ",""right"":" ++ table_to_json r
      ++ ",""left_key"":""" ++ lk ++ """,""right_key"":""" ++ rk ++ """}"
  end.

Definition list_to_json {A} (f : A -> string) (xs : list A) : string :=
  "[" ++ comma_join (List.map f xs) ++ "]".

Definition serialize_params_json (p : Params) : string :=
  "{""cutoff"":" ++ Z_to_string (params_cutoff p)
  ++ ",""base_date"":" ++ Z_to_string q1_base_date ++ "}".

(** Compact single-line JSON object (deterministic field order).
    Template only — no baked cutoff in filters. *)
Definition serialize_logical_spec_json (s : LogicalSpec) : string :=
  "{""version"":""logical_spec_expr_ir_v2"""
  ++ ",""inputs"":" ++ list_to_json table_to_json (ls_inputs s)
  ++ ",""join"":" ++ join_to_json (ls_join s)
  ++ ",""filters"":" ++ list_to_json pred_to_json (ls_filters s)
  ++ ",""group_keys"":" ++ list_to_json expr_to_json (ls_group_keys s)
  ++ ",""aggs"":" ++ list_to_json agg_to_json (ls_aggs s)
  ++ ",""order_keys"":" ++ list_to_json order_key_to_json (ls_order_keys s)
  ++ ",""well_scaled"":"
  ++ (if logical_spec_well_scaled s then "true" else "false")
  ++ "}".

Definition serialize_LogicalSpec_Q1_json : string :=
  serialize_logical_spec_json LogicalSpec_Q1.

(** Full dump body: parameterized template + catalog Params instance (δ=90). *)
Definition serialize_LogicalSpec_Q1_export_json (catalog_delta : Z) : string :=
  let c := cutoff_Q1 catalog_delta in
  "{""version"":""logical_spec_expr_ir_v2"""
  ++ ",""template"":" ++ serialize_LogicalSpec_Q1_json
  ++ ",""catalog_params"":" ++ serialize_params_json (params_of_cutoff c)
  ++ ",""catalog_delta"":" ++ Z_to_string catalog_delta
  ++ "}".
