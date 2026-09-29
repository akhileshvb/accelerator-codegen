(* Q21 strategy — Q1-style query-specific NormalizeSound (not a FormalSQL compiler).

   Architecture (mirrors q1/PROOF_FLOW.md):

     Catalog FormalSQL AST  --normalize_q21-->  LogicalSpec_Q21_rel (fixed DATA)
            |                                         |
            | FormalSQL_exec                          | Eval_Spec
            v                                         v
         EmbedResult  <---- observational equality ---- RelNode rows
                         NormalizeSound_Q21

   - Recognizer stays fail-closed and query-specific.
   - RelNode tree stays the Rocq-authoritative LogicalSpec_Q21_rel.
   - No generic Scan→Join→Semi→Anti→… stack is required for Q21.
   - Shared ValueCorrespond / SlotMap / ScanCorrespond remain available as
     reusable helpers; they are not Stage-2 prerequisites.
   - Stage 3 (Twin/Aux Inv / maintenance) stays blocked until NormalizeSound Qed.
*)
