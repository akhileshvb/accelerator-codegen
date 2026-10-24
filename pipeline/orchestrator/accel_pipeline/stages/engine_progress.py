"""What an engine_backend run has finished, as marker files in the run's artifacts directory: a
relaunch continues from them instead of redoing finished layers. Anything that changes a layer's code
clears the markers of the work that judged the earlier code (the audit, the validation gates)."""

from __future__ import annotations

from pathlib import Path

_AUDIT = "architecture_audit.passed"
_GATE_PREFIX = "validation_gate_"


class Progress:
    def __init__(self, art: Path) -> None:
        self.art = art

    def _marker(self, name: str) -> Path:
        return self.art / name

    def done(self, step: str) -> bool:
        return self._marker(f"{step}.passed").exists()

    def mark(self, step: str) -> None:
        self._marker(f"{step}.passed").touch()

    def layer_passed(self, layer_id: str) -> bool:
        return self._marker(f"layer_{layer_id}.passed").exists()

    def mark_layer(self, layer_id: str) -> None:
        self._marker(f"layer_{layer_id}.passed").touch()

    def reopen_layer(self, layer_id: str) -> None:
        self._marker(f"layer_{layer_id}.passed").unlink(missing_ok=True)
        self.invalidate_judgements()

    def audit_done(self) -> bool:
        return self._marker(_AUDIT).exists()

    def mark_audit(self) -> None:
        self._marker(_AUDIT).touch()

    def gate_passed(self, gate: str) -> bool:
        return self._marker(f"{_GATE_PREFIX}{gate}.passed").exists()

    def mark_gate(self, gate: str) -> None:
        self._marker(f"{_GATE_PREFIX}{gate}.passed").touch()

    def invalidate_judgements(self) -> None:
        self._marker(_AUDIT).unlink(missing_ok=True)
        self.invalidate_gates()

    def invalidate_gates(self) -> None:
        for path in self.art.glob(f"{_GATE_PREFIX}*.passed"):
            path.unlink(missing_ok=True)
