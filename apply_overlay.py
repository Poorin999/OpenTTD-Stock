#!/usr/bin/env python3
"""Puts the stock market mod, with the wallet system, into an OpenTTD source tree. Safe to run more than once.

Usage:  python3 apply_overlay.py /path/to/openttd

It looks for the mod files by name anywhere next to this script, so it does not matter in which folder of the repository they are.
  - copies the C++ files into src/
  - adds the commands to the Commands enum in src/command_type.h
  - adds the new source files to src/CMakeLists.txt
  - adds the strings to src/lang/english.txt
  - economy.cpp: no interest, no overdraft charge and no CmdIncreaseLoan for the original loan
  - window_type.h / widgets: the Central Bank window class (WC_CENTRAL_BANK)
  - finances_gui.cpp, statusbar_gui.cpp: the finances menu and the statusbar button open ShowCentralBankWindow()
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
    "central_bank_gui.h": "src", "central_bank_gui.cpp": "src",
    "central_bank_widget.h": "src/widgets",
    "game_time_manager.h": "src/game_time", "game_time_manager.cpp": "src/game_time", "game_time_driver.cpp": "src/game_time",
    "game_time_aggregator.h": "src/game_time",
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
               "central_bank_gui.cpp", "central_bank_gui.h",
               "game_time/game_time_manager.h", "game_time/game_time_manager.cpp", "game_time/game_time_driver.cpp", "game_time/game_time_aggregator.h"]
REPLACE_KEYS = {"STR_STOCK_TIP_TOOLBOX", "STR_STOCK_TIP_SUMMARY", "STR_STOCK_ORDER_INFO", "STR_STOCK_TIP_DEPOSIT", "STR_STOCK_TIP_WITHDRAW"}
STRING_FILES = ["english_stock_strings.txt", "english_wallet_strings.txt", "english_round8_strings.txt", "english_timeframe_strings.txt", "english_central_bank_strings.txt"]
ANCHORS = ["STR_STOCK_ERROR_NO_ORDER", "STR_STOCK_NEWS_ORDER_EXPIRED"]


def find(name, folder=None):
    """The file with this name (not case sensitive) anywhere next to this script. A copy in its own folder (e.g. src/saveload/)
    always wins, so an old copy that lies elsewhere in the repository can never be used by mistake."""
    import os
    if folder:
        exact = HERE / folder / name
        if exact.is_file():
            return exact
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


def ensure_include(text, line, anchors):
    """Adds an #include line after the first anchor line that exists in the text; nothing happens when it is there already."""
    if line in text:
        return text
    for a in anchors:
        m = re.search(r"^" + re.escape(a) + r"\n", text, re.M)
        if m:
            return text[:m.end()] + line + "\n" + text[m.end():]
    sys.exit(f"ERROR: no place found for {line}")


def main(tree):
    for name, folder in FILES.items():
        src = find(name, folder)
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

    # economy.cpp: switch off the interest and the overdraft charge of the original loan, and the original loan commands.
    p = tree / "src" / "economy.cpp"
    s = p.read_text(encoding="utf-8")
    changed = False
    if "IsLegacyLoanBypassed() ? Money(0)" in s:
        print("economy.cpp: interest of the original loan already switched off")
    else:
        old = re.search(r"([ \t]*)Money yearly_fee = c->current_loan \* _economy\.interest_rate / 100;\n([ \t]*)Money available_money = GetAvailableMoney\(c->index\);\n([ \t]*)if \(available_money < 0\) \{", s)
        if old:
            i1, i2, i3 = old.group(1), old.group(2), old.group(3)
            new = (f"{i1}Money yearly_fee = GameTimeManager::IsLegacyLoanBypassed() ? Money(0) : Money(c->current_loan * _economy.interest_rate / 100);\n"
                   f"{i2}Money available_money = GetAvailableMoney(c->index);\n"
                   f"{i3}if (available_money < 0 && !GameTimeManager::IsLegacyLoanBypassed()) {{")
            s = s[:old.start()] + new + s[old.end():]
            changed = True
            print("economy.cpp: interest and overdraft charge of the original loan switched off")
        else:
            print("WARNING: economy.cpp has a different layout, the original loan interest was not changed")
    if "STR_BANK_ERROR_LEGACY_LOAN" in s:
        print("economy.cpp: original loan commands already guarded")
    else:
        # CmdIncreaseLoan is the only way that the original system puts loan money into the cash (GUI, AI, script, console, network).
        # CmdDecreaseLoan stays, so that an old savegame can still repay its loan.
        inc_loan = re.search(r"^CommandCost CmdIncreaseLoan\([^)]*\)\s*\{\n", s, re.M)
        if inc_loan:
            s = s[:inc_loan.end()] + "\t/* The Central Bank replaces the original loan: no loan money enters the company cash through this command. */\n\tif (GameTimeManager::IsLegacyLoanBypassed()) return CommandCost(STR_BANK_ERROR_LEGACY_LOAN);\n\n" + s[inc_loan.end():]
            changed = True
            print("economy.cpp: CmdIncreaseLoan disabled")
        else:
            print("WARNING: CmdIncreaseLoan not found in economy.cpp, the original loan can still be increased")
    if changed and '#include "game_time/game_time_manager.h"' not in s:
        s = ensure_include(s, '#include "game_time/game_time_manager.h"', ['#include "company_func.h"', '#include "stdafx.h"'])
    if changed:
        p.write_text(s, encoding="utf-8")

    # window_type.h: the window class of the Central Bank.
    p = tree / "src" / "window_type.h"
    s = p.read_text(encoding="utf-8")
    if "WC_CENTRAL_BANK" in s:
        print("window_type.h: already changed")
    else:
        m = re.search(r"^\tWC_STOCK_ORDER,\n", s, re.M)
        if m:
            add = "\n\t/**\n\t * Central Bank window; replaces the company finances window. %Window numbers:\n\t *   - 0 = #CentralBankWidgets\n\t */\n\tWC_CENTRAL_BANK,\n"
            s = s[:m.end()] + add + s[m.end():]
            p.write_text(s, encoding="utf-8")
            print("window_type.h: WC_CENTRAL_BANK added")
        else:
            sys.exit("ERROR: WC_STOCK_ORDER not found in window_type.h - was stock-market.patch applied?")

    # widgets/CMakeLists.txt
    p = tree / "src" / "widgets" / "CMakeLists.txt"
    s = p.read_text(encoding="utf-8")
    if "central_bank_widget.h" in s:
        print("widgets/CMakeLists.txt: already changed")
    else:
        m = re.search(r"^([ \t]*)stock_widget\.h\n", s, re.M)
        if not m:
            sys.exit("ERROR: stock_widget.h not found in src/widgets/CMakeLists.txt - was stock-market.patch applied?")
        s = s[:m.start()] + f"{m.group(1)}central_bank_widget.h\n" + s[m.start():]
        p.write_text(s, encoding="utf-8")
        print("widgets/CMakeLists.txt: central_bank_widget.h added")

    # finances_gui.cpp: the company finances window opens the Central Bank window instead.
    p = tree / "src" / "finances_gui.cpp"
    s = p.read_text(encoding="utf-8")
    if "ShowCentralBankWindow" in s:
        print("finances_gui.cpp: already changed")
    else:
        m = re.search(r"^void ShowCompanyFinances\(CompanyID company\)\s*\{\n", s, re.M)
        if m:
            add = ("\t/* The Central Bank window replaces the original finances window. Every way to the finances (toolbar menu, company window, statusbar) comes here. */\n"
                   "\tif (GameTimeManager::IsLegacyLoanBypassed()) {\n\t\tShowCentralBankWindow();\n\t\treturn;\n\t}\n\n")
            s = s[:m.end()] + add + s[m.end():]
            s = ensure_include(s, '#include "central_bank_gui.h"', ['#include "company_func.h"', '#include "stdafx.h"'])
            s = ensure_include(s, '#include "game_time/game_time_manager.h"', ['#include "central_bank_gui.h"'])
            p.write_text(s, encoding="utf-8")
            print("finances_gui.cpp: ShowCompanyFinances() opens the Central Bank window")
        else:
            sys.exit("ERROR: ShowCompanyFinances not found in finances_gui.cpp")

    # statusbar_gui.cpp: the finance button of the statusbar opens the Central Bank window directly.
    p = tree / "src" / "statusbar_gui.cpp"
    s = p.read_text(encoding="utf-8")
    if "ShowCentralBankWindow" in s:
        print("statusbar_gui.cpp: already changed")
    else:
        s2, n = re.subn(r"(case WID_S_RIGHT:\s*if \(_local_company != COMPANY_SPECTATOR\) )ShowCompanyFinances\(_local_company\);", r"\1ShowCentralBankWindow();", s)
        if n != 1:
            sys.exit("ERROR: the WID_S_RIGHT click handler was not found in statusbar_gui.cpp")
        s2 = ensure_include(s2, '#include "central_bank_gui.h"', ['#include "company_func.h"', '#include "stdafx.h"'])
        p.write_text(s2, encoding="utf-8")
        print("statusbar_gui.cpp: the finance button opens the Central Bank window")

    # english.txt
    wanted = {}
    for name in STRING_FILES:
        for line in find(name, "src").read_text(encoding="utf-8").replace("\r", "").splitlines():
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

