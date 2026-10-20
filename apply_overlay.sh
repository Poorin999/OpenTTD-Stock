#!/bin/bash
# ใช้: bash apply_overlay.sh <โฟลเดอร์ซอร์ส OpenTTD>
set -e
D="${1:-.}"

# 1) คัดลอกไฟล์ใหม่ทับไฟล์เดิม
cp stock_base.h stock_base.cpp stock_cmd.h stock_cmd.cpp \
   stock_gui.h stock_gui.cpp stock_order_gui.cpp "$D/src/"
cp stock_sl.cpp "$D/src/saveload/"
cp stock_widget.h "$D/src/widgets/"

# 2) เพิ่ม CMD_STOCK_MODIFY ต่อจาก CMD_STOCK_CLOSE
grep -q CMD_STOCK_MODIFY "$D/src/command_type.h" || \
sed -i 's|^\(\s*\)CMD_STOCK_CLOSE,.*|&\n\1CMD_STOCK_MODIFY,                ///< change SL/TP of a position|' "$D/src/command_type.h"

# 3) แก้สตริงภาษา
grep '^STR_' english_stock_strings.txt > /tmp/new_strings.txt
sed -i '/^STR_STOCK_TIP_TOOLBOX /d;/^STR_STOCK_TIP_SUMMARY /d;/^STR_STOCK_ORDER_INFO /d' "$D/src/lang/english.txt"
sed -i '/^STR_STOCK_ERROR_NO_ORDER /r /tmp/new_strings.txt' "$D/src/lang/english.txt"