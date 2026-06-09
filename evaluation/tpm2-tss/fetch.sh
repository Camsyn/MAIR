#!/bin/bash
# export PROJ=tpm2-tss
URL="https://github.com/tpm2-software/tpm2-tss.git"

git clone "$URL" repo --depth 1

# Change json_object *jso_content; to json_object *jso_content = NULL;
# Rationale: this variable is initialized in third-party libjson-c.so. Since we do not instrument that library, initialize it manually to avoid MSan false positives.
# sed -i 's/json_object \*jso_content;/json_object *jso_content = NULL;/g' repo/src/tss2-fapi/ifapi_ima_eventlog.c
sed -i 's/\*jso_sub;/*jso_sub = NULL;/g' repo/src/tss2-fapi/ifapi_json_eventlog_serialize.c
sed -i 's/\*jso_digests/*jso_digests = NULL/g' repo/src/tss2-fapi/ifapi_json_eventlog_serialize.c

# Search the repository for potential corpus files
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ -f "$SCRIPT_DIR/create_tpm2tss_corpus.sh" ]; then
    "$SCRIPT_DIR/create_tpm2tss_corpus.sh" || true
else
    echo "Warning: Executable create_tpm2tss_corpus.sh not found, skipping corpus generation" >&2
fi  