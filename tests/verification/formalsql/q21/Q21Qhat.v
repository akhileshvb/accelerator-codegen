(* Q21Qhat — templatized q̂_Q21(nation_name) by nation-constant substitution.
   Catalog instance is definitional via short-circuit on catalog_nation_name. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueString ValueInteger
  Formula SqlQuerySyntax FTuples FTerms ATerms.
From Logos Require Import FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax.
From LogosGenerated Require Import Queries.
From Stdlib Require Import String List.
Import ListNotations.
Import Tuple.
Import NullValues.
Open Scope string_scope.

Require Import Q21Schema.

(** Catalog nation constant as lowered by Logos ([CstString StringBpchar …]). *)
Definition catalog_nation_cst : AggTerm :=
  CstString StringBpchar catalog_nation_name.

Definition nation_cst (nation_name : string) : AggTerm :=
  CstString StringBpchar nation_name.

(** * Substitution on aggterm / funterm *)

Fixpoint subst_nation_funterm (nation_name : string) (f : FunTerm) : FunTerm :=
  match f with
  | F_Constant _ _ => f
  | F_Dot _ _ => f
  | F_Expr _ op args =>
      F_Expr _ op (List.map (subst_nation_funterm nation_name) args)
  end.

Fixpoint subst_nation_aggterm (nation_name : string) (t : AggTerm) : AggTerm :=
  match @aggterm_eq_dec TNull t catalog_nation_cst with
  | left _ => nation_cst nation_name
  | right _ =>
      match t with
      | A_Expr _ f => A_Expr _ (subst_nation_funterm nation_name f)
      | A_agg _ a f => A_agg _ a (subst_nation_funterm nation_name f)
      | A_fun _ op args =>
          A_fun _ op (List.map (subst_nation_aggterm nation_name) args)
      end
  end.

(** * Substitution on scalar_expr / query_expr (mutual) *)

Fixpoint subst_nation_query (nation_name : string) (q : QueryExpr) {struct q}
  : QueryExpr :=
  match q with
  | QExpr_Error outs err => QExpr_Error outs err
  | QExpr_Values outs bag => QExpr_Values outs bag
  | QExpr_Table outs r => QExpr_Table outs r
  | QExpr_Set op l r =>
      QExpr_Set op (subst_nation_query nation_name l)
                   (subst_nation_query nation_name r)
  | QExpr_NaturalJoin l r =>
      QExpr_NaturalJoin (subst_nation_query nation_name l)
                        (subst_nation_query nation_name r)
  | QExpr_CrossJoin l r =>
      QExpr_CrossJoin (subst_nation_query nation_name l)
                      (subst_nation_query nation_name r)
  | QExpr_Join kind pred msel lsel rsel l r =>
      QExpr_Join kind
        (subst_nation_sexpr_bool nation_name pred)
        (List.map (fun '(e, a) => (subst_nation_sexpr_val nation_name e, a)) msel)
        (List.map (fun '(e, a) => (subst_nation_sexpr_val nation_name e, a)) lsel)
        (List.map (fun '(e, a) => (subst_nation_sexpr_val nation_name e, a)) rsel)
        (subst_nation_query nation_name l)
        (subst_nation_query nation_name r)
  | QExpr_Project sel child =>
      QExpr_Project
        (List.map (fun '(e, a) => (subst_nation_sexpr_val nation_name e, a)) sel)
        (subst_nation_query nation_name child)
  | QExpr_RowMap outs f child =>
      QExpr_RowMap outs f (subst_nation_query nation_name child)
  | QExpr_Filter pred child =>
      QExpr_Filter (subst_nation_sexpr_bool nation_name pred)
                   (subst_nation_query nation_name child)
  | QExpr_Group sel gkeys having child =>
      QExpr_Group
        (List.map (fun '(e, a) => (subst_nation_sexpr_val nation_name e, a)) sel)
        (List.map (subst_nation_sexpr_val nation_name) gkeys)
        (subst_nation_sexpr_bool nation_name having)
        (subst_nation_query nation_name child)
  | QExpr_GroupingSets branches child =>
      QExpr_GroupingSets
        (List.map
          (fun '(sel, keys) =>
            (List.map (fun '(e, a) => (subst_nation_sexpr_val nation_name e, a)) sel,
             List.map (subst_nation_sexpr_val nation_name) keys))
          branches)
        (subst_nation_query nation_name child)
  | QExpr_Rank p o a f child =>
      QExpr_Rank p o a f (subst_nation_query nation_name child)
  | QExpr_Window p o items child =>
      QExpr_Window p o items (subst_nation_query nation_name child)
  | QExpr_Distinct child =>
      QExpr_Distinct (subst_nation_query nation_name child)
  | QExpr_OrderBy keys child =>
      QExpr_OrderBy keys (subst_nation_query nation_name child)
  | QExpr_Offset n child =>
      QExpr_Offset n (subst_nation_query nation_name child)
  | QExpr_Fetch n child =>
      QExpr_Fetch n (subst_nation_query nation_name child)
  end

with subst_nation_sexpr_val (nation_name : string)
    (e : @scalar_expr TNull relname ScalarResultValue) {struct e}
  : @scalar_expr TNull relname ScalarResultValue :=
  match e with
  | SExpr_Leaf ty t => SExpr_Leaf ty (subst_nation_aggterm nation_name t)
  | SExpr_Call ty op args =>
      SExpr_Call ty op (List.map (subst_nation_sexpr_val nation_name) args)
  | SExpr_Case ty c th el =>
      SExpr_Case ty
        (subst_nation_sexpr_bool nation_name c)
        (subst_nation_sexpr_val nation_name th)
        (subst_nation_sexpr_val nation_name el)
  | SExpr_BoolValue ty f b =>
      SExpr_BoolValue ty f (subst_nation_sexpr_bool nation_name b)
  | SExpr_Subquery ty def q =>
      SExpr_Subquery ty def (subst_nation_query nation_name q)
  end

with subst_nation_sexpr_bool (nation_name : string)
    (e : @scalar_expr TNull relname ScalarResultBoolean) {struct e}
  : @scalar_expr TNull relname ScalarResultBoolean :=
  match e with
  | SExpr_ValueBool f v =>
      SExpr_ValueBool f (subst_nation_sexpr_val nation_name v)
  | SExpr_Pred p args =>
      SExpr_Pred p (List.map (subst_nation_sexpr_val nation_name) args)
  | SExpr_ConjList sites op args =>
      SExpr_ConjList sites op
        (List.map (subst_nation_sexpr_bool nation_name) args)
  | SExpr_Not e' => SExpr_Not (subst_nation_sexpr_bool nation_name e')
  | SExpr_True => SExpr_True
  | SExpr_Quant q p args child =>
      SExpr_Quant q p
        (List.map (subst_nation_sexpr_val nation_name) args)
        (subst_nation_query nation_name child)
  | SExpr_In args child =>
      SExpr_In (List.map (subst_nation_sexpr_val nation_name) args)
               (subst_nation_query nation_name child)
  | SExpr_Exists child =>
      SExpr_Exists (subst_nation_query nation_name child)
  end.

(** * Public templatized q̂

    Catalog nation short-circuits to the Logos AST (definitional).
    Other nations rewrite the single [CstString StringBpchar "SAUDI ARABIA"]
    leaf via [subst_nation_query]. *)

Definition qhat_Q21 (nation_name : string) : QueryExpr :=
  if String.eqb nation_name catalog_nation_name
  then Queries.source_query_expr
  else subst_nation_query nation_name Queries.source_query_expr.

Lemma qhat_Q21_catalog_instance :
  qhat_Q21 catalog_nation_name = Queries.source_query_expr.
Proof.
  unfold qhat_Q21, catalog_nation_name.
  rewrite String.eqb_refl.
  reflexivity.
Qed.
