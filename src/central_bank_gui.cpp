/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file central_bank_gui.cpp The Central Bank window (it takes the place of the original company finances window) and the account statement window. The two can be open at the same time. */

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

/** True: the window shows Thai text (as in the design). False: the same window in English only. Change it here and build again. */
static constexpr bool USE_THAI = true;

/** The text in Thai or in English. */
static std::string CbText(const char *thai, const char *english)
{
	return USE_THAI ? std::string(thai) : std::string(english);
}

/** The window number of the bank window and of the statement window: both belong to the class #WC_CENTRAL_BANK, so both can be open together. */
static constexpr int CB_WINDOW_BANK = 0;
static constexpr int CB_WINDOW_STATEMENT = 1;

static int CbRowHeight() { return GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(2); }

/** The credit rating of a company: 1 to 5 stars and a letter, A (best) to E. The rating follows the interest rate and the penalty of the bank. */
static int CreditRating(const BankAccount &a, int64_t rate_bp, char &grade)
{
	int stars;
	if (a.in_default) {
		stars = 1;
	} else {
		const int64_t bp = a.debt > 0 ? rate_bp : BANK_RATE_GOOD_MIN_BP;
		stars = bp <= BANK_RATE_GOOD_MAX_BP ? 5 : (bp <= BANK_RATE_POOR_MIN_BP ? 4 : (bp <= 1000 ? 3 : 2));
		if (a.penalty_level > 0) stars = std::max(1, stars - 1);
	}
	grade = "EDCBA"[stars - 1];
	return stars;
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
static void DrawAccountStatement(const Rect &r)
{
	const int left = r.left + ScaleGUITrad(6);
	const int right = r.right - ScaleGUITrad(6);
	const int row_h = CbRowHeight();
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


/** The Central Bank window of the local company: the numbers on the left, the account log on the right. */
struct CentralBankWindow : Window {
	/** What the open query window is for. */
	enum class QueryKind : uint8_t {
		None,
		Borrow,
		Repay,
	};

	static constexpr int SUMMARY_ROWS = 11; ///< Rows of the left panel, with room for the two penalty lines.
	static constexpr int SEPARATORS = 4;    ///< Lines between the groups of the left panel.
	static constexpr int LOG_ROWS = 8;      ///< Lines of the account log that are shown.

	QueryKind query_kind = QueryKind::None; ///< What the open query window is for.

	/** Redraw regularly: the cash of the company changes all the time. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(500), [this](auto) {
		this->SetDirty();
	}};

	CentralBankWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(CB_WINDOW_BANK);
	}

	/** The most that can be repaid from the company cash now. */
	int64_t GetRepayable() const
	{
		if (!Company::IsValidID(_local_company)) return 0;
		return std::max<int64_t>(0, std::min<int64_t>(GetWalletBalance(_local_company, WALLET_CASH), GetCentralBankDebt(_local_company)));
	}

	void OnPaint() override
	{
		/* The button is pushed in while the statement window is open. */
		this->SetWidgetLoweredState(WID_CB_OPEN_STATEMENT, FindWindowById(WC_CENTRAL_BANK, CB_WINDOW_STATEMENT) != nullptr);
		this->DrawWidgets();
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		const int row_h = CbRowHeight();
		switch (widget) {
			case WID_CB_SUMMARY:
				size.width = std::max<uint>(size.width, ScaleGUITrad(270));
				size.height = std::max<uint>(size.height, row_h * SUMMARY_ROWS + ScaleGUITrad(10) * SEPARATORS + ScaleGUITrad(12));
				break;

			case WID_CB_LOG:
				size.width = std::max<uint>(size.width, ScaleGUITrad(300));
				size.height = std::max<uint>(size.height, row_h * (LOG_ROWS + 4) + ScaleGUITrad(14));
				break;

			default: break;
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_CB_SUMMARY: this->DrawSummary(r); break;
			case WID_CB_LOG: this->DrawLog(r); break;
			default: break;
		}
	}

	/** The left panel: cash, deposits, debt, credit limit, interest, status and credit rating. */
	void DrawSummary(const Rect &r) const
	{
		const int left = r.left + ScaleGUITrad(8);
		const int right = r.right - ScaleGUITrad(8);
		int y = r.top + ScaleGUITrad(6);
		const int row_h = CbRowHeight();
		const int sep_h = ScaleGUITrad(10);

		if (!Company::IsValidID(_local_company)) {
			DrawString(left, right, y, STR_STOCK_SPECTATOR, TC_FROMSTRING, SA_LEFT);
			return;
		}

		const Company *c = Company::Get(_local_company);
		const BankAccount &a = _central_bank.accounts[_local_company.base()];
		const int64_t limit = GetCentralBankCreditLimit(_local_company);
		const int64_t rate_bp = GetCentralBankAnnualRateBp(_local_company);
		const int64_t per_day = BankDailyInterest(a.debt, rate_bp, a.carry).interest;

		auto money = [](int64_t v) { return GetString(STR_JUST_CURRENCY_LONG, v); };
		auto row = [&](const std::string &label, const std::string &value, TextColour colour = TC_WHITE) {
			DrawString(left, right, y, label, TC_WHITE, SA_LEFT);
			DrawString(left, right, y, value, colour, SA_RIGHT);
			y += row_h;
		};
		auto separator = [&]() {
			GfxDrawLine(left, y + sep_h / 2 - 1, right, y + sep_h / 2 - 1, GREY_SCALE(4));
			y += sep_h;
		};

		/* Group 1: the cash. */
		row(CbText("เงินสด", "Cash"), money(static_cast<int64_t>(c->money)), c->money < 0 ? TC_RED : TC_GREEN);
		separator();

		/* Group 2: the money in the trading wallets and the debt. */
		row(CbText("ยอดเงินฝาก (กระเป๋าเทรด)", "Deposits (trading wallets)"), money(GetWalletsTotal(_local_company)));
		row(CbText("ยอดเงินกู้คงค้าง", "Outstanding loan"), money(a.debt), a.debt > limit ? TC_RED : TC_ORANGE);
		separator();

		/* Group 3: the credit. */
		row(CbText("วงเงินสินเชื่อ", "Credit limit"), money(limit));
		row(CbText("กู้เพิ่มได้อีก", "Can still borrow"), money(GetCentralBankAvailable(_local_company)), TC_GREEN);
		separator();

		/* Group 4: the interest. */
		row(CbText("อัตราดอกเบี้ย", "Interest rate"), fmt::format("{}.{:02}% / {}", rate_bp / 100, rate_bp % 100, CbText("ปี", "year")), a.in_default ? TC_RED : TC_WHITE);
		row(CbText("ดอกเบี้ย / วัน", "Interest / day"), money(per_day), TC_ORANGE);
		separator();

		/* Group 5: the status and the credit rating. */
		{
			const std::string text = a.in_default ? CbText("ผิดนัด (Default)", "Default") : CbText("เปิด (Active)", "Active");
			const PixelColour fill = a.in_default ? PC_RED : PC_GREEN;
			const int pad = ScaleGUITrad(8);
			const int w = GetStringBoundingBox(text).width + 2 * pad;
			DrawString(left, right, y, CbText("สถานะบัญชี", "Account status"), TC_WHITE, SA_LEFT);
			GfxFillRect(right - w, y, right, y + row_h - 2, fill);
			DrawString(right - w, right, y, text, TC_WHITE, SA_HOR_CENTER);
			y += row_h;
		}
		{
			char grade = 'A';
			const int stars = CreditRating(a, rate_bp, grade);
			const std::string letter = fmt::format("({})", grade);
			const int sz = std::max(4, row_h - ScaleGUITrad(6));
			const int gap = ScaleGUITrad(2);
			const int letter_w = GetStringBoundingBox(letter).width;
			DrawString(left, right, y, CbText("เครดิต", "Credit rating"), TC_WHITE, SA_LEFT);
			DrawString(left, right, y, letter, TC_WHITE, SA_RIGHT);
			int x = right - letter_w - ScaleGUITrad(6) - 5 * (sz + gap) + gap;
			for (int i = 0; i < 5; i++) {
				if (i < stars) {
					GfxFillRect(x, y + 2, x + sz - 1, y + 2 + sz - 1, PC_YELLOW);
				} else {
					GfxFillRect(x, y + 2, x + sz - 1, y + 2 + sz - 1, GREY_SCALE(6));
				}
				x += sz + gap;
			}
			y += row_h;
		}

		/* Warnings: only when the company is in default or still has a penalty. */
		if (a.in_default) {
			y += ScaleGUITrad(2);
			row(CbText("ผิดนัด! ดอกเบี้ยลงโทษ", "DEFAULT! penalty rate"), fmt::format("{}.{:02}%", rate_bp / 100, rate_bp % 100), TC_RED);
		}
		if (a.penalty_level > 0) {
			y += a.in_default ? 0 : ScaleGUITrad(2);
			row(fmt::format("{} {}/{}", CbText("ระดับโทษ", "Penalty level"), static_cast<int>(a.penalty_level), BANK_PENALTY_LEVEL_MAX),
				fmt::format("{} {}%", CbText("วงเงินเหลือ", "limit"), BankCreditPercentForPenalty(a.penalty_level)), a.in_default ? TC_RED : TC_ORANGE);
		}
	}

	/** The name of a line of the account log. */
	static std::string LogName(const BankLogEntry &e)
	{
		switch (e.item) {
			case LedgerItem::BankInterest:    return CbText("ดอกเบี้ย", "Interest");
			case LedgerItem::TradeCommission: return CbText("ค่าคอมมิชชัน", "Commission");
			case LedgerItem::MarginSwap:      return CbText("ค่าสวอป", "Swap fee");
			case LedgerItem::Dividends:       return CbText("เงินปันผล", "Dividend");
			case LedgerItem::RealizedProfit:  return e.amount >= 0 ? CbText("กำไรปิดสถานะ", "Profit") : CbText("ขาดทุนปิดสถานะ", "Loss");
			case LedgerItem::WalletTransfer:  return e.amount >= 0 ? CbText("โอนเข้าเงินสด", "To cash") : CbText("โอนไปกระเป๋าเทรด", "To wallet");
			case LedgerItem::BankLoan:        return e.amount >= 0 ? CbText("กู้เงิน", "Borrow") : CbText("ชำระคืน", "Repay");
			default:                          return "";
		}
	}

	/** The right panel: the latest bookings of the account, the newest first. */
	void DrawLog(const Rect &r) const
	{
		const int left = r.left + ScaleGUITrad(8);
		const int right = r.right - ScaleGUITrad(8);
		int y = r.top + ScaleGUITrad(6);
		const int row_h = CbRowHeight();

		DrawString(left, right, y, CbText("รายการบัญชี", "Account log"), TC_WHITE, SA_LEFT);
		DrawString(left, right, y, std::string("Account Log"), TC_GREY, SA_RIGHT);
		y += row_h;
		GfxDrawLine(r.left + 1, y, r.right - 1, y, GREY_SCALE(4));
		y += ScaleGUITrad(3);

		const int day_x = left;
		const int name_x = left + ScaleGUITrad(64);
		DrawString(day_x, right, y, CbText("วันที่", "Day"), TC_GREY, SA_LEFT);
		DrawString(name_x, right, y, CbText("รายการ", "Item"), TC_GREY, SA_LEFT);
		DrawString(left, right, y, CbText("จำนวน", "Amount"), TC_GREY, SA_RIGHT);
		y += row_h;
		GfxDrawLine(r.left + 1, y, r.right - 1, y, GREY_SCALE(4));
		y += ScaleGUITrad(2);

		if (!Company::IsValidID(_local_company)) return;
		const auto &log = _bank_log[_local_company.base()];

		if (log.empty()) {
			DrawString(left, right, y + row_h, CbText("ยังไม่มีรายการ", "No bookings yet"), TC_GREY, SA_HOR_CENTER);
		}

		int shown = 0;
		for (const BankLogEntry &e : log) {
			if (shown >= LOG_ROWS) break;
			if (shown % 2 == 1) GfxFillRect(r.left + 1, y - 1, r.right - 1, y + row_h - 2, GREY_SCALE(3));

			const TextColour colour = e.item == LedgerItem::BankInterest ? TC_ORANGE : (e.amount >= 0 ? TC_GREEN : TC_RED);
			DrawString(day_x, right, y, fmt::format("{} {}", CbText("วันที่", "Day"), e.day), TC_WHITE, SA_LEFT);
			DrawString(name_x, right, y, LogName(e), TC_WHITE, SA_LEFT);
			DrawString(left, right, y, (e.amount > 0 ? "+" : "") + GetString(STR_JUST_CURRENCY_LONG, e.amount), colour, SA_RIGHT);
			y += row_h;
			shown++;
		}

		/* The note at the bottom of the panel. */
		DrawString(left, right, r.bottom - row_h - ScaleGUITrad(2), CbText("* รายการล่าสุด (เริ่มใหม่เมื่อโหลดเซฟ)", "* Latest bookings (empty after a load)"), TC_GREY, SA_LEFT);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (!Company::IsValidID(_local_company)) return;

		switch (widget) {
			case WID_CB_OPEN_STATEMENT:
				/* The statement is its own window: it opens next to this one, and a second click closes it. */
				if (FindWindowById(WC_CENTRAL_BANK, CB_WINDOW_STATEMENT) != nullptr) {
					CloseWindowById(WC_CENTRAL_BANK, CB_WINDOW_STATEMENT);
				} else {
					ShowCentralBankStatementWindow();
				}
				this->SetDirty();
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

/** The account statement window: this year and last year, in four groups. It is its own window, so it can stay open next to the bank window. */
struct CentralBankStatementWindow : Window {
	static constexpr int STATEMENT_ROWS = 26; ///< Number of rows in the account statement.

	/** Redraw regularly: the numbers change all the time. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(500), [this](auto) {
		this->SetDirty();
	}};

	CentralBankStatementWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(CB_WINDOW_STATEMENT);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_CBS_PANEL) return;
		size.width = std::max<uint>(size.width, ScaleGUITrad(700));
		size.height = std::max<uint>(size.height, CbRowHeight() * STATEMENT_ROWS + ScaleGUITrad(12));
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget == WID_CBS_PANEL) DrawAccountStatement(r);
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_central_bank_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_CB_CAPTION), SetStringTip(STR_CENTRAL_BANK_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		/* Left: the numbers and the three buttons. */
		NWidget(NWID_VERTICAL),
			NWidget(WWT_PANEL, COLOUR_GREY, WID_CB_SUMMARY), SetMinimalSize(270, 120), SetResize(0, 1), SetFill(1, 1),
			EndContainer(),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_PUSHTXTBTN, COLOUR_GREEN, WID_CB_BORROW), SetMinimalSize(80, 14), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_BORROW, STR_CENTRAL_BANK_TIP_BORROW),
				NWidget(WWT_PUSHTXTBTN, COLOUR_ORANGE, WID_CB_REPAY), SetMinimalSize(80, 14), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_REPAY, STR_CENTRAL_BANK_TIP_REPAY),
				NWidget(WWT_PUSHTXTBTN, COLOUR_RED, WID_CB_REPAY_ALL), SetMinimalSize(90, 14), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_REPAY_ALL, STR_CENTRAL_BANK_TIP_REPAY_ALL),
			EndContainer(),
		EndContainer(),
		/* Right: the account log and the button of the statement window. */
		NWidget(NWID_VERTICAL),
			NWidget(WWT_PANEL, COLOUR_GREY, WID_CB_LOG), SetMinimalSize(300, 120), SetResize(1, 1), SetFill(1, 1),
			EndContainer(),
			NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_CB_OPEN_STATEMENT), SetMinimalSize(100, 14), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_STATEMENT, STR_CENTRAL_BANK_TIP_STATEMENT),
		EndContainer(),
	EndContainer(),
};

static constexpr std::initializer_list<NWidgetPart> _nested_central_bank_statement_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_CBS_CAPTION), SetStringTip(STR_CENTRAL_BANK_STATEMENT_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_CBS_PANEL), SetMinimalSize(700, 120), SetFill(1, 1), SetResize(1, 0),
	EndContainer(),
};

static WindowDesc _central_bank_desc(
	WDP_AUTO, "central_bank", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_central_bank_widgets
);

static WindowDesc _central_bank_statement_desc(
	WDP_AUTO, "central_bank_statement", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_central_bank_statement_widgets
);

/**
 * Open the Central Bank window of the local company, or bring it to the front when it is already open.
 * This is the window that replaces the original company finances window.
 */
void ShowCentralBankWindow()
{
	if (!Company::IsValidID(_local_company)) return;
	if (BringWindowToFrontById(WC_CENTRAL_BANK, CB_WINDOW_BANK) != nullptr) return;
	new CentralBankWindow(_central_bank_desc);
}

/** Open the account statement in its own window (window number 1 of #WC_CENTRAL_BANK), or bring it to the front. It can be open together with the bank window. */
void ShowCentralBankStatementWindow()
{
	if (!Company::IsValidID(_local_company)) return;
	if (BringWindowToFrontById(WC_CENTRAL_BANK, CB_WINDOW_STATEMENT) != nullptr) return;
	new CentralBankStatementWindow(_central_bank_statement_desc);
}
