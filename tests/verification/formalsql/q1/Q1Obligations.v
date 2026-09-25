(* Q1 FormalSQL ↔ AuxSpec: fixed semantic obligations (LLM proof-agent interface).

   Architecture (see STATUS.md / PIPELINE.md):
     exact Cat(Q1) --Logos--> qhat_Q1 --[these Qed obligations]--> AuxAnswerSpec
     AuxAnswerSpec --Dafny Twin≡Spec--> Twin --generic runtime--> accelerator

   Design rule: each obligation is a thin Q1 instantiation of a reusable
   FormalSQL facade / AuxSpec fact.  Do NOT re-derive FormalSQL internals,
   digit-count, or Qred here.  Overflow is [Q1NumericSafe] (TCB hypothesis).

   Policy: fail-closed on this path (no trusted holes; no open proofs). *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueNumericTypmod
  ValueString ValueInteger Formula SchemaConstraints SqlOutcome SqlQuerySyntax
  SqlQuerySemantics SqlQueryFacts SqlErrorSemantics SqlOrder
  Env Interp Projection OrderedSet
  FiniteBag FiniteCollection FiniteSet FTuples Bool3.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax FormalSQL.WitnessFacts
  FormalSQL.AggregateOutcomeBridgeFacts
  FormalSQL.GroupingRewriteFacts
  FormalSQL.NumericRegroupFacts
  FormalSQL.ProofAgentFacade.
From LogosGenerated Require Import Schema Queries.
From JetStreamFS Require Import Q1AuxSpec.
From Stdlib Require Import String Ascii List ZArith Lia Ring Sorted Permutation.
Import ListNotations.
Import Tuple.
Import NullValues.
Open Scope string_scope.
Open Scope Z_scope.

Require Import Q1Catalog Q1FormalSQLProof Q1GroupingCorrespondence Q1AuxCorrectAssembly.

(** * Outcome membership (alias for the pipeline statement) *)

Definition FormalSQLOutcomes (D : db_state) (delta : Z)
    (outs : list (tuple TNull)) : Prop :=
  FormalSQL_exec D delta outs.

(** * 1. FilterEq_Q1 — WHERE shipdate ≤ cutoff ↔ AuxSpec SpecView membership

    Spec-side: after FoldSpec, SpecView at [cutoff] accumulates exactly the
    contributions of rows with [shipdate ≤ cutoff], keyed by [RowKey]. *)
Lemma FilterEq_Q1 :
  forall db cutoff gkey,
    keystore_get (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) cutoff) gkey =
    sum_contrib_matching
      (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) cutoff) db) gkey.
Proof.
  exact SpecView_FoldSpec_key_stats.
Qed.

(** * 2. GroupEq_Q1 — GROUP BY (returnflag, linestatus) ↔ AuxSpec RowKey buckets

    FormalSQL [query_make_groups] on Q1 group terms equals the association-list
    bucket of rows sharing a key (facade: [query_make_groups_lookup_key_exact]). *)
Lemma GroupEq_Q1 :
  forall env rows key,
    filter
      (fun members =>
        match members with
        | nil => false
        | row :: _ =>
            Oset.eq_bool (OrderedSet.mk_olists OVal)
              (query_grouping_key env q1_group_terms row) key
        end)
      (@query_make_groups TNull env rows q1_group_terms) =
    match
      filter
        (fun row =>
          Oset.eq_bool (OrderedSet.mk_olists OVal)
            (query_grouping_key env q1_group_terms row) key)
        rows
    with
    | nil => nil
    | _ :: _ =>
        [rev
          (filter
            (fun row =>
              Oset.eq_bool (OrderedSet.mk_olists OVal)
                (query_grouping_key env q1_group_terms row) key)
            rows)]
    end.
Proof.
  exact Q1_group_is_key_bucket.
Qed.

(** * 3. CountEq_Q1 — COUNT-star = AuxSpec count *)
Lemma CountEq_Q1 :
  forall env group_terms group,
    Interp.interp_aggterm TNull
      (Env.env_g TNull env (@Env.Group_By TNull group_terms) group)
      ACountStar =
      value_int64_checked (Z.of_nat (List.length group)).
Proof.
  exact Q1_group_count_star_exact.
Qed.

Lemma CountEq_Q1_spec_bucket :
  forall rows gkey,
    Q1AuxSpec.count (sum_contrib_matching rows gkey) =
    Z.of_nat (length (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey) rows)).
Proof.
  exact sum_contrib_count.
Qed.

Lemma SumQtyEq_Q1_spec_bucket :
  forall db cutoff gkey,
    Q1AuxSpec.sum_qty
      (keystore_get (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) cutoff) gkey) =
    fold_right Z.add 0
      (map Q1AuxSpec.qty
        (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey)
          (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) cutoff) db))).
Proof.
  exact SpecView_FoldSpec_key_sum_qty.
Qed.

Lemma SumBasePriceEq_Q1_spec_bucket :
  forall db cutoff gkey,
    Q1AuxSpec.sum_base_price
      (keystore_get (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) cutoff) gkey) =
    fold_right Z.add 0
      (map Q1AuxSpec.price
        (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey)
          (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) cutoff) db))).
Proof.
  exact SpecView_FoldSpec_key_sum_base_price.
Qed.

Lemma SumDiscPriceEq_Q1_spec_bucket :
  forall db cutoff gkey,
    Q1AuxSpec.sum_disc_price
      (keystore_get (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) cutoff) gkey) =
    fold_right Z.add 0
      (map (fun r => Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r))
        (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey)
          (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) cutoff) db))).
Proof.
  exact SpecView_FoldSpec_key_sum_disc_price.
Qed.

Lemma SumChargeEq_Q1_spec_bucket :
  forall db cutoff gkey,
    Q1AuxSpec.sum_charge
      (keystore_get (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) cutoff) gkey) =
    fold_right Z.add 0
      (map (fun r => Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r)
                       * (100 + Q1AuxSpec.tax r))
        (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey)
          (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) cutoff) db))).
Proof.
  exact SpecView_FoldSpec_key_sum_charge.
Qed.

Lemma SumDiscountEq_Q1_spec_bucket :
  forall db cutoff gkey,
    Q1AuxSpec.sum_discount
      (keystore_get (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec db) cutoff) gkey) =
    fold_right Z.add 0
      (map Q1AuxSpec.disc
        (filter (fun r => Z.eqb (Q1AuxSpec.RowKey r) gkey)
          (filter (fun r => Z.leb (Q1AuxSpec.shipdate r) cutoff) db))).
Proof.
  exact SpecView_FoldSpec_key_sum_discount.
Qed.

(** * 4–8. Sum*Eq_Q1 — FormalSQL SUM(dot) = pure numeric sum (template)

    Instantiates the reusable closed-group SUM bridge.  Column-specific
    Q1 SUMs (qty / base_price / disc_price / charge) apply this at the
    projected attribute / derived column. *)
Lemma SumNumericDotEq_Q1 :
  forall gt grp attr,
    grp <> nil ->
    Forall (fun row => attr inS labels TNull row) grp ->
    Interp.interp_aggterm TNull
      (Env.env_g TNull nil (@Env.Group_By TNull gt) grp)
      (tnull_sum_numeric_dot_term attr) =
      NullValues.interp_sum_numeric
        (map (fun row => dot TNull row attr) grp).
Proof.
  exact Q1_group_sum_numeric_dot_exact.
Qed.

Lemma SumQtyEq_Q1_embed_column :
  forall r,
    dot TNull (EmbedRow_Q1 r) (AttrDecimal "l_quantity" 15 2) =
    embed_decimal2 (Q1AuxSpec.qty r).
Proof.
  exact EmbedRow_dot_qty.
Qed.

Lemma SumBasePriceEq_Q1_embed_column :
  forall r,
    dot TNull (EmbedRow_Q1 r) (AttrDecimal "l_extendedprice" 15 2) =
    embed_decimal2 (Q1AuxSpec.price r).
Proof.
  exact EmbedRow_dot_price.
Qed.

(** SpecApply encodes disc_price / charge contributions used by SumDiscPrice /
    SumCharge AuxSpec accumulators (definitional; no FormalSQL internals). *)
Lemma SumDiscPriceEq_Q1_spec_contrib :
  forall s r sign,
    Q1AuxSpec.sum_disc_price (Q1AuxSpec.SpecApply s r sign) =
    Q1AuxSpec.sum_disc_price s + sign * (Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r)).
Proof.
  intros; unfold Q1AuxSpec.SpecApply; cbn; ring.
Qed.

Lemma SumChargeEq_Q1_spec_contrib :
  forall s r sign,
    Q1AuxSpec.sum_charge (Q1AuxSpec.SpecApply s r sign) =
    Q1AuxSpec.sum_charge s
      + sign * (Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r)
                * (100 + Q1AuxSpec.tax r)).
Proof.
  intros; unfold Q1AuxSpec.SpecApply; cbn; ring.
Qed.

Lemma SumDiscountEq_Q1_spec_contrib :
  forall s r sign,
    Q1AuxSpec.sum_discount (Q1AuxSpec.SpecApply s r sign) =
    Q1AuxSpec.sum_discount s + sign * Q1AuxSpec.disc r.
Proof.
  intros; unfold Q1AuxSpec.SpecApply; cbn; ring.
Qed.

Lemma SumQtyEq_Q1_spec_contrib :
  forall s r sign,
    Q1AuxSpec.sum_qty (Q1AuxSpec.SpecApply s r sign) =
    Q1AuxSpec.sum_qty s + sign * Q1AuxSpec.qty r.
Proof.
  intros; unfold Q1AuxSpec.SpecApply; cbn; ring.
Qed.

Lemma SumBasePriceEq_Q1_spec_contrib :
  forall s r sign,
    Q1AuxSpec.sum_base_price (Q1AuxSpec.SpecApply s r sign) =
    Q1AuxSpec.sum_base_price s + sign * Q1AuxSpec.price r.
Proof.
  intros; unfold Q1AuxSpec.SpecApply; cbn; ring.
Qed.

(** Regroup law for composing per-bucket SUMs (facade). *)
Lemma SumRegroupEq_Q1 :
  forall groups,
    NullValues.interp_sum_numeric
      (map
        (fun group =>
          NullValues.interp_sum_numeric (map finite_numeric_observation group))
        groups) =
    NullValues.interp_sum_numeric
      (map finite_numeric_observation (concat groups)).
Proof.
  exact Q1_sum_numeric_regroup.
Qed.

(** * 9. AvgEq_Q1 — FormalSQL AVG observation uses numeric_div_at_scales

    EmbedResult AVG columns use [embed_avg_from_sum_scale2], not Twin AvgRound.
    Scale witness + mismatch documentation are Qed in Q1FormalSQLProof.v. *)
Lemma AvgEq_Q1_output_scale :
  numeric_pg_div_scale (numeric_of_scaled 1 2) 2 (numeric_of_Z 2) 0 = Some 20.
Proof.
  exact Q1_avg_output_scale_is_20.
Qed.

Lemma AvgEq_Q1_not_avground :
  NullValues.interp_avg_numeric_fixed 15 2
    (Value_numeric (Some (numeric_of_scaled 0 2))
     :: Value_numeric (Some (numeric_of_scaled 1 2)) :: nil)
  <> Value_numeric (Some (numeric_of_scaled (Q1AuxSpec.AvgRound 1 2) 2)).
Proof.
  exact Q1_avg_formalsql_vs_avground_mismatch.
Qed.

(** * 10. ProjectionEq_Q1 — EmbedOutRow is the final output row shape *)
Lemma ProjectionEq_Q1_nil :
  EmbedResult_Q1 nil = nil.
Proof.
  exact EmbedResult_nil.
Qed.

Lemma ProjectionEq_Q1_cons :
  forall o outs,
    EmbedResult_Q1 (o :: outs) =
    EmbedOutRow_Q1 o :: EmbedResult_Q1 outs.
Proof.
  intros; reflexivity.
Qed.

(** * 11. OrderEq_Q1 — ORDER BY (rf, ls) = AuxSpec OutRowSort *)
Lemma OrderEq_Q1_sorted :
  forall DB theta, Sorted OutRle (Q1AuxSpec.AuxAnswerSpec DB theta).
Proof.
  exact AuxAnswerSpec_sorted.
Qed.

Lemma OrderEq_Q1_perm :
  forall DB theta,
    Permutation
      (Q1AuxSpec.SpecFinalize
         (Q1AuxSpec.SpecView (Q1AuxSpec.FoldSpec DB) theta))
      (Q1AuxSpec.AuxAnswerSpec DB theta).
Proof.
  exact AuxAnswerSpec_perm.
Qed.

(** * Catalog / Embed hygiene (kept, useful for agents) *)
Lemma CatalogEq_Q1 : catalog_sql_sha256_ok.
Proof. exact catalog_sql_sha256_matches. Qed.

Lemma UnobservedFieldsEq_Q1 :
  forall r r' : Q1AuxSpec.Row,
    Q1AuxSpec.shipdate r = Q1AuxSpec.shipdate r' ->
    Q1AuxSpec.qty r = Q1AuxSpec.qty r' ->
    Q1AuxSpec.price r = Q1AuxSpec.price r' ->
    Q1AuxSpec.disc r = Q1AuxSpec.disc r' ->
    Q1AuxSpec.tax r = Q1AuxSpec.tax r' ->
    Q1AuxSpec.rf r = Q1AuxSpec.rf r' ->
    Q1AuxSpec.ls r = Q1AuxSpec.ls r' ->
    EmbedRow_Q1 r = EmbedRow_Q1 r'.
Proof.
  exact Q1_unobserved_fields_irrelevant.
Qed.

(** * Top-level Box 1 theorems

    NoError: Qed under Pre + Q1NumericSafe (overflow TCB packaging).
    AuxCorrect: empty DB Qed; nonempty = composition of the obligations above
    into FormalSQL constructor membership — see STATUS open list
    [FormalSQLSpine_Q1].  Fail-closed: the universal nonempty theorem is not
    asserted until that spine is a real Qed via reusable facade lemmas. *)

Theorem FormalSQLNoError_Q1_obligation :
  forall (db : list Q1AuxSpec.Row) (delta : Z)
         (o : sql_outcome (list (tuple TNull))),
    Preconditions_Q1 db delta ->
    Q1NumericSafe db ->
    FormalSQL_outcome (EmbedDB_Q1 db) delta o ->
    exists rows, o = SqlSuccess rows.
Proof.
  exact FormalSQLNoError_Q1.
Qed.

Theorem FormalSQLAuxCorrect_Q1_nil_obligation :
  forall delta,
    Preconditions_Q1 nil delta ->
    FormalSQLOutcomes (EmbedDB_Q1 nil) delta
      (EmbedResult_Q1 (Q1AuxSpec.AuxAnswerSpec nil (EmbedParams_Q1 delta))).
Proof.
  exact FormalSQLAuxCorrect_Q1_nil.
Qed.

(** Remaining open: none for AuxCorrect bag order — [Q1_GroupBagsPerm]
    discharged in [Q1GroupsCorrespond]. Nonempty AuxCorrect is
    [FormalSQLAuxCorrect_Q1] under Pre + Q1NumericSafe + NumericEmbedFaithful.
    [Q1_GroupsCorrespond] is Qed via [Q1_GroupsCorrespond_holds]. *)
