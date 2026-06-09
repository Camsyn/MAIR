#!/bin/bash
export PROG_DIR=$(cd "$(dirname "$0")"; pwd)
export PRE_OPT=""
export POST_OPT="-V"
export stdin="false"

MODE="${1:-raw}"
shift 1 || true

# If arguments are provided, run once; otherwise run common command combinations
if [ $# -gt 0 ]; then
	PRE_OPT="$*"
	export PRE_OPT
	export MSAN_OPTIONS="poison_in_free=0:poison_in_malloc=0"
	echo "Running: $PROG_DIR/../../../run.sh $MODE (PRE_OPT=\"$PRE_OPT\")"
	exec "$PROG_DIR/../../../run.sh" "$MODE"
else
	CASES=(
		# ""            # default invocation
		"-az"         # all / compressed
		"-j"          # JSON output
		"-j"          # JSON output (repeat 1)
		"-j"          # JSON output (repeat 2)
		"-j"          # JSON output (repeat 3)
		"-j"          # JSON output (repeat 4)
		"-j"          # JSON output (repeat 5)
		"-s"          # summary
		# "-r"          # reset counters (may require privileges)
		"-t 1 -c 2"   # periodic sampling twice
	)
	export MSAN_OPTIONS="poison_in_free=0:poison_in_malloc=0"
	for OPT in "${CASES[@]}"; do
		PRE_OPT="$OPT"
		export PRE_OPT
		echo "========== nstat $PRE_OPT =========="
		"$PROG_DIR/../../../run.sh" "$MODE" || true
		printf "\n"
	done
fi

