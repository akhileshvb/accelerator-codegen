(* ValueCorrespond — FormalSQL NullValues.value ↔ RelNode SqlValue.
   Shared LogicalSpec proof library. Query-independent.
   Supported fragment only; unsupported FormalSQL value forms have no
   constructors (fail closed). *)

From Stdlib Require Import List ZArith String Bool.
Import ListNotations.
Open Scope Z_scope.
Open Scope string_scope.

From SQLFS Require Import
  Values ValueCore ValueNumeric ValueInteger ValueString GenericInstance.
From FormalSQLLogical Require Import SqlTypes.

Import NullValues.

(** * Type bridge: FormalSQL attribute → RelNode SqlType (fail closed) *)

Definition formal_attr_name (a : attribute) : string :=
  match a with
  | Attr_string s _
  | Attr_Z s
  | Attr_int32 s
  | Attr_int64 s
  | Attr_bool s
  | Attr_float s
  | Attr_double s
  | Attr_numeric s
  | Attr_decimal s _ _
  | Attr_date s
  | Attr_time s
  | Attr_timestamp s _
  | Attr_timestamptz s _ => s
  end.

(** Only types admitted in the current RelNode verified fragment.
    [Attr_numeric] (no scale), floats, times, timestamps → None. *)
Definition formal_attr_sqltype (a : attribute) : option SqlType :=
  match a with
  | Attr_int32 _ | Attr_int64 _ | Attr_Z _ => Some TInt
  | Attr_bool _ => Some TBool
  | Attr_date _ => Some TDate
  | Attr_string _ _ => Some TText
  | Attr_decimal _ _ sc =>
      if Z.leb 0 sc then Some (TNumeric (Z.to_nat sc)) else None
  | Attr_numeric _
  | Attr_float _ | Attr_double _
  | Attr_time _ | Attr_timestamp _ _ | Attr_timestamptz _ _ => None
  end.

(** * Value correspondence (schema-directed) *)

(** [value_correspond ty fv rv] — FormalSQL value [fv] corresponds to RelNode
    value [rv] at RelNode type [ty].

    Integers: int32/int64/Z carriers ↔ [VInt].
    Numeric: scaled coefficient embedding (same convention as Q1 Embed /
    [numeric_of_scaled]); RelNode stores the coefficient as [VInt] with scale
    in the schema [TNumeric scale].
    Date: day-epoch [Z] ↔ [VInt] at [TDate] (RelNode date carrier).
    Text / bool / NULL: direct.
    [VAvgExact] is RelNode-only (aggregate carrier) — not a FormalSQL table
    value; no constructor here. *)
Inductive value_correspond : SqlType -> value -> SqlValue -> Prop :=
| VC_null_int32 :
    value_correspond TInt (Value_int32 None) VNull
| VC_null_int64 :
    value_correspond TInt (Value_int64 None) VNull
| VC_null_Z :
    value_correspond TInt (Value_Z None) VNull
| VC_null_bool :
    value_correspond TBool (Value_bool None) VNull
| VC_null_date :
    value_correspond TDate (Value_date None) VNull
| VC_null_text :
    forall tm,
      value_correspond TText (Value_string (StringValue tm None)) VNull
| VC_null_numeric :
    forall scale,
      value_correspond (TNumeric scale) (Value_numeric None) VNull
| VC_int32 :
    forall i,
      value_correspond TInt (Value_int32 (Some i)) (VInt (int32_value i))
| VC_int64 :
    forall i,
      value_correspond TInt (Value_int64 (Some i)) (VInt (int64_value i))
| VC_Z :
    forall z,
      value_correspond TInt (Value_Z (Some z)) (VInt z)
| VC_bool :
    forall b,
      value_correspond TBool (Value_bool (Some b)) (VBool b)
| VC_date :
    forall d,
      value_correspond TDate (Value_date (Some d)) (VInt d)
| VC_text :
    forall tm s,
      value_correspond TText (Value_string (StringValue tm (Some s))) (VText s)
| VC_numeric_scaled :
    forall scale coeff,
      value_correspond (TNumeric scale)
        (Value_numeric (Some (numeric_of_scaled coeff (Z.of_nat scale))))
        (VInt coeff).

(** Fail-closed: no constructors for float/double/time/timestamp, mismatched
    type tags, or RelNode [VAvgExact]. *)

Lemma value_correspond_null_is_null :
  forall ty fv,
    value_correspond ty fv VNull ->
    is_null_value fv = true.
Proof.
  intros ty fv H; inversion H; reflexivity.
Qed.

Lemma value_correspond_int32_inject :
  forall i i',
    value_correspond TInt (Value_int32 (Some i)) (VInt (int32_value i')) ->
    int32_value i = int32_value i'.
Proof.
  intros i i' H; inversion H; subst; reflexivity.
Qed.

(** * Functional inverse on non-numeric slots

    [value_correspond] is functional in its RelNode side for every
    non-numeric type: the RelNode value is determined by the FormalSQL value.
    (Numeric would additionally need injectivity of [numeric_of_scaled].) *)

Definition rel_of_value (fv : value) : SqlValue :=
  match fv with
  | Value_int32 (Some i) => VInt (int32_value i)
  | Value_int64 (Some i) => VInt (int64_value i)
  | Value_Z (Some z) => VInt z
  | Value_bool (Some b) => VBool b
  | Value_date (Some d) => VInt d
  | Value_string (_, Some s) => VText s
  | _ => VNull
  end.

Lemma value_correspond_rel_of_value :
  forall ty fv v,
    value_correspond ty fv v ->
    (forall scale, ty <> TNumeric scale) ->
    v = rel_of_value fv.
Proof.
  intros ty fv v H Hty; destruct H; try reflexivity.
  exfalso; eapply Hty; reflexivity.
Qed.
