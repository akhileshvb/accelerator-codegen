(* ScanCorrespond — FormalSQL table source ↔ RelNode RelScan.
   First admitted FormalSQL→RelNode translation rule (verified fragment).
   Shared LogicalSpec proof library. *)

From Stdlib Require Import List ZArith String Bool NArith.
Import ListNotations.
Open Scope string_scope.

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueInteger
  ValueString Formula SchemaConstraints SqlOutcome SqlQuerySyntax
  SqlQuerySemantics FiniteBag FiniteCollection FiniteSet FTuples Bool3
  OrderedSet.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax FormalSQL.WitnessFacts.
From FormalSQLLogical Require Import SqlTypes Schema RelNode Eval.
From FormalSQLLogical.proof Require Import ValueCorrespond SlotMap.

Import Tuple.
Import NullValues.

(** * TableId ↔ FormalSQL relname *)

Definition relname_table_id (r : relname) : TableId :=
  match r with Rel s => s end.

Lemma relname_table_id_roundtrip :
  forall s, relname_table_id (Rel s) = s.
Proof. reflexivity. Qed.

(** * Scan correspondence *)

(** If RelNode DB table [tid] holds [rel_rows], and FormalSQL witness DB [D]
    holds [map embed rel_rows] for [table] with matching name, under a
    well-formed slot map and per-row embed correspondence, then:

    - [RelScan tid] returns exactly [rel_rows] (multiplicity preserved);
    - the ordered witness source list corresponds under [bag_correspond];
    - FormalSQL [query_table_bag] has the same bag cardinality;
    - every successful FormalSQL [QExpr_Table] outcome is bag-equal to that
      table bag (hence same multiplicity as RelScan). *)

Theorem FormalSQL_RelNode_ScanCorrespond :
  forall (sm : SlotMap) (tid : TableId) (table : relname)
         (expected : db_state)
         (outputs : list (attribute TNull))
         (embed : Row -> tuple TNull)
         (rel_rows : list Row)
         (p : Params)
         (D : db_state) (db : DB),
    SlotMap_wf sm ->
    (forall r, row_correspond sm (embed r) r) ->
    relname_table_id table = tid ->
    @_basesort TNull expected table =S=
      @query_outputs_sort TNull outputs ->
    D = witness_database expected
          [WitnessTable table (List.map embed rel_rows)] ->
    db = db_singleton tid rel_rows ->
    eval_rel (RelScan tid) p db = Some (RVRows rel_rows) /\
    bag_correspond sm (List.map embed rel_rows) rel_rows /\
    Febag.cardinal (Fecol.CBag (CTuple TNull))
      (@query_table_bag TNull relname
        (@_basesort TNull D) (@_instance TNull D) outputs table) =
    N.of_nat (List.length rel_rows) /\
    forall env schedule outcome,
      @eval_query_expr_outcome TNull relname
        (@_basesort TNull D) (@_instance TNull D)
        unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        NullValues.is_null_value
        schedule env
        (QExpr_Table outputs table) outcome ->
      exists frs,
        outcome = SqlSuccess frs /\
        @query_same_rows_as_bag TNull frs
          (@query_table_bag TNull relname
            (@_basesort TNull D) (@_instance TNull D) outputs table).
Proof.
  intros sm tid table expected outputs embed rel_rows p D db
    Hwf Hembed Htid Hsort HD Hdb.
  split.
  - (* RelScan *)
    rewrite Hdb. unfold db_singleton, eval_rel, db_lookup.
    rewrite String.eqb_refl. reflexivity.
  - split.
    + apply bag_correspond_map_embed; exact Hembed.
    + split.
      * rewrite HD.
        etransitivity.
        {
          apply (witness_query_table_bag_cardinal_generated_sort
            expected [WitnessTable table (List.map embed rel_rows)]
            outputs table Hsort).
        }
        cbn [witness_rows_for witness_table_relation witness_table_rows].
        rewrite (proj2 (Oset.eq_bool_true_iff ORN table table) eq_refl).
        rewrite length_map.
        reflexivity.
      * intros env schedule outcome Hout.
        inversion Hout; subst.
        eexists; split; [reflexivity | assumption].
Qed.

Corollary FormalSQL_RelNode_ScanCorrespond_nil :
  forall (sm : SlotMap) (tid : TableId) (table : relname)
         (expected : db_state)
         (outputs : list (attribute TNull))
         (embed : Row -> tuple TNull)
         (p : Params)
         (D : db_state) (db : DB),
    SlotMap_wf sm ->
    (forall r, row_correspond sm (embed r) r) ->
    relname_table_id table = tid ->
    @_basesort TNull expected table =S=
      @query_outputs_sort TNull outputs ->
    D = witness_database expected [WitnessTable table nil] ->
    db = db_singleton tid nil ->
    eval_rel (RelScan tid) p db = Some (RVRows nil) /\
    bag_correspond sm nil nil /\
    Febag.cardinal (Fecol.CBag (CTuple TNull))
      (@query_table_bag TNull relname
        (@_basesort TNull D) (@_instance TNull D) outputs table) = 0%N.
Proof.
  intros sm tid table expected outputs embed p D db
    Hwf Hembed Htid Hsort HD Hdb.
  pose proof (FormalSQL_RelNode_ScanCorrespond
    sm tid table expected outputs embed nil p D db
    Hwf Hembed Htid Hsort HD Hdb) as H.
  destruct H as [Hscan [Hbag [Hcard _]]].
  repeat split; try assumption.
Qed.
