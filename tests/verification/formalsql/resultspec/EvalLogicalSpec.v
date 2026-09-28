(* EvalLogicalSpec — LEGACY FLAT expression-IR interpreter (migration scaffolding).
   Canonical generalized IR is FormalSQLLogical (RelNode tree) under
   formalsql/logical/. Q1 tree encoding: q1/Q1LogicalSpecTree.v.
   RelNode Q1Adapt: q1/Q1Adapt.v (not part of generic Eval).

   Rocq public Eval target is RelNode Eval_Spec ∘ Q1Adapt (Q1NormalizeSound).
   Flat↔RelNode observational equality is Qed in q1/Q1RelNodeBridge.v.
   This file remains for migration proofs / Dafny flat Exec scaffolding only.
   Do not add new Q1-name semantic fast paths here. *)

From Stdlib Require Import List ZArith String Bool Lia.
Import ListNotations.
Open Scope Z_scope.

From FormalSQLResultSpec Require Import LogicalSpec.
From FormalSQLResultSpec Require Import QueryResultSpec.
From FormalSQLResultSpec Require Import EvalSpec.
From JetStreamFS Require Import Q1AuxSpec.

(** * Column / expression evaluation *)

Definition column_value (c : ColumnId) (r : Q1AuxSpec.Row) : Z :=
  match c with
  | Col_shipdate => Q1AuxSpec.shipdate r
  | Col_qty => Q1AuxSpec.qty r
  | Col_price => Q1AuxSpec.price r
  | Col_disc => Q1AuxSpec.disc r
  | Col_tax => Q1AuxSpec.tax r
  | Col_rf => Q1AuxSpec.rf r
  | Col_ls => Q1AuxSpec.ls r
  end.

Fixpoint eval_expr (e : Expr) (p : Params) (r : Q1AuxSpec.Row) : option Z :=
  match e with
  | EColumn c => Some (column_value c r)
  | EConst z _ => Some z
  | EParam id _ => Some (params_lookup p id)
  | EAdd a b =>
      match eval_expr a p r, eval_expr b p r with
      | Some va, Some vb => Some (va + vb)
      | _, _ => None
      end
  | ESub a b =>
      match eval_expr a p r, eval_expr b p r with
      | Some va, Some vb => Some (va - vb)
      | _, _ => None
      end
  | EMul a b =>
      match eval_expr a p r, eval_expr b p r with
      | Some va, Some vb => Some (va * vb)
      | _, _ => None
      end
  end.

Definition eval_pred (pr : Pred) (p : Params) (r : Q1AuxSpec.Row) : option bool :=
  match pr with
  | PLe a b =>
      match eval_expr a p r, eval_expr b p r with
      | Some va, Some vb => Some (Z.leb va vb)
      | _, _ => None
      end
  end.

Fixpoint eval_preds (ps : list Pred) (p : Params) (r : Q1AuxSpec.Row) : option bool :=
  match ps with
  | [] => Some true
  | pr :: rest =>
      match eval_pred pr p r, eval_preds rest p r with
      | Some b1, Some b2 => Some (andb b1 b2)
      | _, _ => None
      end
  end.

Fixpoint filter_rows (ps : list Pred) (p : Params) (db : list Q1AuxSpec.Row)
  : option (list Q1AuxSpec.Row) :=
  match db with
  | [] => Some []
  | r :: rest =>
      match eval_preds ps p r, filter_rows ps p rest with
      | Some true, Some rs => Some (r :: rs)
      | Some false, Some rs => Some rs
      | _, _ => None
      end
  end.

Fixpoint eval_exprs (es : list Expr) (p : Params) (r : Q1AuxSpec.Row)
  : option (list Z) :=
  match es with
  | [] => Some []
  | e :: rest =>
      match eval_expr e p r, eval_exprs rest p r with
      | Some v, Some vs => Some (v :: vs)
      | _, _ => None
      end
  end.

Fixpoint list_Z_eqb (a b : list Z) : bool :=
  match a, b with
  | [], [] => true
  | x :: xs, y :: ys => andb (Z.eqb x y) (list_Z_eqb xs ys)
  | _, _ => false
  end.

(** Aggregate accumulator: per-agg (sum, count). *)
Definition AggAcc : Type := list (Z * Z).

Definition empty_acc (n : nat) : AggAcc := List.repeat (0, 0) n.

Definition acc_add_one (acc : AggAcc) (vals : list Z) : option AggAcc :=
  let fix go (a : AggAcc) (vs : list Z) : option AggAcc :=
    match a, vs with
    | [], [] => Some []
    | (s, c) :: a', v :: vs' =>
        match go a' vs' with
        | Some rest => Some ((s + v, c + 1) :: rest)
        | None => None
        end
    | _, _ => None
    end in
  go acc vals.

Definition agg_contrib (a : Agg) (p : Params) (r : Q1AuxSpec.Row) : option Z :=
  match a with
  | AggSum e | AggAvg e => eval_expr e p r
  | AggCountStar => Some 1
  end.

Fixpoint aggs_contrib (aggs : list Agg) (p : Params) (r : Q1AuxSpec.Row)
  : option (list Z) :=
  match aggs with
  | [] => Some []
  | a :: rest =>
      match agg_contrib a p r, aggs_contrib rest p r with
      | Some v, Some vs => Some (v :: vs)
      | _, _ => None
      end
  end.

Definition acc_add_row (aggs : list Agg) (p : Params) (r : Q1AuxSpec.Row)
    (acc : AggAcc) : option AggAcc :=
  match aggs_contrib aggs p r with
  | Some vals => acc_add_one acc vals
  | None => None
  end.

(** Group store: semantic tuple keys (list Z), not Pack. *)
Definition GroupStore : Type := list (list Z * AggAcc).

Fixpoint store_get (st : GroupStore) (k : list Z) : option AggAcc :=
  match st with
  | [] => None
  | (k', a) :: rest =>
      if list_Z_eqb k k' then Some a else store_get rest k
  end.

Fixpoint store_put (st : GroupStore) (k : list Z) (a : AggAcc) : GroupStore :=
  match st with
  | [] => [(k, a)]
  | (k', a') :: rest =>
      if list_Z_eqb k k' then (k, a) :: rest
      else (k', a') :: store_put rest k a
  end.

Definition store_add_row (aggs : list Agg) (gkeys : list Expr) (p : Params)
    (r : Q1AuxSpec.Row) (st : GroupStore) : option GroupStore :=
  match eval_exprs gkeys p r with
  | None => None
  | Some k =>
      let acc0 :=
        match store_get st k with
        | Some a => a
        | None => empty_acc (List.length aggs)
        end in
      match acc_add_row aggs p r acc0 with
      | Some acc1 => Some (store_put st k acc1)
      | None => None
      end
  end.

Fixpoint fold_groups (aggs : list Agg) (gkeys : list Expr) (p : Params)
    (rs : list Q1AuxSpec.Row) (st : GroupStore) : option GroupStore :=
  match rs with
  | [] => Some st
  | r :: rest =>
      match store_add_row aggs gkeys p r st with
      | Some st' => fold_groups aggs gkeys p rest st'
      | None => None
      end
  end.

(** * Generic semantic result *)

Record SpecResultRow : Type := mkSpecResultRow {
  sr_key : list Z;
  sr_acc : AggAcc
}.

Definition store_to_rows (st : GroupStore) : list SpecResultRow :=
  map (fun '(k, a) => mkSpecResultRow k a) st.

(** Resolve an order-key expr against a result row via group-key position. *)
Fixpoint lookup_key_expr (e : Expr) (gkeys : list Expr) (kvals : list Z)
  : option Z :=
  match gkeys, kvals with
  | ge :: gs, v :: vs =>
      if expr_eqb e ge then Some v else lookup_key_expr e gs vs
  | _, _ => None
  end.

Definition eval_order_expr (e : Expr) (gkeys : list Expr) (row : SpecResultRow)
  : option Z :=
  lookup_key_expr e gkeys (sr_key row).

(** Lexicographic ≤ using [ls_order_keys] (Asc/Desc). *)
Fixpoint result_row_le (oks : list OrderKey) (gkeys : list Expr)
    (a b : SpecResultRow) : option bool :=
  match oks with
  | [] => Some true
  | ok :: rest =>
      match eval_order_expr (ok_expr ok) gkeys a,
            eval_order_expr (ok_expr ok) gkeys b with
      | Some va, Some vb =>
          if Z.eqb va vb then result_row_le rest gkeys a b
          else
            match ok_dir ok with
            | SortAsc => Some (Z.leb va vb)
            | SortDesc => Some (Z.leb vb va)
            end
      | _, _ => None
      end
  end.

Fixpoint result_row_insert (oks : list OrderKey) (gkeys : list Expr)
    (x : SpecResultRow) (l : list SpecResultRow)
  : option (list SpecResultRow) :=
  match l with
  | [] => Some [x]
  | y :: ys =>
      match result_row_le oks gkeys x y with
      | Some true => Some (x :: y :: ys)
      | Some false =>
          match result_row_insert oks gkeys x ys with
          | Some ys' => Some (y :: ys')
          | None => None
          end
      | None => None
      end
  end.

Fixpoint result_row_sort (oks : list OrderKey) (gkeys : list Expr)
    (l : list SpecResultRow) : option (list SpecResultRow) :=
  match l with
  | [] => Some []
  | x :: xs =>
      match result_row_sort oks gkeys xs with
      | Some xs' => result_row_insert oks gkeys x xs'
      | None => None
      end
  end.

(** Drop empty groups (count = 0 on every slot, or CountStar slot = 0).
    A group is kept iff some accumulator count is positive. *)
Definition acc_nonempty (acc : AggAcc) : bool :=
  existsb (fun '(_, c) => negb (Z.eqb c 0)) acc.

Definition filter_nonempty (rows : list SpecResultRow) : list SpecResultRow :=
  filter (fun row => acc_nonempty (sr_acc row)) rows.

(** Pure generic IR interpreter: filter → group (tuple keys) → order keys. *)
Definition Eval_Spec (s : LogicalSpec) (p : Params) (db : list Q1AuxSpec.Row)
  : option (list SpecResultRow) :=
  if negb (logical_spec_well_scaled s) then None
  else
    match filter_rows (ls_filters s) p db with
    | None => None
    | Some filtered =>
        match fold_groups (ls_aggs s) (ls_group_keys s) p filtered [] with
        | None => None
        | Some st =>
            result_row_sort (ls_order_keys s) (ls_group_keys s)
              (filter_nonempty (store_to_rows st))
        end
    end.

(** * Q1-only result adapter (LEGACY FLAT — not part of Eval_Spec).
    RelNode adapter: formalsql/q1/Q1Adapt.v *)

Definition Q1AdaptRow (row : SpecResultRow) : option Q1AuxSpec.OutRow :=
  match sr_key row, sr_acc row with
  | [rf; ls],
    [(sum_qty, _);
     (sum_base, _);
     (sum_disc_price, _);
     (sum_charge, _);
     (sum_avg_qty, _);
     (sum_avg_price, _);
     (sum_avg_disc, _);
     (_, c_count)] =>
      if Z.leb c_count 0 then None
      else
        Some (Q1AuxSpec.mkOutRow rf ls
          sum_qty sum_base sum_disc_price sum_charge
          sum_avg_disc
          (Q1AuxSpec.AvgRound sum_avg_qty c_count)
          (Q1AuxSpec.AvgRound sum_avg_price c_count)
          (Q1AuxSpec.AvgRound sum_avg_disc c_count)
          c_count)
  | _, _ => None
  end.

Fixpoint Q1Adapt (rows : list SpecResultRow) : option (list Q1AuxSpec.OutRow) :=
  match rows with
  | [] => Some []
  | r :: rest =>
      match Q1AdaptRow r, Q1Adapt rest with
      | Some o, Some os => Some (o :: os)
      | _, _ => None
      end
  end.

(** Legacy OutRow view for NormalizeSound bridges (adapter ∘ Eval_Spec). *)
Definition Eval_Spec_Q1_rows (s : LogicalSpec) (p : Params) (db : list Q1AuxSpec.Row)
  : option (list Q1AuxSpec.OutRow) :=
  match Eval_Spec s p db with
  | Some rows => Q1Adapt rows
  | None => None
  end.

(** * Filter bridge *)

Lemma filter_rows_Q1 :
  forall c db,
    filter_rows q1_filters (params_of_cutoff c) db =
    Some (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) c) db).
Proof.
  intros c db; induction db as [|r rest IH]; [reflexivity|].
  cbn [filter_rows filter].
  unfold q1_filters, eval_preds, eval_pred, eval_expr in *.
  unfold e_shipdate, e_param_cutoff, column_value, params_lookup, params_of_cutoff in *.
  cbn in *.
  rewrite IH.
  destruct (Z.leb (Q1AuxSpec.shipdate r) c); reflexivity.
Qed.

Lemma aggs_contrib_Q1 :
  forall p r,
    aggs_contrib q1_aggs p r =
    Some [Q1AuxSpec.qty r;
          Q1AuxSpec.price r;
          Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r);
          Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r) * (100 + Q1AuxSpec.tax r);
          Q1AuxSpec.qty r;
          Q1AuxSpec.price r;
          Q1AuxSpec.disc r;
          1].
Proof.
  intros p r.
  unfold q1_aggs, e_qty, e_price, e_disc, e_tax, e_disc_price, e_charge, e_one_scale2.
  simpl. reflexivity.
Qed.

Lemma eval_exprs_Q1_group :
  forall p r,
    eval_exprs q1_ls_group_keys p r =
    Some [Q1AuxSpec.rf r; Q1AuxSpec.ls r].
Proof.
  intros p r. unfold q1_ls_group_keys, e_rf, e_ls. simpl. reflexivity.
Qed.

(** Acc ↔ Stats for one Q1 group. *)
Definition acc_of_stats (s : Q1AuxSpec.Stats) : AggAcc :=
  [(Q1AuxSpec.sum_qty s, Q1AuxSpec.count s);
   (Q1AuxSpec.sum_base_price s, Q1AuxSpec.count s);
   (Q1AuxSpec.sum_disc_price s, Q1AuxSpec.count s);
   (Q1AuxSpec.sum_charge s, Q1AuxSpec.count s);
   (Q1AuxSpec.sum_qty s, Q1AuxSpec.count s);
   (Q1AuxSpec.sum_base_price s, Q1AuxSpec.count s);
   (Q1AuxSpec.sum_discount s, Q1AuxSpec.count s);
   (Q1AuxSpec.count s, Q1AuxSpec.count s)].

Definition stats_of_acc (acc : AggAcc) : option Q1AuxSpec.Stats :=
  match acc with
  | [(sq, cq); (sb, cb); (sdp, cdp); (sc, cc);
     (_, _); (_, _); (sd, cd); (_, cstar)] =>
      if andb (Z.eqb cq cb)
         (andb (Z.eqb cb cdp)
         (andb (Z.eqb cdp cc)
         (andb (Z.eqb cc cd) (Z.eqb cd cstar))))
      then Some (Q1AuxSpec.mkStats cstar sq sb sdp sc sd)
      else None
  | _ => None
  end.

(** SpecApply correspondence — proved in q1/Q1EvalMigration.v (uses Z.eq_sub). *)


(** * Migration: generic Eval_Spec ∘ Q1Adapt = AuxAnswerSpec *)

Lemma Eval_Spec_Q1_unfold :
  forall c db,
    Eval_Spec LogicalSpec_Q1 (params_of_cutoff c) db =
    match filter_rows q1_filters (params_of_cutoff c) db with
    | None => None
    | Some filtered =>
        match fold_groups q1_aggs q1_ls_group_keys (params_of_cutoff c) filtered [] with
        | None => None
        | Some st =>
            result_row_sort q1_ls_order_keys q1_ls_group_keys
              (filter_nonempty (store_to_rows st))
        end
    end.
Proof.
  intros c db.
  unfold Eval_Spec.
  rewrite LogicalSpec_Q1_well_scaled.
  reflexivity.
Qed.

(** IR evaluation path (no Eval_Spec name; used by migration). *)
Definition Q1_IR_eval (c : Z) (db : list Q1AuxSpec.Row)
  : option (list Q1AuxSpec.OutRow) :=
  match filter_rows q1_filters (params_of_cutoff c) db with
  | None => None
  | Some filtered =>
      match fold_groups q1_aggs q1_ls_group_keys (params_of_cutoff c) filtered [] with
      | None => None
      | Some st =>
          match result_row_sort q1_ls_order_keys q1_ls_group_keys
                  (filter_nonempty (store_to_rows st)) with
          | Some rows => Q1Adapt rows
          | None => None
          end
      end
  end.

Lemma Eval_Spec_Q1_eq_IR :
  forall c db,
    Eval_Spec_Q1_rows LogicalSpec_Q1 (params_of_cutoff c) db =
    Q1_IR_eval c db.
Proof.
  intros c db.
  unfold Eval_Spec_Q1_rows.
  rewrite Eval_Spec_Q1_unfold.
  unfold Q1_IR_eval.
  destruct (filter_rows _ _ _) as [filtered|]; [|reflexivity].
  destruct (fold_groups _ _ _ _ _) as [st|]; [|reflexivity].
  destruct (result_row_sort _ _ _); reflexivity.
Qed.

(** Local row contribution / key-bucket merge (mirrors Aux SpecApply path). *)
Definition row_contrib (r : Q1AuxSpec.Row) : Q1AuxSpec.Stats :=
  Q1AuxSpec.SpecApply Q1AuxSpec.EmptyStats r 1.

Fixpoint sum_contrib_key (rs : list Q1AuxSpec.Row) (gkey : Z) : Q1AuxSpec.Stats :=
  match rs with
  | [] => Q1AuxSpec.EmptyStats
  | r :: rest =>
      if Z.eqb (Q1AuxSpec.RowKey r) gkey
      then Q1AuxSpec.SpecMerge (row_contrib r) (sum_contrib_key rest gkey)
      else sum_contrib_key rest gkey
  end.

(** Q1 order keys on a SpecResultRow reduce to (rf,ls) lexicographic ≤. *)
Lemma result_row_le_Q1_keys :
  forall a b rf1 ls1 rf2 ls2,
    sr_key a = [rf1; ls1] ->
    sr_key b = [rf2; ls2] ->
    result_row_le q1_ls_order_keys q1_ls_group_keys a b =
    if Z.eqb rf1 rf2
    then if Z.eqb ls1 ls2 then Some true else Some (Z.leb ls1 ls2)
    else Some (Z.leb rf1 rf2).
Proof.
  intros a b rf1 ls1 rf2 ls2 Ha Hb.
  unfold result_row_le, q1_ls_order_keys, q1_ls_group_keys, e_rf, e_ls.
  unfold eval_order_expr, lookup_key_expr, expr_eqb, column_eqb.
  rewrite Ha, Hb; simpl.
  reflexivity.
Qed.

(** Adapt of a nonempty Q1 acc row. *)
Lemma Q1AdaptRow_of_acc :
  forall rf ls s,
    Q1AuxSpec.count s > 0 ->
    Q1AdaptRow (mkSpecResultRow [rf; ls] (acc_of_stats s)) =
    Some (Q1AuxSpec.mkOutRow rf ls
            (Q1AuxSpec.sum_qty s) (Q1AuxSpec.sum_base_price s)
            (Q1AuxSpec.sum_disc_price s) (Q1AuxSpec.sum_charge s)
            (Q1AuxSpec.sum_discount s)
            (Q1AuxSpec.AvgRound (Q1AuxSpec.sum_qty s) (Q1AuxSpec.count s))
            (Q1AuxSpec.AvgRound (Q1AuxSpec.sum_base_price s) (Q1AuxSpec.count s))
            (Q1AuxSpec.AvgRound (Q1AuxSpec.sum_discount s) (Q1AuxSpec.count s))
            (Q1AuxSpec.count s)).
Proof.
  intros rf ls s Hpos.
  unfold Q1AdaptRow, acc_of_stats.
  simpl.
  destruct (Z.leb (Q1AuxSpec.count s) 0) eqn:Ele.
  - apply Z.leb_le in Ele; lia.
  - reflexivity.
Qed.

(** Witness: Eval_Spec has no Q1-name semantic branch. *)
Lemma Eval_Spec_no_Q1_branch :
  forall s p db,
    Eval_Spec s p db =
    if negb (logical_spec_well_scaled s) then None
    else
      match filter_rows (ls_filters s) p db with
      | None => None
      | Some filtered =>
          match fold_groups (ls_aggs s) (ls_group_keys s) p filtered [] with
          | None => None
          | Some st =>
              result_row_sort (ls_order_keys s) (ls_group_keys s)
                (filter_nonempty (store_to_rows st))
          end
      end.
Proof. intros; reflexivity. Qed.

(** Migration lemmas: see q1/Q1EvalMigration.v. *)

