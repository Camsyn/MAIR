#!/bin/bash
export PROG_DIR=$(cd "$(dirname "$0")"; pwd)
export PRE_OPT=""
export POST_OPT=""
export stdin="false"

MODE="${1:-raw}"
shift 1 || true

# Put subcommand arguments in PRE_OPT so the outer run.sh does not treat them as input files
if [ $# -eq 0 ]; then
	PRE_OPT="statistic show"
else
	PRE_OPT="$*"

fi

export MSAN_OPTIONS="poison_in_free=0:poison_in_malloc=0:halt_on_error=0:exitcode=0:abort_on_error=0:print_stats=0:print_umrs=0:report_umrs=0"


echo "Running: $PROG_DIR/../../../run.sh $MODE (PRE_OPT=\"$PRE_OPT\")"
exec "$PROG_DIR/../../../run.sh" "$MODE"