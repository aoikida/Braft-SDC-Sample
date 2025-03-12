#!/bin/bash

# GDB を使って実行時のベースアドレスを取得
BASE_ADDR=$(gdb -q --batch -ex "start" -ex "info proc mappings" ./counter_server | grep "r-xp" | head -n 1 | awk '{print $1}')

if [[ -z "$BASE_ADDR" ]]; then
    echo "Error: Failed to retrieve base address."
    exit 1
fi

# 16進数から10進数へ変換
BASE_ADDR=$((BASE_ADDR))

# `objdump` で `get` 関数のオフセットを取得 (1行目のみ取得)
OFFSET=$(objdump -d ./counter_server | grep "_ZN7example7Counter3getEPNS_15CounterResponseE" | awk '{print $1}' | head -n 1)

# `objdump` の出力は 16進数の `:` を含まない形にする
if [[ $OFFSET == *:* ]]; then
    OFFSET=$(echo "$OFFSET" | cut -d':' -f1)
fi

# `0x` を追加して 16進数として扱う
OFFSET="0x${OFFSET}"

if [[ -z "$OFFSET" ]]; then
    echo "Error: Failed to retrieve function offset."
    exit 1
fi

# 実行時の `get` 関数のアドレスを計算
TARGET_IP=$(printf "0x%x" $((BASE_ADDR + OFFSET)))

echo "Using IP: $TARGET_IP"

# BFI のパスを確認
BFI_PATH="$HOME/bfi/obj-intel64/bfi.so"
if [[ ! -f "$BFI_PATH" ]]; then
    echo "Error: BFI tool not found at $BFI_PATH" >&2
    exit 1
fi

# `pin` を実行
setarch $(uname -m) -R ~/pin-external-3.31-98869-gfa6f126a8-gcc-linux/pin \
        -t "$BFI_PATH" -cmd WREG -ip "$TARGET_IP" -trigger 1 -ttype IT -- ./counter_server

