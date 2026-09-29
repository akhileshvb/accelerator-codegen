(* Q1AuxSpec -- Rocq-private group model used INSIDE the Q1 Stage 2 proof.

   Formerly dual-emitted to Rocq and Dafny by auxspec/gen_auxspec.py as an
   intermediate specification layer.  That layer is gone: the public Q1
   theorems (q1/Q1FinalTheorem.v) and the Dafny side (q1/dafny/Q1ExecExact.dfy)
   both target the RelNode Eval_Spec directly.  This file is now an ordinary,
   fully proved Rocq definition with no Dafny twin and no trusted role; it
   appears in no public theorem statement.  (It stays under generated/ only to
   keep the JetStreamFS load path of the existing proofs.) *)

Require Import List ZArith Bool.
Import ListNotations.
Open Scope Z_scope.

Module Q1AuxSpec.

Record Stats : Type := mkStats {
  count : Z;
  sum_qty : Z;
  sum_base_price : Z;
  sum_disc_price : Z;
  sum_charge : Z;
  sum_discount : Z
}.

Definition EmptyStats : Stats :=
  mkStats 0 0 0 0 0 0.

Record Row : Type := mkRow {
  shipdate : Z;
  qty : Z;
  price : Z;
  disc : Z;
  tax : Z;
  rf : Z;
  ls : Z
}.

Definition Pack (rf ls : Z) : Z := rf * 256 + ls.

Definition RowKey (r : Row) : Z := Pack (rf r) (ls r).

(** SpecApply — contribution of one row with sign (+1 insert / -1 delete). *)
Definition SpecApply (s : Stats) (r : Row) (sign : Z) : Stats :=
  let disc_price := price r * (100 - disc r) in
  mkStats
    (count s + sign)
    (sum_qty s + sign * qty r)
    (sum_base_price s + sign * price r)
    (sum_disc_price s + sign * disc_price)
    (sum_charge s + sign * disc_price * (100 + tax r))
    (sum_discount s + sign * disc r).

Definition SpecMerge (a b : Stats) : Stats :=
  mkStats
    (count a + count b)
    (sum_qty a + sum_qty b)
    (sum_base_price a + sum_base_price b)
    (sum_disc_price a + sum_disc_price b)
    (sum_charge a + sum_charge b)
    (sum_discount a + sum_discount b).

(** Day×group abstract store as association list (finite map surrogate). *)
Definition CellKey : Type := (Z * Z)%type.
Definition Store : Type := list (CellKey * Stats).

Definition cellkey_eqb (a b : CellKey) : bool :=
  Z.eqb (fst a) (fst b) && Z.eqb (snd a) (snd b).

Fixpoint store_get (st : Store) (k : CellKey) : Stats :=
  match st with
  | [] => EmptyStats
  | (k', s) :: rest => if cellkey_eqb k k'
                       then s else store_get rest k
  end.

(** Map-faithful list update: drop any prior binding of [k], then write [s]
    (or omit when count is zero). Matches Dafny [Store] map overwrite/erase. *)
Definition store_remove (st : Store) (k : CellKey) : Store :=
  filter (fun '(k', _) => negb (cellkey_eqb k' k)) st.

Definition SpecApplyDelta (st : Store) (r : Row) (sign : Z) : Store :=
  let k := (shipdate r, RowKey r) in
  let s2 := SpecApply (store_get st k) r sign in
  if Z.eqb (count s2) 0 then store_remove st k
  else (k, s2) :: store_remove st k.

Fixpoint FoldSpec (db : list Row) : Store :=
  match db with
  | [] => []
  | r :: rest => SpecApplyDelta (FoldSpec rest) r 1
  end.

(** Merge stats for one gkey into an association list. *)
Fixpoint keystore_put (acc : list (Z * Stats)) (gkey : Z) (s : Stats)
  : list (Z * Stats) :=
  match acc with
  | [] => [(gkey, s)]
  | (k, s0) :: rest =>
      if Z.eqb k gkey then (k, SpecMerge s0 s) :: rest
      else (k, s0) :: keystore_put rest gkey s
  end.

(** ViewSpec: keep cells with day <= cutoff; merge by gkey. *)
Fixpoint SpecView (st : Store) (cutoff : Z) : list (Z * Stats) :=
  match st with
  | [] => []
  | ((day, gkey), s) :: rest =>
      let acc := SpecView rest cutoff in
      if Z.leb day cutoff then keystore_put acc gkey s else acc
  end.

Record OutRow : Type := mkOutRow {
  out_rf : Z;
  out_ls : Z;
  out_sum_qty : Z;
  out_sum_base : Z;
  out_sum_disc_price : Z;
  out_sum_charge : Z;
  out_sum_discount : Z;
  out_avg_qty : Z;
  out_avg_price : Z;
  out_avg_disc : Z;
  out_count : Z
}.

Definition Unpack (gkey : Z) : (Z * Z) :=
  (gkey / 256, Z.modulo gkey 256).

(** PostgreSQL NUMERIC AVG rounding: ties away from zero (FormalSQL
    [numeric_round_quot]). Applied to scale-2 integer embeddings for Twin
    correspondence; EmbedResult uses FormalSQL [numeric_div_at_scales] for
    the FormalSQL observation (data-dependent display scale). *)
Definition AvgRound (numerator denominator : Z) : Z :=
  let quotient := Z.quot numerator denominator in
  let remainder := Z.rem numerator denominator in
  if (2 * Z.abs remainder >=? Z.abs denominator)%Z
  then quotient + (if (numerator * denominator <? 0)%Z then (-1) else 1)
  else quotient.

Definition SpecFinalizeOne (gkey : Z) (s : Stats) : list OutRow :=
  if Z.leb (count s) 0 then []
  else
    let '(rf, ls) := Unpack gkey in
    [mkOutRow rf ls
       (sum_qty s) (sum_base_price s) (sum_disc_price s) (sum_charge s)
       (sum_discount s)
       (AvgRound (sum_qty s) (count s))
       (AvgRound (sum_base_price s) (count s))
       (AvgRound (sum_discount s) (count s))
       (count s)].

Fixpoint SpecFinalize (entries : list (Z * Stats)) : list OutRow :=
  match entries with
  | [] => []
  | (gkey, s) :: rest => SpecFinalizeOne gkey s ++ SpecFinalize rest
  end.

(** ORDER BY (l_returnflag, l_linestatus) ascending.  FormalSQL lowers the Q1
    [ORDER BY] to [SortAscNullsLast l_returnflag; SortAscNullsLast l_linestatus]
    on the single-char output columns; ascending string order on one-char codes
    is ascending order on the [Z] ASCII codes [out_rf]/[out_ls].  We finalize
    with an insertion sort by [(out_rf, out_ls)] so [AuxAnswerSpec] matches the
    unique sorted FormalSQL observation (group keys are distinct). *)
Definition OutRowLe (a b : OutRow) : bool :=
  if Z.eqb (out_rf a) (out_rf b)
  then Z.leb (out_ls a) (out_ls b)
  else Z.leb (out_rf a) (out_rf b).

Fixpoint OutRowInsert (x : OutRow) (l : list OutRow) : list OutRow :=
  match l with
  | [] => [x]
  | y :: ys => if OutRowLe x y then x :: y :: ys else y :: OutRowInsert x ys
  end.

Fixpoint OutRowSort (l : list OutRow) : list OutRow :=
  match l with
  | [] => []
  | x :: xs => OutRowInsert x (OutRowSort xs)
  end.

Definition AuxAnswerSpec (db : list Row) (cutoff : Z) : list OutRow :=
  OutRowSort (SpecFinalize (SpecView (FoldSpec db) cutoff)).

End Q1AuxSpec.
