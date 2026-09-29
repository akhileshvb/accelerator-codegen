(* Morphisms — reusable correspondence patterns for relational operators.
   USE WHEN / REQUIRES / PROVES comments are for LLM verification agents.

   Concrete FormalSQL↔RelNode relations live in ValueCorrespond / SlotMap /
   ScanCorrespond. RelNode↔RelNode bag correspondence uses [BagRel] = Forall2. *)

From Stdlib Require Import List.
From FormalSQLLogical Require Import SqlTypes Schema Expr RelNode Eval.
From FormalSQLLogical.proof Require Import SlotMap.

(** Re-export RelNode↔RelNode bag relation (exact multiplicity via Forall2). *)
Export SlotMap (BagRel).

(** * FilterMorphism
    USE WHEN: proving two filtered bags correspond under a row relation.
    REQUIRES: forall r r', Rel r r' -> (eval_pred P p (EvRow r) ↔ eval_pred P' p' (EvRow r')).
    PROVES:   BagRel Rel (filter P bag) (filter P' bag'). *)

Definition RowRel : Type := Row -> Row -> Prop.

(** RelNode [filter_rows] agrees with [List.filter] when keep always decides. *)
Lemma filter_rows_eq_List_filter :
  forall (pr : Pred) (p : Params) (db : list Row) (keep : Row -> bool),
    (forall r, List.In r db -> eval_pred_keep pr p r = Some (keep r)) ->
    filter_rows pr p db = Some (List.filter keep db).
Proof.
  intros pr p db keep Hkeep.
  induction db as [|r rs IH]; simpl.
  - reflexivity.
  - rewrite (Hkeep r (or_introl eq_refl)).
    rewrite IH.
    + destruct (keep r); reflexivity.
    + intros x Hx; apply Hkeep; right; exact Hx.
Qed.

(** * ProjectMorphism
    USE WHEN: projected bags correspond given expression correspondence.
    REQUIRES: expr-wise value correspondence under Rel.
    PROVES:   BagRel on project_rows results. *)

(** * JoinMorphism
    USE WHEN: join results correspond under left/right row maps + cond correspondence.
    REQUIRES: RelL / RelR + predicate correspondence on concatenated rows.
    PROVES:   BagRel (join_inner …) (join_inner …) with multiplicities. *)

(** RelNode Cartesian product: [join_inner] with constantly-true keep
    is exactly [cross_join_rows] (nested-loop order, exact |L|×|R|). *)
Lemma join_against_true_eq_map_app :
  forall (p : Params) (lrow : Row) (right : list Row),
    join_against (EConst (VBool true) TBool) p lrow right =
    Some (List.map (fun rrow => List.app lrow rrow) right).
Proof.
  intros p lrow right.
  induction right as [|rrow rs IH]; simpl; [reflexivity|].
  unfold eval_pred_keep, eval_pred, eval_expr; simpl.
  rewrite IH; reflexivity.
Qed.

Lemma join_inner_true_eq_cross_join_rows :
  forall (p : Params) (left right : list Row),
    join_inner (EConst (VBool true) TBool) p left right =
    Some (cross_join_rows left right).
Proof.
  intros p left right.
  revert right; induction left as [|lrow ls IH]; intros right; simpl.
  - reflexivity.
  - rewrite join_against_true_eq_map_app, IH.
    unfold cross_join_rows; simpl.
    reflexivity.
Qed.

(** [join_inner] with a total keep predicate is filter-of-cross in the same
    nested-loop order (exact multiplicity). *)
Lemma join_against_eq_filter_map_app :
  forall (cond : Pred) (p : Params) (lrow : Row) (right : list Row)
         (keep : Row -> bool),
    (forall rrow,
       eval_pred_keep cond p (List.app lrow rrow) =
         Some (keep (List.app lrow rrow))) ->
    join_against cond p lrow right =
    Some (List.filter keep
            (List.map (fun rrow => List.app lrow rrow) right)).
Proof.
  intros cond p lrow right keep Hkeep.
  induction right as [|rrow rs IH]; simpl; [reflexivity|].
  rewrite (Hkeep rrow), IH.
  destruct (keep (List.app lrow rrow)); reflexivity.
Qed.

Lemma join_inner_eq_filter_cross_join_rows :
  forall (cond : Pred) (p : Params) (left right : list Row)
         (keep : Row -> bool),
    (forall lrow rrow,
       List.In lrow left ->
       eval_pred_keep cond p (List.app lrow rrow) =
         Some (keep (List.app lrow rrow))) ->
    join_inner cond p left right =
    Some (List.filter keep (cross_join_rows left right)).
Proof.
  intros cond p left right keep Hkeep.
  revert right Hkeep; induction left as [|lrow ls IH]; intros right Hkeep; simpl.
  - reflexivity.
  - rewrite (join_against_eq_filter_map_app cond p lrow right keep).
    + rewrite (IH right).
      * unfold cross_join_rows; simpl.
        rewrite List.filter_app; reflexivity.
      * intros lrow' rrow Hin; apply Hkeep; right; exact Hin.
    + intros rrow; apply Hkeep; left; reflexivity.
Qed.

(** Membership-restricted variants: totality of [cond] is only required on
    actual left×right pairs (join conds with [EInputRef] into the right
    schema are undefined on arbitrary short right rows). *)
Lemma join_against_eq_filter_map_app_in :
  forall (cond : Pred) (p : Params) (lrow : Row) (right : list Row)
         (keep : Row -> bool),
    (forall rrow,
       List.In rrow right ->
       eval_pred_keep cond p (List.app lrow rrow) =
         Some (keep (List.app lrow rrow))) ->
    join_against cond p lrow right =
    Some (List.filter keep
            (List.map (fun rrow => List.app lrow rrow) right)).
Proof.
  intros cond p lrow right keep Hkeep.
  induction right as [|rrow rs IH]; simpl; [reflexivity|].
  rewrite (Hkeep rrow (or_introl eq_refl)), IH.
  - destruct (keep (List.app lrow rrow)); reflexivity.
  - intros r Hr; apply Hkeep; right; exact Hr.
Qed.

Lemma join_inner_eq_filter_cross_join_rows_in :
  forall (cond : Pred) (p : Params) (left right : list Row)
         (keep : Row -> bool),
    (forall lrow rrow,
       List.In lrow left ->
       List.In rrow right ->
       eval_pred_keep cond p (List.app lrow rrow) =
         Some (keep (List.app lrow rrow))) ->
    join_inner cond p left right =
    Some (List.filter keep (cross_join_rows left right)).
Proof.
  intros cond p left right keep Hkeep.
  induction left as [|lrow ls IH]; simpl.
  - reflexivity.
  - rewrite (join_against_eq_filter_map_app_in cond p lrow right keep).
    + rewrite IH.
      * unfold cross_join_rows; simpl.
        rewrite List.filter_app; reflexivity.
      * intros lrow' rrow Hin; apply Hkeep; right; exact Hin.
    + intros rrow Hr; apply Hkeep; [left; reflexivity|exact Hr].
Qed.

(** Membership in a RelNode Cartesian product. *)
Lemma In_cross_join_rows :
  forall (left right : list Row) (x : Row),
    List.In x (cross_join_rows left right) ->
    exists l r, List.In l left /\ List.In r right /\ x = List.app l r.
Proof.
  intros left right x Hin.
  unfold cross_join_rows in Hin.
  apply List.in_flat_map in Hin; destruct Hin as [l [Hl Hin]].
  apply List.in_map_iff in Hin; destruct Hin as [r [Heq Hr]].
  exists l, r; repeat split; auto.
Qed.

(** A filter on the left factor lifts through the product when the lifted
    keep agrees with it on every concatenated pair (exact multiplicity). *)
Lemma cross_join_rows_filter_left :
  forall (f g : Row -> bool) (left right : list Row),
    (forall l r, List.In l left -> List.In r right ->
       g (List.app l r) = f l) ->
    cross_join_rows (List.filter f left) right =
    List.filter g (cross_join_rows left right).
Proof.
  intros f g left right Hfg.
  unfold cross_join_rows, Row in *.
  induction left as [|l ls IH]; [reflexivity|].
  assert (IH' : cross_join_rows (List.filter f ls) right =
                List.filter g (cross_join_rows ls right)).
  { apply IH; intros l' r Hl' Hr; apply Hfg; [right; exact Hl'|exact Hr]. }
  assert (Hmap :
    List.filter g (List.map (fun r => List.app l r) right) =
    if f l then List.map (fun r => List.app l r) right else nil).
  { assert (Hr : forall r, List.In r right -> g (List.app l r) = f l).
    { intros r Hr; apply Hfg; [left; reflexivity|exact Hr]. }
    clear IH IH' Hfg.
    induction right as [|r rs IHr]; simpl.
    - destruct (f l); reflexivity.
    - rewrite (Hr r (or_introl eq_refl)).
      rewrite IHr by (intros r' Hr'; apply Hr; right; exact Hr').
      destruct (f l); reflexivity. }
  unfold cross_join_rows in IH'.
  simpl List.flat_map at 2.
  rewrite List.filter_app.
  rewrite Hmap, <- IH'.
  simpl List.filter.
  destruct (f l); reflexivity.
Qed.

(** * SemiJoin as an occurrence-preserving filter
    [join_semi_rows] with a condition total on actual left×right pairs keeps
    each left occurrence exactly once iff some right row matches; it never
    multiplies by the number of matches (SQL EXISTS multiplicity). *)
Lemma join_exists_right_eq_existsb :
  forall (cond : Pred) (p : Params) (lrow : Row) (right : list Row)
         (keep : Row -> bool),
    (forall rrow, List.In rrow right ->
       eval_pred_keep cond p (List.app lrow rrow) =
         Some (keep (List.app lrow rrow))) ->
    join_exists_right cond p lrow right =
    Some (List.existsb (fun rrow => keep (List.app lrow rrow)) right).
Proof.
  intros cond p lrow right keep Hkeep.
  induction right as [|rrow rs IH]; simpl; [reflexivity|].
  rewrite (Hkeep rrow (or_introl eq_refl)).
  destruct (keep (List.app lrow rrow)); simpl; [reflexivity|].
  apply IH; intros r Hr; apply Hkeep; right; exact Hr.
Qed.

Lemma join_semi_rows_eq_filter_existsb :
  forall (cond : Pred) (p : Params) (left right : list Row)
         (keep : Row -> bool),
    (forall lrow rrow,
       List.In lrow left -> List.In rrow right ->
       eval_pred_keep cond p (List.app lrow rrow) =
         Some (keep (List.app lrow rrow))) ->
    join_semi_rows cond p left right =
    Some (List.filter
            (fun lrow => List.existsb (fun rrow => keep (List.app lrow rrow)) right)
            left).
Proof.
  intros cond p left right keep Hkeep.
  induction left as [|lrow ls IH]; simpl; [reflexivity|].
  rewrite (join_exists_right_eq_existsb cond p lrow right keep)
    by (intros r Hr; apply Hkeep; [left; reflexivity|exact Hr]).
  rewrite IH by (intros l r Hl Hr; apply Hkeep; [right; exact Hl|exact Hr]).
  destruct (List.existsb (fun rrow => keep (List.app lrow rrow)) right); reflexivity.
Qed.

(** * AntiJoin as an occurrence-preserving filter
    [join_anti_rows] with a condition total on actual pairs keeps each left
    occurrence exactly once iff no right row matches (SQL NOT EXISTS). *)
Lemma join_anti_rows_eq_filter_negb_existsb :
  forall (cond : Pred) (p : Params) (left right : list Row)
         (keep : Row -> bool),
    (forall lrow rrow,
       List.In lrow left -> List.In rrow right ->
       eval_pred_keep cond p (List.app lrow rrow) =
         Some (keep (List.app lrow rrow))) ->
    join_anti_rows cond p left right =
    Some (List.filter
            (fun lrow =>
               negb (List.existsb (fun rrow => keep (List.app lrow rrow)) right))
            left).
Proof.
  intros cond p left right keep Hkeep.
  induction left as [|lrow ls IH]; simpl; [reflexivity|].
  rewrite (join_exists_right_eq_existsb cond p lrow right keep)
    by (intros r Hr; apply Hkeep; [left; reflexivity|exact Hr]).
  rewrite IH by (intros l r Hl Hr; apply Hkeep; [right; exact Hl|exact Hr]).
  destruct (List.existsb (fun rrow => keep (List.app lrow rrow)) right);
    reflexivity.
Qed.

(** Existence over a filtered list is existence of the conjunction. *)
Lemma existsb_filter_andb :
  forall (A : Type) (h g : A -> bool) (xs : list A),
    List.existsb g (List.filter h xs) =
    List.existsb (fun x => andb (h x) (g x)) xs.
Proof.
  intros A h g xs; induction xs as [|x xs IH]; simpl; [reflexivity|].
  destruct (h x); simpl; rewrite IH; reflexivity.
Qed.

(** Sequential filters fuse into one conjunctive filter. *)
Lemma filter_filter_andb :
  forall (A : Type) (f g : A -> bool) (xs : list A),
    List.filter g (List.filter f xs) =
    List.filter (fun x => andb (f x) (g x)) xs.
Proof.
  intros A f g xs; induction xs as [|x xs IH]; simpl; [reflexivity|].
  destruct (f x); simpl; [destruct (g x); simpl; rewrite IH|rewrite IH];
    reflexivity.
Qed.

(** * SemiJoinMorphism / AntiJoinMorphism
    USE WHEN: EXISTS / NOT EXISTS filters correspond under outer/right maps.
    REQUIRES: cond correspondence on left++right; SQL EXISTS multiplicity
              (outer row emitted at most once).
    PROVES:   BagRel (join_semi_rows / join_anti_rows) on left schemas.
    NOTE:    Cond InputRefs: left slots = outer (e.g. l1); right slots =
             semi/anti probe (l2/l3). Document query slot maps explicitly. *)

(** * FoldGroupsMorphism
    USE WHEN: grouped stores correspond under key + contribution maps.
    REQUIRES: key correspondence + Acc update homomorphism.
    PROVES:   store correspondence after fold_groups.
    NOTE: keys are semantic SqlValue tuples — never Pack.
          GroupStore is list (keys * Acc) with unique-key update (both provers). *)

(** * AggregateMorphism
    USE WHEN: finalize_acc / run_aggregate results correspond.
    REQUIRES: contribution/update homomorphism (FoldGroupsMorphism).
    PROVES:   agg bag / Row bag correspondence. *)

(** * OrderingMorphism
    USE WHEN: sorted sequences correspond.
    REQUIRES: same unique key domain + same values + strict sortedness.
    PROVES:   positional sequence correspondence (result_row_sort). *)

(** See PROOF_LIBRARY_INDEX.md and ScanCorrespond.v for FormalSQL→RelNode Scan. *)
