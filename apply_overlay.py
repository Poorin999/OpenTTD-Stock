#!/usr/bin/env python3
"""Puts the stock market mod, with the wallet system, into an OpenTTD source tree. Safe to run more than once.

Usage:  python3 apply_overlay.py /path/to/openttd

It looks for the mod files by name anywhere next to this script, so it does not matter in which folder of the repository they are.
  - copies the C++ files into src/
  - adds the commands to the Commands enum in src/command_type.h
  - adds the new source files to src/CMakeLists.txt
  - adds the strings to src/lang/english.txt
  - the original loan is removed: no interest or overdraft charge (economy.cpp), CmdIncreaseLoan and CmdDecreaseLoan refused, a new company starts without a loan;
    old loans are moved to the central bank by central_bank.cpp; bankruptcy and the loan score count the central bank debt
  - window_type.h / widgets: the Central Bank window class (WC_CENTRAL_BANK)
  - ShowCompanyFinances() (company_gui.cpp), statusbar_gui.cpp: the finances menu and the statusbar button open ShowCentralBankWindow()
  - timer_game_calendar.cpp, economy_settings.ini: the date of the original game follows the HH:MM clock (a new day starts at 00:00)
"""
import re
import sys
from pathlib import Path

# True: the game writes money_trace.log (next to openttd.exe) with every change of company cash, to find out where money goes.
# Set to False and build again to remove it.
MONEY_TRACE = True

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


def find_definition(tree, pattern):
    """The .cpp file in src/ that holds a definition (matched by the regex), or None. The place of a function differs between OpenTTD versions."""
    rx = re.compile(pattern, re.M)
    for f in sorted((tree / "src").glob("*.cpp")):
        if rx.search(f.read_text(encoding="utf-8", errors="replace")):
            return f
    return None


TRACE_CODE = r"""/* MONEY_TRACE_BEGIN: debug trace of the cash of the companies, written to money_trace.log once per game minute. */
static const char *MoneyTraceName(int type)
{
	static const char *const names[] = {"CONSTRUCTION", "NEW_VEHICLES", "TRAIN_RUN", "ROADVEH_RUN", "AIRCRAFT_RUN", "SHIP_RUN", "PROPERTY",
			"TRAIN_REVENUE", "ROADVEH_REVENUE", "AIRCRAFT_REVENUE", "SHIP_REVENUE", "LOAN_INTEREST", "OTHER"};
	return (type >= 0 && type < 13) ? names[type] : "?";
}

/** Sum of the costs (positive = money out) per company and expense type since the last game minute. */
static std::map<std::pair<int, int>, int64_t> _money_trace_sum;
/** The cash of every company at the last game minute, and the total that the expense functions took since then. */
static int64_t _money_trace_last[MAX_COMPANIES];
static bool _money_trace_known[MAX_COMPANIES];
static int64_t _money_trace_explained[MAX_COMPANIES];

static void MoneyTraceRecord(CompanyID company, int type, int64_t cost)
{
	_money_trace_sum[{company.base(), type}] += cost;
	_money_trace_explained[company.base()] += cost;
}

static void MoneyTraceLine(std::ofstream &f, const GameTimeSnapshot &now, int company, const char *what, int64_t amount)
{
	f << fmt::format("year {} day {:3} {:02}:{:02}  company {}  {:<16} {:+}\n", now.year, now.day, now.hour, now.minute, company, what, amount);
}

static void MoneyTraceWrite(const GameTimeSnapshot &now)
{
	std::ofstream f("money_trace.log", std::ios::app);
	if (!f) return;
	for (const auto &[key, sum] : _money_trace_sum) {
		if (sum != 0) MoneyTraceLine(f, now, key.first, MoneyTraceName(key.second), -sum);
	}
	_money_trace_sum.clear();
	for (const Company *c : Company::Iterate()) {
		const int i = c->index.base();
		const int64_t money = static_cast<int64_t>(c->money);
		if (_money_trace_known[i]) {
			/* Change of the cash that the expense functions do not explain: wallets, central bank, scripts, cheats. */
			const int64_t direct = (money - _money_trace_last[i]) + _money_trace_explained[i];
			if (direct != 0) MoneyTraceLine(f, now, i, "DIRECT", direct);
		}
		_money_trace_known[i] = true;
		_money_trace_last[i] = money;
		_money_trace_explained[i] = 0;
	}
}

static const GameTimeSubscription _money_trace_minute(GameTimeEvent::Minute, [](const GameTimeSnapshot &now) { MoneyTraceWrite(now); });
/* MONEY_TRACE_END */

"""


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

    # economy.cpp: the original loan system is gone. No interest on the original loan and no overdraft charge (the central bank is the only lender),
    # the bankruptcy check and the loan score count the debt at the central bank instead of the original loan.
    p = tree / "src" / "economy.cpp"
    s = p.read_text(encoding="utf-8")
    changed = False
    if "GetCentralBankDebt(c->index)" in s:
        print("economy.cpp: original loan already removed from the economy")
    else:
        old = re.search(r"[ \t]*Money yearly_fee = c->current_loan \* _economy\.interest_rate / 100;\n.*?"
                        r"[ \t]*SubtractMoneyFromCompany\(CommandCost\(EXPENSES_LOAN_INTEREST, up_to_this_month - up_to_previous_month\)\);\n\n", s, re.S)
        bankrupt = "if (c->money - c->current_loan >= -c->GetMaxLoan()) {"
        score = "_score_part[owner][SCORE_LOAN] = _score_info[SCORE_LOAN].needed - c->current_loan;"
        if not old or bankrupt not in s or score not in s:
            sys.exit("ERROR: economy.cpp has a different layout, the original loan was not removed from the economy")
        s = s[:old.start()] + "\t\t/* No interest on an original loan and no overdraft charge: the central bank is the only lender and charges its own interest. */\n" + s[old.end():]
        s = s.replace(bankrupt, "if (c->money - Money(GetCentralBankDebt(c->index)) >= -c->GetMaxLoan()) {", 1)
        s = s.replace(score, "_score_part[owner][SCORE_LOAN] = _score_info[SCORE_LOAN].needed - c->current_loan - Money(GetCentralBankDebt(c->index));", 1)
        s = ensure_include(s, '#include "central_bank.h"', ['#include "company_func.h"', '#include "stdafx.h"'])
        changed = True
        print("economy.cpp: no interest on the original loan; bankruptcy and the loan score count the central bank debt")
    if changed and '#include "game_time/game_time_manager.h"' not in s:
        s = ensure_include(s, '#include "game_time/game_time_manager.h"', ['#include "company_func.h"', '#include "stdafx.h"'])
    if changed:
        p.write_text(s, encoding="utf-8")

    # CmdIncreaseLoan and CmdDecreaseLoan are the only ways into the original loan (GUI, AI, script, console, network): both are refused.
    # A loan that an old savegame still has is moved to the central bank by central_bank.cpp. The file differs between OpenTTD versions.
    for name in ("CmdIncreaseLoan", "CmdDecreaseLoan"):
        loan_rx = r"^CommandCost " + name + r"\([^)]*\)\s*\{\n"
        p = find_definition(tree, loan_rx)
        if p is None:
            print(f"WARNING: {name} not found in any src/*.cpp, the original loan can still be used")
            continue
        s = p.read_text(encoding="utf-8")
        m = re.search(loan_rx, s, re.M)
        if "STR_BANK_ERROR_LEGACY_LOAN" in s[m.end():m.end() + 400]:
            print(f"{p.name}: {name} already disabled")
            continue
        s = s[:m.end()] + "\t/* The original loan does not exist any more: the central bank is the only lender. */\n\treturn CommandCost(STR_BANK_ERROR_LEGACY_LOAN);\n\n" + s[m.end():]
        p.write_text(s, encoding="utf-8")
        print(f"{p.name}: {name} disabled")

    # A new company starts with its own money and without the original loan (company_cmd.cpp: "c->money = c->current_loan = ...").
    start_rx = r"^[ \t]*(\w+)->money\s*=\s*\1->current_loan\s*=\s*[^;]*;[ \t]*\n"
    p = find_definition(tree, start_rx)
    if p is None:
        print("WARNING: the start money of a new company was not found in src/*.cpp, new companies still start with the original loan")
    else:
        s = p.read_text(encoding="utf-8")
        if "GetStartingCapital" in s:
            print(f"{p.name}: start money already changed")
        else:
            m = re.search(start_rx, s, re.M)
            v = m.group(1)
            add = (f"\t/* There is no original loan: a company starts with its own money and no loan. */\n"
                   f"\t{v}->money = Money(GameTimeManager::GetStartingCapital());\n"
                   f"\t{v}->current_loan = 0;\n")
            s = s[:m.end()] + add + s[m.end():]
            s = ensure_include(s, '#include "game_time/game_time_manager.h"', ['#include "company_func.h"', '#include "stdafx.h"'])
            p.write_text(s, encoding="utf-8")
            print(f"{p.name}: a new company starts with the starting capital and no loan")

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

    # The company finances window (company_gui.cpp in current OpenTTD, finances_gui.cpp in some trees) opens the Central Bank window instead.
    fin_rx = r"^void ShowCompanyFinances\(CompanyID company\)\s*\{\n"
    p = find_definition(tree, fin_rx)
    if p is None:
        sys.exit("ERROR: ShowCompanyFinances not found in any src/*.cpp")
    s = p.read_text(encoding="utf-8")
    if "ShowCentralBankWindow" in s:
        print(f"{p.name}: already changed")
    else:
        m = re.search(fin_rx, s, re.M)
        add = ("\t/* The Central Bank window replaces the original finances window. Every way to the finances (toolbar menu, company window, statusbar) comes here. */\n"
               "\tShowCentralBankWindow();\n\treturn;\n\n")
        s = s[:m.end()] + add + s[m.end():]
        s = ensure_include(s, '#include "central_bank_gui.h"', ['#include "company_func.h"', '#include "stdafx.h"'])
        s = ensure_include(s, '#include "game_time/game_time_manager.h"', ['#include "central_bank_gui.h"'])
        p.write_text(s, encoding="utf-8")
        print(f"{p.name}: ShowCompanyFinances() opens the Central Bank window")

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

    # statusbar_gui.cpp: the clock of the mod is shown as HH:MM in front of the date, and the date panel is wider.
    p = tree / "src" / "statusbar_gui.cpp"
    s = p.read_text(encoding="utf-8")
    if "GameTimeManager" in s:
        print("statusbar_gui.cpp: the time is already shown")
    else:
        edits = [
            ("d = GetStringBoundingBox(GetString(STR_JUST_DATE_LONG, GetParamMaxValue(TimerGameCalendar::DateAtStartOfYear(CalendarTime::MAX_YEAR).base())));",
             "d = GetStringBoundingBox(GetString(STR_JUST_DATE_LONG, GetParamMaxValue(TimerGameCalendar::DateAtStartOfYear(CalendarTime::MAX_YEAR).base())));\n\t\t\t\td.width += GetStringBoundingBox(std::string(\"00:00 \")).width; // HH:MM and a space in front of the date"),
            ("DrawString(tr, GetString(STR_JUST_DATE_LONG, TimerGameCalendar::date), TC_WHITE, SA_HOR_CENTER);",
             "DrawString(tr, fmt::format(\"{:02}:{:02} {}\", GameTimeManager::Get().GetCurrentGameHour(), GameTimeManager::Get().GetCurrentGameMinute(), GetString(STR_JUST_DATE_LONG, TimerGameCalendar::date)), TC_WHITE, SA_HOR_CENTER);"),
            ("NWidget(WWT_PANEL, COLOUR_GREY, WID_S_LEFT), SetMinimalSize(140, 12), EndContainer(),",
             "NWidget(WWT_PANEL, COLOUR_GREY, WID_S_LEFT), SetMinimalSize(190, 12), EndContainer(),"),
            ("\tconst IntervalTimer<TimerGameCalendar> daily_interval = {",
             "\tint last_clock_minute = -1; ///< The game minute that is drawn in the date panel.\n\n\t/** The date panel shows HH:MM, so it is drawn again every time the game minute changes. */\n\tconst IntervalTimer<TimerGameTick> clock_interval = {{TimerGameTick::Priority::NONE, 1}, [this](auto) {\n\t\tconst int minute = GameTimeManager::Get().GetCurrentGameMinute();\n\t\tif (minute == this->last_clock_minute) return;\n\t\tthis->last_clock_minute = minute;\n\t\tthis->SetWidgetDirty(WID_S_LEFT);\n\t}};\n\n\tconst IntervalTimer<TimerGameCalendar> daily_interval = {"),
        ]
        for old, repl in edits:
            if s.count(old) != 1:
                sys.exit("ERROR: statusbar_gui.cpp: the text to change was not found: " + old[:50])
            s = s.replace(old, repl)
        s = ensure_include(s, '#include "3rdparty/fmt/format.h"', ['#include "stdafx.h"'])
        s = ensure_include(s, '#include "timer/timer_game_tick.h"', ['#include "timer/timer_game_calendar.h"'])
        s = ensure_include(s, '#include "game_time/game_time_manager.h"', ['#include "timer/timer_window.h"'])
        p.write_text(s, encoding="utf-8")
        print("statusbar_gui.cpp: HH:MM is shown in front of the date")

    # economy.cpp: the quarter of the company statistics (old_economy) closes by the clock of the mod, every 90 game days, not every 3 economy months.
    p = tree / "src" / "economy.cpp"
    s = p.read_text(encoding="utf-8")
    if "CompaniesRollQuarter" in s:
        print("economy.cpp: the quarter already closes by the game clock")
    else:
        check = "\t/* Only run the economic statistics and update company stats every 3rd economy month (1st of quarter). */\n\tif (!HasBit(1 << 0 | 1 << 3 | 1 << 6 | 1 << 9, TimerGameEconomy::month)) return;\n"
        m = re.search(r"\n\tfor \(Company \*c : Company::Iterate\(\)\) \{\n\t\t/\* Drop the oldest history off the end \*/.*?\n\tSetWindowDirty\(WC_COMPANY_LEAGUE, 0\);\n\}\n", s, re.S)
        head = re.search(r"/\*\*\n \* Update the finances of all companies\.", s)
        if check not in s or not m or not head:
            sys.exit("ERROR: economy.cpp has a different layout, the quarter was not connected to the game clock")
        body = m.group(0)
        roll = ("/** Close the quarter of the company statistics: the history moves one place, the ratings and values are updated. */\nstatic void CompaniesRollQuarter()\n{"
                + body)
        s = s[:m.start()] + "\n\tCompaniesRollQuarter();\n}\n" + s[m.end():]
        s = s.replace(check, "\t/* With GAME_TIME_SYNC_QUARTERS the quarter is closed by the clock of the mod instead, see _game_quarter below. */\n\tif (GameTimeManager::AreQuartersSynced()) return;\n\n" + check, 1)
        s = s.replace(head.group(0), roll + "\n" + head.group(0), 1)
        s = s.replace("static void DoAcquireCompany(Company *c, bool hostile_takeover)",
            "/** Every 90 game days (at 00:00 of the day that starts a new game quarter) the quarter of the company statistics closes. */\n"
            "static const GameTimeSubscription _game_quarter(GameTimeEvent::Day, [](const GameTimeSnapshot &now)\n{\n"
            "\tif (!GameTimeManager::AreQuartersSynced()) return;\n"
            "\tconst uint64_t day = now.total_minutes / GameTimeManager::MINUTES_PER_DAY;\n"
            "\tif (day == 0 || day % GameTimeManager::DAYS_PER_QUARTER != 0) return;\n"
            "\tCompaniesRollQuarter();\n});\n\nstatic void DoAcquireCompany(Company *c, bool hostile_takeover)", 1)
        s = ensure_include(s, '#include "game_time/game_time_manager.h"', ['#include "company_func.h"', '#include "stdafx.h"'])
        p.write_text(s, encoding="utf-8")
        print("economy.cpp: the quarter of the company statistics closes by the game clock")

    # timer_game_calendar.cpp: the calendar date follows the HH:MM clock of the mod. A new calendar day starts exactly when the clock passes 23:59 -> 00:00
    # and date_fract (the part of the day) follows the time of the day, so the status bar "HH:MM date" is one clock. Needs wallclock timekeeping units.
    p = tree / "src" / "timer" / "timer_game_calendar.cpp"
    s = p.read_text(encoding="utf-8")
    if "TryConsumeCalendarDay" in s:
        print("timer_game_calendar.cpp: already follows the game clock")
    else:
        old = re.search(r"([ \t]*)/\* If we are using a non-default calendar progression speed.*?\n"
                        r"[ \t]*TimerGameCalendar::date_fract\+\+;\n\n"
                        r"[ \t]*/\* Check if we entered a new day\. \*/\n"
                        r"[ \t]*if \(TimerGameCalendar::date_fract < Ticks::DAY_TICKS\) return true;\n"
                        r"[ \t]*TimerGameCalendar::date_fract = 0;\n"
                        r"[ \t]*TimerGameCalendar::sub_date_fract = 0;\n", s, re.S)
        if not old:
            sys.exit("ERROR: timer_game_calendar.cpp has a different layout, the calendar was not connected to the game clock")
        new = (
            "\t/* The clock of the mod decides when a day starts: at 00:00 exactly, so the date and HH:MM always belong together.\n"
            "\t * This works in calendar and in wallclock timekeeping units; the economy keeps counting its own 74-tick days. */\n"
            "\tif (GameTimeManager::IsCalendarSynced()) {\n"
            "\t\tif (!GameTimeManager::Get().TryConsumeCalendarDay()) {\n"
            "\t\t\t/* Same day: date_fract follows the time of the day (for the vehicle calendar updates, one of 74 steps per day). */\n"
            "\t\t\tconst auto fract = static_cast<TimerGameCalendar::DateFract>(GameTimeManager::Get().GetDayFraction(Ticks::DAY_TICKS));\n"
            "\t\t\tif (fract == TimerGameCalendar::date_fract) return false;\n"
            "\t\t\tTimerGameCalendar::date_fract = fract;\n"
            "\t\t\treturn true;\n"
            "\t\t}\n"
            "\t} else {\n"
            + "".join("\t" + l if l.strip() else l for l in old.group(0).splitlines(True)[:-2])
            + "\t}\n"
            "\tTimerGameCalendar::date_fract = 0;\n"
            "\tTimerGameCalendar::sub_date_fract = 0;\n")
        s = s[:old.start()] + new + s[old.end():]
        for inc, anchors in (('#include "../game_time/game_time_manager.h"', ['#include "timer_game_calendar.h"']),):
            s = ensure_include(s, inc, anchors + ['#include "../stdafx.h"', '#include "stdafx.h"'])
        p.write_text(s, encoding="utf-8")
        print("timer_game_calendar.cpp: the calendar day follows the game clock")

    # economy_settings.ini: new games use wallclock units, with 8760 minutes (365 days of 24 minutes) per calendar year; the setting is then true
    # for the display, and it is what the original calendar would use if GAME_TIME_SYNC_CALENDAR is turned off.
    p = tree / "src" / "table" / "settings" / "economy_settings.ini"
    s = p.read_text(encoding="utf-8")
    if re.search(r"var\s*=\s*economy\.minutes_per_calendar_year\n(?:.*\n)*?def\s*=\s*8760\n", s):
        print("economy_settings.ini: already changed")
    else:
        s2, n1 = re.subn(r"(var\s*=\s*economy\.timekeeping_units\n(?:.*\n)*?def\s*=\s*)TKU_CALENDAR", r"\1TKU_WALLCLOCK", s, count=1)
        s2, n2 = re.subn(r"(var\s*=\s*economy\.minutes_per_calendar_year\n(?:.*\n)*?def\s*=\s*)CalendarTime::DEF_MINUTES_PER_YEAR", r"\g<1>8760", s2, count=1)
        if n1 != 1 or n2 != 1:
            print("WARNING: economy_settings.ini has a different layout, the new-game defaults were not changed (choose wallclock units in the new game settings)")
        else:
            p.write_text(s2, encoding="utf-8")
            print("economy_settings.ini: new games default to wallclock units and 8760 minutes per calendar year")

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
    # Money trace (debug): every change of the cash of a company is summed per game minute and written to money_trace.log.
    # Money that moves without the normal expense functions (wallets, central bank) shows as "DIRECT". Switch off with MONEY_TRACE = False.
    p = find_definition(tree, r"^void SubtractMoneyFromCompanyFract\(")
    if p is None:
        print("WARNING: company_cmd.cpp not found, money trace not added")
    else:
        s = p.read_text(encoding="utf-8")
        if "MONEY_TRACE_BEGIN" in s:
            print(f"{p.name}: money trace already present")
        elif not MONEY_TRACE:
            print(f"{p.name}: money trace is switched off")
        else:
            anchor = "/**\n * Deduct costs of a command from the money of a company."
            call_old = "\tif (cost.GetCost() == 0) return;\n\tassert(cost.GetExpensesType() != INVALID_EXPENSES);\n"
            if anchor not in s or call_old not in s:
                print("WARNING: company_cmd.cpp has a different layout, money trace not added")
            else:
                trace = TRACE_CODE
                s = s.replace(anchor, trace + anchor, 1)
                s = s.replace(call_old, call_old + "\tMoneyTraceRecord(c->index, static_cast<int>(cost.GetExpensesType()), static_cast<int64_t>(cost.GetCost()));\n", 1)
                for inc in ('#include <fstream>', '#include <map>'):
                    s = ensure_include(s, inc, ['#include "stdafx.h"'])
                s = ensure_include(s, '#include "game_time/game_time_manager.h"', ['#include "company_func.h"', '#include "stdafx.h"'])
                s = ensure_include(s, '#include "core/format.hpp"', ['#include "company_func.h"', '#include "stdafx.h"'])
                p.write_text(s, encoding="utf-8")
                print(f"{p.name}: money trace added (money_trace.log)")

    print("overlay applied")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    main(Path(sys.argv[1]).resolve())

