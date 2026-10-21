#!/usr/bin/env python3
"""Puts the stock market mod, with the wallet system, into an OpenTTD source tree. Safe to run more than once.

Usage:  python3 apply_overlay.py /path/to/openttd

It looks for the mod files by name anywhere next to this script, so it does not matter in which folder of the repository they are.
  - copies the C++ files into src/
  - adds the commands to the Commands enum in src/command_type.h
  - adds the new source files to src/CMakeLists.txt
  - adds the strings to src/lang/english.txt
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

FILES = {  # file name -> folder in the OpenTTD tree
    "stock_base.h": "src", "stock_base.cpp": "src",
    "stock_cmd.h": "src", "stock_cmd.cpp": "src",
    "stock_gui.h": "src", "stock_gui.cpp": "src", "stock_order_gui.cpp": "src",
    "wallet_base.h": "src", "wallet_base.cpp": "src",
    "wallet_cmd.h": "src", "wallet_cmd.cpp": "src",
    "central_bank.h": "src", "central_bank.cpp": "src",
    "game_time_manager.h": "src/game_time", "game_time_manager.cpp": "src/game_time", "game_time_driver.cpp": "src/game_time",
    "stock_sl.cpp": "src/saveload",
    "stock_widget.h": "src/widgets",
}
NEW_COMMANDS = [  # in the order they are added after CMD_STOCK_CLOSE
    ("CMD_STOCK_MODIFY", "change a position or a pending order"),
    ("CMD_STOCK_TRANSFER", "move money between the company cash and the first trading wallet"),
    ("CMD_STOCK_ENABLE_DAILY", "start recording daily candles"),
    ("CMD_WALLET_CREATE", "create a trading wallet"),
    ("CMD_WALLET_DELETE", "delete a trading wallet"),
    ("CMD_WALLET_TRANSFER", "move money between two wallets"),
    ("CMD_BANK_BORROW", "borrow from the central bank"),
    ("CMD_BANK_REPAY", "repay the central bank"),
]
NEW_SOURCES = ["wallet_base.cpp", "wallet_base.h", "wallet_cmd.cpp", "wallet_cmd.h", "central_bank.cpp", "central_bank.h",
               "game_time/game_time_manager.h", "game_time/game_time_manager.cpp", "game_time/game_time_driver.cpp"]
REPLACE_KEYS = {"STR_STOCK_TIP_TOOLBOX", "STR_STOCK_TIP_SUMMARY", "STR_STOCK_ORDER_INFO", "STR_STOCK_TIP_DEPOSIT", "STR_STOCK_TIP_WITHDRAW"}
STRING_FILES = ["english_stock_strings.txt", "english_wallet_strings.txt", "english_round8_strings.txt"]
ANCHORS = ["STR_STOCK_ERROR_NO_ORDER", "STR_STOCK_NEWS_ORDER_EXPIRED"]


def find(name):
    """The file with this name (not case sensitive) anywhere next to this script."""
    import os
    hits = []
    for root, dirs, files in os.walk(HERE):
        dirs[:] = [d for d in dirs if d != ".git"]
        for f in files:
            if f.lower() == name.lower():
                hits.append(Path(root) / f)
    if not hits:
        stem = name.split(".")[0].lower()
        similar = [str(Path(r, f).relative_to(HERE)) for r, d, fs in os.walk(HERE) if ".git" not in Path(r).parts
                   for f in fs if stem[:8] in f.lower()]
        sys.exit(f"ERROR: {name} not found next to this script. Files with a similar name: {similar or 'none'}")
    # A copy inside a src/ folder wins over an old copy lying elsewhere; then the shortest path wins.
    return sorted(hits, key=lambda p: (0 if "src" in p.relative_to(HERE).parts else 1, len(p.parts)))[0]


def main(tree):
    for name, folder in FILES.items():
        src = find(name)
        dst = tree / folder / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(src.read_bytes())
        print(f"copy {src.relative_to(HERE)} -> {folder}/{name}")

    # command_type.h
    p = tree / "src" / "command_type.h"
    s = p.read_text(encoding="utf-8")
    m = re.search(r"^([ \t]*)CMD_STOCK_CLOSE,[^\n]*\n", s, re.M)
    if not m:
        sys.exit("ERROR: CMD_STOCK_CLOSE not found in command_type.h - was stock-market.patch applied?")
    indent, pos, added = m.group(1), m.end(), 0
    for name, text in NEW_COMMANDS:
        if re.search(r"^\s*" + name + r",", s, re.M):
            continue
        line = f"{indent}{name},".ljust(40) + f"///< {text}\n"
        s = s[:pos] + line + s[pos:]
        pos += len(line)
        added += 1
    p.write_text(s, encoding="utf-8")
    print(f"command_type.h: {added} commands added")

    # CMakeLists.txt
    p = tree / "src" / "CMakeLists.txt"
    s = p.read_text(encoding="utf-8")
    m = re.search(r"^([ \t]*)stock_gui\.h\n", s, re.M)
    if not m:
        sys.exit("ERROR: stock_gui.h not found in src/CMakeLists.txt - was stock-market.patch applied?")
    missing = [f for f in NEW_SOURCES if not re.search(r"^\s*" + re.escape(f) + r"\s*$", s, re.M)]
    if missing:
        s = s[:m.end()] + "".join(f"{m.group(1)}{f}\n" for f in missing) + s[m.end():]
        p.write_text(s, encoding="utf-8")
    print(f"CMakeLists.txt: {len(missing)} files added")

    # economy.cpp: switch off the interest of the original OpenTTD loan (GAME_TIME_BYPASS_LEGACY_LOANS).
    p = tree / "src" / "economy.cpp"
    s = p.read_text(encoding="utf-8")
    if "IsLegacyLoanBypassed" in s:
        print("economy.cpp: already changed")
    else:
        old = re.search(r"([ \t]*)Money yearly_fee = c->current_loan \* _economy\.interest_rate / 100;\n([ \t]*)Money available_money = GetAvailableMoney\(c->index\);\n([ \t]*)if \(available_money < 0\) \{", s)
        inc = re.search(r'^#include "company_func\.h"\n', s, re.M)
        if old and inc:
            i1, i2, i3 = old.group(1), old.group(2), old.group(3)
            new = (f"{i1}Money yearly_fee = GameTimeManager::IsLegacyLoanBypassed() ? Money(0) : Money(c->current_loan * _economy.interest_rate / 100);\n"
                   f"{i2}Money available_money = GetAvailableMoney(c->index);\n"
                   f"{i3}if (available_money < 0 && !GameTimeManager::IsLegacyLoanBypassed()) {{")
            s = s[:old.start()] + new + s[old.end():]
            s = re.sub(r'^(#include "company_func\.h"\n)', r'\1#include "game_time/game_time_manager.h"\n', s, count=1, flags=re.M)
            p.write_text(s, encoding="utf-8")
            print("economy.cpp: interest of the original loan switched off")
        else:
            print("WARNING: economy.cpp has a different layout, the original loan interest was not changed")

    # english.txt
    wanted = {}
    for name in STRING_FILES:
        for line in find(name).read_text(encoding="utf-8").replace("\r", "").splitlines():
            if line.startswith("STR_"):
                wanted[line.split()[0].split(":")[0]] = line  # a later file wins
    p = tree / "src" / "lang" / "english.txt"
    s = p.read_text(encoding="utf-8")
    for key in REPLACE_KEYS & wanted.keys():
        s = re.sub(r"^" + key + r"[ \t]+:[^\n]*\n", "", s, flags=re.M)
    todo = [line for key, line in wanted.items() if not re.search(r"^" + key + r"[ \t]+:", s, re.M)]
    anchor = None
    for a in ANCHORS:
        anchor = re.search(r"^" + a + r"[ \t]+:[^\n]*\n", s, re.M)
        if anchor:
            break
    if not anchor:
        sys.exit("ERROR: no anchor string found in english.txt - was stock-market.patch applied?")
    s = s[:anchor.end()] + "\n".join(todo) + ("\n" if todo else "") + s[anchor.end():]
    p.write_text(s, encoding="utf-8")
    print(f"english.txt: {len(todo)} strings added")
    print("overlay applied")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    main(Path(sys.argv[1]).resolve())

