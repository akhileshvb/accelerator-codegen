#!/usr/bin/env bash
# Lower exact Cat(Q) via Logos. Failure ⇒ query is not FormalSQL-verified; stop.
# No hand-authored FormalSQL query terms.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOGOS="${LOGOS_ROOT:-$ROOT/third_party/Logos}"
QUERY="${1:?usage: $0 q1|q12|q14|q21}"

die() { echo "logos_lower FAIL ($QUERY): $*" >&2; exit 1; }

case "$QUERY" in
  q1) OUT_DIR="$ROOT/tests/verification/formalsql/q1"; QID=tpch_q1 ;;
  q12) OUT_DIR="$ROOT/tests/verification/formalsql/q12"; QID=tpch_q12 ;;
  q14) OUT_DIR="$ROOT/tests/verification/formalsql/q14"; QID=tpch_q14 ;;
  q21) OUT_DIR="$ROOT/tests/verification/formalsql/q21"; QID=tpch_q21 ;;
  *) die "unknown query" ;;
esac

[[ -d "$LOGOS" ]] || die "missing Logos pin at $LOGOS"

python3 - "$ROOT" "$QID" "$OUT_DIR" <<'PY'
import hashlib, re, sys
from pathlib import Path
root, qid, out = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
text = (root / "src/generated/catalog/postgres/query_catalog.cpp").read_text()
pat = rf'"{re.escape(qid)}",\s*/\* base_sql \*/\s*((?:"(?:\\.|[^"\\])*\"\s*)+),'
m = re.search(pat, text)
if not m:
    print(f"cannot find catalog entry {qid}", file=sys.stderr)
    sys.exit(1)
parts = re.findall(r'"((?:\\.|[^"\\])*)"', m.group(1))
sql = "".join(bytes(p, "utf-8").decode("unicode_escape") for p in parts).strip() + "\n"
out.mkdir(parents=True, exist_ok=True)
(out / "catalog_base_sql.sql").write_text(sql)
digest = hashlib.sha256(sql.encode()).hexdigest()
(out / "catalog_sql.sha256").write_text(digest + "\n")
print(f"wrote {out}/catalog_base_sql.sql sha256={digest}")
PY

SOURCE="$OUT_DIR/catalog_base_sql.sql"
SCHEMA="$OUT_DIR/schema.sql"
ARTIFACT="$OUT_DIR/logos_lower_report.json"

if [[ "$QUERY" == "q14" ]]; then
  # Q14 joins lineitem to part. DECIMAL(15,2) avoids unconstrained NUMERIC.
  cat > "$SCHEMA" <<'EOF'
CREATE TABLE part (
  p_partkey INTEGER NOT NULL,
  p_name VARCHAR(55) NOT NULL,
  p_mfgr CHAR(25) NOT NULL,
  p_brand CHAR(10) NOT NULL,
  p_type VARCHAR(25) NOT NULL,
  p_size INTEGER NOT NULL,
  p_container CHAR(10) NOT NULL,
  p_retailprice DECIMAL(15,2) NOT NULL,
  p_comment VARCHAR(23) NOT NULL
);
CREATE TABLE lineitem (
  l_orderkey BIGINT NOT NULL,
  l_partkey BIGINT NOT NULL,
  l_suppkey BIGINT NOT NULL,
  l_linenumber INTEGER NOT NULL,
  l_quantity DECIMAL(15,2) NOT NULL,
  l_extendedprice DECIMAL(15,2) NOT NULL,
  l_discount DECIMAL(15,2) NOT NULL,
  l_tax DECIMAL(15,2) NOT NULL,
  l_returnflag CHAR(1) NOT NULL,
  l_linestatus CHAR(1) NOT NULL,
  l_shipdate DATE NOT NULL,
  l_commitdate DATE NOT NULL,
  l_receiptdate DATE NOT NULL,
  l_shipinstruct CHAR(25) NOT NULL,
  l_shipmode CHAR(10) NOT NULL,
  l_comment VARCHAR(44) NOT NULL
);
EOF
elif [[ "$QUERY" == "q1" ]]; then
  # FormalSQL rejects unconstrained NUMERIC (inf/NaN/display-scale carriers).
  # Use TPC-H DECIMAL(p,s) so Logos can emit query terms.
  cat > "$SCHEMA" <<'EOF'
CREATE TABLE lineitem (
  l_orderkey BIGINT NOT NULL,
  l_partkey BIGINT NOT NULL,
  l_suppkey BIGINT NOT NULL,
  l_linenumber INTEGER NOT NULL,
  l_quantity DECIMAL(15,2) NOT NULL,
  l_extendedprice DECIMAL(15,2) NOT NULL,
  l_discount DECIMAL(15,2) NOT NULL,
  l_tax DECIMAL(15,2) NOT NULL,
  l_returnflag CHAR(1) NOT NULL,
  l_linestatus CHAR(1) NOT NULL,
  l_shipdate DATE NOT NULL,
  l_commitdate DATE NOT NULL,
  l_receiptdate DATE NOT NULL,
  l_shipinstruct CHAR(25) NOT NULL,
  l_shipmode CHAR(10) NOT NULL,
  l_comment VARCHAR(44) NOT NULL
);
EOF
else
  cat > "$SCHEMA" <<'EOF'
CREATE TABLE nation (
  n_nationkey INTEGER NOT NULL,
  n_name CHAR(25) NOT NULL,
  n_regionkey INTEGER NOT NULL,
  n_comment VARCHAR(152) NOT NULL
);
CREATE TABLE supplier (
  s_suppkey INTEGER NOT NULL,
  s_name CHAR(25) NOT NULL,
  s_address VARCHAR(40) NOT NULL,
  s_nationkey INTEGER NOT NULL,
  s_phone CHAR(15) NOT NULL,
  s_acctbal DECIMAL(15,2) NOT NULL,
  s_comment VARCHAR(101) NOT NULL
);
CREATE TABLE orders (
  o_orderkey INTEGER NOT NULL,
  o_custkey INTEGER NOT NULL,
  o_orderstatus CHAR(1) NOT NULL,
  o_totalprice DECIMAL(15,2) NOT NULL,
  o_orderdate DATE NOT NULL,
  o_orderpriority CHAR(15) NOT NULL,
  o_clerk CHAR(15) NOT NULL,
  o_shippriority INTEGER NOT NULL,
  o_comment VARCHAR(79) NOT NULL
);
CREATE TABLE lineitem (
  l_orderkey INTEGER NOT NULL,
  l_partkey INTEGER NOT NULL,
  l_suppkey INTEGER NOT NULL,
  l_linenumber INTEGER NOT NULL,
  l_quantity DECIMAL(15,2) NOT NULL,
  l_extendedprice DECIMAL(15,2) NOT NULL,
  l_discount DECIMAL(15,2) NOT NULL,
  l_tax DECIMAL(15,2) NOT NULL,
  l_returnflag CHAR(1) NOT NULL,
  l_linestatus CHAR(1) NOT NULL,
  l_shipdate DATE NOT NULL,
  l_commitdate DATE NOT NULL,
  l_receiptdate DATE NOT NULL,
  l_shipinstruct CHAR(25) NOT NULL,
  l_shipmode CHAR(10) NOT NULL,
  l_comment VARCHAR(44) NOT NULL
);
EOF
fi

SOLVER=""
for c in \
  "$LOGOS/bin/logos-solver" \
  "$LOGOS/target/release/logos-solver" \
  "$LOGOS/target/debug/logos-solver" \
  "${CARGO_TARGET_DIR:-}/release/logos-solver"; do
  [[ -n "$c" && -x "$c" ]] && SOLVER="$c" && break
done
[[ -n "$SOLVER" ]] || die "logos-solver not built (cd third_party/Logos && cargo build -p logos-solver; copy to bin/). Cat($QUERY) not FormalSQL-verified."

export LOGOS_CALCITE_IR_COMMAND="${LOGOS_CALCITE_IR_COMMAND:-$LOGOS/scripts/calcite-ir-sqlglot}"
[[ -x "$LOGOS_CALCITE_IR_COMMAND" ]] || die "Calcite IR frontend missing at $LOGOS_CALCITE_IR_COMMAND (build Logos frontend)"

# calcite-ir-sqlglot still invokes Java Calcite for IR emission.
if [[ -z "${JAVA_HOME:-}" && -z "${LOGOS_JAVA_HOME:-}" ]]; then
  for _jh in \
    /usr/lib/jvm/java-21-openjdk \
    /usr/lib/jvm/java-17-openjdk \
    /usr/lib/jvm/default \
    /usr/lib/jvm/default-runtime; do
    if [[ -x "$_jh/bin/java" ]]; then
      export JAVA_HOME="$_jh"
      break
    fi
  done
fi
[[ -n "${JAVA_HOME:-}${LOGOS_JAVA_HOME:-}" ]] \
  || die "JAVA_HOME (or LOGOS_JAVA_HOME) required for Logos Calcite IR; e.g. export JAVA_HOME=/usr/lib/jvm/java-21-openjdk"

# logos-solver clears HOME; prefer the prebuilt bound classpath when present.
# Classpath files may be baked on another machine (e.g. /accel/...); rewrite to
# this checkout when jars are missing at the recorded paths.
if [[ -z "${LOGOS_CALCITE_RUNTIME_CLASSPATH_FILE:-}" ]]; then
  _cp="$LOGOS/frontend/calcite-wrapper/target/logos-runtime-classpath.txt"
  [[ -f "$_cp" ]] && export LOGOS_CALCITE_RUNTIME_CLASSPATH_FILE="$_cp"
fi
if [[ -n "${LOGOS_CALCITE_RUNTIME_CLASSPATH_FILE:-}" && -f "$LOGOS_CALCITE_RUNTIME_CLASSPATH_FILE" ]]; then
  _fixed="$OUT_DIR/.logos-runtime-classpath.txt"
  python3 - "$LOGOS_CALCITE_RUNTIME_CLASSPATH_FILE" "$_fixed" "$ROOT" <<'PY' || die "failed to localize Calcite classpath"
import sys
from pathlib import Path
src, dst, root = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
text = src.read_text().strip()
parts = [p for p in text.split(":") if p]
if parts and not Path(parts[0]).exists():
    # Common Docker/CI bake prefix → this checkout.
    text = text.replace("/accel/", str(root) + "/")
    parts = [p for p in text.split(":") if p]
missing = [p for p in parts if not Path(p).exists()]
if missing:
    sys.stderr.write(
        "Calcite classpath jars missing (rebuild Logos calcite-wrapper):\n  "
        + "\n  ".join(missing[:5])
        + ("\n  ..." if len(missing) > 5 else "")
        + "\n"
    )
    sys.exit(1)
dst.write_text(text + "\n")
print(f"localized Calcite classpath → {dst} ({len(parts)} jars)")
PY
  export LOGOS_CALCITE_RUNTIME_CLASSPATH_FILE="$_fixed"
fi

"$SOLVER" check \
  --schema "$SCHEMA" \
  --source "$SOURCE" \
  --target "$SOURCE" \
  --transform-only \
  --calcite-ir-command "$LOGOS_CALCITE_IR_COMMAND" \
  --sql-default-collation C \
  --sql-character-classification C \
  --sql-locale-provider libc \
  --sql-server-encoding UTF8 \
  --log-dir "$OUT_DIR/logos_log" \
  > "$ARTIFACT" 2>"$OUT_DIR/logos_lower.err" \
  || die "Logos cannot lower exact Cat($QUERY) — not FormalSQL-verified. See $OUT_DIR/logos_lower.err"

# Persist Logos-generated Rocq terms for JetStream FormalSQL proofs (CI-friendly).
GEN_DIR="$OUT_DIR/generated"
FORMAL_SQL_DIR="$OUT_DIR/logos_log/proof-stage/formal-sql"
if [[ -d "$FORMAL_SQL_DIR" ]]; then
  mkdir -p "$GEN_DIR"
  for f in Schema.v Queries.v Witness.v WitnessData.v; do
    [[ -f "$FORMAL_SQL_DIR/$f" ]] && cp -f "$FORMAL_SQL_DIR/$f" "$GEN_DIR/$f"
  done
  if [[ -f "$OUT_DIR/catalog_sql.sha256" ]]; then
    DIGEST="$(tr -d '[:space:]' < "$OUT_DIR/catalog_sql.sha256")"
    case "$QUERY" in
      q1)
        cat > "$OUT_DIR/Q1Catalog.v" <<EOF
(* Q1 catalog pin — filled only by Logos lower of exact Cat(Q1).
   catalog_sql_sha256 must match query_catalog.cpp tpch_q1 base_sql. *)

From Stdlib Require Import String.
Open Scope string_scope.

(** SHA-256 of tests/verification/formalsql/q1/catalog_base_sql.sql *)
Definition catalog_sql_sha256 : string :=
  "$DIGEST".

(** Logos-lowered FormalSQL artifacts live in formalsql/q1/generated/
   (Schema.v / Queries.v / Witness.v), produced by scripts/logos_lower_catalog.sh. *)
EOF
        ;;
      q21)
        cat > "$OUT_DIR/Q21Catalog.v" <<EOF
(* Q21 catalog pin — filled only by Logos lower of exact Cat(Q21).
   catalog_sql_sha256 must match query_catalog.cpp tpch_q21 base_sql. *)

From Stdlib Require Import String.
Open Scope string_scope.

(** SHA-256 of tests/verification/formalsql/q21/catalog_base_sql.sql *)
Definition catalog_sql_sha256 : string :=
  "$DIGEST".

(** Logos-lowered FormalSQL artifacts live in formalsql/q21/generated/
   (Schema.v / Queries.v / Witness.v), produced by scripts/logos_lower_catalog.sh. *)
EOF
        ;;
      q12)
        cat > "$OUT_DIR/Q12Catalog.v" <<EOF
(* Q12 catalog pin — filled only by Logos lower of exact Cat(Q12).
   catalog_sql_sha256 must match query_catalog.cpp tpch_q12 base_sql. *)

From Stdlib Require Import String.
Open Scope string_scope.

(** SHA-256 of tests/verification/formalsql/q12/catalog_base_sql.sql *)
Definition catalog_sql_sha256 : string :=
  "$DIGEST".

(** Logos-lowered FormalSQL artifacts live in formalsql/q12/generated/
   (Schema.v / Queries.v / Witness.v), produced by scripts/logos_lower_catalog.sh. *)
EOF
        ;;
    esac
  fi
fi

echo "logos_lower OK ($QUERY) → $ARTIFACT"
