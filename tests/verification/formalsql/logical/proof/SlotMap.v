(* SlotMap / RowRel / BagRel — schema-directed FormalSQL↔RelNode row bridge.
   Shared LogicalSpec proof library. No query-specific column names. *)

From Stdlib Require Import List ZArith String Bool Nat Lia Sorting.Permutation.
Import ListNotations.
Open Scope string_scope.

From SQLFS Require Import
  Values ValueCore GenericInstance FTuples FiniteSet Join.
From FormalSQLLogical Require Import SqlTypes Schema.
From FormalSQLLogical.proof Require Import ValueCorrespond.

Import Tuple.
Import NullValues.

(** * Slot binding: RelNode slot i ↔ FormalSQL attribute *)

Record SlotBinding : Type := mkSlotBinding {
  sb_formal : attribute TNull;
  sb_rel : Attr
}.

Definition SlotMap : Type := list SlotBinding.

Definition slot_formal_name (sb : SlotBinding) : string :=
  formal_attr_name (sb_formal sb).

Definition slot_rel_name (sb : SlotBinding) : string :=
  attr_name (sb_rel sb).

Definition slot_rel_type (sb : SlotBinding) : SqlType :=
  attr_type (sb_rel sb).

(** Explicit name agreement + type compatibility (no silent positional coincidence). *)
Definition slot_binding_wf (sb : SlotBinding) : Prop :=
  slot_formal_name sb = slot_rel_name sb /\
  formal_attr_sqltype (sb_formal sb) = Some (slot_rel_type sb).

Definition slot_map_names (sm : SlotMap) : list string :=
  List.map slot_formal_name sm.

Definition slot_map_rel_schema (sm : SlotMap) : Schema :=
  List.map sb_rel sm.

(** Well-formedness: unique FormalSQL names, unique RelNode names, each binding WF,
    and the RelNode schema is exactly [slot_map_rel_schema sm]. *)
Definition SlotMap_wf (sm : SlotMap) : Prop :=
  List.NoDup (slot_map_names sm) /\
  List.NoDup (List.map slot_rel_name sm) /\
  List.Forall slot_binding_wf sm.

Lemma SlotMap_wf_nth_binding_wf :
  forall sm i sb,
    SlotMap_wf sm ->
    nth_error sm i = Some sb ->
    slot_binding_wf sb.
Proof.
  intros sm i sb [_ [_ Hfa]] Hi.
  rewrite Forall_forall in Hfa.
  apply Hfa.
  apply nth_error_In in Hi; exact Hi.
Qed.

Lemma SlotMap_wf_length_schema :
  forall sm,
    List.length (slot_map_rel_schema sm) = List.length sm.
Proof.
  intros; unfold slot_map_rel_schema; apply length_map.
Qed.

(** * Row correspondence *)

(** Every RelNode slot holds the SqlValue corresponding to the FormalSQL
    attribute named by [slot_map[i]], under that slot's RelNode type.
    Concatenated join/semi layouts use [sm1 ++ sm2] with [rr1 ++ rr2]. *)
Definition row_correspond (sm : SlotMap) (ft : tuple TNull) (rr : Row) : Prop :=
  List.length rr = List.length sm /\
  forall i sb v,
    nth_error sm i = Some sb ->
    nth_error rr i = Some v ->
    value_correspond (slot_rel_type sb) (dot TNull ft (sb_formal sb)) v.

(** Non-numeric SlotMaps determine the RelNode row of any corresponding
    FormalSQL tuple. *)
Definition slot_map_non_numeric (sm : SlotMap) : Prop :=
  List.Forall (fun sb => forall scale, slot_rel_type sb <> TNumeric scale) sm.

Definition rel_row_of (sm : SlotMap) (ft : tuple TNull) : Row :=
  List.map (fun sb => rel_of_value (dot TNull ft (sb_formal sb))) sm.

Lemma row_correspond_rel_row_of :
  forall sm ft rr,
    slot_map_non_numeric sm ->
    row_correspond sm ft rr ->
    rr = rel_row_of sm ft.
Proof.
  induction sm as [|sb sm IH]; intros ft rr Hnn [Hlen Hdots].
  - destruct rr; [reflexivity|discriminate Hlen].
  - destruct rr as [|v rr]; [discriminate Hlen|].
    inversion Hnn as [|? ? Hsb Hnn']; subst.
    unfold rel_row_of; simpl; f_equal.
    + eapply value_correspond_rel_of_value; [|exact Hsb].
      exact (Hdots 0%nat sb v eq_refl eq_refl).
    + apply IH; [exact Hnn'|].
      split; [simpl in Hlen; injection Hlen; auto|].
      intros i sb' v' Hi Hv; exact (Hdots (S i) sb' v' Hi Hv).
Qed.

(** * Bag / table correspondence

    [bag_correspond] — ordered Forall2. Use when both sides share enumeration
    order (e.g. scan embed via [map], or post-ORDER-BY).

    [bag_correspond_unordered] — permutation lifting of [row_correspond].
    Use for pre-ORDER-BY CrossJoin / filter intermediates: FormalSQL
    [query_cross_join_bag] reifies via [Febag.elements] (canonical order),
    while RelNode [join_inner] uses nested-loop order. Multiplicity is
    preserved exactly; enumeration order is not treated as SQL ORDER BY. *)

Definition bag_correspond (sm : SlotMap) (fts : list (tuple TNull)) (rrs : list Row)
  : Prop :=
  List.Forall2 (row_correspond sm) fts rrs.

(** Unordered bag correspondence: RelNode list may be reordered so that it
    lines up pointwise with the FormalSQL list under [row_correspond].
    Equivalent (up to swapping sides) to existing a mutual permutation of a
    Forall2 witness. Exact multiplicity; no order obligation. *)
Definition bag_correspond_unordered
    (sm : SlotMap) (fts : list (tuple TNull)) (rrs : list Row) : Prop :=
  exists rrs',
    Permutation rrs rrs' /\
    List.Forall2 (row_correspond sm) fts rrs'.

Lemma bag_correspond_nil :
  forall sm, bag_correspond sm nil nil.
Proof. intros; constructor. Qed.

Lemma bag_correspond_unordered_nil :
  forall sm, bag_correspond_unordered sm nil nil.
Proof.
  intros sm.
  exists nil; split; [apply Permutation_refl|constructor].
Qed.

Lemma bag_correspond_to_unordered :
  forall sm fts rrs,
    bag_correspond sm fts rrs ->
    bag_correspond_unordered sm fts rrs.
Proof.
  intros sm fts rrs H.
  exists rrs; split; [apply Permutation_refl|exact H].
Qed.

Lemma bag_correspond_length :
  forall sm fts rrs,
    bag_correspond sm fts rrs ->
    List.length fts = List.length rrs.
Proof.
  intros; eapply Forall2_length; eassumption.
Qed.

Lemma bag_correspond_unordered_length :
  forall sm fts rrs,
    bag_correspond_unordered sm fts rrs ->
    List.length fts = List.length rrs.
Proof.
  intros sm fts rrs [rrs' [HP HF]].
  rewrite (Permutation_length HP).
  eapply Forall2_length; eassumption.
Qed.

Lemma bag_correspond_map_embed :
  forall sm (embed : Row -> tuple TNull) (rrs : list Row),
    (forall r, row_correspond sm (embed r) r) ->
    bag_correspond sm (List.map embed rrs) rrs.
Proof.
  intros sm embed rrs H.
  induction rrs as [|r rs IH]; simpl; constructor; auto.
Qed.

(** * Filter preservation for unordered bags

    Generic: only requires that keep decisions agree on [row_correspond]
    pairs. No FormalSQL / RelNode predicate semantics. *)

Lemma Forall2_filter_keep :
  forall (A B : Type) (R : A -> B -> Prop)
         (keepA : A -> bool) (keepB : B -> bool) xs ys,
    List.Forall2 R xs ys ->
    (forall x y, R x y -> keepA x = keepB y) ->
    List.Forall2 R (List.filter keepA xs) (List.filter keepB ys).
Proof.
  intros A B R keepA keepB xs ys HF Hkeep.
  induction HF as [|x y xs ys HR HFs IH]; simpl.
  - constructor.
  - rewrite (Hkeep x y HR).
    destruct (keepB y); simpl; [constructor; auto|exact IH].
Qed.

Lemma Permutation_filter :
  forall (A : Type) (keep : A -> bool) xs ys,
    Permutation xs ys ->
    Permutation (List.filter keep xs) (List.filter keep ys).
Proof.
  intros A keep xs ys HP.
  induction HP as [ | x l l' H IH | x y l | l l' l'' H1 IH1 H2 IH2 ]; simpl.
  - constructor.
  - destruct (keep x); [constructor; exact IH|exact IH].
  - destruct (keep x); destruct (keep y); simpl;
      try constructor; try apply Permutation_refl;
      try apply perm_swap; try constructor; apply Permutation_refl.
  - eapply Permutation_trans; eassumption.
Qed.

Lemma bag_correspond_filter :
  forall sm fts rrs (keep_f : tuple TNull -> bool) (keep_r : Row -> bool),
    bag_correspond sm fts rrs ->
    (forall ft rr, row_correspond sm ft rr -> keep_f ft = keep_r rr) ->
    bag_correspond sm (List.filter keep_f fts) (List.filter keep_r rrs).
Proof.
  intros sm fts rrs keep_f keep_r HF Hkeep.
  eapply Forall2_filter_keep; eassumption.
Qed.

Lemma bag_correspond_unordered_filter :
  forall sm fts rrs (keep_f : tuple TNull -> bool) (keep_r : Row -> bool),
    bag_correspond_unordered sm fts rrs ->
    (forall ft rr, row_correspond sm ft rr -> keep_f ft = keep_r rr) ->
    bag_correspond_unordered sm
      (List.filter keep_f fts) (List.filter keep_r rrs).
Proof.
  intros sm fts rrs keep_f keep_r [rrs' [HP HF]] Hkeep.
  exists (List.filter keep_r rrs').
  split.
  - apply Permutation_filter; exact HP.
  - eapply Forall2_filter_keep; eassumption.
Qed.

(** Concatenated join layouts: [sm1 ++ sm2] with [rr1 ++ rr2].
    FormalSQL side remains one combined tuple [ft] watched by both maps. *)
Lemma row_correspond_app :
  forall sm1 sm2 ft rr1 rr2,
    row_correspond sm1 ft rr1 ->
    row_correspond sm2 ft rr2 ->
    row_correspond (List.app sm1 sm2) ft (List.app rr1 rr2).
Proof.
  intros sm1 sm2 ft rr1 rr2 [Hlen1 Hdots1] [Hlen2 Hdots2].
  split.
  - rewrite List.length_app, List.length_app, Hlen1, Hlen2; reflexivity.
  - intros i sb v Hi Hv.
    destruct (Compare_dec.le_lt_dec (List.length sm1) i) as [Hge|Hlt].
    + assert (Hlen_rr : (List.length rr1 <= i)%nat) by (rewrite Hlen1; exact Hge).
      rewrite nth_error_app2 in Hi by exact Hge.
      rewrite nth_error_app2 in Hv by exact Hlen_rr.
      rewrite Hlen1 in Hv.
      eapply Hdots2; eassumption.
    + rewrite nth_error_app1 in Hi by exact Hlt.
      rewrite <- Hlen1 in Hlt.
      rewrite nth_error_app1 in Hv by exact Hlt.
      eapply Hdots1; eassumption.
Qed.

Lemma row_correspond_app4 :
  forall sm1 sm2 sm3 sm4 ft rr1 rr2 rr3 rr4,
    row_correspond sm1 ft rr1 ->
    row_correspond sm2 ft rr2 ->
    row_correspond sm3 ft rr3 ->
    row_correspond sm4 ft rr4 ->
    row_correspond (List.app sm1 (List.app sm2 (List.app sm3 sm4))) ft
      (List.app rr1 (List.app rr2 (List.app rr3 rr4))).
Proof.
  intros sm1 sm2 sm3 sm4 ft rr1 rr2 rr3 rr4 H1 H2 H3 H4.
  apply row_correspond_app; [exact H1|].
  apply row_correspond_app; [exact H2|].
  apply row_correspond_app; [exact H3|exact H4].
Qed.

(** * Cross product (Cartesian) — FormalSQL join_tuple ↔ RelNode app

    Watched attrs of [sm1]/[sm2] must be present on the respective sides
    and right watched attrs must not collide with left FormalSQL labels
    (so [dot_join_tuple_2] applies). *)

Definition slot_map_present (sm : SlotMap) (ft : tuple TNull) : Prop :=
  List.Forall (fun sb => sb_formal sb inS labels TNull ft) sm.

Definition slot_map_fresh_from (sm : SlotMap) (ft : tuple TNull) : Prop :=
  List.Forall (fun sb => (sb_formal sb inS? labels TNull ft) = false) sm.

Definition cross_join_tuples
    (left right : list (tuple TNull)) : list (tuple TNull) :=
  List.flat_map
    (fun ft1 => List.map (fun ft2 => join_tuple TNull ft1 ft2) right) left.

Definition cross_join_rows (left right : list Row) : list Row :=
  List.flat_map
    (fun rr1 => List.map (fun rr2 => List.app rr1 rr2) right) left.

Lemma Forall_app_iff :
  forall (A : Type) (P : A -> Prop) xs ys,
    List.Forall P (List.app xs ys) <-> (List.Forall P xs /\ List.Forall P ys).
Proof.
  intros A P xs ys; split.
  - intros H; induction xs as [|x xs IH]; simpl in H; [split; [constructor|exact H]|].
    inversion H; subst; destruct (IH H3) as [Hx Hy]; split; [constructor; assumption|exact Hy].
  - intros [Hx Hy]; induction Hx as [|x xs Hx Hxs IH]; simpl; [exact Hy|constructor; auto].
Qed.

Lemma slot_map_present_app :
  forall sm1 sm2 ft,
    slot_map_present sm1 ft ->
    slot_map_present sm2 ft ->
    slot_map_present (List.app sm1 sm2) ft.
Proof.
  intros sm1 sm2 ft H1 H2.
  unfold slot_map_present in *.
  apply Forall_app_iff; split; assumption.
Qed.

Lemma slot_map_fresh_from_app :
  forall sm1 sm2 ft,
    slot_map_fresh_from sm1 ft ->
    slot_map_fresh_from sm2 ft ->
    slot_map_fresh_from (List.app sm1 sm2) ft.
Proof.
  intros sm1 sm2 ft H1 H2.
  unfold slot_map_fresh_from in *.
  apply Forall_app_iff; split; assumption.
Qed.

Lemma slot_map_present_join_left :
  forall sm ft1 ft2,
    slot_map_present sm ft1 ->
    slot_map_present sm (join_tuple TNull ft1 ft2).
Proof.
  intros sm ft1 ft2 H.
  unfold slot_map_present in *.
  apply Forall_forall; intros sb Hin.
  pose proof (proj1 (Forall_forall _ _) H sb Hin) as Ha.
  unfold join_tuple.
  rewrite (Fset.mem_eq_2 _ _ _ (labels_mk_tuple TNull _ _)), Fset.mem_union, Ha.
  reflexivity.
Qed.

Lemma slot_map_present_join_right :
  forall sm ft1 ft2,
    slot_map_present sm ft2 ->
    slot_map_present sm (join_tuple TNull ft1 ft2).
Proof.
  intros sm ft1 ft2 H.
  unfold slot_map_present in *.
  apply Forall_forall; intros sb Hin.
  pose proof (proj1 (Forall_forall _ _) H sb Hin) as Ha.
  unfold join_tuple.
  rewrite (Fset.mem_eq_2 _ _ _ (labels_mk_tuple TNull _ _)), Fset.mem_union, Ha.
  rewrite Bool.orb_true_r; reflexivity.
Qed.

Lemma slot_map_fresh_from_join :
  forall sm ft1 ft2,
    slot_map_fresh_from sm ft1 ->
    slot_map_fresh_from sm ft2 ->
    slot_map_fresh_from sm (join_tuple TNull ft1 ft2).
Proof.
  intros sm ft1 ft2 H1 H2.
  unfold slot_map_fresh_from in *.
  apply Forall_forall; intros sb Hin.
  pose proof (proj1 (Forall_forall _ _) H1 sb Hin) as Ha1.
  pose proof (proj1 (Forall_forall _ _) H2 sb Hin) as Ha2.
  unfold join_tuple.
  rewrite (Fset.mem_eq_2 _ _ _ (labels_mk_tuple TNull _ _)), Fset.mem_union, Ha1, Ha2.
  reflexivity.
Qed.

Lemma row_correspond_join_tuple_left :
  forall sm ft1 ft2 rr,
    row_correspond sm ft1 rr ->
    slot_map_present sm ft1 ->
    row_correspond sm (join_tuple TNull ft1 ft2) rr.
Proof.
  intros sm ft1 ft2 rr [Hlen Hdots] Hpres.
  split; [exact Hlen|].
  intros i sb v Hi Hv.
  assert (Hin : List.In sb sm) by (eapply nth_error_In; eassumption).
  assert (Hpres' : sb_formal sb inS labels TNull ft1).
  { eapply (proj1 (Forall_forall _ _) Hpres); exact Hin. }
  rewrite (dot_join_tuple_1 TNull ft1 ft2 (sb_formal sb) Hpres').
  eapply Hdots; eassumption.
Qed.

Lemma row_correspond_join_tuple_right :
  forall sm ft1 ft2 rr,
    row_correspond sm ft2 rr ->
    slot_map_present sm ft2 ->
    slot_map_fresh_from sm ft1 ->
    row_correspond sm (join_tuple TNull ft1 ft2) rr.
Proof.
  intros sm ft1 ft2 rr [Hlen Hdots] Hpres Hfresh.
  split; [exact Hlen|].
  intros i sb v Hi Hv.
  assert (Hin : List.In sb sm) by (eapply nth_error_In; eassumption).
  assert (Hpres' : sb_formal sb inS labels TNull ft2).
  { eapply (proj1 (Forall_forall _ _) Hpres); exact Hin. }
  assert (Hfresh' : (sb_formal sb inS? labels TNull ft1) = false).
  { eapply (proj1 (Forall_forall _ _) Hfresh); exact Hin. }
  rewrite (dot_join_tuple_2 TNull ft1 ft2 (sb_formal sb) Hfresh' Hpres').
  eapply Hdots; eassumption.
Qed.

(** Row-level Cartesian product: FormalSQL [join_tuple] ↔ RelNode [++]
    under concatenated slot maps. Reuses [row_correspond_app]. *)
Lemma row_correspond_cross_join :
  forall sm1 sm2 ft1 ft2 rr1 rr2,
    row_correspond sm1 ft1 rr1 ->
    row_correspond sm2 ft2 rr2 ->
    slot_map_present sm1 ft1 ->
    slot_map_present sm2 ft2 ->
    slot_map_fresh_from sm2 ft1 ->
    row_correspond (List.app sm1 sm2)
      (join_tuple TNull ft1 ft2) (List.app rr1 rr2).
Proof.
  intros sm1 sm2 ft1 ft2 rr1 rr2 H1 H2 Hp1 Hp2 Hf2.
  apply row_correspond_app.
  - eapply row_correspond_join_tuple_left; eassumption.
  - eapply row_correspond_join_tuple_right; eassumption.
Qed.

Lemma Forall2_map_app_cross :
  forall sm1 sm2 ft1 rr1 fts2 rrs2,
    row_correspond sm1 ft1 rr1 ->
    List.Forall2 (row_correspond sm2) fts2 rrs2 ->
    slot_map_present sm1 ft1 ->
    (forall ft2, List.In ft2 fts2 -> slot_map_present sm2 ft2) ->
    slot_map_fresh_from sm2 ft1 ->
    List.Forall2 (row_correspond (List.app sm1 sm2))
      (List.map (fun ft2 => join_tuple TNull ft1 ft2) fts2)
      (List.map (fun rr2 => List.app rr1 rr2) rrs2).
Proof.
  intros sm1 sm2 ft1 rr1 fts2 rrs2 H1 HF Hp1 Hp2 Hf2.
  induction HF as [|ft2 rr2 fts2 rrs2 H2 HFs IH]; simpl; [constructor|].
  constructor.
  - eapply row_correspond_cross_join; try eassumption.
    apply Hp2; left; reflexivity.
  - apply IH; intros ft Hin; apply Hp2; right; exact Hin.
Qed.

Lemma Forall2_cross_join :
  forall sm1 sm2 fts1 fts2 rrs1 rrs2,
    List.Forall2 (row_correspond sm1) fts1 rrs1 ->
    List.Forall2 (row_correspond sm2) fts2 rrs2 ->
    (forall ft1, List.In ft1 fts1 -> slot_map_present sm1 ft1) ->
    (forall ft2, List.In ft2 fts2 -> slot_map_present sm2 ft2) ->
    (forall ft1, List.In ft1 fts1 -> slot_map_fresh_from sm2 ft1) ->
    List.Forall2 (row_correspond (List.app sm1 sm2))
      (cross_join_tuples fts1 fts2)
      (cross_join_rows rrs1 rrs2).
Proof.
  intros sm1 sm2 fts1 fts2 rrs1 rrs2 HF1 HF2 Hp1 Hp2 Hf2.
  unfold cross_join_tuples, cross_join_rows.
  induction HF1 as [|ft1 rr1 fts1 rrs1 H1 HFs IH]; simpl; [constructor|].
  apply Forall2_app.
  - eapply Forall2_map_app_cross; try eassumption.
    + apply Hp1; left; reflexivity.
    + apply Hf2; left; reflexivity.
  - apply IH.
    + intros ft Hin; apply Hp1; right; exact Hin.
    + intros ft Hin; apply Hf2; right; exact Hin.
Qed.

Lemma Permutation_flat_map :
  forall (A B : Type) (f : A -> list B) xs ys,
    Permutation xs ys ->
    Permutation (List.flat_map f xs) (List.flat_map f ys).
Proof.
  intros A B f xs ys HP.
  induction HP as [|x xs ys HP IH|x y xs|xs ys zs HP1 IH1 HP2 IH2]; simpl.
  - constructor.
  - apply Permutation_app; [apply Permutation_refl|exact IH].
  - rewrite !List.app_assoc.
    apply Permutation_app; [|apply Permutation_refl].
    apply Permutation_app_comm.
  - eapply Permutation_trans; eassumption.
Qed.

Lemma Permutation_flat_map_pointwise :
  forall (A B : Type) (f g : A -> list B) xs,
    (forall x, List.In x xs -> Permutation (f x) (g x)) ->
    Permutation (List.flat_map f xs) (List.flat_map g xs).
Proof.
  intros A B f g xs H.
  induction xs as [|x xs IH]; simpl; [constructor|].
  apply Permutation_app.
  - apply H; left; reflexivity.
  - apply IH; intros y Hin; apply H; right; exact Hin.
Qed.

Lemma Permutation_cross_join_rows_l :
  forall left left' right,
    Permutation left left' ->
    Permutation (cross_join_rows left right) (cross_join_rows left' right).
Proof.
  intros left left' right HP.
  unfold cross_join_rows.
  apply Permutation_flat_map; exact HP.
Qed.

Lemma Permutation_cross_join_rows_r :
  forall left right right',
    Permutation right right' ->
    Permutation (cross_join_rows left right) (cross_join_rows left right').
Proof.
  intros left right right' HP.
  unfold cross_join_rows.
  apply Permutation_flat_map_pointwise.
  intros rr1 _.
  apply Permutation_map; exact HP.
Qed.

Lemma Permutation_cross_join_rows :
  forall left left' right right',
    Permutation left left' ->
    Permutation right right' ->
    Permutation (cross_join_rows left right) (cross_join_rows left' right').
Proof.
  intros left left' right right' Hl Hr.
  eapply Permutation_trans.
  - apply Permutation_cross_join_rows_l; exact Hl.
  - apply Permutation_cross_join_rows_r; exact Hr.
Qed.

Lemma filter_always_true :
  forall (A : Type) (l : list A), List.filter (fun _ => true) l = l.
Proof. induction l; simpl; [reflexivity|congruence]. Qed.

Lemma flat_map_ext :
  forall (A B : Type) (f g : A -> list B) l,
    (forall x, f x = g x) ->
    List.flat_map f l = List.flat_map g l.
Proof.
  intros A B f g l Hext.
  induction l as [|x xs IH]; simpl; [reflexivity|].
  rewrite Hext, IH; reflexivity.
Qed.

Lemma map_ext_eq :
  forall (A B : Type) (f g : A -> B) l,
    (forall x, f x = g x) ->
    List.map f l = List.map g l.
Proof.
  intros A B f g l Hext.
  induction l as [|x xs IH]; simpl; [reflexivity|].
  rewrite Hext, IH; reflexivity.
Qed.

(** FormalSQL [brute_left_join_list] is the same nested-loop product as
    [cross_join_tuples] (filter always keeps). *)
Lemma cross_join_tuples_eq_brute_left_join_list :
  forall left right,
    cross_join_tuples left right =
    brute_left_join_list (tuple TNull) (join_tuple TNull) left right.
Proof.
  intros left right.
  unfold cross_join_tuples, brute_left_join_list, theta_join_list, d_join_list.
  apply flat_map_ext; intros ft1.
  rewrite filter_always_true.
  apply map_ext_eq; intros; reflexivity.
Qed.

(** Bag-level Cartesian product under [bag_correspond_unordered].
    Exact multiplicity (|L|×|R| on both sides); order via Permutation only. *)
Lemma bag_correspond_unordered_cross_product :
  forall sm1 sm2 fts1 fts2 rrs1 rrs2,
    bag_correspond_unordered sm1 fts1 rrs1 ->
    bag_correspond_unordered sm2 fts2 rrs2 ->
    (forall ft1, List.In ft1 fts1 -> slot_map_present sm1 ft1) ->
    (forall ft2, List.In ft2 fts2 -> slot_map_present sm2 ft2) ->
    (forall ft1, List.In ft1 fts1 -> slot_map_fresh_from sm2 ft1) ->
    bag_correspond_unordered (List.app sm1 sm2)
      (cross_join_tuples fts1 fts2)
      (cross_join_rows rrs1 rrs2).
Proof.
  intros sm1 sm2 fts1 fts2 rrs1 rrs2 [rrs1' [HP1 HF1]] [rrs2' [HP2 HF2]]
    Hp1 Hp2 Hf2.
  exists (cross_join_rows rrs1' rrs2').
  split.
  - apply Permutation_cross_join_rows; assumption.
  - eapply Forall2_cross_join; eassumption.
Qed.

Lemma In_cross_join_tuples :
  forall fts1 fts2 ft,
    List.In ft (cross_join_tuples fts1 fts2) ->
    exists ft1 ft2,
      List.In ft1 fts1 /\ List.In ft2 fts2 /\
      ft = join_tuple TNull ft1 ft2.
Proof.
  intros fts1 fts2 ft Hin.
  unfold cross_join_tuples in Hin.
  apply in_flat_map in Hin.
  destruct Hin as [ft1 [Hin1 Hinmap]].
  apply in_map_iff in Hinmap.
  destruct Hinmap as [ft2 [Heq Hin2]].
  exists ft1, ft2; repeat split; auto.
Qed.

Lemma slot_map_present_cross_join_tuples :
  forall sm1 sm2 fts1 fts2 ft,
    (forall ft1, List.In ft1 fts1 -> slot_map_present sm1 ft1) ->
    (forall ft2, List.In ft2 fts2 -> slot_map_present sm2 ft2) ->
    List.In ft (cross_join_tuples fts1 fts2) ->
    slot_map_present (List.app sm1 sm2) ft.
Proof.
  intros sm1 sm2 fts1 fts2 ft Hp1 Hp2 Hin.
  destruct (In_cross_join_tuples fts1 fts2 ft Hin) as [ft1 [ft2 [H1 [H2 Heq]]]].
  subst ft.
  apply slot_map_present_app.
  - apply slot_map_present_join_left; auto.
  - apply slot_map_present_join_right; auto.
Qed.

Lemma slot_map_fresh_from_cross_join_tuples :
  forall sm fts1 fts2 ft,
    (forall ft1, List.In ft1 fts1 -> slot_map_fresh_from sm ft1) ->
    (forall ft2, List.In ft2 fts2 -> slot_map_fresh_from sm ft2) ->
    List.In ft (cross_join_tuples fts1 fts2) ->
    slot_map_fresh_from sm ft.
Proof.
  intros sm fts1 fts2 ft Hf1 Hf2 Hin.
  destruct (In_cross_join_tuples fts1 fts2 ft Hin) as [ft1 [ft2 [H1 [H2 Heq]]]].
  subst ft.
  apply slot_map_fresh_from_join; auto.
Qed.

(** RelNode↔RelNode bag relation used by Morphisms (replaces vacuous [True]). *)
Definition BagRel (R : Row -> Row -> Prop) (a b : list Row) : Prop :=
  List.Forall2 R a b.
