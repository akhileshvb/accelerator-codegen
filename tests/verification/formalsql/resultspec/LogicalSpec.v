(* LogicalSpec — LEGACY FLAT expression-IR record (Q1-shaped filters/groups/aggs/order).
   Canonical LogicalSpec is the RelNode tree in FormalSQLLogical.RelNode
   (formalsql/logical/). Q1 as RelNode data: q1/Q1LogicalSpecTree.LogicalSpec_Q1_rel.

   Rocq public evaluation uses LogicalSpec_Q1_rel. This flat record remains
   migration / normalize-recognition scaffolding (normalize_q1 still emits it)
   and the Dafny flat Exec DATA dual. Contains Q1 column ids for the flat path
   only — do not extend; put new queries on RelNode. *)

From Stdlib Require Import List ZArith String Bool Nat.
Import ListNotations.
Open Scope Z_scope.
Open Scope string_scope.

From FormalSQLResultSpec Require Import QueryResultSpec.

(** * Scales *)

Inductive Scale : Type :=
| Scale_0
| Scale_2
| Scale_4
| Scale_6.

Definition scale_deg (s : Scale) : nat :=
  match s with
  | Scale_0 => 0%nat
  | Scale_2 => 2%nat
  | Scale_4 => 4%nat
  | Scale_6 => 6%nat
  end.

Definition scale_of_deg (n : nat) : option Scale :=
  match n with
  | 0%nat => Some Scale_0
  | 2%nat => Some Scale_2
  | 4%nat => Some Scale_4
  | 6%nat => Some Scale_6
  | _ => None
  end.

Definition scale_eqb (a b : Scale) : bool :=
  Nat.eqb (scale_deg a) (scale_deg b).

Definition scale_add (a b : Scale) : option Scale :=
  scale_of_deg (scale_deg a + scale_deg b)%nat.

(** * Tables / columns / params *)

Inductive SpecTable : Type :=
| SpecTable_lineitem
| SpecTable_orders
| SpecTable_unknown (name : string).

Inductive ColumnId : Type :=
| Col_shipdate
| Col_qty
| Col_price
| Col_disc
| Col_tax
| Col_rf
| Col_ls.

Inductive ParamId : Type :=
| ParamCutoff.

(** Declared column scales (lineitem fragment). *)
Definition column_scale (c : ColumnId) : Scale :=
  match c with
  | Col_shipdate | Col_rf | Col_ls => Scale_0
  | Col_qty | Col_price | Col_disc | Col_tax => Scale_2
  end.

Definition column_name (c : ColumnId) : string :=
  match c with
  | Col_shipdate => "l_shipdate"
  | Col_qty => "l_quantity"
  | Col_price => "l_extendedprice"
  | Col_disc => "l_discount"
  | Col_tax => "l_tax"
  | Col_rf => "l_returnflag"
  | Col_ls => "l_linestatus"
  end.

Definition param_name (p : ParamId) : string :=
  match p with
  | ParamCutoff => "cutoff"
  end.

Definition param_scale (p : ParamId) : Scale :=
  match p with
  | ParamCutoff => Scale_0
  end.

(** Evaluation environment (not part of LogicalSpec DATA). *)
Record Params : Type := mkParams {
  params_cutoff : Z
}.

Definition params_of_cutoff (c : Z) : Params := mkParams c.

Definition params_lookup (p : Params) (id : ParamId) : Z :=
  match id with
  | ParamCutoff => params_cutoff p
  end.

(** * Expression language *)

Inductive Expr : Type :=
| EColumn (c : ColumnId)
| EConst (z : Z) (s : Scale)
| EParam (id : ParamId) (s : Scale)
| EAdd (a b : Expr)
| ESub (a b : Expr)
| EMul (a b : Expr).

(** Scale of an expression (None = ill-scaled / unsupported). *)
Fixpoint expr_scale (e : Expr) : option Scale :=
  match e with
  | EColumn c => Some (column_scale c)
  | EConst _ s | EParam _ s => Some s
  | EAdd a b | ESub a b =>
      match expr_scale a, expr_scale b with
      | Some sa, Some sb => if scale_eqb sa sb then Some sa else None
      | _, _ => None
      end
  | EMul a b =>
      match expr_scale a, expr_scale b with
      | Some sa, Some sb => scale_add sa sb
      | _, _ => None
      end
  end.

Definition expr_well_scaled (e : Expr) : bool :=
  match expr_scale e with Some _ => true | None => false end.

(** * Predicates / aggregates / order *)

Inductive Pred : Type :=
| PLe (a b : Expr).

Definition pred_well_scaled (p : Pred) : bool :=
  match p with
  | PLe a b =>
      match expr_scale a, expr_scale b with
      | Some sa, Some sb => scale_eqb sa sb
      | _, _ => false
      end
  end.

Inductive Agg : Type :=
| AggSum (e : Expr)
| AggAvg (e : Expr)
| AggCountStar.

Definition agg_well_scaled (a : Agg) : bool :=
  match a with
  | AggSum e | AggAvg e => expr_well_scaled e
  | AggCountStar => true
  end.

Inductive SortDir : Type :=
| SortAsc
| SortDesc.

Record OrderKey : Type := mkOrderKey {
  ok_expr : Expr;
  ok_dir : SortDir
}.

Definition order_key_well_scaled (k : OrderKey) : bool :=
  expr_well_scaled (ok_expr k).

(** * Join (fragment stub; Q1 uses none) *)

Inductive SpecJoin : Type :=
| SpecJoin_none
| SpecJoin_inner_eq (left right : SpecTable) (left_key right_key : string).

(** * LogicalSpec record — query template (no baked parameter values) *)

Record LogicalSpec : Type := mkLogicalSpec {
  ls_inputs : list SpecTable;
  ls_join : SpecJoin;
  ls_filters : list Pred;
  (** Semantic group key: list of Expr values (tuples). Not packed ints. *)
  ls_group_keys : list Expr;
  ls_aggs : list Agg;
  ls_order_keys : list OrderKey
}.

Definition empty_logical_spec : LogicalSpec :=
  mkLogicalSpec [] SpecJoin_none [] [] [] [].

Definition forallb_local {A} (f : A -> bool) (xs : list A) : bool :=
  fold_right (fun x acc => andb (f x) acc) true xs.

Definition logical_spec_well_scaled (s : LogicalSpec) : bool :=
  andb (forallb_local pred_well_scaled (ls_filters s))
  (andb (forallb_local expr_well_scaled (ls_group_keys s))
  (andb (forallb_local agg_well_scaled (ls_aggs s))
        (forallb_local order_key_well_scaled (ls_order_keys s)))).

(** * Decidable equality (exact structural recognition) *)

Definition column_eqb (a b : ColumnId) : bool :=
  match a, b with
  | Col_shipdate, Col_shipdate
  | Col_qty, Col_qty
  | Col_price, Col_price
  | Col_disc, Col_disc
  | Col_tax, Col_tax
  | Col_rf, Col_rf
  | Col_ls, Col_ls => true
  | _, _ => false
  end.

Definition param_eqb (a b : ParamId) : bool :=
  match a, b with
  | ParamCutoff, ParamCutoff => true
  end.

Definition table_eqb (a b : SpecTable) : bool :=
  match a, b with
  | SpecTable_lineitem, SpecTable_lineitem
  | SpecTable_orders, SpecTable_orders => true
  | SpecTable_unknown n1, SpecTable_unknown n2 => String.eqb n1 n2
  | _, _ => false
  end.

Definition join_eqb (a b : SpecJoin) : bool :=
  match a, b with
  | SpecJoin_none, SpecJoin_none => true
  | SpecJoin_inner_eq l1 r1 lk1 rk1, SpecJoin_inner_eq l2 r2 lk2 rk2 =>
      andb (table_eqb l1 l2)
      (andb (table_eqb r1 r2)
      (andb (String.eqb lk1 lk2) (String.eqb rk1 rk2)))
  | _, _ => false
  end.

Definition sort_dir_eqb (a b : SortDir) : bool :=
  match a, b with
  | SortAsc, SortAsc | SortDesc, SortDesc => true
  | _, _ => false
  end.

Fixpoint expr_eqb (a b : Expr) : bool :=
  match a, b with
  | EColumn c1, EColumn c2 => column_eqb c1 c2
  | EConst z1 s1, EConst z2 s2 => andb (Z.eqb z1 z2) (scale_eqb s1 s2)
  | EParam i1 s1, EParam i2 s2 => andb (param_eqb i1 i2) (scale_eqb s1 s2)
  | EAdd x1 y1, EAdd x2 y2
  | ESub x1 y1, ESub x2 y2
  | EMul x1 y1, EMul x2 y2 => andb (expr_eqb x1 x2) (expr_eqb y1 y2)
  | _, _ => false
  end.

Definition pred_eqb (a b : Pred) : bool :=
  match a, b with
  | PLe x1 y1, PLe x2 y2 => andb (expr_eqb x1 x2) (expr_eqb y1 y2)
  end.

Definition agg_eqb (a b : Agg) : bool :=
  match a, b with
  | AggSum e1, AggSum e2 | AggAvg e1, AggAvg e2 => expr_eqb e1 e2
  | AggCountStar, AggCountStar => true
  | _, _ => false
  end.

Definition order_key_eqb (a b : OrderKey) : bool :=
  andb (expr_eqb (ok_expr a) (ok_expr b)) (sort_dir_eqb (ok_dir a) (ok_dir b)).

Fixpoint list_eqb {A} (eqb : A -> A -> bool) (xs ys : list A) : bool :=
  match xs, ys with
  | [], [] => true
  | x :: xs', y :: ys' => andb (eqb x y) (list_eqb eqb xs' ys')
  | _, _ => false
  end.

Definition logical_spec_eqb (a b : LogicalSpec) : bool :=
  andb (list_eqb table_eqb (ls_inputs a) (ls_inputs b))
  (andb (join_eqb (ls_join a) (ls_join b))
  (andb (list_eqb pred_eqb (ls_filters a) (ls_filters b))
  (andb (list_eqb expr_eqb (ls_group_keys a) (ls_group_keys b))
  (andb (list_eqb agg_eqb (ls_aggs a) (ls_aggs b))
        (list_eqb order_key_eqb (ls_order_keys a) (ls_order_keys b)))))).

(** * Q1 helpers — parameterized expression-IR template *)

(** 100 at Scale_2 = "1.00" against discount/tax fields at Scale_2. *)
Definition e_one_scale2 : Expr := EConst 100 Scale_2.

Definition e_qty : Expr := EColumn Col_qty.
Definition e_price : Expr := EColumn Col_price.
Definition e_disc : Expr := EColumn Col_disc.
Definition e_tax : Expr := EColumn Col_tax.
Definition e_rf : Expr := EColumn Col_rf.
Definition e_ls : Expr := EColumn Col_ls.
Definition e_shipdate : Expr := EColumn Col_shipdate.
Definition e_param_cutoff : Expr := EParam ParamCutoff Scale_0.

(** extendedprice * (1 - discount)  with scales 2 * 2 = 4 *)
Definition e_disc_price : Expr :=
  EMul e_price (ESub e_one_scale2 e_disc).

(** extendedprice * (1 - discount) * (1 + tax)  scales 4 * 2 = 6 *)
Definition e_charge : Expr :=
  EMul e_disc_price (EAdd e_one_scale2 e_tax).

Definition q1_filters : list Pred :=
  [PLe e_shipdate e_param_cutoff].

Definition q1_ls_group_keys : list Expr := [e_rf; e_ls].

Definition q1_aggs : list Agg :=
  [AggSum e_qty;
   AggSum e_price;
   AggSum e_disc_price;
   AggSum e_charge;
   AggAvg e_qty;
   AggAvg e_price;
   AggAvg e_disc;
   AggCountStar].

Definition q1_ls_order_keys : list OrderKey :=
  [mkOrderKey e_rf SortAsc; mkOrderKey e_ls SortAsc].

(** Canonical Q1 LogicalSpec template (parameterized; no baked cutoff). *)
Definition LogicalSpec_Q1 : LogicalSpec :=
  mkLogicalSpec
    [SpecTable_lineitem]
    SpecJoin_none
    q1_filters
    q1_ls_group_keys
    q1_aggs
    q1_ls_order_keys.

Lemma LogicalSpec_Q1_well_scaled :
  logical_spec_well_scaled LogicalSpec_Q1 = true.
Proof. reflexivity. Qed.

(** Exact recognizer: [s] equals the canonical Q1 template. *)
Definition is_LogicalSpec_Q1 (s : LogicalSpec) : bool :=
  logical_spec_eqb s LogicalSpec_Q1.

(** Bridge LogicalSpec ↔ legacy QueryResultSpec (cutoff from Params). *)
Definition LogicalSpec_to_Q1 (s : LogicalSpec) (p : Params) : option QueryResultSpec_Q1 :=
  if is_LogicalSpec_Q1 s then Some (S_Q1 (params_cutoff p)) else None.

Lemma LogicalSpec_Q1_to_S_Q1 :
  forall c, LogicalSpec_to_Q1 LogicalSpec_Q1 (params_of_cutoff c) = Some (S_Q1 c).
Proof. intros; reflexivity. Qed.

Lemma is_LogicalSpec_Q1_canonical :
  is_LogicalSpec_Q1 LogicalSpec_Q1 = true.
Proof. reflexivity. Qed.
