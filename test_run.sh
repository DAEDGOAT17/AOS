#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_BIN="${PYTHON_BIN:-python3}"
MODEL="${HARVIS_TEST_MODEL:-gemma3:1b}"
GENERATE_URL="${HARVIS_TEST_ENDPOINT:-http://127.0.0.1:11434/api/generate}"
TAGS_URL="${GENERATE_URL%/api/generate}/api/tags"
LIVE_AI=0
SETUP_AI=0

usage() {
    cat <<'EOF'
Usage: ./test_run.sh [--live-ai | --setup-ai]

Run the Python agentic test suite in an isolated temporary directory.
  --live-ai   Also send a real request to an installed local Ollama model.
  --setup-ai  Pull the configured Ollama model if missing, then run the live check.

Environment:
  HARVIS_TEST_MODEL     Ollama model name (default: gemma3:1b)
  HARVIS_TEST_ENDPOINT  Generate API URL (default: http://127.0.0.1:11434/api/generate)
  PYTHON_BIN            Python executable (default: python3)
EOF
}

case "${1:-}" in
    "") ;;
    --live-ai) LIVE_AI=1 ;;
    --setup-ai) LIVE_AI=1; SETUP_AI=1 ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac

if [[ "$#" -gt 1 ]]; then
    usage >&2
    exit 2
fi

if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
    printf 'Missing Python executable: %s\n' "$PYTHON_BIN" >&2
    exit 1
fi

if [[ ! -d "$ROOT_DIR/tests" || ! -d "$ROOT_DIR/tools" ]]; then
    printf 'Run this script from the AOS repository.\n' >&2
    exit 1
fi

TEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/aos-agent-tests.XXXXXX")"
trap 'rm -rf "$TEST_TMP"' EXIT

if [[ "$LIVE_AI" -eq 1 ]]; then
    if ! command -v ollama >/dev/null 2>&1; then
        printf 'Ollama is required for live AI testing. Install it or run without --live-ai.\n' >&2
        exit 1
    fi

    if ! "$PYTHON_BIN" - "$TAGS_URL" <<'PY'
import sys
import urllib.request

try:
    with urllib.request.urlopen(sys.argv[1], timeout=3) as response:
        if response.status != 200:
            raise RuntimeError(f"HTTP {response.status}")
except Exception as error:
    print(f"Ollama is not reachable at {sys.argv[1]}: {error}", file=sys.stderr)
    sys.exit(1)
PY
    then
        printf 'Start the Ollama service, then retry.\n' >&2
        exit 1
    fi

    if ! "$PYTHON_BIN" - "$TAGS_URL" "$MODEL" <<'PY'
import json
import sys
import urllib.request

with urllib.request.urlopen(sys.argv[1], timeout=3) as response:
    models = json.load(response).get("models", [])
sys.exit(0 if any(model.get("name") == sys.argv[2] for model in models) else 1)
PY
    then
        if [[ "$SETUP_AI" -eq 1 ]]; then
            printf 'Pulling Ollama model %s...\n' "$MODEL"
            ollama pull "$MODEL"
        else
            printf 'Model %s is missing. Run ./test_run.sh --setup-ai to pull it.\n' "$MODEL" >&2
            exit 1
        fi
    fi
fi

printf 'Running Python agentic tests in an isolated directory...\n'
(
    cd "$TEST_TMP"
    export PYTHONDONTWRITEBYTECODE=1
    export PYTHONPATH="$ROOT_DIR/tools${PYTHONPATH:+:$PYTHONPATH}"
    "$PYTHON_BIN" -m unittest discover -s "$ROOT_DIR/tests" -p 'test_*.py' -v
)

if [[ "$LIVE_AI" -eq 1 ]]; then
    printf '\nSending a live prompt to %s using %s...\n' "$GENERATE_URL" "$MODEL"
    "$PYTHON_BIN" - "$GENERATE_URL" "$MODEL" <<'PY'
import json
import sys
import urllib.request

url, model = sys.argv[1:]
payload = {
    "model": model,
    "system": "You are a local test assistant. Reply briefly in plain text.",
    "prompt": "Confirm that the local AI test request works.",
    "stream": False,
}
request = urllib.request.Request(
    url,
    data=json.dumps(payload).encode("utf-8"),
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urllib.request.urlopen(request, timeout=120) as response:
    result = json.load(response)
answer = str(result.get("response", "")).strip()
if not answer:
    raise SystemExit("Ollama returned an empty response.")
print(f"Live AI response: {answer}")
PY
fi

printf '\nAgentic test run passed.\n'