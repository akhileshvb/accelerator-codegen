#!/usr/bin/env bash
# Build/run Logos Calcite frontend in Docker (no host sudo / JDK 17 required).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOGOS="$ROOT/third_party/Logos"
IMAGE="${LOGOS_FRONTEND_IMAGE:-logos-frontend:jdk17}"
DOCKERFILE="$LOGOS/docker/frontend/Dockerfile"
M2_REPO="/accel/third_party/Logos/.cache/m2"
CLASSPATH_FILE="/accel/third_party/Logos/frontend/calcite-wrapper/target/logos-runtime-classpath.txt"
MAVEN_BIN="/accel/third_party/Logos/.cache/apache-maven-3.9.11/bin/mvn"

die() { echo "docker_logos_frontend: $*" >&2; exit 1; }

command -v docker >/dev/null || die "docker not found"
[[ -d "$LOGOS" ]] || die "missing $LOGOS"
[[ -f "$DOCKERFILE" ]] || die "missing $DOCKERFILE"

echo "==> Building $IMAGE"
docker build -t "$IMAGE" -f "$DOCKERFILE" "$LOGOS/docker/frontend"

run_in() {
  docker run --rm \
    -v "$ROOT:/accel" \
    -w /accel/third_party/Logos \
    -e JAVA_HOME=/opt/java/openjdk \
    -e LOGOS_JAVA_HOME=/opt/java/openjdk \
    -e MAVEN_OPTS="-Dorg.slf4j.simpleLogger.log.org.apache.maven.cli.transfer.Slf4jMavenTransferListener=warn" \
    "$IMAGE" \
    bash -lc "$*"
}

echo "==> SQLGlot venv"
run_in 'python3 -m venv .cache/sqlglot-venv && .cache/sqlglot-venv/bin/pip install -q "sqlglot==30.11.0"'

echo "==> Build logos-solver (release)"
run_in 'cargo build -p logos-solver --release && mkdir -p bin && cp -f target/release/logos-solver bin/logos-solver'

echo "==> Bootstrap Maven + Calcite runtime classpath (persistent .cache/m2)"
run_in "set -euo pipefail
  mkdir -p .cache/m2
  if [[ ! -x $MAVEN_BIN ]]; then
    archive=.cache/apache-maven-3.9.11-bin.tar.gz
    if [[ ! -f \$archive ]]; then
      curl -fsSL https://archive.apache.org/dist/maven/maven-3/3.9.11/binaries/apache-maven-3.9.11-bin.tar.gz -o \$archive
    fi
    tar -xzf \$archive -C .cache
  fi
  $MAVEN_BIN -q -f frontend/calcite-wrapper/pom.xml \
    -Dmaven.repo.local=$M2_REPO \
    compile dependency:build-classpath \
    -DincludeScope=runtime \
    -Dmdep.outputFile=$CLASSPATH_FILE
  test -s $CLASSPATH_FILE
  echo classpath_entries=\$(tr ':' '\\n' < $CLASSPATH_FILE | wc -l)
"

echo "==> Calcite IR smoke (make calcite-ir)"
run_in "export LOGOS_CALCITE_RUNTIME_CLASSPATH_FILE=$CLASSPATH_FILE && make calcite-ir"

echo "==> Logos lower Cat(Q1) via JetStream script"
run_in "cd /accel && \
  export LOGOS_CALCITE_IR_COMMAND=/accel/third_party/Logos/scripts/calcite-ir-sqlglot && \
  export LOGOS_CALCITE_RUNTIME_CLASSPATH_FILE=$CLASSPATH_FILE && \
  bash scripts/logos_lower_catalog.sh q1"

echo "docker_logos_frontend: OK"
echo "  image: $IMAGE"
echo "  solver: $LOGOS/bin/logos-solver"
echo "  classpath: $LOGOS/frontend/calcite-wrapper/target/logos-runtime-classpath.txt"
echo "  lower report: $ROOT/tests/verification/formalsql/q1/logos_lower_report.json"
