# #!/bin/bash
URL="https://github.com/nlohmann/json.git"


# Use depth 1 to avoid cloning the history
git clone "$URL" repo --depth 1


# Fetch test data
TEST_DATA_VERSION=3.1.0
wget https://github.com/nlohmann/json_test_data/archive/refs/tags/v$TEST_DATA_VERSION.zip
unzip v$TEST_DATA_VERSION.zip
rm v$TEST_DATA_VERSION.zip
for FORMAT in json bjdata bson cbor msgpack ubjson
do
  corpus_dir="corpus/json@parse_${FORMAT}_fuzzer"
  rm -fr $corpus_dir
  mkdir -p $corpus_dir
  find json_test_data-$TEST_DATA_VERSION -size -5k -name "*.$FORMAT" -exec cp "{}" "$corpus_dir" \;
done
rm -fr json_test_data-$TEST_DATA_VERSION

# The primary fuzzer that takes standard JSON as input is parse_afl_fuzzer.
rm -rf corpus/json@parse_afl_fuzzer
mv corpus/json@parse_json_fuzzer corpus/json@parse_afl_fuzzer

# For C++ programs, libc++ is not instrumented, so MSan may report false positives; filter inputs that trigger them.
FP_JSON_SEEDS=(
  # n_*
  "n_structure_*" "n_string_*" "n_object_*" "n_incomplete_*" "n_number_*" "n_array_*"
  "fail*"
  # i_*
  "i_number*" "i_string_*" "i_object*"
  # y_*
  "y_number_*" "y_string_utf16.json" "n_single_space.json"
)
for seed in ${FP_JSON_SEEDS[@]}; do
    rm -f corpus/json@parse_afl_fuzzer/$seed
done

# For C++ programs, libc++ is not instrumented, so MSan may report false positives; filter inputs that trigger them.
FP_BSON_SEEDS=(
  "1.json.bson" "2.json.bson" "4.json.bson" "5.json.bson"
)
for seed in ${FP_BSON_SEEDS[@]}; do
    rm -f corpus/json@parse_bson_fuzzer/$seed
done

