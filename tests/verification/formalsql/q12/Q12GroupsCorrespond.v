(* Q12GroupsCorrespond — FormalSQL group rows ↔ CollectModes(SpecView).

   Q1 twin of Q1GroupsCorrespond / q1_group_rows_perm_SpecFinalize_embed.
   After Permutation + tnull_sort_ordered_rows_eq_of_perm, discharges bag-sort;
   of_holds composes FormalSQL_exec under Pre + domain/priority/sum_bound. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueInteger
  ValueTemporal ValueString FiniteSet FTuples OrderedSet FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts Interp ATerms FTerms Formula Projection SqlListFacts
  FiniteBag FiniteCollection.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax
  FormalSQL.GroupedFilterOutcomeFacts FormalSQL.QueryCardinality
  FormalSQL.AggregateRuntimeFacts FormalSQL.GroupingRewriteFacts
  FormalSQL.NumericFacts.
From FormalSQLLib Require Import
  SortedGroupResultFacade KeyedSortFacade ListNoDupFacade
  AggregateLeafExactFacade NumericCallExactFacade
  TemporalPredAcceptanceFacade.
From FormalSQLResultSpec Require Import
  QueryResultSpecQ12 Q12EvalSpec Q12SemanticBridge NormalizeQueryQ12.
From LogosGenerated Require Import Schema Queries.
From Stdlib Require Import String Ascii List ZArith Bool Lia Permutation.
Import ListNotations.
Import Tuple.
Open Scope string_scope.
Open Scope Z_scope.

Require Import Q12Catalog.
Require Import Q12NormalizeSound.
Require Import Q12ResultSpecNormalize.
Require Import Q12EmbedDots.
Require Import Q12WhereAcceptance.
Require Import Q12CaseAcceptance.
Require Import Q12GroupAcceptance.
Require Import Q12JoinSpine.
Require Import Q12IdentityProject.

Import Q12EvalSpec.

(** * Group-row observation of CollectModes SpecOut *)

Definition q12_group_attrs : list (attribute TNull) :=
  AttrString "l_shipmode" (StringChar 10)
  :: AttrInt64 "high_line_count"
  :: AttrInt64 "low_line_count"
  :: nil.

Definition q12_group_values_of_SpecOutRow (o : SpecOutRow)
  : list NullValues.value :=
  [ embed_string_char10 (out_shipmode o)
  ; Value_int64 (Some (int64_from_twos_complement (out_high o)))
  ; Value_int64 (Some (int64_from_twos_complement (out_low o)))
  ].

Definition q12_group_tuple_of_SpecOutRow (o : SpecOutRow) : tuple TNull :=
  project_row Queries.scalar_select_list_1
    (q12_group_values_of_SpecOutRow o).

(** FormalSQL group rows from CASE-projected WHERE rows. *)
Definition q12_group_rows_of (orders : list OrderRow) (lines : list LineRow)
  : list (tuple TNull) :=
  map (fun group =>
    project_row Queries.scalar_select_list_1
      (q12_group_select_values group))
    (query_make_groups nil
      (q12_pre_group_rows orders lines) q12_group_terms).

Definition q12_ordered_rows_of (orders : list OrderRow) (lines : list LineRow)
  : list (tuple TNull) :=
  @sort_ordered_rows TNull q12_value_is_null q12_order_keys
    (q12_group_rows_of orders lines).

Definition q12_final_rows_of (orders : list OrderRow) (lines : list LineRow)
  : list (tuple TNull) :=
  map (fun row =>
    project_row Queries.scalar_select_list_0 (q12_outer_dot_values row))
    (q12_ordered_rows_of orders lines).

(** * Dot recovery on SpecOut group tuples *)

Lemma q12_group_tuple_dot_shipmode :
  forall o,
    dot TNull (q12_group_tuple_of_SpecOutRow o) q12_attr_shipmode =
    embed_string_char10 (out_shipmode o).
Proof.
  intro o.
  unfold q12_group_tuple_of_SpecOutRow, q12_group_values_of_SpecOutRow,
    q12_attr_shipmode, project_row, scalar_select_outputs,
    Queries.scalar_select_list_1.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q12_group_tuple_dot_high :
  forall o,
    dot TNull (q12_group_tuple_of_SpecOutRow o) (AttrInt64 "high_line_count") =
    Value_int64 (Some (int64_from_twos_complement (out_high o))).
Proof.
  intro o.
  unfold q12_group_tuple_of_SpecOutRow, q12_group_values_of_SpecOutRow,
    project_row, scalar_select_outputs, Queries.scalar_select_list_1.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q12_group_tuple_dot_low :
  forall o,
    dot TNull (q12_group_tuple_of_SpecOutRow o) (AttrInt64 "low_line_count") =
    Value_int64 (Some (int64_from_twos_complement (out_low o))).
Proof.
  intro o.
  unfold q12_group_tuple_of_SpecOutRow, q12_group_values_of_SpecOutRow,
    project_row, scalar_select_outputs, Queries.scalar_select_list_1.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q12_group_tuple_outer_labels_ok :
  forall o, q12_outer_row_labels_ok (q12_group_tuple_of_SpecOutRow o).
Proof.
  intro o.
  unfold q12_outer_row_labels_ok.
  unfold q12_group_tuple_of_SpecOutRow.
  repeat split;
    (apply q12_attr_inS_project_row;
     unfold scalar_select_outputs, Queries.scalar_select_list_1;
     vm_compute; reflexivity).
Qed.

Lemma q12_outer_dot_values_of_group_tuple :
  forall o,
    q12_outer_dot_values (q12_group_tuple_of_SpecOutRow o) =
    [ embed_string_char10 (out_shipmode o)
    ; Value_int64 (Some (int64_from_twos_complement (out_high o)))
    ; Value_int64 (Some (int64_from_twos_complement (out_low o)))
    ].
Proof.
  intro o.
  unfold q12_outer_dot_values.
  rewrite q12_group_tuple_dot_shipmode, q12_group_tuple_dot_high,
    q12_group_tuple_dot_low.
  reflexivity.
Qed.

(** Outer project of SpecOut group-tuple = EmbedOutRow. *)
Lemma q12_outer_of_group_tuple_eq_EmbedOutRow :
  forall o,
    project_row Queries.scalar_select_list_0
      (q12_outer_dot_values (q12_group_tuple_of_SpecOutRow o)) =
    EmbedOutRow_Q12 o.
Proof.
  intro o.
  rewrite q12_outer_dot_values_of_group_tuple.
  unfold EmbedOutRow_Q12, q12_group_values_of_SpecOutRow.
  reflexivity.
Qed.

(** * Intended perm (stated; proof continues below / via bag technique) *)

Definition q12_group_rows_perm_CollectModes_embed_stmt
    (orders : list OrderRow) (lines : list LineRow) : Prop :=
  Permutation
    (q12_group_rows_of orders lines)
    (map q12_group_tuple_of_SpecOutRow
      (Q12_spec_group_outs orders lines)).


(** * SUM of 0/1 case bits = Int64 of true-count *)

Lemma q12_case_bit_true_value :
  exists i, int32_checked 1 = Some i /\ int32_value i = 1.
Proof.
  unfold int32_checked.
  destruct (Z.leb_spec0 int32_min 1) as [Hlo|]; [|exfalso; unfold int32_min in *; lia].
  destruct (Z.leb_spec0 1 int32_max) as [Hhi|]; [|exfalso; unfold int32_max in *; lia].
  exists (Int32 1 (conj Hlo Hhi)); split; reflexivity.
Qed.

Lemma q12_case_bit_false_value :
  exists i, int32_checked 0 = Some i /\ int32_value i = 0.
Proof.
  unfold int32_checked.
  destruct (Z.leb_spec0 int32_min 0) as [Hlo|]; [|exfalso; unfold int32_min in *; lia].
  destruct (Z.leb_spec0 0 int32_max) as [Hhi|]; [|exfalso; unfold int32_max in *; lia].
  exists (Int32 0 (conj Hlo Hhi)); split; reflexivity.
Qed.

Lemma fold_left_case_bits_count_from :
  forall bits acc,
    fold_left (fun a n => a + int32_value n)
      (NullValues.int32_values (map q12_case_bit bits)) acc =
    acc + Z.of_nat (List.length (List.filter (fun b : bool => b) bits)).
Proof.
  induction bits as [|b bs IH]; intros acc; [cbn; lia|].
  destruct b.
  - (* true *)
    pose proof (int32_checked_0_1_some true) as [i [Hi Hval]].
    simpl map.
    replace (q12_case_bit true) with (NullValues.Value_int32 (Some i))
      by (unfold q12_case_bit; rewrite Hi; reflexivity).
    simpl NullValues.int32_values.
    simpl fold_left.
    destruct Hval as [Hz|Ho].
    + unfold q12_case_bit_z, int32_checked in Hi.
      destruct (Z.leb_spec0 int32_min 1); [|exfalso; unfold int32_min in *; lia].
      destruct (Z.leb_spec0 1 int32_max); [|exfalso; unfold int32_max in *; lia].
      inversion Hi; subst i; simpl in Hz; lia.
    + simpl filter; simpl List.length.
      rewrite Ho.
      rewrite (IH (acc + 1)); lia.
  - pose proof (int32_checked_0_1_some false) as [i [Hi Hval]].
    simpl map.
    replace (q12_case_bit false) with (NullValues.Value_int32 (Some i))
      by (unfold q12_case_bit; rewrite Hi; reflexivity).
    simpl NullValues.int32_values.
    simpl fold_left.
    destruct Hval as [Hz|Ho].
    + simpl filter; simpl List.length.
      rewrite Hz.
      rewrite (IH (acc + 0)); lia.
    + unfold q12_case_bit_z, int32_checked in Hi.
      destruct (Z.leb_spec0 int32_min 0); [|exfalso; unfold int32_min in *; lia].
      destruct (Z.leb_spec0 0 int32_max); [|exfalso; unfold int32_max in *; lia].
      inversion Hi; subst i; simpl in Ho; lia.
Qed.

Lemma fold_left_case_bits_count :
  forall bits,
    fold_left (fun acc next => acc + int32_value next)
      (NullValues.int32_values (map q12_case_bit bits)) 0 =
    Z.of_nat (List.length (List.filter (fun b : bool => b) bits)).
Proof.
  intro bits; rewrite (fold_left_case_bits_count_from bits 0); lia.
Qed.

Lemma interp_sum_int32_case_bits_eq_count :
  forall bits,
    bits <> nil ->
    Z.of_nat (List.length bits) <= int64_max ->
    NullValues.interp_sum_int32_as_int64 (map q12_case_bit bits) =
    NullValues.value_int64_checked
      (Z.of_nat (List.length (List.filter (fun b : bool => b) bits))).
Proof.
  intros bits Hne Hlen.
  unfold NullValues.interp_sum_int32_as_int64.
  assert (Hall : forallb NullValues.is_int32_value (map q12_case_bit bits) = true).
  {
    clear Hne Hlen; induction bits as [|b bs IH]; [reflexivity|].
    cbn [map forallb]; rewrite (q12_case_bit_is_int32 b); exact IH.
  }
  rewrite Hall.
  destruct bits as [|b bs]; [contradiction Hne; reflexivity|].
  pose proof (int32_checked_0_1_some b) as [i [Hi _]].
  (* Force the match on int32_values to the cons branch *)
  assert (Hvals :
    NullValues.int32_values (map q12_case_bit (b :: bs)) =
    i :: NullValues.int32_values (map q12_case_bit bs)).
  {
    simpl map.
    replace (q12_case_bit b) with (NullValues.Value_int32 (Some i))
      by (unfold q12_case_bit; rewrite Hi; reflexivity).
    reflexivity.
  }
  rewrite Hvals.
  rewrite <- Hvals.
  rewrite fold_left_case_bits_count.
  reflexivity.
Qed.

Lemma value_int64_checked_of_range :
  forall z,
    int64_min <= z <= int64_max ->
    NullValues.value_int64_checked z =
    Value_int64 (Some (int64_from_twos_complement z)).
Proof.
  intros z Hz.
  unfold NullValues.value_int64_checked, int64_checked.
  destruct (Z.leb_spec0 int64_min z) as [Hlo|]; [|exfalso; lia].
  destruct (Z.leb_spec0 z int64_max) as [Hhi|]; [|exfalso; lia].
  assert (Heq : {| int64_value := z; int64_range := conj Hlo Hhi |} =
                 int64_from_twos_complement z).
  {
    apply int64_ext; cbn [int64_value].
    unfold int64_from_twos_complement; cbn [int64_value].
    unfold int64_modulus, int64_min, int64_max in Hz |- *.
    rewrite Z.mod_small; lia.
  }
  rewrite Heq; reflexivity.
Qed.


(** * SpecView GetMode step lemmas *)

Lemma GetMode_SetMode_same :
  forall m mode s,
    GetMode (SetMode m mode s) mode = s.
Proof.
  induction m as [|[mode' s'] rest IH]; intros mode s; cbn.
  - rewrite String.eqb_refl; reflexivity.
  - destruct (String.eqb mode mode') eqn:E.
    + apply String.eqb_eq in E; subst; cbn; rewrite String.eqb_refl; reflexivity.
    + cbn; rewrite E; apply IH.
Qed.

Lemma GetMode_SetMode_other :
  forall m mode s mode2,
    mode <> mode2 ->
    GetMode (SetMode m mode s) mode2 = GetMode m mode2.
Proof.
  induction m as [|[mode' s'] rest IH]; intros mode s mode2 Hne; cbn.
  - destruct (String.eqb mode2 mode) eqn:E;
      [apply String.eqb_eq in E; congruence|reflexivity].
  - destruct (String.eqb mode mode') eqn:E.
    + apply String.eqb_eq in E; subst; cbn.
      destruct (String.eqb mode2 mode') eqn:E2;
        [apply String.eqb_eq in E2; congruence|reflexivity].
    + cbn; destruct (String.eqb mode2 mode') eqn:E2; [reflexivity|].
      apply IH; exact Hne.
Qed.

Lemma ApplyLine_high_same_mode :
  forall m pri l start endExclusive m1 m2,
    high (GetMode (ApplyLine m pri l start endExclusive m1 m2 1)
            (l_shipmode l)) =
    high (GetMode m (l_shipmode l)) +
    if LineQualifies l
         && InWindow (l_receiptdate l) start endExclusive
         && ModeAccepted (l_shipmode l) m1 m2
    then match PriLookup pri (l_orderkey l) with
         | Some High => 1
         | _ => 0
         end
    else 0.
Proof.
  intros m pri l start endExclusive m1 m2.
  unfold ApplyLine.
  destruct (LineQualifies l) eqn:Hq; cbn; [|lia].
  destruct (InWindow (l_receiptdate l) start endExclusive) eqn:Hw; cbn; [|lia].
  destruct (ModeAccepted (l_shipmode l) m1 m2) eqn:Hm; cbn; [|lia].
  destruct (PriLookup pri (l_orderkey l)) as [p|] eqn:Hp; cbn; [|lia].
  destruct p; cbn.
  - rewrite GetMode_SetMode_same; unfold SpecApply; cbn; lia.
  - rewrite GetMode_SetMode_same; unfold SpecApply; cbn; lia.
Qed.

Lemma ApplyLine_low_same_mode :
  forall m pri l start endExclusive m1 m2,
    low (GetMode (ApplyLine m pri l start endExclusive m1 m2 1)
           (l_shipmode l)) =
    low (GetMode m (l_shipmode l)) +
    if LineQualifies l
         && InWindow (l_receiptdate l) start endExclusive
         && ModeAccepted (l_shipmode l) m1 m2
    then match PriLookup pri (l_orderkey l) with
         | Some Low => 1
         | _ => 0
         end
    else 0.
Proof.
  intros m pri l start endExclusive m1 m2.
  unfold ApplyLine.
  destruct (LineQualifies l) eqn:Hq; cbn; [|lia].
  destruct (InWindow (l_receiptdate l) start endExclusive) eqn:Hw; cbn; [|lia].
  destruct (ModeAccepted (l_shipmode l) m1 m2) eqn:Hm; cbn; [|lia].
  destruct (PriLookup pri (l_orderkey l)) as [p|] eqn:Hp; cbn; [|lia].
  destruct p; cbn.
  - rewrite GetMode_SetMode_same; unfold SpecApply; cbn; lia.
  - rewrite GetMode_SetMode_same; unfold SpecApply; cbn; lia.
Qed.

Lemma ApplyLine_GetMode_other_mode :
  forall m pri l start endExclusive m1 m2 mode,
    mode <> l_shipmode l ->
    GetMode (ApplyLine m pri l start endExclusive m1 m2 1) mode =
    GetMode m mode.
Proof.
  intros m pri l start endExclusive m1 m2 mode Hne.
  unfold ApplyLine.
  destruct (LineQualifies l && InWindow (l_receiptdate l) start endExclusive
              && ModeAccepted (l_shipmode l) m1 m2) eqn:Hkeep.
  2: reflexivity.
  destruct (PriLookup pri (l_orderkey l)) as [p|]; [|reflexivity].
  apply GetMode_SetMode_other; intro Heq; apply Hne; symmetry; exact Heq.
Qed.


Definition q12_high_delta
    (pri : list (Z * Priority)) (start endExclusive : Z)
    (m1 m2 mode : string) (l : LineRow) : Z :=
  if String.eqb (l_shipmode l) mode
       && LineQualifies l
       && InWindow (l_receiptdate l) start endExclusive
       && ModeAccepted (l_shipmode l) m1 m2
  then match PriLookup pri (l_orderkey l) with
       | Some High => 1
       | _ => 0
       end
  else 0.

Definition q12_low_delta
    (pri : list (Z * Priority)) (start endExclusive : Z)
    (m1 m2 mode : string) (l : LineRow) : Z :=
  if String.eqb (l_shipmode l) mode
       && LineQualifies l
       && InWindow (l_receiptdate l) start endExclusive
       && ModeAccepted (l_shipmode l) m1 m2
  then match PriLookup pri (l_orderkey l) with
       | Some Low => 1
       | _ => 0
       end
  else 0.

Lemma fold_left_Z_add_shift :
  forall (A : Type) (g : A -> Z) (l : list A) (n : Z),
    fold_left (fun acc x => acc + g x) l n =
    n + fold_left (fun acc x => acc + g x) l 0.
Proof.
  intros A g l; induction l as [|x xs IH]; intros n; [cbn; lia|].
  cbn [fold_left].
  rewrite (IH (n + g x)), (IH (0 + g x)); lia.
Qed.

Lemma FoldLines_high_GetMode_from :
  forall lines pri start endExclusive m1 m2 mode m0,
    high (GetMode
      (fold_left
        (fun m l => ApplyLine m pri l start endExclusive m1 m2 1)
        lines m0) mode) =
    high (GetMode m0 mode) +
    fold_left (fun acc l => acc + q12_high_delta pri start endExclusive m1 m2 mode l)
      lines 0.
Proof.
  induction lines as [|l ls IH]; intros pri start endExclusive m1 m2 mode m0;
    [cbn; lia|].
  cbn [fold_left].
  rewrite (IH pri start endExclusive m1 m2 mode
            (ApplyLine m0 pri l start endExclusive m1 m2 1)).
  assert (Hstep :
    high (GetMode (ApplyLine m0 pri l start endExclusive m1 m2 1) mode) =
    high (GetMode m0 mode) +
    q12_high_delta pri start endExclusive m1 m2 mode l).
  {
    unfold q12_high_delta.
    destruct (String.eqb (l_shipmode l) mode) eqn:Em.
    - apply String.eqb_eq in Em; subst mode.
      rewrite ApplyLine_high_same_mode; reflexivity.
    - rewrite ApplyLine_GetMode_other_mode.
      + replace (String.eqb (l_shipmode l) mode) with false
          by (symmetry; exact Em).
        cbn; lia.
      + intro Heq; apply String.eqb_neq in Em; congruence.
  }
  rewrite Hstep.
  (* high m0 + delta + fold ls 0 = high m0 + fold (l::ls) 0 *)
  cbn [fold_left].
  rewrite (fold_left_Z_add_shift _
    (q12_high_delta pri start endExclusive m1 m2 mode) ls
    (0 + q12_high_delta pri start endExclusive m1 m2 mode l)).
  lia.
Qed.

Lemma FoldLines_high_GetMode :
  forall lines pri start endExclusive m1 m2 mode,
    high (GetMode (FoldLines lines pri start endExclusive m1 m2) mode) =
    fold_left (fun acc l => acc + q12_high_delta pri start endExclusive m1 m2 mode l)
      lines 0.
Proof.
  intros.
  unfold FoldLines.
  rewrite (FoldLines_high_GetMode_from lines pri start endExclusive m1 m2 mode nil).
  cbn; lia.
Qed.

Lemma FoldLines_low_GetMode_from :
  forall lines pri start endExclusive m1 m2 mode m0,
    low (GetMode
      (fold_left
        (fun m l => ApplyLine m pri l start endExclusive m1 m2 1)
        lines m0) mode) =
    low (GetMode m0 mode) +
    fold_left (fun acc l => acc + q12_low_delta pri start endExclusive m1 m2 mode l)
      lines 0.
Proof.
  induction lines as [|l ls IH]; intros pri start endExclusive m1 m2 mode m0;
    [cbn; lia|].
  cbn [fold_left].
  rewrite (IH pri start endExclusive m1 m2 mode
            (ApplyLine m0 pri l start endExclusive m1 m2 1)).
  assert (Hstep :
    low (GetMode (ApplyLine m0 pri l start endExclusive m1 m2 1) mode) =
    low (GetMode m0 mode) +
    q12_low_delta pri start endExclusive m1 m2 mode l).
  {
    unfold q12_low_delta.
    destruct (String.eqb (l_shipmode l) mode) eqn:Em.
    - apply String.eqb_eq in Em; subst mode.
      rewrite ApplyLine_low_same_mode; reflexivity.
    - rewrite ApplyLine_GetMode_other_mode.
      + replace (String.eqb (l_shipmode l) mode) with false
          by (symmetry; exact Em).
        cbn; lia.
      + intro Heq; apply String.eqb_neq in Em; congruence.
  }
  rewrite Hstep.
  cbn [fold_left].
  rewrite (fold_left_Z_add_shift _
    (q12_low_delta pri start endExclusive m1 m2 mode) ls
    (0 + q12_low_delta pri start endExclusive m1 m2 mode l)).
  lia.
Qed.

Lemma FoldLines_low_GetMode :
  forall lines pri start endExclusive m1 m2 mode,
    low (GetMode (FoldLines lines pri start endExclusive m1 m2) mode) =
    fold_left (fun acc l => acc + q12_low_delta pri start endExclusive m1 m2 mode l)
      lines 0.
Proof.
  intros.
  unfold FoldLines.
  rewrite (FoldLines_low_GetMode_from lines pri start endExclusive m1 m2 mode nil).
  cbn; lia.
Qed.

Lemma SpecView_high_GetMode :
  forall orders lines mode,
    high (GetMode
      (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
      mode) =
    fold_left
      (fun acc l =>
        acc + q12_high_delta (PriorityMap orders)
          q12_catalog_start q12_catalog_end "MAIL" "SHIP" mode l)
      lines 0.
Proof.
  intros; unfold SpecView; apply FoldLines_high_GetMode.
Qed.

Lemma SpecView_low_GetMode :
  forall orders lines mode,
    low (GetMode
      (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
      mode) =
    fold_left
      (fun acc l =>
        acc + q12_low_delta (PriorityMap orders)
          q12_catalog_start q12_catalog_end "MAIL" "SHIP" mode l)
      lines 0.
Proof.
  intros; unfold SpecView; apply FoldLines_low_GetMode.
Qed.







(** * PriorityMap under UniqueOrderKeys *)

Lemma PriLookup_fold_congr :
  forall orders pri1 pri2 k2,
    PriLookup pri1 k2 = PriLookup pri2 k2 ->
    PriLookup
      (fold_left
        (fun acc o => PriUpsert acc (o_orderkey o) (ClassifyPriority (o_priority o)))
        orders pri1)
      k2 =
    PriLookup
      (fold_left
        (fun acc o => PriUpsert acc (o_orderkey o) (ClassifyPriority (o_priority o)))
        orders pri2)
      k2.
Proof.
  induction orders as [|o os IH]; intros pri1 pri2 k2 Hagree; [exact Hagree|].
  cbn [fold_left].
  apply IH.
  destruct (Z.eq_dec (o_orderkey o) k2) as [Heq|Hne].
  - subst k2. rewrite !PriLookup_PriUpsert_same. reflexivity.
  - rewrite !(PriLookup_PriUpsert_other _ _ _ _ Hne). exact Hagree.
Qed.

Lemma PriLookup_fold_PriUpsert_other :
  forall orders pri k p k2,
    k <> k2 ->
    PriLookup
      (fold_left
        (fun acc o => PriUpsert acc (o_orderkey o) (ClassifyPriority (o_priority o)))
        orders (PriUpsert pri k p))
      k2 =
    PriLookup
      (fold_left
        (fun acc o => PriUpsert acc (o_orderkey o) (ClassifyPriority (o_priority o)))
        orders pri)
      k2.
Proof.
  intros orders pri k p k2 Hne.
  apply PriLookup_fold_congr.
  apply PriLookup_PriUpsert_other; exact Hne.
Qed.

Lemma PriLookup_fold_fresh :
  forall rest pri k p,
    PriLookup pri k = Some p ->
    (forall o, In o rest -> o_orderkey o <> k) ->
    PriLookup
      (fold_left
        (fun acc o => PriUpsert acc (o_orderkey o) (ClassifyPriority (o_priority o)))
        rest pri)
      k = Some p.
Proof.
  induction rest as [|o os IH]; intros pri k p Hlook Hfr; [exact Hlook|].
  cbn [fold_left].
  apply IH.
  - rewrite PriLookup_PriUpsert_other; [exact Hlook|].
    apply Hfr; left; reflexivity.
  - intros o' Ho'; apply Hfr; right; exact Ho'.
Qed.

Lemma PriLookup_PriorityMap_In :
  forall orders o,
    UniqueOrderKeys orders ->
    In o orders ->
    PriLookup (PriorityMap orders) (o_orderkey o) =
    Some (ClassifyPriority (o_priority o)).
Proof.
  intros orders o Huniq Hin.
  unfold UniqueOrderKeys, PriorityMap in *.
  revert o Hin; induction orders as [|o' os IH]; intros o Hin;
    [contradiction Hin|].
  cbn [map] in Huniq.
  inversion Huniq as [| ? ? Hnin Hnd]; subst.
  cbn [fold_left].
  destruct Hin as [Heq|Hin].
  - subst o.
    apply PriLookup_fold_fresh.
    + apply PriLookup_PriUpsert_same.
    + intros o0 Ho0 Heqkey.
      apply Hnin, in_map_iff.
      exists o0; split; [exact Heqkey|exact Ho0].
  - assert (Hne : o_orderkey o' <> o_orderkey o).
    {
      intro Heqkey; apply Hnin, in_map_iff.
      exists o; split; [symmetry; exact Heqkey|exact Hin].
    }
    rewrite (PriLookup_fold_PriUpsert_other os nil
      (o_orderkey o') (ClassifyPriority (o_priority o'))
      (o_orderkey o) Hne).
    apply IH; [exact Hnd|exact Hin].
Qed.

(** * SpecPairKeep ↔ PriLookup under UniqueOrderKeys *)

Lemma SpecPairKeep_PriLookup :
  forall orders o l,
    UniqueOrderKeys orders ->
    In o orders ->
    SpecPairKeep o l = true ->
    PriLookup (PriorityMap orders) (l_orderkey l) =
    Some (ClassifyPriority (o_priority o)).
Proof.
  intros orders o l Huniq Hin Hkeep.
  apply SpecPairKeep_iff in Hkeep.
  destruct Hkeep as [Hk _].
  rewrite <- Hk.
  apply PriLookup_PriorityMap_In; assumption.
Qed.

Lemma q12_priority_is_high_eq_Classify_High :
  forall p,
    q12_priority_is_high p = true <-> ClassifyPriority p = High.
Proof. exact q12_priority_is_high_Classify. Qed.

Lemma q12_high_delta_of_keep :
  forall orders o l mode,
    UniqueOrderKeys orders ->
    In o orders ->
    SpecPairKeep o l = true ->
    q12_high_delta (PriorityMap orders)
      q12_catalog_start q12_catalog_end "MAIL" "SHIP" mode l =
    if String.eqb (l_shipmode l) mode
    then (if q12_priority_is_high (o_priority o) then 1 else 0)
    else 0.
Proof.
  intros orders o l mode Huniq Hin Hkeep.
  unfold q12_high_delta.
  apply SpecPairKeep_iff in Hkeep.
  destruct Hkeep as [Hk [Hq [Hw Hm]]].
  rewrite Hq, Hw, Hm.
  cbn [andb].
  destruct (String.eqb (l_shipmode l) mode) eqn:Em; cbn [andb].
  - rewrite (SpecPairKeep_PriLookup orders o l Huniq Hin).
    2: { apply SpecPairKeep_iff; repeat split; assumption. }
    destruct (q12_priority_is_high (o_priority o)) eqn:Ehi.
    + apply q12_priority_is_high_Classify in Ehi; rewrite Ehi; reflexivity.
    + assert (ClassifyPriority (o_priority o) = Low).
      { apply q12_priority_is_high_negb_Classify_Low.
        rewrite Ehi; reflexivity. }
      rewrite H; reflexivity.
  - reflexivity.
Qed.

Lemma q12_low_delta_of_keep :
  forall orders o l mode,
    UniqueOrderKeys orders ->
    In o orders ->
    SpecPairKeep o l = true ->
    q12_low_delta (PriorityMap orders)
      q12_catalog_start q12_catalog_end "MAIL" "SHIP" mode l =
    if String.eqb (l_shipmode l) mode
    then (if negb (q12_priority_is_high (o_priority o)) then 1 else 0)
    else 0.
Proof.
  intros orders o l mode Huniq Hin Hkeep.
  unfold q12_low_delta.
  apply SpecPairKeep_iff in Hkeep.
  destruct Hkeep as [Hk [Hq [Hw Hm]]].
  rewrite Hq, Hw, Hm.
  cbn [andb].
  destruct (String.eqb (l_shipmode l) mode) eqn:Em; cbn [andb].
  - rewrite (SpecPairKeep_PriLookup orders o l Huniq Hin).
    2: { apply SpecPairKeep_iff; repeat split; assumption. }
    destruct (q12_priority_is_high (o_priority o)) eqn:Ehi.
    + apply q12_priority_is_high_Classify in Ehi; rewrite Ehi; reflexivity.
    + assert (ClassifyPriority (o_priority o) = Low).
      { apply q12_priority_is_high_negb_Classify_Low.
        rewrite Ehi; reflexivity. }
      rewrite H; reflexivity.
  - reflexivity.
Qed.

Lemma PriLookup_PriorityMap_Some_inv :
  forall orders k p,
    UniqueOrderKeys orders ->
    PriLookup (PriorityMap orders) k = Some p ->
    exists o, In o orders /\ o_orderkey o = k /\
      ClassifyPriority (o_priority o) = p.
Proof.
  intros orders k p Huniq Hlook.
  unfold UniqueOrderKeys, PriorityMap in *.
  revert k p Hlook; induction orders as [|o os IH]; intros k p Hlook.
  - cbn in Hlook; discriminate.
  - cbn [map] in Huniq.
    inversion Huniq as [| ? ? Hnin Hnd]; subst; clear Huniq.
    cbn [fold_left] in Hlook.
    destruct (Z.eq_dec (o_orderkey o) k) as [Heq|Hne].
    + subst k.
      assert (Hfr :
        PriLookup
          (fold_left
            (fun acc o' =>
              PriUpsert acc (o_orderkey o') (ClassifyPriority (o_priority o')))
            os
            (PriUpsert nil (o_orderkey o) (ClassifyPriority (o_priority o))))
          (o_orderkey o) =
        Some (ClassifyPriority (o_priority o))).
      {
        apply PriLookup_fold_fresh.
        - apply PriLookup_PriUpsert_same.
        - intros o0 Ho0 Heqkey.
          apply Hnin.
          apply in_map_iff.
          exists o0; split; [exact Heqkey|exact Ho0].
      }
      rewrite Hfr in Hlook; inversion Hlook; subst p; clear Hlook Hfr.
      exists o.
      split; [left; reflexivity|].
      split; reflexivity.
    + rewrite (PriLookup_fold_PriUpsert_other os nil
        (o_orderkey o) (ClassifyPriority (o_priority o)) k Hne) in Hlook.
      destruct (IH Hnd _ _ Hlook) as [o' [Hin [Hk Hp]]].
      exists o'.
      split; [right; exact Hin|].
      split; [exact Hk|exact Hp].
Qed.

Lemma q12_high_delta_zero_no_keep :
  forall orders l mode,
    UniqueOrderKeys orders ->
    (forall o, In o orders -> SpecPairKeep o l = false) ->
    q12_high_delta (PriorityMap orders)
      q12_catalog_start q12_catalog_end "MAIL" "SHIP" mode l = 0.
Proof.
  intros orders l mode Huniq Hnone.
  unfold q12_high_delta.
  destruct (String.eqb (l_shipmode l) mode) eqn:Em; [|reflexivity].
  destruct (LineQualifies l) eqn:Hq; [|reflexivity].
  destruct (InWindow (l_receiptdate l) q12_catalog_start q12_catalog_end)
    eqn:Hw; [|reflexivity].
  destruct (ModeAccepted (l_shipmode l) "MAIL" "SHIP") eqn:Hm; [|reflexivity].
  cbn [andb].
  destruct (PriLookup (PriorityMap orders) (l_orderkey l)) as [p|] eqn:Hp;
    [|reflexivity].
  destruct (PriLookup_PriorityMap_Some_inv orders _ _ Huniq Hp)
    as [o [Hin [Hk _]]].
  assert (Hkeep : SpecPairKeep o l = true).
  { apply SpecPairKeep_iff; repeat split; assumption. }
  specialize (Hnone o Hin); congruence.
Qed.

Lemma q12_low_delta_zero_no_keep :
  forall orders l mode,
    UniqueOrderKeys orders ->
    (forall o, In o orders -> SpecPairKeep o l = false) ->
    q12_low_delta (PriorityMap orders)
      q12_catalog_start q12_catalog_end "MAIL" "SHIP" mode l = 0.
Proof.
  intros orders l mode Huniq Hnone.
  unfold q12_low_delta.
  destruct (String.eqb (l_shipmode l) mode) eqn:Em; [|reflexivity].
  destruct (LineQualifies l) eqn:Hq; [|reflexivity].
  destruct (InWindow (l_receiptdate l) q12_catalog_start q12_catalog_end)
    eqn:Hw; [|reflexivity].
  destruct (ModeAccepted (l_shipmode l) "MAIL" "SHIP") eqn:Hm; [|reflexivity].
  cbn [andb].
  destruct (PriLookup (PriorityMap orders) (l_orderkey l)) as [p|] eqn:Hp;
    [|reflexivity].
  destruct (PriLookup_PriorityMap_Some_inv orders _ _ Huniq Hp)
    as [o [Hin [Hk _]]].
  assert (Hkeep : SpecPairKeep o l = true).
  { apply SpecPairKeep_iff; repeat split; assumption. }
  specialize (Hnone o Hin); congruence.
Qed.



(** * Per-line delta = unique SpecPairKeep contribution *)

Definition q12_keep_high_bit (o : OrderRow) (l : LineRow) (mode : string) : Z :=
  if SpecPairKeep o l && String.eqb (l_shipmode l) mode
  then if q12_priority_is_high (o_priority o) then 1 else 0
  else 0.

Definition q12_keep_low_bit (o : OrderRow) (l : LineRow) (mode : string) : Z :=
  if SpecPairKeep o l && String.eqb (l_shipmode l) mode
  then if negb (q12_priority_is_high (o_priority o)) then 1 else 0
  else 0.

Lemma NoDup_map_In_eq :
  forall (A B : Type) (f : A -> B) (xs : list A) (x y : A),
    NoDup (map f xs) ->
    In x xs ->
    In y xs ->
    f x = f y ->
    x = y.
Proof.
  intros A B f xs; induction xs as [|a xs IH]; intros x y Hnd Hx Hy Heq;
    [contradiction Hx|].
  cbn [map] in Hnd; inversion Hnd as [| ? ? Hnin Hnd']; subst.
  destruct Hx as [Hx|Hx]; destruct Hy as [Hy|Hy]; subst.
  - reflexivity.
  - exfalso; apply Hnin, in_map_iff; exists y; split; [symmetry; exact Heq|exact Hy].
  - exfalso; apply Hnin, in_map_iff; exists x; split; [exact Heq|exact Hx].
  - apply IH; assumption.
Qed.

Lemma UniqueOrderKeys_SpecPairKeep_unique :
  forall orders o1 o2 l,
    UniqueOrderKeys orders ->
    In o1 orders ->
    In o2 orders ->
    SpecPairKeep o1 l = true ->
    SpecPairKeep o2 l = true ->
    o1 = o2.
Proof.
  intros orders o1 o2 l Huniq Hin1 Hin2 Hk1 Hk2.
  apply SpecPairKeep_iff in Hk1; apply SpecPairKeep_iff in Hk2.
  destruct Hk1 as [He1 _]; destruct Hk2 as [He2 _].
  eapply NoDup_map_In_eq; [exact Huniq|exact Hin1|exact Hin2|congruence].
Qed.

Lemma fold_left_add_zero_ext :
  forall (A : Type) (g : A -> Z) (xs : list A) (acc : Z),
    (forall x, In x xs -> g x = 0) ->
    fold_left (fun a x => a + g x) xs acc = acc.
Proof.
  intros A g xs; induction xs as [|x xs IH]; intros acc Hg; [reflexivity|].
  cbn [fold_left].
  rewrite (Hg x (or_introl eq_refl)).
  replace (acc + 0) with acc by lia.
  apply IH; intros y Hy; apply Hg; right; exact Hy.
Qed.

Lemma keep_high_bit_false :
  forall o l mode,
    SpecPairKeep o l = false ->
    q12_keep_high_bit o l mode = 0.
Proof.
  intros o l mode H; unfold q12_keep_high_bit; rewrite H; reflexivity.
Qed.

Lemma keep_low_bit_false :
  forall o l mode,
    SpecPairKeep o l = false ->
    q12_keep_low_bit o l mode = 0.
Proof.
  intros o l mode H; unfold q12_keep_low_bit; rewrite H; reflexivity.
Qed.

Lemma q12_high_delta_eq_fold_keep :
  forall orders l mode,
    UniqueOrderKeys orders ->
    q12_high_delta (PriorityMap orders)
      q12_catalog_start q12_catalog_end "MAIL" "SHIP" mode l =
    fold_left (fun acc o => acc + q12_keep_high_bit o l mode) orders 0.
Proof.
  intros orders l mode Huniq.
  unfold UniqueOrderKeys in Huniq.
  induction orders as [|o os IH].
  - unfold q12_high_delta, PriorityMap; cbn.
    destruct (String.eqb (l_shipmode l) mode && LineQualifies l
                && InWindow (l_receiptdate l) q12_catalog_start q12_catalog_end
                && ModeAccepted (l_shipmode l) "MAIL" "SHIP"); reflexivity.
  - cbn [map] in Huniq; inversion Huniq as [| ? ? Hnin Hnd]; subst.
    change (UniqueOrderKeys (o :: os)) in Huniq.
    cbn [fold_left].
    destruct (SpecPairKeep o l) eqn:Hkeep.
    + (* head keeps: delta = bit, rest keep-bits 0 *)
      rewrite (q12_high_delta_of_keep (o :: os) o l mode Huniq).
      2: { left; reflexivity. }
      2: { exact Hkeep. }
      assert (Hrest0 :
        forall o2, In o2 os -> q12_keep_high_bit o2 l mode = 0).
      {
        intros o2 Ho2.
        destruct (SpecPairKeep o2 l) eqn:E;
          [|apply keep_high_bit_false; exact E].
        exfalso.
        assert (Heq : o = o2).
        {
          eapply UniqueOrderKeys_SpecPairKeep_unique;
            [exact Huniq
            |left; reflexivity|right; exact Ho2|exact Hkeep|exact E].
        }
        subst o2.
        apply Hnin.
        apply in_map_iff.
        exists o; split; [reflexivity|exact Ho2].
      }
      rewrite (fold_left_add_zero_ext _
        (fun o2 => q12_keep_high_bit o2 l mode) os
        (0 + q12_keep_high_bit o l mode) Hrest0).
      unfold q12_keep_high_bit; rewrite Hkeep; cbn [andb].
      destruct (String.eqb (l_shipmode l) mode); lia.
    + (* head does not keep *)
      rewrite (keep_high_bit_false o l mode Hkeep).
      replace (0 + 0) with 0 by lia.
      rewrite <- (IH Hnd).
      (* relate PriorityMap (o::os) delta to PriorityMap os *)
      unfold q12_high_delta.
      destruct (String.eqb (l_shipmode l) mode) eqn:Em; [|reflexivity].
      destruct (LineQualifies l) eqn:Hq; [|reflexivity].
      destruct (InWindow (l_receiptdate l) q12_catalog_start q12_catalog_end)
        eqn:Hw; [|reflexivity].
      destruct (ModeAccepted (l_shipmode l) "MAIL" "SHIP") eqn:Hm; [|reflexivity].
      cbn [andb].
      (* SpecPairKeep false with all guards true ⇒ key mismatch *)
      assert (Hne : o_orderkey o <> l_orderkey l).
      {
        intro Heq.
        assert (Htrue : SpecPairKeep o l = true).
        { apply SpecPairKeep_iff; repeat split; assumption. }
        congruence.
      }
      unfold PriorityMap; cbn [fold_left].
      rewrite (PriLookup_fold_PriUpsert_other os nil
        (o_orderkey o) (ClassifyPriority (o_priority o))
        (l_orderkey l) Hne).
      reflexivity.
Qed.

Lemma q12_low_delta_eq_fold_keep :
  forall orders l mode,
    UniqueOrderKeys orders ->
    q12_low_delta (PriorityMap orders)
      q12_catalog_start q12_catalog_end "MAIL" "SHIP" mode l =
    fold_left (fun acc o => acc + q12_keep_low_bit o l mode) orders 0.
Proof.
  intros orders l mode Huniq.
  unfold UniqueOrderKeys in Huniq.
  induction orders as [|o os IH].
  - unfold q12_low_delta, PriorityMap; cbn.
    destruct (String.eqb (l_shipmode l) mode && LineQualifies l
                && InWindow (l_receiptdate l) q12_catalog_start q12_catalog_end
                && ModeAccepted (l_shipmode l) "MAIL" "SHIP"); reflexivity.
  - cbn [map] in Huniq; inversion Huniq as [| ? ? Hnin Hnd]; subst.
    change (UniqueOrderKeys (o :: os)) in Huniq.
    cbn [fold_left].
    destruct (SpecPairKeep o l) eqn:Hkeep.
    + rewrite (q12_low_delta_of_keep (o :: os) o l mode Huniq).
      2: { left; reflexivity. }
      2: { exact Hkeep. }
      assert (Hrest0 :
        forall o2, In o2 os -> q12_keep_low_bit o2 l mode = 0).
      {
        intros o2 Ho2.
        destruct (SpecPairKeep o2 l) eqn:E;
          [|apply keep_low_bit_false; exact E].
        exfalso.
        assert (Heq : o = o2).
        {
          eapply UniqueOrderKeys_SpecPairKeep_unique;
            [exact Huniq
            |left; reflexivity|right; exact Ho2|exact Hkeep|exact E].
        }
        subst o2.
        apply Hnin.
        apply in_map_iff.
        exists o; split; [reflexivity|exact Ho2].
      }
      rewrite (fold_left_add_zero_ext _
        (fun o2 => q12_keep_low_bit o2 l mode) os
        (0 + q12_keep_low_bit o l mode) Hrest0).
      unfold q12_keep_low_bit; rewrite Hkeep; cbn [andb].
      destruct (String.eqb (l_shipmode l) mode); lia.
    + rewrite (keep_low_bit_false o l mode Hkeep).
      replace (0 + 0) with 0 by lia.
      rewrite <- (IH Hnd).
      unfold q12_low_delta.
      destruct (String.eqb (l_shipmode l) mode) eqn:Em; [|reflexivity].
      destruct (LineQualifies l) eqn:Hq; [|reflexivity].
      destruct (InWindow (l_receiptdate l) q12_catalog_start q12_catalog_end)
        eqn:Hw; [|reflexivity].
      destruct (ModeAccepted (l_shipmode l) "MAIL" "SHIP") eqn:Hm; [|reflexivity].
      cbn [andb].
      assert (Hne : o_orderkey o <> l_orderkey l).
      {
        intro Heq.
        assert (Htrue : SpecPairKeep o l = true).
        { apply SpecPairKeep_iff; repeat split; assumption. }
        congruence.
      }
      unfold PriorityMap; cbn [fold_left].
      rewrite (PriLookup_fold_PriUpsert_other os nil
        (o_orderkey o) (ClassifyPriority (o_priority o))
        (l_orderkey l) Hne).
      reflexivity.
Qed.


(** * Fubini: line-fold of order-fold = order-fold of line-fold *)

Fixpoint zsum_list {A : Type} (f : A -> Z) (xs : list A) : Z :=
  match xs with
  | nil => 0
  | x :: xs => f x + zsum_list f xs
  end.

Lemma fold_left_add_eq_zsum :
  forall (A : Type) (f : A -> Z) (xs : list A) (acc : Z),
    fold_left (fun a x => a + f x) xs acc = acc + zsum_list f xs.
Proof.
  intros A f xs; induction xs as [|x xs IH]; intros acc; [cbn; lia|].
  cbn [fold_left zsum_list]; rewrite IH; lia.
Qed.

Lemma zsum_list_fubini :
  forall (A B : Type) (h : A -> B -> Z) (xs : list A) (ys : list B),
    zsum_list (fun y => zsum_list (fun x => h x y) xs) ys =
    zsum_list (fun x => zsum_list (fun y => h x y) ys) xs.
Proof.
  intros A B h xs ys.
  induction xs as [|x xs IH]; [cbn; induction ys as [|y ys IHy]; cbn; lia|].
  cbn [zsum_list].
  assert (Hsplit :
    forall ys',
      zsum_list (fun y => h x y + zsum_list (fun x' => h x' y) xs) ys' =
      zsum_list (fun y => h x y) ys' +
      zsum_list (fun y => zsum_list (fun x' => h x' y) xs) ys').
  {
    induction ys' as [|y ys' IHy]; [cbn; lia|].
    cbn [zsum_list]; rewrite IHy; lia.
  }
  rewrite (Hsplit ys), IH; lia.
Qed.

Lemma zsum_fold_left_pointwise :
  forall (A B : Type) (h : A -> B -> Z) (xs : list A) (ys : list B),
    zsum_list (fun y => fold_left (fun a x => a + h x y) xs 0) ys =
    zsum_list (fun y => zsum_list (fun x => h x y) xs) ys.
Proof.
  intros A B h xs ys.
  induction ys as [|y ys IHy]; [reflexivity|].
  cbn [zsum_list]; rewrite IHy, (fold_left_add_eq_zsum _ _ xs 0), Z.add_0_l;
    reflexivity.
Qed.

Lemma zsum_fold_left_pointwise_swap :
  forall (A B : Type) (h : A -> B -> Z) (xs : list A) (ys : list B),
    zsum_list (fun x => fold_left (fun a y => a + h x y) ys 0) xs =
    zsum_list (fun x => zsum_list (fun y => h x y) ys) xs.
Proof.
  intros A B h xs ys.
  induction xs as [|x xs IHx]; [reflexivity|].
  cbn [zsum_list]; rewrite IHx, (fold_left_add_eq_zsum _ _ ys 0), Z.add_0_l;
    reflexivity.
Qed.

Lemma fold_left_Z_add_sum_comm :
  forall (A B : Type) (h : A -> B -> Z) (xs : list A) (ys : list B),
    fold_left (fun a y =>
      a + fold_left (fun a x => a + h x y) xs 0) ys 0 =
    fold_left (fun a x =>
      a + fold_left (fun a y => a + h x y) ys 0) xs 0.
Proof.
  intros A B h xs ys.
  rewrite !(fold_left_add_eq_zsum _ _ _ 0), !Z.add_0_l.
  rewrite (zsum_fold_left_pointwise A B h xs ys).
  rewrite (zsum_fold_left_pointwise_swap A B h xs ys).
  apply zsum_list_fubini.
Qed.

Lemma SpecView_high_eq_keep_fold :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    high (GetMode
      (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
      mode) =
    fold_left (fun acc o =>
      acc + fold_left (fun acc l => acc + q12_keep_high_bit o l mode) lines 0)
      orders 0.
Proof.
  intros orders lines mode Huniq.
  rewrite SpecView_high_GetMode.
  rewrite <- (fold_left_Z_add_sum_comm OrderRow LineRow
    (fun o l => q12_keep_high_bit o l mode) orders lines).
  induction lines as [|l ls IH]; [reflexivity|].
  cbn [fold_left].
  rewrite (fold_left_Z_add_shift _
    (q12_high_delta (PriorityMap orders)
      q12_catalog_start q12_catalog_end "MAIL" "SHIP" mode) ls
    (0 + q12_high_delta (PriorityMap orders)
      q12_catalog_start q12_catalog_end "MAIL" "SHIP" mode l)).
  rewrite (fold_left_Z_add_shift _
    (fun l' => fold_left (fun acc o => acc + q12_keep_high_bit o l' mode) orders 0)
    ls
    (0 + fold_left (fun acc o => acc + q12_keep_high_bit o l mode) orders 0)).
  rewrite (q12_high_delta_eq_fold_keep orders l mode Huniq).
  rewrite IH; lia.
Qed.

Lemma SpecView_low_eq_keep_fold :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    low (GetMode
      (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
      mode) =
    fold_left (fun acc o =>
      acc + fold_left (fun acc l => acc + q12_keep_low_bit o l mode) lines 0)
      orders 0.
Proof.
  intros orders lines mode Huniq.
  rewrite SpecView_low_GetMode.
  rewrite <- (fold_left_Z_add_sum_comm OrderRow LineRow
    (fun o l => q12_keep_low_bit o l mode) orders lines).
  induction lines as [|l ls IH]; [reflexivity|].
  cbn [fold_left].
  rewrite (fold_left_Z_add_shift _
    (q12_low_delta (PriorityMap orders)
      q12_catalog_start q12_catalog_end "MAIL" "SHIP" mode) ls
    (0 + q12_low_delta (PriorityMap orders)
      q12_catalog_start q12_catalog_end "MAIL" "SHIP" mode l)).
  rewrite (fold_left_Z_add_shift _
    (fun l' => fold_left (fun acc o => acc + q12_keep_low_bit o l' mode) orders 0)
    ls
    (0 + fold_left (fun acc o => acc + q12_keep_low_bit o l mode) orders 0)).
  rewrite (q12_low_delta_eq_fold_keep orders l mode Huniq).
  rewrite IH; lia.
Qed.


(** * Keep-bits ↔ filtered SpecPairKeep counts *)

Lemma keep_high_bit_eq_bool :
  forall o l mode,
    q12_keep_high_bit o l mode =
    if SpecPairKeep o l && String.eqb (l_shipmode l) mode
         && q12_priority_is_high (o_priority o)
    then 1 else 0.
Proof.
  intros o l mode; unfold q12_keep_high_bit.
  destruct (SpecPairKeep o l); destruct (String.eqb (l_shipmode l) mode);
    destruct (q12_priority_is_high (o_priority o)); reflexivity.
Qed.

Lemma keep_low_bit_eq_bool :
  forall o l mode,
    q12_keep_low_bit o l mode =
    if SpecPairKeep o l && String.eqb (l_shipmode l) mode
         && negb (q12_priority_is_high (o_priority o))
    then 1 else 0.
Proof.
  intros o l mode; unfold q12_keep_low_bit.
  destruct (SpecPairKeep o l); destruct (String.eqb (l_shipmode l) mode);
    destruct (q12_priority_is_high (o_priority o)); reflexivity.
Qed.

Lemma fold_orders_lines_to_zsum :
  forall (h : OrderRow -> LineRow -> Z) orders lines,
    fold_left (fun acc o =>
      acc + fold_left (fun acc l => acc + h o l) lines 0) orders 0 =
    zsum_list (fun o => zsum_list (fun l => h o l) lines) orders.
Proof.
  intros h orders lines.
  rewrite (fold_left_add_eq_zsum _
    (fun o => fold_left (fun acc l => acc + h o l) lines 0) orders 0),
    Z.add_0_l.
  induction orders as [|o os IH]; [reflexivity|].
  cbn [zsum_list]; rewrite IH, (fold_left_add_eq_zsum _ _ lines 0), Z.add_0_l;
    reflexivity.
Qed.

Lemma SpecView_high_eq_keep_zsum :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    high (GetMode
      (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
      mode) =
    zsum_list (fun o =>
      zsum_list (fun l => q12_keep_high_bit o l mode) lines) orders.
Proof.
  intros orders lines mode Huniq.
  rewrite SpecView_high_eq_keep_fold; [|exact Huniq].
  apply fold_orders_lines_to_zsum.
Qed.

Lemma SpecView_low_eq_keep_zsum :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    low (GetMode
      (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
      mode) =
    zsum_list (fun o =>
      zsum_list (fun l => q12_keep_low_bit o l mode) lines) orders.
Proof.
  intros orders lines mode Huniq.
  rewrite SpecView_low_eq_keep_fold; [|exact Huniq].
  apply fold_orders_lines_to_zsum.
Qed.

(** * Mode-keyed pre-group rows and grouping key *)

Definition q12_mode_pre_rows
    (orders : list OrderRow) (lines : list LineRow) (mode : string)
  : list (tuple TNull) :=
  flat_map
    (fun o =>
      map (fun l => q12_pre_group_row o l)
        (List.filter
          (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode)
          lines))
    orders.

Lemma q12_pre_group_row_grouping_key :
  forall o l,
    query_grouping_key nil q12_group_terms (q12_pre_group_row o l) =
    [embed_string_char10 (l_shipmode l)].
Proof.
  intros o l.
  unfold query_grouping_key, q12_group_terms.
  cbn [map].
  unfold DotString.
  rewrite (Interp.interp_aggterm_unfold TNull).
  unfold AExpr; cbn; unfold Dot.
  change (AttrString "l_shipmode" (StringChar 10)) with q12_attr_shipmode.
  assert (Hlab : q12_attr_shipmode inS labels TNull (q12_pre_group_row o l)).
  {
    unfold q12_pre_group_row.
    apply q12_attr_inS_project_row; exact q12_case_select_outputs_shipmode.
  }
  change (
    [if q12_attr_shipmode inS? labels TNull (q12_pre_group_row o l)
     then dot TNull (q12_pre_group_row o l) q12_attr_shipmode
     else NullValues.Value_string (StringValue StringText None)] =
    [embed_string_char10 (l_shipmode l)]).
  rewrite Hlab.
  rewrite q12_pre_group_row_dot_shipmode.
  reflexivity.
Qed.


Lemma q12_group_terms_nonempty : q12_group_terms <> nil.
Proof. unfold q12_group_terms; discriminate. Qed.

Lemma q12_group_is_mode_bucket :
  forall orders lines mode,
    List.filter
      (fun members =>
        match members with
        | nil => false
        | row :: _ =>
            Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
              (query_grouping_key nil q12_group_terms row)
              [embed_string_char10 mode]
        end)
      (query_make_groups nil (q12_pre_group_rows orders lines) q12_group_terms) =
    match
      List.filter
        (fun row =>
          Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
            (query_grouping_key nil q12_group_terms row)
            [embed_string_char10 mode])
        (q12_pre_group_rows orders lines)
    with
    | nil => nil
    | _ :: _ =>
        [List.rev
          (List.filter
            (fun row =>
              Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
                (query_grouping_key nil q12_group_terms row)
                [embed_string_char10 mode])
            (q12_pre_group_rows orders lines))]
    end.
Proof.
  intros orders lines mode.
  apply (query_make_groups_lookup_key_exact TNull nil q12_group_terms
    (q12_pre_group_rows orders lines) [embed_string_char10 mode]).
  exact q12_group_terms_nonempty.
Qed.

Lemma embed_string_char10_inj :
  forall s1 s2,
    embed_string_char10 s1 = embed_string_char10 s2 ->
    s1 = s2.
Proof.
  intros s1 s2 H.
  unfold embed_string_char10 in H.
  inversion H; reflexivity.
Qed.

Lemma q12_grouping_key_eqb_shipmode :
  forall o l mode,
    Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
      (query_grouping_key nil q12_group_terms (q12_pre_group_row o l))
      [embed_string_char10 mode] =
    String.eqb (l_shipmode l) mode.
Proof.
  intros o l mode.
  rewrite q12_pre_group_row_grouping_key.
  destruct (String.eqb (l_shipmode l) mode) eqn:Em.
  - apply String.eqb_eq in Em; subst mode.
    apply Oset.eq_bool_true_iff; reflexivity.
  - apply not_true_is_false; intro Htrue.
    apply Oset.eq_bool_true_iff in Htrue.
    inversion Htrue as [He].
    apply String.eqb_neq in Em.
    apply Em.
    (* He may be embeds-equal or strings-equal depending on inversion *)
    revert He; clear Htrue Em.
    intros He.
    match type of He with
    | embed_string_char10 ?s1 = embed_string_char10 ?s2 =>
        apply embed_string_char10_inj in He; exact He
    | ?s1 = ?s2 => exact He
    end.
Qed.

Lemma filter_pre_group_eq_mode_rows_one :
  forall o lines mode,
    List.filter
      (fun row =>
        Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
          (query_grouping_key nil q12_group_terms row)
          [embed_string_char10 mode])
      (map (fun l => q12_pre_group_row o l)
        (List.filter (fun l => SpecPairKeep o l) lines)) =
    map (fun l => q12_pre_group_row o l)
      (List.filter
        (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode)
        lines).
Proof.
  intros o lines mode.
  induction lines as [|l ls IH]; [reflexivity|].
  cbn [filter].
  destruct (SpecPairKeep o l) eqn:Hk.
  - cbn [map filter].
    rewrite q12_grouping_key_eqb_shipmode.
    destruct (String.eqb (l_shipmode l) mode) eqn:Em.
    + simpl; f_equal; exact IH.
    + simpl; exact IH.
  - simpl; exact IH.
Qed.

Lemma filter_pre_group_eq_mode_rows :
  forall orders lines mode,
    List.filter
      (fun row =>
        Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
          (query_grouping_key nil q12_group_terms row)
          [embed_string_char10 mode])
      (q12_pre_group_rows orders lines) =
    q12_mode_pre_rows orders lines mode.
Proof.
  intros orders lines mode.
  unfold q12_pre_group_rows, q12_mode_pre_rows.
  induction orders as [|o os IH]; [reflexivity|].
  cbn [flat_map].
  rewrite filter_app, filter_pre_group_eq_mode_rows_one, IH.
  reflexivity.
Qed.

(** * Mode-row f1/f2 dots are CASE bits; SUM ↔ SpecView counts *)

Lemma q12_mode_pre_rows_dot_f1 :
  forall orders lines mode,
    map (fun row => dot TNull row q12_attr_f1)
      (q12_mode_pre_rows orders lines mode) =
    flat_map
      (fun o =>
        map (fun l => q12_case_bit (q12_priority_is_high (o_priority o)))
          (List.filter
            (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode)
            lines))
      orders.
Proof.
  intros orders lines mode.
  unfold q12_mode_pre_rows.
  induction orders as [|o os IH]; [reflexivity|].
  cbn [flat_map].
  rewrite map_app.
  rewrite map_map.
  rewrite IH.
  apply f_equal2; [|reflexivity].
  apply map_ext; intro l; apply q12_pre_group_row_dot_f1.
Qed.

Lemma q12_mode_pre_rows_dot_f2 :
  forall orders lines mode,
    map (fun row => dot TNull row q12_attr_f2)
      (q12_mode_pre_rows orders lines mode) =
    flat_map
      (fun o =>
        map (fun l => q12_case_bit (negb (q12_priority_is_high (o_priority o))))
          (List.filter
            (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode)
            lines))
      orders.
Proof.
  intros orders lines mode.
  unfold q12_mode_pre_rows.
  induction orders as [|o os IH]; [reflexivity|].
  cbn [flat_map].
  rewrite map_app.
  rewrite map_map.
  rewrite IH.
  apply f_equal2; [|reflexivity].
  apply map_ext; intro l; apply q12_pre_group_row_dot_f2.
Qed.

Lemma zsum_keep_high_one_order_filter :
  forall o lines mode,
    zsum_list (fun l => q12_keep_high_bit o l mode) lines =
    Z.of_nat (List.length
      (List.filter
        (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode
                    && q12_priority_is_high (o_priority o))
        lines)).
Proof.
  intros o lines mode.
  induction lines as [|l ls IH]; [reflexivity|].
  cbn [zsum_list filter].
  rewrite keep_high_bit_eq_bool.
  destruct (SpecPairKeep o l && String.eqb (l_shipmode l) mode
              && q12_priority_is_high (o_priority o));
    cbn [List.length]; rewrite IH; lia.
Qed.

Lemma zsum_orders_keep_high_eq_count :
  forall orders lines mode,
    zsum_list (fun o =>
      zsum_list (fun l => q12_keep_high_bit o l mode) lines) orders =
    Z.of_nat (List.length
      (flat_map
        (fun o =>
          List.filter
            (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode
                        && q12_priority_is_high (o_priority o))
            lines)
        orders)).
Proof.
  intros orders lines mode.
  induction orders as [|o os IH]; [reflexivity|].
  cbn [zsum_list flat_map].
  rewrite zsum_keep_high_one_order_filter, IH, length_app, Nat2Z.inj_add.
  reflexivity.
Qed.

Lemma SpecView_high_eq_true_count :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    high (GetMode
      (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
      mode) =
    Z.of_nat (List.length
      (flat_map
        (fun o =>
          List.filter
            (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode
                        && q12_priority_is_high (o_priority o))
            lines)
        orders)).
Proof.
  intros orders lines mode Huniq.
  rewrite SpecView_high_eq_keep_zsum; [|exact Huniq].
  apply zsum_orders_keep_high_eq_count.
Qed.

Lemma zsum_keep_low_one_order_filter :
  forall o lines mode,
    zsum_list (fun l => q12_keep_low_bit o l mode) lines =
    Z.of_nat (List.length
      (List.filter
        (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode
                    && negb (q12_priority_is_high (o_priority o)))
        lines)).
Proof.
  intros o lines mode.
  induction lines as [|l ls IH]; [reflexivity|].
  cbn [zsum_list filter].
  rewrite keep_low_bit_eq_bool.
  destruct (SpecPairKeep o l && String.eqb (l_shipmode l) mode
              && negb (q12_priority_is_high (o_priority o)));
    cbn [List.length]; rewrite IH; lia.
Qed.

Lemma zsum_orders_keep_low_eq_count :
  forall orders lines mode,
    zsum_list (fun o =>
      zsum_list (fun l => q12_keep_low_bit o l mode) lines) orders =
    Z.of_nat (List.length
      (flat_map
        (fun o =>
          List.filter
            (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode
                        && negb (q12_priority_is_high (o_priority o)))
            lines)
        orders)).
Proof.
  intros orders lines mode.
  induction orders as [|o os IH]; [reflexivity|].
  cbn [zsum_list flat_map].
  rewrite zsum_keep_low_one_order_filter, IH, length_app, Nat2Z.inj_add.
  reflexivity.
Qed.

Lemma SpecView_low_eq_true_count :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    low (GetMode
      (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
      mode) =
    Z.of_nat (List.length
      (flat_map
        (fun o =>
          List.filter
            (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode
                        && negb (q12_priority_is_high (o_priority o)))
            lines)
        orders)).
Proof.
  intros orders lines mode Huniq.
  rewrite SpecView_low_eq_keep_zsum; [|exact Huniq].
  apply zsum_orders_keep_low_eq_count.
Qed.


(** * CASE-bit lists for a mode bucket *)

Definition q12_mode_high_bits
    (orders : list OrderRow) (lines : list LineRow) (mode : string)
  : list bool :=
  flat_map
    (fun o =>
      map (fun _ => q12_priority_is_high (o_priority o))
        (List.filter
          (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode)
          lines))
    orders.

Lemma length_filter_map_const_true :
  forall (A : Type) (xs : list A),
    List.length (List.filter (fun b : bool => b) (map (fun _ => true) xs)) =
    List.length xs.
Proof.
  induction xs; [reflexivity|]; cbn; f_equal; exact IHxs.
Qed.

Lemma length_filter_map_const_false :
  forall (A : Type) (xs : list A),
    List.length (List.filter (fun b : bool => b) (map (fun _ => false) xs)) =
    0%nat.
Proof.
  induction xs; [reflexivity|]; cbn; exact IHxs.
Qed.

Lemma length_filter_keep_mode_high_one :
  forall o lines mode,
    List.length (List.filter (fun b : bool => b)
      (map (fun _ => q12_priority_is_high (o_priority o))
        (List.filter
          (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode)
          lines))) =
    List.length
      (List.filter
        (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode
                    && q12_priority_is_high (o_priority o))
        lines).
Proof.
  intros o lines mode.
  induction lines as [|l ls IH]; [reflexivity|].
  cbn [filter].
  destruct (SpecPairKeep o l && String.eqb (l_shipmode l) mode) eqn:Hk.
  - cbn [map filter].
    destruct (q12_priority_is_high (o_priority o)); cbn [andb filter List.length];
      [f_equal; exact IH|exact IH].
  - (* keep&&mode = false ⇒ keep&&mode&&high = false *)
    cbn [andb].
    destruct (q12_priority_is_high (o_priority o)); cbn [filter]; exact IH.
Qed.

Lemma length_filter_mode_high_bits :
  forall orders lines mode,
    List.length (List.filter (fun b : bool => b)
      (q12_mode_high_bits orders lines mode)) =
    List.length
      (flat_map
        (fun o =>
          List.filter
            (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode
                        && q12_priority_is_high (o_priority o))
            lines)
        orders).
Proof.
  intros orders lines mode.
  unfold q12_mode_high_bits.
  induction orders as [|o os IH]; [reflexivity|].
  cbn [flat_map]; rewrite filter_app, !length_app, IH.
  rewrite length_filter_keep_mode_high_one; reflexivity.
Qed.

Lemma SpecView_high_eq_mode_high_bits_count :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    high (GetMode
      (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
      mode) =
    Z.of_nat (List.length (List.filter (fun b : bool => b)
      (q12_mode_high_bits orders lines mode))).
Proof.
  intros orders lines mode Huniq.
  rewrite SpecView_high_eq_true_count; [|exact Huniq].
  rewrite length_filter_mode_high_bits; reflexivity.
Qed.

Lemma q12_mode_pre_rows_f1_case_bits :
  forall orders lines mode,
    map (fun row => dot TNull row q12_attr_f1)
      (q12_mode_pre_rows orders lines mode) =
    map q12_case_bit (q12_mode_high_bits orders lines mode).
Proof.
  intros orders lines mode.
  rewrite q12_mode_pre_rows_dot_f1.
  unfold q12_mode_high_bits.
  induction orders as [|o os IH]; [reflexivity|].
  cbn [flat_map]; rewrite map_app, map_map, IH.
  reflexivity.
Qed.

Lemma q12_mode_sum_f1_eq_SpecView_high :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    q12_mode_high_bits orders lines mode <> nil ->
    Z.of_nat (List.length (q12_mode_high_bits orders lines mode)) <= int64_max ->
    NullValues.interp_sum_int32_as_int64
      (map (fun row => dot TNull row q12_attr_f1)
        (q12_mode_pre_rows orders lines mode)) =
    NullValues.value_int64_checked
      (high (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        mode)).
Proof.
  intros orders lines mode Huniq Hne Hlen.
  rewrite q12_mode_pre_rows_f1_case_bits.
  rewrite (SpecView_high_eq_mode_high_bits_count orders lines mode Huniq).
  apply interp_sum_int32_case_bits_eq_count; assumption.
Qed.

Definition q12_mode_low_bits
    (orders : list OrderRow) (lines : list LineRow) (mode : string)
  : list bool :=
  flat_map
    (fun o =>
      map (fun _ => negb (q12_priority_is_high (o_priority o)))
        (List.filter
          (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode)
          lines))
    orders.

Lemma length_filter_keep_mode_low_one :
  forall o lines mode,
    List.length (List.filter (fun b : bool => b)
      (map (fun _ => negb (q12_priority_is_high (o_priority o)))
        (List.filter
          (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode)
          lines))) =
    List.length
      (List.filter
        (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode
                    && negb (q12_priority_is_high (o_priority o)))
        lines).
Proof.
  intros o lines mode.
  induction lines as [|l ls IH]; [reflexivity|].
  cbn [filter].
  destruct (SpecPairKeep o l && String.eqb (l_shipmode l) mode) eqn:Hk.
  - cbn [map filter].
    destruct (q12_priority_is_high (o_priority o)); cbn [negb andb filter List.length];
      [exact IH|f_equal; exact IH].
  - cbn [andb].
    destruct (q12_priority_is_high (o_priority o)); cbn [negb filter]; exact IH.
Qed.

Lemma length_filter_mode_low_bits :
  forall orders lines mode,
    List.length (List.filter (fun b : bool => b)
      (q12_mode_low_bits orders lines mode)) =
    List.length
      (flat_map
        (fun o =>
          List.filter
            (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode
                        && negb (q12_priority_is_high (o_priority o)))
            lines)
        orders).
Proof.
  intros orders lines mode.
  unfold q12_mode_low_bits.
  induction orders as [|o os IH]; [reflexivity|].
  cbn [flat_map]; rewrite filter_app, !length_app, IH.
  rewrite length_filter_keep_mode_low_one; reflexivity.
Qed.

Lemma SpecView_low_eq_mode_low_bits_count :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    low (GetMode
      (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
      mode) =
    Z.of_nat (List.length (List.filter (fun b : bool => b)
      (q12_mode_low_bits orders lines mode))).
Proof.
  intros orders lines mode Huniq.
  rewrite SpecView_low_eq_true_count; [|exact Huniq].
  rewrite length_filter_mode_low_bits; reflexivity.
Qed.

Lemma q12_mode_pre_rows_f2_case_bits :
  forall orders lines mode,
    map (fun row => dot TNull row q12_attr_f2)
      (q12_mode_pre_rows orders lines mode) =
    map q12_case_bit (q12_mode_low_bits orders lines mode).
Proof.
  intros orders lines mode.
  rewrite q12_mode_pre_rows_dot_f2.
  unfold q12_mode_low_bits.
  induction orders as [|o os IH]; [reflexivity|].
  cbn [flat_map]; rewrite map_app, map_map, IH.
  reflexivity.
Qed.

Lemma q12_mode_sum_f2_eq_SpecView_low :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    q12_mode_low_bits orders lines mode <> nil ->
    Z.of_nat (List.length (q12_mode_low_bits orders lines mode)) <= int64_max ->
    NullValues.interp_sum_int32_as_int64
      (map (fun row => dot TNull row q12_attr_f2)
        (q12_mode_pre_rows orders lines mode)) =
    NullValues.value_int64_checked
      (low (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        mode)).
Proof.
  intros orders lines mode Huniq Hne Hlen.
  rewrite q12_mode_pre_rows_f2_case_bits.
  rewrite (SpecView_low_eq_mode_low_bits_count orders lines mode Huniq).
  apply interp_sum_int32_case_bits_eq_count; assumption.
Qed.

Lemma q12_mode_high_bits_length :
  forall orders lines mode,
    List.length (q12_mode_high_bits orders lines mode) =
    List.length (q12_mode_pre_rows orders lines mode).
Proof.
  intros orders lines mode.
  unfold q12_mode_high_bits, q12_mode_pre_rows.
  induction orders as [|o os IH]; [reflexivity|].
  cbn [flat_map]; rewrite !length_app, IH, !length_map; reflexivity.
Qed.

Lemma q12_mode_low_bits_length :
  forall orders lines mode,
    List.length (q12_mode_low_bits orders lines mode) =
    List.length (q12_mode_pre_rows orders lines mode).
Proof.
  intros orders lines mode.
  unfold q12_mode_low_bits, q12_mode_pre_rows.
  induction orders as [|o os IH]; [reflexivity|].
  cbn [flat_map]; rewrite !length_app, IH, !length_map; reflexivity.
Qed.

Lemma q12_mode_bucket_of_pre_rows :
  forall orders lines mode,
    List.filter
      (fun members =>
        match members with
        | nil => false
        | row :: _ =>
            Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
              (query_grouping_key nil q12_group_terms row)
              [embed_string_char10 mode]
        end)
      (query_make_groups nil (q12_pre_group_rows orders lines) q12_group_terms) =
    match q12_mode_pre_rows orders lines mode with
    | nil => nil
    | _ :: _ => [List.rev (q12_mode_pre_rows orders lines mode)]
    end.
Proof.
  intros orders lines mode.
  rewrite q12_group_is_mode_bucket, filter_pre_group_eq_mode_rows.
  destruct (q12_mode_pre_rows orders lines mode); reflexivity.
Qed.

(** * Group select values on a mode bucket = SpecOut columns *)

Lemma q12_mode_pre_row_dot_shipmode :
  forall orders lines mode row,
    In row (q12_mode_pre_rows orders lines mode) ->
    dot TNull row q12_attr_shipmode = embed_string_char10 mode.
Proof.
  intros orders lines mode row Hin.
  unfold q12_mode_pre_rows in Hin.
  apply in_flat_map in Hin.
  destruct Hin as [o [Ho Hin]].
  apply in_map_iff in Hin.
  destruct Hin as [l [Heq Hl]].
  apply filter_In in Hl.
  destruct Hl as [_ Hkeep].
  apply andb_true_iff in Hkeep; destruct Hkeep as [_ Hm].
  apply String.eqb_eq in Hm.
  subst row mode.
  apply q12_pre_group_row_dot_shipmode.
Qed.

Lemma q12_mode_pre_rows_shipmode_Forall :
  forall orders lines mode,
    Forall (fun row =>
      dot TNull row q12_attr_shipmode = embed_string_char10 mode)
      (q12_mode_pre_rows orders lines mode).
Proof.
  intros orders lines mode.
  apply Forall_forall; apply q12_mode_pre_row_dot_shipmode.
Qed.

Lemma Permutation_map_dot_f1_rev :
  forall group,
    Permutation
      (map (fun row => dot TNull row q12_attr_f1) (List.rev group))
      (map (fun row => dot TNull row q12_attr_f1) group).
Proof.
  intro group.
  rewrite map_rev.
  apply Permutation_sym, Permutation_rev.
Qed.

Lemma Permutation_map_dot_f2_rev :
  forall group,
    Permutation
      (map (fun row => dot TNull row q12_attr_f2) (List.rev group))
      (map (fun row => dot TNull row q12_attr_f2) group).
Proof.
  intro group.
  rewrite map_rev.
  apply Permutation_sym, Permutation_rev.
Qed.


Lemma q12_mode_pre_row_labels_shipmode :
  forall orders lines mode row,
    In row (q12_mode_pre_rows orders lines mode) ->
    q12_attr_shipmode inS labels TNull row.
Proof.
  intros orders lines mode row Hin.
  unfold q12_mode_pre_rows in Hin.
  apply in_flat_map in Hin; destruct Hin as [o [_ Hin]].
  apply in_map_iff in Hin; destruct Hin as [l [Heq _]].
  subst row; unfold q12_pre_group_row.
  apply q12_attr_inS_project_row; exact q12_case_select_outputs_shipmode.
Qed.

Lemma q12_group_env_dot_shipmode_of_mode_rows :
  forall orders lines mode,
    q12_mode_pre_rows orders lines mode <> nil ->
    Interp.interp_dot TNull
      (q12_group_env (List.rev (q12_mode_pre_rows orders lines mode)))
      q12_attr_shipmode =
    embed_string_char10 mode.
Proof.
  intros orders lines mode Hne.
  unfold q12_group_env, Env.env_g.
  rewrite (Interp.interp_dot_unfold TNull).
  destruct (ListSort.quicksort (OTuple TNull)
    (List.rev (q12_mode_pre_rows orders lines mode))) as [|t ts] eqn:Hqs.
  - pose proof (ListSort.length_quicksort (OTuple TNull)
      (List.rev (q12_mode_pre_rows orders lines mode))) as Hlen.
    rewrite Hqs in Hlen; cbn in Hlen.
    rewrite length_rev in Hlen.
    destruct (q12_mode_pre_rows orders lines mode) as [|r rs];
      [contradiction Hne; reflexivity|cbn in Hlen; discriminate Hlen].
  - assert (Hin : In t (q12_mode_pre_rows orders lines mode)).
    {
      assert (Hin_qs :
        In t (ListSort.quicksort (OTuple TNull)
          (List.rev (q12_mode_pre_rows orders lines mode)))).
      { rewrite Hqs; left; reflexivity. }
      rewrite <- (ListSort.In_quicksort (OTuple TNull)) in Hin_qs.
      apply in_rev; exact Hin_qs.
    }
    assert (Hlab : (q12_attr_shipmode inS? labels TNull t) = true).
    { exact (q12_mode_pre_row_labels_shipmode orders lines mode t Hin). }
    rewrite Hlab.
    exact (q12_mode_pre_row_dot_shipmode orders lines mode t Hin).
Qed.

(** * Group select values on rev(mode_pre_rows) = SpecOut columns *)

Lemma length_filter_map_const_b_negb :
  forall (A : Type) (xs : list A) (b : bool),
    (List.length (List.filter (fun x : bool => x) (map (fun _ => b) xs)) +
     List.length (List.filter (fun x : bool => x) (map (fun _ => negb b) xs)))%nat =
    List.length xs.
Proof.
  intros A xs b; destruct b.
  - rewrite length_filter_map_const_true, length_filter_map_const_false; lia.
  - rewrite length_filter_map_const_false, length_filter_map_const_true; lia.
Qed.

Lemma q12_mode_high_low_bits_count_sum :
  forall orders lines mode,
    (List.length (List.filter (fun b : bool => b)
      (q12_mode_high_bits orders lines mode)) +
     List.length (List.filter (fun b : bool => b)
      (q12_mode_low_bits orders lines mode)))%nat =
    List.length (q12_mode_pre_rows orders lines mode).
Proof.
  intros orders lines mode.
  unfold q12_mode_high_bits, q12_mode_low_bits, q12_mode_pre_rows.
  induction orders as [|o os IH]; [reflexivity|].
  cbn [flat_map].
  rewrite !filter_app, !length_app, length_map.
  set (b := q12_priority_is_high (o_priority o)).
  set (kept :=
    List.filter (fun l => SpecPairKeep o l && String.eqb (l_shipmode l) mode) lines).
  fold b kept.
  assert (Hhead :
    (List.length (List.filter (fun x : bool => x) (map (fun _ => b) kept)) +
     List.length (List.filter (fun x : bool => x) (map (fun _ => negb b) kept)))%nat =
    List.length kept)
    by (apply length_filter_map_const_b_negb).
  (* Reassociate head/tail sums and discharge via Hhead + IH *)
  lia.
Qed.

Lemma q12_mode_high_low_sum_eq_length :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    high (GetMode
      (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
      mode) +
    low (GetMode
      (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
      mode) =
    Z.of_nat (List.length (q12_mode_pre_rows orders lines mode)).
Proof.
  intros orders lines mode Huniq.
  rewrite (SpecView_high_eq_mode_high_bits_count orders lines mode Huniq).
  rewrite (SpecView_low_eq_mode_low_bits_count orders lines mode Huniq).
  rewrite <- Nat2Z.inj_add, q12_mode_high_low_bits_count_sum.
  reflexivity.
Qed.

Lemma q12_mode_pre_rows_nonempty_iff_finalize :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    q12_mode_pre_rows orders lines mode <> nil <->
    SpecFinalizeOne mode
      (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        mode) <> nil.
Proof.
  intros orders lines mode Huniq.
  pose proof (q12_mode_high_low_sum_eq_length orders lines mode Huniq) as Hsum.
  pose proof (SpecView_high_eq_mode_high_bits_count orders lines mode Huniq) as Hh.
  pose proof (SpecView_low_eq_mode_low_bits_count orders lines mode Huniq) as Hl.
  unfold SpecFinalizeOne.
  split; intro Hne.
  - assert (Hlen_pos :
      0 < Z.of_nat (List.length (q12_mode_pre_rows orders lines mode))).
    {
      destruct (q12_mode_pre_rows orders lines mode);
        [contradiction Hne; reflexivity|cbn; lia].
    }
    pose proof Hsum as Hsum'; rewrite <- Hsum' in Hlen_pos; clear Hsum'.
    destruct
      (high (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        mode) =? 0)%Z eqn:Eh;
      destruct
        (low (GetMode
          (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
          mode) =? 0)%Z eqn:El;
      cbn; try discriminate.
    apply Z.eqb_eq in Eh; apply Z.eqb_eq in El; lia.
  - destruct
      ((high (GetMode
          (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
          mode) =? 0)%Z &&
       (low (GetMode
          (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
          mode) =? 0)%Z) eqn:Ez.
    + contradiction Hne; reflexivity.
    + intro Hnil.
      rewrite Hnil in Hsum; cbn in Hsum.
      rewrite Hh, Hl in Hsum.
      apply andb_false_iff in Ez.
      destruct Ez as [Eh|El].
      * apply Z.eqb_neq in Eh; rewrite Hh in Eh; lia.
      * apply Z.eqb_neq in El; rewrite Hl in El; lia.
Qed.

Lemma q12_mode_bucket_safe :
  forall orders lines mode,
    Z.of_nat (List.length (q12_mode_pre_rows orders lines mode)) <= int64_max ->
    q12_group_bucket_safe (List.rev (q12_mode_pre_rows orders lines mode)).
Proof.
  intros orders lines mode Hlen.
  unfold q12_group_bucket_safe.
  split.
  - rewrite (sum_int32_runtime_error_permutation _
      (map (fun row => dot TNull row q12_attr_f1)
        (q12_mode_pre_rows orders lines mode))
      (Permutation_map_dot_f1_rev _)).
    rewrite q12_mode_pre_rows_f1_case_bits.
    apply sum_int32_runtime_error_case_bits_none.
    rewrite q12_mode_high_bits_length; exact Hlen.
  - rewrite (sum_int32_runtime_error_permutation _
      (map (fun row => dot TNull row q12_attr_f2)
        (q12_mode_pre_rows orders lines mode))
      (Permutation_map_dot_f2_rev _)).
    rewrite q12_mode_pre_rows_f2_case_bits.
    apply sum_int32_runtime_error_case_bits_none.
    rewrite q12_mode_low_bits_length; exact Hlen.
Qed.

Lemma q12_group_select_values_of_mode_rows :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    q12_mode_pre_rows orders lines mode <> nil ->
    Z.of_nat (List.length (q12_mode_pre_rows orders lines mode)) <= int64_max ->
    q12_group_select_values
      (List.rev (q12_mode_pre_rows orders lines mode)) =
    q12_group_values_of_SpecOutRow
      (mkSpecOutRow mode
        (high (GetMode
          (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
          mode))
        (low (GetMode
          (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
          mode))).
Proof.
  intros orders lines mode Huniq Hne Hlen.
  unfold q12_group_select_values, q12_group_values_of_SpecOutRow.
  assert (Hship :
    Interp.interp_dot TNull
      (q12_group_env (List.rev (q12_mode_pre_rows orders lines mode)))
      q12_attr_shipmode =
    embed_string_char10 mode)
    by (apply q12_group_env_dot_shipmode_of_mode_rows; exact Hne).
  assert (Hhigh :
    NullValues.interp_sum_int32_as_int64
      (map (fun row => dot TNull row q12_attr_f1)
        (List.rev (q12_mode_pre_rows orders lines mode))) =
    Value_int64 (Some (int64_from_twos_complement
      (high (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        mode))))).
  {
    rewrite (interp_sum_int32_as_int64_permutation _
      (map (fun row => dot TNull row q12_attr_f1)
        (q12_mode_pre_rows orders lines mode))
      (Permutation_map_dot_f1_rev _)).
    assert (Hne_bits : q12_mode_high_bits orders lines mode <> nil).
    {
      intro Hnil.
      apply Hne.
      pose proof (q12_mode_high_bits_length orders lines mode) as Hl.
      rewrite Hnil in Hl; cbn in Hl.
      destruct (q12_mode_pre_rows orders lines mode); [reflexivity|cbn in Hl; discriminate].
    }
    rewrite (q12_mode_sum_f1_eq_SpecView_high orders lines mode Huniq Hne_bits).
    2: { rewrite q12_mode_high_bits_length; exact Hlen. }
    apply value_int64_checked_of_range.
    pose proof (SpecView_high_eq_mode_high_bits_count orders lines mode Huniq) as Hh.
    rewrite Hh.
    pose proof (filter_length_le (q12_mode_high_bits orders lines mode)
      (fun b : bool => b)) as Hfilt.
    pose proof (q12_mode_high_bits_length orders lines mode) as Hblen.
    rewrite Hblen in Hfilt.
    split.
    + apply Z.le_trans with 0; [unfold int64_min; lia|apply Nat2Z.is_nonneg].
    + apply Z.le_trans with (Z.of_nat (List.length (q12_mode_pre_rows orders lines mode)));
        [|exact Hlen].
      apply Nat2Z.inj_le; exact Hfilt.
  }
  assert (Hlow :
    NullValues.interp_sum_int32_as_int64
      (map (fun row => dot TNull row q12_attr_f2)
        (List.rev (q12_mode_pre_rows orders lines mode))) =
    Value_int64 (Some (int64_from_twos_complement
      (low (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        mode))))).
  {
    rewrite (interp_sum_int32_as_int64_permutation _
      (map (fun row => dot TNull row q12_attr_f2)
        (q12_mode_pre_rows orders lines mode))
      (Permutation_map_dot_f2_rev _)).
    assert (Hne_bits : q12_mode_low_bits orders lines mode <> nil).
    {
      intro Hnil.
      apply Hne.
      pose proof (q12_mode_low_bits_length orders lines mode) as Hl.
      rewrite Hnil in Hl; cbn in Hl.
      destruct (q12_mode_pre_rows orders lines mode); [reflexivity|cbn in Hl; discriminate].
    }
    rewrite (q12_mode_sum_f2_eq_SpecView_low orders lines mode Huniq Hne_bits).
    2: { rewrite q12_mode_low_bits_length; exact Hlen. }
    apply value_int64_checked_of_range.
    pose proof (SpecView_low_eq_mode_low_bits_count orders lines mode Huniq) as Hl.
    rewrite Hl.
    pose proof (filter_length_le (q12_mode_low_bits orders lines mode)
      (fun b : bool => b)) as Hfilt.
    pose proof (q12_mode_low_bits_length orders lines mode) as Hblen.
    rewrite Hblen in Hfilt.
    split.
    + apply Z.le_trans with 0; [unfold int64_min; lia|apply Nat2Z.is_nonneg].
    + apply Z.le_trans with (Z.of_nat (List.length (q12_mode_pre_rows orders lines mode)));
        [|exact Hlen].
      apply Nat2Z.inj_le; exact Hfilt.
  }
  rewrite Hship, Hhigh, Hlow; reflexivity.
Qed.

Lemma q12_group_tuple_of_mode_rows :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    q12_mode_pre_rows orders lines mode <> nil ->
    Z.of_nat (List.length (q12_mode_pre_rows orders lines mode)) <= int64_max ->
    project_row Queries.scalar_select_list_1
      (q12_group_select_values
        (List.rev (q12_mode_pre_rows orders lines mode))) =
    q12_group_tuple_of_SpecOutRow
      (mkSpecOutRow mode
        (high (GetMode
          (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
          mode))
        (low (GetMode
          (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
          mode))).
Proof.
  intros orders lines mode Huniq Hne Hlen.
  unfold q12_group_tuple_of_SpecOutRow.
  rewrite q12_group_select_values_of_mode_rows; [reflexivity|assumption|assumption|assumption].
Qed.

(** * CollectModes ↔ mode buckets ↔ group rows *)

Definition q12_sum_bound (orders : list OrderRow) (lines : list LineRow) : Prop :=
  Z.of_nat (List.length (q12_pre_group_rows orders lines)) <= int64_max.

Lemma q12_mode_pre_rows_length_le_pre_group :
  forall orders lines mode,
    (List.length (q12_mode_pre_rows orders lines mode) <=
     List.length (q12_pre_group_rows orders lines))%nat.
Proof.
  intros orders lines mode.
  rewrite <- (filter_pre_group_eq_mode_rows orders lines mode).
  apply filter_length_le.
Qed.

Lemma q12_sum_bound_mode :
  forall orders lines mode,
    q12_sum_bound orders lines ->
    Z.of_nat (List.length (q12_mode_pre_rows orders lines mode)) <= int64_max.
Proof.
  intros orders lines mode Hb.
  unfold q12_sum_bound in Hb.
  pose proof (q12_mode_pre_rows_length_le_pre_group orders lines mode).
  lia.
Qed.

Lemma SpecFinalizeOne_eq_of_nonempty :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    q12_mode_pre_rows orders lines mode <> nil ->
    SpecFinalizeOne mode
      (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        mode) =
    [mkSpecOutRow mode
      (high (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        mode))
      (low (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        mode))].
Proof.
  intros orders lines mode Huniq Hne.
  pose proof (proj1 (q12_mode_pre_rows_nonempty_iff_finalize orders lines mode Huniq) Hne)
    as Hfin.
  unfold SpecFinalizeOne in *.
  destruct
    ((high (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        mode) =? 0)%Z &&
     (low (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        mode) =? 0)%Z) eqn:Ez.
  - contradiction Hfin; reflexivity.
  - reflexivity.
Qed.

Lemma SpecFinalizeOne_nil_of_empty :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    q12_mode_pre_rows orders lines mode = nil ->
    SpecFinalizeOne mode
      (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        mode) = nil.
Proof.
  intros orders lines mode Huniq Hnil.
  destruct (SpecFinalizeOne mode
    (GetMode
      (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
      mode)) as [|x xs] eqn:Ef; [reflexivity|].
  exfalso.
  assert (Hne_fin :
    SpecFinalizeOne mode
      (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        mode) <> nil)
    by (rewrite Ef; discriminate).
  apply (proj2 (q12_mode_pre_rows_nonempty_iff_finalize orders lines mode Huniq))
    in Hne_fin.
  rewrite Hnil in Hne_fin; contradiction Hne_fin; reflexivity.
Qed.

Lemma Q12_spec_group_outs_as_finalize :
  forall orders lines,
    Q12_spec_group_outs orders lines =
    List.app
      (SpecFinalizeOne "MAIL"
        (GetMode
          (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
          "MAIL"))
      (SpecFinalizeOne "SHIP"
        (GetMode
          (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
          "SHIP")).
Proof.
  intros orders lines.
  unfold Q12_spec_group_outs, CollectModes.
  reflexivity.
Qed.

Lemma q12_pre_group_row_mode_accepted :
  forall orders lines row,
    In row (q12_pre_group_rows orders lines) ->
    exists o l,
      In o orders /\ In l lines /\ SpecPairKeep o l = true /\
      row = q12_pre_group_row o l /\
      ModeAccepted (l_shipmode l) "MAIL" "SHIP" = true.
Proof.
  intros orders lines row Hin.
  unfold q12_pre_group_rows in Hin.
  apply in_flat_map in Hin; destruct Hin as [o [Ho Hin]].
  apply in_map_iff in Hin; destruct Hin as [l [Heq Hl]].
  apply filter_In in Hl; destruct Hl as [Hl Hkeep].
  exists o, l.
  pose proof Hkeep as Hkeep'.
  apply SpecPairKeep_iff in Hkeep'.
  repeat split; try assumption.
  - symmetry; exact Heq.
  - apply Hkeep'.
Qed.

Lemma q12_group_head_mode_accepted :
  forall orders lines group,
    In group (query_make_groups nil
      (q12_pre_group_rows orders lines) q12_group_terms) ->
    exists mode,
      (String.eqb mode "MAIL" || String.eqb mode "SHIP") = true /\
      group = List.rev (q12_mode_pre_rows orders lines mode) /\
      q12_mode_pre_rows orders lines mode <> nil.
Proof.
  intros orders lines group Hg.
  pose proof (query_make_groups_member_nonempty nil
    (q12_pre_group_rows orders lines) q12_group_terms group
    q12_group_terms_nonempty Hg) as Hne_g.
  destruct group as [|row rest]; [contradiction Hne_g; reflexivity|].
  assert (Hin_row : In row (q12_pre_group_rows orders lines)).
  {
    eapply query_make_groups_member_in; [exact Hg|left; reflexivity].
  }
  destruct (q12_pre_group_row_mode_accepted orders lines row Hin_row)
    as [o [l [Ho [Hl [Hkeep [Heqrow Hm]]]]]].
  set (mode := l_shipmode l).
  assert (Hkey :
    query_grouping_key nil q12_group_terms row = [embed_string_char10 mode]).
  {
    rewrite Heqrow, q12_pre_group_row_grouping_key; reflexivity.
  }
  pose proof (query_make_groups_member_exact_key_filter TNull nil
    (q12_pre_group_rows orders lines) q12_group_terms (row :: rest) row
    q12_group_terms_nonempty Hg (or_introl eq_refl)) as Hexact.
  rewrite Hkey in Hexact.
  rewrite filter_pre_group_eq_mode_rows in Hexact.
  exists mode; split; [|split].
  - exact Hm.
  - exact Hexact.
  - intro Hnil; rewrite Hnil in Hexact; discriminate.
Qed.

Lemma q12_group_tuple_of_mode_In_CollectModes :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    q12_sum_bound orders lines ->
    q12_mode_pre_rows orders lines mode <> nil ->
    (String.eqb mode "MAIL" || String.eqb mode "SHIP") = true ->
    In (q12_group_tuple_of_SpecOutRow
          (mkSpecOutRow mode
            (high (GetMode
              (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
              mode))
            (low (GetMode
              (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
              mode))))
      (map q12_group_tuple_of_SpecOutRow (Q12_spec_group_outs orders lines)).
Proof.
  intros orders lines mode Huniq Hb Hne Hm.
  apply in_map_iff.
  exists (mkSpecOutRow mode
    (high (GetMode
      (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP") mode))
    (low (GetMode
      (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP") mode))).
  split; [reflexivity|].
  rewrite Q12_spec_group_outs_as_finalize.
  apply orb_true_iff in Hm; destruct Hm as [Hm|Hm];
    apply String.eqb_eq in Hm; subst mode.
  - rewrite (SpecFinalizeOne_eq_of_nonempty orders lines "MAIL" Huniq Hne).
    left; reflexivity.
  - rewrite (SpecFinalizeOne_eq_of_nonempty orders lines "SHIP" Huniq Hne).
    apply in_or_app; right; left; reflexivity.
Qed.

Lemma q12_group_rows_In_of_mode :
  forall orders lines mode,
    UniqueOrderKeys orders ->
    q12_sum_bound orders lines ->
    q12_mode_pre_rows orders lines mode <> nil ->
    In (project_row Queries.scalar_select_list_1
          (q12_group_select_values
            (List.rev (q12_mode_pre_rows orders lines mode))))
      (q12_group_rows_of orders lines).
Proof.
  intros orders lines mode Huniq Hb Hne.
  unfold q12_group_rows_of.
  apply in_map_iff.
  exists (List.rev (q12_mode_pre_rows orders lines mode)).
  split; [reflexivity|].
  pose proof (q12_mode_bucket_of_pre_rows orders lines mode) as Hbkt.
  destruct (q12_mode_pre_rows orders lines mode) as [|r rs] eqn:Er;
    [contradiction Hne; reflexivity|].
  (* Hbkt is rewritten by the destruct to mention [rev (r::rs)] *)
  assert (Hin_f :
    In (List.rev (r :: rs))
      (List.filter
        (fun members =>
          match members with
          | nil => false
          | row :: _ =>
              Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
                (query_grouping_key nil q12_group_terms row)
                [embed_string_char10 mode]
          end)
        (query_make_groups nil (q12_pre_group_rows orders lines) q12_group_terms))).
  {
    rewrite Hbkt; left; reflexivity.
  }
  apply filter_In in Hin_f; apply Hin_f.
Qed.

Lemma q12_group_rows_In_iff_CollectModes :
  forall orders lines t,
    UniqueOrderKeys orders ->
    q12_sum_bound orders lines ->
    In t (q12_group_rows_of orders lines) <->
    In t (map q12_group_tuple_of_SpecOutRow (Q12_spec_group_outs orders lines)).
Proof.
  intros orders lines t Huniq Hb.
  split; intro Hin.
  - unfold q12_group_rows_of in Hin.
    apply in_map_iff in Hin; destruct Hin as [group [Heq Hg]].
    destruct (q12_group_head_mode_accepted orders lines group Hg)
      as [mode [Hm [Hg_eq Hne]]].
    subst group.
    rewrite <- Heq.
    rewrite (q12_group_tuple_of_mode_rows orders lines mode Huniq Hne
      (q12_sum_bound_mode orders lines mode Hb)).
    apply q12_group_tuple_of_mode_In_CollectModes; assumption.
  - apply in_map_iff in Hin; destruct Hin as [o [Heq Ho]].
    rewrite Q12_spec_group_outs_as_finalize in Ho.
    apply in_app_or in Ho; destruct Ho as [Ho|Ho].
    + (* MAIL *)
      destruct (q12_mode_pre_rows orders lines "MAIL") as [|r rs] eqn:Er.
      * rewrite (SpecFinalizeOne_nil_of_empty orders lines "MAIL" Huniq Er) in Ho;
          contradiction.
      * assert (Hne : q12_mode_pre_rows orders lines "MAIL" <> nil)
          by (rewrite Er; discriminate).
        rewrite (SpecFinalizeOne_eq_of_nonempty orders lines "MAIL" Huniq Hne) in Ho.
        destruct Ho as [Ho|[]]; subst o.
        rewrite <- Heq.
        rewrite <- (q12_group_tuple_of_mode_rows orders lines "MAIL" Huniq Hne
          (q12_sum_bound_mode orders lines "MAIL" Hb)).
        apply q12_group_rows_In_of_mode; [exact Huniq|exact Hb|exact Hne].
    + (* SHIP *)
      destruct (q12_mode_pre_rows orders lines "SHIP") as [|r rs] eqn:Er.
      * rewrite (SpecFinalizeOne_nil_of_empty orders lines "SHIP" Huniq Er) in Ho;
          contradiction.
      * assert (Hne : q12_mode_pre_rows orders lines "SHIP" <> nil)
          by (rewrite Er; discriminate).
        rewrite (SpecFinalizeOne_eq_of_nonempty orders lines "SHIP" Huniq Hne) in Ho.
        destruct Ho as [Ho|[]]; subst o.
        rewrite <- Heq.
        rewrite <- (q12_group_tuple_of_mode_rows orders lines "SHIP" Huniq Hne
          (q12_sum_bound_mode orders lines "SHIP" Hb)).
        apply q12_group_rows_In_of_mode; [exact Huniq|exact Hb|exact Hne].
Qed.

Lemma q12_group_tuple_shipmode_inj :
  forall o1 o2,
    q12_group_tuple_of_SpecOutRow o1 = q12_group_tuple_of_SpecOutRow o2 ->
    out_shipmode o1 = out_shipmode o2.
Proof.
  intros o1 o2 Heq.
  pose proof (q12_group_tuple_dot_shipmode o1) as H1.
  pose proof (q12_group_tuple_dot_shipmode o2) as H2.
  rewrite Heq in H1.
  rewrite H1 in H2.
  apply embed_string_char10_inj in H2; exact H2.
Qed.

Lemma CollectModes_embed_NoDup :
  forall orders lines,
    NoDup (map q12_group_tuple_of_SpecOutRow (Q12_spec_group_outs orders lines)).
Proof.
  intros orders lines.
  rewrite Q12_spec_group_outs_as_finalize.
  unfold SpecFinalizeOne.
  destruct
    ((high (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        "MAIL") =? 0)%Z &&
     (low (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        "MAIL") =? 0)%Z) eqn:Em;
  destruct
    ((high (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        "SHIP") =? 0)%Z &&
     (low (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        "SHIP") =? 0)%Z) eqn:Es;
  cbn [List.app map].
  - constructor.
  - constructor; [intros H; destruct H|constructor].
  - constructor; [intros H; destruct H|constructor].
  - constructor.
    + intros [Heq|H]; [|destruct H].
      apply q12_group_tuple_shipmode_inj in Heq; discriminate.
    + constructor; [intros H; destruct H|constructor].
Qed.



Definition q12_mode_bucket_list
    (orders : list OrderRow) (lines : list LineRow) (mode : string)
  : list (list (tuple TNull)) :=
  match q12_mode_pre_rows orders lines mode with
  | nil => nil
  | _ :: _ => [List.rev (q12_mode_pre_rows orders lines mode)]
  end.

Definition q12_mode_buckets
    (orders : list OrderRow) (lines : list LineRow)
  : list (list (tuple TNull)) :=
  List.app (q12_mode_bucket_list orders lines "MAIL")
           (q12_mode_bucket_list orders lines "SHIP").

Lemma q12_mode_pre_rows_MAIL_SHIP_disjoint :
  forall orders lines,
    q12_mode_pre_rows orders lines "MAIL" <> nil ->
    q12_mode_pre_rows orders lines "SHIP" <> nil ->
    List.rev (q12_mode_pre_rows orders lines "MAIL") <>
    List.rev (q12_mode_pre_rows orders lines "SHIP").
Proof.
  intros orders lines HneM HneS Heq.
  apply (f_equal (@List.rev (tuple TNull))) in Heq.
  rewrite !rev_involutive in Heq.
  destruct (q12_mode_pre_rows orders lines "MAIL") as [|rm rms] eqn:Em;
    [contradiction HneM; reflexivity|].
  destruct (q12_mode_pre_rows orders lines "SHIP") as [|rs rss] eqn:Es;
    [contradiction HneS; reflexivity|].
  injection Heq as Hrow _.
  assert (HinM : In rm (q12_mode_pre_rows orders lines "MAIL"))
    by (rewrite Em; left; reflexivity).
  assert (HinS : In rm (q12_mode_pre_rows orders lines "SHIP"))
    by (rewrite Es, Hrow; left; reflexivity).
  pose proof (q12_mode_pre_row_dot_shipmode orders lines "MAIL" rm HinM) as Hm.
  pose proof (q12_mode_pre_row_dot_shipmode orders lines "SHIP" rm HinS) as Hs.
  rewrite Hm in Hs.
  apply embed_string_char10_inj in Hs; discriminate.
Qed.

Lemma q12_mode_buckets_NoDup :
  forall orders lines,
    NoDup (q12_mode_buckets orders lines).
Proof.
  intros orders lines.
  unfold q12_mode_buckets, q12_mode_bucket_list.
  destruct (q12_mode_pre_rows orders lines "MAIL") as [|rm rms] eqn:Em;
    destruct (q12_mode_pre_rows orders lines "SHIP") as [|rs rss] eqn:Es;
    cbn [List.app].
  - constructor.
  - constructor; [intros H; destruct H|constructor].
  - constructor; [intros H; destruct H|constructor].
  - constructor.
    + intros [Heq|H]; [|destruct H].
      assert (HneM : q12_mode_pre_rows orders lines "MAIL" <> nil)
        by (rewrite Em; discriminate).
      assert (HneS : q12_mode_pre_rows orders lines "SHIP" <> nil)
        by (rewrite Es; discriminate).
      pose proof (q12_mode_pre_rows_MAIL_SHIP_disjoint orders lines HneM HneS) as Hdis.
      rewrite Em, Es in Hdis.
      congruence.
    + constructor; [intros H; destruct H|constructor].
Qed.

Lemma q12_make_groups_incl_mode_buckets :
  forall orders lines,
    incl
      (query_make_groups nil
        (q12_pre_group_rows orders lines) q12_group_terms)
      (q12_mode_buckets orders lines).
Proof.
  intros orders lines g Hg.
  unfold q12_mode_buckets, q12_mode_bucket_list.
  destruct (q12_group_head_mode_accepted orders lines g Hg)
    as [mode [Hm [Heq Hne]]].
  apply orb_true_iff in Hm; destruct Hm as [Hm|Hm];
    apply String.eqb_eq in Hm; subst mode.
  - destruct (q12_mode_pre_rows orders lines "MAIL") as [|r rs] eqn:Er;
      [contradiction Hne; reflexivity|].
    rewrite Heq; left; reflexivity.
  - destruct (q12_mode_pre_rows orders lines "SHIP") as [|r rs] eqn:Er;
      [contradiction Hne; reflexivity|].
    rewrite Heq; apply in_or_app; right; left; reflexivity.
Qed.

Lemma q12_mode_buckets_incl_make_groups :
  forall orders lines,
    incl
      (q12_mode_buckets orders lines)
      (query_make_groups nil
        (q12_pre_group_rows orders lines) q12_group_terms).
Proof.
  intros orders lines g Hg.
  unfold q12_mode_buckets, q12_mode_bucket_list in Hg.
  apply in_app_or in Hg; destruct Hg as [Hg|Hg].
  - destruct (q12_mode_pre_rows orders lines "MAIL") as [|r rs] eqn:Er;
      [contradiction Hg|].
    destruct Hg as [Hg|[]]; subst g.
    pose proof (q12_mode_bucket_of_pre_rows orders lines "MAIL") as Hbkt.
    rewrite Er in Hbkt.
    assert (Hin_f :
      In (List.rev (r :: rs))
        (List.filter
          (fun members =>
            match members with
            | nil => false
            | row :: _ =>
                Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
                  (query_grouping_key nil q12_group_terms row)
                  [embed_string_char10 "MAIL"]
            end)
          (query_make_groups nil
            (q12_pre_group_rows orders lines) q12_group_terms))).
    { rewrite Hbkt; left; reflexivity. }
    apply filter_In in Hin_f; apply Hin_f.
  - destruct (q12_mode_pre_rows orders lines "SHIP") as [|r rs] eqn:Er;
      [contradiction Hg|].
    destruct Hg as [Hg|[]]; subst g.
    pose proof (q12_mode_bucket_of_pre_rows orders lines "SHIP") as Hbkt.
    rewrite Er in Hbkt.
    assert (Hin_f :
      In (List.rev (r :: rs))
        (List.filter
          (fun members =>
            match members with
            | nil => false
            | row :: _ =>
                Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
                  (query_grouping_key nil q12_group_terms row)
                  [embed_string_char10 "SHIP"]
            end)
          (query_make_groups nil
            (q12_pre_group_rows orders lines) q12_group_terms))).
    { rewrite Hbkt; left; reflexivity. }
    apply filter_In in Hin_f; apply Hin_f.
Qed.

Lemma filter_eq_self_of_forall :
  forall {A : Type} (p : A -> bool) (xs : list A),
    (forall x, In x xs -> p x = true) ->
    List.filter p xs = xs.
Proof.
  intros A p xs Hall.
  induction xs as [|x xs IH]; [reflexivity|].
  cbn [filter].
  rewrite (Hall x (or_introl eq_refl)).
  f_equal; apply IH; intros y Hy; apply Hall; right; exact Hy.
Qed.

Lemma length_filter_or_disjoint :
  forall {A : Type} (p q : A -> bool) (xs : list A),
    (forall x, In x xs -> p x = true \/ q x = true) ->
    (forall x, In x xs -> p x = true -> q x = true -> False) ->
    (List.length (List.filter p xs) + List.length (List.filter q xs))%nat =
    List.length xs.
Proof.
  intros A p q xs Hor Hdisj.
  induction xs as [|x xs IH]; [reflexivity|].
  cbn [filter].
  assert (Hin : In x (x :: xs)) by (left; reflexivity).
  assert (IH' :
    (List.length (List.filter p xs) + List.length (List.filter q xs))%nat =
    List.length xs).
  {
    apply IH.
    - intros y Hy; apply Hor; right; exact Hy.
    - intros y Hy; apply Hdisj; right; exact Hy.
  }
  destruct (Hor x Hin) as [Hp|Hq].
  - rewrite Hp.
    destruct (q x) eqn:Hq.
    + exfalso; apply (Hdisj x Hin Hp Hq).
    + cbn [List.length]; lia.
  - destruct (p x) eqn:Hp.
    + exfalso; apply (Hdisj x Hin Hp Hq).
    + rewrite Hq; cbn [List.length]; lia.
Qed.

Lemma q12_group_key_is_mode :
  forall orders lines g mode,
    In g (query_make_groups nil
      (q12_pre_group_rows orders lines) q12_group_terms) ->
    g = List.rev (q12_mode_pre_rows orders lines mode) ->
    match g with
    | nil => false
    | row :: _ =>
        Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
          (query_grouping_key nil q12_group_terms row)
          [embed_string_char10 mode]
    end = true.
Proof.
  intros orders lines g mode Hg Heq.
  pose proof (query_make_groups_member_nonempty nil
    (q12_pre_group_rows orders lines) q12_group_terms g
    q12_group_terms_nonempty Hg) as Hne.
  destruct g as [|row rest]; [contradiction Hne; reflexivity|].
  cbn.
  assert (Hin_rev : In row (List.rev (q12_mode_pre_rows orders lines mode))).
  { rewrite <- Heq; left; reflexivity. }
  apply in_rev in Hin_rev.
  unfold q12_mode_pre_rows in Hin_rev.
  apply in_flat_map in Hin_rev; destruct Hin_rev as [o [_ Hin]].
  apply in_map_iff in Hin; destruct Hin as [l [Heql Hl]].
  apply filter_In in Hl; destruct Hl as [_ Hkeep].
  apply andb_true_iff in Hkeep; destruct Hkeep as [_ Hm].
  apply String.eqb_eq in Hm.
  replace row with (q12_pre_group_row o l) by (exact Heql).
  change (
    Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
      (query_grouping_key nil q12_group_terms (q12_pre_group_row o l))
      [embed_string_char10 mode] = true).
  rewrite q12_pre_group_row_grouping_key, Hm.
  apply Oset.eq_bool_true_iff; reflexivity.
Qed.

Lemma q12_make_groups_length_eq_mode_buckets :
  forall orders lines,
    List.length
      (query_make_groups nil
        (q12_pre_group_rows orders lines) q12_group_terms) =
    List.length (q12_mode_buckets orders lines).
Proof.
  intros orders lines.
  set (gs := query_make_groups nil
    (q12_pre_group_rows orders lines) q12_group_terms).
  set (pM := fun members : list (tuple TNull) =>
    match members with
    | nil => false
    | row :: _ =>
        Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
          (query_grouping_key nil q12_group_terms row)
          [embed_string_char10 "MAIL"]
    end).
  set (pS := fun members : list (tuple TNull) =>
    match members with
    | nil => false
    | row :: _ =>
        Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
          (query_grouping_key nil q12_group_terms row)
          [embed_string_char10 "SHIP"]
    end).
  assert (Hor : forall g, In g gs -> pM g = true \/ pS g = true).
  {
    intros g Hg.
    destruct (q12_group_head_mode_accepted orders lines g Hg)
      as [mode [Hm [Heq _]]].
    apply orb_true_iff in Hm; destruct Hm as [Hm|Hm].
    - apply String.eqb_eq in Hm; subst mode; left.
      unfold pM; eapply q12_group_key_is_mode; [exact Hg|exact Heq].
    - apply String.eqb_eq in Hm; subst mode; right.
      unfold pS; eapply q12_group_key_is_mode; [exact Hg|exact Heq].
  }
  assert (Hdisj : forall g, In g gs -> pM g = true -> pS g = true -> False).
  {
    intros g _ Hm Hs; unfold pM, pS in Hm, Hs.
    destruct g as [|row rest]; [discriminate Hm|].
    apply Oset.eq_bool_true_iff in Hm.
    apply Oset.eq_bool_true_iff in Hs.
    rewrite Hm in Hs.
    apply (f_equal (fun vs : list NullValues.value =>
      match vs with
      | v :: _ => v
      | nil => embed_string_char10 ""
      end)) in Hs.
    apply embed_string_char10_inj in Hs; discriminate.
  }
  pose proof (length_filter_or_disjoint pM pS gs Hor Hdisj) as Hsum.
  pose proof (q12_mode_bucket_of_pre_rows orders lines "MAIL") as HM.
  pose proof (q12_mode_bucket_of_pre_rows orders lines "SHIP") as HS.
  assert (HM' : List.filter pM gs =
    match q12_mode_pre_rows orders lines "MAIL" with
    | nil => nil
    | _ :: _ => [List.rev (q12_mode_pre_rows orders lines "MAIL")]
    end).
  {
    clear Hsum Hor Hdisj HS.
    unfold gs, pM; exact HM.
  }
  assert (HS' : List.filter pS gs =
    match q12_mode_pre_rows orders lines "SHIP" with
    | nil => nil
    | _ :: _ => [List.rev (q12_mode_pre_rows orders lines "SHIP")]
    end).
  {
    clear Hsum Hor Hdisj HM HM'.
    unfold gs, pS; exact HS.
  }
  clear HM HS.
  change (List.length gs =
    List.length (q12_mode_buckets orders lines)).
  unfold q12_mode_buckets, q12_mode_bucket_list.
  destruct (q12_mode_pre_rows orders lines "MAIL") as [|m0 mrest];
    destruct (q12_mode_pre_rows orders lines "SHIP") as [|s0 srest];
    cbn [List.app List.length] in HM', HS' |- *;
    rewrite HM', HS' in Hsum;
    cbn [List.length Nat.add] in Hsum;
    symmetry; exact Hsum.
Qed.

Lemma q12_make_groups_NoDup :
  forall orders lines,
    NoDup
      (query_make_groups nil
        (q12_pre_group_rows orders lines) q12_group_terms).
Proof.
  intros orders lines.
  eapply NoDup_incl_NoDup.
  - apply q12_mode_buckets_NoDup.
  - rewrite (q12_make_groups_length_eq_mode_buckets orders lines).
    apply Nat.le_refl.
  - apply q12_mode_buckets_incl_make_groups.
Qed.

Lemma q12_make_groups_perm_mode_buckets :
  forall orders lines,
    Permutation
      (query_make_groups nil
        (q12_pre_group_rows orders lines) q12_group_terms)
      (q12_mode_buckets orders lines).
Proof.
  intros orders lines.
  apply NoDup_Permutation.
  - apply q12_make_groups_NoDup.
  - apply q12_mode_buckets_NoDup.
  - intro g; split.
    + apply q12_make_groups_incl_mode_buckets.
    + apply q12_mode_buckets_incl_make_groups.
Qed.

Lemma q12_group_rows_of_mode_buckets :
  forall orders lines,
    UniqueOrderKeys orders ->
    q12_sum_bound orders lines ->
    map (fun group =>
      project_row Queries.scalar_select_list_1
        (q12_group_select_values group))
      (q12_mode_buckets orders lines) =
    map q12_group_tuple_of_SpecOutRow (Q12_spec_group_outs orders lines).
Proof.
  intros orders lines Huniq Hb.
  unfold q12_mode_buckets, q12_mode_bucket_list.
  rewrite Q12_spec_group_outs_as_finalize, !map_app.
  f_equal.
  - destruct (q12_mode_pre_rows orders lines "MAIL") as [|r rs] eqn:Er.
    + rewrite (SpecFinalizeOne_nil_of_empty orders lines "MAIL" Huniq Er); reflexivity.
    + assert (Hne : q12_mode_pre_rows orders lines "MAIL" <> nil)
        by (rewrite Er; discriminate).
      rewrite (SpecFinalizeOne_eq_of_nonempty orders lines "MAIL" Huniq Hne).
      cbn [map].
      f_equal.
      rewrite <- Er.
      apply q12_group_tuple_of_mode_rows; [exact Huniq|exact Hne|].
      apply q12_sum_bound_mode; exact Hb.
  - destruct (q12_mode_pre_rows orders lines "SHIP") as [|r rs] eqn:Er.
    + rewrite (SpecFinalizeOne_nil_of_empty orders lines "SHIP" Huniq Er); reflexivity.
    + assert (Hne : q12_mode_pre_rows orders lines "SHIP" <> nil)
        by (rewrite Er; discriminate).
      rewrite (SpecFinalizeOne_eq_of_nonempty orders lines "SHIP" Huniq Hne).
      cbn [map].
      f_equal.
      rewrite <- Er.
      apply q12_group_tuple_of_mode_rows; [exact Huniq|exact Hne|].
      apply q12_sum_bound_mode; exact Hb.
Qed.

Lemma q12_group_rows_perm_CollectModes_embed :
  forall orders lines,
    UniqueOrderKeys orders ->
    q12_sum_bound orders lines ->
    Permutation
      (q12_group_rows_of orders lines)
      (map q12_group_tuple_of_SpecOutRow
        (Q12_spec_group_outs orders lines)).
Proof.
  intros orders lines Huniq Hb.
  unfold q12_group_rows_of.
  rewrite <- (q12_group_rows_of_mode_buckets orders lines Huniq Hb).
  apply Permutation_map.
  apply q12_make_groups_perm_mode_buckets.
Qed.

(** * Sorted bag equality via SortedGroupResultFacade *)

Definition q12_tuple_le (a b : tuple TNull) : bool :=
  match @compare_order_keys TNull q12_value_is_null q12_order_keys a b with
  | Eq | Lt => true
  | Gt => false
  end.

Lemma string_compare_eq_String_compare :
  forall s1 s2, string_compare s1 s2 = String.compare s1 s2.
Proof.
  induction s1 as [|a1 s1 IH]; intros [|a2 s2]; [reflexivity|reflexivity|reflexivity|].
  cbn [string_compare String.compare].
  change (N.compare (N_of_ascii a1) (N_of_ascii a2)) with (Ascii.compare a1 a2).
  destruct (Ascii.compare a1 a2); [apply IH|reflexivity|reflexivity].
Qed.

Lemma compare_order_keys_group_tuple_String_compare :
  forall o1 o2,
    @compare_order_keys TNull q12_value_is_null q12_order_keys
      (q12_group_tuple_of_SpecOutRow o1)
      (q12_group_tuple_of_SpecOutRow o2) =
    String.compare (out_shipmode o1) (out_shipmode o2).
Proof.
  intros o1 o2.
  cbn [compare_order_keys q12_order_keys].
  unfold SortAscNullsLast, SortKey, compare_order_key, compare_order_value.
  rewrite !q12_group_tuple_dot_shipmode.
  unfold embed_string_char10, q12_value_is_null.
  cbn [NullValues.is_null_value].
  unfold sort_key_value_compare, NullValues.sql_order_value_compare.
  unfold NullValues.value_compare.
  cbn.
  rewrite string_compare_eq_String_compare.
  destruct (String.compare (out_shipmode o1) (out_shipmode o2)); reflexivity.
Qed.

Lemma q12_tuple_le_of_String_compare :
  forall o1 o2,
    q12_tuple_le
      (q12_group_tuple_of_SpecOutRow o1)
      (q12_group_tuple_of_SpecOutRow o2) =
    match String.compare (out_shipmode o1) (out_shipmode o2) with
    | Gt => false
    | _ => true
    end.
Proof.
  intros o1 o2.
  unfold q12_tuple_le.
  rewrite compare_order_keys_group_tuple_String_compare.
  destruct (String.compare (out_shipmode o1) (out_shipmode o2)); reflexivity.
Qed.

Lemma SpecOutModeLe_trans :
  forall o1 o2 o3,
    match String.compare (out_shipmode o1) (out_shipmode o2) with
    | Gt => false | _ => true end = true ->
    match String.compare (out_shipmode o2) (out_shipmode o3) with
    | Gt => false | _ => true end = true ->
    match String.compare (out_shipmode o1) (out_shipmode o3) with
    | Gt => false | _ => true end = true.
Proof.
  intros o1 o2 o3 H12 H23.
  rewrite <- !string_compare_eq_String_compare in H12, H23 |- *.
  destruct (string_compare (out_shipmode o1) (out_shipmode o2)) eqn:E12;
    try discriminate H12.
  - pose proof (proj1 (Oset.compare_eq_iff Ostring _ _) E12) as Heq.
    rewrite Heq; exact H23.
  - destruct (string_compare (out_shipmode o2) (out_shipmode o3)) eqn:E23;
      try discriminate H23.
    + pose proof (proj1 (Oset.compare_eq_iff Ostring _ _) E23) as Heq.
      rewrite <- Heq, E12; reflexivity.
    + change (Oset.compare Ostring (out_shipmode o1) (out_shipmode o2) = Lt) in E12.
      change (Oset.compare Ostring (out_shipmode o2) (out_shipmode o3) = Lt) in E23.
      pose proof (Oset.compare_lt_trans Ostring _ _ _ E12 E23) as E13.
      change (match Oset.compare Ostring (out_shipmode o1) (out_shipmode o3) with
              | Gt => false | _ => true end = true).
      rewrite E13; reflexivity.
Qed.

Lemma SpecOutModeLe_antisym :
  forall o1 o2,
    match String.compare (out_shipmode o1) (out_shipmode o2) with
    | Gt => false | _ => true end = true ->
    match String.compare (out_shipmode o2) (out_shipmode o1) with
    | Gt => false | _ => true end = true ->
    out_shipmode o1 = out_shipmode o2.
Proof.
  intros o1 o2 H12 H21.
  rewrite <- !string_compare_eq_String_compare in H12, H21.
  destruct (string_compare (out_shipmode o1) (out_shipmode o2)) eqn:E12;
    try discriminate H12.
  - apply (proj1 (Oset.compare_eq_iff Ostring _ _) E12).
  - change (Oset.compare Ostring (out_shipmode o1) (out_shipmode o2) = Lt) in E12.
    pose proof (Oset.compare_lt_gt Ostring (out_shipmode o1) (out_shipmode o2)) as Hgt.
    rewrite E12 in Hgt.
    (* Hgt : Lt = CompOpp (compare o2 o1) *)
    assert (E21 : Oset.compare Ostring (out_shipmode o2) (out_shipmode o1) = Gt).
    {
      destruct (Oset.compare Ostring (out_shipmode o2) (out_shipmode o1));
        cbn in Hgt; try discriminate Hgt; reflexivity.
    }
    change (string_compare (out_shipmode o2) (out_shipmode o1) = Gt) in E21.
    rewrite E21 in H21; discriminate H21.
Qed.

Lemma q12_group_rows_le_trans_on :
  forall orders lines,
    UniqueOrderKeys orders ->
    q12_sum_bound orders lines ->
    @leA_trans_on (tuple TNull)
      (@order_keys_le TNull q12_value_is_null q12_order_keys)
      (q12_group_rows_of orders lines).
Proof.
  intros orders lines Huniq Hb x y z Hx Hy Hz Hxy Hyz.
  pose proof (q12_group_rows_In_iff_CollectModes orders lines x Huniq Hb) as Ix.
  pose proof (q12_group_rows_In_iff_CollectModes orders lines y Huniq Hb) as Iy.
  pose proof (q12_group_rows_In_iff_CollectModes orders lines z Huniq Hb) as Iz.
  apply (proj1 Ix) in Hx; apply in_map_iff in Hx;
    destruct Hx as [o1 [He1 _]].
  apply (proj1 Iy) in Hy; apply in_map_iff in Hy;
    destruct Hy as [o2 [He2 _]].
  apply (proj1 Iz) in Hz; apply in_map_iff in Hz;
    destruct Hz as [o3 [He3 _]].
  subst x y z.
  change (q12_tuple_le (q12_group_tuple_of_SpecOutRow o1)
    (q12_group_tuple_of_SpecOutRow o2) = true) in Hxy.
  change (q12_tuple_le (q12_group_tuple_of_SpecOutRow o2)
    (q12_group_tuple_of_SpecOutRow o3) = true) in Hyz.
  change (q12_tuple_le (q12_group_tuple_of_SpecOutRow o1)
    (q12_group_tuple_of_SpecOutRow o3) = true).
  rewrite q12_tuple_le_of_String_compare in Hxy, Hyz |- *.
  eapply SpecOutModeLe_trans; eassumption.
Qed.

Lemma CollectModes_shipmode_cases :
  forall orders lines o,
    In o (Q12_spec_group_outs orders lines) ->
    out_shipmode o = "MAIL" \/ out_shipmode o = "SHIP".
Proof.
  intros orders lines o Hin.
  rewrite Q12_spec_group_outs_as_finalize in Hin.
  unfold SpecFinalizeOne in Hin.
  destruct
    ((high (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        "MAIL") =? 0)%Z &&
     (low (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        "MAIL") =? 0)%Z) eqn:Em;
  destruct
    ((high (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        "SHIP") =? 0)%Z &&
     (low (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        "SHIP") =? 0)%Z) eqn:Es;
  cbn [List.app] in Hin.
  - contradiction Hin.
  - destruct Hin as [Heq|[]]; inversion Heq; subst; right; reflexivity.
  - destruct Hin as [Heq|[]]; inversion Heq; subst; left; reflexivity.
  - destruct Hin as [Heq|Hin'].
    + inversion Heq; subst; left; reflexivity.
    + destruct Hin' as [Heq|[]]; inversion Heq; subst; right; reflexivity.
Qed.

Lemma q12_group_rows_order_keys_antisym :
  forall orders lines x y,
    UniqueOrderKeys orders ->
    q12_sum_bound orders lines ->
    In x (q12_group_rows_of orders lines) ->
    In y (q12_group_rows_of orders lines) ->
    @OrderKeysLe TNull q12_value_is_null q12_order_keys x y ->
    @OrderKeysLe TNull q12_value_is_null q12_order_keys y x ->
    x = y.
Proof.
  intros orders lines x y Huniq Hb Hx Hy Hab Hba.
  pose proof (q12_group_rows_In_iff_CollectModes orders lines x Huniq Hb) as Ix.
  pose proof (q12_group_rows_In_iff_CollectModes orders lines y Huniq Hb) as Iy.
  apply (proj1 Ix) in Hx; apply in_map_iff in Hx;
    destruct Hx as [o1 [He1 Hin1]].
  apply (proj1 Iy) in Hy; apply in_map_iff in Hy;
    destruct Hy as [o2 [He2 Hin2]].
  subst x y.
  unfold OrderKeysLe, order_keys_le in Hab, Hba.
  change (q12_tuple_le (q12_group_tuple_of_SpecOutRow o1)
    (q12_group_tuple_of_SpecOutRow o2) = true) in Hab.
  change (q12_tuple_le (q12_group_tuple_of_SpecOutRow o2)
    (q12_group_tuple_of_SpecOutRow o1) = true) in Hba.
  rewrite q12_tuple_le_of_String_compare in Hab, Hba.
  pose proof (SpecOutModeLe_antisym o1 o2 Hab Hba) as Hmode.
  rewrite Q12_spec_group_outs_as_finalize in Hin1, Hin2.
  unfold SpecFinalizeOne in Hin1, Hin2.
  destruct
    ((high (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        "MAIL") =? 0)%Z &&
     (low (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        "MAIL") =? 0)%Z) eqn:Em;
  destruct
    ((high (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        "SHIP") =? 0)%Z &&
     (low (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        "SHIP") =? 0)%Z) eqn:Es;
  cbn [List.app] in Hin1, Hin2.
  - contradiction Hin1.
  - destruct Hin1 as [H1|[]]; destruct Hin2 as [H2|[]]; subst; reflexivity.
  - destruct Hin1 as [H1|[]]; destruct Hin2 as [H2|[]]; subst; reflexivity.
  - destruct Hin1 as [Heq1|Hin1']; destruct Hin2 as [Heq2|Hin2'].
    + inversion Heq1; inversion Heq2; subst; reflexivity.
    + destruct Hin2' as [Heq2'|[]]; inversion Heq1; inversion Heq2'; subst;
      simpl in Hmode; discriminate.
    + destruct Hin1' as [Heq1'|[]]; inversion Heq1'; inversion Heq2; subst;
      simpl in Hmode; discriminate.
    + destruct Hin1' as [Heq1'|[]]; destruct Hin2' as [Heq2'|[]];
      inversion Heq1'; inversion Heq2'; subst; reflexivity.
Qed.

Definition Q12_GroupBagsPerm (orders : list OrderRow) (lines : list LineRow) : Prop :=
  @sort_ordered_rows TNull q12_value_is_null q12_order_keys
    (q12_group_rows_of orders lines) =
  @sort_ordered_rows TNull q12_value_is_null q12_order_keys
    (map q12_group_tuple_of_SpecOutRow (Q12_spec_group_outs orders lines)).

Lemma Q12_GroupBagsPerm_holds :
  forall orders lines,
    UniqueOrderKeys orders ->
    q12_sum_bound orders lines ->
    Q12_GroupBagsPerm orders lines.
Proof.
  intros orders lines Huniq Hb.
  unfold Q12_GroupBagsPerm.
  eapply (@tnull_sort_ordered_rows_eq_of_perm q12_order_keys).
  - apply q12_group_rows_perm_CollectModes_embed; assumption.
  - exact (q12_group_rows_le_trans_on orders lines Huniq Hb).
  - intros x y Hx Hy Hab Hba.
    apply (q12_group_rows_order_keys_antisym orders lines x y
      Huniq Hb Hx Hy Hab Hba).
Qed.

(** CollectModes has ≤2 distinct mode rows: sort coincides with SortByMode. *)
Lemma sort_ordered_rows_map_SortByMode_group_outs :
  forall orders lines,
    @sort_ordered_rows TNull q12_value_is_null q12_order_keys
      (map q12_group_tuple_of_SpecOutRow (Q12_spec_group_outs orders lines)) =
    map q12_group_tuple_of_SpecOutRow
      (SortByMode (Q12_spec_group_outs orders lines)).
Proof.
  intros orders lines.
  rewrite Q12_spec_group_outs_as_finalize.
  unfold SpecFinalizeOne.
  destruct
    ((high (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        "MAIL") =? 0)%Z &&
     (low (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        "MAIL") =? 0)%Z) eqn:Em;
  destruct
    ((high (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        "SHIP") =? 0)%Z &&
     (low (GetMode
        (SpecView orders lines q12_catalog_start q12_catalog_end "MAIL" "SHIP")
        "SHIP") =? 0)%Z) eqn:Es;
  cbn [List.app map SortByMode InsertSortedStr sort_ordered_rows insert_ordered_row].
  - reflexivity.
  - reflexivity.
  - reflexivity.
  - (* [MAIL; SHIP]: MAIL < SHIP *)
    rewrite compare_order_keys_group_tuple_String_compare.
    cbn [out_shipmode].
    assert (Hc : String.compare "MAIL" "SHIP" = Lt) by reflexivity.
    rewrite Hc.
    reflexivity.
Qed.

Definition Q12_GroupsCorrespond (orders : list OrderRow) (lines : list LineRow) : Prop :=
  q12_ordered_rows_of orders lines =
  map q12_group_tuple_of_SpecOutRow
    (SortByMode (Q12_spec_group_outs orders lines)).

Lemma Q12_GroupsCorrespond_holds :
  forall orders lines,
    UniqueOrderKeys orders ->
    q12_sum_bound orders lines ->
    Q12_GroupsCorrespond orders lines.
Proof.
  intros orders lines Huniq Hb.
  unfold Q12_GroupsCorrespond, q12_ordered_rows_of.
  rewrite (Q12_GroupBagsPerm_holds orders lines Huniq Hb).
  apply sort_ordered_rows_map_SortByMode_group_outs.
Qed.

Lemma q12_final_rows_eq_EmbedResult :
  forall orders lines,
    UniqueOrderKeys orders ->
    q12_sum_bound orders lines ->
    q12_final_rows_of orders lines =
    EmbedResult_Q12 (EvalSpec_Q12 S_Q12_catalog orders lines).
Proof.
  intros orders lines Huniq Hb.
  unfold q12_final_rows_of, EmbedResult_Q12.
  rewrite (Q12_GroupsCorrespond_holds orders lines Huniq Hb).
  rewrite Q12_EvalSpec_eq_Sort_group_outs.
  rewrite map_map.
  apply map_ext; intro o; apply q12_outer_of_group_tuple_eq_EmbedOutRow.
Qed.


(** * Package Pre facts + of_holds composition + hyp-free NormalizeSound *)

Lemma Preconditions_Q12_UniqueOrderKeys :
  forall orders lines,
    Preconditions_Q12 orders lines ->
    UniqueOrderKeys orders.
Proof. intros orders lines [_ H]; exact H. Qed.

Lemma Forall_filter :
  forall (A : Type) (P : A -> Prop) (f : A -> bool) (l : list A),
    Forall P l -> Forall P (List.filter f l).
Proof.
  intros A P f l H.
  induction l as [|x xs IH]; [constructor|].
  inversion H; subst; cbn.
  destruct (f x); [constructor; [assumption|apply IH; assumption]|apply IH; assumption].
Qed.

Lemma Forall_rev :
  forall (A : Type) (P : A -> Prop) (l : list A),
    Forall P l -> Forall P (List.rev l).
Proof.
  intros A P l H.
  rewrite Forall_forall in H |- *.
  intros x Hx; apply H; apply in_rev; exact Hx.
Qed.

Lemma q12_mode_pre_rows_labels_Forall :
  forall orders lines mode,
    Forall q12_group_row_labels_ok (q12_mode_pre_rows orders lines mode).
Proof.
  intros orders lines mode.
  rewrite <- (filter_pre_group_eq_mode_rows orders lines mode).
  apply Forall_filter.
  apply q12_pre_group_rows_labels_Forall.
Qed.

Lemma q12_make_groups_bucket_ok :
  forall orders lines group,
    q12_sum_bound orders lines ->
    In group
      (query_make_groups nil
        (q12_pre_group_rows orders lines) q12_group_terms) ->
    Forall q12_group_row_labels_ok group /\
    q12_group_bucket_safe group.
Proof.
  intros orders lines group Hb Hin.
  pose proof (q12_make_groups_incl_mode_buckets orders lines group Hin) as Hinb.
  unfold q12_mode_buckets, q12_mode_bucket_list in Hinb.
  destruct (q12_mode_pre_rows orders lines "MAIL") as [|m0 mrest] eqn:Em;
    destruct (q12_mode_pre_rows orders lines "SHIP") as [|s0 srest] eqn:Es;
    cbn [List.app] in Hinb.
  - contradiction Hinb.
  - destruct Hinb as [Heq|[]]; subst group.
    split.
    + rewrite <- Es; apply Forall_rev; apply q12_mode_pre_rows_labels_Forall.
    + rewrite <- Es; apply q12_mode_bucket_safe;
        apply q12_sum_bound_mode; exact Hb.
  - destruct Hinb as [Heq|[]]; subst group.
    split.
    + rewrite <- Em; apply Forall_rev; apply q12_mode_pre_rows_labels_Forall.
    + rewrite <- Em; apply q12_mode_bucket_safe;
        apply q12_sum_bound_mode; exact Hb.
  - destruct Hinb as [Heq|Hinb']; [|destruct Hinb' as [Heq|[]]]; subst group.
    + split.
      * rewrite <- Em; apply Forall_rev; apply q12_mode_pre_rows_labels_Forall.
      * rewrite <- Em; apply q12_mode_bucket_safe;
          apply q12_sum_bound_mode; exact Hb.
    + split.
      * rewrite <- Es; apply Forall_rev; apply q12_mode_pre_rows_labels_Forall.
      * rewrite <- Es; apply q12_mode_bucket_safe;
          apply q12_sum_bound_mode; exact Hb.
Qed.

Lemma q12_ordered_rows_outer_labels :
  forall orders lines row,
    UniqueOrderKeys orders ->
    q12_sum_bound orders lines ->
    In row (q12_ordered_rows_of orders lines) ->
    q12_outer_row_labels_ok row.
Proof.
  intros orders lines row Huniq Hb Hin.
  unfold q12_ordered_rows_of in Hin.
  pose proof (@sort_ordered_rows_permutation TNull q12_value_is_null
                q12_order_keys (q12_group_rows_of orders lines)) as Hp.
  apply Permutation_sym in Hp.
  apply (Permutation_in _ Hp) in Hin.
  pose proof (q12_group_rows_In_iff_CollectModes orders lines row Huniq Hb) as Iff.
  apply (proj1 Iff) in Hin.
  apply in_map_iff in Hin; destruct Hin as [o [Heq _]].
  subst row; apply q12_group_tuple_outer_labels_ok.
Qed.

(** Composition: leaf holds + identity/join bags + Spec final rows. *)
Lemma FormalSQLAuxCorrect_Q12_of_holds :
  forall orders lines,
    Preconditions_Q12 orders lines ->
    (forall o l, In o orders -> In l lines -> q12_pair_domain_ok o l) ->
    (forall o, In o orders -> q12_priority_embed_ok (o_priority o)) ->
    q12_sum_bound orders lines ->
    q12_final_rows_of orders lines =
      EmbedResult_Q12 (EvalSpec_Q12 S_Q12_catalog orders lines) ->
    FormalSQL_exec_Q12 (EmbedDB_Q12 orders lines)
      (EmbedResult_Q12 (EvalSpec_Q12 S_Q12_catalog orders lines)).
Proof.
  intros orders lines Hpre Hdom Hpri Hb Hfinal.
  set (order_rows := map EmbedOrderRow orders).
  set (line_rows := map EmbedLineRow lines).
  set (proj_o := map (fun row =>
    project_row Queries.scalar_select_list_3 (q12_orders_dot_values row))
    order_rows).
  set (proj_l := map (fun row =>
    project_row Queries.scalar_select_list_4 (q12_lineitem_dot_values row))
    line_rows).
  set (join_rows := q12_embed_join_rows orders lines).
  set (group_rows := q12_group_rows_of orders lines).
  set (ordered_rows := q12_ordered_rows_of orders lines).
  eapply FormalSQLAuxCorrect_Q12_of_leaves
    with (order_rows := order_rows) (line_rows := line_rows)
         (proj_o := proj_o) (proj_l := proj_l)
         (join_rows := join_rows)
         (filt_true_rows := join_rows)
         (where_rows := q12_where_rows orders lines)
         (pre_group_rows := q12_pre_group_rows orders lines)
         (group_rows := group_rows) (ordered_rows := ordered_rows);
    try assumption.
  - unfold order_rows; apply Q12_TableOrdersOk_holds.
  - unfold line_rows; apply Q12_TableLinesOk_holds.
  - unfold proj_o, order_rows; apply Q12_ProjectTableOk_orders_of_embed.
  - unfold proj_l, line_rows; apply Q12_ProjectTableOk_lines_of_embed.
  - unfold proj_o, proj_l, order_rows, line_rows, join_rows.
    apply Q12_JoinBagOk_of_projects.
  - apply Q12_FilterOk_True.
  - apply Q12_FilterOk_where_holds; exact Hdom.
  - apply Q12_ProjectTableOk_case_holds.
    intros o l Ho _; apply Hpri; exact Ho.
  - unfold group_rows, q12_group_rows_of.
    apply Q12_GroupBagOk_holds.
    intros g Hg; apply (q12_make_groups_bucket_ok orders lines);
      [exact Hb|exact Hg].
  - unfold ordered_rows, group_rows, q12_ordered_rows_of.
    apply Q12_OrderOk_canonical.
  - rewrite <- Hfinal.
    unfold q12_final_rows_of, ordered_rows.
    apply Q12_OuterProjectOk_holds.
    intros row Hin.
    apply (q12_ordered_rows_outer_labels orders lines row
             (Preconditions_Q12_UniqueOrderKeys orders lines Hpre) Hb Hin).
Qed.

Theorem FormalSQLAuxCorrect_Q12 :
  forall orders lines,
    Preconditions_Q12 orders lines ->
    (forall o l, In o orders -> In l lines -> q12_pair_domain_ok o l) ->
    (forall o, In o orders -> q12_priority_embed_ok (o_priority o)) ->
    q12_sum_bound orders lines ->
    FormalSQL_exec_Q12 (EmbedDB_Q12 orders lines)
      (EmbedResult_Q12 (EvalSpec_Q12 S_Q12_catalog orders lines)).
Proof.
  intros orders lines Hpre Hdom Hpri Hb.
  apply FormalSQLAuxCorrect_Q12_of_holds; try assumption.
  apply q12_final_rows_eq_EmbedResult;
    [exact (Preconditions_Q12_UniqueOrderKeys orders lines Hpre)|exact Hb].
Qed.

Theorem NormalizeSound_Q12 :
  forall orders lines,
    Preconditions_Q12 orders lines ->
    (forall o l, In o orders -> In l lines -> q12_pair_domain_ok o l) ->
    (forall o, In o orders -> q12_priority_embed_ok (o_priority o)) ->
    q12_sum_bound orders lines ->
    FormalSQL_exec_Q12 (EmbedDB_Q12 orders lines)
      (EmbedResult_Q12 (EvalSpec_Q12 S_Q12_catalog orders lines)).
Proof.
  intros; apply FormalSQLAuxCorrect_Q12; assumption.
Qed.

Theorem Normalize_and_Sound_Q12 :
  forall orders lines,
    Preconditions_Q12 orders lines ->
    (forall o l, In o orders -> In l lines -> q12_pair_domain_ok o l) ->
    (forall o, In o orders -> q12_priority_embed_ok (o_priority o)) ->
    q12_sum_bound orders lines ->
    normalize_query_Q12 Queries.source_query_expr = Some S_Q12_catalog /\
    FormalSQL_exec_Q12 (EmbedDB_Q12 orders lines)
      (EmbedResult_Q12 (EvalSpec_Q12 S_Q12_catalog orders lines)).
Proof.
  intros orders lines Hpre Hdom Hpri Hb.
  split.
  - apply normalize_qhat_Q12_catalog.
  - apply NormalizeSound_Q12; assumption.
Qed.
