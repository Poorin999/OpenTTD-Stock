/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file central_bank_gui.cpp The Central Bank window (it takes the place of the original company finances window) and the account statement window. The two can be open at the same time. */

#include "stdafx.h"
#include <algorithm>
#include <chrono>
#include <optional>
#include "central_bank.h"
#include "gold_reserve.h"
#include "central_bank_gui.h"
#include "commercial_bank.h"
#include "town.h"
#include "tax.h"
#include "zone_market.h"
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
#include "game_time/game_time_manager.h"
#include "stock_dev.h"

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
static constexpr int CB_WINDOW_ARCHIVE = 2; ///< The past statements window: 10 years.
static constexpr int CB_WINDOW_POLICY = 4;  ///< The monetary policy window (3 is the buy gold window).

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

/**
 * The account statement of the local company: two periods side by side, in five groups.
 * @param r The panel.
 * @param offset 0 shows this year and last year; 1 shows last year and the year before (past statement).
 */
static void DrawAccountStatement(const Rect &r, int offset)
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
	const int32_t year = GetLedgerCurrentYear() - offset;
	const int col_w = ScaleGUITrad(116);
	const int pad = ScaleGUITrad(4);
	/* Right edges of the four number columns: first period in and out, second period in and out. */
	const int edge[4] = {right - 3 * col_w, right - 2 * col_w, right - col_w, right};

	auto money = [](int64_t v) { return v == 0 ? std::string("-") : GetString(STR_JUST_CURRENCY_LONG, v); };
	auto cell = [&](int k, const std::string &text, TextColour colour) {
		DrawString(edge[k] - col_w + pad, edge[k] - pad, y, text, colour, SA_RIGHT);
	};

	/* Every second row is shaded, so that the eye can follow a line from the label to its numbers. */
	int row_index = 0;
	auto shade = [&]() {
		if (row_index++ % 2 == 1) GfxFillRect(left - ScaleGUITrad(2), y, right + ScaleGUITrad(2), y + row_h - 1, GREY_SCALE(4));
	};

	const std::string first_name = offset == 0 ? std::string("This Year (Current)") : fmt::format("Year {}", year);
	const std::string second_name = offset == 0 ? std::string("Last Year (Previous)") : fmt::format("Year {}", year - 1);
	DrawString(left, edge[0] - col_w, y, fmt::format("For the period of:  {} and {}", year, year - 1), TC_WHITE, SA_LEFT);
	DrawString(edge[0] - col_w, edge[1], y, first_name, TC_WHITE, SA_HOR_CENTER);
	DrawString(edge[2] - col_w, edge[3], y, second_name, TC_WHITE, SA_HOR_CENTER);
	y += row_h;
	for (int k = 0; k < 4; k++) cell(k, k % 2 == 0 ? "In" : "Out", TC_GREY);
	y += row_h;
	GfxDrawLine(left, y - 1, right, y - 1, GREY_SCALE(8));

	auto category = [&](const char *name) {
		y += ScaleGUITrad(3);
		DrawString(left, right, y, std::string(name), TC_LIGHT_BLUE, SA_LEFT);
		y += row_h;
		GfxDrawLine(left, y - 1, right, y - 1, GREY_SCALE(6));
		row_index = 0;
	};
	/* now_known / before_known: false when the game has no numbers for that year (the yearly expenses of the game are only kept for three years): "n/a". */
	auto line = [&](const char *label, const LedgerEntry &now, const LedgerEntry &before, bool total = false, bool now_known = true, bool before_known = true) {
		shade();
		if (total) GfxDrawLine(edge[0] - col_w, y, right, y, GREY_SCALE(8));
		DrawString(left + ScaleGUITrad(8), edge[0] - col_w, y, std::string(label), total ? TC_LIGHT_BLUE : TC_WHITE, SA_LEFT);
		if (now_known) {
			cell(0, money(now.income), TC_GREEN);
			cell(1, money(now.expense), TC_RED);
		} else {
			cell(0, "n/a", TC_GREY);
			cell(1, "n/a", TC_GREY);
		}
		if (before_known) {
			cell(2, money(before.income), TC_GREEN);
			cell(3, money(before.expense), TC_RED);
		} else {
			cell(2, "n/a", TC_GREY);
			cell(3, "n/a", TC_GREY);
		}
		y += row_h;
	};
	auto booked = [&](const char *label, LedgerItem item) {
		line(label, GetLedgerEntry(_local_company, item, year), GetLedgerEntry(_local_company, item, year - 1));
	};
	/* The yearly expenses of the game: age 0 is this year, 1 last year, 2 the year before; older years are not kept by the game. The ages are shifted by the offset. */
	auto yearly = [&](const char *label, std::initializer_list<ExpensesType> types, bool total = false) {
		const bool now_known = offset < 3;
		const bool before_known = offset + 1 < 3;
		line(label, now_known ? FromYearlyExpenses(c, offset, types) : LedgerEntry{}, before_known ? FromYearlyExpenses(c, offset + 1, types) : LedgerEntry{}, total, now_known, before_known);
	};

	/* The income of the transport goes straight into the company cash (the game pays it when the cargo is delivered), nothing is held back. */
	category("Organic Operating Income (paid into Company Cash)");
	yearly("Trains", {EXPENSES_TRAIN_REVENUE});
	yearly("Road Vehicles", {EXPENSES_ROADVEH_REVENUE});
	yearly("Ships", {EXPENSES_SHIP_REVENUE});
	yearly("Aircraft", {EXPENSES_AIRCRAFT_REVENUE});
	yearly("Total Transport Income", {EXPENSES_TRAIN_REVENUE, EXPENSES_ROADVEH_REVENUE, EXPENSES_SHIP_REVENUE, EXPENSES_AIRCRAFT_REVENUE}, true);

	category("Interest & Fees");
	booked("Central Bank Interest (paid / IORB received)", LedgerItem::BankInterest);
	booked("Stock Commission & Brokerage Fees", LedgerItem::TradeCommission);
	booked("Margin Maintenance / Swap Fees", LedgerItem::MarginSwap);

	category("Dividends & Yields");
	booked("Stock Dividend Payouts", LedgerItem::Dividends);
	booked("Capital Gains / Realized Profit", LedgerItem::RealizedProfit);

	category("Internal Transfers");
	booked("Inter-Wallet Transfer (cash in / out)", LedgerItem::WalletTransfer);
	booked("Company Injection: Loans (paid out / repaid)", LedgerItem::BankLoan);

	category("Gold Reserve (exchange of cash for gold, not earnings)");
	booked("Gold Purchases (bought for the vault)", LedgerItem::GoldBuy);
	booked("Gold Sales (sold from the vault)", LedgerItem::GoldSell);
	booked("Bank Gold Operations (cash paid in / taken out)", LedgerItem::BankGoldOp);
	booked("Gold Storage (vault, 0.5% a year)", LedgerItem::GoldStorage);

	category("Taxes (paid to the Central Bank)");
	booked("Land Tax (per tile owned, daily)", LedgerItem::TaxLand);
	booked("Transport Tax (per vehicle, daily)", LedgerItem::TaxTransport);
	booked("VAT 7% on Transport Income", LedgerItem::TaxVat);
	booked("Capital Gains Tax 15% (trades & gold)", LedgerItem::TaxCapitalGains);
	booked("Dividend Tax 10%", LedgerItem::TaxDividend);
	booked("Income Tax 15% (profit of the quarter)", LedgerItem::TaxIncome);
	booked("IORB Tax 28% (interest on cash)", LedgerItem::TaxInterest);

	category("Fuel (paid to the market, not a tax)");
	booked("Fuel Cost (extra, when fuel is dear)", LedgerItem::FuelCost);

	category("Money zones (fees paid to the Central Bank, banknotes)");
	booked("Exchange Fee (cargo delivered across a zone border)", LedgerItem::ZoneFee);
	booked("Building Permit (foreign zone)", LedgerItem::ZonePermit);
	booked("Banknote Exchange (cash out / in, not earnings)", LedgerItem::ZoneExchange);

	category("Assets & Operations");
	yearly("Vehicle & Infrastructure Running Costs", {EXPENSES_TRAIN_RUN, EXPENSES_ROADVEH_RUN, EXPENSES_AIRCRAFT_RUN, EXPENSES_SHIP_RUN, EXPENSES_PROPERTY});
	yearly("Construction & Land Purchase", {EXPENSES_CONSTRUCTION});
	yearly("Vehicle Purchases", {EXPENSES_NEW_VEHICLES});

	/* The notes wrap inside the window instead of running out of it. */
	y += ScaleGUITrad(6);
	DrawStringMultiLine(left, right, y, r.bottom - ScaleGUITrad(2),
			std::string("Dividends: Out is the dividend that your own company pays. Margin: the daily swap of open positions. "
					"Operations are the yearly expenses of the game. Vehicle depreciation is not a cash expense and is not listed. "
					"The transport, running cost, construction and vehicle lines are kept by the game for three years only: older years show n/a. The other lines go back up to nine years."),
			TC_GREY, SA_TOP | SA_LEFT);
}


/** How many years the past statements window lists: this year and the nine before it. */
static constexpr int ARCHIVE_YEARS = 10;

/** The first game year that exists: nothing is listed before it. */
static int ArchiveFirstYear()
{
	return GameTimeManager::Get().GetSaveState().start_year;
}

/** The year as the design shows it: the game year and, next to it, the Thai Buddhist year. */
static std::string ArchiveYearName(int32_t year)
{
	return fmt::format("{}  (พ.ศ. {})", year, year + 543);
}

/** The list of the last 10 years (left side of the past statements window). The chosen year is highlighted. */
static void DrawArchiveYears(const Rect &r, int selected)
{
	const int row_h = CbRowHeight() + ScaleGUITrad(6);
	const int left = r.left + ScaleGUITrad(4);
	const int right = r.right - ScaleGUITrad(4);
	int y = r.top + ScaleGUITrad(4);
	DrawString(left, right, y, CbText("คลังเอกสาร (10 ปี)", "Archive (10 years)"), TC_LIGHT_BLUE, SA_LEFT);
	y += row_h;
	for (int k = 0; k < ARCHIVE_YEARS; k++, y += row_h) {
		const int32_t year = GetLedgerCurrentYear() - k;
		const bool exists = year >= ArchiveFirstYear();
		if (k == selected) GfxFillRect(left - ScaleGUITrad(2), y - ScaleGUITrad(2), right + ScaleGUITrad(2), y + row_h - ScaleGUITrad(4), GREY_SCALE(5));
		DrawString(left + ScaleGUITrad(4), right, y, ArchiveYearName(year), !exists ? TC_GREY : (k == selected ? TC_WHITE : TC_SILVER), SA_LEFT);
		if (k == 0) DrawString(left, right - ScaleGUITrad(4), y, CbText("ปัจจุบัน", "current"), TC_GREEN, SA_RIGHT);
	}
}

/**
 * The statement of one year (right side of the past statements window), in the same sections as the account statement window.
 * @param r The panel.
 * @param age How many years back: 0 = this year, 9 = nine years ago.
 */
static void DrawArchiveStatement(const Rect &r, int age)
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
	const int32_t year = GetLedgerCurrentYear() - age;
	const int col_w = ScaleGUITrad(110);
	const int pad = ScaleGUITrad(4);
	const int edge[3] = {right - 2 * col_w, right - col_w, right}; // In, Out, Net

	DrawString(left, edge[0] - col_w, y, CbText("Statement ปี ", "Statement ") + ArchiveYearName(year), TC_WHITE, SA_LEFT);
	y += row_h;
	if (year < ArchiveFirstYear()) {
		DrawString(left, right, y, CbText("ไม่มีข้อมูล: เกมยังไม่เริ่มในปีนี้", "No data: the game had not started in this year"), TC_GREY, SA_LEFT);
		return;
	}
	const char *heads[3] = {"In", "Out", "Net"};
	for (int k = 0; k < 3; k++) DrawString(edge[k] - col_w + pad, edge[k] - pad, y, std::string(heads[k]), TC_GREY, SA_RIGHT);
	y += row_h;
	GfxDrawLine(left, y - 1, right, y - 1, GREY_SCALE(8));

	auto money = [](int64_t v) { return v == 0 ? std::string("-") : GetString(STR_JUST_CURRENCY_LONG, v); };
	int row_index = 0;
	int64_t total_in = 0, total_out = 0;
	bool any_unknown = false;

	auto category = [&](const char *name) {
		y += ScaleGUITrad(3);
		DrawString(left, right, y, std::string(name), TC_LIGHT_BLUE, SA_LEFT);
		y += row_h;
		GfxDrawLine(left, y - 1, right, y - 1, GREY_SCALE(6));
		row_index = 0;
	};
	auto line = [&](const char *label, const LedgerEntry &e, bool known, bool count = true) {
		if (row_index++ % 2 == 1) GfxFillRect(left - ScaleGUITrad(2), y, right + ScaleGUITrad(2), y + row_h - 1, GREY_SCALE(4));
		DrawString(left + ScaleGUITrad(8), edge[0] - col_w, y, std::string(label), TC_WHITE, SA_LEFT);
		if (known) {
			DrawString(edge[0] - col_w + pad, edge[0] - pad, y, money(e.income), TC_GREEN, SA_RIGHT);
			DrawString(edge[1] - col_w + pad, edge[1] - pad, y, money(e.expense), TC_RED, SA_RIGHT);
			const int64_t net = e.income - e.expense;
			DrawString(edge[2] - col_w + pad, edge[2] - pad, y, money(net), net < 0 ? TC_RED : TC_GREEN, SA_RIGHT);
			if (count) { total_in += e.income; total_out += e.expense; }
		} else {
			for (int k = 0; k < 3; k++) DrawString(edge[k] - col_w + pad, edge[k] - pad, y, std::string("n/a"), TC_GREY, SA_RIGHT);
			any_unknown = true;
		}
		y += row_h;
	};
	auto booked = [&](const char *label, LedgerItem item) { line(label, GetLedgerEntry(_local_company, item, year), true); };

	/* The game keeps three years of yearly expenses; older years come from the copy in the ledger, which exists only for the years played with it. */
	auto has_copy = [&]() {
		for (LedgerItem item : {LedgerItem::YearTrain, LedgerItem::YearRoadVeh, LedgerItem::YearShip, LedgerItem::YearAircraft, LedgerItem::YearRunning, LedgerItem::YearConstruction, LedgerItem::YearNewVehicles}) {
			const LedgerEntry e = GetLedgerEntry(_local_company, item, year);
			if (e.income != 0 || e.expense != 0) return true;
		}
		return false;
	};
	const bool yearly_known = age < 3 || has_copy();
	auto yearly = [&](const char *label, LedgerItem item, std::initializer_list<ExpensesType> types) {
		line(label, age < 3 ? FromYearlyExpenses(c, age, types) : GetLedgerEntry(_local_company, item, year), yearly_known);
	};

	category("Organic Operating Income (paid into Company Cash)");
	yearly("Trains", LedgerItem::YearTrain, {EXPENSES_TRAIN_REVENUE});
	yearly("Road Vehicles", LedgerItem::YearRoadVeh, {EXPENSES_ROADVEH_REVENUE});
	yearly("Ships", LedgerItem::YearShip, {EXPENSES_SHIP_REVENUE});
	yearly("Aircraft", LedgerItem::YearAircraft, {EXPENSES_AIRCRAFT_REVENUE});

	category("Interest & Fees");
	booked("Central Bank Interest (paid / IORB received)", LedgerItem::BankInterest);
	booked("Stock Commission & Brokerage Fees", LedgerItem::TradeCommission);
	booked("Margin Maintenance / Swap Fees", LedgerItem::MarginSwap);

	category("Dividends & Yields");
	booked("Stock Dividend Payouts", LedgerItem::Dividends);
	booked("Capital Gains / Realized Profit", LedgerItem::RealizedProfit);

	category("Internal Transfers");
	/* Moves between wallets and loans are not earnings: they are shown, but not added to the result of the year. */
	line("Inter-Wallet Transfer (cash in / out)", GetLedgerEntry(_local_company, LedgerItem::WalletTransfer, year), true, false);
	line("Company Injection: Loans (paid out / repaid)", GetLedgerEntry(_local_company, LedgerItem::BankLoan, year), true, false);

	category("Gold Reserve (exchange of cash for gold, not earnings)");
	line("Gold Purchases (bought for the vault)", GetLedgerEntry(_local_company, LedgerItem::GoldBuy, year), true, false);
	line("Gold Sales (sold from the vault)", GetLedgerEntry(_local_company, LedgerItem::GoldSell, year), true, false);
	line("Bank Gold Operations (cash paid in / taken out)", GetLedgerEntry(_local_company, LedgerItem::BankGoldOp, year), true, false);
	booked("Gold Storage (vault, 0.5% a year)", LedgerItem::GoldStorage);

	category("Taxes (paid to the Central Bank)");
	booked("Land Tax (per tile owned, daily)", LedgerItem::TaxLand);
	booked("Transport Tax (per vehicle, daily)", LedgerItem::TaxTransport);
	booked("VAT 7% on Transport Income", LedgerItem::TaxVat);
	booked("Capital Gains Tax 15% (trades & gold)", LedgerItem::TaxCapitalGains);
	booked("Dividend Tax 10%", LedgerItem::TaxDividend);
	booked("Income Tax 15% (profit of the quarter)", LedgerItem::TaxIncome);
	booked("IORB Tax 28% (interest on cash)", LedgerItem::TaxInterest);

	category("Fuel (paid to the market, not a tax)");
	booked("Fuel Cost (extra, when fuel is dear)", LedgerItem::FuelCost);

	category("Money zones (fees paid to the Central Bank, banknotes)");
	booked("Exchange Fee (cargo delivered across a zone border)", LedgerItem::ZoneFee);
	booked("Building Permit (foreign zone)", LedgerItem::ZonePermit);
	line("Banknote Exchange (cash out / in, not earnings)", GetLedgerEntry(_local_company, LedgerItem::ZoneExchange, year), true, false);

	category("Assets & Operations");
	yearly("Vehicle & Infrastructure Running Costs", LedgerItem::YearRunning, {EXPENSES_TRAIN_RUN, EXPENSES_ROADVEH_RUN, EXPENSES_AIRCRAFT_RUN, EXPENSES_SHIP_RUN, EXPENSES_PROPERTY});
	yearly("Construction & Land Purchase", LedgerItem::YearConstruction, {EXPENSES_CONSTRUCTION});
	yearly("Vehicle Purchases", LedgerItem::YearNewVehicles, {EXPENSES_NEW_VEHICLES});

	/* The result of the year. */
	y += ScaleGUITrad(3);
	GfxDrawLine(left, y, right, y, GREY_SCALE(8));
	DrawString(left + ScaleGUITrad(8), edge[0] - col_w, y, CbText("ผลรวมของปี (ไม่รวมการโอน/เงินกู้)", "Result of the year (without transfers and loans)"), TC_LIGHT_BLUE, SA_LEFT);
	DrawString(edge[0] - col_w + pad, edge[0] - pad, y, money(total_in), TC_GREEN, SA_RIGHT);
	DrawString(edge[1] - col_w + pad, edge[1] - pad, y, money(total_out), TC_RED, SA_RIGHT);
	DrawString(edge[2] - col_w + pad, edge[2] - pad, y, money(total_in - total_out), total_in - total_out < 0 ? TC_RED : TC_GREEN, SA_RIGHT);
	y += row_h + ScaleGUITrad(4);
	if (any_unknown) {
		DrawStringMultiLine(left, right, y, r.bottom - ScaleGUITrad(2),
				CbText("n/a: ปีนี้เกิดก่อนที่ม็อดจะเริ่มเก็บค่าใช้จ่ายรายปี (เกมเก็บเองแค่ 3 ปี) จากนี้ไปจะเก็บครบ 10 ปี",
						"n/a: this year is from before the mod kept the yearly expenses (the game keeps only 3 years). From now on 10 years are kept."),
				TC_GREY, SA_TOP | SA_LEFT);
	}
}


/** The Central Bank window of the local company: the numbers on the left, the account log on the right. */
struct CentralBankWindow : Window {
	/** What the open query window is for. */
	enum class QueryKind : uint8_t {
		None,
		Borrow,
		Repay,
	};

	static constexpr int SUMMARY_ROWS = 13; ///< Rows of the left panel, with room for the two penalty lines.
	static constexpr int SEPARATORS = 4;    ///< Lines between the groups of the left panel.
	static constexpr int LOG_ROWS = 8;      ///< Lines of the account log that are shown.

	QueryKind query_kind = QueryKind::None; ///< What the open query window is for.
	Scrollbar *vscroll = nullptr;           ///< Scrollbar of the account log.

	/** Redraw often (about every 100 ms): the cash of the company changes all the time and the window must show what the status bar shows. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(100), [this](auto) {
		this->SyncLog();
		this->SetDirty();
	}};

	CentralBankWindow(WindowDesc &desc) : Window(desc)
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_CB_LOG_SCROLL);
		this->FinishInitNested(CB_WINDOW_BANK);
		this->UpdateSkipRow(true);
		this->SyncLog();
	}

	/** Height of the title and the column heads at the top of the log, and of the note at the bottom (shared by the drawing and the scrollbar). */
	static int LogHeadHeight() { return 2 * CbRowHeight() + ScaleGUITrad(11); }
	static int LogFootHeight() { return CbRowHeight() + ScaleGUITrad(4); }

	/** Tell the scrollbar how many lines the log has and how many fit now. Called when the window changes size, and on every redraw (the log grows by itself). */
	void SyncLog()
	{
		const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_CB_LOG);
		const int avail = static_cast<int>(nw->current_y) - LogHeadHeight() - LogFootHeight();
		this->vscroll->SetCapacity(static_cast<size_t>(std::max(1, avail / std::max(1, CbRowHeight()))));
		this->vscroll->SetCount(Company::IsValidID(_local_company) ? _bank_log[_local_company.base()].size() : 0);
	}

	void OnResize() override
	{
		this->SyncLog();
	}

	/** The most that can be repaid from the company cash now. */
	int64_t GetRepayable() const
	{
		if (!Company::IsValidID(_local_company)) return 0;
		const std::optional<uint32_t> bank = CBankNearestToHQ(_local_company);
		const int64_t debt = bank.has_value() ? GetCommercialDebtAt(*bank, _local_company) : 0;
		return std::max<int64_t>(0, std::min<int64_t>(GetWalletBalance(_local_company, WALLET_CASH), debt));
	}

	void OnPaint() override
	{
		/* The button is pushed in while the statement window is open. */
		this->SetWidgetLoweredState(WID_CB_OPEN_STATEMENT, FindWindowById(WC_CENTRAL_BANK, CB_WINDOW_STATEMENT) != nullptr);
		this->SetWidgetLoweredState(WID_CB_OPEN_POLICY, FindWindowById(WC_CENTRAL_BANK, CB_WINDOW_POLICY) != nullptr);
		this->SyncLog();
		this->DrawWidgets();
	}

	bool skip_row_shown = true; ///< Whether the test buttons row is in the layout now.

	/** Show the test buttons (after 'stock_dev on') or take the row out of the layout completely: no blank line while the tools are off. */
	void UpdateSkipRow(bool force = false)
	{
		const bool on = _stock_dev_tools;
		if (!force && on == this->skip_row_shown) return;
		this->skip_row_shown = on;
		this->GetWidget<NWidgetStacked>(WID_CB_SKIP_SEL)->SetDisplayedPlane(on ? 0 : SZSP_NONE);
		this->ReInit();
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (gui_scope) this->UpdateSkipRow();
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
		/* The bank of this window is the commercial bank nearest to the headquarters. */
		const std::optional<uint32_t> bank = CBankNearestToHQ(_local_company);
		if (!bank.has_value()) {
			DrawStringMultiLine(left, right, y, r.bottom - ScaleGUITrad(2),
					CbText("ยังไม่มีธนาคารให้ใช้: สร้างสำนักงานใหญ่ (HQ) ในโซนที่มีธนาคารก่อน", "No bank to use: build a headquarters (HQ) in a zone that has a bank first"), TC_ORANGE, SA_TOP | SA_LEFT);
			return;
		}
		const Town *bank_town = Town::Get(TownID{static_cast<uint16_t>(*bank)});
		const BankAccount a = CBankAccountOf(*bank, _local_company);
		const int64_t available = CBankAvailable(*bank, _local_company);
		const int64_t limit = a.debt + available;
		const int64_t rate_bp = CBankLoanRateBp(*bank, _local_company);
		const int64_t per_day = BankDailyInterest(a.debt, rate_bp, a.carry).interest;
		const int64_t central_debt = GetCentralBankDebt(_local_company);

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

		/* Group 0: which bank. */
		row(CbText("ธนาคารพาณิชย์ (ใกล้ HQ)", "Commercial bank (nearest HQ)"), GetString(STR_TOWN_NAME, bank_town->index), TC_LIGHT_BLUE);
		separator();

		/* Group 1: the cash. */
		row(CbText("เงินสด", "Cash"), money(static_cast<int64_t>(c->money)), c->money < 0 ? TC_RED : TC_GREEN);
		separator();

		/* Group 2: the money in the trading wallets and the debt. */
		row(CbText("ยอดเงินฝาก (กระเป๋าเทรด)", "Deposits (trading wallets)"), money(GetWalletsTotal(_local_company)));
		row(CbText("ยอดเงินกู้คงค้าง", "Outstanding loan"), money(a.debt), a.debt > limit ? TC_RED : TC_ORANGE);
		if (central_debt > 0) row(CbText("หนี้ธนาคารกลาง (เดิม)", "Central bank loan (old)"), money(central_debt), TC_ORANGE);
		separator();

		/* Group 3: the credit. */
		row(CbText("วงเงินสินเชื่อที่ธนาคารนี้", "Credit limit at this bank"), money(limit));
		row(CbText("กู้เพิ่มได้อีก", "Can still borrow"), money(available), TC_GREEN);
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
			case LedgerItem::BankInterest:    return e.amount > 0 ? CbText("ดอกเบี้ยเงินฝาก (IORB)", "IORB interest") : CbText("ดอกเบี้ย", "Interest");
			case LedgerItem::TradeCommission: return CbText("ค่าคอมมิชชัน", "Commission");
			case LedgerItem::MarginSwap:      return CbText("ค่าสวอป", "Swap fee");
			case LedgerItem::Dividends:       return CbText("เงินปันผล", "Dividend");
			case LedgerItem::RealizedProfit:  return e.amount >= 0 ? CbText("กำไรปิดสถานะ", "Profit") : CbText("ขาดทุนปิดสถานะ", "Loss");
			case LedgerItem::WalletTransfer:  return e.amount >= 0 ? CbText("โอนเข้าเงินสด", "To cash") : CbText("โอนไปกระเป๋าเทรด", "To wallet");
			case LedgerItem::BankLoan:        return e.amount >= 0 ? CbText("กู้เงิน", "Borrow") : CbText("ชำระคืน", "Repay");
			case LedgerItem::GoldBuy:         return CbText("ซื้อทอง", "Gold buy");
			case LedgerItem::GoldSell:        return CbText("ขายทอง", "Gold sale");
			case LedgerItem::TaxLand:         return CbText("ภาษีที่ดิน", "Land tax");
			case LedgerItem::TaxTransport:    return CbText("ภาษีขนส่ง", "Transport tax");
			case LedgerItem::FuelCost:        return CbText("ค่าน้ำมันส่วนเพิ่ม", "Fuel cost");
			case LedgerItem::TaxVat:          return CbText("ภาษีมูลค่าเพิ่ม (VAT)", "VAT");
			case LedgerItem::TaxCapitalGains: return CbText("ภาษีกำไรจากการเทรด", "Capital gains tax");
			case LedgerItem::TaxDividend:     return CbText("ภาษีเงินปันผล", "Dividend tax");
			case LedgerItem::TaxIncome:       return CbText("ภาษีเงินได้ไตรมาส", "Income tax");
			case LedgerItem::TaxInterest:     return CbText("ภาษีดอกเบี้ยเงินสด (IORB)", "IORB tax");
			case LedgerItem::GoldStorage:     return CbText("ค่าเก็บและประกันทองในคลัง", "Gold storage");
			case LedgerItem::ZonePermit:      return CbText("ค่าใบอนุญาตก่อสร้างต่างโซน", "Building permit");
			case LedgerItem::ZoneExchange:    return e.amount >= 0 ? CbText("ขายธนบัตร", "Banknote sale") : CbText("ซื้อธนบัตร", "Banknote buy");
			case LedgerItem::ZoneFee:         return CbText("ค่าธรรมเนียมแลกเงินข้ามโซน", "Zone exchange fee");
			case LedgerItem::BankGoldOp:      return e.amount >= 0 ? CbText("ธนาคารซื้อทอง: เงินเข้า", "Bank gold buy: cash in") : CbText("ธนาคารขายทอง: เงินออก", "Bank gold sale: cash out");
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

		/* The lines that fit between the heads and the note, starting at the scrollbar position. The lines are the sum of the bookings of one kind of one day. */
		const int note_y = r.bottom - LogFootHeight() + ScaleGUITrad(2);
		const size_t first = this->vscroll->GetPosition();
		int shown = 0;
		for (size_t i = first; i < log.size(); i++) {
			if (y + row_h > note_y) break;
			const BankLogEntry &e = log[i];
			if (shown % 2 == 1) GfxFillRect(r.left + 1, y - 1, r.right - 1, y + row_h - 2, GREY_SCALE(3));

			const TextColour colour = e.item == LedgerItem::BankInterest && e.amount < 0 ? TC_ORANGE : (e.amount >= 0 ? TC_GREEN : TC_RED);
			DrawString(day_x, right, y, fmt::format("{} {}", CbText("วันที่", "Day"), e.day), TC_WHITE, SA_LEFT);
			std::string name = LogName(e);
			if (e.count > 1) name += fmt::format(" x{}", e.count); // many bookings of the day are one line
			DrawString(name_x, right, y, name, TC_WHITE, SA_LEFT);
			DrawString(left, right, y, (e.amount > 0 ? "+" : "") + GetString(STR_JUST_CURRENCY_LONG, e.amount), colour, SA_RIGHT);
			y += row_h;
			shown++;
		}

		/* The note at the bottom of the panel. */
		DrawString(left, right, note_y, CbText("* ล่าสุด · ชนิดเดียวกันในวันเดียวกันรวมเป็นก้อนเดียว", "* Latest bookings. The same kind on the same day is one line."), TC_GREY, SA_LEFT);
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

			case WID_CB_OPEN_POLICY:
				/* The monetary policy is its own window, like the statement: a second click closes it. */
				if (FindWindowById(WC_CENTRAL_BANK, CB_WINDOW_POLICY) != nullptr) {
					CloseWindowById(WC_CENTRAL_BANK, CB_WINDOW_POLICY);
				} else {
					ShowCentralBankPolicyWindow();
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

			case WID_CB_SKIP_1H: SkipGameMinutes(60); break;
			case WID_CB_SKIP_1D: SkipGameMinutes(60 * 24); break;
			case WID_CB_SKIP_7D: SkipGameMinutes(60 * 24 * 7); break;
			case WID_CB_SKIP_30D: SkipGameMinutes(60 * 24 * 30); break;
			case WID_CB_SKIP_90D: SkipGameMinutes(60 * 24 * 90); break;

			case WID_CB_REPAY_ALL: {
				const int64_t amount = this->GetRepayable();
				const std::optional<uint32_t> bank = CBankNearestToHQ(_local_company);
				if (amount > 0 && bank.has_value()) {
					Command<CMD_CBANK_REPAY>::Post(STR_CBANK_ERROR_CAPTION, static_cast<uint16_t>(*bank), Money(amount));
				} else if (!bank.has_value() || GetCommercialDebtAt(*bank, _local_company) == 0) {
					/* Only an old central bank loan is left: repay it from the cash. */
					const int64_t old_debt = std::min<int64_t>(GetWalletBalance(_local_company, WALLET_CASH), GetCentralBankDebt(_local_company));
					if (old_debt > 0) Command<CMD_BANK_REPAY>::Post(STR_BANK_ERROR_CANT_REPAY, WALLET_CASH, Money(old_debt));
				}
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

		/* Loan money only ever goes to, and is only repaid from, the company cash. The bank is the commercial bank nearest to the headquarters. */
		const std::optional<uint32_t> bank = CBankNearestToHQ(_local_company);
		if (!bank.has_value()) return;
		if (kind == QueryKind::Borrow) {
			Command<CMD_CBANK_BORROW>::Post(STR_CBANK_ERROR_CAPTION, static_cast<uint16_t>(*bank), Money(StockDisplayToMoney(amount)));
		} else {
			Command<CMD_CBANK_REPAY>::Post(STR_CBANK_ERROR_CAPTION, static_cast<uint16_t>(*bank), Money(StockDisplayToMoney(amount)));
		}
	}
};

/** The account statement window: this year and last year, in four groups. It is its own window, so it can stay open next to the bank window. */
struct CentralBankStatementWindow : Window {
	static constexpr int STATEMENT_ROWS = 43; ///< Number of rows in the account statement, with room for the notes (43: two lines for the permit and the banknote exchange were added).

	int offset = 0; ///< How many years back the statement is: 0 = this year and last year.

	/** Redraw regularly: the numbers change all the time. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(100), [this](auto) {
		this->SetDirty();
	}};

	CentralBankStatementWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(CB_WINDOW_STATEMENT);
		this->UpdateButtons();
	}

	/** The buttons of the past statements: no newer period than this year, no older period than the game keeps. */
	void UpdateButtons()
	{
		this->SetWidgetLoweredState(WID_CBS_OLDER, FindWindowById(WC_CENTRAL_BANK, CB_WINDOW_ARCHIVE) != nullptr);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		/* "Statement ย้อนหลัง": the 10 years open in their own window; a second click closes it. */
		if (widget != WID_CBS_OLDER) return;
		if (FindWindowById(WC_CENTRAL_BANK, CB_WINDOW_ARCHIVE) != nullptr) {
			CloseWindowById(WC_CENTRAL_BANK, CB_WINDOW_ARCHIVE);
		} else {
			ShowCentralBankArchiveWindow();
		}
		this->SetDirty();
	}

	void OnPaint() override
	{
		this->UpdateButtons();
		this->DrawWidgets();
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_CBS_PANEL) return;
		size.width = std::max<uint>(size.width, ScaleGUITrad(760));
		size.height = std::max<uint>(size.height, CbRowHeight() * STATEMENT_ROWS + ScaleGUITrad(12));
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_CBS_PANEL: DrawAccountStatement(r, this->offset); break;
			case WID_CBS_OLDER:
				DrawString(r.left + 2, r.right - 2, CentreBounds(r.top, r.bottom, GetCharacterHeight(FS_NORMAL)), CbText("Statement ย้อนหลัง", "Past Statements"), TC_BLACK, SA_HOR_CENTER);
				break;
			default: break;
		}
	}
};

/*
 * Colours of the monetary policy page. All of them are in ONE place so that the look can be tuned with one edit.
 * The page is toned down (owner request 2026-10-26: the old blue panel with bright white, orange, red and light blue text was too glaring):
 * a grey panel like the rest of the bank windows, soft silver text, cream card titles, and the loud colours only where they mean something.
 */
static constexpr TextColour CBP_TEXT = TC_SILVER;  ///< Labels and plain numbers (was bright white).
static constexpr TextColour CBP_TITLE = TC_CREAM;  ///< Card titles (was light blue).
static constexpr TextColour CBP_GOOD = TC_GREEN;   ///< Good / eased / more room.
static constexpr TextColour CBP_WARN = TC_GOLD;    ///< Tighter / fewer room / plain warnings (was orange).
static constexpr TextColour CBP_BAD = TC_RED;      ///< A real alarm only: emergency mode, inflation far off target, falling jobs, bad debt that the budget did not cover.

/**
 * Draw the monetary policy page as cards: a row of five key numbers, three columns of cards (policy and rates, credit and decision log, gold of the bank)
 * and a wide card with the rules of the bank. Only the old colours are used: the colour of the panel, the grey lines of the old separators and the old text colours.
 * Returns the height that the page needs. With r == nullptr nothing is drawn and only the height is returned (UpdateWidgetSize): every card has a fixed
 * number of rows, so the height never depends on the numbers. A row whose text is too long for its card shortens the label, never the number.
 */
static int DrawMonetaryPolicy(const Rect *r)
{
	const bool paint = r != nullptr;
	const int margin = ScaleGUITrad(8);
	const int gap = ScaleGUITrad(8);
	const int pad_h = ScaleGUITrad(8);
	const int pad_v = ScaleGUITrad(6);
	const int row_h = CbRowHeight() + ScaleGUITrad(3);
	const int title_h = row_h + ScaleGUITrad(3);
	const int text_gap = ScaleGUITrad(6);
	const PixelColour line_colour = GREY_SCALE(4);

	const int panel_left = paint ? r->left : 0;
	const int panel_top = paint ? r->top : 0;
	const int panel_right = paint ? r->right : ScaleGUITrad(980);
	const int inner_left = panel_left + margin;
	const int inner_right = panel_right - margin;
	const int inner_w = inner_right - inner_left;

	const CentralBank &bank = LocalBank(); // The bank of the zone of the headquarters of the local company.
	const MonetaryPolicy &p = bank.policy;
	const int64_t shift = bank.ShiftBp(); // The shift of the policy plus the spread of the country.
	auto pct = [](int64_t bp, bool plus = false) {
		const int64_t a = bp < 0 ? -bp : bp;
		return fmt::format("{}{}.{:02}%", bp < 0 ? "-" : (plus ? "+" : ""), a / 100, a % 100);
	};

	int y = panel_top + margin;

	/* A card: a frame of the old grey line, a title (old heading colour) with a line under it, and n_rows rows of content. y is left at the first row. */
	auto card_height = [&](int n_rows) { return 2 * pad_v + title_h + n_rows * row_h; };
	auto card = [&](int x0, int x1, int top, int n_rows, const std::string &title) {
		if (paint) {
			const int bottom = top + card_height(n_rows) - 1;
			GfxDrawLine(x0, top, x1, top, line_colour);
			GfxDrawLine(x0, bottom, x1, bottom, line_colour);
			GfxDrawLine(x0, top, x0, bottom, line_colour);
			GfxDrawLine(x1, top, x1, bottom, line_colour);
			DrawString(x0 + pad_h, x1 - pad_h, top + pad_v, title, CBP_TITLE, SA_LEFT);
			GfxDrawLine(x0 + pad_h, top + pad_v + row_h, x1 - pad_h, top + pad_v + row_h, line_colour);
		}
		y = top + pad_v + title_h;
	};
	/* One row: label left, number right. The label gets only the room that the number leaves, so the two never overlap. */
	auto row = [&](int x0, int x1, const std::string &label, const std::string &value, TextColour colour = CBP_TEXT) {
		if (paint) {
			const int vw = static_cast<int>(GetStringBoundingBox(value).width);
			DrawString(x0, x1, y, value, colour, SA_RIGHT);
			DrawString(x0, std::max(x0 + 1, x1 - vw - text_gap), y, label, CBP_TEXT, SA_LEFT);
		}
		y += row_h;
	};
	/* A row of two lines for a long text: the label, and under it the text on the whole width. */
	auto row2 = [&](int x0, int x1, const std::string &label, const std::string &value, TextColour colour = CBP_TEXT) {
		if (paint) {
			DrawString(x0, x1, y, label, CBP_TEXT, SA_LEFT);
			DrawString(x0, x1, y + row_h, value, colour, SA_RIGHT);
		}
		y += 2 * row_h;
	};

	const int64_t today = static_cast<int64_t>(GameTimeManager::Get().GetCurrentGameDay());

	/* ---- Row 1: five key numbers. ---- */
	const int kpi_h = 2 * pad_v + GetCharacterHeight(FS_NORMAL) + GetCharacterHeight(FS_LARGE) + ScaleGUITrad(2);
	{
		const int kw = (inner_w - 4 * gap) / 5;
		auto kpi = [&](int i, const std::string &label, const std::string &value, TextColour colour) {
			if (paint) {
				const int x0 = inner_left + i * (kw + gap);
				const int x1 = x0 + kw;
				GfxDrawLine(x0, y, x1, y, line_colour);
				GfxDrawLine(x0, y + kpi_h - 1, x1, y + kpi_h - 1, line_colour);
				GfxDrawLine(x0, y, x0, y + kpi_h - 1, line_colour);
				GfxDrawLine(x1, y, x1, y + kpi_h - 1, line_colour);
				DrawString(x0 + pad_h, x1 - pad_h, y + pad_v, label, CBP_TEXT, SA_LEFT);
				DrawString(x0 + pad_h, x1 - pad_h, y + pad_v + GetCharacterHeight(FS_NORMAL) + ScaleGUITrad(2), value, colour, SA_LEFT, false, FS_LARGE);
			}
		};
		const int64_t idx = GetPriceIndexPpm();
		kpi(0, CbText("ดัชนีราคา", "Price index"), fmt::format("{}.{:03}", idx / 1'000'000, (idx % 1'000'000) / 1000), CBP_TEXT);
		if (p.measured) {
			const int64_t err = p.inflation_bp > POLICY_TARGET_INFLATION_BP ? p.inflation_bp - POLICY_TARGET_INFLATION_BP : POLICY_TARGET_INFLATION_BP - p.inflation_bp;
			kpi(1, CbText("เงินเฟ้อ (ต่อปี)", "Inflation (yearly)"), pct(p.inflation_bp), err <= POLICY_DEADBAND_BP ? CBP_GOOD : (err <= 3 * POLICY_DEADBAND_BP ? CBP_WARN : CBP_BAD));
			kpi(2, CbText("เติบโตที่ใช้ตัดสินใจ (ต่อปี)", "Growth used (yearly)"), pct(p.growth_bp), CBP_TEXT);
			kpi(3, CbText("Payroll (ต่อปี)", "Payrolls (yearly)"), p.has_jobs ? pct(p.jobs_bp, true) : std::string("-"), p.has_jobs ? (p.jobs_bp < 0 ? CBP_BAD : CBP_GOOD) : TC_GREY);
		} else {
			kpi(1, CbText("เงินเฟ้อ (ต่อปี)", "Inflation (yearly)"), CbText("กำลังวัด...", "measuring..."), TC_GREY);
			kpi(2, CbText("เติบโตที่ใช้ตัดสินใจ (ต่อปี)", "Growth used (yearly)"), CbText("กำลังวัด...", "measuring..."), TC_GREY);
			kpi(3, CbText("Payroll (ต่อปี)", "Payrolls (yearly)"), CbText("กำลังวัด...", "measuring..."), TC_GREY);
		}
		kpi(4, CbText("เป้าหมายเงินเฟ้อ", "Inflation target"), pct(POLICY_TARGET_INFLATION_BP), CBP_TEXT);
	}
	y += kpi_h + gap;

	/* ---- Row 2: three columns of cards. ---- */
	const int col_top = y;
	const int cw = (inner_w - 2 * gap) / 3;
	const int cx[3] = {inner_left, inner_left + cw + gap, inner_left + 2 * (cw + gap)};
	int col_bottom[3] = {col_top, col_top, col_top};
	{
		/* Column 1, card 1: the decision of the bank. 6 rows. */
		const int n1 = 6;
		card(cx[0], cx[0] + cw, col_top, n1, CbText("นโยบายดอกเบี้ย (POLICY RATE)", "POLICY RATE"));
		const int a0 = cx[0] + pad_h, a1 = cx[0] + cw - pad_h;
		row(a0, a1, fmt::format("{} [{}]", CbText("ช่วงอัตราดอกเบี้ย (Target range)", "Target range"), BankCount() > 1 ? ZoneCurrencyName(bank.zone) : ""), fmt::format("{} - {}", pct(PolicyLowerBp(shift)), pct(PolicyUpperBp(shift))), shift > 0 ? CBP_WARN : (shift < 0 ? CBP_GOOD : CBP_TEXT));
		row(a0, a1, p.last_unscheduled ? CbText("ประชุมล่าสุด (นอกรอบ!)", "Last meeting (UNSCHEDULED!)") : CbText("การประชุมล่าสุด", "Last meeting"),
				p.measured ? (p.last_step_bp > 0 ? fmt::format("{} {} bp", CbText("ขึ้น", "raised"), p.last_step_bp) : (p.last_step_bp < 0 ? fmt::format("{} {} bp", CbText("ลด", "lowered"), -p.last_step_bp) : std::string(CbText("คงที่", "held")))) : std::string("-"),
				p.last_step_bp > 0 ? CBP_WARN : (p.last_step_bp < 0 ? CBP_GOOD : TC_GREY));
		row(a0, a1, CbText("ทิศทาง (Bias)", "Bias"), p.measured ? (p.wanted_gap_bp >= POLICY_HOLD_BAND_BP ? std::string(CbText("ตึงตัว (อยากขึ้น)", "tightening")) : (p.wanted_gap_bp <= -POLICY_HOLD_BAND_BP ? std::string(CbText("ผ่อนคลาย (อยากลด)", "easing")) : std::string(CbText("เป็นกลาง", "neutral")))) : std::string("-"), TC_GREY);
		row(a0, a1, CbText("ผลต่อตลาดล่าสุด (ทอง / หุ้น)", "Last market push (gold / shares)"), p.measured ? fmt::format("{} / {}", pct(p.last_gold_push_ppm / 100, true), pct(p.last_share_push_ppm / 100, true)) : std::string("-"), p.last_gold_push_ppm > 0 ? CBP_GOOD : (p.last_gold_push_ppm < 0 ? CBP_WARN : TC_GREY));
		row(a0, a1, CbText("โหมดนโยบาย", "Policy mode"), p.emergency ? CbText("ฉุกเฉิน (เงินเฟ้อสูง)", "EMERGENCY (high inflation)") : (p.Extended() ? CbText("กำลังกลับสู่ปกติ", "returning to normal") : CbText("ปกติ", "Normal")), p.emergency ? CBP_BAD : (p.Extended() ? CBP_WARN : TC_GREY));
		row(a0, a1, CbText("ประชุม FOMC ครั้งต่อไป", "Next FOMC meeting"), fmt::format("{} {}", p.last_decision_day < 0 ? POLICY_DECISION_DAYS : ((p.emergency && POLICY_EMERGENCY_REVIEW_DAYS > 0) ? std::max<int64_t>(1, POLICY_EMERGENCY_REVIEW_DAYS - (today - p.last_decision_day)) : PolicyDaysToNextMeeting(today)), CbText("วัน", "days")), TC_GREY);
		col_bottom[0] = col_top + card_height(n1);

		/* Column 1, card 2: the rates now. 6 rows. */
		const int top2 = col_bottom[0] + gap;
		const int n2 = 6;
		card(cx[0], cx[0] + cw, top2, n2, CbText("อัตราดอกเบี้ย (RATES)", "RATES"));
		row(a0, a1, CbText("Prime rate (ช่วงบน + 3.00%)", "Prime rate (upper + 3.00%)"), pct(PolicyPrimeBp(shift)));
		row(a0, a1, "Discount rate / IORB", fmt::format("{} / {}", pct(PolicyDiscountBp(shift)), pct(PolicyIorbBp(shift))));
		row(a0, a1, BANK_IORB_CREATES_MONEY ? CbText("IORB ที่จ่ายแล้ว (เงินใหม่)", "IORB paid so far (new money)") : CbText("IORB ที่จ่ายแล้ว (จากงบธนาคาร)", "IORB paid so far (bank budget)"), GetString(STR_JUST_CURRENCY_LONG, bank.iorb_created), CBP_GOOD);
		row(a0, a1, CbText("ดอกเบี้ยเงินกู้ เครดิตดี", "Loan rate, good credit"), fmt::format("{} - {}", pct(BankShiftedRateBp(BANK_RATE_GOOD_MIN_BP, shift)), pct(BankShiftedRateBp(BANK_RATE_GOOD_MAX_BP, shift))));
		row(a0, a1, CbText("ดอกเบี้ยเงินกู้ เครดิตต่ำ", "Loan rate, poor credit"), fmt::format("{} - {}", pct(BankShiftedRateBp(BANK_RATE_POOR_MIN_BP, shift)), pct(BankShiftedRateBp(BANK_RATE_POOR_MAX_BP, shift))));
		row(a0, a1, CbText("ดอกเบี้ยผิดนัด", "Penalty rate"), fmt::format("{} - {}", pct(BankPenaltyRateBp(1, shift)), pct(BankPenaltyRateBp(BANK_PENALTY_LEVEL_MAX, shift))), CBP_WARN);
		col_bottom[0] = top2 + card_height(n2);
	}
	{
		/* Column 2, card 3: the credit limit. 3 rows. */
		const int n3 = 4;
		card(cx[1], cx[1] + cw, col_top, n3, CbText("วงเงินกู้ (CREDIT)", "CREDIT"));
		const int a0 = cx[1] + pad_h, a1 = cx[1] + cw - pad_h;
		const int64_t share = BankPolicyCreditPercent(shift);
		const int64_t tier_percent = GetCentralBankTierCreditPercent(_local_company);
		row(a0, a1, CbText("วงเงินกู้ต่อส่วนของเจ้าของ", "Credit limit, share of equity"), fmt::format("{}%", share), share < CENTRAL_BANK_CREDIT_PERCENT ? CBP_WARN : (share > CENTRAL_BANK_CREDIT_PERCENT ? CBP_GOOD : CBP_TEXT));
		row(a0, a1, CbText("ระดับเครดิตปรับวงเงิน", "Credit standing scales the limit"), fmt::format("{} x{}%", GetCreditTierName(static_cast<CreditTier>(BankCreditTierIndex(bank))), tier_percent), tier_percent < 100 ? CBP_WARN : (tier_percent > 100 ? CBP_GOOD : CBP_TEXT));
		const int64_t tier_spread = BankTierRateSpreadBp(BankCreditTierIndex(bank));
		row(a0, a1, CbText("ระดับเครดิตปรับดอกเบี้ย", "Credit standing adds to the rate"), fmt::format("{}{}.{:02}%", tier_spread < 0 ? "-" : (tier_spread > 0 ? "+" : ""), (tier_spread < 0 ? -tier_spread : tier_spread) / 100, (tier_spread < 0 ? -tier_spread : tier_spread) % 100), tier_spread > 0 ? CBP_WARN : (tier_spread < 0 ? CBP_GOOD : CBP_TEXT));
		if (Company::IsValidID(_local_company)) {
			row(a0, a1, CbText("วงเงินกู้ของบริษัทคุณตอนนี้", "Your credit limit now"), GetString(STR_JUST_CURRENCY_LONG, GetCentralBankCreditLimit(_local_company)), CBP_GOOD);
		} else {
			row(a0, a1, CbText("วงเงินกู้ของบริษัทคุณตอนนี้", "Your credit limit now"), std::string("-"), TC_GREY);
		}
		const int bottom3 = col_top + card_height(n3);

		/* Column 2, card 4: the decision log as a table, newest first. A header row and 5 rows. */
		const int top4 = bottom3 + gap;
		const int n4 = 6;
		card(cx[1], cx[1] + cw, top4, n4, CbText("บันทึกการตัดสินใจ", "Decision log"));
		const int w = a1 - a0;
		const int c_day = a0, c_act = a0 + w * 17 / 100, c_infl = a0 + w * 29 / 100, c_grow = a0 + w * 45 / 100, c_chg = a0 + w * 64 / 100;
		if (paint) {
			DrawString(c_day, c_act - 2, y, CbText("วัน", "Day"), TC_GREY, SA_LEFT);
			DrawString(c_act, c_infl - 2, y, CbText("ผล", "Move"), TC_GREY, SA_LEFT);
			DrawString(c_infl, c_grow - 2, y, CbText("เฟ้อ", "Infl"), TC_GREY, SA_LEFT);
			DrawString(c_grow, c_chg - 2, y, CbText("โต", "Growth"), TC_GREY, SA_LEFT);
			DrawString(c_chg, a1, y, CbText("อัตรา", "Rate"), TC_GREY, SA_RIGHT);
		}
		y += row_h;
		for (size_t i = 0; i < 5; i++) {
			if (i >= p.log.size()) {
				if (paint && i == 0) DrawString(a0, a1, y, CbText("ยังไม่มีการตัดสินใจ", "No decision yet"), TC_GREY, SA_LEFT);
				y += row_h;
				continue;
			}
			const PolicyDecision &d = p.log[i];
			const char *what_th = d.shift_after > d.shift_before ? "ขึ้น" : (d.shift_after < d.shift_before ? "ลด" : "คงที่");
			const char *what_en = d.shift_after > d.shift_before ? "hike" : (d.shift_after < d.shift_before ? "cut" : "hold");
			const TextColour colour = d.shift_after > d.shift_before ? CBP_WARN : (d.shift_after < d.shift_before ? CBP_GOOD : TC_GREY);
			if (paint) {
				DrawString(c_day, c_act - 2, y, fmt::format("D{}{}{}", d.day + 1, d.emergency ? "!" : "", d.unscheduled ? "*" : ""), CBP_TEXT, SA_LEFT);
				DrawString(c_act, c_infl - 2, y, CbText(what_th, what_en), colour, SA_LEFT);
				DrawString(c_infl, c_grow - 2, y, pct(d.inflation_bp), TC_GREY, SA_LEFT);
				DrawString(c_grow, c_chg - 2, y, pct(d.growth_bp), TC_GREY, SA_LEFT);
				DrawString(c_chg, a1, y, fmt::format("{} -> {}", pct(PolicyUpperBp(d.shift_before)), pct(PolicyUpperBp(d.shift_after))), colour, SA_RIGHT);
				GfxDrawLine(a0, y + row_h - 2, a1, y + row_h - 2, line_colour);
			}
			y += row_h;
		}
		col_bottom[1] = top4 + card_height(n4);
	}
	{
		/* Column 3, card 5: the gold of the bank. The backing has two lines (its text is long), the other two rows one: 4 rows. */
		const int n5 = 4;
		card(cx[2], cx[2] + cw, col_top, n5, CbText("ทองของธนาคาร (BANK GOLD)", "BANK GOLD"));
		const int a0 = cx[2] + pad_h, a1 = cx[2] + cw - pad_h;
		const int64_t backing = BankBackingBp(bank);
		row2(a0, a1, CbText("สำรองทองต่อเงินสดในระบบ (ซื้อ-ขาย)", "Gold backing (buy .. sell band)"), fmt::format("{}.{:02}%  ({} .. {})", backing / 100, backing % 100, pct(BANK_GOLD_BUY_BELOW_BP), pct(BANK_GOLD_SELL_ABOVE_BP)));
		row(a0, a1, CbText("เงินของธนาคารเอง (ทุน + กำไร)", "Own money of the bank"), GetString(STR_JUST_CURRENCY_LONG, bank.trading_revenue_cents / 100), CBP_GOOD);
		row(a0, a1, CbText("หนี้สูญที่ตัดแล้ว (เกินงบ)", "Bad debt written off (beyond budget)"), GetString(STR_JUST_CURRENCY_LONG, bank.written_off) + " (" + GetString(STR_JUST_CURRENCY_LONG, bank.uncovered_loss) + ")", bank.uncovered_loss > 0 ? CBP_BAD : TC_GREY);
		const int bottom5 = col_top + card_height(n5);

		/* Column 3, card 6: the latest gold trades of the bank. 3 rows. */
		const int top6 = bottom5 + gap;
		const int n6 = 3;
		card(cx[2], cx[2] + cw, top6, n6, CbText("ธนาคารซื้อขายทองล่าสุด", "Latest gold trades"));
		for (size_t i = 0; i < 3; i++) {
			if (i >= bank.gold_log.size()) {
				if (paint && i == 0) DrawString(a0, a1, y, CbText("ยังไม่เคยซื้อขาย", "No trade yet"), TC_GREY, SA_LEFT);
				y += row_h;
				continue;
			}
			const BankGoldTrade &t = bank.gold_log[i];
			row(a0, a1, fmt::format("D{} {} {} oz", t.day + 1, t.ounces >= 0 ? CbText("ซื้อ", "bought") : CbText("ขาย", "sold"), t.ounces >= 0 ? t.ounces : -t.ounces),
					fmt::format("@ {}{}", GetString(STR_JUST_CURRENCY_LONG, t.price_cents / 100), t.cash == 0 ? std::string() : fmt::format(" ({}{})", t.cash > 0 ? "+" : "-", GetString(STR_JUST_CURRENCY_LONG, t.cash > 0 ? t.cash : -t.cash))),
					t.ounces >= 0 ? CBP_WARN : CBP_GOOD);
		}
		col_bottom[2] = top6 + card_height(n6);
	}
	y = std::max({col_bottom[0], col_bottom[1], col_bottom[2]}) + gap;

	/* ---- Row 3: the rules of the bank, in two columns, and the long text under them. 4 rows + 2 lines = 6 rows. ---- */
	{
		const int n7 = 6;
		const int top7 = y;
		card(inner_left, inner_right, top7, n7, CbText("กฎของธนาคาร (RULES)", "RULES"));
		if (paint) DrawString(inner_left + pad_h, inner_right - pad_h, top7 + pad_v, fmt::format("{} {} ({} ~{} {})", POLICY_MEETINGS_PER_YEAR, CbText("ประชุม FOMC ครั้งต่อปีเกม", "FOMC meetings per game year"), CbText("ทุก", "every"), POLICY_DECISION_DAYS, CbText("วัน", "days")), TC_GREY, SA_RIGHT);
		const int a0 = inner_left + pad_h, a1 = inner_right - pad_h;
		const int mid = a0 + (a1 - a0) / 2;
		const int rows_top = y;
		/* Left half. */
		row(a0, mid - gap, CbText("ปรับได้ครั้งละ (ปกติ / ฉุกเฉิน)", "Move per meeting (normal / emergency)"), fmt::format("{} / {}", pct(POLICY_MAX_STEP_BP), pct(POLICY_EMERGENCY_STEP_BP)), TC_GREY);
		row(a0, mid - gap, CbText("ช่วงอัตรานโยบาย (ขอบบน)", "Policy rate range (upper bound)"), fmt::format("{} .. {}", pct(PolicyUpperBp(POLICY_SHIFT_MIN_BP)), pct(PolicyUpperBp(p.Extended() ? POLICY_EMERGENCY_SHIFT_MAX_BP : POLICY_SHIFT_MAX_BP))), TC_GREY);
		row(a0, mid - gap, CbText("แถบเป้าหมายเงินเฟ้อ", "Inflation target band"), fmt::format("{} (+/-{})", pct(POLICY_TARGET_INFLATION_BP), pct(POLICY_DEADBAND_BP)), TC_GREY);
		row(a0, mid - gap, CbText("โหมดฉุกเฉิน เริ่ม / สิ้นสุด เมื่อเงินเฟ้อ", "Emergency mode on / off at inflation"), fmt::format("> {} / < {}", pct(POLICY_EMERGENCY_ON_BP), pct(POLICY_EMERGENCY_OFF_BP)), TC_GREY);
		/* Right half. */
		const int left_end = y;
		y = rows_top;
		row(mid + gap, a1, CbText("ประชุมนอกรอบ ลด เมื่อจ้างงาน (ต่อปี)", "Unscheduled cut when jobs (yearly)"), fmt::format("<= {}", pct(POLICY_CRASH_JOBS_BP)), TC_GREY);
		row(mid + gap, a1, CbText("ประชุมนอกรอบ ขึ้น เมื่อเงินเฟ้อ (ต่อปี)", "Unscheduled hike when inflation (yearly)"), fmt::format(">= {}", pct(POLICY_SPIKE_INFLATION_BP)), TC_GREY);
		y = left_end;
		row2(a0, a1, CbText("ประชุมนอกรอบ จากข่าว", "Unscheduled meeting from the news"), CbText("ธนาคารล่ม/ถดถอยรุนแรง = ลด, เงินเฟ้อช็อก = ขึ้น", "bank scare / big recession = cut, big inflation scare = hike"), TC_GREY);
		y = top7 + card_height(n7);
	}

	return y + margin - panel_top;
}

/** The monetary policy window: its own window, so it can stay open next to the bank window. */
struct CentralBankPolicyWindow : Window {
	/** Redraw regularly: the numbers change all the time. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(100), [this](auto) {
		this->SetDirty();
	}};

	CentralBankPolicyWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(CB_WINDOW_POLICY);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_CBP_PANEL) return;
		size.width = std::max<uint>(size.width, ScaleGUITrad(980));
		size.height = std::max<uint>(size.height, DrawMonetaryPolicy(nullptr));
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget == WID_CBP_PANEL) DrawMonetaryPolicy(&r);
	}
};

/** The past statements window ("Statement ย้อนหลัง"): the last 10 years in a list, and the statement of the chosen year. */
struct CentralBankArchiveWindow : Window {
	int selected = 0; ///< Chosen year: 0 = this year, 9 = nine years ago.

	/** Redraw regularly: this year changes all the time. */
	const IntervalTimer<TimerWindow> redraw_interval = {std::chrono::milliseconds(100), [this](auto) {
		this->SetDirty();
	}};

	CentralBankArchiveWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(CB_WINDOW_ARCHIVE);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetDirty();
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget != WID_CBA_YEARS) return;
		const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_CBA_YEARS);
		const int row_h = CbRowHeight() + ScaleGUITrad(6);
		const int row = (pt.y - static_cast<int>(nw->pos_y) - ScaleGUITrad(4)) / row_h - 1; // the first row is the heading
		if (pt.y < static_cast<int>(nw->pos_y) + ScaleGUITrad(4) + row_h || row < 0 || row >= ARCHIVE_YEARS) return;
		this->selected = row;
		this->SetDirty();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_CBA_YEARS:
				size.width = std::max<uint>(size.width, ScaleGUITrad(190));
				size.height = std::max<uint>(size.height, (CbRowHeight() + ScaleGUITrad(6)) * (ARCHIVE_YEARS + 1) + ScaleGUITrad(12));
				break;
			case WID_CBA_PANEL:
				size.width = std::max<uint>(size.width, ScaleGUITrad(640));
				size.height = std::max<uint>(size.height, CbRowHeight() * 38 + ScaleGUITrad(12));
				break;
			default: break;
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_CBA_YEARS: DrawArchiveYears(r, this->selected); break;
			case WID_CBA_PANEL: DrawArchiveStatement(r, this->selected); break;
			default: break;
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
	NWidget(NWID_HORIZONTAL),
		/* Left: the numbers and the three buttons. */
		NWidget(NWID_VERTICAL),
			NWidget(WWT_PANEL, COLOUR_GREY, WID_CB_SUMMARY), SetMinimalSize(270, 120), SetResize(0, 1), SetFill(1, 1),
			EndContainer(),
			/* Test tool: move the game clock forward. Shown only after 'stock_dev on' in the console (plane 0); plane 1 is empty. */
			NWidget(NWID_SELECTION, INVALID_COLOUR, WID_CB_SKIP_SEL),
				NWidget(NWID_HORIZONTAL),
					NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CB_SKIP_1H), SetMinimalSize(52, 14), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_SKIP_1H, STR_CENTRAL_BANK_TIP_SKIP),
					NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CB_SKIP_1D), SetMinimalSize(52, 14), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_SKIP_1D, STR_CENTRAL_BANK_TIP_SKIP),
					NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CB_SKIP_7D), SetMinimalSize(52, 14), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_SKIP_7D, STR_CENTRAL_BANK_TIP_SKIP),
					NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CB_SKIP_30D), SetMinimalSize(52, 14), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_SKIP_30D, STR_CENTRAL_BANK_TIP_SKIP),
					NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CB_SKIP_90D), SetMinimalSize(52, 14), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_SKIP_90D, STR_CENTRAL_BANK_TIP_SKIP),
				EndContainer(),
				NWidget(NWID_SPACER),
			EndContainer(),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_PUSHTXTBTN, COLOUR_GREEN, WID_CB_BORROW), SetMinimalSize(80, 14), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_BORROW, STR_CENTRAL_BANK_TIP_BORROW),
				NWidget(WWT_PUSHTXTBTN, COLOUR_ORANGE, WID_CB_REPAY), SetMinimalSize(80, 14), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_REPAY, STR_CENTRAL_BANK_TIP_REPAY),
				NWidget(WWT_PUSHTXTBTN, COLOUR_RED, WID_CB_REPAY_ALL), SetMinimalSize(90, 14), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_REPAY_ALL, STR_CENTRAL_BANK_TIP_REPAY_ALL),
			EndContainer(),
		EndContainer(),
		/* Right: the account log and the button of the statement window. */
		NWidget(NWID_VERTICAL),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_PANEL, COLOUR_GREY, WID_CB_LOG), SetMinimalSize(300, 120), SetResize(1, 1), SetFill(1, 1), SetScrollbar(WID_CB_LOG_SCROLL),
				EndContainer(),
				NWidget(NWID_VSCROLLBAR, COLOUR_GREY, WID_CB_LOG_SCROLL),
			EndContainer(),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_CB_OPEN_STATEMENT), SetMinimalSize(100, 14), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_STATEMENT, STR_CENTRAL_BANK_TIP_STATEMENT),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_CB_OPEN_POLICY), SetMinimalSize(100, 14), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_CENTRAL_BANK_POLICY, STR_CENTRAL_BANK_TIP_POLICY),
				NWidget(WWT_RESIZEBOX, COLOUR_GREY),
			EndContainer(),
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
	NWidget(WWT_PANEL, COLOUR_GREY, WID_CBS_PANEL), SetMinimalSize(760, 120), SetFill(1, 1), SetResize(1, 1),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_GREY), SetFill(1, 1), SetResize(1, 0),
		EndContainer(),
		NWidget(WWT_PUSHBTN, COLOUR_GREY, WID_CBS_OLDER), SetMinimalSize(190, 14),
		NWidget(WWT_RESIZEBOX, COLOUR_GREY),
	EndContainer(),
};

/* The monetary policy window has its own look: a grey bar with the title (the strong blue was too glaring) on the left, and the close button in the top right corner
 * (the close box works by its widget type, so its place in the row does not matter). */
static constexpr std::initializer_list<NWidgetPart> _nested_central_bank_policy_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_CBP_CAPTION), SetStringTip(STR_CENTRAL_BANK_POLICY_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS), SetAlignment(SA_LEFT | SA_VERT_CENTER),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_CBP_PANEL), SetMinimalSize(380, 120), SetFill(1, 1), SetResize(1, 1),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_GREY), SetFill(1, 0), SetResize(1, 0),
		EndContainer(),
		NWidget(WWT_RESIZEBOX, COLOUR_GREY),
	EndContainer(),
};

static constexpr std::initializer_list<NWidgetPart> _nested_central_bank_archive_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_CBA_CAPTION), SetStringTip(STR_CENTRAL_BANK_ARCHIVE_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_CBA_YEARS), SetMinimalSize(190, 120), SetFill(0, 1), SetResize(0, 1),
		EndContainer(),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_CBA_PANEL), SetMinimalSize(640, 120), SetFill(1, 1), SetResize(1, 1),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_GREY), SetFill(1, 0), SetResize(1, 0),
		EndContainer(),
		NWidget(WWT_RESIZEBOX, COLOUR_GREY),
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

static WindowDesc _central_bank_policy_desc(
	WDP_AUTO, "central_bank_policy", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_central_bank_policy_widgets
);

static WindowDesc _central_bank_archive_desc(
	WDP_AUTO, "central_bank_archive", 0, 0,
	WC_CENTRAL_BANK, WC_NONE,
	{},
	_nested_central_bank_archive_widgets
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

/** Open the monetary policy window (window number 4 of #WC_CENTRAL_BANK), or bring it to the front. It can be open together with the bank window. */
void ShowCentralBankPolicyWindow()
{
	if (!Company::IsValidID(_local_company)) return;
	if (BringWindowToFrontById(WC_CENTRAL_BANK, CB_WINDOW_POLICY) != nullptr) return;
	new CentralBankPolicyWindow(_central_bank_policy_desc);
}

/** Open the past statements window (window number 2 of #WC_CENTRAL_BANK): the last 10 years, or bring it to the front. */
void ShowCentralBankArchiveWindow()
{
	if (!Company::IsValidID(_local_company)) return;
	if (BringWindowToFrontById(WC_CENTRAL_BANK, CB_WINDOW_ARCHIVE) != nullptr) return;
	new CentralBankArchiveWindow(_central_bank_archive_desc);
}
