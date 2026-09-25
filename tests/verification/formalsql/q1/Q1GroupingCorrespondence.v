(* Q1 FormalSQL ↔ AuxSpec correspondence: grouping / aggregate stepping stones.

   Real-Qed building blocks on the shortest path to FormalSQLAuxCorrect_Q1
   (see STATUS.md).  Each lemma is a Q1-instantiated, mechanically
   checked fact against the pinned Logos/FormalSQL grouping kernel:

     - q1_group_terms / Q1_group_is_key_bucket : GROUP BY (l_returnflag,
       l_linestatus) materializes the (rf,ls) buckets of the WHERE-filtered
       lineitem scan (partition ↔ association-list view for FoldSpec/SpecView).
     - Q1_group_count_star_exact : COUNT-star of a bucket = its length
       (the count_order output column).
     - Q1_group_sum_numeric_dot_exact : the grouped SUM of a numeric column of
       a bucket equals the pure numeric sum of that column (template for
       SUM(qty)/SUM(base_price)/SUM($f4)/SUM($f5)).
     - Q1_sum_numeric_regroup : numeric SUM is fold-associative over a partition
       (numeric counterpart of SpecMerge / the FoldSpec regroup law).

   These do NOT assert the universal FormalSQLAuxCorrect_Q1 / FormalSQLNoError_Q1
   (see STATUS.md); they are reusable per-bucket ingredients. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueNumericTypmod
  ValueString ValueInteger Formula SchemaConstraints SqlOutcome SqlQuerySyntax
  SqlQuerySemantics SqlQueryFacts SqlErrorSemantics SqlOrder
  Env Interp Projection OrderedSet
  FiniteBag FiniteCollection FiniteSet FTuples Bool3.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax
  FormalSQL.AggregateOutcomeBridgeFacts
  FormalSQL.GroupingRewriteFacts
  FormalSQL.NumericRegroupFacts
  FormalSQL.ProofAgentFacade.
From Stdlib Require Import String List ZArith Lia.
Import ListNotations.
Import Tuple.
Import NullValues.
Open Scope Z_scope.

(** Q1 GROUP BY key terms, exactly as [scalar_group_key_terms] extracts them
    from [shared_query_expr_0]'s two [SExpr_Leaf (DotString ...)] keys. *)
Definition q1_group_terms : list (@aggterm TNull) :=
  [ DotString "l_returnflag" (StringChar 1)
  ; DotString "l_linestatus" (StringChar 1) ].

Lemma q1_group_terms_nonempty : q1_group_terms <> nil.
Proof. discriminate. Qed.

(** COUNT-star over any group equals its length — the Q1 [count_order] column
    (and, via [FoldSpec], the [out_count] of the bucket's [SpecView]). *)
Lemma Q1_group_count_star_exact :
  forall env group_terms group,
    Interp.interp_aggterm TNull
      (Env.env_g TNull env (@Env.Group_By TNull group_terms) group)
      ACountStar =
      value_int64_checked (Z.of_nat (List.length group)).
Proof.
  intros env group_terms group.
  exact (proj1 (tnull_group_count_star_value_runtime_exact env group_terms group)).
Qed.

(** Q1 grouping over any row list buckets by the (l_returnflag,l_linestatus)
    key: filtering the materialized groups to those whose representative key
    equals [key] yields exactly the single accumulator-ordered bucket of the
    input rows with that key (or no group when none match).  This is the
    partition ↔ association-list bridge the AuxSpec [SpecView] needs. *)
Lemma Q1_group_is_key_bucket :
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
  intros env rows key.
  apply (query_make_groups_lookup_key_exact TNull env q1_group_terms rows key).
  exact q1_group_terms_nonempty.
Qed.

(** Exact SUM over a numeric column of a bucket: the kernel's grouped
    SUM(dot attribute) value equals the pure [interp_sum_numeric] over the
    column values.  Template for SUM(qty), SUM(base_price), SUM($f4), SUM($f5).
    (The library's [group] parameter is implicit here, hence the [@].) *)
Lemma Q1_group_sum_numeric_dot_exact :
  forall gt grp attr,
    grp <> nil ->
    Forall (fun row => attr inS labels TNull row) grp ->
    Interp.interp_aggterm TNull
      (Env.env_g TNull nil (@Env.Group_By TNull gt) grp)
      (tnull_sum_numeric_dot_term attr) =
      NullValues.interp_sum_numeric
        (map (fun row => dot TNull row attr) grp).
Proof.
  intros gt grp attr Hne Hlab.
  exact (proj1 (@tnull_closed_group_sum_numeric_dot_value_runtime_exact
                  gt grp attr Hne Hlab)).
Qed.

(** Numeric SUM fold-association law: summing per-bucket SUMs equals summing the
    flattened column.  This is the numeric-value counterpart of the AuxSpec
    [FoldSpec] / [SpecMerge] association used when regrouping a partition of
    lineitem rows into (l_returnflag, l_linestatus) buckets. *)
Lemma Q1_sum_numeric_regroup :
  forall groups,
    NullValues.interp_sum_numeric
      (map
        (fun group =>
          NullValues.interp_sum_numeric (map finite_numeric_observation group))
        groups) =
    NullValues.interp_sum_numeric
      (map finite_numeric_observation (concat groups)).
Proof.
  exact interp_sum_numeric_finite_regroup.
Qed.
