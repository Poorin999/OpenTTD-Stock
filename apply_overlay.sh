#!/bin/bash
set -e
D="${1:-.}"
HERE="$(cd "$(dirname "$0")" && pwd)"

# หาไฟล์ตามชื่อ ไม่ว่าจะอยู่โฟลเดอร์ไหนใน repo นี้
copy() {
  f="$(find "$HERE" -type f -name "$1" -not -path '*/.git/*' | head -n1)"
  if [ -z "$f" ]; then
    echo "ERROR: ไม่พบไฟล์ $1 ใน repo. ไฟล์ที่มีอยู่:"
    find "$HERE" -type f -not -path '*/.git/*' | head -50
    exit 1
  fi
  echo "copy $f -> $2"
  cp "$f" "$2"
}

copy stock_base.h        "$D/src/"
copy stock_base.cpp      "$D/src/"
copy stock_cmd.h         "$D/src/"
copy stock_cmd.cpp       "$D/src/"
copy stock_gui.h         "$D/src/"
copy stock_gui.cpp       "$D/src/"
copy stock_order_gui.cpp "$D/src/"
copy stock_sl.cpp        "$D/src/saveload/"
copy stock_widget.h      "$D/src/widgets/"

# เพิ่ม CMD_STOCK_MODIFY ต่อจาก CMD_STOCK_CLOSE
grep -q CMD_STOCK_MODIFY "$D/src/command_type.h" || \
sed -i 's|^\(\s*\)CMD_STOCK_CLOSE,.*|&\n\1CMD_STOCK_MODIFY,                ///< change SL/TP of a position|' "$D/src/command_type.h"

# แก้สตริงภาษา
S="$(find "$HERE" -type f -name english_stock_strings.txt | head -n1)"
[ -n "$S" ] || { echo "ERROR: ไม่พบ english_stock_strings.txt"; exit 1; }
tr -d '\r' < "$S" | grep '^STR_' > /tmp/new_strings.txt
sed -i '/^STR_STOCK_TIP_TOOLBOX /d;/^STR_STOCK_TIP_SUMMARY /d;/^STR_STOCK_ORDER_INFO /d' "$D/src/lang/english.txt"
sed -i '/^STR_STOCK_ERROR_NO_ORDER /r /tmp/new_strings.txt' "$D/src/lang/english.txt"
echo "overlay applied"
