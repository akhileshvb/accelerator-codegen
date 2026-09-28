(* Morphisms — reusable correspondence patterns for relational operators.
   USE WHEN / REQUIRES / PROVES comments are for LLM verification agents.

   These are *pattern statements*. Concrete instances live in query modules
   (e.g. Q1) or in FormalSQLLib facades. Do not add query-specific column
   names here. *)

From Stdlib Require Import List.
From FormalSQLLogical Require Import SqlTypes Schema Expr RelNode Eval.

(** * FilterMorphism
    USE WHEN: proving two filtered bags correspond under a row relation.
    REQUIRES: forall r r', Rel r r' -> (eval_pred P p (EvRow r) ↔ eval_pred P' p' (EvRow r')).
    PROVES:   RelBags (filter P bag) (filter P' bag'). *)

Definition RowRel : Type := Row -> Row -> Prop.
Definition BagRel (R : RowRel) (a b : list Row) : Prop :=
  (* placeholder bag correspondence — instantiate with permutation/multiset *)
  True.

(** * ProjectMorphism
    USE WHEN: projected bags correspond given expression correspondence.
    REQUIRES: expr-wise value correspondence under Rel.
    PROVES:   BagRel on project_rows results. *)

(** * JoinMorphism
    USE WHEN: join results correspond under left/right row maps + cond correspondence.
    REQUIRES: RelL / RelR + predicate correspondence on concatenated rows.
    PROVES:   BagRel (join_inner …) (join_inner …) with multiplicities. *)

(** * Bag / permutation helpers (pattern)
    USE WHEN: two operator results agree as multisets but not positionally
              (e.g. relating a list-store first-seen emission to another
              emission order before a total OrderKeys sort).
    PROVES:   multiset equality / IsPermutation scaffolding.
    NOTE: Do NOT use permutation to paper over Rocq↔Dafny GroupStore
          representation divergence — both must use list association stores.
          Physical hash maps belong in Twin/accelerator code only. *)

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

(** See PROOF_LIBRARY_INDEX.md for file pointers and Q1 instantiation examples. *)
