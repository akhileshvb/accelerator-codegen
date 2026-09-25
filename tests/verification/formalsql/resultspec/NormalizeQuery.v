(* NormalizeQuery — FormalSQL QueryExpr → option QueryResultSpec (Q1 TFGOP only).
   Fail closed on unsupported shapes. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueNumericTypmod
  ValueString ValueInteger ValueTemporal Formula SchemaConstraints SqlOutcome
  SqlQuerySyntax SqlOrder FiniteBag FiniteCollection FiniteSet FTuples
  FTerms ATerms.
From Logos Require Import FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax.
From Stdlib Require Import String List ZArith.
Import ListNotations.
Import Tuple.
Import NullValues.
Open Scope string_scope.
Open Scope Z_scope.

From FormalSQLResultSpec Require Import QueryResultSpec.

(** Extract Z constant from a leaf (unfolds Logos [CstZ]). *)
Definition extract_z_cst_leaf
    (e : @scalar_expr TNull relname ScalarResultValue) : option Z :=
  match e return option Z with
  | SExpr_Leaf _ (@A_Expr _ (@F_Constant _ (Value_Z (Some z)))) => Some z
  | _ => None
  end.

(** Pull interval CstZ from TIMESTAMP+INTERVAL args list. *)
Definition extract_delta_from_add_args
    (add_args : list (@scalar_expr TNull relname ScalarResultValue)) : option Z :=
  match add_args with
  | _ :: leaf :: nil =>
      match extract_z_cst_leaf leaf with
      | Some z => Some (- z)
      | None => None
      end
  | _ => None
  end.

(** Pull TIMESTAMP+INTERVAL call from DATE≤TIMESTAMP predicate args. *)
Definition extract_delta_from_pred_args
    (args : list (@scalar_expr TNull relname ScalarResultValue)) : option Z :=
  match args with
  | _ :: second :: nil =>
      match second return option Z with
      | SExpr_Call _ (ScalarTimestampAdd _) add_args =>
          extract_delta_from_add_args add_args
      | _ => None
      end
  | _ => None
  end.

(** Extract INTERVAL days from Q1-shaped DATE≤TIMESTAMP+INTERVAL filter. *)
Definition extract_delta_from_filter
    (f : @scalar_expr TNull relname ScalarResultBoolean) : option Z :=
  match f return option Z with
  | SExpr_Pred _ args => extract_delta_from_pred_args args
  | _ => None
  end.

(** Peel TFGOP Project(OrderBy(Group(Project(Filter(Table))))) → S_Q1(cutoff). *)
Definition normalize_query (q : QueryExpr) : option QueryResultSpec_Q1 :=
  match q with
  | QExpr_Project _
      (QExpr_OrderBy _
        (QExpr_Group _ _ having
          (QExpr_Project _
            (QExpr_Filter filt
              (QExpr_Table _ table))))) =>
      match table with
      | Rel name =>
          if String.eqb name "lineitem"%string then
            match having return option QueryResultSpec_Q1 with
            | SExpr_True =>
                match extract_delta_from_filter filt with
                | Some delta => Some (S_Q1 (cutoff_Q1 delta))
                | None => None
                end
            | _ => None
            end
          else None
      end
  | _ => None
  end.
