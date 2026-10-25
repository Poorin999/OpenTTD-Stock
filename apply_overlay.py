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
  - town_gui.cpp, widgets/town_widget.h: the Finances button of the town window opens the town finance window (town_finance_gui.cpp)
  - ShowCompanyFinances() (company_gui.cpp), statusbar_gui.cpp: the finances menu and the statusbar button open ShowCentralBankWindow()
  - script_company.hpp/.cpp: ScriptCompany::GetCentralBankDebt and GetCentralBankAvailable, so that AIs can see the central bank debt;
    ScriptCompany::GetGoldHoldings and GetGoldPrice, so that AIs and game scripts can see the gold market
  - vehicle.cpp, vehicle_base.h, *_cmd.cpp: vehicles (train, road vehicle, ship, aircraft) do their daily work once per game day (patch_vehicle_time)
  - industries, towns, stations, subsidies, disasters: their day / month / year timers run on game time (patch_world_time)
  - timer_game_calendar.cpp, economy_settings.ini: the date of the original game follows the HH:MM clock (a new day starts at 00:00)
"""
import re
import sys
from pathlib import Path

# True: the game writes money_trace.log (next to openttd.exe) with every change of company cash, to find out where money goes.
# Set to False and build again to remove it.
MONEY_TRACE = False

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
    "tax.h": "src", "tax.cpp": "src",
    "cargo_market.h": "src", "cargo_market.cpp": "src", "CargoMarketMath.h": "src",
    "town_treasury.h": "src", "town_treasury.cpp": "src", "citizen_gold.h": "src", "citizen_terminal.h": "src", "gold_market_math.h": "src",
    "gold_reserve.h": "src", "gold_reserve.cpp": "src", "gold_industry.cpp": "src",
    "valuables.h": "src", "valuables.cpp": "src",
    "gold_panel.h": "src", "gold_panel.cpp": "src", "gold_buy_gui.cpp": "src",
    "town_finance_gui.h": "src", "town_finance_gui.cpp": "src",
    "economy_news.h": "src", "economy_news.cpp": "src",
    "market_events.h": "src", "market_events.cpp": "src", "market_news_ai.h": "src", "market_news_gui.h": "src", "market_news_gui.cpp": "src",
    "game_calendar.h": "src", "game_calendar.cpp": "src",
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
    ("CMD_WALLET_LEVERAGE", "set the leverage of a trading wallet"),
    ("CMD_BANK_BORROW", "borrow from the central bank"),
    ("CMD_BANK_REPAY", "repay the central bank"),
    ("CMD_GOLD_BUY", "buy gold for the reserve"),
    ("CMD_GOLD_SELL", "sell gold of the reserve"),
]
NEW_SOURCES = ["wallet_base.cpp", "wallet_base.h", "wallet_cmd.cpp", "wallet_cmd.h", "central_bank.cpp", "central_bank.h", "tax.cpp", "tax.h", "cargo_market.cpp", "cargo_market.h", "CargoMarketMath.h", "town_treasury.cpp", "town_treasury.h", "citizen_gold.h", "citizen_terminal.h", "gold_market_math.h", "gold_reserve.cpp", "gold_reserve.h", "gold_industry.cpp", "valuables.cpp", "valuables.h", "gold_panel.cpp", "gold_panel.h", "gold_buy_gui.cpp", "town_finance_gui.cpp", "town_finance_gui.h", "economy_news.cpp", "economy_news.h", "market_events.cpp", "market_events.h", "market_news_ai.h", "market_news_gui.cpp", "market_news_gui.h", "game_calendar.cpp", "game_calendar.h", "stock_console.cpp", "stock_dev.h",
               "central_bank_gui.cpp", "central_bank_gui.h",
               "RetailMath.h", "RetailMarketManager.h", "RetailMarketManager.cpp",
               "game_time/game_time_manager.h", "game_time/game_time_manager.cpp", "game_time/game_time_driver.cpp", "game_time/game_time_aggregator.h"]
REPLACE_KEYS = {"STR_STOCK_TIP_TOOLBOX", "STR_STOCK_TIP_SUMMARY", "STR_STOCK_ORDER_INFO", "STR_STOCK_TIP_DEPOSIT", "STR_STOCK_TIP_WITHDRAW"}
STRING_FILES = ["english_stock_strings.txt", "english_wallet_strings.txt", "english_round8_strings.txt", "english_timeframe_strings.txt", "english_central_bank_strings.txt", "english_gold_strings.txt", "english_town_finance_strings.txt", "english_valuables_strings.txt", "english_news_strings.txt"]
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
	/* GOLD_PANEL_PATCH: the gold trading panel is shown in the place of the profit graph, and the news panel (market_news_gui.cpp) in the place of the profit graph when the gold panel is switched off. */
	GoldPanel gold;         ///< The gold trading panel (state and actions).
	MarketNewsPanel news;   ///< The news panel (plane 2): the random events of the market, written by the news writer.
	bool show_gold = true;  ///< Whether the gold panel (plane 1) or the news panel (plane 2) is shown. The profit graph (plane 0) is not reachable any more.

	/** Redraw regularly: the gold price and the cash change all the time, and the news come by themselves. */
	const IntervalTimer<TimerWindow> gold_interval = {std::chrono::milliseconds(500), [this](auto) {
		this->SetDirty();
	}};

	/** Show the gold trading panel (true) or the news panel (false) in the main area of the window. */
	void ShowGold(bool show)
	{
		this->show_gold = show;
		this->GetWidget<NWidgetStacked>(WID_PHG_GOLD_SEL)->SetDisplayedPlane(show ? 1 : 2);
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
			case WID_PHG_GOLD_DIV: this->gold.DrawDivider(r); return;
			case WID_PHG_NEWS: this->news.Draw(r); return;
			default: break;
		}
		this->@BASE@::DrawWidget(r, widget);
	}

	void OnClick(Point pt, WidgetID widget, int click_count) override
	{
		switch (widget) {
			case WID_PHG_GOLD_TOGGLE: this->ShowGold(!this->show_gold); return;
			case WID_PHG_GOLD_INFO:
				/* The GOLD button of the header (panel / price chart) and the range buttons of the chart. */
				if (this->gold.ClickInfo(this->GetWidget<NWidgetBase>(WID_PHG_GOLD_INFO)->GetCurrentRect(), pt)) this->SetDirty();
				return;
			case WID_PHG_NEWS:
				if (this->news.Click(this->GetWidget<NWidgetBase>(WID_PHG_NEWS)->GetCurrentRect(), pt)) this->SetDirty();
				return;
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

GOLD_NESTED_CODE = r"""		/* GOLD_PANEL_PATCH: the main area is the profit graph (plane 0, not shown any more), the gold trading panel (plane 1) or the news panel (plane 2). All fill and resize with the window. */
		NWidget(NWID_SELECTION, @INVALID@, WID_PHG_GOLD_SEL),
@GRAPH_LINE@			NWidget(NWID_VERTICAL),
				NWidget(WWT_PANEL, @PANEL@, WID_PHG_GOLD_INFO), SetMinimalSize(576, 330), SetFill(1, 1), SetResize(1, 1),
				EndContainer(),
				NWidget(NWID_HORIZONTAL),
					NWidget(NWID_VERTICAL),
						NWidget(WWT_PANEL, @PANEL@, WID_PHG_GOLD_BUY_QTY), SetMinimalSize(150, 20), SetFill(1, 0), SetResize(1, 0), SetToolTip(STR_GOLD_TIP_QTY),
						EndContainer(),
						NWidget(WWT_PUSHTXTBTN, @PANEL@, WID_PHG_GOLD_BUY), SetMinimalSize(150, 24), SetFill(1, 0), SetResize(1, 0), SetPadding(0, 6, 6, 10), SetStringTip(STR_GOLD_BUY, STR_GOLD_TIP_BUY),
					EndContainer(),
					NWidget(WWT_PANEL, @PANEL@, WID_PHG_GOLD_DIV), SetMinimalSize(8, 0), SetFill(0, 1),
					EndContainer(),
					NWidget(NWID_VERTICAL),
						NWidget(WWT_PANEL, @PANEL@, WID_PHG_GOLD_SELL_QTY), SetMinimalSize(150, 20), SetFill(1, 0), SetResize(1, 0), SetToolTip(STR_GOLD_TIP_QTY),
						EndContainer(),
						NWidget(WWT_PUSHTXTBTN, @PANEL@, WID_PHG_GOLD_SELL), SetMinimalSize(150, 24), SetFill(1, 0), SetResize(1, 0), SetPadding(0, 10, 6, 6), SetStringTip(STR_GOLD_SELL, STR_GOLD_TIP_SELL),
					EndContainer(),
				EndContainer(),
			EndContainer(),
			NWidget(WWT_PANEL, @PANEL@, WID_PHG_NEWS), SetMinimalSize(576, 330), SetFill(1, 1), SetResize(1, 1),
			EndContainer(),
		EndContainer(),
"""

GOLD_WIDGET_IDS = """	WID_PHG_GOLD_SEL,       ///< Selection: the profit graph, the gold trading panel or the news panel (GOLD_PANEL_PATCH).
	WID_PHG_GOLD_TOGGLE,    ///< Button: switch between the gold panel and the news panel.
	WID_PHG_NEWS,           ///< News panel: the lead story and the list of the latest events.
	WID_PHG_GOLD_INFO,      ///< Gold panel: price, backing, credit standing and holdings.
	WID_PHG_GOLD_BUY_QTY,   ///< Gold panel: ounces to buy (click to type).
	WID_PHG_GOLD_BUY,       ///< Gold panel: Buy Gold button.
	WID_PHG_GOLD_SELL_QTY,  ///< Gold panel: ounces to sell (click to type).
	WID_PHG_GOLD_SELL,      ///< Gold panel: Sell Gold button.
	WID_PHG_GOLD_DIV,       ///< Gold panel: thin line between the Buy and the Sell side.
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
    g = g[:includes[-1].end()] + '#include "gold_panel.h" /* GOLD_PANEL_PATCH */\n#include "market_news_gui.h" /* GOLD_PANEL_PATCH */\n' + g[includes[-1].end():]

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


NEWS_MENU_KEY = "STR_GRAPH_MENU_INCOME_GRAPH"
NEWS_MENU_NAME = "Market News"
INCOME_NEWS_MARKER = "INCOME_NEWS_PATCH"


def patch_income_graph_to_news(tree):
    """The menu entry Graphs > Income graph becomes Market News: ShowIncomeGraph() opens the market news window (market_news_gui.cpp) and the
    entry is renamed in every language file. The income graph window itself stays in the code but can no longer be opened.
    In place, at an anchor; nothing is written when the anchor is missing."""
    target = tree / "src" / "graph_gui.cpp"
    if not target.is_file():
        sys.exit(f"ERROR: {target} not found in the OpenTTD tree")
    text, crlf = _read_lf(target)
    if INCOME_NEWS_MARKER in text:
        print("graph_gui.cpp: income graph already replaced by the news")
    else:
        m = re.search(r"^void ShowIncomeGraph\(\)\s*\{[^{}]*\}[ \t]*\n", text, re.M)
        if not m:
            sys.exit("ERROR: void ShowIncomeGraph() { ... } not found in graph_gui.cpp (anchor of the income graph / news patch)")
        body = f"void ShowIncomeGraph() /* {INCOME_NEWS_MARKER}: the old income graph window can not be opened any more */\n{{\n\tShowMarketNewsWindow();\n}}\n"
        text = text[:m.start()] + body + text[m.end():]
        if '#include "market_news_gui.h"' not in text:
            includes = list(re.finditer(r'^#include "[^"\n]+"[^\n]*\n', text, re.M))
            if not includes:
                sys.exit("ERROR: no #include found in graph_gui.cpp (anchor of the income graph / news patch)")
            text = text[:includes[-1].end()] + f'#include "market_news_gui.h" /* {INCOME_NEWS_MARKER} */\n' + text[includes[-1].end():]
        _write_like(target, text, crlf)
        print("graph_gui.cpp: ShowIncomeGraph() now opens the market news window")

    lang = tree / "src" / "lang"
    changed = 0
    found = 0
    for f in sorted(lang.glob("*.txt")):
        t, c = _read_lf(f)
        if re.search(r"^" + NEWS_MENU_KEY + r"\s*:", t, re.M): found += 1
        new = re.sub(r"^(" + NEWS_MENU_KEY + r"\s*:).*$", lambda mm: mm.group(1) + NEWS_MENU_NAME, t, flags=re.M)
        if new != t:
            _write_like(f, new, c)
            changed += 1
    if found == 0:
        print(f"WARNING: {NEWS_MENU_KEY} not found in any src/lang/*.txt: the menu entry keeps the old name")
    else:
        print(f"lang: the income graph menu entry is now called '{NEWS_MENU_NAME}' ({changed} language files)")


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


CARGO_MARKET_MARKER = "CARGO_MARKET_PATCH"


def patch_cargo_market(tree):
    """The price of cargo follows supply and demand (cargo_market.cpp). Two hooks in economy.cpp, in place, at anchors (nothing is written when one is missing):
    (1) the first line of CargoPayment::PayFinalDelivery counts the delivered units (supply);
    (2) GetTransportedGoodsIncome (the income that OpenTTD calculates for a delivery; the transfers, the final payment and the
        income estimates of the windows all use it) is renamed to GetTransportedGoodsIncomeUnscaled and a wrapper with the old name multiplies it by the price of the cargo."""
    p = tree / "src" / "economy.cpp"
    t, crlf = _read_lf(p)
    if CARGO_MARKET_MARKER in t:
        print("economy.cpp: cargo market hooks already there")
        return
    m = re.search(r"^void CargoPayment::PayFinalDelivery\(CargoType cargo, const CargoPacket \*cp, uint count, TileIndex current_tile\)\n\{\n", t, re.M)
    if not m:
        sys.exit("ERROR: CargoPayment::PayFinalDelivery not found in economy.cpp (anchor of the cargo market patch)")
    t = t[:m.end()] + f"\tCargoMarketOnDelivery(cargo, count); /* {CARGO_MARKET_MARKER} */\n" + t[m.end():]

    m = re.search(r"^Money GetTransportedGoodsIncome\(([^)]*)\)\n\{\n", t, re.M)
    if not m:
        sys.exit("ERROR: Money GetTransportedGoodsIncome(...) not found in economy.cpp (anchor of the cargo market patch)")
    # The body is not touched (its returns differ between versions): the original function gets a new name and a small
    # function with the old name and the same parameters is put after it. It calls the original and multiplies the result by the price.
    params = [x.strip() for x in re.split(r",", m.group(1).replace("\n", " ")) if x.strip()]
    names = []
    for prm in params:
        nm = re.search(r"(\w+)\s*(?:=.*)?$", prm)
        if not nm:
            sys.exit("ERROR: cannot read the parameters of GetTransportedGoodsIncome (cargo market patch)")
        names.append(nm.group(1))
    cargo_param = next((re.search(r"(\w+)\s*(?:=.*)?$", prm).group(1) for prm in params if re.search(r"\bCargoType\b", prm)), None)
    if not names or cargo_param is None:
        sys.exit("ERROR: GetTransportedGoodsIncome has no CargoType parameter (cargo market patch)")
    end = t.find("\n}\n", m.end())
    if end < 0:
        sys.exit("ERROR: end of GetTransportedGoodsIncome not found (cargo market patch)")
    end += 3  # just after the closing brace line
    wrapper = (f"\n/* {CARGO_MARKET_MARKER}: the income of a delivery times the price of the cargo (cargo_market.cpp). */\n"
               f"Money GetTransportedGoodsIncome({m.group(1)})\n{{\n"
               f"\treturn CargoMarketAdjustIncome({cargo_param}, GetTransportedGoodsIncomeUnscaled({', '.join(names)}));\n}}\n")
    t = t[:end] + wrapper + t[end:]
    t = t[:m.start()] + "static Money GetTransportedGoodsIncomeUnscaled(" + t[m.start() + len("Money GetTransportedGoodsIncome("):]
    t = ensure_include(t, '#include "cargo_market.h" /* CARGO_MARKET_PATCH */', ['#include "central_bank.h"', '#include "stdafx.h"'])
    _write_like(p, t, crlf)
    print("economy.cpp: cargo market hooks added")


VEHICLE_TIME_MARKER = "GAME_VEHICLE_TIME_PATCH"


def patch_vehicle_time(tree):
    """Vehicles (trains, road vehicles, ships, aircraft) follow the clock of the mod. In place, at anchors; nothing is written when an anchor is missing.
      - vehicle_base.h: GameServiceDate(), the game day as a date (used for the service date).
      - vehicle.cpp: the daily work of every vehicle (age, running costs, reliability, breakdown check, service check, orders check) runs once per
        GAME day (24 real minutes) instead of once per economy day (74 ticks). It is spread over the whole day, 1/48000 of the vehicles per step.
        The yearly profit window of the vehicles closes with the game year (Year event of the clock).
      - the service date (date_of_last_service) is stamped and compared in game days: vehicle.cpp, train_cmd.cpp, roadveh_cmd.cpp, ship_cmd.cpp,
        aircraft_cmd.cpp, vehicle_gui.cpp, station_cmd.cpp.
    Running costs are paid per tick of movement (running_ticks), so the total is the same; only the day on which they are booked changes."""
    src = tree / "src"
    # vehicle_base.h
    p = src / "vehicle_base.h"
    t, crlf = _read_lf(p)
    if VEHICLE_TIME_MARKER not in t:
        if '#include "timer/timer_game_calendar.h"\n' not in t:
            sys.exit("ERROR: vehicle_base.h has a different layout (vehicle time patch)")
        t = ensure_include(t, '#include "timer/timer_game_economy.h"', ['#include "timer/timer_game_calendar.h"'])
        helper = ('\n/* ' + VEHICLE_TIME_MARKER + ' */\n/** The game day (the calendar day of the mod clock) as a date. It is the date of the service of vehicles: one day is 24 real minutes. */\n'
                  'inline TimerGameEconomy::Date GameServiceDate()\n{\n\treturn TimerGameEconomy::Date(TimerGameCalendar::date.base());\n}\n')
        m = re.search(r'#include "timer/timer_game_economy.h"\n', t)
        t = t[:m.end()] + helper + t[m.end():]
        _write_like(p, t, crlf)
        print("vehicle_base.h: GameServiceDate added")
    else:
        print("vehicle_base.h: vehicle time already patched")

    # vehicle.cpp
    p = src / "vehicle.cpp"
    t, crlf = _read_lf(p)
    if VEHICLE_TIME_MARKER not in t:
        loop = "\tfor (size_t i = TimerGameEconomy::date_fract; i < Vehicle::GetPoolSize(); i += Ticks::DAY_TICKS) {\n"
        func_head = "static void RunEconomyVehicleDayProc()\n{\n"
        if t.count(loop) != 1 or t.count(func_head) != 1:
            sys.exit("ERROR: vehicle.cpp has a different layout, RunEconomyVehicleDayProc was not connected to the game clock")
        helper = (
            "/* " + VEHICLE_TIME_MARKER + " */\n"
            "/** A game day is split in this many steps; each step handles the vehicles with (pool index % steps) == step. */\n"
            "static constexpr uint32_t GAME_VEHICLE_DAY_STEPS = 48000;\n\n"
            "/**\n * The pool indices of the vehicles whose daily work is due now. The clock of the mod decides: normally one step per tick (a step is 1/48000 of a\n"
            " * game day), so each vehicle is handled once per game day. After a jump of the clock (load, skip of the test tools) only the current step is handled.\n */\n"
            "static std::vector<size_t> GameVehicleDayIndices()\n{\n"
            "\tstatic uint32_t last_step = GAME_VEHICLE_DAY_STEPS; // GAME_VEHICLE_DAY_STEPS: no step handled yet\n"
            "\tconst uint32_t now = GameTimeManager::Get().GetDayFraction(GAME_VEHICLE_DAY_STEPS);\n"
            "\tstd::vector<uint32_t> steps;\n"
            "\tconst uint32_t behind = last_step >= GAME_VEHICLE_DAY_STEPS ? GAME_VEHICLE_DAY_STEPS : (now + GAME_VEHICLE_DAY_STEPS - last_step) % GAME_VEHICLE_DAY_STEPS;\n"
            "\tif (behind >= 1000) {\n\t\tsteps.push_back(now); // first call, load or jump\n\t} else {\n"
            "\t\tfor (uint32_t k = 1; k <= behind; k++) steps.push_back((last_step + k) % GAME_VEHICLE_DAY_STEPS);\n\t}\n"
            "\tlast_step = now;\n\n"
            "\tstd::vector<size_t> result;\n"
            "\tfor (uint32_t step : steps) {\n"
            "\t\tfor (size_t i = step; i < Vehicle::GetPoolSize(); i += GAME_VEHICLE_DAY_STEPS) result.push_back(i);\n\t}\n"
            "\treturn result;\n}\n\n")
        t = t.replace(func_head, helper + func_head, 1)
        t = t.replace(loop, "\tfor (size_t i : GameVehicleDayIndices()) { /* " + VEHICLE_TIME_MARKER + " */\n", 1)
        old_timer = "static const IntervalTimer<TimerGameEconomy> _economy_vehicles_yearly({TimerGameEconomy::YEAR, TimerGameEconomy::Priority::VEHICLE}, [](auto)\n{\n"
        if t.count(old_timer) != 1:
            sys.exit("ERROR: vehicle.cpp has a different layout, the yearly profit of vehicles was not connected to the game clock")
        t = t.replace(old_timer, "static const GameTimeSubscription _economy_vehicles_yearly(GameTimeEvent::Year, [](const GameTimeSnapshot &)\n{\n", 1)
        n = 0
        for old, new in (
            ("(this->GetServiceInterval() * EconomyTime::DAYS_IN_ECONOMY_MONTH) >= TimerGameEconomy::date)", "(this->GetServiceInterval() * EconomyTime::DAYS_IN_ECONOMY_MONTH) >= GameServiceDate())"),
            ("this->date_of_last_service + this->GetServiceInterval() >= TimerGameEconomy::date)", "this->date_of_last_service + this->GetServiceInterval() >= GameServiceDate())"),
        ):
            if t.count(old) != 1:
                sys.exit("ERROR: vehicle.cpp NeedsServicing has a different layout (vehicle time patch): " + old[:40])
            t = t.replace(old, new, 1)
        t, n = re.subn(r"(\w+)->date_of_last_service = TimerGameEconomy::date;", r"\1->date_of_last_service = GameServiceDate();", t)
        t = ensure_include(t, '#include "game_time/game_time_manager.h"', ['#include "vehicle_func.h"', '#include "stdafx.h"'])
        if "#include <vector>" not in t:
            t = ensure_include(t, "#include <vector>", ['#include "stdafx.h"'])
        _write_like(p, t, crlf)
        print(f"vehicle.cpp: the daily work of vehicles follows the game clock ({n} service stamp)")
    else:
        print("vehicle.cpp: vehicle time already patched")

    # the other places that stamp or read the service date
    for name in ("train_cmd.cpp", "roadveh_cmd.cpp", "ship_cmd.cpp", "aircraft_cmd.cpp"):
        p = src / name
        t, crlf = _read_lf(p)
        t2, n = re.subn(r"(\w+)->date_of_last_service = TimerGameEconomy::date;", r"\1->date_of_last_service = GameServiceDate();", t)
        if n:
            _write_like(p, t2, crlf)
        print(f"{name}: {n} service stamp(s) use the game day")
    for name, old, new in (
        ("vehicle_gui.cpp", "(TimerGameEconomy::date - v->date_of_last_service)", "(GameServiceDate() - v->date_of_last_service)"),
        ("station_cmd.cpp", "TimerGameEconomy::date - v->date_of_last_service <=", "GameServiceDate() - v->date_of_last_service <="),
    ):
        p = src / name
        t, crlf = _read_lf(p)
        if old in t:
            _write_like(p, t.replace(old, new, 1), crlf)
            print(f"{name}: service date read in game days")
        elif new in t:
            print(f"{name}: already patched")
        else:
            sys.exit(f"ERROR: {name} has a different layout (vehicle time patch)")


WORLD_TIME_MARKER = "GAME_WORLD_TIME_PATCH"

# (file, name of the timer variable, economy period, clock event)
WORLD_TIMERS = [
    ("industry_cmd.cpp", "_economy_industries_daily", "DAY", "Day"),
    ("industry_cmd.cpp", "_economy_industries_monthly", "MONTH", "Month"),
    ("town_cmd.cpp", "_economy_towns_monthly", "MONTH", "Month"),
    ("town_cmd.cpp", "_economy_towns_yearly", "YEAR", "Year"),
    ("station_cmd.cpp", "_economy_stations_monthly", "MONTH", "Month"),
    ("subsidy.cpp", "_economy_subsidies_monthly", "MONTH", "Month"),
    ("disaster_vehicle.cpp", "_economy_disaster_daily", "DAY", "Day"),
]


def patch_world_time(tree):
    """Industries, towns, stations, subsidies and disasters keep their own time by the clock of the mod: the timers that OpenTTD runs on
    economy days / months / years (74 ticks, 30 days, 12 months) run on game days (24 real minutes), game months (30 game days) and game years
    (365 game days). In place: the declaration of each timer is replaced by a GameTimeSubscription with the same body.
    NOT moved (they handle money, which still flows per tick): the company timers of economy.cpp and company_cmd.cpp (infrastructure costs, interest,
    statistics), and the network server timers."""
    src = tree / "src"
    for name, var, period, event in WORLD_TIMERS:
        p = src / name
        t, crlf = _read_lf(p)
        old = re.search(r"static const IntervalTimer<TimerGameEconomy> " + var + r"\(\{TimerGameEconomy::" + period + r", TimerGameEconomy::Priority::\w+\}, \[\]\(auto\)\n", t)
        new = f"static const GameTimeSubscription {var}(GameTimeEvent::{event}, [](const GameTimeSnapshot &) /* {WORLD_TIME_MARKER} */\n"
        if not old:
            if var in t and "GameTimeSubscription " + var in t:
                print(f"{name}: {var} already on game time")
                continue
            sys.exit(f"ERROR: {name}: timer {var} not found (world time patch)")
        t = t[:old.start()] + new + t[old.end():]
        t = ensure_include(t, '#include "game_time/game_time_manager.h"', ['#include "stdafx.h"'])
        _write_like(p, t, crlf)
        print(f"{name}: {var} runs on the game {event.lower()}")


COMPANY_TIME_MARKER = "GAME_COMPANY_TIME_PATCH"


def patch_company_hourly(tree):
    """The monthly work of the companies in economy.cpp (CompaniesGenStatistics: infrastructure costs, bankruptcy check; CompaniesPayInterest;
    HandleEconomyFluctuations) ran on the economy month (30 economy days = about 60 real seconds). It now runs on the Hour event of the clock
    (every game hour = 60 real seconds at normal speed), so the money that it books follows the game clock and the amount per real time is the same."""
    p = tree / "src" / "economy.cpp"
    t, crlf = _read_lf(p)
    if COMPANY_TIME_MARKER in t:
        print("economy.cpp: company monthly work already on the game hour")
        return
    old = re.search(r"static const IntervalTimer<TimerGameEconomy> _economy_companies_monthly\(\{ ?TimerGameEconomy::MONTH, TimerGameEconomy::Priority::COMPANY ?\}, \[\]\(auto\)\n", t)
    if not old:
        sys.exit("ERROR: economy.cpp: _economy_companies_monthly not found (company time patch)")
    t = t[:old.start()] + f"static const GameTimeSubscription _economy_companies_monthly(GameTimeEvent::Hour, [](const GameTimeSnapshot &) /* {COMPANY_TIME_MARKER} */\n" + t[old.end():]
    t = ensure_include(t, '#include "game_time/game_time_manager.h"', ['#include "company_func.h"', '#include "stdafx.h"'])
    _write_like(p, t, crlf)
    print("economy.cpp: company monthly work runs on the game hour")


VAT_MARKER = "GAME_VAT_PATCH"


def patch_vat(tree):
    """VAT on the income of cargo delivery: one call in CargoPayment::~CargoPayment (economy.cpp), right after the income is paid to the company (in place, at an anchor)."""
    p = tree / "src" / "economy.cpp"
    t, crlf = _read_lf(p)
    if VAT_MARKER in t:
        print("economy.cpp: VAT hook already there")
        return
    anchor = "\tSubtractMoneyFromCompany(CommandCost(this->front->GetExpenseType(true), -this->route_profit));\n"
    if t.count(anchor) != 1:
        sys.exit("ERROR: economy.cpp: CargoPayment::~CargoPayment has a different layout (VAT patch)")
    t = t.replace(anchor, anchor + f"\tChargeVat(this->front->owner, static_cast<int64_t>(this->route_profit)); /* {VAT_MARKER} */\n", 1)
    t = ensure_include(t, '#include "tax.h"', ['#include "central_bank.h"', '#include "stdafx.h"'])
    _write_like(p, t, crlf)
    print("economy.cpp: VAT hook added")


GOLD_INDUSTRY_MARKER = "GOLD_INDUSTRY_PATCH"


def patch_gold_industries(tree):
    """Gold mines in every climate and banks that take the gold cargo (gold_industry.cpp): one declaration before ResetIndustries() and one call at
    the end of it, in industry_cmd.cpp (in place, at an anchor; nothing is written when the anchor is missing)."""
    p = tree / "src" / "industry_cmd.cpp"
    if not p.is_file():
        sys.exit(f"ERROR: {p} not found in the OpenTTD tree")
    t, crlf = _read_lf(p)
    if GOLD_INDUSTRY_MARKER in t:
        print("industry_cmd.cpp: gold industries hook already there")
        return
    m = re.search(r"^void ResetIndustries\(\)\n\{\n", t, re.M)
    if not m:
        sys.exit("ERROR: void ResetIndustries() not found in industry_cmd.cpp (anchor of the gold industries patch)")
    end = t.find("\n}\n", m.end())
    if end < 0:
        sys.exit("ERROR: end of ResetIndustries() not found (gold industries patch)")
    t = t[:end + 1] + f"\tApplyGoldIndustries(); /* {GOLD_INDUSTRY_MARKER} */\n" + t[end + 1:]
    t = t[:m.start()] + f"void ApplyGoldIndustries(); /* {GOLD_INDUSTRY_MARKER}: gold_industry.cpp */\n\n" + t[m.start():]
    _write_like(p, t, crlf)
    print("industry_cmd.cpp: gold industries hook added")


COMPANY_YEAR_MARKER = "GAME_COMPANY_YEAR_PATCH"


def patch_company_yearly(tree):
    """The yearly work of the companies (CompaniesYearlyLoop: the yearly_expenses table is shifted by one year, so 'This Year' and 'Last Year' of the
    account statement) ran on the economy year of OpenTTD (365 economy days of 74 ticks, about 13 real minutes). It now runs on the Year event of
    the clock (365 game days), the same year as the ledger lines and the dates of the statement. In place, at an anchor: the one timer
    'IntervalTimer<TimerGameEconomy> ... {TimerGameEconomy::YEAR, TimerGameEconomy::Priority::COMPANY}' in src/*.cpp; nothing is written when
    it is not found exactly once."""
    rx = re.compile(r"static const IntervalTimer<TimerGameEconomy> (\w+)\(\{ ?TimerGameEconomy::YEAR, TimerGameEconomy::Priority::COMPANY ?\}, \[\]\(auto\)")
    hits = []
    for f in sorted((tree / "src").glob("*.cpp")):
        t, crlf = _read_lf(f)
        if COMPANY_YEAR_MARKER in t:
            print(f"{f.name}: company yearly work already on the game year")
            return
        m = rx.search(t)
        if m:
            hits.append((f, t, crlf, m))
    if len(hits) != 1:
        sys.exit(f"ERROR: the yearly company timer (YEAR, Priority::COMPANY) was found {len(hits)} times in src/*.cpp, expected 1 (company yearly patch): {[h[0].name for h in hits]}")
    f, t, crlf, m = hits[0]
    new = f"static const GameTimeSubscription {m.group(1)}(GameTimeEvent::Year, [](const GameTimeSnapshot &) /* {COMPANY_YEAR_MARKER} */"
    t = t[:m.start()] + new + t[m.end():]
    t = ensure_include(t, '#include "game_time/game_time_manager.h"', ['#include "company_func.h"', '#include "stdafx.h"'])
    _write_like(f, t, crlf)
    print(f"{f.name}: company yearly work ({m.group(1)}) runs on the game year")


TOWN_FINANCE_MARKER = "TOWN_FINANCE_PATCH"


def patch_town_gui(tree):
    """Adds the Finances button to the town window: widget WID_TV_FINANCE (widgets/town_widget.h), the button after the cargo graph button
    of the game layout, the click handler and the include (town_gui.cpp). It opens ShowTownFinanceWindow() of town_finance_gui.cpp."""
    gui = tree / "src" / "town_gui.cpp"
    wid = tree / "src" / "widgets" / "town_widget.h"
    for f in (gui, wid):
        if not f.is_file():
            sys.exit(f"ERROR: {f} not found in the OpenTTD tree")
    g, g_crlf = _read_lf(gui)
    w, w_crlf = _read_lf(wid)
    if TOWN_FINANCE_MARKER in g and "WID_TV_FINANCE" in w:
        print("town_gui.cpp, town_widget.h: finance button already there")
        return
    if TOWN_FINANCE_MARKER in g or "WID_TV_FINANCE" in w:
        sys.exit("ERROR: only one of town_gui.cpp and town_widget.h has the finance button patch; restore both from OpenTTD and run again")

    # 1. the widget number (plain ASCII comment, see ROADMAP rule 1)
    m = re.search(r"^([ \t]*)WID_TV_GRAPH,[^\n]*\n", w, re.M)
    if not m:
        sys.exit("ERROR: WID_TV_GRAPH not found in widgets/town_widget.h (anchor of the finance button patch)")
    w = w[:m.end()] + m.group(1) + "WID_TV_FINANCE, ///< Show the finance window of this town.\n" + w[m.end():]

    # 2. the include
    incs = list(re.finditer(r'^#include "widgets/town_widget\.h"[^\n]*\n', g, re.M))
    if not incs:
        sys.exit('ERROR: #include "widgets/town_widget.h" not found in town_gui.cpp (anchor of the finance button patch)')
    g = g[:incs[-1].end()] + '#include "town_finance_gui.h" /* TOWN_FINANCE_PATCH */\n' + g[incs[-1].end():]

    # 3. the click handler, in front of the cargo graph case
    m = re.search(r"^([ \t]*)case WID_TV_GRAPH: \{\n", g, re.M)
    if not m:
        sys.exit("ERROR: case WID_TV_GRAPH not found in town_gui.cpp (anchor of the finance button patch)")
    ind = m.group(1)
    g = g[:m.start()] + f"{ind}case WID_TV_FINANCE: // TOWN_FINANCE_PATCH\n{ind}\tShowTownFinanceWindow(static_cast<uint32_t>(this->window_number));\n{ind}\tbreak;\n\n" + g[m.start():]

    # 4. the button: after the cargo graph button of the game layout (the scenario editor layout has none)
    btns = list(re.finditer(r"^([ \t]*)NWidget\(WWT_PUSHTXTBTN, ([\w:]+), WID_TV_GRAPH\)[^\n]*\n", g, re.M))
    if len(btns) != 1:
        sys.exit(f"ERROR: expected one WID_TV_GRAPH button in town_gui.cpp, found {len(btns)} (anchor of the finance button patch)")
    b = btns[0]
    line = (f"{b.group(1)}NWidget(WWT_PUSHTXTBTN, {b.group(2)}, WID_TV_FINANCE), SetFill(1, 0), SetResize(1, 0), "
            f"SetStringTip(STR_TOWN_FINANCE_BUTTON, STR_TOWN_FINANCE_TIP),\n")
    g = g[:b.end()] + line + g[b.end():]

    _write_like(gui, g, g_crlf)
    _write_like(wid, w, w_crlf)
    print("town_gui.cpp, town_widget.h: Finances button added to the town window")


def main(tree):
    patch_graph_files(tree)
    patch_town_gui(tree)
    patch_graph_window_name(tree)
    patch_income_graph_to_news(tree)
    patch_cargo_delivery(tree)
    patch_cargo_market(tree)
    patch_vat(tree)
    patch_vehicle_time(tree)
    patch_world_time(tree)
    patch_company_hourly(tree)
    patch_company_yearly(tree)
    patch_gold_industries(tree)
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

    # statusbar_gui.cpp: the clock panel of the mod (weekday badge, HH:MM, date: game_calendar.cpp) replaces the date text, and the panel is wider.
    p = tree / "src" / "statusbar_gui.cpp"
    s = p.read_text(encoding="utf-8")
    if "GameTimeManager" in s:
        print("statusbar_gui.cpp: the time is already shown")
    else:
        edits = [
            ("d = GetStringBoundingBox(GetString(STR_JUST_DATE_LONG, GetParamMaxValue(TimerGameCalendar::DateAtStartOfYear(CalendarTime::MAX_YEAR).base())));",
             "d = GetStringBoundingBox(GetString(STR_JUST_DATE_LONG, GetParamMaxValue(TimerGameCalendar::DateAtStartOfYear(CalendarTime::MAX_YEAR).base())));\n\t\t\t\td.width += GameClockExtraWidth(); // the weekday badge, HH:MM and the gaps in front of the date"),
            ("DrawString(tr, GetString(STR_JUST_DATE_LONG, TimerGameCalendar::date), TC_WHITE, SA_HOR_CENTER);",
             "DrawGameClock(tr, GetString(STR_JUST_DATE_LONG, TimerGameCalendar::date));"),
            ("NWidget(WWT_PANEL, COLOUR_GREY, WID_S_LEFT), SetMinimalSize(140, 12), EndContainer(),",
             "NWidget(WWT_PANEL, COLOUR_GREY, WID_S_LEFT), SetMinimalSize(300, 12), EndContainer(),"),
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
        s = ensure_include(s, '#include "game_calendar.h"', ['#include "game_time/game_time_manager.h"'])
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

    # Script API: the gold market (OpenTTD-Stock mod). Same anchors and style as the central bank functions above.
    if hpp.exists() and cpp.exists():
        h = hpp.read_text(encoding="utf-8")
        c = cpp.read_text(encoding="utf-8")
        decl_rx = r"^([ \t]*)static Money GetLoanAmount\(\);[ \t]*\n"
        if "GetGoldHoldings" in h and "GetGoldHoldings" in c:
            print("script_company: gold functions already present")
        elif not re.search(decl_rx, h, re.M):
            print("WARNING: ScriptCompany::GetLoanAmount not found in script_company.hpp, the script API has no gold functions")
        else:
            decls = (
                "\n\t/**\n\t * Get the ounces of gold that the company holds in its vault (OpenTTD-Stock mod).\n"
                "\t * @param company The company to get the gold of.\n\t * @pre ResolveCompanyID(company) != COMPANY_INVALID.\n"
                "\t * @return The ounces of gold, or -1 when the company is not valid.\n\t */\n"
                "\tstatic SQInteger GetGoldHoldings(CompanyID company);\n"
                "\n\t/**\n\t * Get the price of one ounce of gold (OpenTTD-Stock mod): the middle price of the gold market, in the money of the game.\n"
                "\t * @return The price of one ounce, or -1 when the gold market has not started yet.\n\t */\n"
                "\tstatic Money GetGoldPrice();\n"
            )
            m = re.search(decl_rx, h, re.M)
            h = h[:m.end()] + decls + h[m.end():]
            impl = (
                "\n/* static */ SQInteger ScriptCompany::GetGoldHoldings(ScriptCompany::CompanyID company)\n{\n"
                "\tcompany = ResolveCompanyID(company);\n\tif (company == ScriptCompany::COMPANY_INVALID) return -1;\n\n"
                "\treturn static_cast<SQInteger>(::_gold_reserve.GetHoldings(ScriptCompany::FromScriptCompanyID(company)));\n}\n"
                "\n/* static */ Money ScriptCompany::GetGoldPrice()\n{\n"
                "\tif (::_gold_reserve.GetPriceCents() <= 0) return -1;\n"
                "\treturn Money(::_gold_reserve.GetPriceCents() / 100);\n}\n"
            )
            c = ensure_include(c, '#include "../../gold_reserve.h"', ['#include "../../central_bank.h"', '#include "../../company_base.h"', '#include "../../company_func.h"', '#include "script_company.hpp"', '#include "../../stdafx.h"'])
            c = c.rstrip("\n") + "\n" + impl
            hpp.write_text(h, encoding="utf-8")
            cpp.write_text(c, encoding="utf-8")
            print("script_company: GetGoldHoldings and GetGoldPrice added to the script API")

    # Script API: borrow from and repay the central bank (OpenTTD-Stock mod). AIs such as StockHoge had no way to take a loan, only to read the debt.
    if hpp.exists() and cpp.exists():
        h = hpp.read_text(encoding="utf-8")
        c = cpp.read_text(encoding="utf-8")
        decl_rx = r"^([ \t]*)static Money GetLoanAmount\(\);[ \t]*\n"
        if "BorrowFromCentralBank" in h and "BorrowFromCentralBank" in c:
            print("script_company: borrow functions already present")
        elif not re.search(decl_rx, h, re.M):
            print("WARNING: ScriptCompany::GetLoanAmount not found in script_company.hpp, the script API has no borrow functions")
        else:
            decls = (
                "\n\t/**\n\t * Borrow money from the central bank (OpenTTD-Stock mod). The money is paid into the cash of the company.\n"
                "\t * @param amount The money to borrow, more than 0 and at most GetCentralBankAvailable.\n\t * @return True when the loan was paid out.\n\t */\n"
                "\tstatic bool BorrowFromCentralBank(Money amount);\n"
                "\n\t/**\n\t * Repay a loan of the central bank from the cash of the company (OpenTTD-Stock mod).\n"
                "\t * @param amount The money to repay, more than 0 and at most GetCentralBankDebt.\n\t * @return True when the money was repaid.\n\t */\n"
                "\tstatic bool RepayCentralBank(Money amount);\n"
            )
            m = re.search(decl_rx, h, re.M)
            h = h[:m.end()] + decls + h[m.end():]
            impl = (
                "\n/* static */ bool ScriptCompany::BorrowFromCentralBank(Money amount)\n{\n"
                "\tEnforceCompanyModeValid(false);\n\tEnforcePrecondition(false, amount > 0);\n\n"
                "\treturn ScriptObject::Command<CMD_BANK_BORROW>::Do(0 /* WALLET_CASH */, amount);\n}\n"
                "\n/* static */ bool ScriptCompany::RepayCentralBank(Money amount)\n{\n"
                "\tEnforceCompanyModeValid(false);\n\tEnforcePrecondition(false, amount > 0);\n\n"
                "\treturn ScriptObject::Command<CMD_BANK_REPAY>::Do(0 /* WALLET_CASH */, amount);\n}\n"
            )
            c = ensure_include(c, '#include "../../central_bank.h"', ['#include "../../company_base.h"', '#include "../../company_func.h"', '#include "script_company.hpp"', '#include "../../stdafx.h"'])
            c = c.rstrip("\n") + "\n" + impl
            hpp.write_text(h, encoding="utf-8")
            cpp.write_text(c, encoding="utf-8")
            print("script_company: BorrowFromCentralBank and RepayCentralBank added to the script API")

    print("overlay applied")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    main(Path(sys.argv[1]).resolve())

