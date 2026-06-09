#!/bin/bash
set -e
export PROG_DIR=$(cd "$(dirname "$0")"; pwd)
export PRE_OPT=""
export POST_OPT=""
export stdin="false"

# Use a temporary directory to store outputs and minimal configuration
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

# If no strongSwan config is provided, write a minimal placeholder config to avoid startup errors
if [ -z "${STRONGSWAN_CONF:-}" ]; then
  cat > "$TMP_DIR/strongswan.conf" <<'EOF'
charon {
  # minimal placeholder config to allow startup
}
EOF
  export STRONGSWAN_CONF="$TMP_DIR/strongswan.conf"
fi

# Invoke the upper-level runner and redirect all output to a temporary log file
"$PROG_DIR"/../../../run.sh "$@" > "$TMP_DIR/output.log" 2>&1

# Print the log contents to the terminal
cat "$TMP_DIR/output.log"
