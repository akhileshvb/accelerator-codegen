# Logos + FormalSQL pin

JetStream FormalSQL verification pins **Logos** and the **FormalSQL revision
Logos itself vendors**. FormalSQL is never advanced independently of Logos.

| Component | Path | SHA |
|-----------|------|-----|
| Logos | [`third_party/Logos`](../../../third_party/Logos) | `333d5275ac871f5b36ce3a7dc5f6fef5f7879069` |
| FormalSQL (Logos submodule) | `third_party/Logos/vendor/FormalSQL` | `2238975a4a989c844ade5b75d9d0414295fbdfd1` |

Sources:

- Logos: https://github.com/WindOctober/Logos @ `main` commit above
- FormalSQL: https://github.com/WindOctober/FormalSQL (submodule SHA recorded in Logos `.gitmodules` checkout)

## Rocq / opam

Logos is developed with:

- Rocq 9.2
- Rocq standard library 9.1
- Flocq ≥ 4.2.2
- Zarith ≥ 1.14

Use Logos’s local switch recipe when available:

```bash
export ROCQ_OPAM_SWITCH=/path/to/opam-switch
cd third_party/Logos
make formal-sql
make smoke
```

JetStream smoke:

```bash
scripts/run_formalsql_smoke.sh
```

## Policy

- Missing pin, SHA mismatch, or Rocq compile failure ⇒ verification **red**.
- If Logos cannot lower exact `Cat(Q)` ⇒ **Q is not FormalSQL-verified; stop**.
- No hand-authored FormalSQL query terms.
- No MathSem fallback.
