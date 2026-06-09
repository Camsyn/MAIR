#!/bin/bash
export PROG_DIR=$(cd "$(dirname "$0")"; pwd)

# arpd ARP helper daemon options
# Options available according to arpd documentation:
# -l: dump the database and exit
# -f FILE: read the ARP database from a file
# -b DATABASE: database file path
# -a NUMBER: active query count
# -k: suppress kernel broadcast queries
# -n TIME: negative cache timeout
# -p TIME: polling interval
# -R RATE: broadcast rate
# -B NUMBER: consecutive broadcast count
# -h: help text
# -V: version text

export PRE_OPT="-V"
export POST_OPT=""
export stdin="false"

$PROG_DIR/../../../run.sh $@
