(* SqlTypes — generic SQL types, values, and three-valued truth.
   Query-independent. No Pack / physical keys / Q1 schema. *)

From Stdlib Require Import List ZArith String Bool Nat.
Import ListNotations.
Open Scope Z_scope.
Open Scope string_scope.

(** * Types *)

Inductive SqlType : Type :=
| TInt
| TNumeric (scale : nat)   (* fixed-point; scale in decimal digits *)
| TBool
| TDate                    (* days since epoch as Z *)
| TText
| TTyUnknown.

Definition sql_type_eqb (a b : SqlType) : bool :=
  match a, b with
  | TInt, TInt | TBool, TBool | TDate, TDate | TText, TText | TTyUnknown, TTyUnknown => true
  | TNumeric s1, TNumeric s2 => Nat.eqb s1 s2
  | _, _ => false
  end.

(** * Values (NULL + exact AVG carrier for Spec integer fold) *)

Inductive SqlValue : Type :=
| VNull
| VInt (z : Z)
| VBool (b : bool)
| VText (s : string)
(** Exact sum/count pair produced by AVG aggregation; observation bridges
    may round. Not a surface SQL literal. *)
| VAvgExact (sum count : Z).

Definition is_null (v : SqlValue) : bool :=
  match v with VNull => true | _ => false end.

(** * Three-valued truth *)

Inductive Truth3 : Type :=
| TTrue | TFalse | TUnknown.

Definition truth3_and (a b : Truth3) : Truth3 :=
  match a, b with
  | TFalse, _ | _, TFalse => TFalse
  | TUnknown, _ | _, TUnknown => TUnknown
  | TTrue, TTrue => TTrue
  end.

Definition truth3_or (a b : Truth3) : Truth3 :=
  match a, b with
  | TTrue, _ | _, TTrue => TTrue
  | TUnknown, _ | _, TUnknown => TUnknown
  | TFalse, TFalse => TFalse
  end.

Definition truth3_not (a : Truth3) : Truth3 :=
  match a with
  | TTrue => TFalse
  | TFalse => TTrue
  | TUnknown => TUnknown
  end.

Definition truth3_of_bool (b : bool) : Truth3 :=
  if b then TTrue else TFalse.

(** SQL WHERE keeps only TTrue (not Unknown). *)
Definition truth3_is_true (t : Truth3) : bool :=
  match t with TTrue => true | _ => false end.

(** Integer compare with NULL → Unknown. *)
Definition cmp_int_leb (a b : SqlValue) : Truth3 :=
  match a, b with
  | VNull, _ | _, VNull => TUnknown
  | VInt x, VInt y => truth3_of_bool (Z.leb x y)
  | _, _ => TUnknown
  end.

Definition cmp_int_eq (a b : SqlValue) : Truth3 :=
  match a, b with
  | VNull, _ | _, VNull => TUnknown
  | VInt x, VInt y => truth3_of_bool (Z.eqb x y)
  | VBool x, VBool y => truth3_of_bool (Bool.eqb x y)
  | VText x, VText y => truth3_of_bool (String.eqb x y)
  | _, _ => TUnknown
  end.

Definition sql_value_eqb (a b : SqlValue) : bool :=
  match a, b with
  | VNull, VNull => true
  | VInt x, VInt y => Z.eqb x y
  | VBool x, VBool y => Bool.eqb x y
  | VText x, VText y => String.eqb x y
  | VAvgExact s1 c1, VAvgExact s2 c2 => andb (Z.eqb s1 s2) (Z.eqb c1 c2)
  | _, _ => false
  end.

Fixpoint list_sql_value_eqb (a b : list SqlValue) : bool :=
  match a, b with
  | [], [] => true
  | x :: xs, y :: ys => andb (sql_value_eqb x y) (list_sql_value_eqb xs ys)
  | _, _ => false
  end.
