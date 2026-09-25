(* Q12SemanticBridge — compositional Spec/Embed lemmas for NormalizeSound_Q12.

   Proves operator-level ResultSpec facts (Qed):
     classify / qualify / window / mode / CASE contrib / EvalSpec decomp /
     nil EvalSpec / priority-map under UniqueOrderKeys.

   FormalSQL interpreter spine (CrossJoin→Filter→Group→Order→Project)
   matching Embed(EvalSpec) is NOT proved here — residual is
   [Q12_GroupRowsPerm_CollectModes] (intended FS groups ↔ CollectModes
   Permutation; [Q12_GroupBagsPerm] removed), not a silent whole-query
   trust of join+CASE+group+order Spec semantics.
*)

From Stdlib Require Import List ZArith String Lia Bool.
Import ListNotations.
Open Scope Z_scope.
Open Scope string_scope.

From FormalSQLResultSpec Require Import QueryResultSpecQ12 Q12EvalSpec.

Import Q12EvalSpec.

(** * Priority / CASE *)

Lemma ClassifyPriority_urgent :
  ClassifyPriority "1-URGENT" = High.
Proof. reflexivity. Qed.

Lemma ClassifyPriority_high :
  ClassifyPriority "2-HIGH" = High.
Proof. reflexivity. Qed.

Lemma ClassifyPriority_else_low :
  forall p,
    String.eqb p "1-URGENT" = false ->
    String.eqb p "2-HIGH" = false ->
    ClassifyPriority p = Low.
Proof.
  intros p Hu Hh; unfold ClassifyPriority; rewrite Hu, Hh; reflexivity.
Qed.

Lemma SpecApply_high :
  forall s sign,
    SpecApply s High sign = mkSpecStats (high s + sign) (low s).
Proof. reflexivity. Qed.

Lemma SpecApply_low :
  forall s sign,
    SpecApply s Low sign = mkSpecStats (high s) (low s + sign).
Proof. reflexivity. Qed.

(** * Filters *)

Lemma LineQualifies_iff :
  forall l,
    LineQualifies l = true <->
    (l_commitdate l < l_receiptdate l /\ l_shipdate l < l_commitdate l)%Z.
Proof.
  intros l; unfold LineQualifies; split.
  - intro H; apply andb_prop in H; destruct H as [H1 H2].
    apply Z.ltb_lt in H1; apply Z.ltb_lt in H2; split; assumption.
  - intros [H1 H2]; apply andb_true_intro; split;
      [apply Z.ltb_lt|apply Z.ltb_lt]; assumption.
Qed.

Lemma ModeAccepted_mail_ship :
  forall mode,
    ModeAccepted mode "MAIL" "SHIP" = true <->
    (mode = "MAIL" \/ mode = "SHIP").
Proof.
  intros mode; unfold ModeAccepted; split.
  - intro H; apply orb_prop in H; destruct H as [H|H];
      apply String.eqb_eq in H; [left|right]; exact H.
  - intros [H|H]; apply orb_true_intro; [left|right];
      apply String.eqb_eq; exact H.
Qed.

Lemma InWindow_iff :
  forall day start endExclusive,
    InWindow day start endExclusive = true <->
    (start <= day < endExclusive)%Z.
Proof.
  intros day start endExclusive; unfold InWindow; split.
  - intro H; apply andb_prop in H; destruct H as [H1 H2].
    apply Z.leb_le in H1; apply Z.ltb_lt in H2; split; assumption.
  - intros [H1 H2]; apply andb_true_intro; split;
      [apply Z.leb_le|apply Z.ltb_lt]; assumption.
Qed.

(** * EvalSpec decomposition (group + order on Spec side) *)

Lemma EvalSpec_Q12_decomp :
  forall s orders lines,
    EvalSpec_Q12 s orders lines =
    SortByMode
      (CollectModes (qrs_shipmode1 s) (qrs_shipmode2 s)
         (SpecView orders lines (qrs_start s) q12_catalog_end
                   (qrs_shipmode1 s) (qrs_shipmode2 s))).
Proof. reflexivity. Qed.

Lemma EvalSpec_Q12_catalog_decomp :
  forall orders lines,
    EvalSpec_Q12 S_Q12_catalog orders lines =
    SortByMode
      (CollectModes "MAIL" "SHIP"
         (SpecView orders lines q12_catalog_start q12_catalog_end
                   "MAIL" "SHIP")).
Proof. apply EvalSpec_S_Q12_catalog. Qed.

Lemma SpecView_nil_nil :
  forall start endExclusive m1 m2,
    SpecView nil nil start endExclusive m1 m2 = nil.
Proof. reflexivity. Qed.

Lemma CollectModes_empty :
  forall m1 m2,
    CollectModes m1 m2 nil = nil.
Proof.
  intros m1 m2; unfold CollectModes, SpecFinalizeOne, GetMode.
  destruct (String.eqb m1 m2); reflexivity.
Qed.

Lemma EvalSpec_Q12_nil_orders_lines :
  EvalSpec_Q12 S_Q12_catalog nil nil = nil.
Proof. apply EvalSpec_Q12_nil. Qed.

(** * Priority map under unique keys *)

Lemma PriLookup_PriUpsert_same :
  forall pri k p,
    PriLookup (PriUpsert pri k p) k = Some p.
Proof.
  induction pri as [|[k' p'] rest IH]; intros k p; cbn.
  - rewrite Z.eqb_refl; reflexivity.
  - destruct (Z.eqb k k') eqn:Ek.
    + apply Z.eqb_eq in Ek; subst; cbn; rewrite Z.eqb_refl; reflexivity.
    + cbn; rewrite Ek; apply IH.
Qed.

Lemma PriLookup_PriUpsert_other :
  forall pri k p k2,
    k <> k2 ->
    PriLookup (PriUpsert pri k p) k2 = PriLookup pri k2.
Proof.
  induction pri as [|[k' p'] rest IH]; intros k p k2 Hne; cbn.
  - destruct (Z.eqb k2 k) eqn:E; [apply Z.eqb_eq in E; congruence|reflexivity].
  - destruct (Z.eqb k k') eqn:Ek.
    + apply Z.eqb_eq in Ek; subst; cbn.
      destruct (Z.eqb k2 k') eqn:E2; [apply Z.eqb_eq in E2; congruence|reflexivity].
    + cbn; destruct (Z.eqb k2 k') eqn:E2; [reflexivity|apply IH; exact Hne].
Qed.

Lemma PriorityMap_lookup_cons :
  forall orders o,
    ~ In (o_orderkey o) (map o_orderkey orders) ->
    PriLookup (PriorityMap (orders ++ [o])) (o_orderkey o) =
    Some (ClassifyPriority (o_priority o)).
Proof.
  intros orders o Hfresh.
  unfold PriorityMap.
  rewrite fold_left_app; cbn.
  apply PriLookup_PriUpsert_same.
Qed.

(** * Join-side Spec contribution (line applies only when qualify+window+mode+lookup) *)

Lemma ApplyLine_skip_unqualified :
  forall m pri l start endExclusive m1 m2 sign,
    LineQualifies l = false ->
    ApplyLine m pri l start endExclusive m1 m2 sign = m.
Proof.
  intros; unfold ApplyLine; rewrite H; reflexivity.
Qed.

Lemma ApplyLine_skip_window :
  forall m pri l start endExclusive m1 m2 sign,
    LineQualifies l = true ->
    InWindow (l_receiptdate l) start endExclusive = false ->
    ApplyLine m pri l start endExclusive m1 m2 sign = m.
Proof.
  intros; unfold ApplyLine; rewrite H, H0; reflexivity.
Qed.

Lemma ApplyLine_skip_mode :
  forall m pri l start endExclusive m1 m2 sign,
    LineQualifies l = true ->
    InWindow (l_receiptdate l) start endExclusive = true ->
    ModeAccepted (l_shipmode l) m1 m2 = false ->
    ApplyLine m pri l start endExclusive m1 m2 sign = m.
Proof.
  intros; unfold ApplyLine; rewrite H, H0, H1; reflexivity.
Qed.

Lemma ApplyLine_skip_no_order :
  forall m pri l start endExclusive m1 m2 sign,
    LineQualifies l = true ->
    InWindow (l_receiptdate l) start endExclusive = true ->
    ModeAccepted (l_shipmode l) m1 m2 = true ->
    PriLookup pri (l_orderkey l) = None ->
    ApplyLine m pri l start endExclusive m1 m2 sign = m.
Proof.
  intros; unfold ApplyLine; rewrite H, H0, H1, H2; reflexivity.
Qed.

Lemma ApplyLine_high_contrib :
  forall m pri l start endExclusive m1 m2 sign cur,
    LineQualifies l = true ->
    InWindow (l_receiptdate l) start endExclusive = true ->
    ModeAccepted (l_shipmode l) m1 m2 = true ->
    PriLookup pri (l_orderkey l) = Some High ->
    GetMode m (l_shipmode l) = cur ->
    ApplyLine m pri l start endExclusive m1 m2 sign =
    SetMode m (l_shipmode l) (SpecApply cur High sign).
Proof.
  intros; unfold ApplyLine; rewrite H, H0, H1, H2, H3; reflexivity.
Qed.

Lemma ApplyLine_low_contrib :
  forall m pri l start endExclusive m1 m2 sign cur,
    LineQualifies l = true ->
    InWindow (l_receiptdate l) start endExclusive = true ->
    ModeAccepted (l_shipmode l) m1 m2 = true ->
    PriLookup pri (l_orderkey l) = Some Low ->
    GetMode m (l_shipmode l) = cur ->
    ApplyLine m pri l start endExclusive m1 m2 sign =
    SetMode m (l_shipmode l) (SpecApply cur Low sign).
Proof.
  intros; unfold ApplyLine; rewrite H, H0, H1, H2, H3; reflexivity.
Qed.
