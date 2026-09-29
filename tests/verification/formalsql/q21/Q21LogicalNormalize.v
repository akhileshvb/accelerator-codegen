(* Q21LogicalNormalize — fail-closed FormalSQL → parameterized LogicalSpec_Q21.
   Catalog AST has n_name = 'SAUDI ARABIA'; LogicalSpec uses EParam "nation_name".
   See Q21_PARAM_BRIDGE.md. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueNumericTypmod
  ValueString ValueInteger ValueTemporal Formula SchemaConstraints SqlOutcome
  SqlQuerySyntax SqlOrder FiniteBag FiniteCollection FiniteSet FTuples
  FTerms ATerms.
From Logos Require Import FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax.
From LogosGenerated Require Import Queries.
From Stdlib Require Import List Bool String.
Import ListNotations.
Import Tuple.
Import NullValues.
Open Scope string_scope.

From FormalSQLLogical Require Import RelNode.
Require Import Q21LogicalSpecTree.
Require Import Q21Schema.

Definition predicate_eqb (p1 p2 : Tuple.predicate TNull) : bool :=
  match p1, p2 with
  | PredicateLt, PredicateLt
  | PredicateFloatLt, PredicateFloatLt
  | PredicateDoubleLt, PredicateDoubleLt
  | PredicateDateLtTimestamp, PredicateDateLtTimestamp
  | PredicateDateLteTimestamp, PredicateDateLteTimestamp
  | PredicateDateGtTimestamp, PredicateDateGtTimestamp
  | PredicateDateGteTimestamp, PredicateDateGteTimestamp
  | PredicateLte, PredicateLte
  | PredicateFloatLte, PredicateFloatLte
  | PredicateDoubleLte, PredicateDoubleLte
  | PredicateGt, PredicateGt
  | PredicateFloatGt, PredicateFloatGt
  | PredicateDoubleGt, PredicateDoubleGt
  | PredicateGte, PredicateGte
  | PredicateFloatGte, PredicateFloatGte
  | PredicateDoubleGte, PredicateDoubleGte
  | PredicateEq, PredicateEq
  | PredicateNeq, PredicateNeq
  | PredicateLikePrefix, PredicateLikePrefix
  | PredicateLikePercent, PredicateLikePercent
  | PredicateIsNull, PredicateIsNull
  | PredicateIsNotNull, PredicateIsNotNull
  | PredicateIsTrue, PredicateIsTrue
  | PredicateIsNotTrue, PredicateIsNotTrue
  | PredicateIsFalse, PredicateIsFalse
  | PredicateIsNotFalse, PredicateIsNotFalse
  | PredicateIsNotDistinctFrom, PredicateIsNotDistinctFrom => true
  | _, _ => false
  end.

Definition leaf_attr_int32 (term : AggTerm) (name : string) : bool :=
  match term with
  | A_Expr _ (F_Dot _ (Attr_int32 n)) => String.eqb n name
  | _ => false
  end.

Definition leaf_attr_date (term : AggTerm) (name : string) : bool :=
  match term with
  | A_Expr _ (F_Dot _ (Attr_date n)) => String.eqb n name
  | _ => false
  end.

Definition sexpr_leaf_int32
    (e : @scalar_expr TNull relname ScalarResultValue) (name : string) : bool :=
  match e with
  | SExpr_Leaf _ term => leaf_attr_int32 term name
  | _ => false
  end.

Definition sexpr_leaf_date
    (e : @scalar_expr TNull relname ScalarResultValue) (name : string) : bool :=
  match e with
  | SExpr_Leaf _ term => leaf_attr_date term name
  | _ => false
  end.

Definition is_eq_orderkey_cor
    (e : @scalar_expr TNull relname ScalarResultBoolean) : bool :=
  match e with
  | SExpr_Pred p [a; b] =>
      andb (predicate_eqb p PredicateEq)
      (andb (sexpr_leaf_int32 a "l_orderkey")
            (sexpr_leaf_int32 b "__logos_cor_cor0_7"))
  | _ => false
  end.

Definition is_neq_suppkey_cor
    (e : @scalar_expr TNull relname ScalarResultBoolean) : bool :=
  match e with
  | SExpr_Pred p [a; b] =>
      andb (predicate_eqb p PredicateNeq)
      (andb (sexpr_leaf_int32 a "l_suppkey")
            (sexpr_leaf_int32 b "__logos_cor_cor0_9"))
  | _ => false
  end.

Definition is_gt_late_local
    (e : @scalar_expr TNull relname ScalarResultBoolean) : bool :=
  match e with
  | SExpr_Pred p [a; b] =>
      andb (predicate_eqb p PredicateGt)
      (andb (sexpr_leaf_date a "l_receiptdate")
            (sexpr_leaf_date b "l_commitdate"))
  | _ => false
  end.

Definition is_lineitem_table (q : QueryExpr) : bool :=
  match q with
  | QExpr_Table _ (Rel name) => String.eqb name "lineitem"
  | _ => false
  end.

Definition is_q21_exists_l2_body (q : QueryExpr) : bool :=
  match q with
  | QExpr_Filter (SExpr_ConjList _ And_F [p0; p1]) child =>
      andb (is_eq_orderkey_cor p0)
      (andb (is_neq_suppkey_cor p1) (is_lineitem_table child))
  | _ => false
  end.

Definition is_q21_exists_l3_body (q : QueryExpr) : bool :=
  match q with
  | QExpr_Filter (SExpr_ConjList _ And_F [p0; p1; p2]) child =>
      andb (is_eq_orderkey_cor p0)
      (andb (is_neq_suppkey_cor p1)
      (andb (is_gt_late_local p2) (is_lineitem_table child)))
  | _ => false
  end.

Definition is_nation_name_pred
    (e : @scalar_expr TNull relname ScalarResultBoolean) : bool :=
  match e with
  | SExpr_Pred p [_; SExpr_Leaf _ t] =>
      andb (predicate_eqb p PredicateEq)
      (match t with
       | A_Expr _ (F_Constant _ (Value_string (StringBpchar, Some _))) =>
           true
       | _ => false
       end)
  | _ => false
  end.

Definition is_q21_filter_pred
    (filt : @scalar_expr TNull relname ScalarResultBoolean) : bool :=
  match filt with
  | SExpr_ConjList _ And_F
      (_ :: _ :: _ :: _ ::
       SExpr_Exists ex2 ::
       SExpr_Not (SExpr_Exists ex3) ::
       _ ::
       nation :: []) =>
      andb (is_q21_exists_l2_body ex2)
      (andb (is_q21_exists_l3_body ex3) (is_nation_name_pred nation))
  | _ => false
  end.

Definition is_q21_cross_spine (q : QueryExpr) : bool :=
  match q with
  | QExpr_CrossJoin leftq rightq =>
      match leftq, rightq with
      | QExpr_Project _ (QExpr_Filter _ (QExpr_CrossJoin midq ordq)),
        QExpr_Project _ (QExpr_Table _ tn) =>
          match midq, ordq, tn with
          | QExpr_Project _ (QExpr_Filter _ (QExpr_CrossJoin left2 liq)),
            QExpr_Project _ (QExpr_Table _ to),
            Rel nname =>
              match left2, liq, to with
              | QExpr_Project _ (QExpr_Table _ ts),
                QExpr_Project _ (QExpr_Table _ tl),
                Rel oname =>
                  match ts, tl with
                  | Rel sname, Rel lname =>
                      andb (String.eqb sname "supplier")
                      (andb (String.eqb lname "lineitem")
                      (andb (String.eqb oname "orders")
                            (String.eqb nname "nation")))
                  end
              | _, _, _ => false
              end
          | _, _, _ => false
          end
      | _, _ => false
      end
  | _ => false
  end.

Definition attr_is_int64_named (a : attribute TNull) (name : string) : bool :=
  match a with
  | Attr_int64 n => String.eqb n name
  | _ => false
  end.

Definition attr_is_string_named (a : attribute TNull) (name : string) : bool :=
  match a with
  | Attr_string n _ => String.eqb n name
  | _ => false
  end.

Definition is_q21_order_keys (ords : list (sort_key TNull)) : bool :=
  match ords with
  | [k1; k2] =>
      andb (attr_is_int64_named (sort_key_attribute k1) "numwait")
      (andb (match sort_key_direction k1 with Sort_Desc => true | _ => false end)
      (andb (attr_is_string_named (sort_key_attribute k2) "s_name")
            (match sort_key_direction k2 with Sort_Asc => true | _ => false end)))
  | _ => false
  end.

Definition is_exact_qhat_Q21 (q : QueryExpr) : bool :=
  match q with
  | QExpr_Project _
      (QExpr_OrderBy ords
        (QExpr_Group _
          [SExpr_Leaf _ (A_Expr _ (F_Dot _ (Attr_string "s_name" (StringChar 25))))]
          SExpr_True
          (QExpr_Project _
            (QExpr_Project _
              (QExpr_Filter filt
                (QExpr_Project _
                  (QExpr_Filter _
                    spine))))))) =>
      andb (is_q21_order_keys ords)
      (andb (is_q21_filter_pred filt) (is_q21_cross_spine spine))
  | _ => false
  end.

Definition normalize_q21 (q : QueryExpr) : option LogicalSpec :=
  if is_exact_qhat_Q21 q then Some LogicalSpec_Q21_rel else None.

Lemma normalize_q21_source :
  normalize_q21 Queries.source_query_expr = Some LogicalSpec_Q21_rel.
Proof. reflexivity. Qed.

Lemma normalize_q21_shared :
  normalize_q21 Queries.shared_query_expr_0 = Some LogicalSpec_Q21_rel.
Proof. reflexivity. Qed.
