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
  - script_company.hpp/.cpp: ScriptCompany::GetCentralBankDebt and GetCentralBankAvailable, so that AIs can see the central bank debt
  - timer_game_calendar.cpp, economy_settings.ini: the date of the original game follows the HH:MM clock (a new day starts at 00:00)
"""
import re
import sys
from pathlib import Path

# True: the game writes money_trace.log (next to openttd.exe) with every change of company cash, to find out where money goes.
# Set to False and build again to remove it.
MONEY_TRACE = True

# True: the fixed fee that OpenTTD takes from every company (about 1/4 of the base station price, 99 pounds at the start) is removed.
# The money trace showed it is taken about once per game hour. Set to False and build again to keep the fee.
REMOVE_FIXED_COMPANY_FEE = True

HERE = Path(__file__).resolve().parent

FILES = {  # file name -> folder in the OpenTTD tree
    "stock_base.h": "src", "stock_base.cpp": "src",
    "stock_cmd.h": "src", "stock_cmd.cpp": "src",
    "stock_gui.h": "src", "stock_gui.cpp": "src", "stock_order_gui.cpp": "src",
    "wallet_base.h": "src", "wallet_base.cpp": "src",
    "wallet_cmd.h": "src", "wallet_cmd.cpp": "src",
    "central_bank.h": "src", "central_bank.cpp": "src",
    "gold_reserve.h": "src", "gold_reserve.cpp": "src",
    "gold_panel.h": "src", "gold_panel.cpp": "src", "gold_buy_gui.cpp": "src",
    "stock_dev.h": "src", "stock_console.cpp": "src",
    "central_bank_gui.h": "src", "central_bank_gui.cpp": "src",
    "central_bank_widget.h": "src/widgets",
    "RetailMath.h": "src", "RetailMarketManager.h": "src", "RetailMarketManager.cpp": "src",
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
    ("CMD_GOLD_BUY", "buy gold for the reserve"),
    ("CMD_GOLD_SELL", "sell gold of the reserve"),
]
NEW_SOURCES = ["wallet_base.cpp", "wallet_base.h", "wallet_cmd.cpp", "wallet_cmd.h", "central_bank.cpp", "central_bank.h", "gold_reserve.cpp", "gold_reserve.h", "gold_panel.cpp", "gold_panel.h", "gold_buy_gui.cpp", "stock_console.cpp", "stock_dev.h",
               "central_bank_gui.cpp", "central_bank_gui.h",
               "RetailMath.h", "RetailMarketManager.h", "RetailMarketManager.cpp",
               "game_time/game_time_manager.h", "game_time/game_time_manager.cpp", "game_time/game_time_driver.cpp", "game_time/game_time_aggregator.h"]
REPLACE_KEYS = {"STR_STOCK_TIP_TOOLBOX", "STR_STOCK_TIP_SUMMARY", "STR_STOCK_ORDER_INFO", "STR_STOCK_TIP_DEPOSIT", "STR_STOCK_TIP_WITHDRAW"}
STRING_FILES = ["english_stock_strings.txt", "english_wallet_strings.txt", "english_round8_strings.txt", "english_timeframe_strings.txt", "english_central_bank_strings.txt", "english_gold_strings.txt"]
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


# The gold panel goes into the operating profit graph window of OpenTTD (graph_gui.cpp, widgets/graph_widget.h).
# These two files are PATCHED IN PLACE, not replaced: the code is inserted at anchors, and the names that the file itself uses
# (colours, base class) are read from the file, so it fits the real OpenTTD 15.3 whatever the naming style of that version is.
# If an anchor is not found the script stops with an error and nothing is written. A file that has the marker is left alone.
GOLD_MARKER = "GOLD_PANEL_PATCH"

GOLD_STRUCT_CODE = r"""
	/* GOLD_PANEL_PATCH: the gold trading panel is shown in the place of the profit graph. */
	GoldPanel gold;         ///< The gold trading panel (state and actions).
	bool show_gold = true;  ///< Whether the gold panel (plane 1) or the profit graph (plane 0) is shown.

	/** Redraw regularly while the gold panel is shown: the price and the cash change all the time. */
	const IntervalTimer<TimerWindow> gold_interval = {std::chrono::milliseconds(500), [this](auto) {
		if (this->show_gold) this->SetDirty();
	}};

	/** Show the gold trading panel (true) or the profit graph (false) in the main area of the window. */
	void ShowGold(bool show)
	{
		this->show_gold = show;
		this->GetWidget<NWidgetStacked>(WID_PHG_GOLD_SEL)->SetDisplayedPlane(show ? 1 : 0);
		this->SetWidgetLoweredState(WID_PHG_GOLD_TOGGLE, show);
		this->SetDirty();
	}

	void OnPaint() override
	{
		/* Buy Gold opens the buy window (it checks the unborrowed liquid cash and the stock of the market); Sell Gold only works with gold that the company holds. */
		this->SetWidgetDisabledState(WID_PHG_GOLD_BUY, !this->gold.CanOpenBuyWindow());
		this->SetWidgetDisabledState(WID_PHG_GOLD_SELL, !this->gold.CanSell());
		this->DrawWidgets();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_PHG_GOLD_INFO: this->gold.DrawInfo(r); return;
			case WID_PHG_GOLD_BUY_QTY: this->gold.DrawQuantity(r, true); return;
			case WID_PHG_GOLD_SELL_QTY: this->gold.DrawQuantity(r, false); return;
			default: break;
		}
		this->@BASE@::DrawWidget(r, widget);
	}

	void OnClick(Point pt, WidgetID widget, int click_count) override
	{
		switch (widget) {
			case WID_PHG_GOLD_TOGGLE: this->ShowGold(!this->show_gold); return;
			case WID_PHG_GOLD_BUY_QTY: this->gold.AskQuantity(this, true); return;
			case WID_PHG_GOLD_SELL_QTY: this->gold.AskQuantity(this, false); return;
			case WID_PHG_GOLD_BUY: ShowGoldBuyWindow(this->gold.buy_qty); return;
			case WID_PHG_GOLD_SELL: this->gold.Sell(); this->SetDirty(); return;
			default: break;
		}
		this->@BASE@::OnClick(pt, widget, click_count);
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		this->gold.QueryFinished(str);
		this->SetDirty();
	}

"""

GOLD_NESTED_CODE = r"""		/* GOLD_PANEL_PATCH: the main area is the profit graph (plane 0) or the gold trading panel (plane 1). Both fill and resize with the window. */
		NWidget(NWID_SELECTION, @INVALID@, WID_PHG_GOLD_SEL),
@GRAPH_LINE@			NWidget(NWID_VERTICAL),
				NWidget(WWT_PANEL, @PANEL@, WID_PHG_GOLD_INFO), SetMinimalSize(576, 140), SetFill(1, 1), SetResize(1, 1),
				EndContainer(),
				NWidget(NWID_HORIZONTAL),
					NWidget(WWT_PANEL, @PANEL@, WID_PHG_GOLD_BUY_QTY), SetMinimalSize(150, 14), SetFill(1, 0), SetResize(1, 0), SetToolTip(STR_GOLD_TIP_QTY),
					EndContainer(),
					NWidget(WWT_PUSHTXTBTN, @PANEL@, WID_PHG_GOLD_BUY), SetMinimalSize(90, 14), SetStringTip(STR_GOLD_BUY, STR_GOLD_TIP_BUY),
					NWidget(WWT_PANEL, @PANEL@, WID_PHG_GOLD_SELL_QTY), SetMinimalSize(150, 14), SetFill(1, 0), SetResize(1, 0), SetToolTip(STR_GOLD_TIP_QTY),
					EndContainer(),
					NWidget(WWT_PUSHTXTBTN, @PANEL@, WID_PHG_GOLD_SELL), SetMinimalSize(90, 14), SetStringTip(STR_GOLD_SELL, STR_GOLD_TIP_SELL),
				EndContainer(),
			EndContainer(),
		EndContainer(),
"""

GOLD_WIDGET_IDS = """	WID_PHG_GOLD_SEL,       ///< Selection: the profit graph or the gold trading panel (GOLD_PANEL_PATCH).
	WID_PHG_GOLD_TOGGLE,    ///< Button: switch between the gold panel and the profit graph.
	WID_PHG_GOLD_INFO,      ///< Gold panel: price, backing, credit standing and holdings.
	WID_PHG_GOLD_BUY_QTY,   ///< Gold panel: ounces to buy (click to type).
	WID_PHG_GOLD_BUY,       ///< Gold panel: Buy Gold button.
	WID_PHG_GOLD_SELL_QTY,  ///< Gold panel: ounces to sell (click to type).
	WID_PHG_GOLD_SELL,      ///< Gold panel: Sell Gold button.
"""


def _read_lf(path):
    raw = path.read_bytes().decode("utf-8")
    return raw.replace("\r\n", "\n"), "\r\n" in raw


def _write_like(path, text, crlf):
    path.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))


CONSOLE_MARKER = "STOCK_CONSOLE_PATCH"


def patch_console_file(tree):
    """Register the test commands of the mod ('stock_dev', 'stock_skip', in stock_console.cpp): one declaration and one call in
    IConsoleStdLibRegister() of console_cmds.cpp. In place, at an anchor; nothing is written when the anchor is missing."""
    target = tree / "src" / "console_cmds.cpp"
    if not target.is_file():
        sys.exit(f"ERROR: {target} not found in the OpenTTD tree")
    text, crlf = _read_lf(target)
    if CONSOLE_MARKER in text:
        print("console_cmds.cpp: test commands already registered")
        return
    m = re.search(r"^void IConsoleStdLibRegister\(\)\n\{\n", text, re.M)
    if not m:
        sys.exit("ERROR: void IConsoleStdLibRegister() not found in console_cmds.cpp (anchor of the console patch)")
    call = f"\tRegisterStockConsoleCommands(); /* {CONSOLE_MARKER} */\n"
    decl = f"void RegisterStockConsoleCommands(); /* {CONSOLE_MARKER}: stock_console.cpp */\n\n"
    text = text[:m.start()] + decl + text[m.start():m.end()] + call + text[m.end():]
    _write_like(target, text, crlf)
    print("console_cmds.cpp: test commands registered")


def patch_graph_files(tree):
    gui = tree / "src" / "graph_gui.cpp"
    wid = tree / "src" / "widgets" / "graph_widget.h"
    for f in (gui, wid):
        if not f.is_file():
            sys.exit(f"ERROR: {f} not found in the OpenTTD tree")
    g, g_crlf = _read_lf(gui)
    w, w_crlf = _read_lf(wid)

    if GOLD_MARKER in g and GOLD_MARKER in w:
        print("graph_gui.cpp, graph_widget.h: gold panel already there")
        return
    if GOLD_MARKER in g or GOLD_MARKER in w:
        sys.exit("ERROR: only one of graph_gui.cpp and graph_widget.h has the gold panel patch; restore both from OpenTTD and run again")

    # 1. the widget numbers
    m = re.search(r"^[ \t]*WID_PHG_DETAILED_PERFORMANCE,[^\n]*\n", w, re.M)
    if not m:
        sys.exit("ERROR: WID_PHG_DETAILED_PERFORMANCE not found in widgets/graph_widget.h (anchor of the gold panel patch)")
    w = w[:m.end()] + "\n" + GOLD_WIDGET_IDS + w[m.end():]

    # 2. the include
    includes = list(re.finditer(r'^#include "widgets/graph_widget\.h"[^\n]*\n', g, re.M))
    if not includes:
        sys.exit('ERROR: #include "widgets/graph_widget.h" not found in graph_gui.cpp (anchor of the gold panel patch)')
    g = g[:includes[-1].end()] + '#include "gold_panel.h" /* GOLD_PANEL_PATCH */\n' + g[includes[-1].end():]

    # 3. the window: new members and handlers; the base class is read from the file
    m = re.search(r"^struct OperatingProfitGraphWindow : ([A-Za-z_]\w*) \{\n", g, re.M)
    if not m:
        sys.exit("ERROR: struct OperatingProfitGraphWindow not found in graph_gui.cpp (anchor of the gold panel patch)")
    base = m.group(1)
    struct_start = m.end()
    nxt = re.search(r"^(?:struct|static|/\*\*|void) ", g[struct_start:], re.M)
    struct_end = struct_start + (nxt.start() if nxt else len(g) - struct_start)
    body = g[struct_start:struct_end]
    ctor = re.search(r"^([ \t]*)this->InitializeWindow\(window_number\);[^\n]*\n", body, re.M)
    if not ctor:
        sys.exit("ERROR: this->InitializeWindow(window_number); not found in OperatingProfitGraphWindow (anchor of the gold panel patch)")
    body = body[:ctor.end()] + ctor.group(1) + "this->ShowGold(true);\n" + body[ctor.end():]
    g = g[:struct_start] + GOLD_STRUCT_CODE.replace("@BASE@", base) + body + g[struct_end:]

    # 4. the layout: the colours are read from the file
    m = re.search(r"_nested_operating_profit_widgets\s*=\s*\{\n(.*?)\n\};", g, re.S)
    if not m:
        sys.exit("ERROR: _nested_operating_profit_widgets not found in graph_gui.cpp (anchor of the gold panel patch)")
    block = m.group(1)
    panel = re.search(r"NWidget\(WWT_PANEL, ([\w:]+), WID_GRAPH_BACKGROUND\)", block)
    graph = re.search(r"^([ \t]*)(NWidget\(WWT_EMPTY, ([\w:]+), WID_GRAPH_GRAPH\)[^\n]*\n)", block, re.M)
    key = re.search(r"^([ \t]*)NWidget\(WWT_PUSHTXTBTN, ([\w:]+), WID_GRAPH_KEY_BUTTON\)", block, re.M)
    if not (panel and graph and key):
        sys.exit("ERROR: the layout of the operating profit window is not what the gold panel patch expects (WID_GRAPH_BACKGROUND, WID_GRAPH_GRAPH, WID_GRAPH_KEY_BUTTON)")
    nested = (GOLD_NESTED_CODE.replace("@INVALID@", graph.group(3)).replace("@PANEL@", panel.group(1))
              .replace("@GRAPH_LINE@", "\t" + graph.group(1) + graph.group(2)))
    toggle = (f"{key.group(1)}NWidget(WWT_PUSHTXTBTN, {key.group(2)}, WID_PHG_GOLD_TOGGLE), SetMinimalSize(80, 0), "
              f"SetStringTip(STR_GOLD_TOGGLE, STR_GOLD_TIP_TOGGLE),\n")
    # the later position first, so that the earlier one stays valid
    new_block = block[:graph.start()] + nested + block[graph.end():]
    new_block = new_block[:key.start()] + toggle + new_block[key.start():]
    g = g[:m.start(1)] + new_block + g[m.end(1):]

    _write_like(gui, g, g_crlf)
    _write_like(wid, w, w_crlf)
    print(f"graph_gui.cpp, graph_widget.h: gold panel added (base class {base}, panel colour {panel.group(1)})")


# The operating profit graph window now holds the gold trading panel, so its name changes in every language:
# the menu entry and the caption of the window become "Gold Investment Types" (the Thai text was กราฟแสดงผลประกอบการ).
GOLD_WINDOW_NAME = "Gold Investment Types"
GOLD_NAME_KEYS = {"STR_GRAPH_MENU_OPERATING_PROFIT_GRAPH": "", "STR_GRAPH_OPERATING_PROFIT_CAPTION": "{WHITE}"}


def patch_graph_window_name(tree):
    """Renames the operating profit graph (menu entry and window caption) in english.txt and in every other language file."""
    lang = tree / "src" / "lang"
    if not lang.is_dir():
        sys.exit(f"ERROR: {lang} not found in the OpenTTD tree")
    changed = 0
    for f in sorted(lang.glob("*.txt")):
        text, crlf = _read_lf(f)
        new = text
        for key, prefix in GOLD_NAME_KEYS.items():
            new = re.sub(r"^(" + key + r"\s*:).*$", lambda m: m.group(1) + prefix + GOLD_WINDOW_NAME, new, flags=re.M)
        if new != text:
            _write_like(f, new, crlf)
            changed += 1
    if changed == 0:
        sys.exit("ERROR: STR_GRAPH_MENU_OPERATING_PROFIT_GRAPH / STR_GRAPH_OPERATING_PROFIT_CAPTION not found in any src/lang/*.txt")
    print(f"lang: the operating profit graph is now called '{GOLD_WINDOW_NAME}' ({changed} language files)")


CARGO_MARKER = "GOLD_CARGO_PATCH"


def patch_cargo_delivery(tree):
    """Gold and valuables that are delivered to a station feed the stock of the gold market: one call at the start of
    CargoPayment::PayFinalDelivery in economy.cpp (in place, at an anchor; nothing is written when the anchor is missing)."""
    target = tree / "src" / "economy.cpp"
    if not target.is_file():
        sys.exit(f"ERROR: {target} not found in the OpenTTD tree")
    text, crlf = _read_lf(target)
    if CARGO_MARKER in text:
        print("economy.cpp: gold delivery hook already there")
        return
    m = re.search(r"^void CargoPayment::PayFinalDelivery\(CargoType cargo, const CargoPacket \*cp, uint count, TileIndex current_tile\)\n\{\n", text, re.M)
    if not m:
        sys.exit("ERROR: CargoPayment::PayFinalDelivery not found in economy.cpp (anchor of the gold delivery patch)")
    text = text[:m.end()] + f"\tGoldFromDelivery(cargo, count); /* {CARGO_MARKER} */\n" + text[m.end():]
    text = ensure_include(text, '#include "gold_reserve.h" /* GOLD_CARGO_PATCH */', ['#include "central_bank.h"', '#include "stdafx.h"'])
    _write_like(target, text, crlf)
    print("economy.cpp: gold delivery hook added")


def main(tree):
    patch_graph_files(tree)
    patch_graph_window_name(tree)
    patch_cargo_delivery(tree)
    patch_console_file(tree)
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

    # economy.cpp: the fixed fee of OpenTTD (EXPENSES_OTHER, a quarter of the base station price) that is taken together with the loan interest.
    p = tree / "src" / "economy.cpp"
    s = p.read_text(encoding="utf-8")
    fee = "\t\tSubtractMoneyFromCompany(CommandCost(EXPENSES_OTHER, _price[PR_STATION_VALUE] >> 2));\n"
    if not REMOVE_FIXED_COMPANY_FEE:
        print("economy.cpp: the fixed company fee is kept")
    elif fee in s:
        s = s.replace(fee, "\t\t/* The fixed fee of OpenTTD is removed: the central bank interest is the only cost of money. */\n", 1)
        p.write_text(s, encoding="utf-8")
        print("economy.cpp: fixed company fee removed")
    else:
        print("economy.cpp: fixed company fee already removed (or not found)")

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
            add = "\n\t/**\n\t * Central Bank window; replaces the company finances window. %Window numbers:\n\t *   - 0 = the bank window (#CentralBankWidgets)\n\t *   - 1 = the account statement window\n\t */\n\tWC_CENTRAL_BANK,\n"
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

    # Script API: the AI companies (and game scripts) can read the debt of the central bank. The original loan functions of ScriptCompany stay as they are
    # (they only describe the loan that does not exist any more); these are the functions of the money system of the mod.
    hpp = tree / "src" / "script" / "api" / "script_company.hpp"
    cpp = tree / "src" / "script" / "api" / "script_company.cpp"
    if not hpp.exists() or not cpp.exists():
        print("WARNING: src/script/api/script_company.hpp/.cpp not found, the script API has no central bank functions")
    else:
        h = hpp.read_text(encoding="utf-8")
        c = cpp.read_text(encoding="utf-8")
        decl_rx = r"^([ \t]*)static Money GetLoanAmount\(\);[ \t]*\n"
        if "GetCentralBankDebt" in h and "GetCentralBankDebt" in c:
            print("script_company: central bank functions already present")
        elif not re.search(decl_rx, h, re.M):
            print("WARNING: ScriptCompany::GetLoanAmount not found in script_company.hpp, the script API has no central bank functions")
        else:
            decls = (
                "\n\t/**\n\t * Get the money that the company owes to the central bank (OpenTTD-Stock mod). The original loan does not exist in this game.\n"
                "\t * @param company The company to get the debt of.\n\t * @pre ResolveCompanyID(company) != COMPANY_INVALID.\n"
                "\t * @return The debt in the money of the game, or -1 when the company is not valid.\n\t */\n"
                "\tstatic Money GetCentralBankDebt(CompanyID company);\n"
                "\n\t/**\n\t * Get the credit that the central bank still lends to the company (OpenTTD-Stock mod): the credit limit minus the debt.\n"
                "\t * @param company The company to get the available credit of.\n\t * @pre ResolveCompanyID(company) != COMPANY_INVALID.\n"
                "\t * @return The available credit, or -1 when the company is not valid.\n\t */\n"
                "\tstatic Money GetCentralBankAvailable(CompanyID company);\n"
            )
            m = re.search(decl_rx, h, re.M)
            h = h[:m.end()] + decls + h[m.end():]
            impl = (
                "\n/* static */ Money ScriptCompany::GetCentralBankDebt(ScriptCompany::CompanyID company)\n{\n"
                "\tcompany = ResolveCompanyID(company);\n\tif (company == ScriptCompany::COMPANY_INVALID) return -1;\n\n"
                "\treturn ::GetCentralBankDebt(ScriptCompany::FromScriptCompanyID(company));\n}\n"
                "\n/* static */ Money ScriptCompany::GetCentralBankAvailable(ScriptCompany::CompanyID company)\n{\n"
                "\tcompany = ResolveCompanyID(company);\n\tif (company == ScriptCompany::COMPANY_INVALID) return -1;\n\n"
                "\treturn ::GetCentralBankAvailable(ScriptCompany::FromScriptCompanyID(company));\n}\n"
            )
            c = ensure_include(c, '#include "../../central_bank.h"', ['#include "../../company_base.h"', '#include "../../company_func.h"', '#include "script_company.hpp"', '#include "../../stdafx.h"'])
            c = c.rstrip("\n") + "\n" + impl
            hpp.write_text(h, encoding="utf-8")
            cpp.write_text(c, encoding="utf-8")
            print("script_company: GetCentralBankDebt and GetCentralBankAvailable added to the script API")

    print("overlay applied")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    main(Path(sys.argv[1]).resolve())

