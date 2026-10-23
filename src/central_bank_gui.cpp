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
#include "core/format.hpp"
#include "company_func.h"
#include "economy_type.h"
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

	static constexpr int ROWS = 13;           ///< Number of rows in the summary panel.
	static constexpr int STATEMENT_ROWS = 26; ///< Number of rows in the account statement.

	QueryKind query_kind = QueryKind::None; ///< What the open query window is for.
	bool show_statement = false;            ///< Whether the panel shows the account statement instead of the summary.

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

	void OnPaint() override
	{
		this->SetWidgetLoweredState(WID_CB_STATEMENT, this->show_statement);
		this->DrawWidgets();
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_CB_SUMMARY) return;
		if (this->show_statement) {
			size.width = std::max<uint>(size.width, ScaleGUITrad(700));
			size.height = std::max<uint>(size.height, this->RowHeight() * STATEMENT_ROWS + ScaleGUITrad(12));
			return;
		}
		size.width = std::max<uint>(size.width, ScaleGUITrad(520));
		size.height = std::max<uint>(size.height, this->RowHeight() * ROWS + ScaleGUITrad(8));
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_CB_SUMMARY) return;
		if (this->show_statement) {
			this->DrawStatement(r);
			return;
		}

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
		const int64_t rate_bp = GetCentralBankAnnualRateBp(_local_company);
		const int64_t per_day = BankDailyInterest(a.debt, rate_bp, a.carry).interest;
		const int64_t liquid = GetUnborrowedLiquidCash(_local_company);

		auto money = [](int64_t v) { return GetString(STR_JUST_CURRENCY_LONG, v); };
		auto row = [&](const std::string &label, const std::string &value, TextColour colour = TC_WHITE) {
			DrawString(left, right, y, label, colour, SA_LEFT);
			DrawString(left, right, y, value, colour, SA_RIGHT);
			y += row_h;
		};

		row("Company cash", money(static_cast<int64_t>(c->money)), c->money < 0 ? TC_RED : TC_WHITE);
		row("Transport income this year (already in the cash)", money(FromYearlyExpenses(c, 0, {EXPENSES_TRAIN_REVENUE, EXPENSES_ROADVEH_REVENUE, EXPENSES_SHIP_REVENUE, EXPENSES_AIRCRAFT_REVENUE}).income), TC_GREEN);
		row("Central bank debt", money(a.debt), a.debt > limit ? TC_RED : TC_WHITE);
		row("Credit limit", money(limit));
		row("Available to borrow", money(GetCentralBankAvailable(_local_company)));
		row("Interest rate now (per year)", fmt::format("{}.{:02}%", rate_bp / 100, rate_bp % 100), a.in_default ? TC_RED : TC_WHITE);
		row("Interest per game day (paid from cash at 00:00)", money(per_day), TC_GREY);
		row("Unborrowed liquid cash (may be traded)", money(liquid), TC_LIGHT_BLUE);
		row("Money in trading wallets", money(GetWalletsTotal(_local_company)));

		/* Penalty status: shown only when the company is in default or still has a penalty level. */
		if (a.in_default) {
			row("DEFAULT: not enough cash for the interest", fmt::format("penalty rate {}.{:02}%", rate_bp / 100, rate_bp % 100), TC_RED);
			row("Unpaid interest is added to the debt", "pay in full to end the default", TC_RED);
		}
		if (a.penalty_level > 0) {
			const TextColour colour = a.in_default ? TC_RED : TC_ORANGE;
			row(fmt::format("Penalty level {} of {}: credit limit reduced", static_cast<int>(a.penalty_level), BANK_PENALTY_LEVEL_MAX), fmt::format("{}% of normal", BankCreditPercentForPenalty(a.penalty_level)), colour);
			if (!a.in_default) {
				const int days_left = std::max(0, BANK_PENALTY_RECOVERY_DAYS - static_cast<int>(a.clean_days));
				row("Pay the interest on time to lower the penalty", fmt::format("{} game days to the next level", days_left), TC_ORANGE);
			}
		} else if (!a.in_default && a.debt > 0) {
			row("Credit standing", "Good", TC_GREEN);
		}
	}

	/** The net cost of some kinds of expenses of the game in one year as a ledger entry: the costs are expenses, a net refund is income. */
	static LedgerEntry FromYearlyExpenses(const Company *c, size_t age, std::initializer_list<ExpensesType> types)
	{
		int64_t sum = 0;
		for (ExpensesType type : types) sum += static_cast<int64_t>(c->yearly_expenses[age][type]);
		LedgerEntry entry;
		if (sum >= 0) {
			entry.expense = sum;
		} else {
			entry.income = -sum;
		}
		return entry;
	}

	/** The account statement of the local company: this year and last year, in four groups. */
	void DrawStatement(const Rect &r) const
	{
		const int left = r.left + ScaleGUITrad(6);
		const int right = r.right - ScaleGUITrad(6);
		const int row_h = this->RowHeight();
		int y = r.top + ScaleGUITrad(4);

		if (!Company::IsValidID(_local_company)) {
			DrawString(left, right, y, STR_STOCK_SPECTATOR, TC_FROMSTRING, SA_LEFT);
			return;
		}

		const Company *c = Company::Get(_local_company);
		const int32_t year = GetLedgerCurrentYear();
		const int col_w = ScaleGUITrad(96);
		const int pad = ScaleGUITrad(4);
		/* Right edges of the four number columns: this year in and out, last year in and out. */
		const int edge[4] = {right - 3 * col_w, right - 2 * col_w, right - col_w, right};

		auto money = [](int64_t v) { return v == 0 ? std::string("-") : GetString(STR_JUST_CURRENCY_SHORT, v); };
		auto cell = [&](int k, const std::string &text, TextColour colour) {
			DrawString(edge[k] - col_w + pad, edge[k] - pad, y, text, colour, SA_RIGHT);
		};

		DrawString(edge[0] - col_w, edge[1], y, std::string("This year"), TC_WHITE, SA_HOR_CENTER);
		DrawString(edge[2] - col_w, edge[3], y, std::string("Last year"), TC_WHITE, SA_HOR_CENTER);
		y += row_h;
		for (int k = 0; k < 4; k++) cell(k, k % 2 == 0 ? "In" : "Out", TC_GREY);
		y += row_h;

		auto category = [&](const char *name) {
			y += ScaleGUITrad(2);
			DrawString(left, right, y, std::string(name), TC_LIGHT_BLUE, SA_LEFT);
			y += row_h;
		};
		auto line = [&](const char *label, const LedgerEntry &now, const LedgerEntry &before) {
			DrawString(left + ScaleGUITrad(8), edge[0] - col_w, y, std::string(label), TC_WHITE, SA_LEFT);
			cell(0, money(now.income), TC_GREEN);
			cell(1, money(now.expense), TC_RED);
			cell(2, money(before.income), TC_GREEN);
			cell(3, money(before.expense), TC_RED);
			y += row_h;
		};
		auto booked = [&](const char *label, LedgerItem item) {
			line(label, GetLedgerEntry(_local_company, item, year), GetLedgerEntry(_local_company, item, year - 1));
		};
		auto yearly = [&](const char *label, std::initializer_list<ExpensesType> types) {
			line(label, FromYearlyExpenses(c, 0, types), FromYearlyExpenses(c, 1, types));
		};

		/* The income of the transport goes straight into the company cash (the game pays it when the cargo is delivered), nothing is held back. */
		category("Organic Operating Income (paid into Company Cash)");
		yearly("Trains", {EXPENSES_TRAIN_REVENUE});
		yearly("Road Vehicles", {EXPENSES_ROADVEH_REVENUE});
		yearly("Ships", {EXPENSES_SHIP_REVENUE});
		yearly("Aircraft", {EXPENSES_AIRCRAFT_REVENUE});
		yearly("Total Transport Income", {EXPENSES_TRAIN_REVENUE, EXPENSES_ROADVEH_REVENUE, EXPENSES_SHIP_REVENUE, EXPENSES_AIRCRAFT_REVENUE});

		category("Interest & Fees");
		booked("Central Bank Daily Interest", LedgerItem::BankInterest);
		booked("Stock Commission & Brokerage Fees", LedgerItem::TradeCommission);
		booked("Margin Maintenance / Swap Fees", LedgerItem::MarginSwap);

		category("Dividends & Yields");
		booked("Stock Dividend Payouts", LedgerItem::Dividends);
		booked("Capital Gains / Realized Profit", LedgerItem::RealizedProfit);

		category("Internal Transfers");
		booked("Inter-Wallet Transfer (cash in / out)", LedgerItem::WalletTransfer);
		booked("Company Injection: Loans (paid out / repaid)", LedgerItem::BankLoan);

		category("Assets & Operations");
		yearly("Vehicle & Infrastructure Running Costs", {EXPENSES_TRAIN_RUN, EXPENSES_ROADVEH_RUN, EXPENSES_AIRCRAFT_RUN, EXPENSES_SHIP_RUN, EXPENSES_PROPERTY});
		yearly("Construction & Land Purchase", {EXPENSES_CONSTRUCTION});
		yearly("Vehicle Purchases", {EXPENSES_NEW_VEHICLES});

		y += ScaleGUITrad(4);
		DrawString(left, right, y, std::string("Dividends: Out is the dividend that your own company pays. Margin: the daily swap of open positions."), TC_GREY, SA_LEFT);
		y += row_h;
		DrawString(left, right, y, std::string("Operations are the yearly expenses of the game. Vehicle depreciation is not a cash expense and is not listed."), TC_GREY, SA_LEFT);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (!Company::IsValidID(_local_company)) return;

		switch (widget) {
			case WID_CB_STATEMENT:
				this->show_statement = !this->show_statement;
				this->ReInit(); // The statement is larger than the summary.
				break;

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
	NWidget(WWT_PANEL, COLOUR_GREY, WID_CB_SUMMARY), SetMinimalSize(520, 120), SetResize(1, 0), SetFill(1, 0),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_CB_STATEMENT), SetMinimalSize(100, 14), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_STATEMENT, STR_CENTRAL_BANK_TIP_STATEMENT),
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
