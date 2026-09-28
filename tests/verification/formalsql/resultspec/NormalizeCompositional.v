(* NormalizeCompositional — FormalSQL → option LogicalSpec (expression-IR).
   Sole Rocq authority for LogicalSpec. Fail closed on unsupported nodes.
   JSON/Dafny Spec dumps are derived from normalize_q1 output only.

   normalize_q1: exact structural recognition of the supported Q1 AST
   (≡ qhat_Q1 δ for some δ). Checks SELECT / GROUP BY / ORDER BY / filter /
   table expressions — not spine arity alone. Returns the parameterized
   LogicalSpec_Q1 template (params bound at Eval). *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueNumericTypmod
  ValueString ValueInteger ValueTemporal Formula SchemaConstraints SqlOutcome
  SqlQuerySyntax SqlOrder FiniteBag FiniteCollection FiniteSet FTuples
  FTerms ATerms.
From Logos Require Import FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax.
From LogosGenerated Require Import Queries.
From Stdlib Require Import String List ZArith Bool Nat.
Import ListNotations.
Import Tuple.
Import NullValues.
Open Scope string_scope.
Open Scope Z_scope.

From FormalSQLResultSpec Require Import LogicalSpec.
From FormalSQLResultSpec Require Import QueryResultSpec.
From FormalSQLResultSpec Require Import NormalizeQuery. (* extract_delta_from_filter *)

(** * Lightweight structural equality (String/Z only — no Oset/eq_dec) *)

Definition string_typmod_eqb (a b : string_typmod) : bool :=
  match a, b with
  | StringChar n1, StringChar n2 => Nat.eqb n1 n2
  | StringVarcharN n1, StringVarcharN n2 => Nat.eqb n1 n2
  | StringText, StringText
  | StringVarchar, StringVarchar
  | StringBpchar, StringBpchar => true
  | _, _ => false
  end.

Definition attribute_eqb (a b : attribute TNull) : bool :=
  match a, b with
  | Attr_string n1 tm1, Attr_string n2 tm2 =>
      andb (String.eqb n1 n2) (string_typmod_eqb tm1 tm2)
  | Attr_Z n1, Attr_Z n2
  | Attr_int32 n1, Attr_int32 n2
  | Attr_int64 n1, Attr_int64 n2
  | Attr_bool n1, Attr_bool n2
  | Attr_float n1, Attr_float n2
  | Attr_double n1, Attr_double n2
  | Attr_numeric n1, Attr_numeric n2
  | Attr_date n1, Attr_date n2
  | Attr_time n1, Attr_time n2 => String.eqb n1 n2
  | Attr_decimal n1 p1 s1, Attr_decimal n2 p2 s2 =>
      andb (String.eqb n1 n2) (andb (Z.eqb p1 p2) (Z.eqb s1 s2))
  | Attr_timestamp n1 p1, Attr_timestamp n2 p2
  | Attr_timestamptz n1 p1, Attr_timestamptz n2 p2 =>
      andb (String.eqb n1 n2) (Z.eqb p1 p2)
  | _, _ => false
  end.

Definition type_eqb (a b : type TNull) : bool := TNullTypeEqb a b.

Definition value_eqb (v1 v2 : value) : bool :=
  match v1, v2 with
  | Value_Z (Some z1), Value_Z (Some z2) => Z.eqb z1 z2
  | Value_Z None, Value_Z None => true
  | Value_date (Some z1), Value_date (Some z2) => Z.eqb z1 z2
  | Value_date None, Value_date None => true
  | Value_int32 (Some i1), Value_int32 (Some i2) =>
      (* Exact: compare represented Z, not constructor-only. *)
      Z.eqb (int32_value i1) (int32_value i2)
  | Value_int32 None, Value_int32 None => true
  | Value_int64 (Some i1), Value_int64 (Some i2) =>
      Z.eqb (int64_value i1) (int64_value i2)
  | Value_int64 None, Value_int64 None => true
  (* Non-Q1 constant leaves: constructor match only (unused by normalize_q1). *)
  | Value_string _, Value_string _
  | Value_bool _, Value_bool _
  | Value_float _, Value_float _
  | Value_double _, Value_double _
  | Value_numeric _, Value_numeric _
  | Value_time _, Value_time _
  | Value_timestamp _, Value_timestamp _
  | Value_timestamptz _, Value_timestamptz _ => true
  | _, _ => false
  end.

Definition funterm_eqb (a b : @funterm TNull) : bool :=
  match a, b with
  | F_Dot _ a1, F_Dot _ a2 => attribute_eqb a1 a2
  | F_Constant _ v1, F_Constant _ v2 => value_eqb v1 v2
  (* F_Expr is outside the Q1 leaf fragment (ops are SExpr_Call). *)
  | F_Expr _ _ _, F_Expr _ _ _ => true
  | _, _ => false
  end.

Definition aggregate_function_eqb (a b : aggregate_function) : bool :=
  match a, b with
  | AggregateAverageNumericFixed p1 s1, AggregateAverageNumericFixed p2 s2
  | AggregateStddevSampleNumericFixed p1 s1, AggregateStddevSampleNumericFixed p2 s2 =>
      andb (Z.eqb p1 p2) (Z.eqb s1 s2)
  | AggregateAverageNumericAtScale s1, AggregateAverageNumericAtScale s2 =>
      Z.eqb s1 s2
  | AggregateNumericDisplayScale _, AggregateNumericDisplayScale _ => true
  | AggregateCount, AggregateCount
  | AggregateSumZ, AggregateSumZ
  | AggregateSumInt32, AggregateSumInt32
  | AggregateSumInt64Numeric, AggregateSumInt64Numeric
  | AggregateSumFloat, AggregateSumFloat
  | AggregateSumDouble, AggregateSumDouble
  | AggregateSumNumeric, AggregateSumNumeric
  | AggregateBitAndInt32, AggregateBitAndInt32
  | AggregateBitOrInt32, AggregateBitOrInt32
  | AggregateBitAndInt64, AggregateBitAndInt64
  | AggregateBitOrInt64, AggregateBitOrInt64
  | AggregateMaxZ, AggregateMaxZ
  | AggregateMaxInt32, AggregateMaxInt32
  | AggregateMaxInt64, AggregateMaxInt64
  | AggregateMaxFloat, AggregateMaxFloat
  | AggregateMaxDouble, AggregateMaxDouble
  | AggregateMaxNumeric, AggregateMaxNumeric
  | AggregateMaxString, AggregateMaxString
  | AggregateMinZ, AggregateMinZ
  | AggregateMinInt32, AggregateMinInt32
  | AggregateMinInt64, AggregateMinInt64
  | AggregateMinFloat, AggregateMinFloat
  | AggregateMinDouble, AggregateMinDouble
  | AggregateMinNumeric, AggregateMinNumeric
  | AggregateSingleValueInt32, AggregateSingleValueInt32
  | AggregateAverageZ, AggregateAverageZ
  | AggregateAverageInt32Numeric, AggregateAverageInt32Numeric
  | AggregateAverageInt64Numeric, AggregateAverageInt64Numeric
  | AggregateVariancePopulationInt32, AggregateVariancePopulationInt32
  | AggregateVarianceSampleInt32, AggregateVarianceSampleInt32
  | AggregateStddevPopulationInt32, AggregateStddevPopulationInt32
  | AggregateStddevSampleInt32, AggregateStddevSampleInt32
  | AggregateAverageFloat, AggregateAverageFloat
  | AggregateAverageDouble, AggregateAverageDouble => true
  | _, _ => false
  end.

Definition aggregate_quantifier_eqb (a b : aggregate_quantifier) : bool :=
  match a, b with
  | AggregateAll, AggregateAll => true
  | AggregateDistinct, AggregateDistinct => true
  | _, _ => false
  end.

Definition scalar_numeric_kind_eqb (a b : scalar_numeric_kind) : bool :=
  match a, b with
  | ScalarInt32, ScalarInt32
  | ScalarInt64, ScalarInt64
  | ScalarFloat, ScalarFloat
  | ScalarDouble, ScalarDouble
  | ScalarNumeric, ScalarNumeric => true
  | _, _ => false
  end.

Definition scalar_numeric_source_eqb (a b : scalar_numeric_source) : bool :=
  match a, b with
  | ScalarSourceZ, ScalarSourceZ
  | ScalarSourceInt32, ScalarSourceInt32
  | ScalarSourceInt64, ScalarSourceInt64
  | ScalarSourceNumeric, ScalarSourceNumeric => true
  | _, _ => false
  end.

Definition scalar_cast_eqb (a b : scalar_cast) : bool :=
  match a, b with
  | ScalarCastToNumeric s1, ScalarCastToNumeric s2
  | ScalarCastToNumericTypmod s1, ScalarCastToNumericTypmod s2 =>
      scalar_numeric_source_eqb s1 s2
  | ScalarCastIdentity, ScalarCastIdentity
  | ScalarCastInt32ToDouble, ScalarCastInt32ToDouble
  | ScalarCastInt32ToInt64, ScalarCastInt32ToInt64
  | ScalarCastInt64ToInt32, ScalarCastInt64ToInt32
  | ScalarCastNumericToInt32, ScalarCastNumericToInt32
  | ScalarCastStringToInt32, ScalarCastStringToInt32
  | ScalarCastStringToInt64, ScalarCastStringToInt64
  | ScalarCastDateToTimestamp, ScalarCastDateToTimestamp
  | ScalarCastTimestampToDate, ScalarCastTimestampToDate
  | ScalarCastStringExplicit, ScalarCastStringExplicit
  | ScalarCoerceStringImplicit, ScalarCoerceStringImplicit => true
  | _, _ => false
  end.

Definition scalar_timestamp_unit_eqb (a b : scalar_timestamp_unit) : bool :=
  match a, b with
  | ScalarTimestampMicrosecond, ScalarTimestampMicrosecond
  | ScalarTimestampSecond, ScalarTimestampSecond
  | ScalarTimestampMinute, ScalarTimestampMinute
  | ScalarTimestampHour, ScalarTimestampHour
  | ScalarTimestampDay, ScalarTimestampDay
  | ScalarTimestampMonth, ScalarTimestampMonth
  | ScalarTimestampYear, ScalarTimestampYear => true
  | _, _ => false
  end.

(** Exact on Q1 ops; reflexive same-constructor match for the rest. *)
(** Structural equality on the closed FormalSQL predicate vocabulary.
    Required so [SExpr_Pred] compare operators, not only argument lists
    (e.g. [shipdate <= cutoff] must not match a different pred with equal args). *)
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

Lemma predicate_eqb_refl : forall p, predicate_eqb p p = true.
Proof. intros p; destruct p; reflexivity. Qed.

Definition scalar_op_eqb (op1 op2 : scalar_operator TNull) : bool :=
  match op1, op2 with
  | ScalarMultiply k1, ScalarMultiply k2
  | ScalarSubtract k1, ScalarSubtract k2
  | ScalarAdd k1, ScalarAdd k2
  | ScalarDivide k1, ScalarDivide k2
  | ScalarNegate k1, ScalarNegate k2 => scalar_numeric_kind_eqb k1 k2
  | ScalarCast c1, ScalarCast c2 => scalar_cast_eqb c1 c2
  | ScalarTimestampAdd u1, ScalarTimestampAdd u2 => scalar_timestamp_unit_eqb u1 u2
  | ScalarCase, ScalarCase
  | ScalarNumericDivideResultScale, ScalarNumericDivideResultScale
  | ScalarNumericDivideTypmod, ScalarNumericDivideTypmod
  | ScalarPowerHalfInt64ToInt32, ScalarPowerHalfInt64ToInt32
  | ScalarStringConcat, ScalarStringConcat
  | ScalarSubstringNonnegative, ScalarSubstringNonnegative => true
  | ScalarPredicateValue _, ScalarPredicateValue _
  | ScalarBoolean _, ScalarBoolean _
  | ScalarStringCase _, ScalarStringCase _
  | ScalarExtractDate _, ScalarExtractDate _ => true
  | _, _ => false
  end.

Fixpoint aggterm_eqb (a b : @aggterm TNull) {struct a} : bool :=
  match a, b with
  | A_Expr _ t1, A_Expr _ t2 => funterm_eqb t1 t2
  | A_agg _ (AggregateCall f1 q1) t1, A_agg _ (AggregateCall f2 q2) t2 =>
      andb (aggregate_function_eqb f1 f2)
      (andb (aggregate_quantifier_eqb q1 q2) (funterm_eqb t1 t2))
  | A_agg _ AggregateCountStar _, A_agg _ AggregateCountStar _ => true
  | A_fun _ op1 args1, A_fun _ op2 args2 =>
      let fix list_eq xs ys :=
        match xs, ys with
        | nil, nil => true
        | x :: xs', y :: ys' => andb (aggterm_eqb x y) (list_eq xs' ys')
        | _, _ => false
        end in
      andb (scalar_op_eqb op1 op2) (list_eq args1 args2)
  | _, _ => false
  end.

Fixpoint scalar_expr_eqb
    {R} (a b : @scalar_expr TNull relname R) {struct a} : bool :=
  let fix list_eq_value
      (xs ys : list (@scalar_expr TNull relname ScalarResultValue)) : bool :=
    match xs, ys with
    | nil, nil => true
    | x :: xs', y :: ys' =>
        andb (scalar_expr_eqb x y) (list_eq_value xs' ys')
    | _, _ => false
    end in
  let fix list_eq_bool
      (xs ys : list (@scalar_expr TNull relname ScalarResultBoolean)) : bool :=
    match xs, ys with
    | nil, nil => true
    | x :: xs', y :: ys' =>
        andb (scalar_expr_eqb x y) (list_eq_bool xs' ys')
    | _, _ => false
    end in
  match a, b with
  | SExpr_Leaf rt1 term1, SExpr_Leaf rt2 term2 =>
      andb (type_eqb rt1 rt2) (aggterm_eqb term1 term2)
  | SExpr_Call rt1 op1 args1, SExpr_Call rt2 op2 args2 =>
      andb (type_eqb rt1 rt2)
      (andb (scalar_op_eqb op1 op2) (list_eq_value args1 args2))
  | SExpr_Pred p1 args1, SExpr_Pred p2 args2 =>
      andb (predicate_eqb p1 p2) (list_eq_value args1 args2)
  | SExpr_True, SExpr_True => true
  | SExpr_Case rt1 c1 t1 e1, SExpr_Case rt2 c2 t2 e2 =>
      andb (type_eqb rt1 rt2)
      (andb (scalar_expr_eqb c1 c2)
      (andb (scalar_expr_eqb t1 t2) (scalar_expr_eqb e1 e2)))
  | SExpr_BoolValue rt1 _ e1, SExpr_BoolValue rt2 _ e2 =>
      andb (type_eqb rt1 rt2) (scalar_expr_eqb e1 e2)
  | SExpr_ValueBool _ e1, SExpr_ValueBool _ e2 => scalar_expr_eqb e1 e2
  | SExpr_ConjList _ _ es1, SExpr_ConjList _ _ es2 => list_eq_bool es1 es2
  | SExpr_Not e1, SExpr_Not e2 => scalar_expr_eqb e1 e2
  | SExpr_Quant _ _ args1 _, SExpr_Quant _ _ args2 _ => list_eq_value args1 args2
  | SExpr_In args1 _, SExpr_In args2 _ => list_eq_value args1 args2
  | SExpr_Exists _, SExpr_Exists _ => true
  | SExpr_Subquery rt1 _ _, SExpr_Subquery rt2 _ _ => type_eqb rt1 rt2
  | _, _ => false
  end.

Definition select_item_eqb
    (a b : (@scalar_expr TNull relname ScalarResultValue * attribute TNull)%type)
  : bool :=
  andb (scalar_expr_eqb (fst a) (fst b)) (attribute_eqb (snd a) (snd b)).

Fixpoint select_list_eqb
    (xs ys : list
      (@scalar_expr TNull relname ScalarResultValue * attribute TNull)%type)
  : bool :=
  match xs, ys with
  | nil, nil => true
  | x :: xs', y :: ys' =>
      andb (select_item_eqb x y) (select_list_eqb xs' ys')
  | _, _ => false
  end.

Definition sort_key_eqb (a b : sort_key TNull) : bool :=
  let dir_ok :=
    match sort_key_direction a, sort_key_direction b with
    | Sort_Asc, Sort_Asc | Sort_Desc, Sort_Desc => true
    | _, _ => false
    end in
  let nulls_ok :=
    match sort_key_null_direction a, sort_key_null_direction b with
    | Nulls_Last, Nulls_Last | Nulls_First, Nulls_First => true
    | _, _ => false
    end in
  andb (attribute_eqb (sort_key_attribute a) (sort_key_attribute b))
       (andb dir_ok nulls_ok).

Fixpoint sort_keys_eqb (xs ys : list (sort_key TNull)) : bool :=
  match xs, ys with
  | nil, nil => true
  | x :: xs', y :: ys' => andb (sort_key_eqb x y) (sort_keys_eqb xs' ys')
  | _, _ => false
  end.

Fixpoint attr_list_eqb (xs ys : list (attribute TNull)) : bool :=
  match xs, ys with
  | nil, nil => true
  | x :: xs', y :: ys' => andb (attribute_eqb x y) (attr_list_eqb xs' ys')
  | _, _ => false
  end.

Fixpoint group_keys_eqb
    (xs ys : list (@scalar_expr TNull relname ScalarResultValue)) : bool :=
  match xs, ys with
  | nil, nil => true
  | x :: xs', y :: ys' =>
      andb (scalar_expr_eqb x y) (group_keys_eqb xs' ys')
  | _, _ => false
  end.

(** Exact Q1 table outputs (same as q1_table_outputs in Q1FormalSQLProof). *)
Definition q1_expected_table_outputs : list (attribute TNull) :=
  [ AttrInt64 "l_orderkey"
  ; AttrInt64 "l_partkey"
  ; AttrInt64 "l_suppkey"
  ; AttrInt32 "l_linenumber"
  ; AttrDecimal "l_quantity" 15 2
  ; AttrDecimal "l_extendedprice" 15 2
  ; AttrDecimal "l_discount" 15 2
  ; AttrDecimal "l_tax" 15 2
  ; AttrString "l_returnflag" (StringChar 1)
  ; AttrString "l_linestatus" (StringChar 1)
  ; AttrDate "l_shipdate"
  ; AttrDate "l_commitdate"
  ; AttrDate "l_receiptdate"
  ; AttrString "l_shipinstruct" (StringChar 25)
  ; AttrString "l_shipmode" (StringChar 10)
  ; AttrString "l_comment" (StringVarcharN 44)
  ].

Definition q1_expected_group_keys
  : list (@scalar_expr TNull relname ScalarResultValue) :=
  [ @SExpr_Leaf TNull relname type_string (DotString "l_returnflag" (StringChar 1))
  ; @SExpr_Leaf TNull relname type_string (DotString "l_linestatus" (StringChar 1))
  ].

Definition q1_expected_order_keys : list (sort_key TNull) :=
  [ SortAscNullsLast (AttrString "l_returnflag" (StringChar 1))
  ; SortAscNullsLast (AttrString "l_linestatus" (StringChar 1))
  ].

Definition q1_filter_at_expected (delta : Z)
  : @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname
    (PredicateDateLteTimestamp : FTuples.Tuple.predicate TNull)
    [@SExpr_Leaf TNull relname type_date (DotDate "l_shipdate")
    ; @SExpr_Call TNull relname type_timestamp
        (ScalarTimestampAdd ScalarTimestampDay)
        [@SExpr_Call TNull relname type_timestamp
           (ScalarCast ScalarCastDateToTimestamp)
           [@SExpr_Leaf TNull relname type_date (CstDate q1_base_date)]
        ; @SExpr_Leaf TNull relname type_Z (CstZ (-delta))]].

Definition is_q1_filter_at
    (f : @scalar_expr TNull relname ScalarResultBoolean) (delta : Z) : bool :=
  scalar_expr_eqb f (q1_filter_at_expected delta).

Definition table_is_lineitem (table : relname) : bool :=
  match table with
  | Rel name => String.eqb name "lineitem"
  end.

(** Exact structural match against supported Q1 AST (= qhat_Q1 δ). *)
Definition is_exact_qhat_Q1_ast
    (outer : list
      (@scalar_expr TNull relname ScalarResultValue * attribute TNull)%type)
    (ords : list (sort_key TNull))
    (gsel : list
      (@scalar_expr TNull relname ScalarResultValue * attribute TNull)%type)
    (gkeys : list (@scalar_expr TNull relname ScalarResultValue))
    (having : @scalar_expr TNull relname ScalarResultBoolean)
    (inner : list
      (@scalar_expr TNull relname ScalarResultValue * attribute TNull)%type)
    (filt : @scalar_expr TNull relname ScalarResultBoolean)
    (outs : list (attribute TNull))
    (table : relname)
    (delta : Z) : bool :=
  andb (table_is_lineitem table)
  (andb (attr_list_eqb outs q1_expected_table_outputs)
  (andb (is_q1_filter_at filt delta)
  (andb (select_list_eqb inner Queries.scalar_select_list_2)
  (andb (select_list_eqb gsel Queries.scalar_select_list_1)
  (andb (group_keys_eqb gkeys q1_expected_group_keys)
  (andb (match having with SExpr_True => true | _ => false end)
  (andb (sort_keys_eqb ords q1_expected_order_keys)
        (select_list_eqb outer Queries.scalar_select_list_0)))))))).

(** Fail-closed Q1 normalize: exact AST match → parameterized LogicalSpec_Q1. *)
Definition normalize_q1 (q : QueryExpr) : option LogicalSpec :=
  match q with
  | QExpr_Project outer
      (QExpr_OrderBy ords
        (QExpr_Group gsel gkeys having
          (QExpr_Project inner
            (QExpr_Filter filt
              (QExpr_Table outs table))))) =>
      match extract_delta_from_filter filt with
      | Some delta =>
          if is_exact_qhat_Q1_ast outer ords gsel gkeys having inner
               filt outs table delta
          then Some LogicalSpec_Q1
          else None
      | None => None
      end
  | _ => None
  end.

(** Legacy name used by older scripts: same as normalize_q1. *)
Definition normalize (q : QueryExpr) : option LogicalSpec :=
  normalize_q1 q.
