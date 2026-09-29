# Neutral AuxSpec (shared Rocq + Dafny Spec ops)

`gen_auxspec.py` emits:

- `tests/verification/formalsql/generated/Q*AuxSpec.v`
- `tests/verification/q*/dafny/generated/Q*AuxSpec.dfy`

```bash
python3 gen_auxspec.py          # write
python3 gen_auxspec.py --check  # CI freshness
```

Rocq proves FormalSQL membership of `EmbedResult(AuxAnswerSpec)` after Embed.
Dafny proves TwinOp ≡ SpecOp, then `FoldTwin = FoldSpec`.
