(* Typing / well-formedness — fail-closed schema inference for RelNode. *)

From Stdlib Require Import List ZArith String Bool Nat.
Import ListNotations.
Open Scope string_scope.
Open Scope list_scope.

From FormalSQLLogical Require Import SqlTypes Schema Expr Agg RelNode.

(** Catalog: table id → schema. Query modules supply this. *)
Definition Catalog : Type := list (TableId * Schema).

Fixpoint catalog_lookup (c : Catalog) (t : TableId) : option Schema :=
  match c with
  | [] => None
  | (k, s) :: rest => if String.eqb k t then Some s else catalog_lookup rest t
  end.

Definition digit_string (d : nat) : string :=
  match d with
  | 0%nat => "0" | 1%nat => "1" | 2%nat => "2" | 3%nat => "3" | 4%nat => "4"
  | 5%nat => "5" | 6%nat => "6" | 7%nat => "7" | 8%nat => "8" | _ => "9"
  end.

Fixpoint nat_string_aux (n fuel : nat) (acc : string) : string :=
  match fuel with
  | 0%nat => acc
  | S fuel' =>
      let d := Nat.modulo n 10 in
      let acc' := digit_string d ++ acc in
      let n' := Nat.div n 10 in
      if Nat.eqb n' 0 then acc' else nat_string_aux n' fuel' acc'
  end.

Definition nat_string (n : nat) : string :=
  if Nat.eqb n 0 then "0" else nat_string_aux n (S n) "".

(** Expression typing under an input schema. GroupRef/AggRef need agg ctx. *)
Inductive TyCtx : Type :=
| TyRow (s : Schema)
| TyAgg (group_n : nat) (agg_n : nat).

Fixpoint type_expr (ctx : TyCtx) (e : Expr) : option SqlType :=
  match e with
  | EInputRef i =>
      match ctx with
      | TyRow s =>
          match nth_error s i with
          | Some a => Some (attr_type a)
          | None => None
          end
      | TyAgg _ _ => None
      end
  | EConst _ ty | EParam _ ty => Some ty
  | EUnary op e' =>
      match type_expr ctx e' with
      | None => None
      | Some ty =>
          match op with
          | UNeg => Some ty
          | UNot | UIsNull | UIsNotNull => Some TBool
          end
      end
  | EBinary op l r =>
      match type_expr ctx l, type_expr ctx r with
      | Some _, Some _ =>
          match op with
          | BAdd | BSub | BMul | BDiv => type_expr ctx l
          | _ => Some TBool
          end
      | _, _ => None
      end
  | ECase brs el =>
      let fix go (bs : list (Expr * Expr)) : bool :=
        match bs with
        | [] => true
        | (c, _) :: rest =>
            match type_expr ctx c with
            | Some TBool => go rest
            | _ => false
            end
        end in
      if go brs then type_expr ctx el else None
  | ECast e' ty =>
      match type_expr ctx e' with Some _ => Some ty | None => None end
  | EGroupRef i =>
      match ctx with
      | TyAgg gn _ => if Nat.ltb i gn then Some TInt else None
      | _ => None
      end
  | EAggRef i =>
      match ctx with
      | TyAgg _ an => if Nat.ltb i an then Some TInt else None
      | _ => None
      end
  | EFunction _ _ => None
  end.

Definition type_pred (ctx : TyCtx) (p : Pred) : bool :=
  match type_expr ctx p with Some TBool => true | _ => false end.

Definition synthetic_attr (prefix : string) (i : nat) : Attr :=
  mkAttr (prefix ++ nat_string i) TInt true.

Fixpoint group_schema (n : nat) (i : nat) : Schema :=
  match n with
  | 0%nat => []
  | S n' => synthetic_attr "g_" i :: group_schema n' (S i)
  end.

Fixpoint agg_schema (n : nat) (i : nat) : Schema :=
  match n with
  | 0%nat => []
  | S n' => synthetic_attr "a_" i :: agg_schema n' (S i)
  end.

Definition aggregate_out_schema (ng na : nat) : Schema :=
  List.app (group_schema ng 0%nat) (agg_schema na 0%nat).

(** Schema inference (fail-closed). *)
Fixpoint infer_schema (cat : Catalog) (n : RelNode) : option Schema :=
  match n with
  | RelScan t => catalog_lookup cat t
  | RelFilter input pred =>
      match infer_schema cat input with
      | Some s => if type_pred (TyRow s) pred then Some s else None
      | None => None
      end
  | RelProject input exprs =>
      match infer_schema cat input with
      | None => None
      | Some s =>
          let fix go (es : list Expr) : option Schema :=
            match es with
            | [] => Some []
            | e :: rest =>
                match type_expr (TyRow s) e, go rest with
                | Some ty, Some rs => Some (mkAttr "" ty true :: rs)
                | _, _ => None
                end
            end in
          (* Allow GroupRef/AggRef when input is Aggregate: detect via TyAgg. *)
          match input with
          | RelAggregate _ g ags =>
              let ctx := TyAgg (List.length g) (List.length ags) in
              let fix goA (es : list Expr) : option Schema :=
                match es with
                | [] => Some []
                | e :: rest =>
                    match type_expr ctx e, goA rest with
                    | Some ty, Some rs => Some (mkAttr "" ty true :: rs)
                    | _, _ => None
                    end
                end in
              goA exprs
          | _ => go exprs
          end
      end
  | RelJoin _ lft rgt cond =>
      match infer_schema cat lft, infer_schema cat rgt with
      | Some sl, Some sr =>
          let s := List.app sl sr in
          if type_pred (TyRow s) cond then Some s else None
      | _, _ => None
      end
  | RelAggregate input g ags =>
      match infer_schema cat input with
      | None => None
      | Some s =>
          let ctx := TyRow s in
          let fix ok_g (es : list Expr) : bool :=
            match es with
            | [] => true
            | e :: rest =>
                match type_expr ctx e with Some _ => ok_g rest | None => false end
            end in
          if ok_g g then
            Some (aggregate_out_schema (List.length g) (List.length ags))
          else None
      end
  | RelSort input oks =>
      match infer_schema cat input with
      | None => None
      | Some s =>
          let ctx :=
            match input with
            | RelAggregate _ g ags => TyAgg (List.length g) (List.length ags)
            | RelProject (RelAggregate _ g ags) _ =>
                TyRow s  (* projected *)
            | _ => TyRow s
            end in
          let fix ok (ks : list OrderKey) : bool :=
            match ks with
            | [] => true
            | k :: rest =>
                match type_expr (TyRow s) (ok_expr k) with
                | Some _ => ok rest
                | None =>
                    match type_expr ctx (ok_expr k) with
                    | Some _ => ok rest
                    | None => false
                    end
                end
            end in
          if ok oks then Some s else None
      end
  | RelDistinct input | RelLimit input _ _ => infer_schema cat input
  end.

Definition logical_spec_wf (cat : Catalog) (s : LogicalSpec) : bool :=
  match infer_schema cat s with Some _ => true | None => false end.
