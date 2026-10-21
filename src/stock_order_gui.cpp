/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file stock_order_gui.cpp The New Order window of the trading terminal (market execution and pending orders). */

#include "stdafx.h"
#include "stock_gui.h"
#include "stock_base.h"
#include "stock_cmd.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "gfx_func.h"
#include "querystring_gui.h"
#include "strings_func.h"
#include "window_func.h"
#include "window_gui.h"
#include "zoom_func.h"

#include "widgets/stock_widget.h"

#include "table/strings.h"

#include "safeguards.h"

struct StockOrderWindow : Window {
	/** The pending order types, in the same order as the type buttons after the first one. */
	static constexpr StockOrderType PENDING_TYPES[] = {
		StockOrderType::BuyLimit, StockOrderType::SellLimit, StockOrderType::BuyStop, StockOrderType::SellStop,
	};

	CompanyID symbol = INVALID_OWNER; ///< Symbol (company) that is traded.
	int type_index = 0;               ///< 0 = Market Execution, 1..4 = the pending order types.

	QueryString volume_editbox{16}; ///< Volume in LOT.
	QueryString price_editbox{24};  ///< Price of a pending order.
	QueryString sl_editbox{24};     ///< Stop Loss.
	QueryString tp_editbox{24};     ///< Take Profit.
	QueryString expiry_editbox{8};  ///< Lifetime of a pending order in days; empty for an order that does not expire.

	StockOrderWindow(WindowDesc &desc, CompanyID symbol) : Window(desc), symbol(symbol)
	{
		this->CreateNestedTree();
		this->FinishInitNested(0);

		this->querystrings[WID_SO_VOLUME] = &this->volume_editbox;
		this->querystrings[WID_SO_PRICE] = &this->price_editbox;
		this->querystrings[WID_SO_SL] = &this->sl_editbox;
		this->querystrings[WID_SO_TP] = &this->tp_editbox;
		this->querystrings[WID_SO_EXPIRY] = &this->expiry_editbox;
		this->volume_editbox.text.Assign("1.00");

		this->EnsureValidSymbol();
		this->UpdateStates();
	}

	/** Whether the shares of this company may be traded: it must exist and not be the company of the local player. */
	static bool IsTradable(CompanyID company)
	{
		return Company::IsValidID(company) && company != _local_company;
	}

	/** Make sure #symbol is a tradable company; pick the first one when it is not. INVALID_OWNER when there is none. */
	void EnsureValidSymbol()
	{
		if (IsTradable(this->symbol)) return;
		this->symbol = INVALID_OWNER;
		for (const Company *c : Company::Iterate()) {
			if (!IsTradable(c->index)) continue;
			this->symbol = c->index;
			return;
		}
	}

	/** Update which buttons are pressed and usable. */
	void UpdateStates()
	{
		for (int i = 0; i < 5; i++) {
			this->SetWidgetLoweredState(WID_SO_TYPE_MARKET + i, i == this->type_index);
		}
		const bool market = this->type_index == 0;
		this->SetWidgetDisabledState(WID_SO_PRICE, market);
		this->SetWidgetDisabledState(WID_SO_EXPIRY, market);
		this->SetWidgetDisabledState(WID_SO_SELL, !market);
		this->SetWidgetDisabledState(WID_SO_BUY, !market);
		this->SetWidgetDisabledState(WID_SO_PLACE, market);
		this->SetDirty();
	}

	void SetSymbol(CompanyID company)
	{
		if (!IsTradable(company)) return;
		this->symbol = company;
		this->SetDirty();
	}

	void CycleSymbol(bool forward)
	{
		std::vector<CompanyID> ids;
		for (const Company *c : Company::Iterate()) {
			if (IsTradable(c->index)) ids.push_back(c->index);
		}
		if (ids.empty()) return;

		size_t pos = 0;
		for (size_t i = 0; i < ids.size(); i++) {
			if (ids[i] == this->symbol) pos = i;
		}
		pos = (pos + (forward ? 1 : ids.size() - 1)) % ids.size();
		this->SetSymbol(ids[pos]);
	}

	/** Volume the player typed, in hundredths of a LOT. 0 when invalid. */
	uint32_t GetVolume() const
	{
		double lots = 0;
		if (!ParseStockNumber(this->volume_editbox.text.GetText(), lots) || lots <= 0) return 0;
		const double units = std::round(lots * 100.0);
		if (units < 1 || units > STOCK_MAX_VOLUME) return 0;
		return static_cast<uint32_t>(units);
	}

	/** A price that can be left empty: 0 when empty, -1 when invalid. */
	static int64_t GetOptionalPrice(const QueryString &editbox)
	{
		double value = 0;
		const std::string_view text = editbox.text.GetText();
		if (text.find_first_not_of(' ') == std::string_view::npos) return 0;
		if (!ParseStockNumber(text, value) || value <= 0) return -1;
		return StockDisplayToMoney(value);
	}

	/** Lifetime the player typed, in days: 0 when empty (never expires), more than the maximum when invalid, so that the command refuses it. */
	uint32_t GetExpiry() const
	{
		const std::string_view text = this->expiry_editbox.text.GetText();
		if (text.find_first_not_of(' ') == std::string_view::npos) return 0;
		double days = 0;
		if (!ParseStockNumber(text, days) || days < 1 || days > STOCK_MAX_EXPIRY_DAYS) return STOCK_MAX_EXPIRY_DAYS + 1;
		return static_cast<uint32_t>(std::round(days));
	}

	void Send(StockOrderType type)
	{
		if (!Company::IsValidID(_local_company) || !IsTradable(this->symbol)) return;

		const bool market = type == StockOrderType::Buy || type == StockOrderType::Sell;
		int64_t price = 0;
		if (!market) price = GetOptionalPrice(this->price_editbox);

		Command<CMD_STOCK_OPEN>::Post(STR_STOCK_ERROR_CANT_OPEN, this->symbol, type, this->GetVolume(), Money(price),
				Money(GetOptionalPrice(this->sl_editbox)), Money(GetOptionalPrice(this->tp_editbox)), market ? 0 : this->GetExpiry(), GetStockActiveWallet());
	}

	void OnPaint() override
	{
		this->EnsureValidSymbol();
		this->DrawWidgets();
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_SO_PREV: this->CycleSymbol(false); break;
			case WID_SO_NEXT: this->CycleSymbol(true); break;

			case WID_SO_TYPE_MARKET:
			case WID_SO_TYPE_BUY_LIMIT:
			case WID_SO_TYPE_SELL_LIMIT:
			case WID_SO_TYPE_BUY_STOP:
			case WID_SO_TYPE_SELL_STOP:
				this->type_index = widget - WID_SO_TYPE_MARKET;
				this->UpdateStates();
				break;

			case WID_SO_SELL: this->Send(StockOrderType::Sell); break;
			case WID_SO_BUY:  this->Send(StockOrderType::Buy); break;
			case WID_SO_PLACE:
				if (this->type_index >= 1 && this->type_index <= 4) this->Send(PENDING_TYPES[this->type_index - 1]);
				break;

			default: break;
		}
	}

	void OnEditboxChanged([[maybe_unused]] WidgetID widget) override
	{
		this->SetWidgetDirty(WID_SO_INFO);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (gui_scope) this->SetDirty();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (!Company::IsValidID(this->symbol)) return;
		const int top = CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL));

		switch (widget) {
			case WID_SO_SYMBOL:
				DrawString(r.left + 4, r.right - 4, top, GetString(STR_STOCK_SYMBOL, this->symbol), TC_WHITE, SA_HOR_CENTER);
				break;

			case WID_SO_PRICES:
				DrawString(r.left + 4, r.right - 4, top, GetString(STR_STOCK_ORDER_PRICES, GetStockBid(this->symbol), GetStockAsk(this->symbol)), TC_FROMSTRING, SA_HOR_CENTER);
				break;

			case WID_SO_INFO: {
				const uint32_t volume = this->GetVolume();
				const bool market = this->type_index == 0;
				const bool buy = IsStockBuy(market ? StockOrderType::Buy : PENDING_TYPES[std::clamp(this->type_index, 1, 4) - 1]);
				const int64_t price = market ? GetStockExecutionPrice(this->symbol, buy, volume) : (buy ? GetStockAsk(this->symbol) : GetStockBid(this->symbol));
				const int64_t value = price * static_cast<int64_t>(volume);
				DrawString(r.left + 4, r.right - 4, top, GetString(STR_STOCK_ORDER_INFO, static_cast<int64_t>(volume), value, value / STOCK_MARGIN_DIVISOR, GetStockCommission(value)), TC_FROMSTRING, SA_LEFT);
				break;
			}

			default: break;
		}
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_stock_order_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY), SetStringTip(STR_STOCK_ORDER_CAPTION),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY),
		NWidget(NWID_VERTICAL), SetPIP(4, 3, 4),
			/* Symbol. */
			NWidget(NWID_HORIZONTAL), SetPIP(4, 2, 4),
				NWidget(WWT_TEXT, INVALID_COLOUR), SetStringTip(STR_STOCK_ORDER_LABEL_SYMBOL), SetMinimalSize(96, 14), SetFill(0, 1),
				NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SO_PREV), SetMinimalSize(22, 14), SetStringTip(STR_STOCK_PREV, STR_STOCK_TIP_PREV),
				NWidget(WWT_PANEL, COLOUR_GREY, WID_SO_SYMBOL), SetMinimalSize(160, 14), SetResize(1, 0), SetFill(1, 0),
				EndContainer(),
				NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SO_NEXT), SetMinimalSize(22, 14), SetStringTip(STR_STOCK_NEXT, STR_STOCK_TIP_NEXT),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(4, 0, 4),
				NWidget(WWT_PANEL, COLOUR_GREY, WID_SO_PRICES), SetMinimalSize(300, 14), SetResize(1, 0), SetFill(1, 0),
				EndContainer(),
			EndContainer(),
			/* Order type. */
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize), SetPIP(4, 1, 4),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_SO_TYPE_MARKET), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_STOCK_ORDER_TYPE_MARKET, STR_STOCK_TIP_TYPE),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_SO_TYPE_BUY_LIMIT), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_STOCK_TYPE_BUY_LIMIT, STR_STOCK_TIP_TYPE),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_SO_TYPE_SELL_LIMIT), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_STOCK_TYPE_SELL_LIMIT, STR_STOCK_TIP_TYPE),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_SO_TYPE_BUY_STOP), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_STOCK_TYPE_BUY_STOP, STR_STOCK_TIP_TYPE),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_SO_TYPE_SELL_STOP), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_STOCK_TYPE_SELL_STOP, STR_STOCK_TIP_TYPE),
			EndContainer(),
			/* Volume, price, SL, TP. */
			NWidget(NWID_HORIZONTAL), SetPIP(4, 2, 4),
				NWidget(WWT_TEXT, INVALID_COLOUR), SetStringTip(STR_STOCK_ORDER_LABEL_VOLUME), SetMinimalSize(96, 14), SetFill(0, 1),
				NWidget(WWT_EDITBOX, COLOUR_GREY, WID_SO_VOLUME), SetMinimalSize(120, 12), SetResize(1, 0), SetFill(1, 0), SetPadding(1), SetStringTip(STR_STOCK_ORDER_HINT_VOLUME, STR_STOCK_TIP_VOLUME),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(4, 2, 4),
				NWidget(WWT_TEXT, INVALID_COLOUR), SetStringTip(STR_STOCK_ORDER_LABEL_PRICE), SetMinimalSize(96, 14), SetFill(0, 1),
				NWidget(WWT_EDITBOX, COLOUR_GREY, WID_SO_PRICE), SetMinimalSize(120, 12), SetResize(1, 0), SetFill(1, 0), SetPadding(1), SetStringTip(STR_STOCK_ORDER_HINT_PRICE, STR_STOCK_TIP_PRICE),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(4, 2, 4),
				NWidget(WWT_TEXT, INVALID_COLOUR), SetStringTip(STR_STOCK_ORDER_LABEL_SL), SetMinimalSize(96, 14), SetFill(0, 1),
				NWidget(WWT_EDITBOX, COLOUR_GREY, WID_SO_SL), SetMinimalSize(120, 12), SetResize(1, 0), SetFill(1, 0), SetPadding(1), SetStringTip(STR_STOCK_ORDER_HINT_NONE, STR_STOCK_TIP_SL),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(4, 2, 4),
				NWidget(WWT_TEXT, INVALID_COLOUR), SetStringTip(STR_STOCK_ORDER_LABEL_TP), SetMinimalSize(96, 14), SetFill(0, 1),
				NWidget(WWT_EDITBOX, COLOUR_GREY, WID_SO_TP), SetMinimalSize(120, 12), SetResize(1, 0), SetFill(1, 0), SetPadding(1), SetStringTip(STR_STOCK_ORDER_HINT_NONE, STR_STOCK_TIP_TP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(4, 2, 4),
				NWidget(WWT_TEXT, INVALID_COLOUR), SetStringTip(STR_STOCK_ORDER_LABEL_EXPIRY), SetMinimalSize(96, 14), SetFill(0, 1),
				NWidget(WWT_EDITBOX, COLOUR_GREY, WID_SO_EXPIRY), SetMinimalSize(120, 12), SetResize(1, 0), SetFill(1, 0), SetPadding(1), SetStringTip(STR_STOCK_ORDER_HINT_EXPIRY, STR_STOCK_TIP_EXPIRY),
			EndContainer(),
			/* Value and margin. */
			NWidget(NWID_HORIZONTAL), SetPIP(4, 0, 4),
				NWidget(WWT_PANEL, COLOUR_GREY, WID_SO_INFO), SetMinimalSize(300, 14), SetResize(1, 0), SetFill(1, 0),
				EndContainer(),
			EndContainer(),
			/* Execution. */
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize), SetPIP(4, 2, 4),
				NWidget(WWT_PUSHTXTBTN, COLOUR_RED, WID_SO_SELL), SetMinimalSize(100, 18), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_STOCK_ORDER_SELL, STR_STOCK_TIP_SELL),
				NWidget(WWT_PUSHTXTBTN, COLOUR_LIGHT_BLUE, WID_SO_BUY), SetMinimalSize(100, 18), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_STOCK_ORDER_BUY, STR_STOCK_TIP_BUY),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(4, 0, 4),
				NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SO_PLACE), SetMinimalSize(200, 18), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_STOCK_ORDER_PLACE, STR_STOCK_TIP_PLACE),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _stock_order_desc(
	WDP_AUTO, "stock_order", 0, 0,
	WC_STOCK_ORDER, WC_NONE,
	{},
	_nested_stock_order_widgets
);

/**
 * Open the New Order window for a symbol.
 * @param symbol Company whose shares are traded.
 */
void ShowStockOrderWindow(CompanyID symbol)
{
	StockOrderWindow *w = dynamic_cast<StockOrderWindow *>(BringWindowToFrontById(WC_STOCK_ORDER, 0));
	if (w != nullptr) {
		if (Company::IsValidID(symbol)) w->SetSymbol(symbol);
		return;
	}
	new StockOrderWindow(_stock_order_desc, symbol);
}

/**
 * Change the symbol of the New Order window, when it is open.
 * @param symbol Company whose shares are traded.
 */
void SetStockOrderSymbol(CompanyID symbol)
{
	StockOrderWindow *w = dynamic_cast<StockOrderWindow *>(FindWindowById(WC_STOCK_ORDER, 0));
	if (w != nullptr) w->SetSymbol(symbol);
}
