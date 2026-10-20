#!/usr/bin/env python3
"""Adds the missing wallet / timeframe integration to an OpenTTD-Stock checkout.

Usage:  python3 apply_wallet_fix.py /path/to/OpenTTD-Stock

It edits two files and is safe to run more than once:
  src/command_type.h   adds CMD_STOCK_TRANSFER and CMD_STOCK_ENABLE_DAILY to the Commands enum
  src/lang/english.txt adds the Deposit / Withdraw / Daily-Monthly-Yearly strings
"""
import re
import sys
from pathlib import Path

NEW_STRINGS = """STR_STOCK_DEPOSIT                                               :Deposit
STR_STOCK_WITHDRAW                                              :Withdraw
STR_STOCK_TF_MONTH                                              :Monthly
STR_STOCK_TF_YEAR                                               :Yearly
STR_STOCK_TIP_DEPOSIT                                           :{BLACK}Move money from your company cash to the trading balance. It still counts as equity
STR_STOCK_TIP_WITHDRAW                                          :{BLACK}Move money from the trading balance back to your company cash. Only the free margin can be withdrawn
STR_STOCK_TIP_TF_MONTH                                          :{BLACK}Show one candle per month
STR_STOCK_TIP_TF_YEAR                                           :{BLACK}Show one candle per year. Drag the chart with the mouse to scroll through the history
STR_STOCK_QUERY_DEPOSIT                                         :Amount to deposit
STR_STOCK_QUERY_WITHDRAW                                        :Amount to withdraw
STR_STOCK_TRADING_BALANCE                                       :{WHITE}   Trading balance: {CURRENCY_LONG}
STR_STOCK_ERROR_CANT_TRANSFER                                   :{WHITE}Can't transfer the money...
STR_STOCK_ERROR_BAD_AMOUNT                                      :{WHITE}Invalid amount
STR_STOCK_ERROR_NO_FUNDS                                        :{WHITE}Not enough money
STR_STOCK_TF_DAY                                                :Daily
STR_STOCK_TIP_TF_DAY                                            :{BLACK}Show one candle per day. Daily candles are only recorded from the first time you open this view, so there is no history before that
STR_STOCK_DAILY_WAIT                                            :{WHITE}Recording daily candles. They appear as the days pass
"""

def main(root: Path) -> int:
    # 1) command_type.h
    p = root / "src" / "command_type.h"
    s = p.read_text(encoding="utf-8")
    if "CMD_STOCK_TRANSFER" in s:
        print("command_type.h: already patched")
    else:
        m = re.search(r"^([ \t]*)CMD_STOCK_MODIFY,[^\n]*\n", s, re.M)
        if not m:
            print("command_type.h: CMD_STOCK_MODIFY not found - is this the right checkout?")
            return 1
        ind = m.group(1)
        add = (ind + "CMD_STOCK_TRANSFER,                 ///< move money between company cash and the trading balance\n" +
               ind + "CMD_STOCK_ENABLE_DAILY,             ///< start recording daily candles\n")
        s = s[:m.end()] + add + s[m.end():]
        p.write_text(s, encoding="utf-8")
        print("command_type.h: patched")

    # 2) english.txt
    p = root / "src" / "lang" / "english.txt"
    s = p.read_text(encoding="utf-8")
    missing = []
    for line in NEW_STRINGS.strip().splitlines():
        key = line.split(" ", 1)[0]
        if not re.search(r"^" + re.escape(key) + r"\s", s, re.M):
            missing.append(line)
    if not missing:
        print("english.txt: already patched")
    else:
        m = re.search(r"^STR_STOCK_NEWS_ORDER_EXPIRED[^\n]*\n", s, re.M)
        if not m:
            print("english.txt: STR_STOCK_NEWS_ORDER_EXPIRED not found - is this the right checkout?")
            return 1
        s = s[:m.end()] + "\n".join(missing) + "\n" + s[m.end():]
        p.write_text(s, encoding="utf-8")
        print(f"english.txt: added {len(missing)} strings")
    return 0

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(Path(sys.argv[1])))
