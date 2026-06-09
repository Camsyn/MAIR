#!/bin/bash
set -e

ROOT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
CORPUS_DIR="$ROOT_DIR/corpus/lua"

mkdir -p "$CORPUS_DIR"

cat > "$CORPUS_DIR/hello.lua" << 'EOF'
print('hello world')
EOF

cat > "$CORPUS_DIR/loop.lua" << 'EOF'
local s = 0
for i=1,1e4 do s = s + i end
print(s)
EOF

cat > "$CORPUS_DIR/table.lua" << 'EOF'
local t = {a=1,b=2}
t.c = 3
for k,v in pairs(t) do print(k,v) end
EOF

cat > "$CORPUS_DIR/func.lua" << 'EOF'
local function f(x) return x*x end
print(f(7))
EOF

cat > "$CORPUS_DIR/string.lua" << 'EOF'
local s = string.gsub('a,b,c', ',', '|')
print(s)
EOF

echo "Generated corpus at: $CORPUS_DIR"




