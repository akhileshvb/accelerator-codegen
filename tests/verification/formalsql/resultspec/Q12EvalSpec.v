(* Q12EvalSpec — result-only EvalSpec for QueryResultSpec_Q12.
   Mirrors Dafny Q12ResultSpec (join lookup + CASE high/low + sort).
   No accelerator Inv / day_partitions in the theorem statement. *)

From Stdlib Require Import List ZArith String Lia Bool.
Import ListNotations.
Open Scope Z_scope.
Open Scope string_scope.

From FormalSQLResultSpec Require Import QueryResultSpecQ12.

Module Q12EvalSpec.

Inductive Priority : Type := High | Low.

Record OrderRow : Type := mkOrderRow {
  o_orderkey : Z;
  o_priority : string
}.

Record LineRow : Type := mkLineRow {
  l_orderkey : Z;
  l_shipmode : string;
  l_commitdate : Z;
  l_receiptdate : Z;
  l_shipdate : Z
}.

Record SpecStats : Type := mkSpecStats {
  high : Z;
  low : Z
}.

Record SpecOutRow : Type := mkSpecOutRow {
  out_shipmode : string;
  out_high : Z;
  out_low : Z
}.

Definition EmptyStats : SpecStats := mkSpecStats 0 0.

Definition ClassifyPriority (p : string) : Priority :=
  if String.eqb p "1-URGENT" then High
  else if String.eqb p "2-HIGH" then High
  else Low.

Definition LineQualifies (l : LineRow) : bool :=
  (l_commitdate l <? l_receiptdate l)%Z && (l_shipdate l <? l_commitdate l)%Z.

Definition ModeAccepted (mode m1 m2 : string) : bool :=
  String.eqb mode m1 || String.eqb mode m2.

Definition InWindow (day start endExclusive : Z) : bool :=
  (start <=? day)%Z && (day <? endExclusive)%Z.

Definition SpecApply (s : SpecStats) (p : Priority) (sign : Z) : SpecStats :=
  match p with
  | High => mkSpecStats (high s + sign) (low s)
  | Low => mkSpecStats (high s) (low s + sign)
  end.

Fixpoint PriUpsert (pri : list (Z * Priority)) (k : Z) (p : Priority) : list (Z * Priority) :=
  match pri with
  | nil => [(k, p)]
  | (k', p') :: rest =>
      if Z.eqb k k' then (k, p) :: rest
      else (k', p') :: PriUpsert rest k p
  end.

Definition PriorityMap (orders : list OrderRow) : list (Z * Priority) :=
  fold_left
    (fun pri o => PriUpsert pri (o_orderkey o) (ClassifyPriority (o_priority o)))
    orders nil.

Fixpoint PriLookup (pri : list (Z * Priority)) (k : Z) : option Priority :=
  match pri with
  | nil => None
  | (k', p) :: rest => if Z.eqb k k' then Some p else PriLookup rest k
  end.

Fixpoint GetMode (m : list (string * SpecStats)) (mode : string) : SpecStats :=
  match m with
  | nil => EmptyStats
  | (mode', s) :: rest =>
      if String.eqb mode mode' then s else GetMode rest mode
  end.

Fixpoint SetMode (m : list (string * SpecStats)) (mode : string) (s : SpecStats)
  : list (string * SpecStats) :=
  match m with
  | nil => [(mode, s)]
  | (mode', s') :: rest =>
      if String.eqb mode mode' then (mode, s) :: rest
      else (mode', s') :: SetMode rest mode s
  end.

Definition ApplyLine
    (m : list (string * SpecStats))
    (pri : list (Z * Priority))
    (l : LineRow)
    (start endExclusive : Z)
    (m1 m2 : string)
    (sign : Z) : list (string * SpecStats) :=
  if LineQualifies l
     && InWindow (l_receiptdate l) start endExclusive
     && ModeAccepted (l_shipmode l) m1 m2
  then
    match PriLookup pri (l_orderkey l) with
    | None => m
    | Some p =>
        let cur := GetMode m (l_shipmode l) in
        SetMode m (l_shipmode l) (SpecApply cur p sign)
    end
  else m.

Definition FoldLines
    (lines : list LineRow)
    (pri : list (Z * Priority))
    (start endExclusive : Z)
    (m1 m2 : string) : list (string * SpecStats) :=
  fold_left
    (fun m l => ApplyLine m pri l start endExclusive m1 m2 1)
    lines nil.

Definition SpecView
    (orders : list OrderRow) (lines : list LineRow)
    (start endExclusive : Z) (m1 m2 : string)
  : list (string * SpecStats) :=
  FoldLines lines (PriorityMap orders) start endExclusive m1 m2.

Definition SpecFinalizeOne (mode : string) (s : SpecStats) : list SpecOutRow :=
  if (high s =? 0)%Z && (low s =? 0)%Z then nil
  else [mkSpecOutRow mode (high s) (low s)].

Fixpoint InsertSortedStr (rows : list SpecOutRow) (row : SpecOutRow) : list SpecOutRow :=
  match rows with
  | nil => [row]
  | r :: rest =>
      match String.compare (out_shipmode row) (out_shipmode r) with
      | Lt => row :: rows
      | _ => r :: InsertSortedStr rest row
      end
  end.

Fixpoint SortByMode (rows : list SpecOutRow) : list SpecOutRow :=
  match rows with
  | nil => nil
  | r :: rest => InsertSortedStr (SortByMode rest) r
  end.

Definition CollectModes (m1 m2 : string) (m : list (string * SpecStats))
  : list SpecOutRow :=
  let r1 := SpecFinalizeOne m1 (GetMode m m1) in
  if String.eqb m1 m2 then r1
  else r1 ++ SpecFinalizeOne m2 (GetMode m m2).

(** Catalog window end: DATE '1995-01-01' + 12 calendar months.
    Matches accelerator TpchQ12AddMonths(9131, 12) = 9496 (1996-01-01). *)
Definition q12_catalog_end : Z := 9496.

Definition EvalSpec_Q12 (s : QueryResultSpec_Q12)
    (orders : list OrderRow) (lines : list LineRow) : list SpecOutRow :=
  SortByMode
    (CollectModes (qrs_shipmode1 s) (qrs_shipmode2 s)
       (SpecView orders lines (qrs_start s) q12_catalog_end
                 (qrs_shipmode1 s) (qrs_shipmode2 s))).

Lemma EvalSpec_S_Q12_catalog :
  forall orders lines,
    EvalSpec_Q12 S_Q12_catalog orders lines =
    SortByMode
      (CollectModes "MAIL" "SHIP"
         (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")).
Proof. reflexivity. Qed.

Lemma EvalSpec_Q12_nil :
  EvalSpec_Q12 S_Q12_catalog nil nil = nil.
Proof. reflexivity. Qed.

End Q12EvalSpec.
