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
		Borrow,       ///< Amount to borrow from the central bank.
		Repay,        ///< Amount to repay to the central bank.
	};

	/** What the toolbox shows. */
	enum class ToolView : uint8_t {
		Orders,     ///< The open positions and the pending orders.
		History,    ///< The closed trades.
		Portfolio,  ///< The portfolio summary.
		Ranking,    ///< The ranking of the companies.
		Bank,       ///< The loan at the central bank.
	};

	CompanyID selected = INVALID_OWNER; ///< Symbol (company) whose chart is shown.
	Scrollbar *vscroll = nullptr;       ///< Scrollbar of the toolbox.
	Scrollbar *watch_scroll = nullptr;  ///< Scrollbar of the Market Watch.
	ToolView view = ToolView::Orders;   ///< What the toolbox shows.
	StockRankSort rank_sort = StockRankSort::MarketCap; ///< How the ranking is sorted.
	QueryKind query_kind = QueryKind::None; ///< What the query window is asking for.
	uint32_t query_id = 0;              ///< Number of the position that the query window is about.
	bool is_dragging_chart = false;     ///< Whether the mouse button is held down on the chart.
	int drag_start_x = 0;               ///< X position (in the window) where the drag started, or where the last candle step was made.
	int chart_scroll_offset = 0;        ///< Number of candles the chart is scrolled back into the past; 0 shows the newest candles at the right edge.
	bool view_daily = false;            ///< Whether the chart shows one candle per day.
	bool view_yearly = false;           ///< Whether the chart shows one candle per year instead of one per month.

	StockWindow(WindowDesc &desc) : Window(desc)
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_ST_TOOL_SCROLLBAR);
		this->watch_scroll = this->GetScrollbar(WID_ST_WATCH_SCROLLBAR);
		this->FinishInitNested(0);
		this->UpdateTimeframeButtons();
		this->EnsureValidSelection();
		this->OnInvalidateData(0);
	}

	/** Show which timeframe button is active. */
	void UpdateTimeframeButtons()
	{
		this->SetWidgetLoweredState(WID_ST_TF_DAY, this->view_daily);
		this->SetWidgetLoweredState(WID_ST_TF_MONTH, !this->view_daily && !this->view_yearly);
		this->SetWidgetLoweredState(WID_ST_TF_YEAR, this->view_yearly);
	}

	int RowHeight() const { return GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(2); }
	int HeaderHeight() const { return this->RowHeight() + ScaleGUITrad(3); }

	/** Make sure #selected is a company that exists. */
	void EnsureValidSelection()
	{
		if (Company::IsValidID(this->selected)) return;
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
	 * The candles of the selected symbol in the active timeframe, the oldest first.
	 * In the yearly view the monthly candles of one calendar year are combined: the open of the first month,
	 * the highest high, the lowest low and the close of the last month. The year is stored as year * 12 in ym.
	 */
	std::vector<StockCandle> GetChartCandles() const
	{
		std::vector<StockCandle> months;
		if (!Company::IsValidID(this->selected)) return months;
		const size_t index = this->selected.base();
		months = _stock_market.candles[index];
		if (_stock_market.has_current[index]) months.push_back(_stock_market.current[index]);
		if (this->view_daily) {
			std::vector<StockCandle> days = _stock_market.daily[index];
			if (_stock_market.has_daily_current[index]) days.push_back(_stock_market.daily_current[index]);
			return days;
		}
		if (!this->view_yearly) return months;

		std::vector<StockCandle> years;
		for (const StockCandle &m : months) {
			const int32_t year_ym = (m.ym / 12) * 12;
			if (!years.empty() && years.back().ym == year_ym) {
				StockCandle &y = years.back();
				y.high = std::max(y.high, m.high);
				y.low = std::min(y.low, m.low);
				y.close = m.close;
				y.volume += m.volume;
			} else {
				StockCandle y{year_ym, m.open, m.high, m.low, m.close};
				y.volume = m.volume;
				years.push_back(y);
			}
		}
		return years;
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
		this->chart_scroll_offset = std::clamp(this->chart_scroll_offset, 0, std::max(0, count - 1));
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
			case ToolView::Bank:      return 0;
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
		this->watch_scroll->SetCount(Company::GetNumItems());
		this->SetDirty();
	}

	void OnPaint() override
	{
		this->EnsureValidSelection();
		this->ClampChartScroll();
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

		if (kind == QueryKind::Borrow || kind == QueryKind::Repay) {
			double amount = 0;
			if (!ParseStockNumber(*str, amount) || amount <= 0) return;
			const uint8_t wallet = GetStockActiveWallet();
			if (kind == QueryKind::Borrow) {
				Command<CMD_BANK_BORROW>::Post(STR_BANK_ERROR_CANT_BORROW, wallet, Money(StockDisplayToMoney(amount)));
			} else {
				Command<CMD_BANK_REPAY>::Post(STR_BANK_ERROR_CANT_REPAY, wallet, Money(StockDisplayToMoney(amount)));
			}
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

	/** While the mouse button is held down on the chart, scroll the chart one candle for every candle width of mouse movement. */
	void OnMouseLoop() override
	{
		if (!this->is_dragging_chart) return;

		/* The button was released (also when it was released outside of the window). */
		if (!_left_button_down) {
			this->is_dragging_chart = false;
			return;
		}

		const int slot = std::max(1, ScaleGUITrad(10)); // Must be the same as the candle width in DrawChart.
		const int delta = (_cursor.pos.x - this->left) - this->drag_start_x;
		const int steps = delta / slot; // Whole candles only; rounds towards zero.
		if (steps == 0) return;

		/* Keep the rest of the movement, so a slow drag does not lose pixels. */
		this->drag_start_x += steps * slot;

		/* Dragging to the right goes back in time, dragging to the left comes back to the newest candles. */
		const size_t count = this->GetChartCandles().size();
		const int max_offset = count > 0 ? static_cast<int>(count) - 1 : 0; // At least one candle stays visible.
		this->chart_scroll_offset = std::clamp(this->chart_scroll_offset + steps, 0, max_offset);
		this->SetDirty();
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_ST_CHART:
				this->is_dragging_chart = true;
				this->drag_start_x = pt.x;
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

			case WID_ST_WALLET: {
				/* Switch to the next wallet of the local company. */
				if (!Company::IsValidID(_local_company)) break;
				const std::vector<uint8_t> ids = _wallets.GetIds(_local_company);
				if (ids.empty()) break;
				const uint8_t current = GetStockActiveWallet();
				size_t pos = 0;
				for (size_t i = 0; i < ids.size(); i++) {
					if (ids[i] == current) pos = i;
				}
				SetStockActiveWallet(ids[(pos + 1) % ids.size()]);
				this->OnInvalidateData();
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

			case WID_ST_TF_DAY:
			case WID_ST_TF_MONTH:
			case WID_ST_TF_YEAR: {
				const bool daily = widget == WID_ST_TF_DAY;
				const bool yearly = widget == WID_ST_TF_YEAR;
				if (daily && !_stock_market.record_daily) Command<CMD_STOCK_ENABLE_DAILY>::Post(STR_STOCK_ERROR_CANT_MODIFY, true);
				if (this->view_yearly != yearly || this->view_daily != daily) {
					this->view_yearly = yearly;
					this->view_daily = daily;
					this->chart_scroll_offset = 0; // The units of the offset change with the timeframe.
				}
				this->UpdateTimeframeButtons();
				this->SetDirty();
				break;
			}

			case WID_ST_HISTORY:
			case WID_ST_PORTFOLIO:
			case WID_ST_RANKING:
			case WID_ST_BANK: {
				/* These buttons are toggles; pressing the active one goes back to the open trades. */
				const ToolView wanted = widget == WID_ST_HISTORY ? ToolView::History : widget == WID_ST_PORTFOLIO ? ToolView::Portfolio : widget == WID_ST_BANK ? ToolView::Bank : ToolView::Ranking;
				this->view = this->view == wanted ? ToolView::Orders : wanted;
				this->SetWidgetLoweredState(WID_ST_HISTORY, this->view == ToolView::History);
				this->SetWidgetLoweredState(WID_ST_PORTFOLIO, this->view == ToolView::Portfolio);
				this->SetWidgetLoweredState(WID_ST_RANKING, this->view == ToolView::Ranking);
				this->SetWidgetLoweredState(WID_ST_BANK, this->view == ToolView::Bank);
				this->vscroll->SetPosition(0);
				this->OnInvalidateData();
				break;
			}

			case WID_ST_WATCH: {
				const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_ST_WATCH);
				if (pt.y - nw->pos_y < this->HeaderHeight()) break;
				const int row = (pt.y - nw->pos_y - this->HeaderHeight()) / this->RowHeight();
				const size_t wanted = static_cast<size_t>(std::max(0, row)) + this->watch_scroll->GetPosition();
				size_t n = 0;
				for (const Company *c : Company::Iterate()) {
					if (n++ == wanted) {
						this->selected = c->index;
						this->chart_scroll_offset = 0;
						SetStockOrderSymbol(c->index);
						this->SetDirty();
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
				if (this->view == ToolView::Bank) {
					this->ClickBank(pt, nw);
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

	/** The most that the selected wallet can repay now: its balance, for a trading wallet only the free margin. */
	int64_t GetRepayable() const
	{
		if (!Company::IsValidID(_local_company)) return 0;
		const uint8_t wallet = GetStockActiveWallet();
		int64_t available = GetWalletBalance(_local_company, wallet);
		if (IsTradingWallet(_local_company, wallet)) available = std::min(available, GetStockFreeMargin(_local_company, wallet));
		return std::max<int64_t>(0, std::min(available, GetCentralBankDebt(_local_company)));
	}

	/** A click in the bank view: the rows 2, 3 and 4 are the buttons Borrow, Repay and Repay all. */
	void ClickBank(Point pt, const NWidgetBase *nw)
	{
		if (!Company::IsValidID(_local_company)) return;
		const int row = (pt.y - static_cast<int>(nw->pos_y) - ScaleGUITrad(2)) / std::max(1, this->RowHeight());
		const uint8_t wallet = GetStockActiveWallet();
		if (row == 2) {
			this->query_kind = QueryKind::Borrow;
			ShowQueryString({}, STR_STOCK_QUERY_BORROW, 24, this, CS_ALPHANUMERAL, {});
		} else if (row == 3) {
			this->query_kind = QueryKind::Repay;
			ShowQueryString({}, STR_STOCK_QUERY_REPAY, 24, this, CS_ALPHANUMERAL, {});
		} else if (row == 4) {
			const int64_t amount = this->GetRepayable();
			if (amount > 0) Command<CMD_BANK_REPAY>::Post(STR_BANK_ERROR_CANT_REPAY, wallet, Money(amount));
		}
	}

	/** The toolbox in bank mode: the loan at the central bank of the local company. */
	void DrawBank(const Rect &r) const
	{
		const int row_h = this->RowHeight();
		const int left = r.left + ScaleGUITrad(4);
		if (!Company::IsValidID(_local_company)) {
			DrawString(left, r.right - 4, r.top + ScaleGUITrad(2), STR_STOCK_SPECTATOR, TC_FROMSTRING, SA_LEFT);
			return;
		}

		const BankAccount &a = _central_bank.accounts[_local_company.base()];
		const int64_t limit = GetCentralBankCreditLimit(_local_company);
		const int64_t available = GetCentralBankAvailable(_local_company);
		const int64_t per_day = a.debt * CENTRAL_BANK_RATE_PPM_PER_DAY / STOCK_PPM;
		const int64_t rate_tenths = CENTRAL_BANK_RATE_PPM_PER_DAY * 365 / 1000; // Tenths of a percent per year.
		auto money = [](int64_t v) { return GetString(STR_JUST_CURRENCY_LONG, v); };
		const std::string wallet = GetWalletName(_local_company, GetStockActiveWallet());

		int y = r.top + ScaleGUITrad(2);
		DrawString(left, r.right - 4, y, "Central bank loan: " + money(a.debt) + "     Credit limit: " + money(limit) + "     Available: " + money(available),
				a.debt > limit ? TC_RED : TC_WHITE, SA_LEFT);
		y += row_h;
		DrawString(left, r.right - 4, y, fmt::format("Interest: {}% a year, added to the debt every day (about ", StockTenths(rate_tenths)) + money(per_day) + " a day). Added so far: " + money(a.interest_paid),
				TC_GREY, SA_LEFT);
		y += row_h;

		const int right = std::min(r.right - ScaleGUITrad(8), r.left + ScaleGUITrad(420));
		const std::pair<std::string, bool> buttons[3] = {
			{"Borrow into " + wallet + "...", available > 0},
			{"Repay from " + wallet + "...", a.debt > 0},
			{"Repay as much as possible from " + wallet + " (" + money(this->GetRepayable()) + ")", this->GetRepayable() > 0},
		};
		for (const auto &[text, enabled] : buttons) {
			GfxFillRect(left, y, right, y + row_h - 3, GREY_SCALE(5));
			DrawString(left + ScaleGUITrad(4), right - ScaleGUITrad(4), y, text, enabled ? TC_WHITE : TC_GREY, SA_LEFT);
			y += row_h;
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
		this->selected = list[row].company;
		this->chart_scroll_offset = 0;
		SetStockOrderSymbol(this->selected);
		this->SetDirty();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_ST_ACCOUNT: this->DrawAccount(r); break;
			case WID_ST_WALLET: this->DrawWallet(r); break;
			case WID_ST_WATCH:   this->DrawWatch(r); break;
			case WID_ST_TITLE:   this->DrawTitle(r); break;
			case WID_ST_SUMMARY: this->DrawSummary(r); break;
			case WID_ST_CHART:   this->DrawChart(r); break;
			case WID_ST_TOOLBOX: this->DrawToolbox(r); break;
			case WID_ST_PORTFOLIO:
				DrawString(r.left + 2, r.right - 2, CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL)), std::string("Portfolio"), TC_BLACK, SA_HOR_CENTER);
				break;
			case WID_ST_BANK:
				DrawString(r.left + 2, r.right - 2, CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL)), std::string("Bank"), TC_BLACK, SA_HOR_CENTER);
				break;
			case WID_ST_RANKING:
				DrawString(r.left + 2, r.right - 2, CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL)), std::string("Ranking"), TC_BLACK, SA_HOR_CENTER);
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

	/** The selected wallet with its balance. */
	void DrawWallet(const Rect &r) const
	{
		if (!Company::IsValidID(_local_company)) return;
		const int top = CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL));
		const uint8_t wallet = GetStockActiveWallet();
		DrawString(r.left + 4, r.right - 4, top, GetString(STR_STOCK_WALLET, GetWalletName(_local_company, wallet), GetWalletBalance(_local_company, wallet)), TC_FROMSTRING, SA_LEFT);
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
		if (!Company::IsValidID(this->selected)) return;
		const int top = CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL));
		DrawString(r.left + 4, r.right - 4, top, GetString(STR_STOCK_TITLE, this->selected, GetStockBid(this->selected), GetStockAsk(this->selected)), TC_FROMSTRING, SA_LEFT);

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
		if (!Company::IsValidID(this->selected)) return;
		const int top = CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL));
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
		if (!Company::IsValidID(this->selected)) return;

		const std::vector<StockCandle> candles = this->GetChartCandles();

		const int small_height = GetCharacterHeight(FS_SMALL);
		const int axis_width = ScaleGUITrad(64);
		const int left = r.left + axis_width;
		const int right = r.right - ScaleGUITrad(6);
		const int top = r.top + ScaleGUITrad(6);
		const int full_bottom = r.bottom - small_height - ScaleGUITrad(4);
		if (right - left < 20 || full_bottom - top < 20) return;

		/* When there is enough room, the chart has three panes: the price on top, the volume bars under it and the RSI at the bottom. */
		const bool panes = (full_bottom - top) >= ScaleGUITrad(150);
		const int rsi_height = panes ? (full_bottom - top) / 4 : 0;
		const int vol_height = panes ? (full_bottom - top) / 6 : 0;
		const int gap = panes ? ScaleGUITrad(4) : 0;
		const int bottom = full_bottom - rsi_height - vol_height - 2 * gap;
		if (bottom - top < 20) return;
		const int vol_top = bottom + gap;
		const int vol_bottom = vol_top + vol_height;

		GfxFillRect(left, top, right, bottom, GREY_SCALE(2));
		if (vol_height > 0) GfxFillRect(left, vol_top, right, vol_bottom, GREY_SCALE(2));
		if (rsi_height > 0) GfxFillRect(left, full_bottom - rsi_height, right, full_bottom, GREY_SCALE(2));

		if (candles.empty()) {
			if (this->view_daily) DrawString(left + 4, right - 4, top + 4, STR_STOCK_DAILY_WAIT, TC_WHITE, SA_LEFT);
			return;
		}

		const int slot = std::max(1, ScaleGUITrad(10));
		const int body_half = ScaleGUITrad(3);
		const size_t max_visible = static_cast<size_t>(std::max(1, (right - left) / slot));

		/* The visible candles are [first, last). Scrolled back by chart_scroll_offset candles, but never past the oldest candle. */
		const size_t offset = std::min(static_cast<size_t>(std::max(0, this->chart_scroll_offset)), candles.size() - 1);
		const size_t last = candles.size() - offset;
		const size_t first = last > max_visible ? last - max_visible : 0;

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
		const double range = static_cast<double>(hi - lo);

		auto to_y = [&](int64_t value) {
			return bottom - static_cast<int>(static_cast<double>(value - lo) * (bottom - top) / range);
		};

		/* Horizontal grid lines with price labels. */
		for (int i = 0; i <= 4; i++) {
			const int64_t value = lo + (hi - lo) * i / 4;
			const int y = to_y(value);
			GfxDrawLine(left, y, right, y, GREY_SCALE(4));
			DrawString(r.left + 2, left - 4, y - small_height / 2, GetString(STR_JUST_CURRENCY_SHORT, value), TC_WHITE, SA_RIGHT, false, FS_SMALL);
		}

		/* Candles, the newest at the right edge. */
		const int count = static_cast<int>(last - first);
		const int x0 = right - count * slot;
		for (int i = 0; i < count; i++) {
			const StockCandle &c = candles[first + i];
			const int cx = x0 + i * slot + slot / 2;
			const PixelColour colour = c.close >= c.open ? PC_GREEN : PC_RED;

			GfxDrawLine(cx, to_y(c.high), cx, to_y(c.low), PC_WHITE);

			const int body_top = to_y(std::max(c.open, c.close));
			int body_bottom = to_y(std::min(c.open, c.close));
			if (body_bottom <= body_top) body_bottom = body_top + 1;
			GfxFillRect(cx - body_half, body_top, cx + body_half, body_bottom, colour);

			/* One small tick for every candle (one month, or one year in the yearly view). */
			GfxDrawLine(cx, full_bottom, cx, full_bottom + ScaleGUITrad(2), PC_WHITE);

			if (this->view_daily) {
				/* A month label at the first day of every month; the next one is about 30 candles further. */
				const auto ymd = TimerGameEconomy::ConvertDateToYMD(TimerGameEconomy::Date{c.ym});
				if (ymd.day == 1) {
					static const char *const names[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
					GfxDrawLine(cx, full_bottom, cx, full_bottom + ScaleGUITrad(3), PC_WHITE);
					DrawString(cx - ScaleGUITrad(20), cx + ScaleGUITrad(20), full_bottom + ScaleGUITrad(3), std::string(names[ymd.month % 12]), TC_WHITE, SA_HOR_CENTER, false, FS_SMALL);
				}
				continue;
			}

			if (this->view_yearly) {
				/* A year label every five years; each label is wider than one candle. */
				const int year = c.ym / 12;
				if (year % 5 == 0) {
					GfxDrawLine(cx, full_bottom, cx, full_bottom + ScaleGUITrad(3), PC_WHITE);
					DrawString(cx - ScaleGUITrad(20), cx + ScaleGUITrad(20), full_bottom + ScaleGUITrad(3), fmt::format("{}", year), TC_WHITE, SA_HOR_CENTER, false, FS_SMALL);
				}
				continue;
			}

			/* A label every quarter (Jan, Apr, Jul, Oct); each label is wider than one candle, so not every month. */
			const int month_index = c.ym % 12; // 0 = January.
			if (month_index % 3 == 0) {
				static const char *const month_names[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
				GfxDrawLine(cx, full_bottom, cx, full_bottom + ScaleGUITrad(3), PC_WHITE);
				DrawString(cx - ScaleGUITrad(20), cx + ScaleGUITrad(20), full_bottom + ScaleGUITrad(3), std::string(month_names[month_index]), TC_WHITE, SA_HOR_CENTER, false, FS_SMALL);
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
				if (p.owner != _local_company.base() || p.target != this->selected.base()) continue;
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
				const int y = std::clamp(to_y(sma[i]), top, bottom);
				if (have_prev) GfxDrawLine(prev_x, prev_y, x, y, colour);
				prev_x = x;
				prev_y = y;
				have_prev = true;
			}
		};
		draw_sma(STOCK_SMA_SLOW, PC_ORANGE);
		draw_sma(STOCK_SMA_FAST, PC_YELLOW);
		DrawString(left + ScaleGUITrad(4), right - 4, top + ScaleGUITrad(2), fmt::format("SMA {}", STOCK_SMA_FAST), TC_YELLOW, SA_LEFT, false, FS_SMALL);
		DrawString(left + ScaleGUITrad(54), right - 4, top + ScaleGUITrad(2), fmt::format("SMA {}", STOCK_SMA_SLOW), TC_ORANGE, SA_LEFT, false, FS_SMALL);

		/* Volume bars under the price: the shares traded by the orders of the players in every candle. */
		if (vol_height > 0) {
			int64_t max_volume = 0;
			for (size_t i = first; i < last; i++) max_volume = std::max(max_volume, candles[i].volume);
			DrawString(r.left + 2, left - 4, vol_top, std::string("Vol"), TC_WHITE, SA_RIGHT, false, FS_SMALL);
			if (max_volume > 0) {
				DrawString(r.left + 2, left - 4, vol_top + small_height, StockVolumeToString(static_cast<uint32_t>(std::min<int64_t>(max_volume, UINT32_MAX))), TC_WHITE, SA_RIGHT, false, FS_SMALL);
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
				GfxDrawLine(left, y, right, y, GREY_SCALE(4));
				DrawString(r.left + 2, left - 4, y - small_height / 2, GetString(STR_JUST_INT, level), TC_WHITE, SA_RIGHT, false, FS_SMALL);
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
			DrawString(c_symbol, c_type, y, Company::IsValidID(target) ? GetString(STR_STOCK_SYMBOL, target) : GetString(STR_STOCK_HISTORY_GONE), TC_WHITE, SA_LEFT);
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
			case ToolView::Bank:      this->DrawBank(r); return;
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
			DrawString(c_symbol, c_type, y, GetString(STR_STOCK_SYMBOL, target), TC_WHITE, SA_LEFT);
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
		NWidget(WWT_PUSHTXTBTN, COLOUR_LIGHT_BLUE, WID_ST_NEW_ORDER), SetMinimalSize(100, 14), SetStringTip(STR_STOCK_NEW_ORDER, STR_STOCK_TIP_NEW_ORDER),
		NWidget(WWT_TEXTBTN, COLOUR_LIGHT_BLUE, WID_ST_HISTORY), SetMinimalSize(80, 14), SetStringTip(STR_STOCK_HISTORY, STR_STOCK_TIP_HISTORY),
		/* Panels instead of buttons: a panel keeps its pressed look while its view is active, and its text is drawn in DrawWidget. */
		NWidget(WWT_PANEL, COLOUR_LIGHT_BLUE, WID_ST_PORTFOLIO), SetMinimalSize(80, 14),
		EndContainer(),
		NWidget(WWT_PANEL, COLOUR_LIGHT_BLUE, WID_ST_RANKING), SetMinimalSize(80, 14),
		EndContainer(),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_ST_DEPOSIT), SetMinimalSize(70, 14), SetStringTip(STR_STOCK_DEPOSIT, STR_STOCK_TIP_DEPOSIT),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_ST_WITHDRAW), SetMinimalSize(70, 14), SetStringTip(STR_STOCK_WITHDRAW, STR_STOCK_TIP_WITHDRAW),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_ACCOUNT), SetMinimalSize(380, 14), SetResize(1, 0), SetFill(1, 0),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_WALLET), SetMinimalSize(230, 14), SetFill(0, 0), SetToolTip(STR_STOCK_TIP_WALLET),
		EndContainer(),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_ST_WALLET_NEW), SetMinimalSize(90, 14), SetStringTip(STR_STOCK_WALLET_NEW, STR_STOCK_TIP_WALLET_NEW),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_ST_WALLET_DELETE), SetMinimalSize(90, 14), SetStringTip(STR_STOCK_WALLET_DELETE, STR_STOCK_TIP_WALLET_DELETE),
		NWidget(WWT_PUSHBTN, COLOUR_GREY, WID_STOCK_CLOSE_ALL), SetMinimalSize(90, 14),
		NWidget(WWT_PANEL, COLOUR_LIGHT_BLUE, WID_ST_BANK), SetMinimalSize(80, 14),
		EndContainer(),
		NWidget(WWT_PANEL, COLOUR_GREY), SetFill(1, 1), SetResize(1, 0),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_WATCH), SetMinimalSize(230, 200), SetFill(0, 1), SetScrollbar(WID_ST_WATCH_SCROLLBAR), SetToolTip(STR_STOCK_TIP_WATCH),
		EndContainer(),
		NWidget(NWID_VSCROLLBAR, COLOUR_GREY, WID_ST_WATCH_SCROLLBAR),
		NWidget(NWID_VERTICAL),
			NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_TITLE), SetMinimalSize(400, 16), SetResize(1, 0), SetFill(1, 0),
			EndContainer(),
			NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_SUMMARY), SetMinimalSize(400, 16), SetResize(1, 0), SetFill(1, 0), SetToolTip(STR_STOCK_TIP_SUMMARY),
			EndContainer(),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_ST_TF_DAY), SetMinimalSize(70, 14), SetStringTip(STR_STOCK_TF_DAY, STR_STOCK_TIP_TF_DAY),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_ST_TF_MONTH), SetMinimalSize(70, 14), SetStringTip(STR_STOCK_TF_MONTH, STR_STOCK_TIP_TF_MONTH),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_ST_TF_YEAR), SetMinimalSize(70, 14), SetStringTip(STR_STOCK_TF_YEAR, STR_STOCK_TIP_TF_YEAR),
				/* A grey panel instead of a spacer: a spacer draws nothing, which leaves a hole in the window. */
				NWidget(WWT_PANEL, COLOUR_GREY), SetFill(1, 1), SetResize(1, 0),
				EndContainer(),
			EndContainer(),
			NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_CHART), SetMinimalSize(400, 184), SetResize(1, 1), SetFill(1, 1),
			EndContainer(),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_TOOLBOX), SetMinimalSize(700, 100), SetResize(1, 0), SetFill(1, 1), SetScrollbar(WID_ST_TOOL_SCROLLBAR), SetToolTip(STR_STOCK_TIP_TOOLBOX),
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

