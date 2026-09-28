(* Eval — generic RelNode interpreter. No query-specific branches.
   Operates only on LogicalSpec / Row / Schema / Params / DB. *)

From Stdlib Require Import List ZArith String Bool Nat Lia Ascii.
Import ListNotations.
Open Scope Z_scope.
Open Scope string_scope.
Open Scope list_scope.

From FormalSQLLogical Require Import SqlTypes Schema Expr Agg RelNode Typing.

(** * Expression evaluation *)

Inductive EvalCtx : Type :=
| EvRow (r : Row)
| EvAgg (keys : list SqlValue) (vals : list SqlValue).

Definition ctx_lookup_input (ctx : EvalCtx) (i : nat) : option SqlValue :=
  match ctx with
  | EvRow r => row_nth r i
  | EvAgg keys vals => row_nth (List.app keys vals) i
  end.

Definition as_int (v : SqlValue) : option Z :=
  match v with VInt z => Some z | _ => None end.

Definition arith (op : BinaryOp) (x y : Z) : option Z :=
  match op with
  | BAdd => Some (x + y)
  | BSub => Some (x - y)
  | BMul => Some (x * y)
  | BDiv => if Z.eqb y 0 then None else Some (Z.div x y)
  | _ => None
  end.

Definition cmp_truth (op : BinaryOp) (a b : SqlValue) : Truth3 :=
  match op with
  | BEq => cmp_int_eq a b
  | BNe => truth3_not (cmp_int_eq a b)
  | BLe => cmp_int_leb a b
  | BLt =>
      truth3_and (cmp_int_leb a b) (truth3_not (cmp_int_eq a b))
  | BGe => cmp_int_leb b a
  | BGt =>
      truth3_and (cmp_int_leb b a) (truth3_not (cmp_int_eq a b))
  | _ => TUnknown
  end.

(** Prefix / LIKE: text only; NULL → Unknown. Simplified: PrefixMatch = prefix. *)
Definition text_prefix (hay pref : string) : bool :=
  let fix go (h p : string) : bool :=
    match p with
    | EmptyString => true
    | String pc ps =>
        match h with
        | EmptyString => false
        | String hc hs => if Ascii.eqb hc pc then go hs ps else false
        end
    end in go hay pref.

Fixpoint eval_expr (e : Expr) (p : Params) (ctx : EvalCtx) : option SqlValue :=
  match e with
  | EInputRef i => ctx_lookup_input ctx i
  | EConst v _ => Some v
  | EParam id _ => params_lookup p id
  | EUnary op e' =>
      match eval_expr e' p ctx with
      | None => None
      | Some v =>
          match op with
          | UNeg => match as_int v with Some z => Some (VInt (-z)) | None => None end
          | UNot =>
              match v with
              | VBool b => Some (VBool (negb b))
              | VNull => Some VNull
              | _ => None
              end
          | UIsNull => Some (VBool (is_null v))
          | UIsNotNull => Some (VBool (negb (is_null v)))
          end
      end
  | EBinary op l r =>
      match eval_expr l p ctx, eval_expr r p ctx with
      | Some va, Some vb =>
          match op with
          | BAdd | BSub | BMul | BDiv =>
              match va, vb with
              | VNull, _ | _, VNull => Some VNull
              | VInt x, VInt y =>
                  match arith op x y with Some z => Some (VInt z) | None => None end
              | _, _ => None
              end
          | BAnd =>
              match va, vb with
              | VBool a, VBool b => Some (VBool (andb a b))
              | VNull, _ | _, VNull => Some VNull
              | _, _ => None
              end
          | BOr =>
              match va, vb with
              | VBool a, VBool b => Some (VBool (orb a b))
              | VNull, _ | _, VNull => Some VNull
              | _, _ => None
              end
          | BEq | BNe | BLt | BLe | BGt | BGe =>
              match cmp_truth op va vb with
              | TTrue => Some (VBool true)
              | TFalse => Some (VBool false)
              | TUnknown => Some VNull
              end
          | BPrefixMatch | BLike =>
              match va, vb with
              | VNull, _ | _, VNull => Some VNull
              | VText h, VText pref => Some (VBool (text_prefix h pref))
              | _, _ => None
              end
          end
      | _, _ => None
      end
  | ECase brs el =>
      let fix go (bs : list (Expr * Expr)) : option SqlValue :=
        match bs with
        | [] => eval_expr el p ctx
        | (c, t) :: rest =>
            match eval_expr c p ctx with
            | Some (VBool true) => eval_expr t p ctx
            | Some (VBool false) | Some VNull => go rest
            | _ => None
            end
        end in go brs
  | ECast e' _ => eval_expr e' p ctx
  | EGroupRef i =>
      match ctx with
      | EvAgg keys _ => row_nth keys i
      | EvRow r => row_nth r i
      end
  | EAggRef i =>
      match ctx with
      | EvAgg _ vals => row_nth vals i
      | EvRow r => row_nth r i
      end
  | EFunction _ _ => None
  end.

(** Predicate as SQL WHERE: keep row iff truth is True (not Unknown). *)
Definition eval_pred (pr : Pred) (p : Params) (ctx : EvalCtx) : option Truth3 :=
  match eval_expr pr p ctx with
  | Some (VBool true) => Some TTrue
  | Some (VBool false) => Some TFalse
  | Some VNull => Some TUnknown
  | Some _ => None
  | None => None
  end.

Definition eval_pred_keep (pr : Pred) (p : Params) (r : Row) : option bool :=
  match eval_pred pr p (EvRow r) with
  | Some t => Some (truth3_is_true t)
  | None => None
  end.

(** * Filter / project *)

Fixpoint filter_rows (pr : Pred) (p : Params) (db : list Row)
  : option (list Row) :=
  match db with
  | [] => Some []
  | r :: rest =>
      match eval_pred_keep pr p r, filter_rows pr p rest with
      | Some true, Some rs => Some (r :: rs)
      | Some false, Some rs => Some rs
      | _, _ => None
      end
  end.

Fixpoint eval_exprs (es : list Expr) (p : Params) (ctx : EvalCtx)
  : option (list SqlValue) :=
  match es with
  | [] => Some []
  | e :: rest =>
      match eval_expr e p ctx, eval_exprs rest p ctx with
      | Some v, Some vs => Some (v :: vs)
      | _, _ => None
      end
  end.

Fixpoint project_rows (es : list Expr) (p : Params) (db : list Row)
  : option (list Row) :=
  match db with
  | [] => Some []
  | r :: rest =>
      match eval_exprs es p (EvRow r), project_rows es p rest with
      | Some vs, Some rs => Some (vs :: rs)
      | _, _ => None
      end
  end.

(** Project over aggregate bags (GroupRef/AggRef). *)
Fixpoint project_agg_rows (es : list Expr) (p : Params)
    (bags : list (list SqlValue * list SqlValue))
  : option (list Row) :=
  match bags with
  | [] => Some []
  | (keys, vals) :: rest =>
      match eval_exprs es p (EvAgg keys vals), project_agg_rows es p rest with
      | Some vs, Some rs => Some (vs :: rs)
      | _, _ => None
      end
  end.

(** * Join (inner only fully; others fail-closed for now if used) *)

Fixpoint join_inner (cond : Pred) (p : Params)
    (left : list Row) (right : list Row) : option (list Row) :=
  match left with
  | [] => Some []
  | lrow :: ls =>
      let fix against (rs : list Row) : option (list Row) :=
        match rs with
        | [] => Some []
        | rrow :: rs' =>
            let combined := List.app lrow rrow in
            match eval_pred_keep cond p combined, against rs' with
            | Some true, Some rest => Some (combined :: rest)
            | Some false, Some rest => Some rest
            | _, _ => None
            end
        end in
      match against right, join_inner cond p ls right with
      | Some here, Some later => Some (List.app here later)
      | _, _ => None
      end
  end.

(** * Aggregation *)

Definition acc_add_slot (kind : AggKind) (slot : AccSlot) (v : option SqlValue)
  : option AccSlot :=
  match kind with
  | AggCountStar =>
      Some (mkAccSlot (acc_sum slot + 1) (acc_count slot + 1) true)
  | AggCount =>
      match v with
      | Some VNull => Some slot
      | Some _ => Some (mkAccSlot (acc_sum slot + 1) (acc_count slot + 1) true)
      | None => None
      end
  | AggSum | AggAvg =>
      match v with
      | Some VNull => Some slot
      | Some (VInt z) =>
          Some (mkAccSlot (acc_sum slot + z) (acc_count slot + 1) true)
      | _ => None
      end
  | AggMin =>
      match v with
      | Some VNull => Some slot
      | Some (VInt z) =>
          if negb (acc_init slot) then Some (mkAccSlot z 1 true)
          else if Z.ltb z (acc_sum slot) then Some (mkAccSlot z (acc_count slot + 1) true)
          else Some (mkAccSlot (acc_sum slot) (acc_count slot + 1) true)
      | _ => None
      end
  | AggMax =>
      match v with
      | Some VNull => Some slot
      | Some (VInt z) =>
          if negb (acc_init slot) then Some (mkAccSlot z 1 true)
          else if Z.ltb (acc_sum slot) z then Some (mkAccSlot z (acc_count slot + 1) true)
          else Some (mkAccSlot (acc_sum slot) (acc_count slot + 1) true)
      | _ => None
      end
  end.

Fixpoint aggs_contrib (aggs : list AggSpec) (p : Params) (r : Row)
  : option (list (option SqlValue)) :=
  match aggs with
  | [] => Some []
  | a :: rest =>
      let one :=
        match agg_kind a, agg_expr a with
        | AggCountStar, _ => Some (Some (VInt 1))
        | _, Some e =>
            match eval_expr e p (EvRow r) with
            | Some v => Some (Some v)
            | None => None
            end
        | _, None => None
        end in
      match one, aggs_contrib rest p r with
      | Some v, Some vs => Some (v :: vs)
      | _, _ => None
      end
  end.

Fixpoint acc_add_one (aggs : list AggSpec) (acc : AggAcc)
    (vals : list (option SqlValue)) : option AggAcc :=
  match aggs, acc, vals with
  | [], [], [] => Some []
  | a :: ags, sl :: accs, v :: vs =>
      match acc_add_slot (agg_kind a) sl v, acc_add_one ags accs vs with
      | Some sl', Some rest => Some (sl' :: rest)
      | _, _ => None
      end
  | _, _, _ => None
  end.

Definition acc_add_row (aggs : list AggSpec) (p : Params) (r : Row)
    (acc : AggAcc) : option AggAcc :=
  match aggs_contrib aggs p r with
  | Some vals => acc_add_one aggs acc vals
  | None => None
  end.

(** Group store: association list of semantic tuple keys → Acc. *)
Definition GroupStore : Type := list (list SqlValue * AggAcc).

Fixpoint store_find (st : GroupStore) (k : list SqlValue)
  : option AggAcc :=
  match st with
  | [] => None
  | (k', a) :: rest =>
      if list_sql_value_eqb k k' then Some a else store_find rest k
  end.

Fixpoint store_update (st : GroupStore) (k : list SqlValue) (a : AggAcc)
  : GroupStore :=
  match st with
  | [] => [(k, a)]
  | (k', a') :: rest =>
      if list_sql_value_eqb k k' then (k, a) :: rest
      else (k', a') :: store_update rest k a
  end.

Definition store_add_row (aggs : list AggSpec) (gkeys : list Expr)
    (p : Params) (r : Row) (st : GroupStore) : option GroupStore :=
  match eval_exprs gkeys p (EvRow r) with
  | None => None
  | Some keys =>
      let acc0 :=
        match store_find st keys with
        | Some a => a
        | None => empty_acc (List.length aggs)
        end in
      match acc_add_row aggs p r acc0 with
      | Some acc' => Some (store_update st keys acc')
      | None => None
      end
  end.

Fixpoint fold_groups (aggs : list AggSpec) (gkeys : list Expr)
    (p : Params) (rs : list Row) (st : GroupStore) : option GroupStore :=
  match rs with
  | [] => Some st
  | r :: rest =>
      match store_add_row aggs gkeys p r st with
      | Some st' => fold_groups aggs gkeys p rest st'
      | None => None
      end
  end.

Definition finalize_slot (kind : AggKind) (sl : AccSlot) : SqlValue :=
  match kind with
  | AggCountStar | AggCount => VInt (acc_count sl)
  | AggSum => if acc_init sl then VInt (acc_sum sl) else VNull
  | AggAvg =>
      if Z.eqb (acc_count sl) 0 then VNull
      else VAvgExact (acc_sum sl) (acc_count sl)
  | AggMin | AggMax => if acc_init sl then VInt (acc_sum sl) else VNull
  end.

Fixpoint finalize_acc (aggs : list AggSpec) (acc : AggAcc)
  : option (list SqlValue) :=
  match aggs, acc with
  | [], [] => Some []
  | a :: ags, sl :: accs =>
      match finalize_acc ags accs with
      | Some vs => Some (finalize_slot (agg_kind a) sl :: vs)
      | None => None
      end
  | _, _ => None
  end.

(** Nonempty group: CountStar/Count > 0 or any init slot. Global (gkeys=[]) always emits. *)
Definition acc_nonempty (acc : AggAcc) : bool :=
  existsb (fun sl => orb (acc_init sl) (negb (Z.eqb (acc_count sl) 0))) acc.

Fixpoint store_to_agg_bags (aggs : list AggSpec) (st : GroupStore)
  : option (list (list SqlValue * list SqlValue)) :=
  match st with
  | [] => Some []
  | (k, a) :: rest =>
      if negb (acc_nonempty a) then store_to_agg_bags aggs rest
      else
        match finalize_acc aggs a, store_to_agg_bags aggs rest with
        | Some vals, Some rs => Some ((k, vals) :: rs)
        | _, _ => None
        end
  end.

Definition run_aggregate (aggs : list AggSpec) (gkeys : list Expr)
    (p : Params) (rs : list Row)
  : option (list (list SqlValue * list SqlValue)) :=
  match fold_groups aggs gkeys p rs [] with
  | None => None
  | Some st =>
      (* Global aggregation: empty input still yields one row. *)
      if andb (Nat.eqb (List.length gkeys) 0%nat) (Nat.eqb (List.length st) 0%nat)
      then
        match finalize_acc aggs (empty_acc (List.length aggs)) with
        | Some vals => Some [([], vals)]
        | None => None
        end
      else store_to_agg_bags aggs st
  end.

(** Materialize agg bags as flat rows (keys ++ vals) for Sort/Distinct without Project. *)
Definition agg_bags_to_rows
    (bags : list (list SqlValue * list SqlValue)) : list Row :=
  map (fun '(k, v) => List.app k v) bags.

(** * Sort *)

Definition value_le (nulls : NullOrder) (a b : SqlValue) : option bool :=
  match a, b with
  | VNull, VNull => Some true
  | VNull, _ =>
      match nulls with NullsFirst => Some true | NullsLast => Some false end
  | _, VNull =>
      match nulls with NullsFirst => Some false | NullsLast => Some true end
  | VInt x, VInt y => Some (Z.leb x y)
  | _, _ => None
  end.

Definition order_key_le (p : Params) (k : OrderKey) (ra rb : Row)
  : option bool :=
  match eval_expr (ok_expr k) p (EvRow ra),
        eval_expr (ok_expr k) p (EvRow rb) with
  | Some va, Some vb =>
      match value_le (ok_nulls k) va vb, value_le (ok_nulls k) vb va with
      | Some lea, Some leb =>
          match ok_dir k with
          | SortAsc => Some lea
          | SortDesc => Some leb
          end
      | _, _ => None
      end
  | _, _ => None
  end.

Fixpoint order_keys_le (p : Params) (oks : list OrderKey) (ra rb : Row)
  : option bool :=
  match oks with
  | [] => Some true
  | k :: rest =>
      match order_key_le p k ra rb, order_key_le p k rb ra with
      | Some true, Some true => order_keys_le p rest ra rb  (* equal *)
      | Some true, Some false => Some true
      | Some false, Some true => Some false
      | _, _ => None
      end
  end.

Fixpoint row_insert (p : Params) (oks : list OrderKey)
    (x : Row) (xs : list Row) : option (list Row) :=
  match xs with
  | [] => Some [x]
  | y :: ys =>
      match order_keys_le p oks x y with
      | Some true => Some (x :: y :: ys)
      | Some false =>
          match row_insert p oks x ys with
          | Some ys' => Some (y :: ys')
          | None => None
          end
      | None => None
      end
  end.

Fixpoint result_row_sort (p : Params) (oks : list OrderKey)
    (rs : list Row) : option (list Row) :=
  match rs with
  | [] => Some []
  | x :: xs =>
      match result_row_sort p oks xs with
      | Some xs' => row_insert p oks x xs'
      | None => None
      end
  end.

(** * Distinct / Limit *)

Fixpoint row_in (r : Row) (rs : list Row) : bool :=
  match rs with
  | [] => false
  | x :: xs => if list_sql_value_eqb r x then true else row_in r xs
  end.

Fixpoint distinct_rows (rs : list Row) : list Row :=
  match rs with
  | [] => []
  | r :: rest =>
      let rest' := distinct_rows rest in
      if row_in r rest' then rest' else r :: rest'
  end.

Fixpoint skip_n {A} (n : nat) (xs : list A) : list A :=
  match n, xs with
  | 0%nat, _ => xs
  | S n', [] => []
  | S n', _ :: ys => skip_n n' ys
  end.

Fixpoint take_n {A} (n : nat) (xs : list A) : list A :=
  match n, xs with
  | 0%nat, _ => []
  | S n', [] => []
  | S n', x :: ys => x :: take_n n' ys
  end.

Definition limit_rows (offset count : nat) (rs : list Row) : list Row :=
  take_n count (skip_n offset rs).

(** * Eval_Spec — recursive RelNode interpreter *)

(** Intermediate: either plain rows or post-aggregate bags (for GroupRef Project). *)
Inductive RelVal : Type :=
| RVRows (rs : list Row)
| RVAgg (bags : list (list SqlValue * list SqlValue)).

Fixpoint eval_rel (n : RelNode) (p : Params) (db : DB) : option RelVal :=
  match n with
  | RelScan t =>
      match db_lookup db t with
      | Some rs => Some (RVRows rs)
      | None => None
      end
  | RelFilter input pred =>
      match eval_rel input p db with
      | Some (RVRows rs) =>
          match filter_rows pred p rs with
          | Some rs' => Some (RVRows rs')
          | None => None
          end
      | _ => None
      end
  | RelProject input exprs =>
      match eval_rel input p db with
      | Some (RVRows rs) =>
          match project_rows exprs p rs with
          | Some rs' => Some (RVRows rs')
          | None => None
          end
      | Some (RVAgg bags) =>
          match project_agg_rows exprs p bags with
          | Some rs' => Some (RVRows rs')
          | None => None
          end
      | None => None
      end
  | RelJoin kind lft rgt cond =>
      match kind with
      | JoinInner | JoinCross =>
          match eval_rel lft p db, eval_rel rgt p db with
          | Some (RVRows ls), Some (RVRows rs) =>
              let cond' :=
                match kind with
                | JoinCross => EConst (VBool true) TBool
                | _ => cond
                end in
              match join_inner cond' p ls rs with
              | Some out => Some (RVRows out)
              | None => None
              end
          | _, _ => None
          end
      | _ => None  (* outer joins: fail-closed until proved *)
      end
  | RelAggregate input gkeys aggs =>
      match eval_rel input p db with
      | Some (RVRows rs) =>
          match run_aggregate aggs gkeys p rs with
          | Some bags => Some (RVAgg bags)
          | None => None
          end
      | _ => None
      end
  | RelSort input oks =>
      match eval_rel input p db with
      | Some (RVRows rs) =>
          match result_row_sort p oks rs with
          | Some rs' => Some (RVRows rs')
          | None => None
          end
      | Some (RVAgg bags) =>
          let rs := agg_bags_to_rows bags in
          match result_row_sort p oks rs with
          | Some rs' => Some (RVRows rs')
          | None => None
          end
      | None => None
      end
  | RelDistinct input =>
      match eval_rel input p db with
      | Some (RVRows rs) => Some (RVRows (distinct_rows rs))
      | _ => None
      end
  | RelLimit input off cnt =>
      match eval_rel input p db with
      | Some (RVRows rs) => Some (RVRows (limit_rows off cnt rs))
      | _ => None
      end
  end.

(** Public generic evaluator: option (bag of Rows). *)
Definition Eval_Spec (s : LogicalSpec) (p : Params) (db : DB)
  : option (list Row) :=
  match eval_rel s p db with
  | Some (RVRows rs) => Some rs
  | Some (RVAgg bags) => Some (agg_bags_to_rows bags)
  | None => None
  end.
