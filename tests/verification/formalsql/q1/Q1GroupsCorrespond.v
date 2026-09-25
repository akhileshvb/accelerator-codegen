(* Q1 FormalSQL groups ↔ SpecView correspondence (C).

   Narrow residual [Q1_GroupsCorrespond]: ordered FormalSQL group-output rows
   equal map of AuxAnswerSpec embeds — NOT the outer-projected final list.
   Bag↔key building blocks below are Qed toward discharging this from
   [query_make_groups] ↔ [RowKey] + [NumericEmbedFaithful]. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueString
  SqlQuerySyntax SqlQuerySemantics SqlOrder SqlListFacts
  OrderedSet Env Interp FiniteBag FiniteSet FTuples Projection ListSort.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax
  FormalSQL.GroupingRewriteFacts FormalSQL.ProofAgentFacade
  FormalSQL.QueryCardinality.
From FormalSQLLib Require Import
  TemporalPredAcceptanceFacade KeyedSortFacade OrderRowsFacade
  ListNoDupFacade NumericFoldFacade SortedGroupResultFacade.

From JetStreamFS Require Import Q1AuxSpec.
From Stdlib Require Import String List ZArith Lia Permutation Ascii
  Sorting.Sorted Bool NArith.
Import ListNotations.
Import Tuple.
Open Scope Z_scope.

Require Import Q1Catalog Q1FormalSQLProof Q1GroupingCorrespondence
  Q1AuxCorrectAssembly Q1Obligations Q1FormalSQLSpine Q1NumericEmbed.

(** Group-output values from an Aux [OutRow] (same cells as [embed_outrow_values]). *)
Definition q1_group_values_of_OutRow (o : Q1AuxSpec.OutRow)
  : list NullValues.value :=
  embed_outrow_values o.

Definition q1_group_tuple_of_OutRow (o : Q1AuxSpec.OutRow) : tuple TNull :=
  project_row q1_group_select (q1_group_values_of_OutRow o).

(** Narrow correspondence (theorem hypothesis): ordered group observations
    match AuxAnswerSpec embeds.  Strictly weaker than final-row list-eq
    (outer project remains for D). *)
Definition Q1_GroupsCorrespond (db : list Q1AuxSpec.Row) (delta : Z) : Prop :=
  q1_ordered_rows_of db delta =
    map q1_group_tuple_of_OutRow
      (Q1AuxSpec.AuxAnswerSpec db (EmbedParams_Q1 delta)).

(** * Bag↔key building blocks (Qed) *)

Definition q1_key_values (gkey : Z) : list NullValues.value :=
  let '(rf, ls) := Q1AuxSpec.Unpack gkey in
  [embed_flag_char rf; embed_flag_char ls].

Definition q1_aux_bucket (db : list Q1AuxSpec.Row) (delta : Z) (gkey : Z)
  : list Q1AuxSpec.Row :=
  filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey)
    (q1_filtered_aux db delta).

Definition q1_inner_of_aux (rs : list Q1AuxSpec.Row) : list (tuple TNull) :=
  map (fun r => project_row q1_inner_select (q1_inner_values r)) rs.

Lemma q1_inner_rf_inS :
  forall r,
    q1_attr_rf inS labels TNull
      (project_row q1_inner_select (q1_inner_values r)).
Proof.
  intro r.
  apply q1_attr_inS_project_row, q1_inner_select_attr_inS_rf.
Qed.

Lemma q1_inner_ls_inS :
  forall r,
    q1_attr_ls inS labels TNull
      (project_row q1_inner_select (q1_inner_values r)).
Proof.
  intro r.
  apply q1_attr_inS_project_row, q1_inner_select_attr_inS_ls.
Qed.

(** Dot leaves of an Aux inner projection are the embed flag chars. *)
Lemma q1_inner_key_dots :
  forall r,
    dot TNull (project_row q1_inner_select (q1_inner_values r)) q1_attr_rf =
      embed_flag_char (Q1AuxSpec.rf r) /\
    dot TNull (project_row q1_inner_select (q1_inner_values r)) q1_attr_ls =
      embed_flag_char (Q1AuxSpec.ls r).
Proof.
  intro r; split.
  - apply q1_inner_project_dot_rf.
  - apply q1_inner_project_dot_ls.
Qed.

Lemma Unpack_Pack_RowOk :
  forall r, RowOk_Q1 r ->
    Q1AuxSpec.Unpack (Q1AuxSpec.RowKey r) =
    (Q1AuxSpec.rf r, Q1AuxSpec.ls r).
Proof.
  intros r Hrow.
  destruct Hrow as [_ [Hrf [Hls _]]].
  unfold Q1AuxSpec.Unpack, Q1AuxSpec.RowKey, Q1AuxSpec.Pack.
  f_equal.
  - rewrite Z.div_add_l by lia.
    rewrite (Z.div_small (Q1AuxSpec.ls r) 256) by lia.
    lia.
  - rewrite Z.add_comm.
    rewrite Z.mod_add by lia.
    rewrite Z.mod_small by lia.
    reflexivity.
Qed.

Lemma q1_key_values_of_row_ok :
  forall r, RowOk_Q1 r ->
    q1_key_values (Q1AuxSpec.RowKey r) =
    [embed_flag_char (Q1AuxSpec.rf r); embed_flag_char (Q1AuxSpec.ls r)].
Proof.
  intros r Hrow.
  unfold q1_key_values.
  rewrite Unpack_Pack_RowOk by exact Hrow.
  reflexivity.
Qed.

(** Kernel lookup: FormalSQL groups filtered by key = [rev bucket] or nil. *)
Lemma q1_groups_lookup_key_values :
  forall db delta gkey,
    filter
      (fun members =>
        match members with
        | nil => false
        | row :: _ =>
            Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
              (query_grouping_key nil q1_group_terms row)
              (q1_key_values gkey)
        end)
      (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) =
    match
      filter
        (fun row =>
          Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
            (query_grouping_key nil q1_group_terms row)
            (q1_key_values gkey))
        (q1_inner_proj_of db delta)
    with
    | nil => nil
    | _ :: _ =>
        [rev
          (filter
            (fun row =>
              Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
                (query_grouping_key nil q1_group_terms row)
                (q1_key_values gkey))
            (q1_inner_proj_of db delta))]
    end.
Proof.
  intros db delta gkey.
  exact (Q1_group_is_key_bucket nil (q1_inner_proj_of db delta)
           (q1_key_values gkey)).
Qed.

Lemma q1_inner_grouping_key :
  forall r,
    query_grouping_key nil q1_group_terms
      (project_row q1_inner_select (q1_inner_values r)) =
    [embed_flag_char (Q1AuxSpec.rf r); embed_flag_char (Q1AuxSpec.ls r)].
Proof.
  intro r.
  unfold query_grouping_key, q1_group_terms.
  cbn [map].
  unfold DotString.
  rewrite !(Interp.interp_aggterm_unfold TNull).
  unfold Dot, AExpr.
  rewrite !(Interp.interp_funterm_unfold TNull).
  change q1_attr_rf with (AttrString "l_returnflag" (StringChar 1)) in *.
  change q1_attr_ls with (AttrString "l_linestatus" (StringChar 1)) in *.
  rewrite (interp_dot_env_t nil
    (project_row q1_inner_select (q1_inner_values r))
    (AttrString "l_returnflag" (StringChar 1)) (q1_inner_rf_inS r)).
  rewrite (interp_dot_env_t nil
    (project_row q1_inner_select (q1_inner_values r))
    (AttrString "l_linestatus" (StringChar 1)) (q1_inner_ls_inS r)).
  change (AttrString "l_returnflag" (StringChar 1)) with q1_attr_rf.
  change (AttrString "l_linestatus" (StringChar 1)) with q1_attr_ls.
  rewrite q1_inner_project_dot_rf, q1_inner_project_dot_ls.
  reflexivity.
Qed.

Lemma q1_inner_grouping_key_RowKey :
  forall r, RowOk_Q1 r ->
    query_grouping_key nil q1_group_terms
      (project_row q1_inner_select (q1_inner_values r)) =
    q1_key_values (Q1AuxSpec.RowKey r).
Proof.
  intros r Hrow.
  rewrite q1_inner_grouping_key, q1_key_values_of_row_ok by exact Hrow.
  reflexivity.
Qed.

Lemma ascii_of_N_inj_byte :
  forall a b,
    0 <= a < 256 ->
    0 <= b < 256 ->
    ascii_of_N (Z.to_N a) = ascii_of_N (Z.to_N b) ->
    a = b.
Proof.
  intros a b Ha Hb Heq.
  assert (HaN : (Z.to_N a < 256)%N).
  { change 256%N with (Z.to_N 256). apply Z2N.inj_lt; lia. }
  assert (HbN : (Z.to_N b < 256)%N).
  { change 256%N with (Z.to_N 256). apply Z2N.inj_lt; lia. }
  apply (f_equal N_of_ascii) in Heq.
  rewrite !N_ascii_embedding in Heq by assumption.
  apply Z2N.inj in Heq; lia.
Qed.

Lemma embed_flag_char_inj :
  forall a b,
    0 <= a < 256 ->
    0 <= b < 256 ->
    embed_flag_char a = embed_flag_char b ->
    a = b.
Proof.
  intros a b Ha Hb Heq.
  unfold embed_flag_char in Heq.
  rewrite (Z.mod_small a 256) in Heq by lia.
  rewrite (Z.mod_small b 256) in Heq by lia.
  inversion Heq.
  apply ascii_of_N_inj_byte; assumption.
Qed.

Lemma Pack_inj_byte :
  forall rf1 ls1 rf2 ls2,
    0 <= ls1 < 256 ->
    0 <= ls2 < 256 ->
    Q1AuxSpec.Pack rf1 ls1 = Q1AuxSpec.Pack rf2 ls2 ->
    rf1 = rf2 /\ ls1 = ls2.
Proof.
  intros rf1 ls1 rf2 ls2 H1 H2 Heq.
  unfold Q1AuxSpec.Pack in Heq.
  assert (Hmod :
    Z.modulo (rf1 * 256 + ls1) 256 = Z.modulo (rf2 * 256 + ls2) 256)
    by (rewrite Heq; reflexivity).
  rewrite Z.add_comm, Z.mod_add in Hmod by lia.
  rewrite (Z.add_comm (rf2 * 256) ls2), Z.mod_add in Hmod by lia.
  rewrite !Z.mod_small in Hmod by lia.
  subst ls2.
  nia.
Qed.

Lemma RowKey_byte_bound :
  forall r, RowOk_Q1 r ->
    0 <= Q1AuxSpec.RowKey r < 256 * 256.
Proof.
  intros r Hrow.
  destruct Hrow as [_ [Hrf [Hls _]]].
  unfold Q1AuxSpec.RowKey, Q1AuxSpec.Pack.
  nia.
Qed.

Lemma Pack_Unpack_byte :
  forall gkey,
    0 <= gkey < 256 * 256 ->
    Q1AuxSpec.Pack (fst (Q1AuxSpec.Unpack gkey)) (snd (Q1AuxSpec.Unpack gkey))
      = gkey.
Proof.
  intros gkey Hg.
  unfold Q1AuxSpec.Pack, Q1AuxSpec.Unpack; cbn.
  pose proof (Z.div_mod gkey 256 ltac:(lia)) as Hdm.
  lia.
Qed.

Lemma q1_key_values_inj :
  forall g1 g2,
    0 <= g1 < 256 * 256 ->
    0 <= g2 < 256 * 256 ->
    q1_key_values g1 = q1_key_values g2 ->
    g1 = g2.
Proof.
  intros g1 g2 Hb1 Hb2 Heq.
  rewrite <- (Pack_Unpack_byte g1 Hb1), <- (Pack_Unpack_byte g2 Hb2).
  destruct (Q1AuxSpec.Unpack g1) as [rf1 ls1] eqn:U1.
  destruct (Q1AuxSpec.Unpack g2) as [rf2 ls2] eqn:U2.
  cbn [fst snd].
  assert (Heq' :
    [embed_flag_char rf1; embed_flag_char ls1] =
    [embed_flag_char rf2; embed_flag_char ls2]).
  { unfold q1_key_values in Heq; rewrite U1, U2 in Heq; exact Heq. }
  assert (Erf : embed_flag_char rf1 = embed_flag_char rf2).
  {
    pose proof (f_equal (fun l : list _ => nth 0 l (embed_flag_char 0)) Heq')
      as Hnth; cbn in Hnth; exact Hnth.
  }
  assert (Els : embed_flag_char ls1 = embed_flag_char ls2).
  {
    pose proof (f_equal (fun l : list _ => nth 1 l (embed_flag_char 0)) Heq')
      as Hnth; cbn in Hnth; exact Hnth.
  }
  assert (Hr1 : 0 <= rf1 < 256).
  {
    unfold Q1AuxSpec.Unpack in U1; injection U1 as E1 E2; subst rf1 ls1.
    split; [apply Z.div_pos; lia | apply Z.div_lt_upper_bound; lia].
  }
  assert (Hr2 : 0 <= rf2 < 256).
  {
    unfold Q1AuxSpec.Unpack in U2; injection U2 as E1 E2; subst rf2 ls2.
    split; [apply Z.div_pos; lia | apply Z.div_lt_upper_bound; lia].
  }
  assert (Hl1 : 0 <= ls1 < 256).
  {
    unfold Q1AuxSpec.Unpack in U1; injection U1 as E1 E2; subst rf1 ls1.
    apply Z.mod_pos_bound; lia.
  }
  assert (Hl2 : 0 <= ls2 < 256).
  {
    unfold Q1AuxSpec.Unpack in U2; injection U2 as E1 E2; subst rf2 ls2.
    apply Z.mod_pos_bound; lia.
  }
  apply embed_flag_char_inj in Erf; [|exact Hr1|exact Hr2].
  apply embed_flag_char_inj in Els; [|exact Hl1|exact Hl2].
  subst; reflexivity.
Qed.

Lemma q1_key_values_eq_byte :
  forall r gkey,
    RowOk_Q1 r ->
    0 <= gkey < 256 * 256 ->
    Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
      (q1_key_values (Q1AuxSpec.RowKey r))
      (q1_key_values gkey) =
    Z.eqb (Q1AuxSpec.RowKey r) gkey.
Proof.
  intros r gkey Hrow Hg.
  destruct (Z.eqb (Q1AuxSpec.RowKey r) gkey) eqn:Ek.
  - apply Z.eqb_eq in Ek; subst gkey.
    apply Oset.eq_bool_true_iff; reflexivity.
  - unfold q1_key_values.
    rewrite (Unpack_Pack_RowOk r Hrow).
    destruct (Q1AuxSpec.Unpack gkey) as [rf' ls'] eqn:Eu.
    cbn [fst snd].
    destruct (Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
      [embed_flag_char (Q1AuxSpec.rf r); embed_flag_char (Q1AuxSpec.ls r)]
      [embed_flag_char rf'; embed_flag_char ls']) eqn:Eeq.
    + apply Oset.eq_bool_true_iff in Eeq.
      destruct Hrow as [_ [Hrf [Hls _]]].
      assert (Hls'bnd : 0 <= ls' < 256).
      { change ls' with (snd (rf', ls')).
        rewrite <- Eu; unfold Q1AuxSpec.Unpack; cbn.
        apply Z.mod_pos_bound; lia. }
      assert (Hrf'bnd : 0 <= rf' < 256).
      { change rf' with (fst (rf', ls')).
        rewrite <- Eu; unfold Q1AuxSpec.Unpack; cbn.
        split; [apply Z.div_pos; lia|apply Z.div_lt_upper_bound; lia]. }
      apply (f_equal (fun vs => match vs with v::_ => v | _ => embed_flag_char 0 end)) in Eeq as Erf.
      apply (f_equal (fun vs => match vs with _::v::_ => v | _ => embed_flag_char 0 end)) in Eeq as Els.
      apply embed_flag_char_inj in Erf; [|lia|lia].
      apply embed_flag_char_inj in Els; [|lia|lia].
      subst rf' ls'.
      assert (Heq : Q1AuxSpec.RowKey r = gkey).
      { rewrite <- (Pack_Unpack_byte gkey Hg).
        unfold Q1AuxSpec.RowKey; rewrite Eu; cbn; reflexivity. }
      apply Z.eqb_neq in Ek; contradiction.
    + reflexivity.
Qed.

Lemma q1_grouping_eq_RowKey_byte :
  forall r gkey,
    RowOk_Q1 r ->
    0 <= gkey < 256 * 256 ->
    Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
      (query_grouping_key nil q1_group_terms
        (project_row q1_inner_select (q1_inner_values r)))
      (q1_key_values gkey) =
    Z.eqb (Q1AuxSpec.RowKey r) gkey.
Proof.
  intros r gkey Hrow Hg.
  rewrite q1_inner_grouping_key_RowKey by exact Hrow.
  apply q1_key_values_eq_byte; assumption.
Qed.

(* filter_map_ext_In: FormalSQLLib.ListNoDupFacade *)

Lemma q1_filter_inner_proj_by_key :
  forall db delta gkey,
    Preconditions_Q1 db delta ->
    0 <= gkey < 256 * 256 ->
    filter
      (fun row =>
        Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
          (query_grouping_key nil q1_group_terms row)
          (q1_key_values gkey))
      (q1_inner_proj_of db delta) =
    q1_inner_of_aux (q1_aux_bucket db delta gkey).
Proof.
  intros db delta gkey Hpre Hg.
  unfold q1_inner_proj_of, q1_inner_of_aux, q1_aux_bucket.
  erewrite (@filter_map_ext_In Q1AuxSpec.Row (tuple TNull));
    [|intros r Hin; apply q1_grouping_eq_RowKey_byte; [|exact Hg];
      eapply q1_filtered_aux_RowOk; eassumption].
  reflexivity.
Qed.

Lemma q1_groups_of_aux_bucket :
  forall db delta gkey,
    Preconditions_Q1 db delta ->
    0 <= gkey < 256 * 256 ->
    filter
      (fun members =>
        match members with
        | nil => false
        | row :: _ =>
            Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
              (query_grouping_key nil q1_group_terms row)
              (q1_key_values gkey)
        end)
      (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) =
    match q1_aux_bucket db delta gkey with
    | nil => nil
    | _ :: _ => [rev (q1_inner_of_aux (q1_aux_bucket db delta gkey))]
    end.
Proof.
  intros db delta gkey Hpre Hg.
  rewrite q1_groups_lookup_key_values.
  rewrite q1_filter_inner_proj_by_key by assumption.
  destruct (q1_aux_bucket db delta gkey); reflexivity.
Qed.

(* fold_right_Z_add_*: FormalSQLLib.NumericFoldFacade *)

Lemma length_rev_eq :
  forall (A : Type) (l : list A), length (rev l) = length l.
Proof. intros; apply length_rev. Qed.

Lemma q1_embed_f4_eq_inner :
  forall r, q1_embed_f4 r = NullValues.Value_numeric (Some (q1_f4_num r)).
Proof. reflexivity. Qed.

Lemma q1_embed_f5_eq_inner :
  forall r, q1_embed_f5 r = NullValues.Value_numeric (Some (q1_f5_num r)).
Proof. reflexivity. Qed.

Lemma q1_inner_dots_qty_map :
  forall rs,
    map (fun row => dot TNull row q1_attr_qty) (q1_inner_of_aux rs) =
    map (fun r => embed_decimal2 (Q1AuxSpec.qty r)) rs.
Proof.
  intros rs; unfold q1_inner_of_aux; rewrite map_map.
  apply map_ext; intro r; apply q1_inner_project_dot_qty.
Qed.

Lemma q1_inner_dots_price_map :
  forall rs,
    map (fun row => dot TNull row q1_attr_price) (q1_inner_of_aux rs) =
    map (fun r => embed_decimal2 (Q1AuxSpec.price r)) rs.
Proof.
  intros rs; unfold q1_inner_of_aux; rewrite map_map.
  apply map_ext; intro r; apply q1_inner_project_dot_price.
Qed.

Lemma q1_inner_dots_disc_map :
  forall rs,
    map (fun row => dot TNull row q1_attr_disc) (q1_inner_of_aux rs) =
    map (fun r => embed_decimal2 (Q1AuxSpec.disc r)) rs.
Proof.
  intros rs; unfold q1_inner_of_aux; rewrite map_map.
  apply map_ext; intro r; apply q1_inner_project_dot_disc.
Qed.

Lemma q1_inner_dots_f4_map :
  forall rs,
    map (fun row => dot TNull row q1_attr_f4) (q1_inner_of_aux rs) =
    map q1_embed_f4 rs.
Proof.
  intros rs; unfold q1_inner_of_aux; rewrite map_map.
  apply map_ext; intro r.
  rewrite q1_inner_project_dot_f4, q1_embed_f4_eq_inner; reflexivity.
Qed.

Lemma q1_inner_dots_f5_map :
  forall rs,
    map (fun row => dot TNull row q1_attr_f5) (q1_inner_of_aux rs) =
    map q1_embed_f5 rs.
Proof.
  intros rs; unfold q1_inner_of_aux; rewrite map_map.
  apply map_ext; intro r.
  rewrite q1_inner_project_dot_f5, q1_embed_f5_eq_inner; reflexivity.
Qed.

(** SpecView stats at [gkey] equal Z-folds over the Aux bucket. *)
Lemma q1_aux_bucket_count :
  forall db delta gkey,
    Q1AuxSpec.count
      (keystore_get
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
        gkey) =
    Z.of_nat (length (q1_aux_bucket db delta gkey)).
Proof.
  intros db delta gkey.
  unfold q1_aux_bucket, q1_filtered_aux.
  rewrite SpecView_FoldSpec_key_count.
  reflexivity.
Qed.

Lemma q1_aux_bucket_sum_qty :
  forall db delta gkey,
    Q1AuxSpec.sum_qty
      (keystore_get
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
        gkey) =
    fold_right Z.add 0 (map Q1AuxSpec.qty (q1_aux_bucket db delta gkey)).
Proof.
  intros db delta gkey.
  unfold q1_aux_bucket, q1_filtered_aux.
  rewrite SpecView_FoldSpec_key_sum_qty.
  reflexivity.
Qed.

Lemma q1_aux_bucket_sum_base :
  forall db delta gkey,
    Q1AuxSpec.sum_base_price
      (keystore_get
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
        gkey) =
    fold_right Z.add 0 (map Q1AuxSpec.price (q1_aux_bucket db delta gkey)).
Proof.
  intros db delta gkey.
  unfold q1_aux_bucket, q1_filtered_aux.
  rewrite SpecView_FoldSpec_key_sum_base_price.
  reflexivity.
Qed.

Lemma q1_aux_bucket_sum_disc_price :
  forall db delta gkey,
    Q1AuxSpec.sum_disc_price
      (keystore_get
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
        gkey) =
    fold_right Z.add 0 (map q1_row_disc_price (q1_aux_bucket db delta gkey)).
Proof.
  intros db delta gkey.
  unfold q1_aux_bucket, q1_filtered_aux, q1_row_disc_price.
  rewrite SpecView_FoldSpec_key_sum_disc_price.
  reflexivity.
Qed.

Lemma q1_aux_bucket_sum_charge :
  forall db delta gkey,
    Q1AuxSpec.sum_charge
      (keystore_get
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
        gkey) =
    fold_right Z.add 0 (map q1_row_charge (q1_aux_bucket db delta gkey)).
Proof.
  intros db delta gkey.
  unfold q1_aux_bucket, q1_filtered_aux, q1_row_charge, q1_row_disc_price.
  rewrite SpecView_FoldSpec_key_sum_charge.
  reflexivity.
Qed.

Lemma q1_aux_bucket_sum_discount :
  forall db delta gkey,
    Q1AuxSpec.sum_discount
      (keystore_get
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
        gkey) =
    fold_right Z.add 0 (map Q1AuxSpec.disc (q1_aux_bucket db delta gkey)).
Proof.
  intros db delta gkey.
  unfold q1_aux_bucket, q1_filtered_aux.
  rewrite SpecView_FoldSpec_key_sum_discount.
  reflexivity.
Qed.


(** * Group_By key dots + Faithful on rev buckets *)

Lemma q1_group_env_dot_exists :
  forall (attr : attribute TNull) (group : list (tuple TNull)),
    group <> nil ->
    Forall (fun row => attr inS labels TNull row) group ->
    exists t,
      In t group /\
      Interp.interp_dot TNull (q1_group_env group) attr = dot TNull t attr.
Proof.
  intros attr group Hne Hfa.
  unfold q1_group_env, Env.env_g.
  rewrite (Interp.interp_dot_unfold TNull (_ :: nil) attr).
  destruct (@quicksort _ (OTuple TNull) group) as [|t rest] eqn:Q.
  - assert (Hlen : length (@quicksort _ (OTuple TNull) group) = length group)
      by apply (@length_quicksort _ (OTuple TNull)).
    rewrite Q in Hlen; cbn in Hlen.
    destruct group; [contradiction Hne; reflexivity|discriminate].
  - exists t; split.
    + apply (proj2 (@In_quicksort _ (OTuple TNull) group t)).
      rewrite Q; left; reflexivity.
    + assert (HinS : attr inS? labels TNull t = true).
      { pose proof (proj2 (@In_quicksort _ (OTuple TNull) group t)) as Hin.
        rewrite Q in Hin; specialize (Hin (or_introl eq_refl)).
        apply (proj1 (Forall_forall _ _) Hfa) in Hin; exact Hin. }
      rewrite HinS; reflexivity.
Qed.

Lemma q1_group_env_dot_uniform :
  forall (attr : attribute TNull) (group : list (tuple TNull)) (v : NullValues.value),
    group <> nil ->
    Forall (fun row => attr inS labels TNull row /\ dot TNull row attr = v) group ->
    Interp.interp_dot TNull (q1_group_env group) attr = v.
Proof.
  intros attr group v Hne Hfa.
  assert (Hlab : Forall (fun row => attr inS labels TNull row) group).
  { eapply Forall_impl; [|exact Hfa]; intros row [Hin _]; exact Hin. }
  destruct (q1_group_env_dot_exists attr group Hne Hlab) as [t [Hin Heq]].
  rewrite Heq.
  pose proof (proj1 (Forall_forall _ _) Hfa t Hin) as [_ Hv]; exact Hv.
Qed.

Lemma Forall_map_RowOk_aux_bucket :
  forall db delta gkey,
    Preconditions_Q1 db delta ->
    Forall RowOk_Q1 (q1_aux_bucket db delta gkey).
Proof.
  intros db delta gkey Hpre.
  unfold q1_aux_bucket.
  apply Forall_forall; intros r Hin.
  apply filter_In in Hin; destruct Hin as [Hin _].
  eapply q1_filtered_aux_RowOk; eassumption.
Qed.

Lemma Forall_aux_bucket_RowKey :
  forall db delta gkey,
    Forall (fun r => Q1AuxSpec.RowKey r = gkey) (q1_aux_bucket db delta gkey).
Proof.
  intros db delta gkey.
  unfold q1_aux_bucket.
  apply Forall_forall; intros r Hin.
  apply filter_In in Hin; destruct Hin as [_ Heq].
  apply Z.eqb_eq in Heq; exact Heq.
Qed.

Lemma q1_inner_of_aux_rev :
  forall rs,
    q1_inner_of_aux (rev rs) = rev (q1_inner_of_aux rs).
Proof.
  intro rs; unfold q1_inner_of_aux; apply map_rev.
Qed.

Lemma q1_dots_rev_inner_qty :
  forall rs,
    map (fun row => dot TNull row q1_attr_qty)
      (rev (q1_inner_of_aux rs)) =
    map (fun r => embed_decimal2 (Q1AuxSpec.qty r)) (rev rs).
Proof.
  intro rs.
  rewrite <- q1_inner_of_aux_rev, q1_inner_dots_qty_map; reflexivity.
Qed.

Lemma q1_dots_rev_inner_price :
  forall rs,
    map (fun row => dot TNull row q1_attr_price)
      (rev (q1_inner_of_aux rs)) =
    map (fun r => embed_decimal2 (Q1AuxSpec.price r)) (rev rs).
Proof.
  intro rs.
  rewrite <- q1_inner_of_aux_rev, q1_inner_dots_price_map; reflexivity.
Qed.

Lemma q1_dots_rev_inner_disc :
  forall rs,
    map (fun row => dot TNull row q1_attr_disc)
      (rev (q1_inner_of_aux rs)) =
    map (fun r => embed_decimal2 (Q1AuxSpec.disc r)) (rev rs).
Proof.
  intro rs.
  rewrite <- q1_inner_of_aux_rev, q1_inner_dots_disc_map; reflexivity.
Qed.

Lemma q1_dots_rev_inner_f4 :
  forall rs,
    map (fun row => dot TNull row q1_attr_f4)
      (rev (q1_inner_of_aux rs)) =
    map q1_embed_f4 (rev rs).
Proof.
  intro rs.
  rewrite <- q1_inner_of_aux_rev, q1_inner_dots_f4_map; reflexivity.
Qed.

Lemma q1_dots_rev_inner_f5 :
  forall rs,
    map (fun row => dot TNull row q1_attr_f5)
      (rev (q1_inner_of_aux rs)) =
    map q1_embed_f5 (rev rs).
Proof.
  intro rs.
  rewrite <- q1_inner_of_aux_rev, q1_inner_dots_f5_map; reflexivity.
Qed.

Lemma q1_rev_inner_key_dots :
  forall rs gkey,
    Forall RowOk_Q1 rs ->
    Forall (fun r => Q1AuxSpec.RowKey r = gkey) rs ->
    rs <> nil ->
    let group := rev (q1_inner_of_aux rs) in
    Interp.interp_dot TNull (q1_group_env group) q1_attr_rf =
      embed_flag_char (fst (Q1AuxSpec.Unpack gkey)) /\
    Interp.interp_dot TNull (q1_group_env group) q1_attr_ls =
      embed_flag_char (snd (Q1AuxSpec.Unpack gkey)).
Proof.
  intros rs gkey Hok Hkeys Hne.
  set (group := rev (q1_inner_of_aux rs)).
  assert (Hne' : group <> nil).
  { intro Hnil.
    unfold group in Hnil.
    apply (f_equal (@length _)) in Hnil; cbn in Hnil.
    rewrite length_rev in Hnil.
    unfold q1_inner_of_aux in Hnil; rewrite length_map in Hnil.
    destruct rs; [contradiction Hne; reflexivity|cbn in Hnil; lia]. }
  assert (Hfa_rf :
    Forall (fun row =>
      q1_attr_rf inS labels TNull row /\
      dot TNull row q1_attr_rf = embed_flag_char (fst (Q1AuxSpec.Unpack gkey)))
      group).
  { unfold group.
    rewrite <- q1_inner_of_aux_rev.
    unfold q1_inner_of_aux.
    apply Forall_forall; intros row Hin.
    apply in_map_iff in Hin; destruct Hin as [r [Heq Hinr]].
    subst row.
    apply in_rev in Hinr.
    pose proof (proj1 (Forall_forall _ _) Hok r Hinr) as Hrow.
    pose proof (proj1 (Forall_forall _ _) Hkeys r Hinr) as Hk.
    split; [apply q1_inner_rf_inS|].
    rewrite q1_inner_project_dot_rf.
    rewrite <- Hk, (Unpack_Pack_RowOk r Hrow); cbn; reflexivity. }
  assert (Hfa_ls :
    Forall (fun row =>
      q1_attr_ls inS labels TNull row /\
      dot TNull row q1_attr_ls = embed_flag_char (snd (Q1AuxSpec.Unpack gkey)))
      group).
  { unfold group.
    rewrite <- q1_inner_of_aux_rev.
    unfold q1_inner_of_aux.
    apply Forall_forall; intros row Hin.
    apply in_map_iff in Hin; destruct Hin as [r [Heq Hinr]].
    subst row.
    apply in_rev in Hinr.
    pose proof (proj1 (Forall_forall _ _) Hok r Hinr) as Hrow.
    pose proof (proj1 (Forall_forall _ _) Hkeys r Hinr) as Hk.
    split; [apply q1_inner_ls_inS|].
    rewrite q1_inner_project_dot_ls.
    rewrite <- Hk, (Unpack_Pack_RowOk r Hrow); cbn; reflexivity. }
  split.
  - apply q1_group_env_dot_uniform; assumption.
  - apply q1_group_env_dot_uniform; assumption.
Qed.


Definition q1_outrow_of_stats (gkey : Z) (s : Q1AuxSpec.Stats) : Q1AuxSpec.OutRow :=
  let '(rf, ls) := Q1AuxSpec.Unpack gkey in
  Q1AuxSpec.mkOutRow rf ls
    (Q1AuxSpec.sum_qty s) (Q1AuxSpec.sum_base_price s)
    (Q1AuxSpec.sum_disc_price s) (Q1AuxSpec.sum_charge s)
    (Q1AuxSpec.sum_discount s)
    (Q1AuxSpec.AvgRound (Q1AuxSpec.sum_qty s) (Q1AuxSpec.count s))
    (Q1AuxSpec.AvgRound (Q1AuxSpec.sum_base_price s) (Q1AuxSpec.count s))
    (Q1AuxSpec.AvgRound (Q1AuxSpec.sum_discount s) (Q1AuxSpec.count s))
    (Q1AuxSpec.count s).

Lemma SpecFinalizeOne_of_positive :
  forall gkey s,
    0 < Q1AuxSpec.count s ->
    Q1AuxSpec.SpecFinalizeOne gkey s = [q1_outrow_of_stats gkey s].
Proof.
  intros gkey s Hpos.
  unfold Q1AuxSpec.SpecFinalizeOne, q1_outrow_of_stats.
  destruct (Z.leb (Q1AuxSpec.count s) 0) eqn:Ele.
  - apply Z.leb_le in Ele; lia.
  - destruct (Q1AuxSpec.Unpack gkey); reflexivity.
Qed.

Lemma q1_group_select_values_of_rev_bucket :
  forall db delta gkey,
    Preconditions_Q1 db delta ->
    NumericEmbedFaithful ->
    0 <= gkey < 256 * 256 ->
    q1_aux_bucket db delta gkey <> nil ->
    q1_group_select_values
      (rev (q1_inner_of_aux (q1_aux_bucket db delta gkey))) =
    q1_group_values_of_OutRow
      (q1_outrow_of_stats gkey
        (keystore_get
          (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
          gkey)).
Proof.
  intros db delta gkey Hpre Hnum Hg Hne.
  set (rs := q1_aux_bucket db delta gkey).
  set (s := keystore_get
    (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta)) gkey).
  fold rs; fold s.
  assert (Hok : Forall RowOk_Q1 rs).
  { unfold rs; apply Forall_map_RowOk_aux_bucket; exact Hpre. }
  assert (Hkeys : Forall (fun r => Q1AuxSpec.RowKey r = gkey) rs).
  { unfold rs; apply Forall_aux_bucket_RowKey. }
  assert (Hne' : rs <> nil) by (unfold rs; exact Hne).
  destruct (q1_rev_inner_key_dots rs gkey Hok Hkeys Hne') as [Hrf Hls].
  destruct (Hnum (rev rs)) as [Hsum_qty [Hsum_price [Hsum_f4 [Hsum_f5
    [Havg_qty [Havg_price Havg_disc]]]]]].
  assert (Hqty :
    fold_right Z.add 0 (map Q1AuxSpec.qty rs) = Q1AuxSpec.sum_qty s).
  { unfold rs, s; symmetry; apply q1_aux_bucket_sum_qty. }
  assert (Hbase :
    fold_right Z.add 0 (map Q1AuxSpec.price rs) = Q1AuxSpec.sum_base_price s).
  { unfold rs, s; symmetry; apply q1_aux_bucket_sum_base. }
  assert (Hf4 :
    fold_right Z.add 0 (map q1_row_disc_price rs) = Q1AuxSpec.sum_disc_price s).
  { unfold rs, s; symmetry; apply q1_aux_bucket_sum_disc_price. }
  assert (Hf5 :
    fold_right Z.add 0 (map q1_row_charge rs) = Q1AuxSpec.sum_charge s).
  { unfold rs, s; symmetry; apply q1_aux_bucket_sum_charge. }
  assert (Hdisc :
    fold_right Z.add 0 (map Q1AuxSpec.disc rs) = Q1AuxSpec.sum_discount s).
  { unfold rs, s; symmetry; apply q1_aux_bucket_sum_discount. }
  assert (Hcnt : Z.of_nat (length rs) = Q1AuxSpec.count s).
  { unfold rs, s; symmetry; apply q1_aux_bucket_count. }
  unfold q1_group_select_values.
  rewrite Hrf, Hls.
  rewrite !q1_dots_rev_inner_qty, !q1_dots_rev_inner_price,
    !q1_dots_rev_inner_f4, !q1_dots_rev_inner_f5, !q1_dots_rev_inner_disc.
  rewrite Hsum_qty, fold_right_Z_add_rev, Hqty.
  rewrite Hsum_price, fold_right_Z_add_rev, Hbase.
  rewrite Hsum_f4, fold_right_Z_add_rev, Hf4.
  rewrite Hsum_f5, fold_right_Z_add_rev, Hf5.
  rewrite Havg_qty, fold_right_Z_add_rev, length_rev_eq, Hqty, Hcnt.
  rewrite Havg_price, fold_right_Z_add_rev, length_rev_eq, Hbase, Hcnt.
  rewrite Havg_disc, fold_right_Z_add_rev, length_rev_eq, Hdisc, Hcnt.
  rewrite length_rev_eq.
  unfold q1_inner_of_aux; rewrite length_map, Hcnt.
  unfold q1_group_values_of_OutRow, q1_outrow_of_stats, embed_outrow_values.
  destruct (Q1AuxSpec.Unpack gkey) as [rf ls]; cbn.
  reflexivity.
Qed.
(** * Per-bucket group tuple = OutRow embed *)

Lemma q1_group_tuple_of_rev_bucket :
  forall db delta gkey,
    Preconditions_Q1 db delta ->
    NumericEmbedFaithful ->
    0 <= gkey < 256 * 256 ->
    q1_aux_bucket db delta gkey <> nil ->
    project_row q1_group_select
      (q1_group_select_values
        (rev (q1_inner_of_aux (q1_aux_bucket db delta gkey)))) =
    q1_group_tuple_of_OutRow
      (q1_outrow_of_stats gkey
        (keystore_get
          (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
          gkey)).
Proof.
  intros db delta gkey Hpre Hnum Hg Hne.
  unfold q1_group_tuple_of_OutRow.
  rewrite q1_group_select_values_of_rev_bucket by assumption.
  reflexivity.
Qed.

Lemma q1_aux_bucket_nonempty_iff_count_pos :
  forall db delta gkey,
    q1_aux_bucket db delta gkey <> nil <->
    0 < Q1AuxSpec.count
      (keystore_get
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
        gkey).
Proof.
  intros db delta gkey.
  rewrite q1_aux_bucket_count.
  split; intro H.
  - destruct (q1_aux_bucket db delta gkey); [contradiction H; reflexivity|cbn; lia].
  - intro Hnil; rewrite Hnil in H; cbn in H; lia.
Qed.

Lemma SpecFinalize_map_of_positive :
  forall entries,
    Forall (fun '(gkey, s) => 0 < Q1AuxSpec.count s) entries ->
    Q1AuxSpec.SpecFinalize entries =
    map (fun '(gkey, s) => q1_outrow_of_stats gkey s) entries.
Proof.
  induction entries as [|[gkey s] rest IH]; intros Hfa; [reflexivity|].
  inversion Hfa as [|? ? Hpos Hfa']; subst.
  cbn [Q1AuxSpec.SpecFinalize map].
  rewrite SpecFinalizeOne_of_positive by exact Hpos.
  rewrite IH by exact Hfa'.
  reflexivity.
Qed.

Lemma SpecMerge_count_add :
  forall a b,
    Q1AuxSpec.count (Q1AuxSpec.SpecMerge a b) =
    Q1AuxSpec.count a + Q1AuxSpec.count b.
Proof. intros [] []; reflexivity. Qed.

Lemma keystore_put_In_count_pos :
  forall acc gkey s gkey' s',
    0 < Q1AuxSpec.count s ->
    Forall (fun '( _k, s0) => 0 < Q1AuxSpec.count s0) acc ->
    In (gkey', s') (Q1AuxSpec.keystore_put acc gkey s) ->
    0 < Q1AuxSpec.count s'.
Proof.
  induction acc as [|[k s0] rest IH]; intros gkey s gkey' s' Hs Hfa Hin.
  - cbn in Hin; destruct Hin as [Heq|[]]; inversion Heq; subst; exact Hs.
  - inversion Hfa as [|? ? Hs0 Hfa']; subst.
    cbn [Q1AuxSpec.keystore_put] in Hin.
    destruct (Z.eqb k gkey) eqn:Ek.
    + destruct Hin as [Heq|Hin'].
      * inversion Heq; subst.
        rewrite SpecMerge_count_add; lia.
      * exact (proj1 (Forall_forall _ _) Hfa' (gkey', s') Hin').
    + destruct Hin as [Heq|Hin'].
      * inversion Heq; subst; exact Hs0.
      * apply (IH gkey s gkey' s' Hs Hfa' Hin').
Qed.

Lemma FoldSpec_store_In_count_pos :
  forall db day gkey s,
    In ((day, gkey), s) (Q1AuxSpec.FoldSpec db) ->
    0 < Q1AuxSpec.count s.
Proof.
  induction db as [|r rest IH]; intros day gkey s Hin; [contradiction|].
  rewrite FoldSpec_cons_eq in Hin.
  cbn [In] in Hin.
  destruct Hin as [Heq|Hin].
  - inversion Heq; subst.
    pose proof (FoldSpec_store_get_count_nonneg rest
      (Q1AuxSpec.shipdate r, Q1AuxSpec.RowKey r)) as Hnn.
    unfold Q1AuxSpec.SpecApply.
    destruct (Q1AuxSpec.store_get (Q1AuxSpec.FoldSpec rest)
      (Q1AuxSpec.shipdate r, Q1AuxSpec.RowKey r)) as [c q b d ch di].
    cbn in Hnn |- *; lia.
  - apply filter_In in Hin; destruct Hin as [Hin _].
    eapply IH; exact Hin.
Qed.

Lemma SpecView_FoldSpec_In_count_pos :
  forall db cutoff gkey s,
    In (gkey, s)
      (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) cutoff) ->
    0 < Q1AuxSpec.count s.
Proof.
  intros db cutoff.
  revert db.
  (* Induct on the store list shape of FoldSpec db *)
  intros db gkey s.
  set (st := Q1AuxSpec.FoldSpec db).
  assert (Hst : forall day k s0, In ((day, k), s0) st -> 0 < Q1AuxSpec.count s0).
  { subst st; apply FoldSpec_store_In_count_pos. }
  clearbody st. clear db.
  revert gkey s; induction st as [|[[day k] s0] rest IH];
    intros gkey s Hin; [contradiction|].
  cbn [Q1AuxSpec.SpecView] in Hin.
  assert (Hs0 : 0 < Q1AuxSpec.count s0)
    by (apply (Hst day k); left; reflexivity).
  assert (Hrest : forall day' k' s', In ((day', k'), s') rest ->
                    0 < Q1AuxSpec.count s').
  { intros day' k' s' Hin'; apply (Hst day' k'); right; exact Hin'. }
  destruct (Z.leb day cutoff) eqn:Ele.
  - assert (Hfa : Forall (fun '(_k, s1) => 0 < Q1AuxSpec.count s1)
                    (Q1AuxSpec.SpecView rest cutoff)).
    { apply Forall_forall; intros [gk sx] Hinx.
      specialize (IH Hrest) as IHrest.
      apply (IHrest gk sx Hinx). }
    eapply keystore_put_In_count_pos; [exact Hs0|exact Hfa|exact Hin].
  - specialize (IH Hrest) as IHrest.
    apply (IHrest gkey s Hin).
Qed.

Lemma SpecFinalize_SpecView_FoldSpec :
  forall db cutoff,
    Q1AuxSpec.SpecFinalize
      (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) cutoff) =
    map (fun '(gkey, s) => q1_outrow_of_stats gkey s)
      (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) cutoff).
Proof.
  intros db cutoff.
  apply SpecFinalize_map_of_positive.
  apply Forall_forall; intros [gkey s] Hin.
  eapply SpecView_FoldSpec_In_count_pos; exact Hin.
Qed.

Lemma SpecFinalizeOne_of_nonempty_bucket :
  forall db delta gkey,
    q1_aux_bucket db delta gkey <> nil ->
    Q1AuxSpec.SpecFinalizeOne gkey
      (keystore_get
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
        gkey) =
    [q1_outrow_of_stats gkey
      (keystore_get
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
        gkey)].
Proof.
  intros db delta gkey Hne.
  apply SpecFinalizeOne_of_positive.
  rewrite q1_aux_bucket_count.
  destruct (q1_aux_bucket db delta gkey); [contradiction Hne; reflexivity|cbn; lia].
Qed.

Lemma q1_group_tuple_dot_rf :
  forall o,
    dot TNull (q1_group_tuple_of_OutRow o)
      (AttrString "l_returnflag" (StringChar 1)) =
    embed_flag_char (Q1AuxSpec.out_rf o).
Proof.
  intro o.
  unfold q1_group_tuple_of_OutRow, project_row, scalar_select_outputs,
    q1_group_select, Queries.scalar_select_list_1, q1_group_values_of_OutRow,
    embed_outrow_values.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_group_tuple_dot_ls :
  forall o,
    dot TNull (q1_group_tuple_of_OutRow o)
      (AttrString "l_linestatus" (StringChar 1)) =
    embed_flag_char (Q1AuxSpec.out_ls o).
Proof.
  intro o.
  unfold q1_group_tuple_of_OutRow, project_row, scalar_select_outputs,
    q1_group_select, Queries.scalar_select_list_1, q1_group_values_of_OutRow,
    embed_outrow_values.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma Ncompare_ZtoN :
  forall a b,
    0 <= a ->
    0 <= b ->
    (Z.to_N a ?= Z.to_N b)%N =
    if Z.eqb a b then Eq else if Z.ltb a b then Lt else Gt.
Proof.
  intros a b Ha Hb.
  destruct (Z.eqb a b) eqn:Heq.
  - apply Z.eqb_eq in Heq; subst.
    rewrite N.compare_refl; reflexivity.
  - apply Z.eqb_neq in Heq.
    destruct (Z.ltb a b) eqn:Hlt.
    + apply Z.ltb_lt in Hlt.
      apply N.compare_lt_iff.
      apply Z2N.inj_lt; lia.
    + apply Z.ltb_ge in Hlt.
      assert (Hb_lt : b < a) by lia.
      apply N.compare_gt_iff.
      apply Z2N.inj_lt; lia.
Qed.

Lemma embed_flag_char_compare :
  forall a b,
    0 <= a < 256 ->
    0 <= b < 256 ->
    NullValues.sql_order_value_compare
      (embed_flag_char a) (embed_flag_char b) =
    if Z.eqb a b then Eq else if Z.ltb a b then Lt else Gt.
Proof.
  intros a b Ha Hb.
  unfold embed_flag_char.
  rewrite !Z.mod_small by lia.
  unfold NullValues.sql_order_value_compare; cbn [Oset.compare].
  cbn.
  rewrite !N_ascii_embedding.
  2-3: change 256%N with (Z.to_N 256); apply Z2N.inj_lt; lia.
  rewrite (Ncompare_ZtoN a b); [|lia|lia].
  destruct (Z.eqb a b); [|destruct (Z.ltb a b)]; reflexivity.
Qed.

Lemma OutRow_rf_ls_byte :
  forall gkey s,
    0 <= gkey < 256 * 256 ->
    let o := q1_outrow_of_stats gkey s in
    0 <= Q1AuxSpec.out_rf o < 256 /\
    0 <= Q1AuxSpec.out_ls o < 256.
Proof.
  intros gkey s Hg; cbn.
  unfold q1_outrow_of_stats.
  destruct (Q1AuxSpec.Unpack gkey) as [rf ls] eqn:Eu; cbn.
  unfold Q1AuxSpec.Unpack in Eu; injection Eu as Erf Els; subst.
  split.
  - split; [apply Z.div_pos; lia|apply Z.div_lt_upper_bound; lia].
  - apply Z.mod_pos_bound; lia.
Qed.

Lemma compare_order_keys_group_tuple_OutRowLe :
  forall o1 o2,
    0 <= Q1AuxSpec.out_rf o1 < 256 ->
    0 <= Q1AuxSpec.out_ls o1 < 256 ->
    0 <= Q1AuxSpec.out_rf o2 < 256 ->
    0 <= Q1AuxSpec.out_ls o2 < 256 ->
    @compare_order_keys TNull value_is_null q1_order_keys
      (q1_group_tuple_of_OutRow o1) (q1_group_tuple_of_OutRow o2) =
    if Q1AuxSpec.OutRowLe o1 o2
    then if Q1AuxSpec.OutRowLe o2 o1 then Eq else Lt
    else Gt.
Proof.
  intros o1 o2 Hr1 Hl1 Hr2 Hl2.
  cbn [compare_order_keys q1_order_keys].
  unfold SortAscNullsLast, SortKey, compare_order_key.
  simpl sort_key_attribute.
  simpl sort_key_direction.
  simpl sort_key_null_direction.
  simpl sort_key_value_compare.
  rewrite (q1_group_tuple_dot_rf o1), (q1_group_tuple_dot_rf o2),
          (q1_group_tuple_dot_ls o1), (q1_group_tuple_dot_ls o2).
  unfold compare_order_value, value_is_null.
  assert (Hnr1 : NullValues.is_null_value (embed_flag_char (Q1AuxSpec.out_rf o1)) = false)
    by (vm_compute; reflexivity).
  assert (Hnr2 : NullValues.is_null_value (embed_flag_char (Q1AuxSpec.out_rf o2)) = false)
    by (vm_compute; reflexivity).
  rewrite Hnr1, Hnr2.
  rewrite (embed_flag_char_compare _ _ Hr1 Hr2).
  unfold Q1AuxSpec.OutRowLe.
  destruct (Z.eqb (Q1AuxSpec.out_rf o1) (Q1AuxSpec.out_rf o2)) eqn:Erf.
  - (* rf equal: simpl reduces ls compare to N.compare of mods *)
    simpl.
    unfold embed_flag_char.
    rewrite !Z.mod_small by lia.
    rewrite !N_ascii_embedding.
    2-3: change 256%N with (Z.to_N 256); apply Z2N.inj_lt; lia.
    rewrite (Ncompare_ZtoN _ _ (proj1 Hl1) (proj1 Hl2)).
    destruct (Z.eqb (Q1AuxSpec.out_ls o1) (Q1AuxSpec.out_ls o2)) eqn:Els.
    + apply Z.eqb_eq in Erf, Els.
      assert (E1 : Z.leb (Q1AuxSpec.out_ls o1) (Q1AuxSpec.out_ls o2) = true)
        by (apply Z.leb_le; lia).
      assert (E2 : Z.leb (Q1AuxSpec.out_ls o2) (Q1AuxSpec.out_ls o1) = true)
        by (apply Z.leb_le; lia).
      rewrite E1.
      destruct (Z.eqb (Q1AuxSpec.out_rf o2) (Q1AuxSpec.out_rf o1)) eqn:Eba.
      * rewrite E2; reflexivity.
      * apply Z.eqb_neq in Eba; lia.
    + destruct (Z.ltb (Q1AuxSpec.out_ls o1) (Q1AuxSpec.out_ls o2)) eqn:Elt.
      * apply Z.ltb_lt in Elt.
        assert (E1 : Z.leb (Q1AuxSpec.out_ls o1) (Q1AuxSpec.out_ls o2) = true)
          by (apply Z.leb_le; lia).
        assert (E2 : Z.leb (Q1AuxSpec.out_ls o2) (Q1AuxSpec.out_ls o1) = false)
          by (apply Z.leb_nle; lia).
        (* OutRowLe reverse uses eqb rf (true) then leb ls *)
        rewrite E1.
        destruct (Z.eqb (Q1AuxSpec.out_rf o2) (Q1AuxSpec.out_rf o1)) eqn:Eba.
        -- rewrite E2; reflexivity.
        -- apply Z.eqb_eq in Erf; apply Z.eqb_neq in Eba; lia.
      * apply Z.ltb_ge in Elt; apply Z.eqb_neq in Els.
        assert (E1 : Z.leb (Q1AuxSpec.out_ls o1) (Q1AuxSpec.out_ls o2) = false)
          by (apply Z.leb_nle; lia).
        rewrite E1; reflexivity.
  - simpl.
    destruct (Z.ltb (Q1AuxSpec.out_rf o1) (Q1AuxSpec.out_rf o2)) eqn:Elt.
    + apply Z.ltb_lt in Elt; apply Z.eqb_neq in Erf.
      assert (E1 : Z.leb (Q1AuxSpec.out_rf o1) (Q1AuxSpec.out_rf o2) = true)
        by (apply Z.leb_le; lia).
      assert (E2 : Z.leb (Q1AuxSpec.out_rf o2) (Q1AuxSpec.out_rf o1) = false)
        by (apply Z.leb_nle; lia).
      rewrite E1.
      destruct (Z.eqb (Q1AuxSpec.out_rf o2) (Q1AuxSpec.out_rf o1)) eqn:Eba.
      { apply Z.eqb_eq in Eba; lia. }
      rewrite E2; reflexivity.
    + apply Z.ltb_ge in Elt; apply Z.eqb_neq in Erf.
      assert (E1 : Z.leb (Q1AuxSpec.out_rf o1) (Q1AuxSpec.out_rf o2) = false)
        by (apply Z.leb_nle; lia).
      rewrite E1; reflexivity.
Qed.

(** SpecView↔[query_make_groups] sorted bag equality.
    Discharged by [Q1_GroupBagsPerm_holds] via SortedGroupResultFacade
    (Permutation + list-local le_trans + key antisym). *)
Definition Q1_GroupBagsPerm (db : list Q1AuxSpec.Row) (delta : Z) : Prop :=
  @sort_ordered_rows TNull value_is_null q1_order_keys
    (q1_group_rows_of db delta) =
  @sort_ordered_rows TNull value_is_null q1_order_keys
    (map q1_group_tuple_of_OutRow
      (Q1AuxSpec.SpecFinalize
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta)))).

Lemma insert_ordered_row_map_OutRowInsert :
  forall x l,
    0 <= Q1AuxSpec.out_rf x < 256 ->
    0 <= Q1AuxSpec.out_ls x < 256 ->
    Forall (fun o =>
      0 <= Q1AuxSpec.out_rf o < 256 /\ 0 <= Q1AuxSpec.out_ls o < 256) l ->
    @insert_ordered_row TNull value_is_null q1_order_keys
      (q1_group_tuple_of_OutRow x)
      (map q1_group_tuple_of_OutRow l) =
    map q1_group_tuple_of_OutRow (Q1AuxSpec.OutRowInsert x l).
Proof.
  intros x l Hxrf Hxls Hfa.
  induction l as [|y ys IH]; [reflexivity|].
  inversion Hfa as [|? ? [Hyrf Hyls] Hfa']; subst.
  cbn [map Q1AuxSpec.OutRowInsert insert_ordered_row].
  rewrite (compare_order_keys_group_tuple_OutRowLe x y Hxrf Hxls Hyrf Hyls).
  destruct (Q1AuxSpec.OutRowLe x y) eqn:Exy.
  - destruct (Q1AuxSpec.OutRowLe y x); reflexivity.
  - cbn; f_equal; apply IH; exact Hfa'.
Qed.

(* Forall_permutation: FormalSQLLib.ListNoDupFacade *)

Lemma sort_ordered_rows_map_OutRowSort :
  forall l,
    Forall (fun o =>
      0 <= Q1AuxSpec.out_rf o < 256 /\ 0 <= Q1AuxSpec.out_ls o < 256) l ->
    @sort_ordered_rows TNull value_is_null q1_order_keys
      (map q1_group_tuple_of_OutRow l) =
    map q1_group_tuple_of_OutRow (Q1AuxSpec.OutRowSort l).
Proof.
  induction l as [|x xs IH]; intros Hfa; [reflexivity|].
  inversion Hfa as [|? ? [Hxrf Hxls] Hfa']; subst.
  cbn [map Q1AuxSpec.OutRowSort sort_ordered_rows].
  rewrite (IH Hfa').
  apply insert_ordered_row_map_OutRowInsert; try assumption.
  eapply Forall_permutation; [apply OutRowSort_perm|exact Hfa'].
Qed.

Lemma keystore_get_of_In :
  forall acc gkey s,
    NoDup (map fst acc) ->
    In (gkey, s) acc ->
    keystore_get acc gkey = s.
Proof.
  induction acc as [|[k s0] rest IH]; intros gkey s Hnd Hin; [contradiction|].
  inversion Hnd as [|? ? Hnin Hnd']; subst.
  cbn in Hin; destruct Hin as [Heq|Hin].
  - inversion Heq; subst; cbn; rewrite Z.eqb_refl; reflexivity.
  - cbn; destruct (Z.eqb k gkey) eqn:Ek.
    + apply Z.eqb_eq in Ek; subst k.
      exfalso; apply Hnin, in_map_iff.
      exists (gkey, s); split; [reflexivity|exact Hin].
    + apply IH; assumption.
Qed.

Lemma in_map_fst_keystore_put :
  forall acc gkey s k,
    In k (map fst (Q1AuxSpec.keystore_put acc gkey s)) ->
    k = gkey \/ In k (map fst acc).
Proof.
  induction acc as [|[k0 s0] rest IH]; intros gkey s k Hin.
  - cbn in Hin; destruct Hin as [Heq|[]]; left; symmetry; exact Heq.
  - cbn [Q1AuxSpec.keystore_put] in Hin.
    destruct (Z.eqb k0 gkey) eqn:Ek.
    + cbn in Hin; destruct Hin as [Heq|Hin'].
      * right; left; exact Heq.
      * right; right; exact Hin'.
    + cbn in Hin; destruct Hin as [Heq|Hin'].
      * right; left; exact Heq.
      * apply IH in Hin'; destruct Hin' as [Heq|Hin''].
        -- left; exact Heq.
        -- right; right; exact Hin''.
Qed.

Lemma keystore_put_NoDup_fst :
  forall acc gkey s,
    NoDup (map fst acc) ->
    NoDup (map fst (Q1AuxSpec.keystore_put acc gkey s)).
Proof.
  induction acc as [|[k s0] rest IH]; intros gkey s Hnd.
  - cbn; constructor; [intros []|constructor].
  - inversion Hnd as [|? ? Hnin Hnd']; subst.
    cbn [Q1AuxSpec.keystore_put].
    destruct (Z.eqb k gkey) eqn:Ek.
    + cbn; constructor; [exact Hnin|exact Hnd'].
    + cbn [map fst]; constructor.
      * intros Hin.
        apply in_map_fst_keystore_put in Hin.
        destruct Hin as [Heq|Hin'].
        -- subst; apply Z.eqb_neq in Ek; contradiction.
        -- contradiction.
      * apply IH; exact Hnd'.
Qed.

Lemma SpecView_NoDup_fst :
  forall st cutoff,
    NoDup (map fst (Q1AuxSpec.SpecView st cutoff)).
Proof.
  induction st as [|[[day k] s0] rest IH]; intros cutoff; [constructor|].
  cbn [Q1AuxSpec.SpecView].
  destruct (Z.leb day cutoff).
  - apply keystore_put_NoDup_fst, IH.
  - apply IH.
Qed.

Lemma SpecView_FoldSpec_OutRow_byte :
  forall db delta,
    Preconditions_Q1 db delta ->
    Forall (fun o =>
      0 <= Q1AuxSpec.out_rf o < 256 /\ 0 <= Q1AuxSpec.out_ls o < 256)
      (Q1AuxSpec.SpecFinalize
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))).
Proof.
  intros db delta Hpre.
  rewrite SpecFinalize_SpecView_FoldSpec.
  apply Forall_forall; intros o Hin.
  apply in_map_iff in Hin; destruct Hin as [[gkey s] [Heq Hin]].
  subst o.
  assert (Hg : 0 <= gkey < 256 * 256).
  {
    assert (Hs : keystore_get
      (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
      gkey = s).
    { apply keystore_get_of_In; [apply SpecView_NoDup_fst|exact Hin]. }
    pose proof (SpecView_FoldSpec_In_count_pos db (EmbedParams_Q1 delta)
                  gkey s Hin) as Hpos.
    rewrite <- Hs, q1_aux_bucket_count in Hpos.
    assert (Hne' : q1_aux_bucket db delta gkey <> nil).
    { intro Hnil; rewrite Hnil in Hpos; cbn in Hpos; lia. }
    destruct (q1_aux_bucket db delta gkey) as [|r rs] eqn:Eb;
      [contradiction Hne'; reflexivity|].
    assert (Hrow : RowOk_Q1 r).
    { pose proof (Forall_map_RowOk_aux_bucket db delta gkey Hpre) as Hok.
      rewrite Eb in Hok; inversion Hok; assumption. }
    assert (Hk : Q1AuxSpec.RowKey r = gkey).
    { pose proof (Forall_aux_bucket_RowKey db delta gkey) as Hkeys.
      rewrite Eb in Hkeys; inversion Hkeys; assumption. }
    rewrite <- Hk; apply RowKey_byte_bound; exact Hrow.
  }
  exact (OutRow_rf_ls_byte gkey s Hg).
Qed.

(** * Discharge [Q1_GroupBagsPerm] from bag↔key + Faithful *)

Lemma keystore_get_pos_In :
  forall acc gkey,
    0 < Q1AuxSpec.count (keystore_get acc gkey) ->
    exists s, In (gkey, s) acc /\ keystore_get acc gkey = s.
Proof.
  induction acc as [|[k s0] rest IH]; intros gkey Hpos.
  - cbn in Hpos; lia.
  - cbn in Hpos |- *.
    destruct (Z.eqb k gkey) eqn:Ek.
    + apply Z.eqb_eq in Ek; subst k.
      exists s0; split; [left; reflexivity|reflexivity].
    + destruct (IH gkey Hpos) as [s [Hin Hs]].
      exists s; split; [right; exact Hin|exact Hs].
Qed.

Lemma q1_group_in_of_nonempty_bucket :
  forall db delta gkey,
    Preconditions_Q1 db delta ->
    0 <= gkey < 256 * 256 ->
    q1_aux_bucket db delta gkey <> nil ->
    In (rev (q1_inner_of_aux (q1_aux_bucket db delta gkey)))
      (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms).
Proof.
  intros db delta gkey Hpre Hg Hne.
  pose proof (q1_groups_of_aux_bucket db delta gkey Hpre Hg) as Hfilt.
  destruct (q1_aux_bucket db delta gkey) as [|r rs] eqn:Eb;
    [congruence|].
  assert (Hin_f :
    In (rev (q1_inner_of_aux (r :: rs)))
      (filter
        (fun members =>
          match members with
          | nil => false
          | row :: _ =>
              Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
                (query_grouping_key nil q1_group_terms row)
                (q1_key_values gkey)
          end)
        (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms))).
  { rewrite Hfilt; left; reflexivity. }
  apply filter_In in Hin_f; exact (proj1 Hin_f).
Qed.

Lemma In_group_rows_of_SpecView_entry :
  forall db delta gkey s,
    Preconditions_Q1 db delta ->
    NumericEmbedFaithful ->
    In (gkey, s)
      (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta)) ->
    In (q1_group_tuple_of_OutRow (q1_outrow_of_stats gkey s))
      (q1_group_rows_of db delta).
Proof.
  intros db delta gkey s Hpre Hnum Hin.
  pose proof (SpecView_FoldSpec_In_count_pos db (EmbedParams_Q1 delta)
                gkey s Hin) as Hpos.
  assert (Hs : keystore_get
    (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
    gkey = s).
  { apply keystore_get_of_In; [apply SpecView_NoDup_fst|exact Hin]. }
  assert (Hne : q1_aux_bucket db delta gkey <> nil).
  { apply q1_aux_bucket_nonempty_iff_count_pos; rewrite Hs; exact Hpos. }
  assert (Hg : 0 <= gkey < 256 * 256).
  {
    destruct (q1_aux_bucket db delta gkey) as [|r rs] eqn:Eb;
      [contradiction Hne; reflexivity|].
    assert (Hrow : RowOk_Q1 r).
    { pose proof (Forall_map_RowOk_aux_bucket db delta gkey Hpre) as Hok.
      rewrite Eb in Hok; inversion Hok; assumption. }
    assert (Hk : Q1AuxSpec.RowKey r = gkey).
    { pose proof (Forall_aux_bucket_RowKey db delta gkey) as Hkeys.
      rewrite Eb in Hkeys; inversion Hkeys; assumption. }
    rewrite <- Hk; apply RowKey_byte_bound; exact Hrow.
  }
  pose proof (q1_group_in_of_nonempty_bucket db delta gkey Hpre Hg Hne)
    as Hgin.
  unfold q1_group_rows_of.
  apply in_map_iff.
  exists (rev (q1_inner_of_aux (q1_aux_bucket db delta gkey))).
  split; [|exact Hgin].
  rewrite <- Hs.
  apply q1_group_tuple_of_rev_bucket; assumption.
Qed.

Lemma In_group_rows_of_SpecFinalize :
  forall db delta t,
    Preconditions_Q1 db delta ->
    NumericEmbedFaithful ->
    In t (map q1_group_tuple_of_OutRow
      (Q1AuxSpec.SpecFinalize
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta)))) ->
    In t (q1_group_rows_of db delta).
Proof.
  intros db delta t Hpre Hnum Hin.
  rewrite SpecFinalize_SpecView_FoldSpec in Hin.
  apply in_map_iff in Hin; destruct Hin as [o [Heq Hin]].
  subst t.
  apply in_map_iff in Hin; destruct Hin as [[gkey s] [Heq Hin]].
  subst o.
  apply In_group_rows_of_SpecView_entry; assumption.
Qed.

Lemma q1_group_eq_rev_aux_bucket :
  forall db delta group r,
    Preconditions_Q1 db delta ->
    In group (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) ->
    In r (q1_filtered_aux db delta) ->
    In (project_row q1_inner_select (q1_inner_values r)) group ->
    group =
      rev (q1_inner_of_aux (q1_aux_bucket db delta (Q1AuxSpec.RowKey r))).
Proof.
  intros db delta group r Hpre Hgin Hinr Hrow.
  set (gkey := Q1AuxSpec.RowKey r).
  assert (Hrok : RowOk_Q1 r)
    by (eapply q1_filtered_aux_RowOk; eassumption).
  assert (Hg : 0 <= gkey < 256 * 256)
    by (unfold gkey; apply RowKey_byte_bound; exact Hrok).
  assert (Hne_bucket : q1_aux_bucket db delta gkey <> nil).
  {
    intro Hnil.
    assert (Hinb : In r (q1_aux_bucket db delta gkey)).
    { unfold q1_aux_bucket, gkey; apply filter_In; split;
        [exact Hinr|apply Z.eqb_refl]. }
    rewrite Hnil in Hinb; contradiction.
  }
  pose proof (q1_groups_of_aux_bucket db delta gkey Hpre Hg) as Hfilt.
  assert (Hne_group : group <> nil)
    by (eapply query_make_groups_member_nonempty;
          [exact q1_group_terms_nonempty|exact Hgin]).
  destruct group as [|head rest] eqn:Eg;
    [contradiction Hne_group; reflexivity|].
  assert (Hkey :
    Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
      (query_grouping_key nil q1_group_terms head)
      (q1_key_values gkey) = true).
  {
    pose proof (query_make_groups_member_homogeneous nil
      (q1_inner_proj_of db delta) q1_group_terms (head :: rest)
      head (project_row q1_inner_select (q1_inner_values r))
      Hgin ltac:(left; reflexivity) Hrow) as Hhom.
    change (query_grouping_key nil q1_group_terms head =
            query_grouping_key nil q1_group_terms
              (project_row q1_inner_select (q1_inner_values r))) in Hhom.
    rewrite (q1_inner_grouping_key_RowKey r Hrok) in Hhom.
    apply Oset.eq_bool_true_iff.
    unfold gkey; rewrite <- Hhom; reflexivity.
  }
  assert (Hin_f :
    In (head :: rest)
      (filter
        (fun members =>
          match members with
          | nil => false
          | row :: _ =>
              Oset.eq_bool (OrderedSet.mk_olists (OVal TNull))
                (query_grouping_key nil q1_group_terms row)
                (q1_key_values gkey)
          end)
        (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms))).
  {
    apply filter_In; split.
    - exact Hgin.
    - exact Hkey.
  }
  rewrite Hfilt in Hin_f.
  destruct (q1_aux_bucket db delta gkey) as [|r0 rs] eqn:Eb;
    [contradiction Hne_bucket; reflexivity|].
  (* Reduce only the [match]; keep [q1_inner_of_aux] folded. *)
  simpl (match _ with nil => _ | _ :: _ => _ end) in Hin_f.
  destruct Hin_f as [Heq|[]].
  symmetry; exact Heq.
Qed.

Lemma In_SpecFinalize_of_group_rows :
  forall db delta t,
    Preconditions_Q1 db delta ->
    NumericEmbedFaithful ->
    In t (q1_group_rows_of db delta) ->
    In t (map q1_group_tuple_of_OutRow
      (Q1AuxSpec.SpecFinalize
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta)))).
Proof.
  intros db delta t Hpre Hnum Hin.
  unfold q1_group_rows_of in Hin.
  apply in_map_iff in Hin; destruct Hin as [group [Heqt Hgin]].
  assert (Hne_group : group <> nil)
    by (eapply query_make_groups_member_nonempty;
          [exact q1_group_terms_nonempty|exact Hgin]).
  destruct group as [|row rest] eqn:Eg;
    [contradiction Hne_group; reflexivity|].
  assert (Hin_proj : In row (q1_inner_proj_of db delta))
    by (eapply query_make_groups_member_in;
          [exact Hgin|left; reflexivity]).
  destruct (q1_inner_proj_in_inv db delta row Hin_proj)
    as [r [Hinr Heqrow]].
  set (gkey := Q1AuxSpec.RowKey r).
  assert (Hrok : RowOk_Q1 r)
    by (eapply q1_filtered_aux_RowOk; eassumption).
  assert (Hg : 0 <= gkey < 256 * 256)
    by (unfold gkey; apply RowKey_byte_bound; exact Hrok).
  assert (Hgrp :
    row :: rest =
      rev (q1_inner_of_aux (q1_aux_bucket db delta gkey))).
  {
    apply (q1_group_eq_rev_aux_bucket db delta (row :: rest) r Hpre Hgin Hinr).
    rewrite <- Heqrow; left; reflexivity.
  }
  assert (Hne : q1_aux_bucket db delta gkey <> nil).
  { intro Hnil; rewrite Hnil in Hgrp; cbn in Hgrp; discriminate. }
  set (s := keystore_get
    (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta)) gkey).
  assert (Hpos : 0 < Q1AuxSpec.count s).
  { unfold s; apply q1_aux_bucket_nonempty_iff_count_pos; exact Hne. }
  assert (Hin_sv : In (gkey, s)
    (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))).
  {
    unfold s.
    destruct (keystore_get_pos_In
      (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
      gkey Hpos) as [s' [Hin' Hs']].
    rewrite Hs'; exact Hin'.
  }
  rewrite SpecFinalize_SpecView_FoldSpec.
  apply in_map_iff.
  exists (q1_outrow_of_stats gkey s); split.
  - subst t. rewrite Hgrp.
    unfold s; symmetry; apply q1_group_tuple_of_rev_bucket; assumption.
  - apply in_map_iff; exists (gkey, s); split; [reflexivity|exact Hin_sv].
Qed.

Lemma q1_group_rows_In_iff_SpecFinalize :
  forall db delta t,
    Preconditions_Q1 db delta ->
    NumericEmbedFaithful ->
    In t (q1_group_rows_of db delta) <->
    In t (map q1_group_tuple_of_OutRow
      (Q1AuxSpec.SpecFinalize
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta)))).
Proof.
  intros db delta t Hpre Hnum; split.
  - apply In_SpecFinalize_of_group_rows; assumption.
  - apply In_group_rows_of_SpecFinalize; assumption.
Qed.

(** * GROUP_BAGS: same map + unique keys + sort uniqueness *)

Definition q1_tuple_le (a b : tuple TNull) : bool :=
  match @compare_order_keys TNull value_is_null q1_order_keys a b with
  | Gt => false
  | Eq | Lt => true
  end.

Definition Q1TupleLe (a b : tuple TNull) : Prop := q1_tuple_le a b = true.

Lemma ordered_pair_Q1TupleLe :
  forall a b,
    @ordered_pair TNull value_is_null q1_order_keys a b <-> Q1TupleLe a b.
Proof.
  intros a b; unfold ordered_pair, Q1TupleLe, q1_tuple_le.
  destruct (@compare_order_keys TNull value_is_null q1_order_keys a b);
    split; intros H; try exact I; try reflexivity; try contradiction; discriminate.
Qed.

Lemma ordered_rows_Sorted_Q1 :
  forall rows,
    @ordered_rows TNull value_is_null q1_order_keys rows ->
    Sorted Q1TupleLe rows.
Proof.
  induction rows as [|a rows IH]; intros Hord; [constructor|].
  destruct rows as [|b bs]; [repeat constructor|].
  destruct Hord as [Hab Htail].
  constructor.
  - apply IH; exact Htail.
  - constructor; apply ordered_pair_Q1TupleLe; exact Hab.
Qed.

Lemma sort_ordered_rows_Sorted_Q1 :
  forall rows,
    Sorted Q1TupleLe
      (@sort_ordered_rows TNull value_is_null q1_order_keys rows).
Proof.
  intros rows.
  apply ordered_rows_Sorted_Q1, sort_ordered_rows_is_ordered.
Qed.

Lemma SpecView_entries_NoDup_fst :
  forall db delta,
    NoDup
      (map fst
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))).
Proof.
  intros db delta; apply SpecView_NoDup_fst.
Qed.

Lemma q1_outrow_of_stats_key :
  forall gkey s,
    Q1AuxSpec.Pack (Q1AuxSpec.out_rf (q1_outrow_of_stats gkey s))
      (Q1AuxSpec.out_ls (q1_outrow_of_stats gkey s)) =
    Q1AuxSpec.Pack (fst (Q1AuxSpec.Unpack gkey)) (snd (Q1AuxSpec.Unpack gkey)).
Proof.
  intros gkey s.
  unfold q1_outrow_of_stats.
  destruct (Q1AuxSpec.Unpack gkey) as [rf ls]; reflexivity.
Qed.

Lemma q1_outrow_Pack_gkey :
  forall gkey s,
    0 <= gkey < 256 * 256 ->
    Q1AuxSpec.Pack (Q1AuxSpec.out_rf (q1_outrow_of_stats gkey s))
      (Q1AuxSpec.out_ls (q1_outrow_of_stats gkey s)) = gkey.
Proof.
  intros gkey s Hg.
  rewrite q1_outrow_of_stats_key, Pack_Unpack_byte by exact Hg.
  reflexivity.
Qed.

Lemma SpecView_FoldSpec_In_key_byte :
  forall db delta gkey s,
    Preconditions_Q1 db delta ->
    In (gkey, s)
      (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta)) ->
    0 <= gkey < 256 * 256.
Proof.
  intros db delta gkey s Hpre Hin.
  assert (Hs : keystore_get
    (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
    gkey = s).
  { apply keystore_get_of_In; [apply SpecView_NoDup_fst|exact Hin]. }
  pose proof (SpecView_FoldSpec_In_count_pos db (EmbedParams_Q1 delta)
                gkey s Hin) as Hpos.
  rewrite <- Hs, q1_aux_bucket_count in Hpos.
  assert (Hne' : q1_aux_bucket db delta gkey <> nil).
  { intro Hnil; rewrite Hnil in Hpos; cbn in Hpos; lia. }
  destruct (q1_aux_bucket db delta gkey) as [|r rs] eqn:Eb;
    [contradiction Hne'; reflexivity|].
  assert (Hrow : RowOk_Q1 r).
  { pose proof (Forall_map_RowOk_aux_bucket db delta gkey Hpre) as Hok.
    rewrite Eb in Hok; inversion Hok; assumption. }
  assert (Hk : Q1AuxSpec.RowKey r = gkey).
  { pose proof (Forall_aux_bucket_RowKey db delta gkey) as Hkeys.
    rewrite Eb in Hkeys; inversion Hkeys; assumption. }
  rewrite <- Hk; apply RowKey_byte_bound; exact Hrow.
Qed.

Lemma SpecFinalize_keys_eq_fst :
  forall db delta,
    Preconditions_Q1 db delta ->
    map (fun o =>
        Q1AuxSpec.Pack (Q1AuxSpec.out_rf o) (Q1AuxSpec.out_ls o))
      (Q1AuxSpec.SpecFinalize
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))) =
    map fst
      (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta)).
Proof.
  intros db delta Hpre.
  rewrite SpecFinalize_SpecView_FoldSpec.
  set (entries :=
    Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta)).
  assert (Hfa : Forall
    (fun '(gkey, s) => 0 <= gkey < 256 * 256) entries).
  {
    apply Forall_forall; intros [gkey s] Hin.
    eapply SpecView_FoldSpec_In_key_byte; eassumption.
  }
  induction entries as [|[gkey s] rest IH]; [reflexivity|].
  inversion Hfa as [|? ? Hg Hfa']; subst.
  cbn [map]; f_equal.
  - apply q1_outrow_Pack_gkey; exact Hg.
  - apply IH; exact Hfa'.
Qed.

Lemma SpecFinalize_NoDup :
  forall db delta,
    Preconditions_Q1 db delta ->
    NoDup
      (Q1AuxSpec.SpecFinalize
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))).
Proof.
  intros db delta Hpre.
  pose proof (SpecFinalize_keys_eq_fst db delta Hpre) as Hkeys.
  pose proof (SpecView_NoDup_fst (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
    as Hnd.
  rewrite <- Hkeys in Hnd.
  eapply NoDup_map_inv; exact Hnd.
Qed.

(** * GROUP_BAGS close: NoDup → Permutation → sorted uniqueness *)

Definition q1_tuple_order_vals (t : tuple TNull) : list NullValues.value :=
  [dot TNull t (AttrString "l_returnflag" (StringChar 1));
   dot TNull t (AttrString "l_linestatus" (StringChar 1))].

Lemma q1_group_tuple_order_vals_OutRow :
  forall o,
    q1_tuple_order_vals (q1_group_tuple_of_OutRow o) =
    [embed_flag_char (Q1AuxSpec.out_rf o); embed_flag_char (Q1AuxSpec.out_ls o)].
Proof.
  intro o; unfold q1_tuple_order_vals.
  rewrite q1_group_tuple_dot_rf, q1_group_tuple_dot_ls; reflexivity.
Qed.

(* NoDup_map_inj: FormalSQLLib.ListNoDupFacade *)

Lemma SpecFinalize_Pack_inj :
  forall db delta o1 o2,
    Preconditions_Q1 db delta ->
    In o1
      (Q1AuxSpec.SpecFinalize
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))) ->
    In o2
      (Q1AuxSpec.SpecFinalize
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))) ->
    Q1AuxSpec.Pack (Q1AuxSpec.out_rf o1) (Q1AuxSpec.out_ls o1) =
    Q1AuxSpec.Pack (Q1AuxSpec.out_rf o2) (Q1AuxSpec.out_ls o2) ->
    o1 = o2.
Proof.
  intros db delta o1 o2 Hpre Hin1 Hin2 Hpack.
  rewrite SpecFinalize_SpecView_FoldSpec in Hin1, Hin2.
  apply in_map_iff in Hin1; destruct Hin1 as [[g1 s1] [He1 Hin1]].
  apply in_map_iff in Hin2; destruct Hin2 as [[g2 s2] [He2 Hin2]].
  subst o1 o2.
  assert (Hg1 : 0 <= g1 < 256 * 256)
    by (eapply SpecView_FoldSpec_In_key_byte; eassumption).
  assert (Hg2 : 0 <= g2 < 256 * 256)
    by (eapply SpecView_FoldSpec_In_key_byte; eassumption).
  rewrite !q1_outrow_Pack_gkey in Hpack by assumption.
  subst g2.
  pose proof (SpecView_NoDup_fst (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
    as Hnd.
  pose proof (keystore_get_of_In _ _ _ Hnd Hin1) as Hs1.
  pose proof (keystore_get_of_In _ _ _ Hnd Hin2) as Hs2.
  rewrite Hs1 in Hs2; subst s2; reflexivity.
Qed.

Lemma SpecFinalize_embed_NoDup :
  forall db delta,
    Preconditions_Q1 db delta ->
    NoDup
      (map q1_group_tuple_of_OutRow
        (Q1AuxSpec.SpecFinalize
          (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta)))).
Proof.
  intros db delta Hpre.
  apply NoDup_map_inj; [|apply SpecFinalize_NoDup; exact Hpre].
  intros o1 o2 Hin1 Hin2 Heq.
  pose proof (SpecView_FoldSpec_OutRow_byte db delta Hpre) as Hfa.
  assert (Hb1 : 0 <= Q1AuxSpec.out_rf o1 < 256 /\ 0 <= Q1AuxSpec.out_ls o1 < 256)
    by (apply (proj1 (Forall_forall _ _) Hfa); exact Hin1).
  assert (Hb2 : 0 <= Q1AuxSpec.out_rf o2 < 256 /\ 0 <= Q1AuxSpec.out_ls o2 < 256)
    by (apply (proj1 (Forall_forall _ _) Hfa); exact Hin2).
  destruct Hb1 as [Hr1 Hl1]; destruct Hb2 as [Hr2 Hl2].
  assert (Erf : embed_flag_char (Q1AuxSpec.out_rf o1) =
                embed_flag_char (Q1AuxSpec.out_rf o2)).
  { rewrite <- (q1_group_tuple_dot_rf o1), <- (q1_group_tuple_dot_rf o2), Heq.
    reflexivity. }
  assert (Els : embed_flag_char (Q1AuxSpec.out_ls o1) =
                embed_flag_char (Q1AuxSpec.out_ls o2)).
  { rewrite <- (q1_group_tuple_dot_ls o1), <- (q1_group_tuple_dot_ls o2), Heq.
    reflexivity. }
  apply embed_flag_char_inj in Erf; [|lia|lia].
  apply embed_flag_char_inj in Els; [|lia|lia].
  apply (SpecFinalize_Pack_inj db delta o1 o2 Hpre Hin1 Hin2).
  f_equal; [exact Erf|exact Els].
Qed.

(* NoDup_map_of_filter_unique, filter_ext_In: FormalSQLLib.ListNoDupFacade *)

Definition q1_group_head_key (g : list (tuple TNull)) : list NullValues.value :=
  match g with
  | nil => nil
  | row :: _ => query_grouping_key nil q1_group_terms row
  end.

Lemma q1_groups_head_key_filter_length_le_1 :
  forall db delta key,
    (length
      (filter
        (fun g =>
          Oset.eq_bool (OrderedSet.mk_olists NullValues.OVal)
            (q1_group_head_key g) key)
        (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms))
      <= 1)%nat.
Proof.
  intros db delta key.
  assert (Hf :
    filter
      (fun g =>
        Oset.eq_bool (OrderedSet.mk_olists NullValues.OVal)
          (q1_group_head_key g) key)
      (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) =
    filter
      (fun members =>
        match members with
        | nil => false
        | row :: _ =>
            Oset.eq_bool (OrderedSet.mk_olists NullValues.OVal)
              (query_grouping_key nil q1_group_terms row) key
        end)
      (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms)).
  {
    apply filter_ext_In; intros g Hin.
    assert (Hne : g <> nil)
      by (eapply query_make_groups_member_nonempty;
            [exact q1_group_terms_nonempty|exact Hin]).
    unfold q1_group_head_key.
    destruct g; [contradiction Hne; reflexivity|reflexivity].
  }
  rewrite Hf.
  rewrite (Q1_group_is_key_bucket nil (q1_inner_proj_of db delta) key).
  destruct
    (filter
      (fun row =>
        Oset.eq_bool (OrderedSet.mk_olists NullValues.OVal)
          (query_grouping_key nil q1_group_terms row) key)
      (q1_inner_proj_of db delta));
    cbn; lia.
Qed.

Lemma q1_groups_head_key_NoDup :
  forall db delta,
    NoDup
      (map q1_group_head_key
        (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms)).
Proof.
  intros db delta.
  eapply NoDup_map_of_filter_unique
    with (eqb := Oset.eq_bool (OrderedSet.mk_olists NullValues.OVal)).
  - intros x y H;
      apply (proj1 (Oset.eq_bool_true_iff
                      (OrderedSet.mk_olists NullValues.OVal) x y));
      exact H.
  - intros x;
      apply (proj2 (Oset.eq_bool_true_iff
                      (OrderedSet.mk_olists NullValues.OVal) x x));
      reflexivity.
  - apply q1_groups_head_key_filter_length_le_1.
Qed.

Lemma length_q1_group_rows_eq_SpecFinalize :
  forall db delta,
    Preconditions_Q1 db delta ->
    length (q1_group_rows_of db delta) =
    length
      (Q1AuxSpec.SpecFinalize
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))).
Proof.
  intros db delta Hpre.
  unfold q1_group_rows_of.
  rewrite length_map.
  (* length groups = length (map head_key groups) and that equals length SpecView *)
  set (groups :=
    query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms).
  change (length groups =
    length
      (Q1AuxSpec.SpecFinalize
        (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta)))).
  rewrite SpecFinalize_SpecView_FoldSpec, length_map.
  set (entries :=
    Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta)).
  (* length groups = length (map head_key groups) *)
  assert (Hg : length groups = length (map q1_group_head_key groups))
    by (rewrite length_map; reflexivity).
  rewrite Hg.
  (* Both key lists NoDup; show mutual inclusion of In ⇒ same length *)
  pose proof (q1_groups_head_key_NoDup db delta) as HndG.
  fold groups in HndG.
  pose proof (SpecView_NoDup_fst (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))
    as HndS.
  fold entries in HndS.
  (* Convert SpecView Z-keys to value-list keys via q1_key_values *)
  assert (HndV :
    NoDup (map q1_key_values (map fst entries))).
  {
    apply NoDup_map_inj; [|exact HndS].
    intros g1 g2 Hin1 Hin2 Heq.
    assert (Hb1 : 0 <= g1 < 256 * 256).
    { apply in_map_iff in Hin1; destruct Hin1 as [[g s] [He Hin]].
      cbn in He; subst g.
      eapply SpecView_FoldSpec_In_key_byte; eassumption. }
    assert (Hb2 : 0 <= g2 < 256 * 256).
    { apply in_map_iff in Hin2; destruct Hin2 as [[g s] [He Hin]].
      cbn in He; subst g.
      eapply SpecView_FoldSpec_In_key_byte; eassumption. }
    apply (q1_key_values_inj g1 g2 Hb1 Hb2 Heq).
  }
  (* Mutual incl between map head_key groups and map q1_key_values (map fst entries) *)
  assert (Hincl1 :
    incl (map q1_group_head_key groups)
         (map q1_key_values (map fst entries))).
  {
    intros k Hin.
    apply in_map_iff in Hin; destruct Hin as [g [Hk Hgin]].
    subst k.
    assert (Hne : g <> nil)
      by (eapply query_make_groups_member_nonempty;
            [exact q1_group_terms_nonempty|exact Hgin]).
    destruct g as [|row rest]; [contradiction Hne; reflexivity|].
    cbn [q1_group_head_key].
    (* row in inner_proj; recover Aux row and RowKey *)
    assert (Hin_proj : In row (q1_inner_proj_of db delta))
      by (eapply query_make_groups_member_in; [exact Hgin|left; reflexivity]).
    destruct (q1_inner_proj_in_inv db delta row Hin_proj)
      as [r [Hinr Heqrow]].
    set (gkey := Q1AuxSpec.RowKey r).
    assert (Hrok : RowOk_Q1 r)
      by (eapply q1_filtered_aux_RowOk; eassumption).
    assert (Hgk : 0 <= gkey < 256 * 256)
      by (unfold gkey; apply RowKey_byte_bound; exact Hrok).
    assert (Hkey :
      query_grouping_key nil q1_group_terms row = q1_key_values gkey).
    {
      rewrite Heqrow, q1_inner_grouping_key_RowKey by exact Hrok.
      reflexivity.
    }
    rewrite Hkey.
    (* gkey in SpecView: nonempty bucket *)
    assert (Hne_b : q1_aux_bucket db delta gkey <> nil).
    {
      intro Hnil.
      assert (Hinb : In r (q1_aux_bucket db delta gkey)).
      { unfold q1_aux_bucket, gkey; apply filter_In; split;
          [exact Hinr|apply Z.eqb_refl]. }
      rewrite Hnil in Hinb; contradiction.
    }
    pose proof (q1_aux_bucket_nonempty_iff_count_pos db delta gkey) as Hiffc.
    assert (Hpos : 0 < Q1AuxSpec.count
      (keystore_get entries gkey)).
    { unfold entries; apply (proj1 Hiffc); exact Hne_b. }
    destruct (keystore_get_pos_In entries gkey Hpos) as [s [Hin_e Hs]].
    apply in_map_iff; exists gkey; split; [reflexivity|].
    apply in_map_iff; exists (gkey, s); split; [reflexivity|exact Hin_e].
  }
  assert (Hincl2 :
    incl (map q1_key_values (map fst entries))
         (map q1_group_head_key groups)).
  {
    intros k Hin.
    apply in_map_iff in Hin; destruct Hin as [gkey [Hk Hin1]].
    subst k.
    apply in_map_iff in Hin1; destruct Hin1 as [[gkey' s] [He Hin_e]].
    cbn in He; subst gkey'.
    assert (Hgk : 0 <= gkey < 256 * 256)
      by (eapply SpecView_FoldSpec_In_key_byte; eassumption).
    pose proof (SpecView_FoldSpec_In_count_pos db (EmbedParams_Q1 delta)
                  gkey s Hin_e) as Hpos.
    assert (Hs : keystore_get entries gkey = s)
      by (apply keystore_get_of_In; [exact HndS|exact Hin_e]).
    rewrite <- Hs in Hpos; unfold entries in Hpos;
      rewrite q1_aux_bucket_count in Hpos.
    assert (Hne_b : q1_aux_bucket db delta gkey <> nil).
    { intro Hnil; rewrite Hnil in Hpos; cbn in Hpos; lia. }
    pose proof (q1_group_in_of_nonempty_bucket db delta gkey Hpre Hgk Hne_b)
      as Hgin.
    apply in_map_iff.
    exists (rev (q1_inner_of_aux (q1_aux_bucket db delta gkey))).
    split; [|exact Hgin].
    destruct (q1_aux_bucket db delta gkey) as [|r rs] eqn:Eb;
      [contradiction Hne_b; reflexivity|].
    assert (Hne_g :
      rev (q1_inner_of_aux (r :: rs)) <> nil).
    { intro Hnil; apply (f_equal (@length _)) in Hnil;
        rewrite length_rev in Hnil;
        unfold q1_inner_of_aux in Hnil; rewrite length_map in Hnil;
        cbn in Hnil; discriminate. }
    destruct (rev (q1_inner_of_aux (r :: rs))) as [|row rest] eqn:Er;
      [contradiction Hne_g; reflexivity|].
    change (q1_group_head_key (row :: rest) = q1_key_values gkey).
    cbn [q1_group_head_key].
    assert (Hin_row : In row (rev (q1_inner_of_aux (r :: rs))))
      by (rewrite Er; left; reflexivity).
    apply in_rev in Hin_row.
    unfold q1_inner_of_aux in Hin_row.
    apply in_map_iff in Hin_row; destruct Hin_row as [r' [Heqrow Hinr']].
    assert (Hrok : RowOk_Q1 r').
    { pose proof (Forall_map_RowOk_aux_bucket db delta gkey Hpre) as Hok.
      rewrite Eb in Hok; apply (proj1 (Forall_forall _ _) Hok); exact Hinr'. }
    assert (Hkrow : Q1AuxSpec.RowKey r' = gkey).
    { pose proof (Forall_aux_bucket_RowKey db delta gkey) as Hkeys.
      rewrite Eb in Hkeys; apply (proj1 (Forall_forall _ _) Hkeys); exact Hinr'. }
    rewrite <- Heqrow, q1_inner_grouping_key_RowKey by exact Hrok.
    rewrite Hkrow; reflexivity.
  }
  apply Nat.le_antisymm.
  - (* length map head_key <= length entries via key-values map *)
    assert (Hlen : length (map q1_key_values (map fst entries)) = length entries)
      by (rewrite !length_map; reflexivity).
    rewrite <- Hlen.
    apply NoDup_incl_length; [exact HndG|exact Hincl1].
  - assert (Hlen : length (map q1_key_values (map fst entries)) = length entries)
      by (rewrite !length_map; reflexivity).
    rewrite <- Hlen.
    apply NoDup_incl_length; [exact HndV|exact Hincl2].
Qed.

Lemma q1_group_rows_perm_SpecFinalize_embed :
  forall db delta,
    Preconditions_Q1 db delta ->
    NumericEmbedFaithful ->
    Permutation
      (q1_group_rows_of db delta)
      (map q1_group_tuple_of_OutRow
        (Q1AuxSpec.SpecFinalize
          (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db)
            (EmbedParams_Q1 delta)))).
Proof.
  intros db delta Hpre Hnum.
  apply Permutation_sym.
  apply NoDup_Permutation_bis.
  - apply SpecFinalize_embed_NoDup; exact Hpre.
  - rewrite List.length_map.
    rewrite (length_q1_group_rows_eq_SpecFinalize db delta Hpre).
    apply Nat.le_refl.
  - intros t Hin.
    apply (proj2 (q1_group_rows_In_iff_SpecFinalize db delta t Hpre Hnum));
      exact Hin.
Qed.

Lemma q1_tuple_le_total :
  forall a b, q1_tuple_le a b = true \/ q1_tuple_le b a = true.
Proof.
  intros a b; unfold q1_tuple_le.
  pose proof (@compare_order_keys_opposite TNull value_is_null q1_order_keys a b)
    as Hop.
  destruct (@compare_order_keys TNull value_is_null q1_order_keys a b);
    destruct (@compare_order_keys TNull value_is_null q1_order_keys b a);
    simpl in Hop; try discriminate; try (left; reflexivity); try (right; reflexivity).
Qed.

Lemma SpecFinalize_embed_order_vals_inj :
  forall db delta x y,
    Preconditions_Q1 db delta ->
    In x
      (map q1_group_tuple_of_OutRow
        (Q1AuxSpec.SpecFinalize
          (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db)
            (EmbedParams_Q1 delta)))) ->
    In y
      (map q1_group_tuple_of_OutRow
        (Q1AuxSpec.SpecFinalize
          (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db)
            (EmbedParams_Q1 delta)))) ->
    q1_tuple_order_vals x = q1_tuple_order_vals y ->
    x = y.
Proof.
  intros db delta x y Hpre Hin1 Hin2 Hv.
  apply in_map_iff in Hin1; destruct Hin1 as [o1 [He1 Hin1]].
  apply in_map_iff in Hin2; destruct Hin2 as [o2 [He2 Hin2]].
  subst x y.
  rewrite !q1_group_tuple_order_vals_OutRow in Hv.
  assert (Erf : embed_flag_char (Q1AuxSpec.out_rf o1) =
                embed_flag_char (Q1AuxSpec.out_rf o2)).
  {
    pose proof (f_equal (fun l : list _ => nth 0 l (embed_flag_char 0)) Hv)
      as Hnth; cbn in Hnth; exact Hnth.
  }
  assert (Els : embed_flag_char (Q1AuxSpec.out_ls o1) =
                embed_flag_char (Q1AuxSpec.out_ls o2)).
  {
    pose proof (f_equal (fun l : list _ => nth 1 l (embed_flag_char 0)) Hv)
      as Hnth; cbn in Hnth; exact Hnth.
  }
  pose proof (SpecView_FoldSpec_OutRow_byte db delta Hpre) as Hfa.
  assert (Hb1 : 0 <= Q1AuxSpec.out_rf o1 < 256 /\ 0 <= Q1AuxSpec.out_ls o1 < 256)
    by (apply (proj1 (Forall_forall _ _) Hfa); exact Hin1).
  assert (Hb2 : 0 <= Q1AuxSpec.out_rf o2 < 256 /\ 0 <= Q1AuxSpec.out_ls o2 < 256)
    by (apply (proj1 (Forall_forall _ _) Hfa); exact Hin2).
  destruct Hb1 as [Hr1 Hl1]; destruct Hb2 as [Hr2 Hl2].
  apply embed_flag_char_inj in Erf; [|lia|lia].
  apply embed_flag_char_inj in Els; [|lia|lia].
  f_equal.
  apply (SpecFinalize_Pack_inj db delta o1 o2 Hpre Hin1 Hin2).
  f_equal; [exact Erf|exact Els].
Qed.

Lemma OutRowLe_antisym :
  forall o1 o2,
    Q1AuxSpec.OutRowLe o1 o2 = true ->
    Q1AuxSpec.OutRowLe o2 o1 = true ->
    Q1AuxSpec.out_rf o1 = Q1AuxSpec.out_rf o2 /\
    Q1AuxSpec.out_ls o1 = Q1AuxSpec.out_ls o2.
Proof.
  intros o1 o2 H12 H21.
  unfold Q1AuxSpec.OutRowLe in H12, H21.
  destruct (Z.eqb (Q1AuxSpec.out_rf o1) (Q1AuxSpec.out_rf o2)) eqn:Erf12;
    destruct (Z.eqb (Q1AuxSpec.out_rf o2) (Q1AuxSpec.out_rf o1)) eqn:Erf21.
  - rewrite Z.eqb_eq in Erf12.
    rewrite Z.leb_le in H12, H21.
    split; [exact Erf12|lia].
  - rewrite Z.eqb_eq in Erf12; rewrite Z.eqb_neq in Erf21; lia.
  - rewrite Z.eqb_neq in Erf12; rewrite Z.eqb_eq in Erf21; lia.
  - rewrite Z.leb_le in H12, H21; lia.
Qed.

Lemma OutRowLe_trans :
  forall o1 o2 o3,
    Q1AuxSpec.OutRowLe o1 o2 = true ->
    Q1AuxSpec.OutRowLe o2 o3 = true ->
    Q1AuxSpec.OutRowLe o1 o3 = true.
Proof.
  intros o1 o2 o3 H12 H23.
  unfold Q1AuxSpec.OutRowLe in *.
  destruct (Z.eqb (Q1AuxSpec.out_rf o1) (Q1AuxSpec.out_rf o2)) eqn:E12;
    destruct (Z.eqb (Q1AuxSpec.out_rf o2) (Q1AuxSpec.out_rf o3)) eqn:E23;
    destruct (Z.eqb (Q1AuxSpec.out_rf o1) (Q1AuxSpec.out_rf o3)) eqn:E13;
    rewrite ?Z.eqb_eq, ?Z.eqb_neq in *;
    rewrite ?Z.leb_le in *; lia.
Qed.

(** Bag-local ORDER BY ≤ on SpecFinalize embeds coincides with [OutRowLe]. *)
Lemma q1_tuple_le_of_OutRowLe :
  forall o1 o2,
    0 <= Q1AuxSpec.out_rf o1 < 256 ->
    0 <= Q1AuxSpec.out_ls o1 < 256 ->
    0 <= Q1AuxSpec.out_rf o2 < 256 ->
    0 <= Q1AuxSpec.out_ls o2 < 256 ->
    q1_tuple_le (q1_group_tuple_of_OutRow o1) (q1_group_tuple_of_OutRow o2) =
    Q1AuxSpec.OutRowLe o1 o2.
Proof.
  intros o1 o2 Hr1 Hl1 Hr2 Hl2.
  unfold q1_tuple_le.
  rewrite (compare_order_keys_group_tuple_OutRowLe o1 o2 Hr1 Hl1 Hr2 Hl2).
  destruct (Q1AuxSpec.OutRowLe o1 o2); destruct (Q1AuxSpec.OutRowLe o2 o1); reflexivity.
Qed.

Lemma q1_group_rows_le_trans_on :
  forall db delta,
    Preconditions_Q1 db delta ->
    NumericEmbedFaithful ->
    @leA_trans_on (tuple TNull)
      (@order_keys_le TNull value_is_null q1_order_keys)
      (q1_group_rows_of db delta).
Proof.
  intros db delta Hpre Hnum x y z Hx Hy Hz Hxy Hyz.
  pose proof (SpecView_FoldSpec_OutRow_byte db delta Hpre) as Hfa.
  assert (Hin_spec :
    forall t, In t (q1_group_rows_of db delta) ->
      In t (map q1_group_tuple_of_OutRow
        (Q1AuxSpec.SpecFinalize
          (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))))).
  { intros t Ht; apply (proj1 (q1_group_rows_In_iff_SpecFinalize db delta t Hpre Hnum)); exact Ht. }
  apply Hin_spec in Hx, Hy, Hz.
  apply in_map_iff in Hx; destruct Hx as [o1 [He1 Hin1]].
  apply in_map_iff in Hy; destruct Hy as [o2 [He2 Hin2]].
  apply in_map_iff in Hz; destruct Hz as [o3 [He3 Hin3]].
  subst x y z.
  assert (Hb1 : 0 <= Q1AuxSpec.out_rf o1 < 256 /\ 0 <= Q1AuxSpec.out_ls o1 < 256)
    by (apply (proj1 (Forall_forall _ _) Hfa); exact Hin1).
  assert (Hb2 : 0 <= Q1AuxSpec.out_rf o2 < 256 /\ 0 <= Q1AuxSpec.out_ls o2 < 256)
    by (apply (proj1 (Forall_forall _ _) Hfa); exact Hin2).
  assert (Hb3 : 0 <= Q1AuxSpec.out_rf o3 < 256 /\ 0 <= Q1AuxSpec.out_ls o3 < 256)
    by (apply (proj1 (Forall_forall _ _) Hfa); exact Hin3).
  destruct Hb1 as [Hr1 Hl1]; destruct Hb2 as [Hr2 Hl2]; destruct Hb3 as [Hr3 Hl3].
  change (q1_tuple_le (q1_group_tuple_of_OutRow o1) (q1_group_tuple_of_OutRow o2) = true) in Hxy.
  change (q1_tuple_le (q1_group_tuple_of_OutRow o2) (q1_group_tuple_of_OutRow o3) = true) in Hyz.
  change (q1_tuple_le (q1_group_tuple_of_OutRow o1) (q1_group_tuple_of_OutRow o3) = true).
  rewrite (q1_tuple_le_of_OutRowLe o1 o2 Hr1 Hl1 Hr2 Hl2) in Hxy.
  rewrite (q1_tuple_le_of_OutRowLe o2 o3 Hr2 Hl2 Hr3 Hl3) in Hyz.
  rewrite (q1_tuple_le_of_OutRowLe o1 o3 Hr1 Hl1 Hr3 Hl3).
  eapply OutRowLe_trans; eassumption.
Qed.

(** Antisym on FormalSQL group rows via SpecFinalize embeds + OutRowLe. *)
Lemma q1_group_rows_order_keys_antisym :
  forall db delta x y,
    Preconditions_Q1 db delta ->
    NumericEmbedFaithful ->
    In x (q1_group_rows_of db delta) ->
    In y (q1_group_rows_of db delta) ->
    @OrderKeysLe TNull value_is_null q1_order_keys x y ->
    @OrderKeysLe TNull value_is_null q1_order_keys y x ->
    x = y.
Proof.
  intros db delta x y Hpre Hnum Hx Hy Hab Hba.
  pose proof (SpecView_FoldSpec_OutRow_byte db delta Hpre) as Hfa.
  assert (Hin_spec :
    forall t, In t (q1_group_rows_of db delta) ->
      In t (map q1_group_tuple_of_OutRow
        (Q1AuxSpec.SpecFinalize
          (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) (EmbedParams_Q1 delta))))).
  { intros t Ht; apply (proj1 (q1_group_rows_In_iff_SpecFinalize db delta t Hpre Hnum)); exact Ht. }
  apply Hin_spec in Hx, Hy.
  apply in_map_iff in Hx; destruct Hx as [o1 [He1 Hin1]].
  apply in_map_iff in Hy; destruct Hy as [o2 [He2 Hin2]].
  subst x y.
  assert (Hb1 : 0 <= Q1AuxSpec.out_rf o1 < 256 /\ 0 <= Q1AuxSpec.out_ls o1 < 256)
    by (apply (proj1 (Forall_forall _ _) Hfa); exact Hin1).
  assert (Hb2 : 0 <= Q1AuxSpec.out_rf o2 < 256 /\ 0 <= Q1AuxSpec.out_ls o2 < 256)
    by (apply (proj1 (Forall_forall _ _) Hfa); exact Hin2).
  destruct Hb1 as [Hr1 Hl1]; destruct Hb2 as [Hr2 Hl2].
  unfold OrderKeysLe, order_keys_le in Hab, Hba.
  change (q1_tuple_le (q1_group_tuple_of_OutRow o1) (q1_group_tuple_of_OutRow o2) = true) in Hab.
  change (q1_tuple_le (q1_group_tuple_of_OutRow o2) (q1_group_tuple_of_OutRow o1) = true) in Hba.
  rewrite (q1_tuple_le_of_OutRowLe o1 o2 Hr1 Hl1 Hr2 Hl2) in Hab.
  rewrite (q1_tuple_le_of_OutRowLe o2 o1 Hr2 Hl2 Hr1 Hl1) in Hba.
  destruct (OutRowLe_antisym o1 o2 Hab Hba) as [Erf Els].
  f_equal.
  apply (SpecFinalize_Pack_inj db delta o1 o2 Hpre Hin1 Hin2).
  f_equal; [exact Erf|exact Els].
Qed.

(** Discharge [Q1_GroupBagsPerm] via SortedGroupResultFacade
    (Permutation + list-local le_trans + key antisym). *)
Lemma Q1_GroupBagsPerm_holds :
  forall db delta,
    Preconditions_Q1 db delta ->
    NumericEmbedFaithful ->
    Q1_GroupBagsPerm db delta.
Proof.
  intros db delta Hpre Hnum.
  unfold Q1_GroupBagsPerm.
  eapply (@sort_ordered_rows_eq_of_perm TNull value_is_null q1_order_keys).
  - apply q1_group_rows_perm_SpecFinalize_embed; assumption.
  - exact (q1_group_rows_le_trans_on db delta Hpre Hnum).
  - intros x y Hx Hy Hab Hba.
    apply (q1_group_rows_order_keys_antisym db delta x y Hpre Hnum Hx Hy Hab Hba).
Qed.

Lemma Q1_GroupsCorrespond_holds :
  forall db delta,
    Preconditions_Q1 db delta ->
    NumericEmbedFaithful ->
    Q1_GroupsCorrespond db delta.
Proof.
  intros db delta Hpre Hnum.
  unfold Q1_GroupsCorrespond, q1_ordered_rows_of.
  rewrite (Q1_GroupBagsPerm_holds db delta Hpre Hnum).
  unfold Q1AuxSpec.AuxAnswerSpec.
  apply sort_ordered_rows_map_OutRowSort.
  apply SpecView_FoldSpec_OutRow_byte; exact Hpre.
Qed.
