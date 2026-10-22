/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file central_bank_gui.cpp The Central Bank window: it takes the place of the original company finances window. */

#include "stdafx.h"
#include <chrono>
#include <optional>
#include "central_bank.h"
#include "central_bank_gui.h"
#include "wallet_base.h"
#include "wallet_cmd.h"
#include "stock_gui.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "gfx_func.h"
#include "strings_func.h"
#include "textbuf_gui.h"
#include "window_func.h"
#include "window_gui.h"
#include "zoom_func.h"
#include "timer/timer.h"
#include "timer/timer_window.h"

#include "widgets/central_bank_widget.h"

#include "table/strings.h"

#include "safeguards.h"

/** The Central Bank window of the local company. */
struct CentralBankWindow : Window {
	/** What the open query window is for. */
	enum class QueryKind : uint8_t {
		None,
		Borrow,
		Repay,
	};

	static constexpr int ROWS = 8; ///< Number of rows in the summary panel.

	QueryKind query_kind = QueryKind::None; ///< What the open query window is for.

	/** Redraw regularly: the cash of the company changes all the time. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(500), [this](auto) {
		this->SetDirty();
	}};

	CentralBankWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(0);
	}

	int RowHeight() const { return GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(2); }

	/** The most that can be repaid from the company cash now. */
	int64_t GetRepayable() const
	{
		if (!Company::IsValidID(_local_company)) return 0;
		return std::max<int64_t>(0, std::min<int64_t>(GetWalletBalance(_local_company, WALLET_CASH), GetCentralBankDebt(_local_company)));
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_CB_SUMMARY) return;
		size.width = std::max<uint>(size.width, ScaleGUITrad(420));
		size.height = std::max<uint>(size.height, this->RowHeight() * ROWS + ScaleGUITrad(8));
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_CB_SUMMARY) return;

		const int left = r.left + ScaleGUITrad(6);
		const int right = r.right - ScaleGUITrad(6);
		int y = r.top + ScaleGUITrad(4);
		const int row_h = this->RowHeight();

		if (!Company::IsValidID(_local_company)) {
			DrawString(left, right, y, STR_STOCK_SPECTATOR, TC_FROMSTRING, SA_LEFT);
			return;
		}

		const Company *c = Company::Get(_local_company);
		const BankAccount &a = _central_bank.accounts[_local_company.base()];
		const int64_t limit = GetCentralBankCreditLimit(_local_company);
		const int64_t per_day = a.debt * CENTRAL_BANK_RATE_PPM_PER_DAY / STOCK_PPM;
		const int64_t liquid = GetUnborrowedLiquidCash(_local_company);

		auto money = [](int64_t v) { return GetString(STR_JUST_CURRENCY_LONG, v); };
		auto row = [&](const std::string &label, const std::string &value, TextColour colour = TC_WHITE) {
			DrawString(left, right, y, label, colour, SA_LEFT);
			DrawString(left, right, y, value, colour, SA_RIGHT);
			y += row_h;
		};

		row("Company cash", money(static_cast<int64_t>(c->money)), c->money < 0 ? TC_RED : TC_WHITE);
		row("Central bank debt", money(a.debt), a.debt > limit ? TC_RED : TC_WHITE);
		row("Credit limit", money(limit));
		row("Available to borrow", money(GetCentralBankAvailable(_local_company)));
		row("Interest per day (added to the debt)", money(per_day), TC_GREY);
		row("Unborrowed liquid cash (may be traded)", money(liquid), TC_LIGHT_BLUE);
		row("Money in trading wallets", money(GetWalletsTotal(_local_company)));
		if (c->current_loan > 0) {
			/* Only an old savegame has this; the original loan can not be increased any more. */
			row("Original OpenTTD loan (frozen)", money(static_cast<int64_t>(c->current_loan)), TC_ORANGE);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (!Company::IsValidID(_local_company)) return;

		switch (widget) {
			case WID_CB_BORROW:
				this->query_kind = QueryKind::Borrow;
				ShowQueryString({}, STR_STOCK_QUERY_BORROW, 24, this, CS_ALPHANUMERAL, {});
				break;

			case WID_CB_REPAY:
				this->query_kind = QueryKind::Repay;
				ShowQueryString({}, STR_STOCK_QUERY_REPAY, 24, this, CS_ALPHANUMERAL, {});
				break;

			case WID_CB_REPAY_ALL: {
				const int64_t amount = this->GetRepayable();
				if (amount > 0) Command<CMD_BANK_REPAY>::Post(STR_BANK_ERROR_CANT_REPAY, WALLET_CASH, Money(amount));
				break;
			}

			default: break;
		}
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		const QueryKind kind = this->query_kind;
		this->query_kind = QueryKind::None;
		if (!str.has_value() || kind == QueryKind::None || !Company::IsValidID(_local_company)) return;

		double amount = 0;
		if (!ParseStockNumber(*str, amount) || amount <= 0) return;

		/* Loan money only ever goes to, and is only repaid from, the company cash. */
		if (kind == QueryKind::Borrow) {
			Command<CMD_BANK_BORROW>::Post(STR_BANK_ERROR_CANT_BORROW, WALLET_CASH, Money(StockDisplayToMoney(amount)));
		} else {
			Command<CMD_BANK_REPAY>::Post(STR_BANK_ERROR_CANT_REPAY, WALLET_CASH, Money(StockDisplayToMoney(amount)));
		}
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_central_bank_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_CB_CAPTION), SetStringTip(STR_CENTRAL_BANK_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_CB_SUMMARY), SetMinimalSize(420, 120), SetResize(1, 0), SetFill(1, 0),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CB_BORROW), SetMinimalSize(100, 14), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_BORROW, STR_CENTRAL_BANK_TIP_BORROW),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CB_REPAY), SetMinimalSize(100, 14), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_REPAY, STR_CENTRAL_BANK_TIP_REPAY),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CB_REPAY_ALL), SetMinimalSize(100, 14), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_REPAY_ALL, STR_CENTRAL_BANK_TIP_REPAY_ALL),
	EndContainer(),
};

static WindowDesc _central_bank_desc(
	WDP_AUTO, "central_bank", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_central_bank_widgets
);

/**
 * Open the Central Bank window of the local company, or bring it to the front when it is already open.
 * This is the window that replaces the original company finances window.
 */
void ShowCentralBankWindow()
{
	if (!Company::IsValidID(_local_company)) return;
	if (BringWindowToFrontById(WC_CENTRAL_BANK, 0) != nullptr) return;
	new CentralBankWindow(_central_bank_desc);
}
