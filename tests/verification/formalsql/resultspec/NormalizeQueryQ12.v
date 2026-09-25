(* NormalizeQueryQ12 — peel FormalSQL CrossJoin TFGOP → S_Q12_catalog. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore
  SqlQuerySyntax SqlOrder FiniteBag FiniteCollection FiniteSet FTuples.
From Logos Require Import FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax.
From Stdlib Require Import String List ZArith.
Import ListNotations.
Open Scope string_scope.
Open Scope Z_scope.

From FormalSQLResultSpec Require Import QueryResultSpecQ12.

(** Recognize orders ⋈ lineitem CrossJoin under Project wrappers. *)
Definition is_orders_lineitem_join (q : QueryExpr) : bool :=
  match q with
  | QExpr_CrossJoin
      (QExpr_Project _ (QExpr_Table _ (Rel t1)))
      (QExpr_Project _ (QExpr_Table _ (Rel t2))) =>
      (String.eqb t1 "orders"%string && String.eqb t2 "lineitem"%string)%bool
  | _ => false
  end.

(** Peel Project(OrderBy(Group(Project(Filter(Filter(join)))))) → S_Q12_catalog. *)
Definition normalize_query_Q12 (q : QueryExpr) : option QueryResultSpec_Q12 :=
  match q with
  | QExpr_Project _
      (QExpr_OrderBy _
        (QExpr_Group _ _ having
          (QExpr_Project _
            (QExpr_Filter _
              (QExpr_Filter _ join))))) =>
      if is_orders_lineitem_join join then
        match having with
        | SExpr_True => Some S_Q12_catalog
        | _ => @None QueryResultSpec_Q12
        end
      else @None QueryResultSpec_Q12
  | _ => @None QueryResultSpec_Q12
  end.
