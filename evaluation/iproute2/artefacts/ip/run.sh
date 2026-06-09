#!/bin/bash
export PROG_DIR=$(cd "$(dirname "$0")"; pwd)
export PRE_OPT=""
export POST_OPT=""
export stdin="false"

MODE="${1:-raw}"
shift 1 || true

# Put subcommand arguments in PRE_OPT so the outer run.sh does not treat them as input files
if [ $# -eq 0 ]; then
	PRE_OPT="-help"
else
	PRE_OPT="$*"
fi

export MSAN_OPTIONS="poison_in_free=0:poison_in_malloc=0"


echo "Running: $PROG_DIR/../../../run.sh $MODE (PRE_OPT=\"$PRE_OPT\")"
exec "$PROG_DIR/../../../run.sh" "$MODE"

