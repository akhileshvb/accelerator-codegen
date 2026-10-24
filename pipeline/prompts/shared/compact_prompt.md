Your context is being compacted. Write the summary you will continue from; everything not in it is lost
except what is on disk. Be specific (paths, numbers, function names), not narrative. Sections:

1. **Task and stage.** The stage you are in, what finishes it (its exit gate or artifact), and the rules
   you were given that you keep needing (owned paths, forbidden paths, required tools).
2. **Design.** The selected state, its read path and each write path (which writes are incremental and
   which still rebuild), and the primitives that hold each state component. Point to analysis.json for
   detail instead of copying it.
3. **Files.** Every file you created or changed, one line each: what it holds and its current state
   (complete, partial, broken).
4. **Evidence.** The latest result of every check and gate you ran (pass/fail, the failing case, its
   duration), and the measured numbers: read speedup and its baseline, write and refresh cost, thread
   scaling, profile hot spots.
5. **Tried.** Changes you tried and kept or reverted, with the measured effect, so you do not repeat them.
6. **Open.** Known bugs, unhandled cases, and requests or blockers outstanding.
7. **Next.** The exact next steps, in order.
