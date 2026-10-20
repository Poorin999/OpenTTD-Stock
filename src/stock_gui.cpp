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
#include "zoom_func.h"

#include "widgets/stock_widget.h"

#include "table/strings.h"

#include "safeguards.h"

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

/** Horizontal layout of the toolbox with the open trades. It is shared by the drawing and the click handling. */
struct StockToolboxColumns {
	int id;      ///< Left edge of the ticket column.
	int symbol;  ///< Left edge of the symbol column.
	int type;    ///< Left edge of the type column.
	int volume;  ///< Right edge of the volume column.
	int open;    ///< Right edge of the open price column, left edge of the S / L column.
	int sl;      ///< Right edge of the S / L column, left edge of the T / P column.
	int tp;      ///< Right edge of the T / P column, left edge of the price column.
	int price;   ///< Right edge of the price column, left edge of the profit column.
	int profit;  ///< Right edge of the profit column.
	int close;   ///< Left edge of the close button.

	explicit StockToolboxColumns(const Rect &r) :
		id(r.left + ScaleGUITrad(4)),
		symbol(r.left + ScaleGUITrad(48)),
		type(r.left + ScaleGUITrad(168)),
		volume(r.left + ScaleGUITrad(300)),
		open(r.left + ScaleGUITrad(380)),
		sl(r.left + ScaleGUITrad(450)),
		tp(r.left + ScaleGUITrad(520)),
		price(r.left + ScaleGUITrad(590)),
		profit(r.left + ScaleGUITrad(680)),
		close(r.right - ScaleGUITrad(26))
	{}
};

struct StockWindow : Window {
	/** What the open query window is for. */
	enum class QueryKind : uint8_t {
		None,         ///< No query window is open.
		CloseVolume,  ///< Volume to close of a position.
		StopLoss,     ///< New Stop Loss of a position.
		TakeProfit,   ///< New Take Profit of a position.
	};

	CompanyID selected = INVALID_OWNER; ///< Symbol (company) whose chart is shown.
	Scrollbar *vscroll = nullptr;       ///< Scrollbar of the toolbox.
	bool show_history = false;          ///< Whether the toolbox shows the closed trades instead of the open ones.
	QueryKind query_kind = QueryKind::None; ///< What the query window is asking for.
	uint32_t query_id = 0;              ///< Number of the position that the query window is about.
	bool is_dragging_chart = false;     ///< Whether the mouse button is held down on the chart.
	int drag_start_x = 0;               ///< X position (in the window) where the drag started, or where the last candle step was made.
	int chart_scroll_offset = 0;        ///< Number of candles the chart is scrolled back into the past; 0 shows the newest candles at the right edge.

	StockWindow(WindowDesc &desc) : Window(desc)
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_ST_TOOL_SCROLLBAR);
		this->FinishInitNested(0);
		this->EnsureValidSelection();
		this->OnInvalidateData(0);
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

	/** The rows of the toolbox: first the open positions, then the pending orders of the local company. */
	std::vector<const StockOrder *> GetRows() const
	{
		std::vector<const StockOrder *> rows;
		if (!Company::IsValidID(_local_company)) return rows;
		for (const StockOrder &p : _stock_market.positions) {
			if (p.owner == _local_company.base()) rows.push_back(&p);
		}
		for (const StockOrder &p : _stock_market.pending) {
			if (p.owner == _local_company.base()) rows.push_back(&p);
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

	void OnResize() override
	{
		/* The toolbox widget has no vertical resize step, so compute the number of visible rows by hand. */
		const NWidgetBase *toolbox = this->GetWidget<NWidgetBase>(WID_ST_TOOLBOX);
		this->vscroll->SetCapacity(static_cast<size_t>(std::max(1, (static_cast<int>(toolbox->current_y) - this->HeaderHeight()) / std::max(1, this->RowHeight()))));
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		this->vscroll->SetCount(this->show_history ? this->GetHistoryRows().size() : this->GetRows().size());
		this->SetDirty();
	}

	void OnPaint() override
	{
		this->EnsureValidSelection();
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

			default: break;
		}
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		const QueryKind kind = this->query_kind;
		this->query_kind = QueryKind::None;
		if (!str.has_value() || !Company::IsValidID(_local_company)) return;

		/* The position may have been closed while the query window was open. */
		const StockOrder *position = _stock_market.FindPosition(this->query_id, _local_company);
		if (position == nullptr) return;

		switch (kind) {
			case QueryKind::CloseVolume: {
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
				Command<CMD_STOCK_MODIFY>::Post(STR_STOCK_ERROR_CANT_MODIFY, position->id, Money(sl), Money(tp));
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
		int max_offset = 0;
		if (Company::IsValidID(this->selected)) {
			const size_t index = this->selected.base();
			const size_t count = _stock_market.candles[index].size() + (_stock_market.has_current[index] ? 1 : 0);
			max_offset = count > 0 ? static_cast<int>(count) - 1 : 0; // At least one candle stays visible.
		}
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

			case WID_ST_HISTORY:
				this->show_history = !this->show_history;
				this->SetWidgetLoweredState(WID_ST_HISTORY, this->show_history);
				this->vscroll->SetPosition(0);
				this->OnInvalidateData();
				break;

			case WID_ST_WATCH: {
				const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_ST_WATCH);
				if (pt.y - nw->pos_y < this->HeaderHeight()) break;
				const int row = (pt.y - nw->pos_y - this->HeaderHeight()) / this->RowHeight();
				int n = 0;
				for (const Company *c : Company::Iterate()) {
					if (n++ == row) {
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
				if (this->show_history) break; // Closed trades can not be changed.
				const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_ST_TOOLBOX);
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
				} else if (is_position && pt.x >= col.open && pt.x < col.sl) {
					this->OpenQuery(QueryKind::StopLoss, o);
				} else if (is_position && pt.x >= col.sl && pt.x < col.tp) {
					this->OpenQuery(QueryKind::TakeProfit, o);
				}
				break;
			}

			default: break;
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_ST_ACCOUNT: this->DrawAccount(r); break;
			case WID_ST_WATCH:   this->DrawWatch(r); break;
			case WID_ST_TITLE:   this->DrawTitle(r); break;
			case WID_ST_SUMMARY: this->DrawSummary(r); break;
			case WID_ST_CHART:   this->DrawChart(r); break;
			case WID_ST_TOOLBOX: this->DrawToolbox(r); break;
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
		const int64_t balance = static_cast<int64_t>(Company::Get(_local_company)->money);
		const int64_t floating = GetStockFloatingProfit(_local_company);
		const int64_t margin = GetStockUsedMargin(_local_company);
		const int64_t equity = balance + floating;
		const int64_t level = GetStockMarginLevel(_local_company);
		std::string text;
		if (level < 0) {
			text = GetString(STR_STOCK_ACCOUNT, balance, equity, margin, equity - margin);
		} else {
			/* Below the margin call level the line turns red; below the stop-out level positions are closed by force. */
			text = GetString(level < STOCK_MARGIN_CALL_LEVEL ? STR_STOCK_ACCOUNT_CALL : STR_STOCK_ACCOUNT_LEVEL, balance, equity, margin, equity - margin, level);
		}
		DrawString(r.left + 4, r.right - 4, top, text, TC_FROMSTRING, SA_LEFT);
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

		for (const Company *c : Company::Iterate()) {
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
		const size_t index = this->selected.base();

		std::vector<StockCandle> candles = _stock_market.candles[index];
		if (_stock_market.has_current[index]) candles.push_back(_stock_market.current[index]);

		const int small_height = GetCharacterHeight(FS_SMALL);
		const int axis_width = ScaleGUITrad(64);
		const int left = r.left + axis_width;
		const int right = r.right - ScaleGUITrad(6);
		const int top = r.top + ScaleGUITrad(6);
		const int full_bottom = r.bottom - small_height - ScaleGUITrad(4);
		if (right - left < 20 || full_bottom - top < 20) return;

		/* The lower quarter of the chart shows the RSI, when there is enough room for it. */
		const int rsi_height = (full_bottom - top) >= ScaleGUITrad(150) ? (full_bottom - top) / 4 : 0;
		const int gap = rsi_height > 0 ? ScaleGUITrad(4) : 0;
		const int bottom = full_bottom - rsi_height - gap;
		if (bottom - top < 20) return;

		GfxFillRect(left, top, right, bottom, GREY_SCALE(2));
		if (rsi_height > 0) GfxFillRect(left, bottom + gap, right, full_bottom, GREY_SCALE(2));

		if (candles.empty()) return;

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

			/* One small tick for every month (one candle = one month). */
			GfxDrawLine(cx, full_bottom, cx, full_bottom + ScaleGUITrad(2), PC_WHITE);

			/* A label every quarter (Jan, Apr, Jul, Oct); each label is wider than one candle, so not every month. */
			const int month_index = c.ym % 12; // 0 = January.
			if (month_index % 3 == 0) {
				static const char *const month_names[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
				GfxDrawLine(cx, full_bottom, cx, full_bottom + ScaleGUITrad(3), PC_WHITE);
				DrawString(cx - ScaleGUITrad(20), cx + ScaleGUITrad(20), full_bottom + ScaleGUITrad(3), std::string(month_names[month_index]), TC_WHITE, SA_HOR_CENTER, false, FS_SMALL);
			}
		}

		/* Moving average of the closing price over 20 candles, drawn as a yellow line. */
		static constexpr size_t MA_PERIOD = 20;
		int prev_x = 0;
		int prev_y = 0;
		bool have_prev = false;
		for (size_t i = first; i < last; i++) {
			if (i + 1 < MA_PERIOD) {
				have_prev = false;
				continue;
			}
			int64_t sum = 0;
			for (size_t k = i + 1 - MA_PERIOD; k <= i; k++) sum += candles[k].close;
			const int x = x0 + static_cast<int>(i - first) * slot + slot / 2;
			const int y = std::clamp(to_y(sum / static_cast<int64_t>(MA_PERIOD)), top, bottom);
			if (have_prev) GfxDrawLine(prev_x, prev_y, x, y, PC_YELLOW);
			prev_x = x;
			prev_y = y;
			have_prev = true;
		}

		/* RSI over 14 candles in the lower pane, with the 30 and 70 levels. */
		if (rsi_height > 0) {
			static constexpr size_t RSI_PERIOD = 14;
			const int rsi_top = bottom + gap;
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

		/* The same column edges as the open trades. */
		const int c_id = r.left + ScaleGUITrad(4);
		const int c_symbol = r.left + ScaleGUITrad(48);
		const int c_type = r.left + ScaleGUITrad(168);
		const int c_volume = r.left + ScaleGUITrad(300);
		const int c_open = r.left + ScaleGUITrad(380);
		const int c_close = r.left + ScaleGUITrad(450);
		const int c_date = r.left + ScaleGUITrad(520);
		const int c_reason = r.left + ScaleGUITrad(590);
		const int c_profit = r.left + ScaleGUITrad(680);

		int y = r.top + ScaleGUITrad(2);
		DrawString(c_id, c_symbol, y, STR_STOCK_COL_ID, TC_BLACK, SA_LEFT);
		DrawString(c_symbol, c_type, y, STR_STOCK_COL_SYMBOL, TC_BLACK, SA_LEFT);
		DrawString(c_type, c_volume - ScaleGUITrad(60), y, STR_STOCK_COL_TYPE, TC_BLACK, SA_LEFT);
		DrawString(c_volume - ScaleGUITrad(60), c_volume, y, STR_STOCK_COL_VOLUME, TC_BLACK, SA_RIGHT);
		DrawString(c_volume, c_open, y, STR_STOCK_COL_OPEN, TC_BLACK, SA_RIGHT);
		DrawString(c_open, c_close, y, STR_STOCK_COL_CLOSE_PRICE, TC_BLACK, SA_RIGHT);
		DrawString(c_close, c_date, y, STR_STOCK_COL_CLOSED, TC_BLACK, SA_RIGHT);
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
			DrawString(c_close, c_date, y, StockMonthToString(t.closed_ym), TC_WHITE, SA_RIGHT);
			DrawString(c_date, c_reason, y, GetStockCloseReasonString(t.reason), t.reason == StockCloseReason::StopOut ? TC_RED : TC_WHITE, SA_RIGHT);
			DrawString(c_reason, c_profit, y, GetString(STR_JUST_CURRENCY_LONG, t.profit), t.profit >= 0 ? TC_GREEN : TC_RED, SA_RIGHT);
		}
	}

	/** The toolbox: open positions with floating profit, then pending orders. */
	void DrawToolbox(const Rect &r) const
	{
		if (this->show_history) {
			this->DrawHistory(r);
			return;
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
		DrawString(c_tp, c_price, y, STR_STOCK_COL_PRICE, TC_BLACK, SA_RIGHT);
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
			DrawString(c_volume, c_open, y, GetString(STR_JUST_CURRENCY_LONG, o.price), TC_WHITE, SA_RIGHT);
			/* The levels of an open position can be clicked to change them, which the colour shows. */
			const TextColour level_colour = is_position ? TC_LIGHT_BLUE : TC_WHITE;
			DrawString(c_open, c_sl, y, o.sl != 0 ? GetString(STR_JUST_CURRENCY_LONG, o.sl) : std::string("-"), level_colour, SA_RIGHT);
			DrawString(c_sl, c_tp, y, o.tp != 0 ? GetString(STR_JUST_CURRENCY_LONG, o.tp) : std::string("-"), level_colour, SA_RIGHT);

			const int64_t now = IsStockBuy(o.type) ? GetStockBid(target) : GetStockAsk(target);
			DrawString(c_tp, c_price, y, GetString(STR_JUST_CURRENCY_LONG, now), TC_WHITE, SA_RIGHT);

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
		NWidget(WWT_PUSHTXTBTN, COLOUR_LIGHT_BLUE, WID_ST_NEW_ORDER), SetMinimalSize(110, 14), SetStringTip(STR_STOCK_NEW_ORDER, STR_STOCK_TIP_NEW_ORDER),
		NWidget(WWT_TEXTBTN, COLOUR_LIGHT_BLUE, WID_ST_HISTORY), SetMinimalSize(110, 14), SetStringTip(STR_STOCK_HISTORY, STR_STOCK_TIP_HISTORY),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_ACCOUNT), SetMinimalSize(520, 14), SetResize(1, 0), SetFill(1, 0),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_WATCH), SetMinimalSize(230, 200), SetFill(0, 1), SetToolTip(STR_STOCK_TIP_WATCH),
		EndContainer(),
		NWidget(NWID_VERTICAL),
			NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_TITLE), SetMinimalSize(400, 16), SetResize(1, 0), SetFill(1, 0),
			EndContainer(),
			NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_SUMMARY), SetMinimalSize(400, 16), SetResize(1, 0), SetFill(1, 0), SetToolTip(STR_STOCK_TIP_SUMMARY),
			EndContainer(),
			NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_CHART), SetMinimalSize(400, 184), SetResize(1, 1), SetFill(1, 1),
			EndContainer(),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_ST_TOOLBOX), SetMinimalSize(630, 100), SetResize(1, 0), SetFill(1, 1), SetScrollbar(WID_ST_TOOL_SCROLLBAR), SetToolTip(STR_STOCK_TIP_TOOLBOX),
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
