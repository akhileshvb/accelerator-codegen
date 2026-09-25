(* Reusable TNull NUMERIC Call/Mul/Sub/Add/Cast exactness.

   Packages Logos [scalar_value_expr_call_exact] + NullValues numeric
   interp / [numeric_binary_runtime_error_total] so query proofs assemble
   disc_price / charge-style trees from leaf dots + fits premises — no
   [EScalar_*] / [EProjectRows_Cons] nesting in query files. *)

From Stdlib Require Import List String ZArith Lia QArith Qcanon Bool.
From SQLFS Require Import
  OrderedSet FiniteSet FiniteBag FiniteCollection FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts Values ValueCore ValueNumeric ValueInteger GenericInstance
  FTuples Interp ListSort.
From Logos.FormalSQL Require Import
  TNullSyntax QueryTNullSyntax GroupedFilterOutcomeFacts
  NumericFacts NumericDerivedFacts.
From FormalSQLLib Require Import TemporalPredAcceptanceFacade NumericScaledFits.

Import Tuple.
Import ListNotations.
Open Scope string_scope.
Open Scope Z_scope.

(** * Small-carrier fits (TPC-H discount / tax coefficients) *)

Lemma numeric_one_minus_disc_scaled2_fits :
  forall d, 0 <= d <= 10 ->
    numeric_runtime_fits_bool
      (numeric_sub (numeric_of_Z 1) (numeric_of_scaled d 2)) = true.
Proof.
  intros d Hd.
  assert (Hcases : d = 0 \/ d = 1 \/ d = 2 \/ d = 3 \/ d = 4 \/ d = 5
            \/ d = 6 \/ d = 7 \/ d = 8 \/ d = 9 \/ d = 10) by lia.
  destruct Hcases as [H|[H|[H|[H|[H|[H|[H|[H|[H|[H|H]]]]]]]]]];
    rewrite H; vm_compute; reflexivity.
Qed.

Lemma numeric_one_plus_tax_scaled2_fits :
  forall t, 0 <= t <= 8 ->
    numeric_runtime_fits_bool
      (numeric_add (numeric_of_Z 1) (numeric_of_scaled t 2)) = true.
Proof.
  intros t Ht.
  assert (Hcases : t = 0 \/ t = 1 \/ t = 2 \/ t = 3 \/ t = 4 \/ t = 5
            \/ t = 6 \/ t = 7 \/ t = 8) by lia.
  destruct Hcases as [H|[H|[H|[H|[H|[H|[H|[H|H]]]]]]]];
    rewrite H; vm_compute; reflexivity.
Qed.

(** Disc-price / charge factor fits under TPC-H Q1 discount/tax bounds.
    Mul-tree fits for [$f4]/[$f5] are left as premises of
    [tnull_disc_price_exact] / [tnull_charge_exact] (discharged per query
    from domain bounds + digit budget). *)
Lemma numeric_disc_tax_factor_fits_tpch_q1 :
  forall disc tax,
    0 <= disc <= 10 ->
    0 <= tax <= 8 ->
    numeric_runtime_fits_bool
      (numeric_sub (numeric_of_Z 1) (numeric_of_scaled disc 2)) = true /\
    numeric_runtime_fits_bool
      (numeric_add (numeric_of_Z 1) (numeric_of_scaled tax 2)) = true.
Proof.
  intros disc tax Hd Ht.
  split.
  - apply numeric_one_minus_disc_scaled2_fits; exact Hd.
  - apply numeric_one_plus_tax_scaled2_fits; exact Ht.
Qed.

(** Max-price disc-price / charge fits witnesses (largest in-domain magnitude).
    Query proofs combine these with domain bounds to discharge mul-tree fits. *)
Lemma numeric_disc_price_charge_fits_max_price_tpch_q1 :
  forall disc tax,
    0 <= disc <= 10 ->
    0 <= tax <= 8 ->
    numeric_runtime_fits_bool
      (numeric_mul (numeric_of_scaled 1000000000000 2)
        (numeric_sub (numeric_of_Z 1) (numeric_of_scaled disc 2))) = true /\
    numeric_runtime_fits_bool
      (numeric_mul
        (numeric_mul (numeric_of_scaled 1000000000000 2)
          (numeric_sub (numeric_of_Z 1) (numeric_of_scaled disc 2)))
        (numeric_add (numeric_of_Z 1) (numeric_of_scaled tax 2))) = true.
Proof.
  intros disc tax Hd Ht.
  assert (HdC : disc = 0 \/ disc = 1 \/ disc = 2 \/ disc = 3 \/ disc = 4
            \/ disc = 5 \/ disc = 6 \/ disc = 7 \/ disc = 8 \/ disc = 9
            \/ disc = 10) by lia.
  assert (HtC : tax = 0 \/ tax = 1 \/ tax = 2 \/ tax = 3 \/ tax = 4
            \/ tax = 5 \/ tax = 6 \/ tax = 7 \/ tax = 8) by lia.
  destruct HdC as [Hd0|[Hd0|[Hd0|[Hd0|[Hd0|[Hd0|[Hd0|[Hd0|[Hd0|[Hd0|Hd0]]]]]]]]]];
  destruct HtC as [Ht0|[Ht0|[Ht0|[Ht0|[Ht0|[Ht0|[Ht0|[Ht0|Ht0]]]]]]]];
    rewrite Hd0, Ht0; split; vm_compute; reflexivity.
Qed.

(** Lift max-price disc_price / charge fits to any in-domain price. *)
Lemma numeric_disc_price_charge_fits_mono_tpch_q1 :
  forall price Pmax disc tax,
    0 <= price <= Pmax ->
    0 <= disc <= 10 ->
    0 <= tax <= 8 ->
    numeric_runtime_fits_bool
      (numeric_mul (numeric_of_scaled Pmax 2)
        (numeric_sub (numeric_of_Z 1) (numeric_of_scaled disc 2))) = true ->
    numeric_runtime_fits_bool
      (numeric_mul
        (numeric_mul (numeric_of_scaled Pmax 2)
          (numeric_sub (numeric_of_Z 1) (numeric_of_scaled disc 2)))
        (numeric_add (numeric_of_Z 1) (numeric_of_scaled tax 2))) = true ->
    numeric_runtime_fits_bool
      (numeric_mul (numeric_of_scaled price 2)
        (numeric_sub (numeric_of_Z 1) (numeric_of_scaled disc 2))) = true /\
    numeric_runtime_fits_bool
      (numeric_mul
        (numeric_mul (numeric_of_scaled price 2)
          (numeric_sub (numeric_of_Z 1) (numeric_of_scaled disc 2)))
        (numeric_add (numeric_of_Z 1) (numeric_of_scaled tax 2))) = true.
Proof.
  intros price Pmax disc tax Hp Hd Ht Hf4 Hf5.
  rewrite (numeric_one_minus_scaled2 disc) in Hf4, Hf5 |- *.
  rewrite (numeric_one_plus_scaled2 tax) in Hf5 |- *.
  split.
  - eapply numeric_mul_scaled2_fits_mono; [| |exact Hf4]; lia.
  - eapply numeric_mul_scaled2_charge_fits_mono; [| | |exact Hf5]; lia.
Qed.

(** * int32 constant + CAST AS NUMERIC *)

Lemma int32_checked_1_some :
  exists i : int32, int32_checked 1 = Some i /\ int32_value i = 1.
Proof.
  unfold int32_checked.
  destruct (Z.leb_spec0 int32_min 1) as [Hlo|Hlo];
    [|exfalso; unfold int32_min in Hlo; lia].
  destruct (Z.leb_spec0 1 int32_max) as [Hhi|Hhi];
    [|exfalso; unfold int32_max in Hhi; lia].
  exists (Int32 1 (conj Hlo Hhi)).
  split; reflexivity.
Qed.

Lemma tnull_cast_one_to_numeric_exact :
  forall basesort instance boolean_schedule env,
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Call TNull relname type_numeric
        (ScalarCast (ScalarCastToNumeric ScalarSourceInt32))
        [@SExpr_Leaf TNull relname type_int32 (CstInt32 1)])
      (NullValues.Value_numeric (Some (numeric_of_Z 1))).
Proof.
  intros basesort instance boolean_schedule env.
  destruct int32_checked_1_some as [i [Hchk Hval]].
  eapply (@scalar_value_expr_call_exact TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule).
  - eapply (@scalar_value_list_exact_cons TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule).
    + unfold CstInt32. rewrite Hchk.
      apply tnull_leaf_constant_exact.
    + apply (@scalar_value_list_exact_nil TNull relname basesort instance unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        tnull_value_is_null boolean_schedule).
  - apply tnull_scalar_call_success_of_local; [exact I|reflexivity|].
    cbn. rewrite Hval. reflexivity.
Qed.

(** * Binary NUMERIC operators *)

Lemma tnull_numeric_mul_exact :
  forall basesort instance boolean_schedule env el er (vl vr : numeric),
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env el
      (NullValues.Value_numeric (Some vl)) ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env er
      (NullValues.Value_numeric (Some vr)) ->
    numeric_runtime_fits_bool (numeric_mul vl vr) = true ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Call TNull relname type_numeric
        (ScalarMultiply ScalarNumeric) [el; er])
      (NullValues.Value_numeric (Some (numeric_mul vl vr))).
Proof.
  intros basesort instance boolean_schedule env el er vl vr Hel Her Hfits.
  eapply (@scalar_value_expr_call_exact TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule).
  - eapply (@scalar_value_list_exact_cons TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule);
      [exact Hel|].
    eapply (@scalar_value_list_exact_cons TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule);
      [exact Her|].
    apply (@scalar_value_list_exact_nil TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule).
  - apply tnull_scalar_call_success_of_local; [exact I| |reflexivity].
    cbn [NullValues.scalar_operator_local_runtime_error].
    apply numeric_binary_runtime_error_total; exact Hfits.
Qed.

Lemma tnull_numeric_sub_exact :
  forall basesort instance boolean_schedule env el er (vl vr : numeric),
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env el
      (NullValues.Value_numeric (Some vl)) ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env er
      (NullValues.Value_numeric (Some vr)) ->
    numeric_runtime_fits_bool (numeric_sub vl vr) = true ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Call TNull relname type_numeric
        (ScalarSubtract ScalarNumeric) [el; er])
      (NullValues.Value_numeric (Some (numeric_sub vl vr))).
Proof.
  intros basesort instance boolean_schedule env el er vl vr Hel Her Hfits.
  eapply (@scalar_value_expr_call_exact TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule).
  - eapply (@scalar_value_list_exact_cons TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule);
      [exact Hel|].
    eapply (@scalar_value_list_exact_cons TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule);
      [exact Her|].
    apply (@scalar_value_list_exact_nil TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule).
  - apply tnull_scalar_call_success_of_local; [exact I| |reflexivity].
    cbn [NullValues.scalar_operator_local_runtime_error].
    apply numeric_binary_runtime_error_total; exact Hfits.
Qed.

Lemma tnull_numeric_add_exact :
  forall basesort instance boolean_schedule env el er (vl vr : numeric),
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env el
      (NullValues.Value_numeric (Some vl)) ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env er
      (NullValues.Value_numeric (Some vr)) ->
    numeric_runtime_fits_bool (numeric_add vl vr) = true ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Call TNull relname type_numeric
        (ScalarAdd ScalarNumeric) [el; er])
      (NullValues.Value_numeric (Some (numeric_add vl vr))).
Proof.
  intros basesort instance boolean_schedule env el er vl vr Hel Her Hfits.
  eapply (@scalar_value_expr_call_exact TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule).
  - eapply (@scalar_value_list_exact_cons TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule);
      [exact Hel|].
    eapply (@scalar_value_list_exact_cons TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule);
      [exact Her|].
    apply (@scalar_value_list_exact_nil TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule).
  - apply tnull_scalar_call_success_of_local; [exact I| |reflexivity].
    cbn [NullValues.scalar_operator_local_runtime_error].
    apply numeric_binary_runtime_error_total; exact Hfits.
Qed.

(** * Disc-price ($f4) and charge ($f5) expression shapes *)

Definition numeric_one_minus_disc_expr (disc_attr : string) (prec scale : Z)
  : @scalar_expr TNull relname ScalarResultValue :=
  @SExpr_Call TNull relname type_numeric (ScalarSubtract ScalarNumeric)
    [@SExpr_Call TNull relname type_numeric
       (ScalarCast (ScalarCastToNumeric ScalarSourceInt32))
       [@SExpr_Leaf TNull relname type_int32 (CstInt32 1)]
    ; @SExpr_Leaf TNull relname type_numeric (DotDecimal disc_attr prec scale)].

Definition numeric_one_plus_tax_expr (tax_attr : string) (prec scale : Z)
  : @scalar_expr TNull relname ScalarResultValue :=
  @SExpr_Call TNull relname type_numeric (ScalarAdd ScalarNumeric)
    [@SExpr_Call TNull relname type_numeric
       (ScalarCast (ScalarCastToNumeric ScalarSourceInt32))
       [@SExpr_Leaf TNull relname type_int32 (CstInt32 1)]
    ; @SExpr_Leaf TNull relname type_numeric (DotDecimal tax_attr prec scale)].

Definition numeric_disc_price_expr
    (price_attr disc_attr : string) (prec scale : Z)
  : @scalar_expr TNull relname ScalarResultValue :=
  @SExpr_Call TNull relname type_numeric (ScalarMultiply ScalarNumeric)
    [@SExpr_Leaf TNull relname type_numeric (DotDecimal price_attr prec scale)
    ; numeric_one_minus_disc_expr disc_attr prec scale].

Definition numeric_charge_expr
    (price_attr disc_attr tax_attr : string) (prec scale : Z)
  : @scalar_expr TNull relname ScalarResultValue :=
  @SExpr_Call TNull relname type_numeric (ScalarMultiply ScalarNumeric)
    [numeric_disc_price_expr price_attr disc_attr prec scale
    ; numeric_one_plus_tax_expr tax_attr prec scale].

Lemma tnull_dot_decimal_exact :
  forall basesort instance boolean_schedule env name prec scale expected,
    Interp.interp_dot TNull env (AttrDecimal name prec scale) = expected ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Leaf TNull relname type_numeric (DotDecimal name prec scale))
      expected.
Proof.
  intros. apply tnull_leaf_dot_exact; exact H.
Qed.

Lemma tnull_dot_string_exact :
  forall basesort instance boolean_schedule env name typmod expected,
    Interp.interp_dot TNull env (AttrString name typmod) = expected ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Leaf TNull relname type_string (DotString name typmod))
      expected.
Proof.
  intros. apply tnull_leaf_dot_exact; exact H.
Qed.

Lemma tnull_dot_numeric_exact :
  forall basesort instance boolean_schedule env name expected,
    Interp.interp_dot TNull env (AttrNumeric name) = expected ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Leaf TNull relname type_numeric (DotNumeric name))
      expected.
Proof.
  intros. apply tnull_leaf_dot_exact; exact H.
Qed.

Lemma tnull_dot_int64_exact :
  forall basesort instance boolean_schedule env name expected,
    Interp.interp_dot TNull env (AttrInt64 name) = expected ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Leaf TNull relname type_int64 (DotInt64 name))
      expected.
Proof.
  intros. apply tnull_leaf_dot_exact; exact H.
Qed.

Lemma tnull_dot_int32_exact :
  forall basesort instance boolean_schedule env name expected,
    Interp.interp_dot TNull env (AttrInt32 name) = expected ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Leaf TNull relname type_int32 (DotInt32 name))
      expected.
Proof.
  intros. apply tnull_leaf_dot_exact; exact H.
Qed.

Lemma tnull_dot_date_exact :
  forall basesort instance boolean_schedule env name expected,
    Interp.interp_dot TNull env (AttrDate name) = expected ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Leaf TNull relname type_date (DotDate name))
      expected.
Proof.
  intros. apply tnull_leaf_dot_exact; exact H.
Qed.

Lemma tnull_one_minus_disc_exact :
  forall basesort instance boolean_schedule env disc_attr prec scale disc_n,
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Leaf TNull relname type_numeric (DotDecimal disc_attr prec scale))
      (NullValues.Value_numeric (Some disc_n)) ->
    numeric_runtime_fits_bool (numeric_sub (numeric_of_Z 1) disc_n) = true ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (numeric_one_minus_disc_expr disc_attr prec scale)
      (NullValues.Value_numeric
        (Some (numeric_sub (numeric_of_Z 1) disc_n))).
Proof.
  intros basesort instance boolean_schedule env disc_attr prec scale disc_n
    Hdisc Hfits.
  unfold numeric_one_minus_disc_expr.
  eapply tnull_numeric_sub_exact.
  - apply tnull_cast_one_to_numeric_exact.
  - exact Hdisc.
  - exact Hfits.
Qed.

Lemma tnull_one_plus_tax_exact :
  forall basesort instance boolean_schedule env tax_attr prec scale tax_n,
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Leaf TNull relname type_numeric (DotDecimal tax_attr prec scale))
      (NullValues.Value_numeric (Some tax_n)) ->
    numeric_runtime_fits_bool (numeric_add (numeric_of_Z 1) tax_n) = true ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (numeric_one_plus_tax_expr tax_attr prec scale)
      (NullValues.Value_numeric
        (Some (numeric_add (numeric_of_Z 1) tax_n))).
Proof.
  intros basesort instance boolean_schedule env tax_attr prec scale tax_n
    Htax Hfits.
  unfold numeric_one_plus_tax_expr.
  eapply tnull_numeric_add_exact.
  - apply tnull_cast_one_to_numeric_exact.
  - exact Htax.
  - exact Hfits.
Qed.

Lemma tnull_disc_price_exact :
  forall basesort instance boolean_schedule env
      price_attr disc_attr prec scale price_n disc_n,
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Leaf TNull relname type_numeric (DotDecimal price_attr prec scale))
      (NullValues.Value_numeric (Some price_n)) ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Leaf TNull relname type_numeric (DotDecimal disc_attr prec scale))
      (NullValues.Value_numeric (Some disc_n)) ->
    numeric_runtime_fits_bool (numeric_sub (numeric_of_Z 1) disc_n) = true ->
    numeric_runtime_fits_bool
      (numeric_mul price_n (numeric_sub (numeric_of_Z 1) disc_n)) = true ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (numeric_disc_price_expr price_attr disc_attr prec scale)
      (NullValues.Value_numeric
        (Some (numeric_mul price_n (numeric_sub (numeric_of_Z 1) disc_n)))).
Proof.
  intros basesort instance boolean_schedule env
    price_attr disc_attr prec scale price_n disc_n
    Hprice Hdisc Hfits_sub Hfits_mul.
  unfold numeric_disc_price_expr.
  eapply tnull_numeric_mul_exact.
  - exact Hprice.
  - eapply tnull_one_minus_disc_exact; eassumption.
  - exact Hfits_mul.
Qed.

Lemma tnull_charge_exact :
  forall basesort instance boolean_schedule env
      price_attr disc_attr tax_attr prec scale price_n disc_n tax_n,
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Leaf TNull relname type_numeric (DotDecimal price_attr prec scale))
      (NullValues.Value_numeric (Some price_n)) ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Leaf TNull relname type_numeric (DotDecimal disc_attr prec scale))
      (NullValues.Value_numeric (Some disc_n)) ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Leaf TNull relname type_numeric (DotDecimal tax_attr prec scale))
      (NullValues.Value_numeric (Some tax_n)) ->
    numeric_runtime_fits_bool (numeric_sub (numeric_of_Z 1) disc_n) = true ->
    numeric_runtime_fits_bool (numeric_add (numeric_of_Z 1) tax_n) = true ->
    numeric_runtime_fits_bool
      (numeric_mul price_n (numeric_sub (numeric_of_Z 1) disc_n)) = true ->
    numeric_runtime_fits_bool
      (numeric_mul
        (numeric_mul price_n (numeric_sub (numeric_of_Z 1) disc_n))
        (numeric_add (numeric_of_Z 1) tax_n)) = true ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (numeric_charge_expr price_attr disc_attr tax_attr prec scale)
      (NullValues.Value_numeric
        (Some (numeric_mul
          (numeric_mul price_n (numeric_sub (numeric_of_Z 1) disc_n))
          (numeric_add (numeric_of_Z 1) tax_n)))).
Proof.
  intros basesort instance boolean_schedule env
    price_attr disc_attr tax_attr prec scale price_n disc_n tax_n
    Hprice Hdisc Htax Hfits_sub Hfits_add Hfits_f4 Hfits_f5.
  unfold numeric_charge_expr.
  eapply tnull_numeric_mul_exact.
  - eapply tnull_disc_price_exact; eassumption.
  - eapply tnull_one_plus_tax_exact; eassumption.
  - exact Hfits_f5.
Qed.
