/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file stock_gui.cpp The trading terminal: Market Watch, candlestick chart and the toolbox with open trades. */

#include "stdafx.h"
#include <cmath>
#include <optional>
#include "3rdparty/fmt/format.h"
#include "stock_gui.h"
#include "stock_base.h"
#include "stock_cmd.h"
#include "wallet_cmd.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "currency.h"
#include "dropdown_func.h"
#include "dropdown_type.h"
#include "gfx_func.h"
#include "palette_func.h"
#include "strings_func.h"
#include "textbuf_gui.h"
#include "window_func.h"
#include "window_gui.h"
#include "timer/timer_game_economy.h"
#include "zoom_func.h"

#include "widgets/stock_widget.h"
#include "central_bank.h"
#include "valuables.h"
#include "game_time/game_time_manager.h"

#include "table/strings.h"

#include "safeguards.h"

/**
 * Widget number of the Close All button. It has this name in this file; the number is defined in widgets/stock_widget.h,
 * together with the wallet widgets that follow it.
 */
static constexpr WidgetID WID_STOCK_CLOSE_ALL = WID_ST_CLOSE_ALL;

/**
 * Read a number the player typed. Both '.' and ',' are accepted as decimal separator.
 * @param text The text of an edit box or query window.
 * @param[out] value The parsed value.
 * @return True when the text holds a valid number, false when it is empty or invalid.
 */
bool ParseStockNumber(std::string_view text, double &value)
{
	std::string s(text);
	for (char &c : s) {
		if (c == ',') c = '.';
	}
	const size_t begin = s.find_first_not_of(' ');
	if (begin == std::string::npos) return false;
	s = s.substr(begin);

	char *end = nullptr;
	value = std::strtod(s.c_str(), &end);
	if (end == s.c_str()) return false;
	while (*end == ' ') end++;
	return *end == '\0' && std::isfinite(value);
}

/** The exchange rate of the display currency; never below 1, so that the conversions below are exact inverses of each other. */
static double GetStockCurrencyRate()
{
	return std::max<double>(1.0, GetCurrency().rate);
}

/** Convert a price the player typed (in the display currency) to internal money. */
int64_t StockDisplayToMoney(double value)
{
	return static_cast<int64_t>(std::llround(value / GetStockCurrencyRate()));
}

/** Convert internal money to a number in the display currency that can be edited, e.g. "12.50". */
std::string StockMoneyToEditString(int64_t money)
{
	return fmt::format("{:.2f}", static_cast<double>(money) * GetStockCurrencyRate());
}

/**
 * Format a volume (hundredths of a LOT) as a LOT number with two decimals.
 * @param volume Volume in hundredths of a LOT.
 * @return For example "1.25".
 */
std::string StockVolumeToString(uint32_t volume)
{
	return fmt::format("{}.{:02}", volume / 100, volume % 100);
}

/** Text that names an order type. */
StringID GetStockOrderTypeString(StockOrderType type)
{
	switch (type) {
		case StockOrderType::Buy:       return STR_STOCK_TYPE_BUY;
		case StockOrderType::Sell:      return STR_STOCK_TYPE_SELL;
		case StockOrderType::BuyLimit:  return STR_STOCK_TYPE_BUY_LIMIT;
		case StockOrderType::SellLimit: return STR_STOCK_TYPE_SELL_LIMIT;
		case StockOrderType::BuyStop:   return STR_STOCK_TYPE_BUY_STOP;
		case StockOrderType::SellStop:  return STR_STOCK_TYPE_SELL_STOP;
		default: return STR_STOCK_TYPE_BUY;
	}
}

/** The text for a close reason. */
static StringID GetStockCloseReasonString(StockCloseReason reason)
{
	switch (reason) {
		case StockCloseReason::StopLoss:   return STR_STOCK_REASON_SL;
		case StockCloseReason::TakeProfit: return STR_STOCK_REASON_TP;
		case StockCloseReason::StopOut:    return STR_STOCK_REASON_STOP_OUT;
		case StockCloseReason::Delisted:   return STR_STOCK_REASON_DELISTED;
		default:                           return STR_STOCK_REASON_MANUAL;
	}
}

/** Economy year * 12 + month as "2026-03". */
static std::string StockMonthToString(int32_t ym)
{
	const int month = ym % 12 + 1;
	return fmt::format("{}-{:02}", ym / 12, month);
}

/** Whether the terminal shows Thai texts for the valuables (the same switch as the central bank window). */
static constexpr bool STOCK_USE_THAI = true;

/** A text in Thai or English, like CbText in the central bank window. */
static std::string StockText(const char *thai, const char *english)
{
	return STOCK_USE_THAI ? std::string(thai) : std::string(english);
}

/** The timeframe that a timeframe button selects. All timeframes are periods of the game clock of the mod (see game_time_manager.h). */
static GameTimeframe FrameOfWidget(WidgetID widget)
{
	switch (widget) {
		case WID_ST_TF_M5:    return GameTimeframe::M5;
		case WID_ST_TF_MONTH: return GameTimeframe::H4;
		case WID_ST_TF_YEAR:  return GameTimeframe::H12;
		case WID_ST_TF_D1:    return GameTimeframe::D1;
		case WID_ST_TF_W1:    return GameTimeframe::W1;
		case WID_ST_TF_MN:    return GameTimeframe::MN;
		case WID_ST_TF_Y1:    return GameTimeframe::Y1;
		default:              return GameTimeframe::H1; // WID_ST_TF_DAY. The first three widgets keep their old names: they are the 1h, 4h and 12h buttons.
	}
}

/** The leverage as text: "1:100", or "1:Unlimited" for the divisor 0. */
static std::string LeverageText(int64_t divisor)
{
	return divisor > 0 ? fmt::format("1:{}", divisor) : std::string("1:Unlimited");
}

static uint8_t _stock_active_wallet = WALLET_CASH; ///< The wallet that the terminal shows and trades with; only for the local company.

/**
 * The wallet that the terminal shows and that new trades are opened with. When the remembered wallet does not exist
 * (any more) for the local company, the first trading wallet is used.
 * @return Number of the wallet; WALLET_CASH when the local company has no wallet.
 */
uint8_t GetStockActiveWallet()
{
	if (!Company::IsValidID(_local_company)) return WALLET_CASH;
	if (_stock_active_wallet != WALLET_CASH && GetWalletKind(_local_company, _stock_active_wallet) != WalletKind::End) return _stock_active_wallet;

	const Wallet *wallet = _wallets.FindFirst(_local_company, WalletKind::Trading);
	_stock_active_wallet = wallet != nullptr ? wallet->id : WALLET_CASH;
	return _stock_active_wallet;
}

/** Choose the wallet that the terminal shows and trades with. */
void SetStockActiveWallet(uint8_t wallet)
{
	_stock_active_wallet = wallet;
}

/** Number of candles of the fast and of the slow simple moving average. */
static constexpr size_t STOCK_SMA_FAST = 10;
static constexpr size_t STOCK_SMA_SLOW = 30;

/**
 * Simple moving average of the closing price. Entry i is the average of the closes of the candles i + 1 - period .. i
 * and is only valid for i + 1 >= period. It is calculated in one pass with a running sum.
 * @param candles The candles, the oldest first.
 * @param period Number of candles in the average, at least 1.
 * @return One value per candle; 0 where there are not enough candles before it.
 */
static std::vector<int64_t> ComputeSMA(const std::vector<StockCandle> &candles, size_t period)
{
	std::vector<int64_t> out(candles.size(), 0);
	if (period == 0) return out;
	int64_t sum = 0;
	for (size_t i = 0; i < candles.size(); i++) {
		sum += candles[i].close;
		if (i >= period) sum -= candles[i - period].close;
		if (i + 1 >= period) out[i] = sum / static_cast<int64_t>(period);
	}
	return out;
}

/** A tenths value as text with one decimal, e.g. 123 -> "12.3". */
static std::string StockTenths(int64_t tenths)
{
	const int64_t a = tenths < 0 ? -tenths : tenths;
	return fmt::format("{}{}.{}", tenths < 0 ? "-" : "", a / 10, a % 10);
}

/** Horizontal layout of the toolbox with the open trades. It is shared by the drawing and the click handling. */
struct StockToolboxColumns {
	int id;      ///< Left edge of the ticket column.
	int symbol;  ///< Left edge of the symbol column.
	int type;    ///< Left edge of the type column.
	int volume;  ///< Right edge of the volume column.
	int open;    ///< Right edge of the open price column, left edge of the S / L column.
	int sl;      ///< Right edge of the S / L column, left edge of the T / P column.
	int tp;      ///< Right edge of the T / P column, left edge of the trailing stop column.
	int trail;   ///< Right edge of the trailing stop column, left edge of the price column.
	int price;   ///< Right edge of the price column, left edge of the profit column.
	int profit;  ///< Right edge of the profit column.
	int close;   ///< Left edge of the close button.

	explicit StockToolboxColumns(const Rect &r) :
		id(r.left + ScaleGUITrad(4)),
		symbol(r.left + ScaleGUITrad(44)),
		type(r.left + ScaleGUITrad(132)),
		volume(r.left + ScaleGUITrad(250)),
		open(r.left + ScaleGUITrad(326)),
		sl(r.left + ScaleGUITrad(394)),
		tp(r.left + ScaleGUITrad(462)),
		trail(r.left + ScaleGUITrad(530)),
		price(r.left + ScaleGUITrad(598)),
		profit(r.left + ScaleGUITrad(684)),
		close(r.right - ScaleGUITrad(26))
	{}
};

/** Widths of one candle (in unscaled pixels) that the mouse wheel steps through on the chart. */
static constexpr int CHART_SLOT_WIDTHS[] = {3, 4, 6, 8, 10, 14, 20, 28, 40};
static constexpr int CHART_ZOOM_DEFAULT = 4; ///< Index of the default width (10).
static constexpr int CHART_ZOOM_MAX = static_cast<int>(std::size(CHART_SLOT_WIDTHS)) - 1;
static constexpr double CHART_VZOOM_MIN = 0.05; ///< Most zoomed in price range.
static constexpr double CHART_VZOOM_MAX = 8.0;  ///< Most zoomed out price range.
static constexpr int CHART_WIDTH_PX = 960;      ///< Width in unscaled pixels of the chart panel (the smallest size of the panel, and so its size when the window opens).
static constexpr int CHART_HEIGHT_PX = 420;     ///< Height in unscaled pixels of the chart panel.

struct StockWindow : Window {
	static constexpr size_t PORTFOLIO_FIXED_ROWS = 3; ///< Lines of the portfolio that do not scroll: two summary lines and the table header.

	/** What the open query window is for. */
	enum class QueryKind : uint8_t {
		None,         ///< No query window is open.
		CloseVolume,  ///< Volume to close of a position.
		StopLoss,     ///< New Stop Loss of a position.
		TakeProfit,   ///< New Take Profit of a position.
		Deposit,      ///< Amount to move from the company cash to the trading balance.
		Withdraw,     ///< Amount to move from the trading balance to the company cash.
		PendingPrice, ///< New trigger price of a pending order.
		Trail,        ///< New trailing stop distance of a position.
	};

	/** What the toolbox shows. */
	enum class ToolView : uint8_t {
		Orders,     ///< The open positions and the pending orders.
		History,    ///< The closed trades.
		Portfolio,  ///< The portfolio summary.
		Ranking,    ///< The ranking of the companies.
	};

	CompanyID selected = INVALID_OWNER; ///< Symbol (a company, or a valuable: the symbol number is STOCK_FIRST_VALUABLE and up) whose chart is shown.
	bool show_valuables = false;        ///< Whether the Market Watch lists the valuables instead of the companies.
	Scrollbar *vscroll = nullptr;       ///< Scrollbar of the toolbox.
	Scrollbar *watch_scroll = nullptr;  ///< Scrollbar of the Market Watch.
	ToolView view = ToolView::Orders;   ///< What the toolbox shows.
	StockRankSort rank_sort = StockRankSort::MarketCap; ///< How the ranking is sorted.
	QueryKind query_kind = QueryKind::None; ///< What the query window is asking for.
	uint32_t query_id = 0;              ///< Number of the position that the query window is about.
	bool is_dragging_chart = false;     ///< Whether the mouse button is held down on the chart.
	int drag_start_x = 0;               ///< X position (in the window) where the drag started, or where the last candle step was made.
	int chart_scroll_offset = 0;        ///< Number of candles the chart is scrolled back into the past; 0 shows the newest candle at the right edge; below 0 the chart is scrolled into the future: that many empty candle slots (whitespace) are shown at the right of the newest candle.
	int drag_start_y = 0;               ///< Y position (in the window) where the last vertical drag step was made.
	int chart_zoom = CHART_ZOOM_DEFAULT; ///< Index in #CHART_SLOT_WIDTHS: the width of one candle. Mouse wheel on the chart.
	double chart_vzoom = 1.0;           ///< Height of the visible price range, 1.0 = fit all visible candles; smaller is zoomed in. Ctrl + mouse wheel.
	bool chart_expanded = false;        ///< Whether the window is enlarged to the screen by the Expand button.
	int restore_left = 0;               ///< Position and size of the window before it was enlarged.
	int restore_top = 0;
	int restore_width = 0;
	int restore_height = 0;
	double chart_vshift = 0.0;          ///< Vertical scroll of the price range as a part of the fitted range; positive shows higher prices. Drag up/down.
	int hover_x = -1;                   ///< Mouse X (in the window) while it is over the chart, otherwise -1. Used by the crosshair.
	int hover_y = -1;                   ///< Mouse Y (in the window) while it is over the chart, otherwise -1.
	GameTimeframe frame = GameTimeframe::H1; ///< Timeframe of the chart: one candle per 5 game minutes, 1h, 4h, 12h, game day, game week, game month (30 days) or game year (365 days).

	StockWindow(WindowDesc &desc) : Window(desc)
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_ST_TOOL_SCROLLBAR);
		this->watch_scroll = this->GetScrollbar(WID_ST_WATCH_SCROLLBAR);
		this->FinishInitNested(0);
		this->UpdateTimeframeButtons();
		this->EnsureValidSelection();
		this->UpdateTabButtons();
		this->OnInvalidateData(0);
	}

	/** Show which timeframe button is active. */
	void UpdateTimeframeButtons()
	{
		this->SetWidgetLoweredState(WID_ST_TF_M5, this->frame == GameTimeframe::M5);
		this->SetWidgetLoweredState(WID_ST_TF_DAY, this->frame == GameTimeframe::H1);   // The first three widgets keep their old names: they are the 1h, 4h and 12h buttons now.
		this->SetWidgetLoweredState(WID_ST_TF_MONTH, this->frame == GameTimeframe::H4);
		this->SetWidgetLoweredState(WID_ST_TF_YEAR, this->frame == GameTimeframe::H12);
		this->SetWidgetLoweredState(WID_ST_TF_D1, this->frame == GameTimeframe::D1);
		this->SetWidgetLoweredState(WID_ST_TF_W1, this->frame == GameTimeframe::W1);
		this->SetWidgetLoweredState(WID_ST_TF_MN, this->frame == GameTimeframe::MN);
		this->SetWidgetLoweredState(WID_ST_TF_Y1, this->frame == GameTimeframe::Y1);
	}

	int RowHeight() const { return GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(2); }
	int HeaderHeight() const { return this->RowHeight() + ScaleGUITrad(3); }

	/** Make sure #selected is a symbol that exists. */
	void EnsureValidSelection()
	{
		if (IsStockSymbolValid(this->selected.base())) return;
		if (Company::IsValidID(_local_company)) {
			this->selected = _local_company;
			return;
		}
		for (const Company *c : Company::Iterate()) {
			this->selected = c->index;
			return;
		}
		this->selected = INVALID_OWNER;
	}

	/**
	 * The candles of the selected symbol in the active timeframe of game time, the oldest first: the closed candles and
	 * the running one. ym holds the number of the candle (game minutes since the start, divided by the length of the candle:
	 * 5 minutes, or 1, 4, 12, 24, 168, 720 or 8,760 hours); the prices are converted to whole money units, like the other candles.
	 */
	std::vector<StockCandle> GetChartCandles() const
	{
		std::vector<StockCandle> candles;
		if (!IsStockSymbolValid(this->selected.base())) return candles;
		const TimeframeAggregator &frames = _stock_market.frames[this->selected.base()];

		auto convert = [](const TimeframeBucket &b) {
			StockCandle c;
			c.ym = static_cast<int32_t>(std::min<uint64_t>(b.index, INT32_MAX));
			c.open = StockFixedToPrice(b.open);
			c.high = StockFixedToPrice(b.high);
			c.low = StockFixedToPrice(b.low);
			c.close = StockFixedToPrice(b.close);
			c.volume = b.volume;
			return c;
		};
		for (const TimeframeBucket &b : frames.Closed(this->frame)) candles.push_back(convert(b));
		if (frames.HasRunning(this->frame)) candles.push_back(convert(frames.Running(this->frame)));
		return candles;
	}

	/** An open position or a pending order of the local company with this number, or nullptr. */
	static const StockOrder *FindOwnOrder(uint32_t id)
	{
		if (!Company::IsValidID(_local_company)) return nullptr;
		const StockOrder *order = _stock_market.FindPosition(id, _local_company);
		if (order == nullptr) order = _stock_market.FindPending(id, _local_company);
		return order;
	}

	/**
	 * Keep the scroll offset of the chart inside the candles that exist in the active timeframe. The offset is counted
	 * in candles of that timeframe, so it can be out of range after the timeframe or the symbol was changed, or when
	 * old candles were dropped.
	 */
	void ClampChartScroll()
	{
		const int count = static_cast<int>(this->GetChartCandles().size());
		this->chart_scroll_offset = std::clamp(this->chart_scroll_offset, -this->ChartMaxFuture(), std::max(0, count - 1));
	}

	/** Width in pixels of the area of the chart where candles are drawn (without the price axis); must follow the layout of DrawChart. */
	int ChartPlotWidth() const
	{
		const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_ST_CHART);
		return static_cast<int>(nw->current_x) - 1 - ScaleGUITrad(6) - ScaleGUITrad(64);
	}

	/** Number of candle slots that fit in the chart at the current zoom. */
	int ChartMaxVisible() const
	{
		return std::max(1, this->ChartPlotWidth() / this->ChartSlot());
	}

	/** Most empty slots (whitespace) that the chart can be scrolled into the future: all but one slot, so the newest candle stays on the screen. */
	int ChartMaxFuture() const
	{
		return this->ChartMaxVisible() - 1;
	}

	/** Width of one candle in pixels, from the zoom of the chart. */
	int ChartSlot() const
	{
		return std::max(1, ScaleGUITrad(CHART_SLOT_WIDTHS[std::clamp(this->chart_zoom, 0, CHART_ZOOM_MAX)]));
	}

	/** Back to the price range that fits the candles (the symbol or the timeframe changed, or double click). */
	void ResetChartVertical()
	{
		this->chart_vzoom = 1.0;
		this->chart_vshift = 0.0;
	}

	/** Enlarge the window to (nearly) the whole screen, so that the chart gets big, or put it back to its old size. */
	void ToggleChartExpand()
	{
		this->SetDirty();
		if (!this->chart_expanded) {
			this->restore_left = this->left;
			this->restore_top = this->top;
			this->restore_width = this->width;
			this->restore_height = this->height;
			const int new_top = ScaleGUITrad(32); // Below the main toolbar.
			const int new_height = std::max(this->height, static_cast<int>(_screen.height) - new_top - ScaleGUITrad(16)); // Above the status bar.
			const int new_width = std::max(this->width, static_cast<int>(_screen.width));
			this->left = 0;
			this->top = new_top;
			ResizeWindow(this, new_width - this->width, new_height - this->height);
			this->chart_expanded = true;
		} else {
			this->left = this->restore_left;
			this->top = this->restore_top;
			ResizeWindow(this, this->restore_width - this->width, this->restore_height - this->height);
			this->chart_expanded = false;
		}
		this->SetDirty();
	}

	/** Height in pixels of the price pane of the chart; must follow the layout of DrawChart. */
	int ChartPriceHeight() const
	{
		const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_ST_CHART);
		const int full = static_cast<int>(nw->current_y) - GetCharacterHeight(FS_SMALL) - ScaleGUITrad(4) - ScaleGUITrad(6);
		if (full < ScaleGUITrad(150)) return std::max(1, full);
		return std::max(1, full - full / 4 - full / 6 - 2 * ScaleGUITrad(4));
	}

	/** The mouse wheel on the chart: zoom the candles in and out; with Ctrl: zoom the price range. */
	void OnMouseWheel(int wheel, WidgetID widget) override
	{
		if (widget != WID_ST_CHART || wheel == 0) return;
		const bool zoom_in = wheel < 0;
		if (_ctrl_pressed) {
			this->chart_vzoom = std::clamp(this->chart_vzoom * (zoom_in ? 0.8 : 1.25), CHART_VZOOM_MIN, CHART_VZOOM_MAX);
		} else {
			this->chart_zoom = std::clamp(this->chart_zoom + (zoom_in ? 1 : -1), 0, CHART_ZOOM_MAX);
		}
		this->SetDirty();
	}

	/** The rows of the toolbox: first the open positions, then the pending orders of the selected wallet of the local company. */
	std::vector<const StockOrder *> GetRows() const
	{
		std::vector<const StockOrder *> rows;
		if (!Company::IsValidID(_local_company)) return rows;
		const uint8_t wallet = GetStockActiveWallet();
		for (const StockOrder &p : _stock_market.positions) {
			if (p.owner == _local_company.base() && p.wallet == wallet) rows.push_back(&p);
		}
		for (const StockOrder &p : _stock_market.pending) {
			if (p.owner == _local_company.base() && p.wallet == wallet) rows.push_back(&p);
		}
		return rows;
	}

	/** The closed trades of the local company, the newest first. */
	std::vector<const StockClosedTrade *> GetHistoryRows() const
	{
		std::vector<const StockClosedTrade *> rows;
		if (!Company::IsValidID(_local_company)) return rows;
		for (auto it = _stock_market.history.rbegin(); it != _stock_market.history.rend(); ++it) {
			if (it->owner == _local_company.base()) rows.push_back(&*it);
		}
		return rows;
	}

	/** Number of scrollable rows of what the toolbox shows. The portfolio has three fixed lines above its months. */
	size_t GetViewRowCount() const
	{
		switch (this->view) {
			case ToolView::History:   return this->GetHistoryRows().size();
			case ToolView::Portfolio: return this->GetPortfolioMonths().size() + PORTFOLIO_FIXED_ROWS;
			case ToolView::Ranking:   return Company::GetNumItems();
			default:                  return this->GetRows().size();
		}
	}

	/** The months of the portfolio table of the local company, the newest first. */
	std::vector<StockMonthProfit> GetPortfolioMonths() const
	{
		std::vector<StockMonthProfit> months;
		if (!Company::IsValidID(_local_company)) return months;
		months = _stock_market.stats[_local_company.base()].months;
		std::reverse(months.begin(), months.end());
		return months;
	}

	void OnResize() override
	{
		/* The toolbox widget has no vertical resize step, so compute the number of visible rows by hand. */
		const NWidgetBase *toolbox = this->GetWidget<NWidgetBase>(WID_ST_TOOLBOX);
		this->vscroll->SetCapacity(static_cast<size_t>(std::max(1, (static_cast<int>(toolbox->current_y) - this->HeaderHeight()) / std::max(1, this->RowHeight()))));

		/* The same for the Market Watch. */
		const NWidgetBase *watch = this->GetWidget<NWidgetBase>(WID_ST_WATCH);
		this->watch_scroll->SetCapacity(static_cast<size_t>(std::max(1, (static_cast<int>(watch->current_y) - this->HeaderHeight()) / std::max(1, this->RowHeight()))));
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		this->vscroll->SetCount(this->GetViewRowCount());
		this->watch_scroll->SetCount(this->show_valuables ? VALUABLE_COUNT : Company::GetNumItems());
		this->SetDirty();
	}

	void OnDropdownSelect(WidgetID widget, int index, [[maybe_unused]] int click_result) override
	{
		if (widget == WID_ST_LEVERAGE && Company::IsValidID(_local_company)) {
			if (index >= 0 && index < WALLET_LEVERAGE_COUNT) {
				Command<CMD_WALLET_LEVERAGE>::Post(STR_WALLET_ERROR_CANT_LEVERAGE, GetStockActiveWallet(), static_cast<uint8_t>(index));
			}
			return;
		}
		if (widget != WID_ST_WALLET_DROPDOWN || !Company::IsValidID(_local_company)) return;
		for (uint8_t id : _wallets.GetIds(_local_company)) {
			if (id == index) {
				SetStockActiveWallet(id);
				this->OnInvalidateData();
				return;
			}
		}
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget == WID_ST_WALLET_DROPDOWN && Company::IsValidID(_local_company)) {
			const uint8_t wallet = GetStockActiveWallet();
			return GetString(STR_STOCK_WALLET, GetWalletName(_local_company, wallet), GetWalletBalance(_local_company, wallet));
		}
		if (widget == WID_ST_LEVERAGE && Company::IsValidID(_local_company)) {
			return GetString(STR_STOCK_LEVERAGE_VALUE, LeverageText(GetWalletLeverageDivisor(_local_company, GetStockActiveWallet())));
		}
		return this->Window::GetWidgetString(widget, stringid);
	}

	void OnPaint() override
	{
		this->EnsureValidSelection();
		this->ClampChartScroll();
		/* New orders are blocked while the wallet is in margin call danger. */
		bool block_orders = false;
		if (Company::IsValidID(_local_company)) {
			const uint8_t wallet = GetStockActiveWallet();
			block_orders = !IsTradingWallet(_local_company, wallet) || GetStockRisk(_local_company, wallet).level >= StockRiskLevel::Warning;
		}
		this->SetWidgetDisabledState(WID_ST_NEW_ORDER, block_orders);
		this->DrawWidgets();
	}

	/**
	 * Open a query window for a position.
	 * @param kind What to ask for.
	 * @param position The position.
	 */
	void OpenQuery(QueryKind kind, const StockOrder &position)
	{
		this->query_kind = kind;
		this->query_id = position.id;

		switch (kind) {
			case QueryKind::CloseVolume:
				ShowQueryString(StockVolumeToString(position.volume), STR_STOCK_QUERY_CLOSE_VOLUME, 16, this, CS_ALPHANUMERAL, QueryStringFlag::AcceptUnchanged);
				break;

			case QueryKind::StopLoss:
				ShowQueryString(position.sl != 0 ? StockMoneyToEditString(position.sl) : std::string{}, STR_STOCK_QUERY_SL, 24, this, CS_ALPHANUMERAL, {});
				break;

			case QueryKind::TakeProfit:
				ShowQueryString(position.tp != 0 ? StockMoneyToEditString(position.tp) : std::string{}, STR_STOCK_QUERY_TP, 24, this, CS_ALPHANUMERAL, {});
				break;

			case QueryKind::PendingPrice:
				ShowQueryString(StockMoneyToEditString(position.price), STR_STOCK_COL_PRICE, 24, this, CS_ALPHANUMERAL, {});
				break;

			case QueryKind::Trail:
				ShowQueryString(position.trail != 0 ? StockMoneyToEditString(position.trail) : std::string{}, STR_STOCK_QUERY_TRAIL, 24, this, CS_ALPHANUMERAL, {});
				break;

			default: break;
		}
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		const QueryKind kind = this->query_kind;
		this->query_kind = QueryKind::None;
		if (!str.has_value() || !Company::IsValidID(_local_company)) return;

		if (kind == QueryKind::Deposit || kind == QueryKind::Withdraw) {
			double amount = 0;
			if (!ParseStockNumber(*str, amount) || amount <= 0) return;
			/* Deposit: from the company cash to the selected wallet. Withdraw: from the selected wallet back to the cash. */
			const uint8_t wallet = GetStockActiveWallet();
			if (wallet == WALLET_CASH) return;
			const bool deposit = kind == QueryKind::Deposit;
			Command<CMD_WALLET_TRANSFER>::Post(STR_STOCK_ERROR_CANT_TRANSFER, deposit ? WALLET_CASH : wallet, deposit ? wallet : WALLET_CASH, Money(StockDisplayToMoney(amount)));
			return;
		}

		/* The position or order may have been closed or cancelled while the query window was open. */
		const StockOrder *position = FindOwnOrder(this->query_id);
		if (position == nullptr) return;

		switch (kind) {
			case QueryKind::CloseVolume: {
				if (_stock_market.FindPosition(position->id, _local_company) == nullptr) return; // Only open positions can be closed in parts.
				double lots = 0;
				if (!ParseStockNumber(*str, lots) || lots <= 0) return;
				const double units = std::round(lots * 100.0);
				if (units < 1) return;
				Command<CMD_STOCK_CLOSE>::Post(STR_STOCK_ERROR_CANT_CLOSE, position->id, static_cast<uint32_t>(std::min<double>(units, position->volume)));
				break;
			}

			case QueryKind::StopLoss:
			case QueryKind::TakeProfit: {
				/* An empty text removes the level. */
				int64_t value = 0;
				if (str->find_first_not_of(' ') != std::string::npos) {
					double number = 0;
					if (!ParseStockNumber(*str, number) || number <= 0) return;
					value = StockDisplayToMoney(number);
				}
				const int64_t sl = kind == QueryKind::StopLoss ? value : position->sl;
				const int64_t tp = kind == QueryKind::TakeProfit ? value : position->tp;
				Command<CMD_STOCK_MODIFY>::Post(STR_STOCK_ERROR_CANT_MODIFY, position->id, Money(sl), Money(tp), Money(position->price), Money(position->trail));
				break;
			}

			case QueryKind::PendingPrice: {
				/* The trigger price of a pending order is changed in place, without deleting and placing the order again. */
				double number = 0;
				if (!ParseStockNumber(*str, number) || number <= 0) return;
				Command<CMD_STOCK_MODIFY>::Post(STR_STOCK_ERROR_CANT_MODIFY, position->id, Money(position->sl), Money(position->tp), Money(StockDisplayToMoney(number)), Money(0));
				break;
			}

			case QueryKind::Trail: {
				/* An empty text switches the trailing stop off. Only open positions can trail. */
				if (_stock_market.FindPosition(position->id, _local_company) == nullptr) return;
				int64_t distance = 0;
				if (str->find_first_not_of(' ') != std::string::npos) {
					double number = 0;
					if (!ParseStockNumber(*str, number) || number <= 0) return;
					distance = StockDisplayToMoney(number);
				}
				Command<CMD_STOCK_MODIFY>::Post(STR_STOCK_ERROR_CANT_MODIFY, position->id, Money(position->sl), Money(position->tp), Money(position->price), Money(distance));
				break;
			}

			default: break;
		}
	}

	/** Remember where the mouse is over the chart, for the crosshair. */
	void OnMouseOver(Point pt, WidgetID widget) override
	{
		const int old_x = this->hover_x;
		const int old_y = this->hover_y;
		if (widget == WID_ST_CHART) {
			this->hover_x = pt.x;
			this->hover_y = pt.y;
		} else {
			this->hover_x = -1;
			this->hover_y = -1;
		}
		if (old_x != this->hover_x || old_y != this->hover_y) this->SetWidgetDirty(WID_ST_CHART);
	}

	/**
	 * While the mouse button is held down on the chart, scroll the chart one candle for every candle width of mouse movement.
	 * Dragging to the right goes back in time. Dragging to the left comes back to the newest candle and then goes on into the
	 * future, where the chart is empty (whitespace), like in TradingView.
	 */
	void OnMouseLoop() override
	{
		if (!this->is_dragging_chart) return;

		/* The button was released (also when it was released outside of the window). */
		if (!_left_button_down) {
			this->is_dragging_chart = false;
			return;
		}

		bool changed = false;

		/* Vertical: dragging up or down moves the price range, one pixel of the chart for every pixel of the mouse. */
		const int dy = (_cursor.pos.y - this->top) - this->drag_start_y;
		if (dy != 0) {
			this->drag_start_y += dy;
			this->chart_vshift = std::clamp(this->chart_vshift + static_cast<double>(dy) / this->ChartPriceHeight() * this->chart_vzoom, -4.0, 4.0);
			changed = true;
		}

		const int slot = this->ChartSlot(); // Must be the same as the candle width in DrawChart.
		const int delta = (_cursor.pos.x - this->left) - this->drag_start_x;
		const int steps = delta / slot; // Whole candles only; rounds towards zero.
		if (steps != 0) {
			/* Keep the rest of the movement, so a slow drag does not lose pixels. */
			this->drag_start_x += steps * slot;

			/* Dragging to the right goes back in time, dragging to the left comes back to the newest candles and then into the free space after them. */
			const size_t count = this->GetChartCandles().size();
			const int max_offset = count > 0 ? static_cast<int>(count) - 1 : 0; // At least one candle stays visible.
			this->chart_scroll_offset = std::clamp(this->chart_scroll_offset + steps, -this->ChartMaxFuture(), max_offset);
			changed = true;
		}
		if (changed) this->SetDirty();
	}

	/** Show the chart of a symbol and make it the symbol of the New Order window. */
	void SelectSymbol(CompanyID symbol)
	{
		this->selected = symbol;
		this->chart_scroll_offset = 0;
		this->ResetChartVertical();
		SetStockOrderSymbol(symbol);

		/* The Market Watch shows the list that the symbol belongs to. */
		const bool valuables = IsValuableSymbol(symbol.base());
		if (valuables != this->show_valuables) this->watch_scroll->SetPosition(0);
		this->show_valuables = valuables;
		this->UpdateTabButtons();
		this->OnInvalidateData();
	}

	/** Show which tab of the Market Watch is active. */
	void UpdateTabButtons()
	{
		this->SetWidgetLoweredState(WID_ST_TAB_COMPANIES, !this->show_valuables);
		this->SetWidgetLoweredState(WID_ST_TAB_VALUABLES, this->show_valuables);
	}

	void OnClick(Point pt, WidgetID widget, int click_count) override
	{
		switch (widget) {
			case WID_ST_CHART:
				/* Double click: back to the default view (newest candles, default zoom, price range that fits). */
				if (click_count >= 2) {
					this->chart_zoom = CHART_ZOOM_DEFAULT;
					this->chart_scroll_offset = 0;
					this->ResetChartVertical();
					this->is_dragging_chart = false;
					this->SetDirty();
					break;
				}
				this->is_dragging_chart = true;
				this->drag_start_x = pt.x;
				this->drag_start_y = pt.y;
				break;

			case WID_ST_CHART_EXPAND:
				this->ToggleChartExpand();
				break;

			case WID_ST_NEW_ORDER:
				ShowStockOrderWindow(this->selected);
				break;

			case WID_STOCK_CLOSE_ALL: {
				if (!Company::IsValidID(_local_company)) break;
				/* Collect the numbers first: with a single player the commands change the list of positions at once. */
				std::vector<std::pair<uint32_t, uint32_t>> to_close;
				for (const StockOrder &p : _stock_market.positions) {
					if (p.owner == _local_company.base() && p.wallet == GetStockActiveWallet()) to_close.emplace_back(p.id, p.volume);
				}
				for (const auto &[id, volume] : to_close) {
					Command<CMD_STOCK_CLOSE>::Post(STR_STOCK_ERROR_CANT_CLOSE, id, volume);
				}
				break;
			}

			case WID_ST_LEVERAGE: {
				if (!Company::IsValidID(_local_company)) break;
				const Wallet *w = _wallets.Find(_local_company, GetStockActiveWallet());
				if (w == nullptr || w->kind != WalletKind::Trading) break;
				DropDownList list;
				for (uint8_t level = 0; level < WALLET_LEVERAGE_COUNT; level++) {
					list.push_back(MakeDropDownListStringItem(fmt::format("LV {}", LeverageText(WALLET_LEVERAGE_DIVISORS[level])), level));
				}
				ShowDropDownList(this, std::move(list), w->leverage, WID_ST_LEVERAGE, 0);
				break;
			}

			case WID_ST_WALLET_DROPDOWN: {
				if (!Company::IsValidID(_local_company)) break;
				DropDownList list;
				for (uint8_t id : _wallets.GetIds(_local_company)) {
					/* No colour code in this string: the list picks black text, and white text on the highlighted row. {BLACK} would stay black on the black highlight. */
					list.push_back(MakeDropDownListStringItem(GetString(STR_STOCK_WALLET_ITEM, GetWalletName(_local_company, id), GetWalletBalance(_local_company, id)), id));
				}
				if (list.empty()) break;
				ShowDropDownList(this, std::move(list), GetStockActiveWallet(), WID_ST_WALLET_DROPDOWN, 0);
				break;
			}

			case WID_ST_WALLET_NEW:
				if (Company::IsValidID(_local_company)) Command<CMD_WALLET_CREATE>::Post(STR_WALLET_ERROR_CANT_CREATE, WalletKind::Trading);
				break;

			case WID_ST_WALLET_DELETE:
				if (Company::IsValidID(_local_company)) Command<CMD_WALLET_DELETE>::Post(STR_WALLET_ERROR_CANT_DELETE, GetStockActiveWallet());
				break;

			case WID_ST_DEPOSIT:
			case WID_ST_WITHDRAW: {
				if (!Company::IsValidID(_local_company)) break;
				const bool deposit = widget == WID_ST_DEPOSIT;
				this->query_kind = deposit ? QueryKind::Deposit : QueryKind::Withdraw;
				ShowQueryString({}, deposit ? STR_STOCK_QUERY_DEPOSIT : STR_STOCK_QUERY_WITHDRAW, 24, this, CS_ALPHANUMERAL, {});
				break;
			}

			case WID_ST_TF_M5:
			case WID_ST_TF_DAY:
			case WID_ST_TF_MONTH:
			case WID_ST_TF_YEAR:
			case WID_ST_TF_D1:
			case WID_ST_TF_W1:
			case WID_ST_TF_MN:
			case WID_ST_TF_Y1: {
				const GameTimeframe wanted = FrameOfWidget(widget);
				if (this->frame != wanted) {
					this->frame = wanted;
					this->chart_scroll_offset = 0; // The units of the offset change with the timeframe.
					this->ResetChartVertical();
				}
				this->UpdateTimeframeButtons();
				this->SetDirty();
				break;
			}

			case WID_ST_HISTORY:
			case WID_ST_PORTFOLIO:
			case WID_ST_RANKING: {
				/* These buttons are toggles; pressing the active one goes back to the open trades. */
				const ToolView wanted = widget == WID_ST_HISTORY ? ToolView::History : widget == WID_ST_PORTFOLIO ? ToolView::Portfolio : ToolView::Ranking;
				this->view = this->view == wanted ? ToolView::Orders : wanted;
				this->SetWidgetLoweredState(WID_ST_HISTORY, this->view == ToolView::History);
				this->SetWidgetLoweredState(WID_ST_PORTFOLIO, this->view == ToolView::Portfolio);
				this->SetWidgetLoweredState(WID_ST_RANKING, this->view == ToolView::Ranking);
				this->vscroll->SetPosition(0);
				this->OnInvalidateData();
				break;
			}

			case WID_ST_TAB_COMPANIES:
			case WID_ST_TAB_VALUABLES: {
				const bool valuables = widget == WID_ST_TAB_VALUABLES;
				if (valuables == this->show_valuables) break;
				/* The chart follows the tab: the first valuable, or the company of the player (or the first company), is shown. */
				if (valuables) {
					this->SelectSymbol(CompanyID{static_cast<uint8_t>(STOCK_FIRST_VALUABLE)});
				} else {
					this->selected = INVALID_OWNER;
					this->show_valuables = false;
					this->EnsureValidSelection();
					this->SelectSymbol(this->selected);
				}
				break;
			}

			case WID_ST_WATCH: {
				const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_ST_WATCH);
				if (pt.y - nw->pos_y < this->HeaderHeight()) break;
				const int row = (pt.y - nw->pos_y - this->HeaderHeight()) / this->RowHeight();
				const size_t wanted = static_cast<size_t>(std::max(0, row)) + this->watch_scroll->GetPosition();
				if (this->show_valuables) {
					if (wanted < VALUABLE_COUNT) this->SelectSymbol(CompanyID{static_cast<uint8_t>(STOCK_FIRST_VALUABLE + wanted)});
					break;
				}
				size_t n = 0;
				for (const Company *c : Company::Iterate()) {
					if (n++ == wanted) {
						this->SelectSymbol(c->index);
						break;
					}
				}
				break;
			}

			case WID_ST_TOOLBOX: {
				const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_ST_TOOLBOX);
				if (this->view == ToolView::Ranking) {
					this->ClickRanking(pt, nw);
					break;
				}
				if (this->view != ToolView::Orders) break; // Closed trades and the portfolio can not be changed.
				if (pt.y - nw->pos_y < this->HeaderHeight()) break;
				const size_t row = this->vscroll->GetScrolledRowFromWidget(pt.y, this, WID_ST_TOOLBOX, this->HeaderHeight(), this->RowHeight());
				const std::vector<const StockOrder *> rows = this->GetRows();
				if (row >= rows.size()) break;

				const StockOrder &o = *rows[row];
				const bool is_position = o.type == StockOrderType::Buy || o.type == StockOrderType::Sell;
				const StockToolboxColumns col(nw->GetCurrentRect());

				if (pt.x >= col.close) {
					if (is_position) {
						/* Ask how much of the position to close; the default is all of it. */
						this->OpenQuery(QueryKind::CloseVolume, o);
					} else {
						Command<CMD_STOCK_CLOSE>::Post(STR_STOCK_ERROR_CANT_CLOSE, o.id, o.volume);
					}
				} else if (!is_position && pt.x >= col.volume && pt.x < col.open) {
					/* The price of a pending order is its trigger price: click it to change it. */
					this->OpenQuery(QueryKind::PendingPrice, o);
				} else if (pt.x >= col.open && pt.x < col.sl) {
					this->OpenQuery(QueryKind::StopLoss, o);
				} else if (pt.x >= col.sl && pt.x < col.tp) {
					this->OpenQuery(QueryKind::TakeProfit, o);
				} else if (is_position && pt.x >= col.tp && pt.x < col.trail) {
					this->OpenQuery(QueryKind::Trail, o);
				}
				break;
			}

			default: break;
		}
	}

	/** Column edges of the ranking; shared by the drawing and the clicks. */
	struct RankColumns {
		int rank, symbol, price, change, cap, bar;
		explicit RankColumns(const Rect &r) :
			rank(r.left + ScaleGUITrad(4)), symbol(r.left + ScaleGUITrad(44)), price(r.left + ScaleGUITrad(250)),
			change(r.left + ScaleGUITrad(340)), cap(r.left + ScaleGUITrad(470)), bar(r.left + ScaleGUITrad(486))
		{}
	};

	/** A click in the ranking: the header changes the sorting, a row selects the symbol. */
	void ClickRanking(Point pt, const NWidgetBase *nw)
	{
		const RankColumns col(nw->GetCurrentRect());
		if (pt.y - static_cast<int>(nw->pos_y) < this->HeaderHeight()) {
			if (pt.x < col.symbol + ScaleGUITrad(60)) return;
			if (pt.x < col.price) {
				this->rank_sort = StockRankSort::Price;
			} else if (pt.x < col.change) {
				this->rank_sort = StockRankSort::Change;
			} else {
				this->rank_sort = StockRankSort::MarketCap;
			}
			this->SetDirty();
			return;
		}
		const size_t row = this->vscroll->GetScrolledRowFromWidget(pt.y, this, WID_ST_TOOLBOX, this->HeaderHeight(), this->RowHeight());
		const std::vector<StockRankEntry> list = GetStockRanking(this->rank_sort);
		if (row >= list.size()) return;
		this->SelectSymbol(list[row].company);
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_ST_ACCOUNT: this->DrawAccount(r); break;
			case WID_ST_PNL: this->DrawPnl(r); break;
			case WID_ST_RISK: this->DrawRisk(r); break;
			case WID_ST_WATCH:   this->DrawWatch(r); break;
			case WID_ST_TITLE:   this->DrawTitle(r); break;
			case WID_ST_SUMMARY: this->DrawSummary(r); break;
			case WID_ST_CHART:   this->DrawChart(r); break;
			case WID_ST_TOOLBOX: this->DrawToolbox(r); break;
			case WID_ST_TAB_COMPANIES:
				DrawString(r.left + 2, r.right - 2, CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL)), StockText("บริษัท", "Companies"), this->show_valuables ? TC_BLACK : TC_WHITE, SA_HOR_CENTER);
				break;
			case WID_ST_TAB_VALUABLES:
				DrawString(r.left + 2, r.right - 2, CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL)), StockText("ทรัพย์สินมีค่า", "Valuables"), this->show_valuables ? TC_WHITE : TC_BLACK, SA_HOR_CENTER);
				break;
			case WID_ST_PORTFOLIO:
				DrawString(r.left + 2, r.right - 2, CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL)), std::string("Portfolio"), TC_BLACK, SA_HOR_CENTER);
				break;
			case WID_ST_RANKING:
				DrawString(r.left + 2, r.right - 2, CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL)), std::string("Ranking"), TC_BLACK, SA_HOR_CENTER);
				break;
			case WID_ST_CHART_EXPAND:
				DrawString(r.left + 2, r.right - 2, CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL)), std::string(this->chart_expanded ? "Shrink" : "Expand"), TC_BLACK, SA_HOR_CENTER);
				break;
			case WID_STOCK_CLOSE_ALL:
				DrawString(r.left + 2, r.right - 2, CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL)), std::string("Close All"), TC_BLACK, SA_HOR_CENTER);
				break;
			default: break;
		}
	}

	void DrawAccount(const Rect &r) const
	{
		const int top = CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL));
		if (!Company::IsValidID(_local_company)) {
			DrawString(r.left + 4, r.right - 4, top, STR_STOCK_SPECTATOR, TC_FROMSTRING, SA_LEFT);
			return;
		}
		/* Every wallet has its own balance, margin and margin level; the account line shows the selected one.
		 * This must match GetStockEquity(), which is what the margin checks and the margin level use. */
		const uint8_t wallet = GetStockActiveWallet();
		if (!IsTradingWallet(_local_company, wallet)) {
			/* The fuel reserve has no positions, so there is nothing to show but how to fill it. */
			DrawString(r.left + 4, r.right - 4, top, GetString(STR_STOCK_ACCOUNT_RESERVE, GetWalletName(_local_company, wallet)), TC_FROMSTRING, SA_LEFT);
			return;
		}
		const int64_t balance = GetWalletBalance(_local_company, wallet);
		const int64_t floating = GetStockFloatingProfit(_local_company, wallet);
		const int64_t margin = GetStockUsedMargin(_local_company, wallet);
		const int64_t equity = balance + floating;
		const int64_t level = GetStockMarginLevel(_local_company, wallet);
		std::string text;
		if (level < 0) {
			text = GetString(STR_STOCK_ACCOUNT, balance, equity, margin, equity - margin);
		} else {
			/* Below the margin call level the line turns red; below the stop-out level positions are closed by force. */
			text = GetString(level < STOCK_MARGIN_CALL_LEVEL ? STR_STOCK_ACCOUNT_CALL : STR_STOCK_ACCOUNT_LEVEL, balance, equity, margin, equity - margin, level);
		}
		DrawString(r.left + 4, r.right - 4, top, text, TC_FROMSTRING, SA_LEFT);
	}

	/** Draw a caption and a money value that is green for a profit and red for a loss. @return The x position after the text. */
	static int DrawSignedMoney(int x, int right, int top, const std::string &caption, int64_t value)
	{
		DrawString(x, right, top, caption, TC_BLACK, SA_LEFT);
		x += GetStringBoundingBox(caption).width;
		const std::string money = GetString(STR_JUST_CURRENCY_LONG, value);
		DrawString(x, right, top, money, value < 0 ? TC_RED : (value > 0 ? TC_GREEN : TC_BLACK), SA_LEFT);
		return x + GetStringBoundingBox(money).width;
	}

	/** Floating and realised profit of the selected wallet. */
	void DrawPnl(const Rect &r) const
	{
		if (!Company::IsValidID(_local_company)) return;
		const uint8_t wallet = GetStockActiveWallet();
		if (!IsTradingWallet(_local_company, wallet)) return;
		const int top = CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL));
		int x = DrawSignedMoney(r.left + ScaleGUITrad(4), r.right - 4, top, "Unrealized P/L: ", GetStockFloatingProfit(_local_company, wallet));
		x += ScaleGUITrad(16);
		DrawSignedMoney(x, r.right - 4, top, "Realized P/L (session): ", GetStockSessionRealized(_local_company, wallet));
	}

	/** Margin usage and the warning when the free margin is low. */
	void DrawRisk(const Rect &r) const
	{
		if (!Company::IsValidID(_local_company)) return;
		const uint8_t wallet = GetStockActiveWallet();
		if (!IsTradingWallet(_local_company, wallet)) return;
		const int top = CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL));
		const StockRisk risk = GetStockRisk(_local_company, wallet);
		if (risk.level == StockRiskLevel::None) {
			DrawString(r.left + ScaleGUITrad(4), r.right - 4, top, std::string("Margin Usage: 0.0%"), TC_BLACK, SA_LEFT);
			return;
		}
		const TextColour colour = risk.level == StockRiskLevel::Safe ? TC_GREEN : TC_RED;
		const std::string usage = fmt::format("Margin Usage: {}.{}%", risk.usage_tenths / 10, risk.usage_tenths % 10);
		DrawString(r.left + ScaleGUITrad(4), r.right - 4, top, usage, colour, SA_LEFT);
		if (risk.level >= StockRiskLevel::Warning) {
			const int x = r.left + ScaleGUITrad(4) + GetStringBoundingBox(usage).width + ScaleGUITrad(12);
			DrawString(x, r.right - 4, top, std::string("WARNING: LOW FREE MARGIN / MARGIN CALL RISK"), TC_RED, SA_LEFT);
		}
	}

	/** Market Watch: one row per symbol with the Bid and Ask price. */
	void DrawWatch(const Rect &r) const
	{
		const int row_h = this->RowHeight();
		const int ask_right = r.right - ScaleGUITrad(4);
		const int bid_right = ask_right - ScaleGUITrad(74);
		const int left = r.left + ScaleGUITrad(4);

		int y = r.top + ScaleGUITrad(2);
		DrawString(left, bid_right - ScaleGUITrad(74), y, STR_STOCK_COL_SYMBOL, TC_BLACK, SA_LEFT);
		DrawString(bid_right - ScaleGUITrad(70), bid_right, y, STR_STOCK_COL_BID, TC_BLACK, SA_RIGHT);
		DrawString(ask_right - ScaleGUITrad(70), ask_right, y, STR_STOCK_COL_ASK, TC_BLACK, SA_RIGHT);
		y = r.top + this->HeaderHeight();
		GfxDrawLine(r.left + 1, y - 2, r.right - 1, y - 2, GREY_SCALE(4));

		/* The valuables: one row each, with the code of the symbol and the change since the last month. */
		if (this->show_valuables) {
			const size_t first_valuable = this->watch_scroll->GetPosition();
			for (size_t k = first_valuable; k < VALUABLE_COUNT; k++) {
				if (y + row_h > r.bottom) break;
				const CompanyID id{static_cast<uint8_t>(STOCK_FIRST_VALUABLE + k)};
				if (id == this->selected) GfxFillRect(r.left + 1, y - 1, r.right - 1, y + row_h - 2, GREY_SCALE(4));
				DrawString(left, bid_right - ScaleGUITrad(74), y, GetStockSymbolShortName(id.base()), TC_WHITE, SA_LEFT);
				DrawString(bid_right - ScaleGUITrad(70), bid_right, y, GetString(STR_JUST_CURRENCY_SHORT, GetStockBid(id)), TC_LIGHT_BLUE, SA_RIGHT);
				DrawString(ask_right - ScaleGUITrad(70), ask_right, y, GetString(STR_JUST_CURRENCY_SHORT, GetStockAsk(id)), TC_LIGHT_BLUE, SA_RIGHT);
				y += row_h;
			}
			return;
		}

		/* Companies that have been scrolled out of the top of the list are skipped. */
		const size_t first = this->watch_scroll->GetPosition();
		size_t n = 0;
		for (const Company *c : Company::Iterate()) {
			if (n++ < first) continue;
			if (y + row_h > r.bottom) break;
			if (c->index == this->selected) GfxFillRect(r.left + 1, y - 1, r.right - 1, y + row_h - 2, GREY_SCALE(4));

			DrawString(left, bid_right - ScaleGUITrad(74), y, GetString(STR_STOCK_SYMBOL, c->index), TC_WHITE, SA_LEFT);
			DrawString(bid_right - ScaleGUITrad(70), bid_right, y, GetString(STR_JUST_CURRENCY_SHORT, GetStockBid(c->index)), TC_LIGHT_BLUE, SA_RIGHT);
			DrawString(ask_right - ScaleGUITrad(70), ask_right, y, GetString(STR_JUST_CURRENCY_SHORT, GetStockAsk(c->index)), TC_LIGHT_BLUE, SA_RIGHT);
			y += row_h;
		}
	}

	/** The price one month ago, used to show the change. */
	int64_t GetReferencePrice() const
	{
		const size_t index = this->selected.base();
		if (!_stock_market.candles[index].empty()) return _stock_market.candles[index].back().close;
		if (_stock_market.has_current[index]) return _stock_market.current[index].open;
		return GetStockPrice(this->selected);
	}

	void DrawTitle(const Rect &r) const
	{
		if (!IsStockSymbolValid(this->selected.base())) return;
		const int top = CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL));
		if (IsValuableSymbol(this->selected.base())) {
			DrawString(r.left + 4, r.right - 4, top, GetString(STR_STOCK_TITLE_SYMBOL, GetStockSymbolName(this->selected.base()), GetStockBid(this->selected), GetStockAsk(this->selected)), TC_FROMSTRING, SA_LEFT);
		} else {
			DrawString(r.left + 4, r.right - 4, top, GetString(STR_STOCK_TITLE, this->selected, GetStockBid(this->selected), GetStockAsk(this->selected)), TC_FROMSTRING, SA_LEFT);
		}

		const int64_t price = GetStockPrice(this->selected);
		const int64_t reference = this->GetReferencePrice();
		const int64_t tenths = reference > 0 ? (price - reference) * 1000 / reference : 0;
		const int64_t abs_tenths = tenths < 0 ? -tenths : tenths;
		const StringID str = tenths >= 0 ? STR_STOCK_CHANGE_UP : STR_STOCK_CHANGE_DOWN;
		DrawString(r.left + 4, r.right - 4, top, GetString(str, abs_tenths / 10, abs_tenths % 10), TC_FROMSTRING, SA_RIGHT);
	}

	/** The summary row: P/E, EPS and dividend yield of the selected symbol. */
	void DrawSummary(const Rect &r) const
	{
		if (!IsStockSymbolValid(this->selected.base())) return;
		const int top = CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL));
		if (IsValuableSymbol(this->selected.base())) {
			/* A valuable has no earnings: show what one unit is, the leverage and the spread. */
			const ValuableInfo &info = ValuablesManager::Info(this->selected.base() - STOCK_FIRST_VALUABLE);
			const int64_t bid = GetStockBid(this->selected);
			const int64_t ask = GetStockAsk(this->selected);
			const std::string text = fmt::format("{}: 1 unit = {}   {} {}   Spread {}   1 LOT = 100 units", info.name, info.unit,
					StockText("เลเวอเรจ", "Leverage"), LeverageText(Company::IsValidID(_local_company) ? GetWalletLeverageDivisor(_local_company, GetStockActiveWallet()) : STOCK_MARGIN_DIVISOR), ask - bid);
			DrawString(r.left + 4, r.right - 4, top, text, TC_LIGHT_BLUE, SA_LEFT);
			return;
		}
		const StockSummary s = GetStockSummary(this->selected);

		std::string text;
		if (!s.has_earnings) {
			text = GetString(STR_STOCK_SUMMARY_NONE);
		} else if (!s.has_pe) {
			text = GetString(STR_STOCK_SUMMARY_NO_PE, s.eps);
		} else {
			text = GetString(STR_STOCK_SUMMARY, s.pe_tenths / 10, s.pe_tenths % 10, s.eps, s.yield_tenths / 10, s.yield_tenths % 10);
		}
		DrawString(r.left + 4, r.right - 4, top, text, TC_FROMSTRING, SA_LEFT);
	}

	void DrawChart(const Rect &r) const
	{
		if (!IsStockSymbolValid(this->selected.base())) return;

		/* Temporary: the chart is switched off while the clock of the game is rebuilt (see GAME_TIME_DECOUPLE_CHART). The candles are still recorded. */
		if constexpr (GameTimeManager::IsChartDecoupled()) {
			GfxFillRect(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, GREY_SCALE(2));
			const GameTimeSnapshot now = GameTimeManager::Get().GetSnapshot();
			DrawString(r.left + 8, r.right - 8, r.top + ScaleGUITrad(8), std::string("Chart switched off (game clock mode)"), TC_WHITE, SA_LEFT);
			DrawString(r.left + 8, r.right - 8, r.top + ScaleGUITrad(8) + GetCharacterHeight(FS_NORMAL) + 2,
					fmt::format("Game time: year {}, day {}, {:02}:{:02}", now.year, now.day, now.hour, now.minute), TC_LIGHT_BLUE, SA_LEFT);
			return;
		}

		const std::vector<StockCandle> candles = this->GetChartCandles();

		const int small_height = GetCharacterHeight(FS_SMALL);
		const int axis_width = ScaleGUITrad(64);
		const int left = r.left + ScaleGUITrad(6);
		const int right = r.right - axis_width; // The price axis is on the right, like in TradingView.
		const int top = r.top + ScaleGUITrad(6);
		const int full_bottom = r.bottom - small_height - ScaleGUITrad(4);
		if (right - left < 20 || full_bottom - top < 20) return;

		/* When there is enough room, the chart has three panes: the price on top, the volume bars under it and the RSI at the bottom. */
		const bool panes = (full_bottom - top) >= ScaleGUITrad(150);
		/* The volume and RSI panes are low (1/10 and 1/8 of the chart, was 1/6 and 1/4) so the price pane gets the room; the minimums keep the "Vol" and 30/70 labels readable. */
		const int rsi_height = panes ? std::max<int>(ScaleGUITrad(36), (full_bottom - top) / 8) : 0;
		const int vol_height = panes ? std::max<int>(ScaleGUITrad(28), (full_bottom - top) / 10) : 0;
		const int gap = panes ? ScaleGUITrad(4) : 0;
		const int bottom = full_bottom - rsi_height - vol_height - 2 * gap;
		if (bottom - top < 20) return;
		const int vol_top = bottom + gap;
		const int vol_bottom = vol_top + vol_height;

		/* Almost black chart, the grid is only a little lighter. */
		GfxFillRect(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, GREY_SCALE(1));
		GfxFillRect(left, top, right, bottom, GREY_SCALE(1));
		if (vol_height > 0) GfxFillRect(left, vol_top, right, vol_bottom, GREY_SCALE(1));
		if (rsi_height > 0) GfxFillRect(left, full_bottom - rsi_height, right, full_bottom, GREY_SCALE(1));
		GfxDrawLine(right, r.top + 1, right, r.bottom - 1, GREY_SCALE(3)); // Line between the chart and the price axis.

		if (candles.empty()) {
			DrawString(left + 4, right - 4, top + 4, STR_STOCK_FRAME_WAIT, TC_WHITE, SA_LEFT);
			return;
		}

		const int slot = this->ChartSlot();
		const int body_half = std::max(0, slot * 3 / 10); // Half width of the volume bars (unchanged).
		/* Candle body like in TradingView: about 70% of the slot, always an odd number of pixels so the 1 pixel wick is exactly in the
		 * middle, and a gap of at least one pixel to the next candle. Zoomed far out the body becomes the 1 pixel wick. */
		int candle_width = std::max(1, slot * 7 / 10);
		if (slot >= 3) candle_width = std::min(candle_width, slot - 1);
		if (candle_width % 2 == 0) candle_width--;
		const int candle_half = (candle_width - 1) / 2;
		const size_t max_visible = static_cast<size_t>(std::max(1, (right - left) / slot));

		/* The visible candles are [first, last). Scrolled back by chart_scroll_offset candles, but never past the oldest candle.
		 * A negative chart_scroll_offset scrolls into the future instead: the newest candle moves to the left and the slots
		 * after it stay empty (whitespace), like in TradingView. At least one candle stays visible. */
		const size_t future = static_cast<size_t>(std::clamp(-this->chart_scroll_offset, 0, static_cast<int>(max_visible) - 1));
		const size_t offset = std::min(static_cast<size_t>(std::max(0, this->chart_scroll_offset)), candles.size() - 1);
		const size_t last = candles.size() - offset;
		const size_t candle_slots = max_visible - future;
		const size_t first = last > candle_slots ? last - candle_slots : 0;

		int64_t lo = INT64_MAX;
		int64_t hi = INT64_MIN;
		for (size_t i = first; i < last; i++) {
			lo = std::min(lo, candles[i].low);
			hi = std::max(hi, candles[i].high);
		}
		/* Cheap shares: when the price hardly moves, widen the range so that the candles keep a visible height. */
		if (hi == lo || hi - lo < 20) {
			hi += 10;
			lo = std::max<int64_t>(0, lo - 10);
		}
		const int64_t margin = std::max<int64_t>((hi - lo) / 10, 1);
		lo = std::max<int64_t>(lo - margin, 0);
		hi += margin;

		/* Zoom and scroll of the price range: the fitted range is shrunk or stretched around its middle, then moved up or down. */
		if (this->chart_vzoom != 1.0 || this->chart_vshift != 0.0) {
			const double fitted = static_cast<double>(hi - lo);
			const double centre = static_cast<double>(lo + hi) / 2.0 + this->chart_vshift * fitted;
			const double half = std::max(1.0, fitted * this->chart_vzoom / 2.0);
			lo = static_cast<int64_t>(std::floor(centre - half));
			hi = static_cast<int64_t>(std::ceil(centre + half));
			if (hi <= lo) hi = lo + 1;
		}
		const double range = static_cast<double>(hi - lo);

		auto to_y = [&](int64_t value) {
			return bottom - static_cast<int>(static_cast<double>(value - lo) * (bottom - top) / range);
		};

		/* Everything of the price pane is clipped to the pane: when the price range is zoomed or scrolled, candles and lines
		 * can lie outside of it, and must not be drawn over the title above or the volume pane below. */
		auto clipped_line = [&](int x1, int y1, int x2, int y2, PixelColour colour) {
			if ((y1 < top && y2 < top) || (y1 > bottom && y2 > bottom)) return;
			const int ox1 = x1, oy1 = y1, ox2 = x2, oy2 = y2;
			if (oy1 != oy2) {
				auto x_at = [&](int yc) { return ox1 + static_cast<int>(static_cast<int64_t>(ox2 - ox1) * (yc - oy1) / (oy2 - oy1)); };
				if (oy1 < top) { x1 = x_at(top); y1 = top; } else if (oy1 > bottom) { x1 = x_at(bottom); y1 = bottom; }
				if (oy2 < top) { x2 = x_at(top); y2 = top; } else if (oy2 > bottom) { x2 = x_at(bottom); y2 = bottom; }
			} else {
				y1 = y2 = std::clamp(y1, top, bottom);
			}
			GfxDrawLine(x1, y1, x2, y2, colour);
		};

		/* Horizontal grid lines with price labels. */
		for (int i = 0; i <= 4; i++) {
			const int64_t value = lo + (hi - lo) * i / 4;
			const int y = to_y(value);
			GfxDrawLine(left, y, right, y, GREY_SCALE(3));
			DrawString(right + ScaleGUITrad(6), r.right - 2, y - small_height / 2, GetString(STR_JUST_CURRENCY_SHORT, value), TC_WHITE, SA_LEFT, false, FS_SMALL);
		}

		/* Texts under the chart for the candle with the number ym (the candles of the future have numbers too: they go on counting). */
		auto candle_label = [&](int64_t ym) {
			std::string label;
			const uint64_t start_min = static_cast<uint64_t>(ym) * static_cast<uint64_t>(GameTimeframeMinutes(this->frame));
			switch (this->frame) {
				case GameTimeframe::M5:
					if (start_min % 30 == 0) {
						label = start_min % (24 * 60) == 0 ? fmt::format("D{}", start_min / (24 * 60) + 1) : fmt::format("{:02}:{:02}", (start_min / 60) % 24, start_min % 60);
					}
					break;
				case GameTimeframe::H1:
				case GameTimeframe::H4:
				case GameTimeframe::H12: {
					const uint64_t start_hour = start_min / 60;
					const uint64_t label_every = this->frame == GameTimeframe::H1 ? 6 : this->frame == GameTimeframe::H4 ? 24 : 96;
					if (start_hour % label_every == 0) {
						label = (this->frame == GameTimeframe::H1 && start_hour % 24 != 0) ?
								fmt::format("{:02}:00", start_hour % 24) : fmt::format("D{}", start_hour / 24 + 1);
					}
					break;
				}
				case GameTimeframe::D1:
					if (ym % 7 == 0) label = fmt::format("D{}", ym + 1);
					break;
				case GameTimeframe::W1:
					if (ym % 4 == 0) label = fmt::format("W{}", ym + 1);
					break;
				case GameTimeframe::MN:
					if (ym % 6 == 0) label = fmt::format("M{}", ym + 1);
					break;
				case GameTimeframe::Y1:
					if (ym % 3 == 0) label = fmt::format("{}", GameTimeManager::Get().GetSaveState().start_year + ym);
					break;
				default:
					break;
			}
			return label;
		};
		auto candle_tag = [&](int64_t ym) {
			std::string tag;
			const uint64_t start_min = static_cast<uint64_t>(ym) * static_cast<uint64_t>(GameTimeframeMinutes(this->frame));
			switch (this->frame) {
				case GameTimeframe::M5:
				case GameTimeframe::H1:
				case GameTimeframe::H4:
				case GameTimeframe::H12:
					tag = fmt::format("D{} {:02}:{:02}", start_min / (24 * 60) + 1, (start_min / 60) % 24, start_min % 60);
					break;
				case GameTimeframe::D1: tag = fmt::format("D{}", ym + 1); break;
				case GameTimeframe::W1: tag = fmt::format("W{}", ym + 1); break;
				case GameTimeframe::MN: tag = fmt::format("M{}", ym + 1); break;
				case GameTimeframe::Y1: tag = fmt::format("{}", GameTimeManager::Get().GetSaveState().start_year + ym); break;
				default: break;
			}
			return tag;
		};

		/* Candles, the newest at the right edge, or when the chart is scrolled into the future, followed by empty slots. */
		const int count = static_cast<int>(last - first);
		const int x0 = right - (count + static_cast<int>(future)) * slot;
		int last_label_x = INT32_MIN / 2; // X of the last label that was drawn.
		/* A label is about 40 pixels wide: when the candles are narrow (zoomed out), only labels that have room are drawn. */
		auto draw_label = [&](int cx, const std::string &label) {
			if (label.empty() || cx - last_label_x < ScaleGUITrad(44)) return;
			last_label_x = cx;
			GfxDrawLine(cx, full_bottom, cx, full_bottom + ScaleGUITrad(3), PC_WHITE);
			DrawString(cx - ScaleGUITrad(20), cx + ScaleGUITrad(20), full_bottom + ScaleGUITrad(3), label, TC_WHITE, SA_HOR_CENTER, false, FS_SMALL);
		};
		for (int i = 0; i < count; i++) {
			const StockCandle &c = candles[first + i];
			const int cx = x0 + i * slot + slot / 2;
			const PixelColour colour = c.close >= c.open ? PC_GREEN : PC_RED;

			clipped_line(cx, to_y(c.high), cx, to_y(c.low), colour);

			const int body_top = to_y(std::max(c.open, c.close));
			int body_bottom = to_y(std::min(c.open, c.close));
			if (body_bottom < body_top) body_bottom = body_top; // Open equal to close (doji): the body is a line of 1 pixel, it never disappears.
			if (body_bottom >= top && body_top <= bottom) GfxFillRect(cx - candle_half, std::clamp(body_top, top, bottom), cx + candle_half, std::clamp(body_bottom, top, bottom), colour);

			/* One small tick for every candle. */
			GfxDrawLine(cx, full_bottom, cx, full_bottom + ScaleGUITrad(2), PC_WHITE);

			/* Labels in game time. Each label is wider than one candle, so only some candles get one: every 30 minutes on the 5m chart,
			 * every 6 hours on the 1h chart (the time, or the day at midnight), every day on the 4h chart, every 4 days on the 12h chart,
			 * every 7 days on the day chart, every 4 weeks on the week chart, every 6 months on the month chart and every 3 years on the year chart. */
			draw_label(cx, candle_label(c.ym));
		}

		/* The free space after the newest candle (only when the chart is scrolled into the future): a tick and a label for the times to come. */
		if (future > 0) {
			const int64_t newest_ym = candles[last - 1].ym;
			for (int k = 0; k < static_cast<int>(future); k++) {
				const int cx = x0 + (count + k) * slot + slot / 2;
				GfxDrawLine(cx, full_bottom, cx, full_bottom + ScaleGUITrad(2), PC_WHITE);
				draw_label(cx, candle_label(newest_ym + 1 + k));
			}
		}

		/* Position lines: the entry price (green), Stop Loss (red) and Take Profit (blue) of the open positions of the local
		 * company in this symbol, as dashed horizontal lines. Levels outside of the visible price range are not drawn. */
		if (Company::IsValidID(_local_company)) {
			auto dashed_line = [&](int64_t price, PixelColour colour) {
				const int y = to_y(price);
				if (y < top || y > bottom) return;
				const int dash = std::max(1, ScaleGUITrad(4));
				for (int x = left; x < right; x += 2 * dash) GfxDrawLine(x, y, std::min(x + dash - 1, right), y, colour);
			};
			for (const StockOrder &p : _stock_market.positions) {
				if (p.owner != _local_company.base() || p.target != this->selected.base() || p.wallet != GetStockActiveWallet()) continue;
				dashed_line(p.price, PC_GREEN);
				if (p.sl > 0) dashed_line(p.sl, PC_RED);
				if (p.tp > 0) dashed_line(p.tp, PC_LIGHT_BLUE);
			}
		}

		/* Two simple moving averages of the closing price: a fast one (yellow) and a slow one (orange). */
		auto draw_sma = [&](size_t period, PixelColour colour) {
			const std::vector<int64_t> sma = ComputeSMA(candles, period);
			int prev_x = 0;
			int prev_y = 0;
			bool have_prev = false;
			for (size_t i = first; i < last; i++) {
				if (i + 1 < period) {
					have_prev = false;
					continue;
				}
				const int x = x0 + static_cast<int>(i - first) * slot + slot / 2;
				const int y = to_y(sma[i]);
				if (have_prev) clipped_line(prev_x, prev_y, x, y, colour);
				prev_x = x;
				prev_y = y;
				have_prev = true;
			}
		};
		draw_sma(STOCK_SMA_SLOW, PC_ORANGE);
		draw_sma(STOCK_SMA_FAST, PC_YELLOW);
		DrawString(left + ScaleGUITrad(4), right - 4, top + small_height + ScaleGUITrad(4), fmt::format("SMA {}", STOCK_SMA_FAST), TC_YELLOW, SA_LEFT, false, FS_SMALL);
		DrawString(left + ScaleGUITrad(54), right - 4, top + small_height + ScaleGUITrad(4), fmt::format("SMA {}", STOCK_SMA_SLOW), TC_ORANGE, SA_LEFT, false, FS_SMALL);

		/* Last price: a dashed line across the chart and a tag on the price axis, green when the newest candle is up, red when down. */
		{
			const StockCandle &newest = candles.back();
			const PixelColour tag_colour = newest.close >= newest.open ? PC_GREEN : PC_RED;
			const int y = to_y(newest.close);
			if (y >= top && y <= bottom) {
				const int dash = std::max(1, ScaleGUITrad(2));
				for (int x = left; x < right; x += 2 * dash) GfxDrawLine(x, y, std::min(x + dash - 1, right), y, tag_colour);
				GfxFillRect(right + 1, y - small_height / 2 - 1, r.right - 2, y + small_height / 2 + 1, tag_colour);
				DrawString(right + ScaleGUITrad(6), r.right - 2, y - small_height / 2, GetString(STR_JUST_CURRENCY_SHORT, newest.close), TC_WHITE, SA_LEFT, false, FS_SMALL);
			}
		}

		/* The line above the chart: Open, High, Low and Close of the candle under the mouse (or the newest one) and its change. */
		{
			size_t shown = last - 1;
			if (this->hover_x >= left && this->hover_x <= right && count > 0) {
				const int idx = (this->hover_x - x0) / slot;
				if (idx >= 0 && idx < count) shown = first + static_cast<size_t>(idx);
			}
			const StockCandle &c = candles[shown];
			const int64_t prev_close = shown > 0 ? candles[shown - 1].close : c.open;
			const double change = prev_close > 0 ? static_cast<double>(c.close - prev_close) * 100.0 / static_cast<double>(prev_close) : 0.0;
			const std::string ohlc = fmt::format("O {}  H {}  L {}  C {}  {:+.2f}%",
					GetString(STR_JUST_CURRENCY_SHORT, c.open), GetString(STR_JUST_CURRENCY_SHORT, c.high),
					GetString(STR_JUST_CURRENCY_SHORT, c.low), GetString(STR_JUST_CURRENCY_SHORT, c.close), change);
			DrawString(left + ScaleGUITrad(4), right - 4, top + ScaleGUITrad(2), ohlc, c.close >= c.open ? TC_GREEN : TC_RED, SA_LEFT, false, FS_SMALL);
		}

		/* Volume bars under the price: the shares traded by the orders of the players in every candle. */
		if (vol_height > 0) {
			int64_t max_volume = 0;
			for (size_t i = first; i < last; i++) max_volume = std::max(max_volume, candles[i].volume);
			DrawString(right + ScaleGUITrad(6), r.right - 2, vol_top, std::string("Vol"), TC_GREY, SA_LEFT, false, FS_SMALL);
			if (max_volume > 0) {
				DrawString(right + ScaleGUITrad(6), r.right - 2, vol_top + small_height, StockVolumeToString(static_cast<uint32_t>(std::min<int64_t>(max_volume, UINT32_MAX))), TC_WHITE, SA_LEFT, false, FS_SMALL);
				for (int i = 0; i < count; i++) {
					const StockCandle &c = candles[first + i];
					if (c.volume <= 0) continue;
					const int cx = x0 + i * slot + slot / 2;
					const int h = std::max<int>(1, static_cast<int>(c.volume * (vol_height - 2) / max_volume));
					GfxFillRect(cx - body_half, vol_bottom - h, cx + body_half, vol_bottom - 1, c.close >= c.open ? PC_GREEN : PC_RED);
				}
			} else {
				DrawString(left + ScaleGUITrad(4), right - 4, vol_top + ScaleGUITrad(2), std::string("No trades in this period"), TC_GREY, SA_LEFT, false, FS_SMALL);
			}
		}

		/* RSI over 14 candles in the lower pane, with the 30 and 70 levels. */
		if (rsi_height > 0) {
			static constexpr size_t RSI_PERIOD = 14;
			const int rsi_top = full_bottom - rsi_height;
			auto rsi_y = [&](int64_t value) {
				return full_bottom - static_cast<int>(value * (full_bottom - rsi_top) / 100);
			};

			for (const int level : {30, 70}) {
				const int y = rsi_y(level);
				GfxDrawLine(left, y, right, y, GREY_SCALE(3));
				DrawString(right + ScaleGUITrad(6), r.right - 2, y - small_height / 2, GetString(STR_JUST_INT, level), TC_WHITE, SA_LEFT, false, FS_SMALL);
			}

			int prev_rx = 0;
			int prev_ry = 0;
			bool have_prev_rsi = false;
			for (size_t i = first; i < last; i++) {
				if (i < RSI_PERIOD) {
					have_prev_rsi = false;
					continue;
				}
				int64_t gain = 0;
				int64_t loss = 0;
				for (size_t k = i + 1 - RSI_PERIOD; k <= i; k++) {
					const int64_t change = candles[k].close - candles[k - 1].close;
					if (change > 0) {
						gain += change;
					} else {
						loss -= change;
					}
				}
				const int64_t total = gain + loss;
				const int64_t rsi = total > 0 ? gain * 100 / total : 50;
				const int x = x0 + static_cast<int>(i - first) * slot + slot / 2;
				const int y = rsi_y(rsi);
				if (have_prev_rsi) GfxDrawLine(prev_rx, prev_ry, x, y, PC_LIGHT_BLUE);
				prev_rx = x;
				prev_ry = y;
				have_prev_rsi = true;
			}
		}

		/* Crosshair: dashed lines through the mouse position, with the price on the axis and the time under the chart. The vertical
		 * line snaps to the middle of the candle under the mouse. */
		if (this->hover_x >= left && this->hover_x <= right && this->hover_y >= top && this->hover_y <= full_bottom) {
			/* The slots of the free space after the newest candle count too: the crosshair can be there, and shows the time to come. */
			const int total_slots = count + static_cast<int>(future);
			const int idx = std::clamp((this->hover_x - x0) / slot, 0, total_slots - 1);
			if (this->hover_x >= x0) {
				const int64_t hover_ym = idx >= count ? static_cast<int64_t>(candles[last - 1].ym) + 1 + (idx - count) : static_cast<int64_t>(candles[first + static_cast<size_t>(idx)].ym);
				const int hx = x0 + idx * slot + slot / 2;
				const int dash = std::max(1, ScaleGUITrad(3));
				for (int y = top; y < full_bottom; y += 2 * dash) GfxDrawLine(hx, y, hx, std::min(y + dash - 1, full_bottom), GREY_SCALE(10));
				for (int x = left; x < right; x += 2 * dash) GfxDrawLine(x, this->hover_y, std::min(x + dash - 1, right), this->hover_y, GREY_SCALE(10));

				/* Price tag, only while the mouse is in the price pane. */
				if (this->hover_y <= bottom) {
					const int64_t value = lo + static_cast<int64_t>(static_cast<double>(bottom - this->hover_y) * range / (bottom - top));
					GfxFillRect(right + 1, this->hover_y - small_height / 2 - 1, r.right - 2, this->hover_y + small_height / 2 + 1, GREY_SCALE(6));
					DrawString(right + ScaleGUITrad(6), r.right - 2, this->hover_y - small_height / 2, GetString(STR_JUST_CURRENCY_SHORT, value), TC_WHITE, SA_LEFT, false, FS_SMALL);
				}

				/* Time tag under the chart, in game time. */
				const std::string tag = candle_tag(hover_ym);
				if (!tag.empty()) {
					const int half_w = ScaleGUITrad(34);
					GfxFillRect(hx - half_w, full_bottom + 1, hx + half_w, r.bottom - 1, GREY_SCALE(6));
					DrawString(hx - half_w, hx + half_w, full_bottom + ScaleGUITrad(2), tag, TC_WHITE, SA_HOR_CENTER, false, FS_SMALL);
				}
			}
		}
	}

	/** The toolbox in history mode: the closed trades of the local company, the newest first. */
	void DrawHistory(const Rect &r) const
	{
		const int row_h = this->RowHeight();
		const int header_h = this->HeaderHeight();

		/* The same column edges as the open trades, with the dates of opening and closing. */
		const int c_id = r.left + ScaleGUITrad(4);
		const int c_symbol = r.left + ScaleGUITrad(44);
		const int c_type = r.left + ScaleGUITrad(120);
		const int c_volume = r.left + ScaleGUITrad(230);
		const int c_open = r.left + ScaleGUITrad(300);
		const int c_close = r.left + ScaleGUITrad(370);
		const int c_opened = r.left + ScaleGUITrad(440);
		const int c_date = r.left + ScaleGUITrad(510);
		const int c_reason = r.left + ScaleGUITrad(580);
		const int c_profit = r.left + ScaleGUITrad(680);

		int y = r.top + ScaleGUITrad(2);
		DrawString(c_id, c_symbol, y, STR_STOCK_COL_ID, TC_BLACK, SA_LEFT);
		DrawString(c_symbol, c_type, y, STR_STOCK_COL_SYMBOL, TC_BLACK, SA_LEFT);
		DrawString(c_type, c_volume - ScaleGUITrad(60), y, STR_STOCK_COL_TYPE, TC_BLACK, SA_LEFT);
		DrawString(c_volume - ScaleGUITrad(60), c_volume, y, STR_STOCK_COL_VOLUME, TC_BLACK, SA_RIGHT);
		DrawString(c_volume, c_open, y, STR_STOCK_COL_OPEN, TC_BLACK, SA_RIGHT);
		DrawString(c_open, c_close, y, STR_STOCK_COL_CLOSE_PRICE, TC_BLACK, SA_RIGHT);
		DrawString(c_close, c_opened, y, std::string("Opened"), TC_BLACK, SA_RIGHT);
		DrawString(c_opened, c_date, y, STR_STOCK_COL_CLOSED, TC_BLACK, SA_RIGHT);
		DrawString(c_date, c_reason, y, STR_STOCK_COL_REASON, TC_BLACK, SA_RIGHT);
		DrawString(c_reason, c_profit, y, STR_STOCK_COL_PROFIT, TC_BLACK, SA_RIGHT);
		GfxDrawLine(r.left + 1, r.top + header_h - 2, r.right - 1, r.top + header_h - 2, GREY_SCALE(4));

		const std::vector<const StockClosedTrade *> rows = this->GetHistoryRows();
		if (rows.empty()) {
			DrawString(c_id, r.right - 4, r.top + header_h + 2, STR_STOCK_HISTORY_EMPTY, TC_FROMSTRING, SA_LEFT);
			return;
		}

		const size_t first = this->vscroll->GetPosition();
		const size_t last = std::min<size_t>(rows.size(), first + this->vscroll->GetCapacity());
		y = r.top + header_h;
		for (size_t i = first; i < last; i++, y += row_h) {
			const StockClosedTrade &t = *rows[i];
			const CompanyID target{t.target};

			DrawString(c_id, c_symbol, y, GetString(STR_JUST_INT, t.id), TC_WHITE, SA_LEFT);
			DrawString(c_symbol, c_type, y, IsStockSymbolValid(target.base()) ? GetStockSymbolShortName(target.base()) : GetString(STR_STOCK_HISTORY_GONE), TC_WHITE, SA_LEFT);
			DrawString(c_type, c_volume - ScaleGUITrad(60), y, GetStockOrderTypeString(t.type), IsStockBuy(t.type) ? TC_LIGHT_BLUE : TC_ORANGE, SA_LEFT);
			DrawString(c_volume - ScaleGUITrad(60), c_volume, y, StockVolumeToString(t.volume), TC_WHITE, SA_RIGHT);
			DrawString(c_volume, c_open, y, GetString(STR_JUST_CURRENCY_LONG, t.open_price), TC_WHITE, SA_RIGHT);
			DrawString(c_open, c_close, y, GetString(STR_JUST_CURRENCY_LONG, t.close_price), TC_WHITE, SA_RIGHT);
			DrawString(c_close, c_opened, y, t.open_ym > 0 ? StockMonthToString(t.open_ym) : std::string("-"), TC_WHITE, SA_RIGHT);
			DrawString(c_opened, c_date, y, StockMonthToString(t.closed_ym), TC_WHITE, SA_RIGHT);
			DrawString(c_date, c_reason, y, GetStockCloseReasonString(t.reason), t.reason == StockCloseReason::StopOut ? TC_RED : TC_WHITE, SA_RIGHT);
			DrawString(c_reason, c_profit, y, GetString(STR_JUST_CURRENCY_LONG, t.profit), t.profit >= 0 ? TC_GREEN : TC_RED, SA_RIGHT);
		}
	}

	/** The toolbox in portfolio mode: the results of the local company and its profit per month. */
	void DrawPortfolio(const Rect &r) const
	{
		const int row_h = this->RowHeight();
		const int header_h = this->HeaderHeight();
		const int left = r.left + ScaleGUITrad(4);
		if (!Company::IsValidID(_local_company)) {
			DrawString(left, r.right - 4, r.top + ScaleGUITrad(2), STR_STOCK_SPECTATOR, TC_FROMSTRING, SA_LEFT);
			return;
		}

		const StockStats &s = _stock_market.stats[_local_company.base()];
		int64_t floating = 0;
		size_t open_positions = 0;
		for (const StockOrder &p : _stock_market.positions) {
			if (p.owner != _local_company.base()) continue;
			floating += GetStockProfit(p);
			open_positions++;
		}
		const int64_t win_tenths = s.closed > 0 ? static_cast<int64_t>(s.wins) * 1000 / s.closed : 0;
		const int32_t now = GetStockCurrentMonth();
		int64_t month_profit = 0;
		for (const StockMonthProfit &m : s.months) {
			if (m.ym == now) month_profit = m.profit;
		}

		int y = r.top + ScaleGUITrad(2);
		auto money = [](int64_t v) { return GetString(STR_JUST_CURRENCY_LONG, v); };
		DrawString(left, r.right - 4, y, "Total profit/loss: " + money(s.total_profit) + "     Floating: " + money(floating) + "     This month: " + money(month_profit),
				s.total_profit >= 0 ? TC_GREEN : TC_RED, SA_LEFT);
		y += row_h;
		DrawString(left, r.right - 4, y, fmt::format("Closed trades: {}     Wins: {}     Win rate: {}%     Open positions: {}", s.closed, s.wins, StockTenths(win_tenths), open_positions),
				TC_WHITE, SA_LEFT);
		y += row_h;

		const int c_month = left;
		const int c_trades = r.left + ScaleGUITrad(200);
		const int c_rate = r.left + ScaleGUITrad(300);
		const int c_profit = r.left + ScaleGUITrad(440);
		const int c_total = r.left + ScaleGUITrad(580);
		DrawString(c_month, c_trades, y, std::string("Month"), TC_BLACK, SA_LEFT);
		DrawString(c_month, c_trades, y, std::string("Trades"), TC_BLACK, SA_RIGHT);
		DrawString(c_trades, c_rate, y, std::string("Win rate"), TC_BLACK, SA_RIGHT);
		DrawString(c_rate, c_profit, y, std::string("Profit"), TC_BLACK, SA_RIGHT);
		DrawString(c_profit, c_total, y, std::string("Cumulative"), TC_BLACK, SA_RIGHT);
		GfxDrawLine(r.left + 1, y + row_h - 1, r.right - 1, y + row_h - 1, GREY_SCALE(4));
		y = r.top + header_h + (PORTFOLIO_FIXED_ROWS - 1) * row_h;

		const std::vector<StockMonthProfit> months = this->GetPortfolioMonths(); // The newest first.
		if (months.empty()) {
			DrawString(left, r.right - 4, y, std::string("No closed trades yet."), TC_GREY, SA_LEFT);
			return;
		}

		/* The cumulative profit of a month is the sum of that month and all older ones that are remembered. */
		std::vector<int64_t> cumulative(months.size(), 0);
		int64_t run = 0;
		for (size_t i = months.size(); i-- > 0;) {
			run += months[i].profit;
			cumulative[i] = run;
		}

		const size_t capacity = static_cast<size_t>(std::max(1, static_cast<int>(this->vscroll->GetCapacity())));
		const size_t visible = capacity > PORTFOLIO_FIXED_ROWS ? capacity - PORTFOLIO_FIXED_ROWS : 1;
		const size_t first = this->vscroll->GetPosition();
		const size_t last = std::min(months.size(), first + visible);
		for (size_t i = first; i < last; i++, y += row_h) {
			const StockMonthProfit &m = months[i];
			DrawString(c_month, c_trades, y, StockMonthToString(m.ym), TC_WHITE, SA_LEFT);
			DrawString(c_month, c_trades, y, GetString(STR_JUST_INT, m.trades), TC_WHITE, SA_RIGHT);
			DrawString(c_trades, c_rate, y, StockTenths(m.trades > 0 ? static_cast<int64_t>(m.wins) * 1000 / m.trades : 0) + "%", TC_WHITE, SA_RIGHT);
			DrawString(c_rate, c_profit, y, GetString(STR_JUST_CURRENCY_LONG, m.profit), m.profit >= 0 ? TC_GREEN : TC_RED, SA_RIGHT);
			DrawString(c_profit, c_total, y, GetString(STR_JUST_CURRENCY_LONG, cumulative[i]), cumulative[i] >= 0 ? TC_GREEN : TC_RED, SA_RIGHT);
		}
	}

	/** The toolbox in ranking mode: all companies by market capitalisation, price or change; click a header to sort. */
	void DrawRanking(const Rect &r) const
	{
		const int row_h = this->RowHeight();
		const int header_h = this->HeaderHeight();
		const RankColumns col(r);

		const char *mark = " v";
		int y = r.top + ScaleGUITrad(2);
		DrawString(col.rank, col.symbol, y, std::string("#"), TC_BLACK, SA_LEFT);
		DrawString(col.symbol, col.price - ScaleGUITrad(90), y, STR_STOCK_COL_SYMBOL, TC_BLACK, SA_LEFT);
		DrawString(col.symbol, col.price, y, std::string("Price") + (this->rank_sort == StockRankSort::Price ? mark : ""), TC_BLACK, SA_RIGHT);
		DrawString(col.price, col.change, y, std::string("Change") + (this->rank_sort == StockRankSort::Change ? mark : ""), TC_BLACK, SA_RIGHT);
		DrawString(col.change, col.cap, y, std::string("Market cap") + (this->rank_sort == StockRankSort::MarketCap ? mark : ""), TC_BLACK, SA_RIGHT);
		GfxDrawLine(r.left + 1, r.top + header_h - 2, r.right - 1, r.top + header_h - 2, GREY_SCALE(4));

		const std::vector<StockRankEntry> list = GetStockRanking(this->rank_sort);
		int64_t top_cap = 1;
		for (const StockRankEntry &e : list) top_cap = std::max(top_cap, e.market_cap);

		const size_t first = this->vscroll->GetPosition();
		const size_t last = std::min<size_t>(list.size(), first + this->vscroll->GetCapacity());
		const int bar_max = std::max(ScaleGUITrad(10), r.right - ScaleGUITrad(8) - col.bar);
		y = r.top + header_h;
		for (size_t i = first; i < last; i++, y += row_h) {
			const StockRankEntry &e = list[i];
			const bool mine = e.company == _local_company;
			if (e.company == this->selected) GfxFillRect(r.left + 1, y - 1, r.right - 1, y + row_h - 2, GREY_SCALE(4));

			DrawString(col.rank, col.symbol, y, GetString(STR_JUST_INT, i + 1), TC_WHITE, SA_LEFT);
			DrawString(col.symbol, col.price - ScaleGUITrad(90), y, GetString(STR_STOCK_SYMBOL, e.company), mine ? TC_GOLD : TC_WHITE, SA_LEFT);
			DrawString(col.symbol, col.price, y, GetString(STR_JUST_CURRENCY_LONG, e.price), TC_LIGHT_BLUE, SA_RIGHT);
			DrawString(col.price, col.change, y, StockTenths(e.change_tenths) + "%", e.change_tenths >= 0 ? TC_GREEN : TC_RED, SA_RIGHT);
			DrawString(col.change, col.cap, y, GetString(STR_JUST_CURRENCY_LONG, e.market_cap), TC_WHITE, SA_RIGHT);

			/* A bar shows the market cap compared to the biggest company. */
			const int w = std::max<int>(1, static_cast<int>(e.market_cap * bar_max / top_cap));
			GfxFillRect(col.bar, y + 1, col.bar + w, y + row_h - 4, mine ? PC_ORANGE : PC_LIGHT_BLUE);
		}
	}

	/** The toolbox: open positions with floating profit, then pending orders. */
	void DrawToolbox(const Rect &r) const
	{
		switch (this->view) {
			case ToolView::History:   this->DrawHistory(r); return;
			case ToolView::Portfolio: this->DrawPortfolio(r); return;
			case ToolView::Ranking:   this->DrawRanking(r); return;
			default: break;
		}

		const int row_h = this->RowHeight();
		const int header_h = this->HeaderHeight();

		/* Column edges. */
		const StockToolboxColumns col(r);
		const int c_id = col.id;
		const int c_symbol = col.symbol;
		const int c_type = col.type;
		const int c_volume = col.volume;
		const int c_open = col.open;
		const int c_sl = col.sl;
		const int c_tp = col.tp;
		const int c_trail = col.trail;
		const int c_price = col.price;
		const int c_profit = col.profit;
		const int close_left = col.close;

		int y = r.top + ScaleGUITrad(2);
		DrawString(c_id, c_symbol, y, STR_STOCK_COL_ID, TC_BLACK, SA_LEFT);
		DrawString(c_symbol, c_type, y, STR_STOCK_COL_SYMBOL, TC_BLACK, SA_LEFT);
		DrawString(c_type, c_volume - ScaleGUITrad(60), y, STR_STOCK_COL_TYPE, TC_BLACK, SA_LEFT);
		DrawString(c_volume - ScaleGUITrad(60), c_volume, y, STR_STOCK_COL_VOLUME, TC_BLACK, SA_RIGHT);
		DrawString(c_volume, c_open, y, STR_STOCK_COL_OPEN, TC_BLACK, SA_RIGHT);
		DrawString(c_open, c_sl, y, STR_STOCK_COL_SL, TC_BLACK, SA_RIGHT);
		DrawString(c_sl, c_tp, y, STR_STOCK_COL_TP, TC_BLACK, SA_RIGHT);
		DrawString(c_tp, c_trail, y, std::string("Trail"), TC_BLACK, SA_RIGHT);
		DrawString(c_trail, c_price, y, STR_STOCK_COL_PRICE, TC_BLACK, SA_RIGHT);
		DrawString(c_price, c_profit, y, STR_STOCK_COL_PROFIT, TC_BLACK, SA_RIGHT);
		GfxDrawLine(r.left + 1, r.top + header_h - 2, r.right - 1, r.top + header_h - 2, GREY_SCALE(4));

		const std::vector<const StockOrder *> rows = this->GetRows();
		if (rows.empty()) {
			DrawString(c_id, r.right - 4, r.top + header_h + 2, STR_STOCK_EMPTY, TC_FROMSTRING, SA_LEFT);
			return;
		}

		const size_t first = this->vscroll->GetPosition();
		const size_t last = std::min<size_t>(rows.size(), first + this->vscroll->GetCapacity());
		y = r.top + header_h;
		for (size_t i = first; i < last; i++, y += row_h) {
			const StockOrder &o = *rows[i];
			const CompanyID target{o.target};
			const bool is_position = o.type == StockOrderType::Buy || o.type == StockOrderType::Sell;

			DrawString(c_id, c_symbol, y, GetString(STR_JUST_INT, o.id), TC_WHITE, SA_LEFT);
			DrawString(c_symbol, c_type, y, GetStockSymbolShortName(target.base()), TC_WHITE, SA_LEFT);
			DrawString(c_type, c_volume - ScaleGUITrad(60), y, GetStockOrderTypeString(o.type), IsStockBuy(o.type) ? TC_LIGHT_BLUE : TC_ORANGE, SA_LEFT);
			DrawString(c_volume - ScaleGUITrad(60), c_volume, y, StockVolumeToString(o.volume), TC_WHITE, SA_RIGHT);
			/* The price of a pending order and the levels of every trade can be clicked to change them, which the colour shows. */
			DrawString(c_volume, c_open, y, GetString(STR_JUST_CURRENCY_LONG, o.price), is_position ? TC_WHITE : TC_LIGHT_BLUE, SA_RIGHT);
			const TextColour level_colour = TC_LIGHT_BLUE;
			DrawString(c_open, c_sl, y, o.sl != 0 ? GetString(STR_JUST_CURRENCY_LONG, o.sl) : std::string("-"), level_colour, SA_RIGHT);
			DrawString(c_sl, c_tp, y, o.tp != 0 ? GetString(STR_JUST_CURRENCY_LONG, o.tp) : std::string("-"), level_colour, SA_RIGHT);

			const int64_t now = IsStockBuy(o.type) ? GetStockBid(target) : GetStockAsk(target);
			if (is_position) DrawString(c_tp, c_trail, y, o.trail != 0 ? GetString(STR_JUST_CURRENCY_LONG, o.trail) : std::string("-"), level_colour, SA_RIGHT);
			DrawString(c_trail, c_price, y, GetString(STR_JUST_CURRENCY_LONG, now), TC_WHITE, SA_RIGHT);

			if (is_position) {
				const int64_t profit = GetStockProfit(o);
				DrawString(c_price, c_profit, y, GetString(STR_JUST_CURRENCY_LONG, profit), profit >= 0 ? TC_GREEN : TC_RED, SA_RIGHT);
			} else {
				DrawString(c_price, c_profit, y, o.expiry > 0 ? GetString(STR_STOCK_PENDING_EXPIRY, o.expiry) : GetString(STR_STOCK_PENDING), TC_GREY, SA_RIGHT);
			}

			/* The close button. */
			GfxFillRect(close_left, y, r.right - ScaleGUITrad(4), y + row_h - 3, GREY_SCALE(5));
			DrawString(close_left, r.right - ScaleGUITrad(4), y, STR_STOCK_CLOSE_X, TC_WHITE, SA_HOR_CENTER);
		}
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_stock_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY), SetStringTip(STR_STOCK_CAPTION),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_DEFSIZEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, COLOUR_LIGHT_BLUE, WID_ST_NEW_ORDER), SetMinimalSize(88, 14), SetStringTip(STR_STOCK_NEW_ORDER, STR_STOCK_TIP_NEW_ORDER),
		NWidget(WWT_TEXTBTN, COLOUR_LIGHT_BLUE, WID_ST_HISTORY), SetMinimalSize(70, 14), SetStringTip(STR_STOCK_HISTORY, STR_STOCK_TIP_HISTORY),
		/* Panels instead of buttons: a panel keeps its pressed look while its view is active, and its text is drawn in DrawWidget. */
		NWidget(WWT_PANEL, COLOUR_LIGHT_BLUE, WID_ST_PORTFOLIO), SetMinimalSize(70, 14),
		EndContainer(),
		NWidget(WWT_PANEL, COLOUR_LIGHT_BLUE, WID_ST_RANKING), SetMinimalSize(70, 14),
		EndContainer(),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_ST_DEPOSIT), SetMinimalSize(64, 14), SetStringTip(STR_STOCK_DEPOSIT, STR_STOCK_TIP_DEPOSIT),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_ST_WITHDRAW), SetMinimalSize(64, 14), SetStringTip(STR_STOCK_WITHDRAW, STR_STOCK_TIP_WITHDRAW),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_ACCOUNT), SetMinimalSize(300, 14), SetResize(1, 0), SetFill(1, 0),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_ST_WALLET_DROPDOWN), SetMinimalSize(190, 14), SetFill(0, 0), SetStringTip(STR_STOCK_WALLET, STR_STOCK_TIP_WALLET_SELECT),
		NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_ST_LEVERAGE), SetMinimalSize(130, 14), SetFill(0, 0), SetStringTip(STR_STOCK_LEVERAGE, STR_STOCK_TIP_LEVERAGE),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_ST_WALLET_NEW), SetMinimalSize(76, 14), SetStringTip(STR_STOCK_WALLET_NEW, STR_STOCK_TIP_WALLET_NEW),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_ST_WALLET_DELETE), SetMinimalSize(76, 14), SetStringTip(STR_STOCK_WALLET_DELETE, STR_STOCK_TIP_WALLET_DELETE),
		NWidget(WWT_PUSHBTN, COLOUR_GREY, WID_STOCK_CLOSE_ALL), SetMinimalSize(76, 14),
		NWidget(WWT_PANEL, COLOUR_GREY), SetFill(1, 1), SetResize(1, 0),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_PNL), SetMinimalSize(260, 16), SetFill(1, 0), SetResize(1, 0),
		EndContainer(),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_RISK), SetMinimalSize(260, 16), SetFill(1, 0), SetResize(1, 0),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_VERTICAL),
			/* The two tabs of the Market Watch. Panels, like the Portfolio and Ranking buttons: the text is drawn in DrawWidget. */
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
				NWidget(WWT_PANEL, COLOUR_LIGHT_BLUE, WID_ST_TAB_COMPANIES), SetMinimalSize(100, 14), SetFill(1, 0), SetToolTip(STR_STOCK_TIP_TAB_COMPANIES),
				EndContainer(),
				NWidget(WWT_PANEL, COLOUR_LIGHT_BLUE, WID_ST_TAB_VALUABLES), SetMinimalSize(100, 14), SetFill(1, 0), SetToolTip(STR_STOCK_TIP_TAB_VALUABLES),
				EndContainer(),
			EndContainer(),
			NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_WATCH), SetMinimalSize(200, 136), SetFill(0, 1), SetScrollbar(WID_ST_WATCH_SCROLLBAR), SetToolTip(STR_STOCK_TIP_WATCH),
			EndContainer(),
		EndContainer(),
		NWidget(NWID_VSCROLLBAR, COLOUR_GREY, WID_ST_WATCH_SCROLLBAR),
		NWidget(NWID_VERTICAL),
			NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_TITLE), SetMinimalSize(300, 16), SetResize(1, 0), SetFill(1, 0),
			EndContainer(),
			NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_SUMMARY), SetMinimalSize(300, 16), SetResize(1, 0), SetFill(1, 0), SetToolTip(STR_STOCK_TIP_SUMMARY),
			EndContainer(),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_ST_TF_M5), SetMinimalSize(40, 14), SetStringTip(STR_STOCK_TF_5M, STR_STOCK_TIP_TF_5M),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_ST_TF_DAY), SetMinimalSize(44, 14), SetStringTip(STR_STOCK_TF_1H, STR_STOCK_TIP_TF_1H),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_ST_TF_MONTH), SetMinimalSize(44, 14), SetStringTip(STR_STOCK_TF_4H, STR_STOCK_TIP_TF_4H),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_ST_TF_YEAR), SetMinimalSize(44, 14), SetStringTip(STR_STOCK_TF_12H, STR_STOCK_TIP_TF_12H),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_ST_TF_D1), SetMinimalSize(40, 14), SetStringTip(STR_STOCK_TF_D1, STR_STOCK_TIP_TF_D1),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_ST_TF_W1), SetMinimalSize(40, 14), SetStringTip(STR_STOCK_TF_W1, STR_STOCK_TIP_TF_W1),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_ST_TF_MN), SetMinimalSize(40, 14), SetStringTip(STR_STOCK_TF_MN, STR_STOCK_TIP_TF_MN),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_ST_TF_Y1), SetMinimalSize(40, 14), SetStringTip(STR_STOCK_TF_Y1, STR_STOCK_TIP_TF_Y1),
				/* A grey panel instead of a spacer: a spacer draws nothing, which leaves a hole in the window. */
				NWidget(WWT_PANEL, COLOUR_GREY), SetFill(1, 1), SetResize(1, 0),
				EndContainer(),
				NWidget(WWT_PUSHBTN, COLOUR_GREY, WID_ST_CHART_EXPAND), SetMinimalSize(60, 14),
			EndContainer(),
			NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_CHART), SetMinimalSize(CHART_WIDTH_PX, CHART_HEIGHT_PX), SetResize(1, 1), SetFill(1, 1), SetToolTip(STR_STOCK_TIP_CHART),
			EndContainer(),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_TOOLBOX), SetMinimalSize(560, 84), SetResize(1, 0), SetFill(1, 1), SetScrollbar(WID_ST_TOOL_SCROLLBAR), SetToolTip(STR_STOCK_TIP_TOOLBOX),
		EndContainer(),
		NWidget(NWID_VERTICAL),
			NWidget(NWID_VSCROLLBAR, COLOUR_GREY, WID_ST_TOOL_SCROLLBAR),
			NWidget(WWT_RESIZEBOX, COLOUR_GREY),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _stock_desc(
	WDP_AUTO, "stock_market", 880, 440,
	WC_STOCK_MARKET, WC_NONE,
	{},
	_nested_stock_widgets
);

/** Open the trading terminal, or bring it to the front when it is already open. */
void ShowStockWindow()
{
	if (BringWindowToFrontById(WC_STOCK_MARKET, 0) != nullptr) return;
	new StockWindow(_stock_desc);
}

