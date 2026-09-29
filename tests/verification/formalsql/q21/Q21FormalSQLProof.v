(* Q21FormalSQLProof — EmbedDB / Preconditions / FormalSQL_exec / tightened wrappers.
   No Admit/Axiom. Empty DB is not the public Pre domain. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueNumericTypmod
  ValueString ValueInteger ValueTemporal Formula SchemaConstraints SqlOutcome
  SqlQuerySyntax SqlQuerySemantics SqlOrder FiniteBag FiniteCollection
  FiniteSet FTuples Bool3 OrderedSet.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax FormalSQL.WitnessFacts.
From LogosGenerated Require Import Schema Queries.
From FormalSQLLogical Require Import SqlTypes Schema RelNode Eval.
From FormalSQLLogical.proof Require Import ValueCorrespond SlotMap ScanCorrespond.
From Stdlib Require Import String List ZArith NArith Bool Lia.
Import ListNotations.
Import Tuple.
Import NullValues.
Open Scope string_scope.
Open Scope Z_scope.

Require Import Q21Schema.
Require Import Q21LogicalSpecTree.
Require Import Q21Qhat.

(** * Schedule / null *)

Definition q21_boolean_schedule : boolean_site -> boolean_evaluation_order :=
  fun _ => BooleanLeftFirst.

Definition value_is_null (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

(** * RelNode DB view *)

Definition db_empty_Q21 : DB :=
  [(table_supplier, []);
   (table_lineitem, []);
   (table_orders, []);
   (table_nation, [])].

Definition Params_Q21 (nation_name : string) : Params :=
  params_nation nation_name.

Definition Eval_Spec_Q21 (nation_name : string) (db : DB)
  : option (list Row) :=
  Eval_Spec LogicalSpec_Q21_rel (Params_Q21 nation_name) db.

Lemma Params_Q21_catalog_instance :
  Params_Q21 catalog_nation_name = params_catalog_nation.
Proof. reflexivity. Qed.

Lemma Eval_Spec_Q21_empty :
  forall nation_name,
    Eval_Spec_Q21 nation_name db_empty_Q21 = Some [].
Proof. intros; reflexivity. Qed.

(** * Row well-formedness (watched schemas) *)

Definition row_ok_supplier (r : Row) : bool :=
  match r with
  | [VInt _; VText _; VInt _] => true
  | _ => false
  end.

Definition row_ok_lineitem (r : Row) : bool :=
  match r with
  | [VInt _; VInt _; VInt _; VInt _] => true
  | _ => false
  end.

Definition row_ok_orders (r : Row) : bool :=
  match r with
  | [VInt _; VText _] => true
  | _ => false
  end.

Definition row_ok_nation (r : Row) : bool :=
  match r with
  | [VInt _; VText _] => true
  | _ => false
  end.

Definition rows_ok (ok : Row -> bool) (rs : list Row) : bool :=
  List.forallb ok rs.

Definition DB_ok_Q21 (db : DB) : bool :=
  match db_lookup db table_supplier,
        db_lookup db table_lineitem,
        db_lookup db table_orders,
        db_lookup db table_nation with
  | Some ss, Some ls, Some os, Some ns =>
      andb (rows_ok row_ok_supplier ss)
      (andb (rows_ok row_ok_lineitem ls)
      (andb (rows_ok row_ok_orders os)
            (rows_ok row_ok_nation ns)))
  | _, _, _, _ => false
  end.

(** Nation query parameter — supported representation domain.
    - length ≤ 25: fits TPC-H [n_name] CHAR(25) / FormalSQL embed.
    - [string_rtrim_spaces] identity: FormalSQL [CstString StringBpchar]
      stores [string_explicit_cast] (= rtrim), while RelNode [EParam]
      compares with [String.eqb] on the raw parameter. TPC-H nation names
      are trimmed identifiers; this is a representation-domain constraint
      on the parameter, not a semantic weakening of equality. *)
Definition NationNameOk (nation_name : string) : Prop :=
  (String.length nation_name <= 25)%nat /\
  string_rtrim_spaces nation_name = nation_name.

Lemma catalog_nation_name_ok : NationNameOk catalog_nation_name.
Proof.
  unfold NationNameOk, catalog_nation_name.
  split; [vm_compute; lia | vm_compute; reflexivity].
Qed.

(** CHAR(n) carrier already in FormalSQL canonical form (cast is identity).
    TPC-H: [o_orderstatus] is CHAR(1) ∈ {F,O,P}; [n_name]/[s_name] are
    trimmed CHAR(25) strings. Representation-domain constraints matching
    PostgreSQL CHAR storage after canonicalization — not semantic
    weakenings: FormalSQL [sql_string_compare] already rtrims CHAR/Bpchar. *)
Definition Char1Canonical (s : string) : Prop :=
  string_explicit_cast (StringChar 1) s = s.

Definition Char25Canonical (s : string) : Prop :=
  string_explicit_cast (StringChar 25) s = s.

(** * Embed RelNode watched rows → FormalSQL full catalog tuples *)

Definition empty_varchar40 : NullValues.value :=
  Value_string (StringValue (StringVarcharN 40) (Some "")).
Definition empty_char15 : NullValues.value :=
  Value_string (StringValue (StringChar 15) (Some "")).
Definition empty_varchar101 : NullValues.value :=
  Value_string (StringValue (StringVarcharN 101) (Some "")).
Definition empty_varchar152 : NullValues.value :=
  Value_string (StringValue (StringVarcharN 152) (Some "")).
Definition empty_varchar79 : NullValues.value :=
  Value_string (StringValue (StringVarcharN 79) (Some "")).
Definition empty_varchar44 : NullValues.value :=
  Value_string (StringValue (StringVarcharN 44) (Some "")).
Definition empty_char25 : NullValues.value :=
  Value_string (StringValue (StringChar 25) (Some "")).
Definition empty_char10 : NullValues.value :=
  Value_string (StringValue (StringChar 10) (Some "")).
Definition empty_char1 : NullValues.value :=
  Value_string (StringValue (StringChar 1) (Some "")).
Definition empty_char15p : NullValues.value := empty_char15.
Definition zero_numeric_15_2 : NullValues.value :=
  Value_numeric (Some (numeric_of_scaled 0 2)).

Definition supplier_formal_attrs : list (attribute TNull) :=
  [Attr_int32 "s_suppkey";
   Attr_string "s_name" (StringChar 25);
   Attr_string "s_address" (StringVarcharN 40);
   Attr_int32 "s_nationkey";
   Attr_string "s_phone" (StringChar 15);
   Attr_decimal "s_acctbal" 15 2;
   Attr_string "s_comment" (StringVarcharN 101)].

Definition EmbedRow_supplier (r : Row) : option (tuple TNull) :=
  match r with
  | [VInt sk; VText sn; VInt nk] =>
      match int32_checked sk, int32_checked nk with
      | Some sk32, Some nk32 =>
          Some (mk_tuple_lists supplier_formal_attrs
            [Value_int32 (Some sk32);
             Value_string (StringValue (StringChar 25)
               (Some (string_explicit_cast (StringChar 25) sn)));
             empty_varchar40;
             Value_int32 (Some nk32);
             empty_char15;
             zero_numeric_15_2;
             empty_varchar101])
      | _, _ => None
      end
  | _ => None
  end.

Definition EmbedRow_lineitem (r : Row) : option (tuple TNull) :=
  match r with
  | [VInt ok; VInt sk; VInt cd; VInt rd] =>
      match int32_checked ok, int32_checked sk with
      | Some ok32, Some sk32 =>
          Some (mk_tuple_lists
            [Attr_int32 "l_orderkey"; Attr_int32 "l_partkey";
             Attr_int32 "l_suppkey"; Attr_int32 "l_linenumber";
             Attr_decimal "l_quantity" 15 2; Attr_decimal "l_extendedprice" 15 2;
             Attr_decimal "l_discount" 15 2; Attr_decimal "l_tax" 15 2;
             Attr_string "l_returnflag" (StringChar 1);
             Attr_string "l_linestatus" (StringChar 1);
             Attr_date "l_shipdate"; Attr_date "l_commitdate";
             Attr_date "l_receiptdate";
             Attr_string "l_shipinstruct" (StringChar 25);
             Attr_string "l_shipmode" (StringChar 10);
             Attr_string "l_comment" (StringVarcharN 44)]
            [Value_int32 (Some ok32); Value_int32 (Some (int32_from_twos_complement 0));
             Value_int32 (Some sk32); Value_int32 (Some (int32_from_twos_complement 0));
             zero_numeric_15_2; zero_numeric_15_2; zero_numeric_15_2; zero_numeric_15_2;
             empty_char1; empty_char1;
             Value_date (Some 0); Value_date (Some cd); Value_date (Some rd);
             empty_char25; empty_char10; empty_varchar44])
      | _, _ => None
      end
  | _ => None
  end.

Definition EmbedRow_orders (r : Row) : option (tuple TNull) :=
  match r with
  | [VInt ok; VText st] =>
      match int32_checked ok with
      | Some ok32 =>
          Some (mk_tuple_lists
            [Attr_int32 "o_orderkey"; Attr_int32 "o_custkey";
             Attr_string "o_orderstatus" (StringChar 1);
             Attr_decimal "o_totalprice" 15 2; Attr_date "o_orderdate";
             Attr_string "o_orderpriority" (StringChar 15);
             Attr_string "o_clerk" (StringChar 15);
             Attr_int32 "o_shippriority";
             Attr_string "o_comment" (StringVarcharN 79)]
            [Value_int32 (Some ok32);
             Value_int32 (Some (int32_from_twos_complement 0));
             Value_string (StringValue (StringChar 1)
               (Some (string_explicit_cast (StringChar 1) st)));
             zero_numeric_15_2; Value_date (Some 0);
             empty_char15p; empty_char15p;
             Value_int32 (Some (int32_from_twos_complement 0));
             empty_varchar79])
      | None => None
      end
  | _ => None
  end.

Definition EmbedRow_nation (r : Row) : option (tuple TNull) :=
  match r with
  | [VInt nk; VText nn] =>
      match int32_checked nk with
      | Some nk32 =>
          Some (mk_tuple_lists
            [Attr_int32 "n_nationkey";
             Attr_string "n_name" (StringChar 25);
             Attr_int32 "n_regionkey";
             Attr_string "n_comment" (StringVarcharN 152)]
            [Value_int32 (Some nk32);
             Value_string (StringValue (StringChar 25)
               (Some (string_explicit_cast (StringChar 25) nn)));
             Value_int32 (Some (int32_from_twos_complement 0));
             empty_varchar152])
      | None => None
      end
  | _ => None
  end.

Fixpoint embed_rows (embed : Row -> option (tuple TNull)) (rs : list Row)
  : option (list (tuple TNull)) :=
  match rs with
  | [] => Some []
  | r :: rest =>
      match embed r, embed_rows embed rest with
      | Some t, Some ts => Some (t :: ts)
      | _, _ => None
      end
  end.

Definition EmbedDB_Q21 (db : DB) : option db_state :=
  match db_lookup db table_supplier,
        db_lookup db table_lineitem,
        db_lookup db table_orders,
        db_lookup db table_nation with
  | Some ss, Some ls, Some os, Some ns =>
      match embed_rows EmbedRow_supplier ss,
            embed_rows EmbedRow_lineitem ls,
            embed_rows EmbedRow_orders os,
            embed_rows EmbedRow_nation ns with
      | Some ss', Some ls', Some os', Some ns' =>
          Some (witness_database Schema.generated_schema
            [WitnessTable (Rel "supplier") ss';
             WitnessTable (Rel "lineitem") ls';
             WitnessTable (Rel "orders") os';
             WitnessTable (Rel "nation") ns'])
      | _, _, _, _ => None
      end
  | _, _, _, _ => None
  end.

Lemma EmbedDB_Q21_empty :
  EmbedDB_Q21 db_empty_Q21 = Some
    (witness_database Schema.generated_schema
      [WitnessTable (Rel "supplier") nil;
       WitnessTable (Rel "lineitem") nil;
       WitnessTable (Rel "orders") nil;
       WitnessTable (Rel "nation") nil]).
Proof. reflexivity. Qed.

(** * CHAR-domain facts for watched text carriers (TPC-H representation)

    [Char1Canonical] / [Char25Canonical]: FormalSQL [string_explicit_cast]
    is identity on the RelNode [VText] carrier. TPC-H stores
    [o_orderstatus] ∈ {F,O,P} as CHAR(1) and [n_name]/[s_name] as trimmed
    CHAR(25) — after PostgreSQL CHAR canonicalization these equal the
    unpadded carriers. Representation-domain constraints, not semantic
    weakenings: FormalSQL [sql_string_compare] already rtrims CHAR/Bpchar. *)

Definition row_supplier_char_ok (r : Row) : Prop :=
  match r with
  | [VInt _; VText sn; VInt _] => Char25Canonical sn
  | _ => False
  end.

Definition row_orders_char_ok (r : Row) : Prop :=
  match r with
  | [VInt _; VText st] => Char1Canonical st
  | _ => False
  end.

Definition row_nation_char_ok (r : Row) : Prop :=
  match r with
  | [VInt _; VText nn] => Char25Canonical nn
  | _ => False
  end.

Definition DB_CharCanonical_Q21 (db : DB) : Prop :=
  match db_lookup db table_supplier,
        db_lookup db table_orders,
        db_lookup db table_nation with
  | Some ss, Some os, Some ns =>
      List.Forall row_supplier_char_ok ss /\
      List.Forall row_orders_char_ok os /\
      List.Forall row_nation_char_ok ns
  | _, _, _ => False
  end.

(** * Preconditions — nonempty domain (not empty-only) *)

Definition Preconditions_Q21 (db : DB) (nation_name : string) : Prop :=
  DB_ok_Q21 db = true /\
  NationNameOk nation_name /\
  DB_CharCanonical_Q21 db /\
  EmbedDB_Q21 db <> None.

Lemma Preconditions_Q21_empty :
  forall nation_name,
    NationNameOk nation_name ->
    Preconditions_Q21 db_empty_Q21 nation_name.
Proof.
  intros nation_name Hn.
  unfold Preconditions_Q21.
  split; [|split; [|split]].
  - reflexivity.
  - exact Hn.
  - unfold DB_CharCanonical_Q21, db_empty_Q21, db_lookup; simpl.
    repeat split; constructor.
  - rewrite EmbedDB_Q21_empty; discriminate.
Qed.

(** * FormalSQL_exec / result embed *)

Definition FormalSQL_exec (D : db_state) (q : QueryExpr)
    (outs : list (tuple TNull)) : Prop :=
  @eval_query_expr_possible_outcome TNull relname
    (@_basesort TNull D)
    (@_instance TNull D)
    unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null
    nil
    q
    (SqlSuccess outs).

Definition q21_out_attrs : list (attribute TNull) :=
  [Attr_string "__logos_output_0" (StringChar 25);
   Attr_int64 "__logos_output_1"].

Definition Embed_Q21_Result_row (r : Row) : option (tuple TNull) :=
  match r with
  | [VText s; VInt n] =>
      match int64_checked n with
      | Some i =>
          Some (mk_tuple_lists q21_out_attrs
                  [Value_string (StringValue (StringChar 25)
                     (Some (string_explicit_cast (StringChar 25) s)));
                   Value_int64 (Some i)])
      | None => None
      end
  | _ => None
  end.

Fixpoint Embed_Q21_Result (rs : list Row) : option (list (tuple TNull)) :=
  match rs with
  | [] => Some []
  | r :: rest =>
      match Embed_Q21_Result_row r, Embed_Q21_Result rest with
      | Some t, Some ts => Some (t :: ts)
      | _, _ => None
      end
  end.

Lemma Embed_Q21_Result_nil : Embed_Q21_Result nil = Some nil.
Proof. reflexivity. Qed.

(** * Nonempty embed domain (sanity example, not empty-only Pre) *)

Definition supplier_row_example : Row :=
  [VInt 1; VText "Supplier#000000001"; VInt 0].

Definition db_nonempty_supplier_Q21 : DB :=
  [(table_supplier, [supplier_row_example]);
   (table_lineitem, []);
   (table_orders, []);
   (table_nation, [])].

Lemma DB_ok_Q21_nonempty_supplier :
  DB_ok_Q21 db_nonempty_supplier_Q21 = true.
Proof. reflexivity. Qed.

Lemma int32_checked_nonneg_small :
  forall z, (0 <= z <= 100)%Z -> exists r, int32_checked z = Some r.
Proof.
  intros z Hz.
  unfold int32_checked.
  destruct (Z.leb_spec0 int32_min z) as [Hlo|Hlo].
  2: {
    (* int32_min <= 0 <= z, contradiction *)
    unfold int32_min in Hlo; lia.
  }
  destruct (Z.leb_spec0 z int32_max) as [Hhi|Hhi].
  2: {
    unfold int32_max in Hhi; lia.
  }
  eexists; reflexivity.
Qed.

Lemma EmbedRow_supplier_example_Some :
  exists ft, EmbedRow_supplier supplier_row_example = Some ft.
Proof.
  unfold supplier_row_example, EmbedRow_supplier.
  destruct (int32_checked_nonneg_small 1 ltac:(lia)) as [sk32 Hsk].
  destruct (int32_checked_nonneg_small 0 ltac:(lia)) as [nk32 Hnk].
  rewrite Hsk, Hnk.
  eexists; reflexivity.
Qed.

Lemma EmbedDB_Q21_nonempty_supplier_Some :
  EmbedDB_Q21 db_nonempty_supplier_Q21 <> None.
Proof.
  unfold EmbedDB_Q21, db_nonempty_supplier_Q21, supplier_row_example,
         embed_rows, EmbedRow_supplier, db_lookup.
  simpl.
  destruct (int32_checked_nonneg_small 1 ltac:(lia)) as [sk32 Hsk].
  destruct (int32_checked_nonneg_small 0 ltac:(lia)) as [nk32 Hnk].
  rewrite Hsk, Hnk.
  discriminate.
Qed.

Lemma Preconditions_Q21_nonempty_supplier :
  NationNameOk catalog_nation_name ->
  Char25Canonical "Supplier#000000001" ->
  Preconditions_Q21 db_nonempty_supplier_Q21 catalog_nation_name.
Proof.
  intros Hn Hcan.
  unfold Preconditions_Q21, DB_CharCanonical_Q21,
    db_nonempty_supplier_Q21, db_lookup, supplier_row_example.
  simpl.
  split; [|split; [|split]].
  - apply DB_ok_Q21_nonempty_supplier.
  - exact Hn.
  - repeat split.
    + constructor; [|constructor].
      unfold row_supplier_char_ok; exact Hcan.
    + constructor.
    + constructor.
  - apply EmbedDB_Q21_nonempty_supplier_Some.
Qed.

(** * First nonempty semantic leaf: supplier embed ↔ SlotMap row_correspond *)

Definition slot_map_supplier : SlotMap :=
  [mkSlotBinding (Attr_int32 "s_suppkey") (attr "s_suppkey" TInt);
   mkSlotBinding (Attr_string "s_name" (StringChar 25)) (attr "s_name" TText);
   mkSlotBinding (Attr_int32 "s_nationkey") (attr "s_nationkey" TInt)].

Lemma supplier_attr_inS_suppkey :
  Attr_int32 "s_suppkey" inS Fset.mk_set (A TNull) supplier_formal_attrs.
Proof.
  change ((Attr_int32 "s_suppkey" inS?
             Fset.mk_set (A TNull) supplier_formal_attrs) = true).
  vm_compute; reflexivity.
Qed.

Lemma supplier_attr_inS_s_name :
  Attr_string "s_name" (StringChar 25)
    inS Fset.mk_set (A TNull) supplier_formal_attrs.
Proof.
  change ((Attr_string "s_name" (StringChar 25) inS?
             Fset.mk_set (A TNull) supplier_formal_attrs) = true).
  vm_compute; reflexivity.
Qed.

Lemma supplier_attr_inS_nationkey :
  Attr_int32 "s_nationkey" inS Fset.mk_set (A TNull) supplier_formal_attrs.
Proof.
  change ((Attr_int32 "s_nationkey" inS?
             Fset.mk_set (A TNull) supplier_formal_attrs) = true).
  vm_compute; reflexivity.
Qed.

Lemma slot_map_supplier_wf : SlotMap_wf slot_map_supplier.
Proof.
  unfold SlotMap_wf, slot_map_supplier, slot_binding_wf,
    slot_formal_name, slot_rel_name, slot_rel_type, slot_map_names.
  repeat split.
  - repeat constructor; simpl; intuition discriminate.
  - repeat constructor; simpl; intuition discriminate.
  - repeat constructor; simpl; try reflexivity.
Qed.

(** Local int32 invertibility (avoid pulling NumericDerivedFacts). *)
Lemma int32_checked_result_value_local :
  forall z r, int32_checked z = Some r -> int32_value r = z.
Proof.
  intros z r H.
  unfold int32_checked in H.
  destruct (Z.leb_spec0 int32_min z) as [Hlo|]; [|discriminate].
  destruct (Z.leb_spec0 z int32_max) as [Hhi|]; [|discriminate].
  injection H as <-; reflexivity.
Qed.

Lemma EmbedRow_supplier_row_correspond :
  forall sk sn nk ft,
    Char25Canonical sn ->
    EmbedRow_supplier [VInt sk; VText sn; VInt nk] = Some ft ->
    row_correspond slot_map_supplier ft [VInt sk; VText sn; VInt nk].
Proof.
  intros sk sn nk ft Hcan Hemb.
  unfold EmbedRow_supplier in Hemb.
  destruct (int32_checked sk) as [sk32|] eqn:Hsk; try discriminate.
  destruct (int32_checked nk) as [nk32|] eqn:Hnk; try discriminate.
  injection Hemb as Hft; subst ft.
  unfold row_correspond, slot_map_supplier; cbn [List.length nth_error].
  split; [reflexivity|].
  intros i sb v Hi Hv.
  destruct i as [|i]; cbn [nth_error] in Hi.
  - (* s_suppkey *)
    injection Hi as <-; cbn [nth_error] in Hv; injection Hv as <-.
    unfold slot_rel_type, sb_formal; simpl attr_type.
    match goal with
    | |- value_correspond _
          (dot TNull (mk_tuple_lists ?atts ?vals) ?att) _ =>
        rewrite (mk_tuple_lists_dot atts vals att supplier_attr_inS_suppkey)
    end.
    simpl mk_att.
    pose proof (int32_checked_result_value_local _ _ Hsk) as Hskv.
    rewrite <- Hskv.
    apply VC_int32.
  - destruct i as [|i]; cbn [nth_error] in Hi.
    + (* s_name *)
      injection Hi as <-; cbn [nth_error] in Hv; injection Hv as <-.
      unfold slot_rel_type, sb_formal; simpl attr_type.
      match goal with
      | |- value_correspond _
            (dot TNull (mk_tuple_lists ?atts ?vals) ?att) _ =>
          rewrite (mk_tuple_lists_dot atts vals att supplier_attr_inS_s_name)
      end.
      simpl mk_att.
      (* goal has unfolded cast; Char25Canonical rewrites the outer form *)
      change (string_rtrim_spaces (string_take 25 sn))
        with (string_explicit_cast (StringChar 25) sn).
      unfold Char25Canonical in Hcan; rewrite Hcan.
      apply VC_text.
    + destruct i as [|i]; cbn [nth_error] in Hi.
      * (* s_nationkey *)
        injection Hi as <-; cbn [nth_error] in Hv; injection Hv as <-.
        unfold slot_rel_type, sb_formal; simpl attr_type.
        match goal with
        | |- value_correspond _
              (dot TNull (mk_tuple_lists ?atts ?vals) ?att) _ =>
            rewrite (mk_tuple_lists_dot atts vals att
                       supplier_attr_inS_nationkey)
        end.
        simpl mk_att.
        pose proof (int32_checked_result_value_local _ _ Hnk) as Hnkv.
        rewrite <- Hnkv.
        apply VC_int32.
      * destruct i; discriminate Hi.
Qed.

Lemma supplier_example_char25_canonical :
  Char25Canonical "Supplier#000000001".
Proof. vm_compute; reflexivity. Qed.

Lemma EmbedRow_supplier_example_correspond :
  exists ft,
    EmbedRow_supplier supplier_row_example = Some ft /\
    row_correspond slot_map_supplier ft supplier_row_example.
Proof.
  unfold supplier_row_example.
  destruct (EmbedRow_supplier_example_Some) as [ft0 Hft0].
  unfold supplier_row_example in Hft0.
  rewrite Hft0.
  exists ft0; split; [reflexivity|].
  apply EmbedRow_supplier_row_correspond with (sk := 1) (nk := 0).
  - apply supplier_example_char25_canonical.
  - exact Hft0.
Qed.
