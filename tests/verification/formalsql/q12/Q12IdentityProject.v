(* Identity table projects — structural (=t= + bag), no cbn/vm_compute. *)
From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueInteger
  ValueTemporal ValueString FiniteSet FTuples OrderedSet Projection
  SqlQuerySyntax SqlQuerySemantics FiniteBag FiniteCollection Join.
From Logos Require Import FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax.
From LogosGenerated Require Import Schema Queries.
From Stdlib Require Import String List.
Import ListNotations Tuple.
Open Scope string_scope.

Require Import Q12NormalizeSound.
Require Import Q12EmbedDots.
Require Import Q12WhereAcceptance.
Require Import Q12JoinSpine.

Lemma orders_select_outputs :
  scalar_select_outputs Queries.scalar_select_list_3 = orders_attrs.
Proof. reflexivity. Qed.

Lemma lines_select_outputs :
  scalar_select_outputs Queries.scalar_select_list_4 = lineitem_attrs.
Proof. reflexivity. Qed.

Lemma q12_orders_dot_values_map_dot :
  forall o,
    q12_orders_dot_values (EmbedOrderRow o) =
    map (fun a => dot TNull (EmbedOrderRow o) a) orders_attrs.
Proof.
  intro o; unfold q12_orders_dot_values, orders_attrs; reflexivity.
Qed.

Lemma q12_lineitem_dot_values_map_dot :
  forall l,
    q12_lineitem_dot_values (EmbedLineRow l) =
    map (fun a => dot TNull (EmbedLineRow l) a) lineitem_attrs.
Proof.
  intro l; unfold q12_lineitem_dot_values, lineitem_attrs; reflexivity.
Qed.

Lemma find_combine_map_dot :
  forall (t : tuple TNull) attrs a,
    In a attrs ->
    Oset.find (OAtt TNull) a
      (combine attrs (map (fun a0 => dot TNull t a0) attrs)) =
    Some (dot TNull t a).
Proof.
  intros t attrs a Hin.
  induction attrs as [|a0 attrs IH]; [contradiction Hin|].
  cbn [map combine Oset.find Mem.find].
  destruct Hin as [Heq|Hin].
  - subst a0.
    rewrite (Oset.eq_bool_refl (OAtt TNull) a); reflexivity.
  - destruct (Oset.eq_bool (OAtt TNull) a a0) eqn:Heq.
    + apply Oset.eq_bool_true_iff in Heq; subst a0; reflexivity.
    + apply IH; exact Hin.
Qed.

Lemma mem_mk_set_In_attr :
  forall (attrs : list (attribute TNull)) a,
    a inS Fset.mk_set (A TNull) attrs <-> In a attrs.
Proof.
  intros attrs a.
  rewrite Fset.mem_mk_set.
  split.
  - intro H; apply Oset.mem_bool_true_iff in H; exact H.
  - intro H; apply Oset.mem_bool_true_iff; exact H.
Qed.

Lemma orders_project_id_t :
  forall o,
    project_row Queries.scalar_select_list_3
      (q12_orders_dot_values (EmbedOrderRow o)) =t=
    EmbedOrderRow o.
Proof.
  intro o.
  rewrite q12_orders_dot_values_map_dot.
  unfold project_row.
  rewrite orders_select_outputs.
  set (t := EmbedOrderRow o).
  set (attrs := orders_attrs).
  assert (Hlab : labels TNull t =S= Fset.mk_set (A TNull) attrs).
  { unfold t, attrs, EmbedOrderRow; apply mk_tuple_lists_labels. }
  set (f := fun a =>
    match Oset.find (OAtt TNull) a
            (combine attrs (map (fun a0 => dot TNull t a0) attrs)) with
    | Some v => v
    | None => default_value TNull (type_of_attribute TNull a)
    end).
  change (mk_tuple TNull (Fset.mk_set (A TNull) attrs) f =t= t).
  refine (Oeset.compare_eq_trans _ _ _ _
    (@mk_tuple_eq_2 TNull (Fset.mk_set (A TNull) attrs) f (dot TNull t) _)
    (@mk_tuple_idem TNull t (Fset.mk_set (A TNull) attrs) Hlab)).
  intros a Ha.
  assert (Hin : In a attrs).
  { apply (mem_mk_set_In_attr attrs a); exact Ha. }
  unfold f; rewrite (find_combine_map_dot t attrs a Hin); reflexivity.
Qed.

Lemma lines_project_id_t :
  forall l,
    project_row Queries.scalar_select_list_4
      (q12_lineitem_dot_values (EmbedLineRow l)) =t=
    EmbedLineRow l.
Proof.
  intro l.
  rewrite q12_lineitem_dot_values_map_dot.
  unfold project_row.
  rewrite lines_select_outputs.
  set (t := EmbedLineRow l).
  set (attrs := lineitem_attrs).
  assert (Hlab : labels TNull t =S= Fset.mk_set (A TNull) attrs).
  { unfold t, attrs, EmbedLineRow; apply mk_tuple_lists_labels. }
  set (f := fun a =>
    match Oset.find (OAtt TNull) a
            (combine attrs (map (fun a0 => dot TNull t a0) attrs)) with
    | Some v => v
    | None => default_value TNull (type_of_attribute TNull a)
    end).
  change (mk_tuple TNull (Fset.mk_set (A TNull) attrs) f =t= t).
  refine (Oeset.compare_eq_trans _ _ _ _
    (@mk_tuple_eq_2 TNull (Fset.mk_set (A TNull) attrs) f (dot TNull t) _)
    (@mk_tuple_idem TNull t (Fset.mk_set (A TNull) attrs) Hlab)).
  intros a Ha.
  assert (Hin : In a attrs).
  { apply (mem_mk_set_In_attr attrs a); exact Ha. }
  unfold f; rewrite (find_combine_map_dot t attrs a Hin); reflexivity.
Qed.

(** Pairwise =t= ⇒ bag equality of projected vs embed maps. *)
Lemma query_rows_bag_map_t_eq :
  forall (A : Type) (f g : A -> tuple TNull) (xs : list A),
    (forall x, In x xs -> f x =t= g x) ->
    query_rows_bag (map f xs) =BE= query_rows_bag (map g xs).
Proof.
  intros A f g xs Hf.
  unfold query_rows_bag.
  apply Febag.nb_occ_equal; intro y.
  rewrite !(Febag.nb_occ_mk_bag (Fecol.CBag (CTuple TNull)) y).
  revert xs Hf.
  induction xs as [|x xs IH]; intros Hf; [reflexivity|].
  cbn [map Oeset.nb_occ].
  assert (Ht : f x =t= g x) by (apply Hf; left; reflexivity).
  rewrite (Oeset.compare_eq_2 (OTuple TNull) y _ _ Ht).
  apply f_equal.
  apply IH; intros z Hz; apply Hf; right; exact Hz.
Qed.

Lemma proj_orders_bag_eq_embed :
  forall orders,
    query_rows_bag
      (map (fun row =>
        project_row Queries.scalar_select_list_3 (q12_orders_dot_values row))
        (map EmbedOrderRow orders))
    =BE=
    query_rows_bag (map EmbedOrderRow orders).
Proof.
  intro orders.
  rewrite map_map.
  apply query_rows_bag_map_t_eq.
  intros o _; apply orders_project_id_t.
Qed.

Lemma proj_lines_bag_eq_embed :
  forall lines,
    query_rows_bag
      (map (fun row =>
        project_row Queries.scalar_select_list_4 (q12_lineitem_dot_values row))
        (map EmbedLineRow lines))
    =BE=
    query_rows_bag (map EmbedLineRow lines).
Proof.
  intro lines.
  rewrite map_map.
  apply query_rows_bag_map_t_eq.
  intros l _; apply lines_project_id_t.
Qed.

Lemma filter_true_id :
  forall (A : Type) (l : list A),
    List.filter (fun _ : A => true) l = l.
Proof.
  induction l as [|x xs IH]; [reflexivity|cbn; f_equal; exact IH].
Qed.

Lemma q12_embed_join_eq_brute :
  forall orders lines,
    q12_embed_join_rows orders lines =
    @brute_left_join_list (tuple TNull) (join_tuple TNull)
      (map EmbedOrderRow orders) (map EmbedLineRow lines).
Proof.
  intros orders lines.
  unfold q12_embed_join_rows, brute_left_join_list, theta_join_list, d_join_list.
  induction orders as [|o os IH]; [reflexivity|].
  cbn [map flat_map].
  rewrite !filter_true_id in IH |- *.
  rewrite map_map, IH.
  reflexivity.
Qed.

Lemma query_cross_join_bag_congr :
  forall b1 b1' b2 b2',
    b1 =BE= b1' ->
    b2 =BE= b2' ->
    @query_cross_join_bag TNull b1 b2 =BE= @query_cross_join_bag TNull b1' b2'.
Proof.
  intros b1 b1' b2 b2' H1 H2.
  unfold query_cross_join_bag.
  apply Febag.nb_occ_equal; intro x.
  rewrite !(Febag.nb_occ_mk_bag (Fecol.CBag (CTuple TNull)) x).
  apply Oeset.permut_nb_occ.
  eapply (@theta_join_list_permut_eq (tuple TNull) (OTuple TNull)
            (join_tuple TNull) (@join_tuple_eq_1 TNull) (@join_tuple_eq_2 TNull)).
  - intros; reflexivity.
  - apply Oeset.nb_occ_permut; intro y.
    rewrite <- !(Febag.nb_occ_elements (Fecol.CBag (CTuple TNull))).
    apply (Febag.nb_occ_eq_2 (Fecol.CBag (CTuple TNull)) _ _ H1).
  - apply Oeset.nb_occ_permut; intro y.
    rewrite <- !(Febag.nb_occ_elements (Fecol.CBag (CTuple TNull))).
    apply (Febag.nb_occ_eq_2 (Fecol.CBag (CTuple TNull)) _ _ H2).
Qed.

(** Embed-join is a bag-legal CrossJoin of embed tables. *)
Lemma Q12_JoinBagOk_embed :
  forall orders lines,
    Q12_JoinBagOk
      (map EmbedOrderRow orders)
      (map EmbedLineRow lines)
      (q12_embed_join_rows orders lines).
Proof.
  intros orders lines.
  unfold Q12_JoinBagOk, query_same_rows_as_bag, query_cross_join_bag,
    query_rows_bag.
  rewrite q12_embed_join_eq_brute.
  apply Febag.nb_occ_equal; intro x.
  rewrite !(Febag.nb_occ_mk_bag (Fecol.CBag (CTuple TNull)) x).
  apply Oeset.permut_nb_occ.
  apply Oeset.permut_sym.
  eapply (@theta_join_list_permut_eq (tuple TNull) (OTuple TNull)
            (join_tuple TNull) (@join_tuple_eq_1 TNull) (@join_tuple_eq_2 TNull)).
  - intros; reflexivity.
  - apply Oeset.nb_occ_permut; intro y;
      rewrite <- (Febag.nb_occ_elements (Fecol.CBag (CTuple TNull))),
        (Febag.nb_occ_mk_bag (Fecol.CBag (CTuple TNull))); reflexivity.
  - apply Oeset.nb_occ_permut; intro y;
      rewrite <- (Febag.nb_occ_elements (Fecol.CBag (CTuple TNull))),
        (Febag.nb_occ_mk_bag (Fecol.CBag (CTuple TNull))); reflexivity.
Qed.

Lemma Q12_JoinBagOk_of_projects :
  forall orders lines,
    Q12_JoinBagOk
      (map (fun row =>
        project_row Queries.scalar_select_list_3 (q12_orders_dot_values row))
        (map EmbedOrderRow orders))
      (map (fun row =>
        project_row Queries.scalar_select_list_4 (q12_lineitem_dot_values row))
        (map EmbedLineRow lines))
      (q12_embed_join_rows orders lines).
Proof.
  intros orders lines.
  unfold Q12_JoinBagOk, query_same_rows_as_bag.
  pose proof (Q12_JoinBagOk_embed orders lines) as Hemb.
  unfold Q12_JoinBagOk, query_same_rows_as_bag in Hemb.
  refine (Febag.equal_trans (Fecol.CBag (CTuple TNull)) _ _ _ Hemb _).
  apply (Febag.equal_sym (Fecol.CBag (CTuple TNull))).
  apply query_cross_join_bag_congr.
  - apply proj_orders_bag_eq_embed.
  - apply proj_lines_bag_eq_embed.
Qed.
