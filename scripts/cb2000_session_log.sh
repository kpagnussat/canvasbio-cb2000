#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
STATE_FILE="${CB2000_SESSION_STATE_FILE:-$ROOT_DIR/docs/cb2000_session_state.md}"

if [[ ! -f "$STATE_FILE" ]]; then
  mkdir -p "$(dirname "$STATE_FILE")"
  cat > "$STATE_FILE" <<'HEADER'
# CB2000 Session State

Registro manual de comandos, resultados e decisões de sessão.
HEADER
fi

if [[ $# -lt 4 ]]; then
  cat >&2 <<'USAGE'
Usage:
  cb2000_session_log.sh "<comando>" "<resultado>" "<assinatura_erro>" "<decisao>"

Example:
  cb2000_session_log.sh \
    "G_MESSAGES_DEBUG=all sudo ./img-capture" \
    "fail" \
    "machine->completed" \
    "manter hipótese de corrida SSM/timer"
USAGE
  exit 1
fi

CMD="$1"
RESULT="$2"
SIGNATURE="$3"
DECISION="$4"
NOW="$(date '+%Y-%m-%d %H:%M:%S')"

cat >> "$STATE_FILE" <<ENTRY

### $NOW
- comando: \
\`$CMD\`
- resultado: $RESULT
- assinatura_erro: $SIGNATURE
- decisao: $DECISION
ENTRY

echo "Session state updated: $STATE_FILE"
