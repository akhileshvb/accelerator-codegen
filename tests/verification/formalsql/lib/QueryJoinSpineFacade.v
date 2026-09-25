(* Generic FormalSQL join-spine facade (Q21 / multi-table TPC-H shape).

   Logos-lowered Q21 shape (Calcite expand of INNER JOINs → CrossJoin+Filter):

     Project ( OrderBy ( Group ( Project ( Project (
       Filter_where ( Project ( Filter_true (
         CrossJoin
           (Project (Filter_true (CrossJoin
             (Project (Filter_true (CrossJoin
               (Project Table_supplier)
               (Project Table_lineitem))))
             (Project Table_orders))))
           (Project Table_nation)
       )))
     )))))

   Proves EQuery_* constructor composition ONCE.  Query proofs discharge
   named intermediate successes (table scans / projects / joins / filters /
   group / order / outer project) and apply [query_spine_join4_success].

   Empty-DB path never evaluates EXISTS inside the WHERE filter: Filter on
   nil rows uses [EFilterRows_Nil]. *)

Set Implicit Arguments.

From Stdlib Require Import List NArith.
Open Scope N_scope.
From SQLFS Require Import
  OrderedSet FiniteSet FiniteBag FiniteCollection FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts Values GenericInstance.
From Logos.FormalSQL Require Import TNullSyntax QueryTNullSyntax.

Import Tuple.
Import ListNotations.

Section QueryJoinSpine.

Context {T : Tuple.Rcd} {relname : Type}.

Variable basesort : relname -> Fset.set (A T).
Variable instance : relname -> Febag.bag (Fecol.CBag (CTuple T)).
Variable unknown : Bool.b (B T).
Variable symbol_runtime_error :
  scalar_operator T -> list (option sql_runtime_error * value T) ->
  option sql_runtime_error.
Variable aggregate_runtime_error :
  aggregate T -> list (option sql_runtime_error * value T) ->
  option sql_runtime_error.
Variable value_is_null : value T -> bool.
Variable boolean_schedule : boolean_site -> boolean_evaluation_order.

Local Abbreviation eval_query :=
  (@eval_query_expr_outcome T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

Local Abbreviation eval_filter :=
  (@eval_filter_rows_outcome T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

Local Abbreviation eval_project :=
  (@eval_project_rows_outcome T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

Local Abbreviation eval_group :=
  (@eval_group_bag_outcome T relname basesort instance unknown
    symbol_runtime_error aggregate_runtime_error value_is_null
    boolean_schedule).

Local Abbreviation order_rows :=
  (@order_by_rows T value_is_null).

Definition select_list := list (scalar_expr T relname ScalarResultValue * attribute T).
Definition group_key_list := list (scalar_expr T relname ScalarResultValue).
Definition bool_expr := scalar_expr T relname ScalarResultBoolean.

(** [elements (mk_bag nil)] is empty as a list. *)
Lemma elements_mk_bag_nil :
  forall (BA : Febag.Rcd (OTuple T)),
    Febag.elements BA (Febag.mk_bag BA nil) = nil.
Proof.
  intro BA.
  set (els := Febag.elements BA (Febag.mk_bag BA nil)).
  assert (Hall : forall y, Oeset.nb_occ (OTuple T) y els = 0).
  {
    intro y; unfold els.
    rewrite <- Febag.nb_occ_elements, Febag.nb_occ_mk_bag.
    reflexivity.
  }
  destruct els as [|a l]; [reflexivity|].
  specialize (Hall a); cbn in Hall.
  rewrite Oeset.compare_eq_refl in Hall.
  destruct (Oeset.nb_occ _ a l); discriminate.
Qed.

(** Cross-join of two empty row-bags is empty (nb_occ). *)
Lemma query_cross_join_nil_nil :
  query_same_rows_as_bag (T:=T) nil
    (query_cross_join_bag (query_rows_bag nil) (query_rows_bag nil)).
Proof.
  unfold query_same_rows_as_bag, query_cross_join_bag, query_rows_bag.
  rewrite Febag.nb_occ_equal; intro x.
  rewrite !Febag.nb_occ_mk_bag, !elements_mk_bag_nil.
  reflexivity.
Qed.

(** Join of nil on the left with any right bag is nil. *)
Lemma query_cross_join_nil_left :
  forall right_rows,
    query_same_rows_as_bag (T:=T) nil
      (query_cross_join_bag
         (query_rows_bag nil) (query_rows_bag right_rows)).
Proof.
  intro right_rows.
  unfold query_same_rows_as_bag, query_cross_join_bag, query_rows_bag.
  rewrite Febag.nb_occ_equal; intro x.
  rewrite !Febag.nb_occ_mk_bag, elements_mk_bag_nil.
  reflexivity.
Qed.

(** Fixed Q21-shaped AST: 4-table CrossJoin ladder + Filter_true wrappers +
    WHERE + double Project into Group/Order/outer Project. *)
Definition query_spine_join4
    (supplier_outputs lineitem_outputs orders_outputs nation_outputs
       : list (attribute T))
    (supplier lineitem orders nation : relname)
    (proj_supplier proj_lineitem proj_orders proj_nation
       proj_s_l proj_s_l_o proj_join_out
       proj_cor proj_pre_group group_select outer_select : select_list)
    (filter_true where_formula having : bool_expr)
    (group_keys : group_key_list)
    (order_keys : list (sort_key T))
  : query_expr T relname :=
  QExpr_Project outer_select
    (QExpr_OrderBy order_keys
      (QExpr_Group group_select group_keys having
        (QExpr_Project proj_pre_group
          (QExpr_Project proj_cor
            (QExpr_Filter where_formula
              (QExpr_Project proj_join_out
                (QExpr_Filter filter_true
                  (QExpr_CrossJoin
                    (QExpr_Project proj_s_l_o
                      (QExpr_Filter filter_true
                        (QExpr_CrossJoin
                          (QExpr_Project proj_s_l
                            (QExpr_Filter filter_true
                              (QExpr_CrossJoin
                                (QExpr_Project proj_supplier
                                  (QExpr_Table supplier_outputs supplier))
                                (QExpr_Project proj_lineitem
                                  (QExpr_Table lineitem_outputs lineitem)))))
                          (QExpr_Project proj_orders
                            (QExpr_Table orders_outputs orders)))))
                    (QExpr_Project proj_nation
                      (QExpr_Table nation_outputs nation)))))))))).

(** Constructor composition: leaves ⇒ membership. *)
Lemma query_spine_join4_success :
  forall (env : Env.env T)
    supplier_outputs lineitem_outputs orders_outputs nation_outputs
    supplier lineitem orders nation
    proj_supplier proj_lineitem proj_orders proj_nation
    proj_s_l proj_s_l_o proj_join_out
    proj_cor proj_pre_group group_select outer_select
    filter_true where_formula having
    group_keys order_keys
    (supplier_rows lineitem_rows orders_rows nation_rows
     proj_s_rows proj_l_rows proj_o_rows proj_n_rows
     join_sl_rows join_slo_rows join_all_rows
     filt_true1 filt_true2 filt_true3
     join_out_rows where_rows cor_rows pre_group_rows
     group_rows ordered_rows final_rows : list (tuple T))
    (group_output_bag : Febag.bag (Fecol.CBag (CTuple T))),
    query_same_rows_as_bag supplier_rows
      (@query_table_bag T relname basesort instance
         supplier_outputs supplier) ->
    query_same_rows_as_bag lineitem_rows
      (@query_table_bag T relname basesort instance
         lineitem_outputs lineitem) ->
    query_same_rows_as_bag orders_rows
      (@query_table_bag T relname basesort instance
         orders_outputs orders) ->
    query_same_rows_as_bag nation_rows
      (@query_table_bag T relname basesort instance
         nation_outputs nation) ->
    eval_project env proj_supplier supplier_rows (SqlSuccess proj_s_rows) ->
    eval_project env proj_lineitem lineitem_rows (SqlSuccess proj_l_rows) ->
    eval_project env proj_orders orders_rows (SqlSuccess proj_o_rows) ->
    eval_project env proj_nation nation_rows (SqlSuccess proj_n_rows) ->
    query_same_rows_as_bag join_sl_rows
      (query_cross_join_bag
         (query_rows_bag proj_s_rows) (query_rows_bag proj_l_rows)) ->
    eval_filter env filter_true join_sl_rows (SqlSuccess filt_true1) ->
    eval_project env proj_s_l filt_true1 (SqlSuccess join_sl_rows) ->
    (* After first Filter_true+Project, CrossJoin with orders *)
    query_same_rows_as_bag join_slo_rows
      (query_cross_join_bag
         (query_rows_bag join_sl_rows) (query_rows_bag proj_o_rows)) ->
    eval_filter env filter_true join_slo_rows (SqlSuccess filt_true2) ->
    eval_project env proj_s_l_o filt_true2 (SqlSuccess join_slo_rows) ->
    query_same_rows_as_bag join_all_rows
      (query_cross_join_bag
         (query_rows_bag join_slo_rows) (query_rows_bag proj_n_rows)) ->
    eval_filter env filter_true join_all_rows (SqlSuccess filt_true3) ->
    eval_project env proj_join_out filt_true3 (SqlSuccess join_out_rows) ->
    eval_filter env where_formula join_out_rows (SqlSuccess where_rows) ->
    eval_project env proj_cor where_rows (SqlSuccess cor_rows) ->
    eval_project env proj_pre_group cor_rows (SqlSuccess pre_group_rows) ->
    eval_group env group_select group_keys having
      (query_rows_bag pre_group_rows) (SqlSuccess group_output_bag) ->
    query_same_rows_as_bag group_rows group_output_bag ->
    order_rows order_keys group_rows ordered_rows ->
    eval_project env outer_select ordered_rows (SqlSuccess final_rows) ->
    eval_query env
      (query_spine_join4
         supplier_outputs lineitem_outputs orders_outputs nation_outputs
         supplier lineitem orders nation
         proj_supplier proj_lineitem proj_orders proj_nation
         proj_s_l proj_s_l_o proj_join_out
         proj_cor proj_pre_group group_select outer_select
         filter_true where_formula having
         group_keys order_keys)
      (SqlSuccess final_rows).
Proof.
  intros env
    supplier_outputs lineitem_outputs orders_outputs nation_outputs
    supplier lineitem orders nation
    proj_supplier proj_lineitem proj_orders proj_nation
    proj_s_l proj_s_l_o proj_join_out
    proj_cor proj_pre_group group_select outer_select
    filter_true where_formula having
    group_keys order_keys
    supplier_rows lineitem_rows orders_rows nation_rows
    proj_s_rows proj_l_rows proj_o_rows proj_n_rows
    join_sl_rows join_slo_rows join_all_rows
    filt_true1 filt_true2 filt_true3
    join_out_rows where_rows cor_rows pre_group_rows
    group_rows ordered_rows final_rows
    group_output_bag
    Hs Hl Ho Hn
    Hps Hpl Hpo Hpn
    Hjl_bag Hft1 Hpsl
    Hjlo_bag Hft2 Hpslo
    Hja_bag Hft3 Hpjo
    Hw Hcor Hpre
    Hgroup Hgroup_rows Horder Houter.
  (* Outer Project / Order / Group *)
  eapply EQuery_ProjectRows; [| exact Houter ].
  eapply EQuery_OrderBySuccess; [| exact Horder ].
  eapply EQuery_GroupBagSuccess.
  2: exact Hgroup.
  2: exact Hgroup_rows.
  (* pre-group projects + where + join ladder *)
  eapply EQuery_ProjectRows; [| exact Hpre ].
  eapply EQuery_ProjectRows; [| exact Hcor ].
  eapply EQuery_FilterRows; [| exact Hw ].
  eapply EQuery_ProjectRows; [| exact Hpjo ].
  eapply EQuery_FilterRows; [| exact Hft3 ].
  eapply EQuery_CrossJoinSuccess.
  - (* left: project filter crossjoin (s⋈l) ⋈ o *)
    eapply EQuery_ProjectRows; [| exact Hpslo ].
    eapply EQuery_FilterRows; [| exact Hft2 ].
    eapply EQuery_CrossJoinSuccess.
    + eapply EQuery_ProjectRows; [| exact Hpsl ].
      eapply EQuery_FilterRows; [| exact Hft1 ].
      eapply EQuery_CrossJoinSuccess.
      * eapply EQuery_ProjectRows; [| exact Hps ].
        eapply EQuery_Table; exact Hs.
      * eapply EQuery_ProjectRows; [| exact Hpl ].
        eapply EQuery_Table; exact Hl.
      * exact Hjl_bag.
    + eapply EQuery_ProjectRows; [| exact Hpo ].
      eapply EQuery_Table; exact Ho.
    + exact Hjlo_bag.
  - eapply EQuery_ProjectRows; [| exact Hpn ].
    eapply EQuery_Table; exact Hn.
  - exact Hja_bag.
Qed.

(** Empty specialization: all intermediate row lists are nil. *)
Lemma query_spine_join4_empty_success :
  forall (env : Env.env T)
    supplier_outputs lineitem_outputs orders_outputs nation_outputs
    supplier lineitem orders nation
    proj_supplier proj_lineitem proj_orders proj_nation
    proj_s_l proj_s_l_o proj_join_out
    proj_cor proj_pre_group group_select outer_select
    filter_true where_formula having
    group_keys order_keys,
    query_same_rows_as_bag nil
      (@query_table_bag T relname basesort instance
         supplier_outputs supplier) ->
    query_same_rows_as_bag nil
      (@query_table_bag T relname basesort instance
         lineitem_outputs lineitem) ->
    query_same_rows_as_bag nil
      (@query_table_bag T relname basesort instance
         orders_outputs orders) ->
    query_same_rows_as_bag nil
      (@query_table_bag T relname basesort instance
         nation_outputs nation) ->
    eval_group env group_select group_keys having
      (query_rows_bag (@nil (tuple T))) (SqlSuccess (query_rows_bag nil)) ->
    order_rows order_keys nil nil ->
    eval_query env
      (query_spine_join4
         supplier_outputs lineitem_outputs orders_outputs nation_outputs
         supplier lineitem orders nation
         proj_supplier proj_lineitem proj_orders proj_nation
         proj_s_l proj_s_l_o proj_join_out
         proj_cor proj_pre_group group_select outer_select
         filter_true where_formula having
         group_keys order_keys)
      (SqlSuccess nil).
Proof.
  intros env
    supplier_outputs lineitem_outputs orders_outputs nation_outputs
    supplier lineitem orders nation
    proj_supplier proj_lineitem proj_orders proj_nation
    proj_s_l proj_s_l_o proj_join_out
    proj_cor proj_pre_group group_select outer_select
    filter_true where_formula having
    group_keys order_keys
    Hs Hl Ho Hn Hgroup Horder.
  eapply query_spine_join4_success
    with
      (supplier_rows := nil) (lineitem_rows := nil)
      (orders_rows := nil) (nation_rows := nil)
      (proj_s_rows := nil) (proj_l_rows := nil)
      (proj_o_rows := nil) (proj_n_rows := nil)
      (join_sl_rows := nil) (join_slo_rows := nil)
      (join_all_rows := nil)
      (filt_true1 := nil) (filt_true2 := nil) (filt_true3 := nil)
      (join_out_rows := nil) (where_rows := nil)
      (cor_rows := nil) (pre_group_rows := nil)
      (group_rows := nil) (ordered_rows := nil) (final_rows := nil)
      (group_output_bag := query_rows_bag nil).
  all: try exact Hs; try exact Hl; try exact Ho; try exact Hn.
  all: try apply EProjectRows_Nil.
  all: try apply EFilterRows_Nil.
  all: try apply query_cross_join_nil_nil.
  - exact Hgroup.
  - apply Febag.equal_refl.
  - exact Horder.
Qed.

End QueryJoinSpine.
