(* Generic FormalSQL CASE exact facade.

   Packages [EScalar_CaseThen] / [EScalar_CaseElse] so query proofs obtain
   [scalar_value_expr_exact_at] for [SExpr_Case] from condition acceptance
   plus exactness of the selected arm — without stepping CASE constructors
   in query files. *)

From Stdlib Require Import List.
From SQLFS Require Import
  OrderedSet FiniteSet FiniteBag FiniteCollection FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts Values GenericInstance.
From Logos.FormalSQL Require Import
  TNullSyntax QueryTNullSyntax GroupedFilterOutcomeFacts.

Import Tuple.
Import ListNotations.

Section CaseExact.

Context {T : Tuple.Rcd} {relname : Type}.

Variable basesort : relname -> Fset.set (A T).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple T)).
Variable unknown : Bool.b (B T).
Variable symbol_runtime_error :
  scalar_operator T -> list (option sql_runtime_error * value T) ->
  option sql_runtime_error.
Variable aggregate_runtime_error :
  aggregate T -> list (option sql_runtime_error * value T) ->
  option sql_runtime_error.
Variable value_is_null : value T -> bool.
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Local Abbreviation acceptance_at :=
  (@scalar_expr_acceptance_exact_at T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

Local Abbreviation value_exact :=
  (@scalar_value_expr_exact_at T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

Theorem scalar_value_expr_case_exact_of_acceptance :
  forall env result_type condition then_expression else_expression
      accepted then_value else_value,
    acceptance_at env condition accepted ->
    (accepted = true -> value_exact env then_expression then_value) ->
    (accepted = false -> value_exact env else_expression else_value) ->
    value_exact env
      (SExpr_Case result_type condition then_expression else_expression)
      (if accepted then then_value else else_value).
Proof.
  intros env result_type condition then_expression else_expression
    accepted then_value else_value
    [[truth [Hcond His]] [Hcond_uniq Hcond_safe]] Hthen Helse.
  unfold scalar_value_expr_exact_at.
  destruct accepted eqn:Ha.
  - specialize (Hthen eq_refl) as [Htsucc [Htuniq Htsafe]].
    split.
    + eapply EScalar_CaseThen; [exact Hcond|exact His|exact Htsucc].
    + split.
      * intros observed Heval.
        inversion Heval; subst; try discriminate.
        -- match goal with
           | Harm : eval_scalar_value_expr_outcome _ _ _ _ _ _ _ _
                      then_expression (SqlSuccess observed) |- _ =>
             rewrite (Htuniq observed Harm); reflexivity
           end.
        -- match goal with
           | Htruth : eval_scalar_boolean_expr_outcome _ _ _ _ _ _ _ _
                        ?cond (SqlSuccess ?tr),
             Hsel : Bool.is_true _ ?tr = false |- _ =>
             pose proof (Hcond_uniq tr Htruth) as His';
             rewrite His' in Hsel; discriminate Hsel
           end.
      * intros error Heval.
        inversion Heval; subst; try discriminate.
        -- match goal with
           | Hc : eval_scalar_boolean_expr_outcome _ _ _ _ _ _ _ _
                    ?cond (SqlError ?e) |- _ =>
             elim (Hcond_safe e Hc)
           end.
        -- match goal with
           | Harm : eval_scalar_value_expr_outcome _ _ _ _ _ _ _ _
                      then_expression (SqlError ?e),
             Hsel : Bool.is_true _ _ = true |- _ =>
             elim (Htsafe e Harm)
           end.
        -- match goal with
           | Htruth : eval_scalar_boolean_expr_outcome _ _ _ _ _ _ _ _
                        ?cond (SqlSuccess ?tr),
             Hsel : Bool.is_true _ ?tr = false |- _ =>
             pose proof (Hcond_uniq tr Htruth) as His';
             rewrite His' in Hsel; discriminate Hsel
           end.
  - specialize (Helse eq_refl) as [Hesucc [Heuniq Hesafe]].
    split.
    + eapply EScalar_CaseElse; [exact Hcond|exact His|exact Hesucc].
    + split.
      * intros observed Heval.
        inversion Heval; subst; try discriminate.
        -- match goal with
           | Htruth : eval_scalar_boolean_expr_outcome _ _ _ _ _ _ _ _
                        ?cond (SqlSuccess ?tr),
             Hsel : Bool.is_true _ ?tr = true |- _ =>
             pose proof (Hcond_uniq tr Htruth) as His';
             rewrite His' in Hsel; discriminate Hsel
           end.
        -- match goal with
           | Harm : eval_scalar_value_expr_outcome _ _ _ _ _ _ _ _
                      else_expression (SqlSuccess observed) |- _ =>
             rewrite (Heuniq observed Harm); reflexivity
           end.
      * intros error Heval.
        inversion Heval; subst; try discriminate.
        -- match goal with
           | Hc : eval_scalar_boolean_expr_outcome _ _ _ _ _ _ _ _
                    ?cond (SqlError ?e) |- _ =>
             elim (Hcond_safe e Hc)
           end.
        -- match goal with
           | Htruth : eval_scalar_boolean_expr_outcome _ _ _ _ _ _ _ _
                        ?cond (SqlSuccess ?tr),
             Hsel : Bool.is_true _ ?tr = true |- _ =>
             pose proof (Hcond_uniq tr Htruth) as His';
             rewrite His' in Hsel; discriminate Hsel
           end.
        -- match goal with
           | Harm : eval_scalar_value_expr_outcome _ _ _ _ _ _ _ _
                      else_expression (SqlError ?e),
             Hsel : Bool.is_true _ _ = false |- _ =>
             elim (Hesafe e Harm)
           end.
Qed.

End CaseExact.

Section CaseExact_TNull.

Variable basesort : relname -> Fset.set (A TNull).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple TNull)).
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Definition tnull_value_is_null (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

Theorem tnull_scalar_value_expr_case_exact :
  forall env result_type condition then_expression else_expression
      accepted then_value else_value,
    @scalar_expr_acceptance_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env condition accepted ->
    (accepted = true ->
      @scalar_value_expr_exact_at TNull relname basesort instance unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        tnull_value_is_null boolean_schedule env then_expression then_value) ->
    (accepted = false ->
      @scalar_value_expr_exact_at TNull relname basesort instance unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        tnull_value_is_null boolean_schedule env else_expression else_value) ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (SExpr_Case result_type condition then_expression else_expression)
      (if accepted then then_value else else_value).
Proof.
  intros; eapply (@scalar_value_expr_case_exact_of_acceptance TNull relname
    basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule); eassumption.
Qed.

End CaseExact_TNull.
