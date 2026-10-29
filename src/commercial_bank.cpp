/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file commercial_bank.cpp The commercial banks of the towns, see commercial_bank.h and commercial_bank_math.h. Integer maths only, the same on every client. */

#include "stdafx.h"
#include "commercial_bank.h"
#include "commercial_bank_math.h"
#include "town_treasury.h"
#include "town_wallet_math.h"
#include "central_bank.h"
#include "citizen_gold.h"
#include "market_events.h"
#include "game_time/game_time_manager.h"
#include "3rdparty/fmt/format.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "core/backup_type.hpp"
#include "economy_func.h"
#include "industry.h"
#include "industrytype.h"
#include "industry_cmd.h"
#include "map_func.h"
#include "tile_map.h"
#include "news_func.h"
#include "openttd.h"
#include "strings_func.h"
#include "town.h"
#include "window_func.h"

#include "table/strings.h"

#include <algorithm>
#include <climits>
#include <optional>
#include <set>
#include <variant>
#include <vector>

#include "safeguards.h"

static_assert(cbank::GROUPS == static_cast<int>(citizengold::STYLE_COUNT), "one deposit per group of citizens");

CommercialBankManager _commercial_banks;

static uint32_t TownKey(const Town *t) { return t->index.base(); }

static const Town *TownOf(uint32_t key)
{
	const TownID id{static_cast<uint16_t>(key)};
	return Town::IsValidID(id) ? Town::Get(id) : nullptr;
}

/** Everything that a bank has lent to companies. */
static int64_t FirmLoans(uint32_t town)
{
	int64_t sum = 0;
	for (auto it = _commercial_banks.loans.lower_bound(CBankLoanKey(town, 0)); it != _commercial_banks.loans.end() && (it->first >> 4) == town; ++it) sum += it->second.debt;
	return sum;
}

static int64_t TownLoan(uint32_t town)
{
	const auto it = _town_treasury.accounts.find(town);
	return it == _town_treasury.accounts.end() ? 0 : it->second.cb_debt;
}

int64_t CBankLoansTotal(uint32_t town)
{
	if (_commercial_banks.banks.find(town) == _commercial_banks.banks.end()) return 0;
	return FirmLoans(town) + TownLoan(town);
}

int64_t CBankEquity(uint32_t town)
{
	const auto it = _commercial_banks.banks.find(town);
	return it == _commercial_banks.banks.end() ? 0 : cbank::Equity(it->second.sheet, CBankLoansTotal(town));
}

int CBankRating(uint32_t town)
{
	const auto it = _commercial_banks.banks.find(town);
	if (it == _commercial_banks.banks.end()) return 0;
	return cbank::RatingIndex(CBankEquity(town), CBankLoansTotal(town));
}

int64_t GetCommercialDebtAt(uint32_t town, CompanyID company)
{
	if (company.base() >= MAX_COMPANIES) return 0;
	const auto it = _commercial_banks.loans.find(CBankLoanKey(town, company.base()));
	return it == _commercial_banks.loans.end() ? 0 : it->second.debt;
}

int64_t GetCommercialDebt(CompanyID company)
{
	if (company.base() >= MAX_COMPANIES) return 0;
	int64_t sum = 0;
	for (const auto &[key, a] : _commercial_banks.loans) if ((key & 15) == company.base()) sum += a.debt;
	return sum;
}

int64_t GetCreditPoolRoom(CompanyID company)
{
	if (!Company::IsValidID(company)) return 0;
	return std::max<int64_t>(0, cbank::TotalCreditLimit(GetCentralBankCreditLimit(company)) - GetCentralBankDebt(company) - GetCommercialDebt(company));
}

/** The signed spread of a bank over the central bank rate (its rating, a bank building). */
static int64_t MarginOf(uint32_t town)
{
	const auto it = _commercial_banks.banks.find(town);
	if (it == _commercial_banks.banks.end()) return 0;
	return cbank::LoanMarginBp(CBankRating(town), it->second.has_building);
}

int64_t CBankLoanRateBp(uint32_t town, CompanyID company)
{
	if (!Company::IsValidID(company)) return 0;
	const CentralBank &cb = BankOfCompany(company);
	BankAccount a;
	const auto it = _commercial_banks.loans.find(CBankLoanKey(town, company.base()));
	if (it != _commercial_banks.loans.end()) a = it->second;
	if (a.in_default) return BankPenaltyRateBp(std::max<int>(1, a.penalty_level), cb.ShiftBp());
	const int64_t news_bp = _market_events.CreditSpreadBp(company.base(), static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes()));
	return BankRiskRateBp(BankAnnualRateBp(BankDebtToEquityPpm(a.debt, GetCentralBankEquity(company))), news_bp, MarginOf(town), cb.ShiftBp());
}

int64_t CBankAvailable(uint32_t town, CompanyID company)
{
	const auto bit = _commercial_banks.banks.find(town);
	const Town *t = TownOf(town);
	if (bit == _commercial_banks.banks.end() || t == nullptr || !Company::IsValidID(company)) return 0;
	if (BankOfTile(t->xy).zone != BankOfCompany(company).zone) return 0; // a bank lends in the money of its own zone
	const auto lit = _commercial_banks.loans.find(CBankLoanKey(town, company.base()));
	if (lit != _commercial_banks.loans.end() && lit->second.in_default) return 0;
	const int64_t loans = CBankLoansTotal(town);
	const int64_t equity = cbank::Equity(bit->second.sheet, loans);
	const int64_t exposure = cbank::LargeExposureLimit(equity) - GetCommercialDebtAt(town, company);
	const int64_t penalty_pct = lit != _commercial_banks.loans.end() ? BankCreditPercentForPenalty(lit->second.penalty_level) : 100;
	return std::max<int64_t>(0, std::min({GetCreditPoolRoom(company) * penalty_pct / 100, cbank::Capacity(bit->second.sheet, equity, loans), exposure}));
}

std::optional<uint32_t> CBankNearestToHQ(CompanyID company)
{
	if (!Company::IsValidID(company)) return std::nullopt;
	const TileIndex hq = Company::Get(company)->location_of_HQ;
	if (hq == INVALID_TILE) return std::nullopt;
	const auto zone = BankOfCompany(company).zone;
	std::optional<uint32_t> best;
	uint best_dist = 0;
	for (const auto &[key, b] : _commercial_banks.banks) { // map order: on a tie the lower town index wins, the same on every client
		const Town *t = TownOf(key);
		if (t == nullptr || BankOfTile(t->xy).zone != zone) continue;
		const uint d = DistanceManhattan(hq, t->xy);
		if (!best.has_value() || d < best_dist) { best = key; best_dist = d; }
	}
	return best;
}

BankAccount CBankAccountOf(uint32_t town, CompanyID company)
{
	if (company.base() >= MAX_COMPANIES) return BankAccount{};
	const auto it = _commercial_banks.loans.find(CBankLoanKey(town, company.base()));
	return it == _commercial_banks.loans.end() ? BankAccount{} : it->second;
}

/** The commercial bank lends to its town (see the header). */
int64_t CBankLendToTown(uint32_t town, TownAccount &account, int64_t scaled_population, int64_t want)
{
	const auto it = _commercial_banks.banks.find(town);
	if (it == _commercial_banks.banks.end() || want <= 0) return 0;
	CommercialBank &b = it->second;
	const int64_t loans = CBankLoansTotal(town);
	const int64_t equity = cbank::Equity(b.sheet, loans);
	const int64_t room = std::max<int64_t>(0, cbank::TownLoanLimit(scaled_population) - account.cb_debt);
	const int64_t give = std::min({want, room, cbank::Capacity(b.sheet, equity, loans)});
	if (give <= 0) return 0;
	b.sheet.liquid -= give;
	account.cb_debt += give;
	account.treasury += give;
	return give;
}

static void InvalidateBankData() { InvalidateWindowData(WC_CENTRAL_BANK, 0); }

/**
 * Borrow from the commercial bank of a town. The money goes into the cash of the company. Same rules as the central bank: only inside the money zone of the
 * company's headquarters, within the credit pool of the company, the capacity of the bank and the large exposure limit; a company in default at that bank gets nothing.
 */
CommandCost CmdCBankBorrow(DoCommandFlags flags, uint16_t town, Money amount_in)
{
	const int64_t amount = amount_in;
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;
	if (amount <= 0) return CommandCost(STR_STOCK_ERROR_BAD_AMOUNT);
	const Town *t = TownOf(town);
	if (t == nullptr || _commercial_banks.banks.find(town) == _commercial_banks.banks.end()) return CommandCost(STR_CBANK_ERROR_NO_BANK);
	if (BankOfTile(t->xy).zone != BankOfCompany(_current_company).zone) return CommandCost(STR_CBANK_ERROR_OTHER_ZONE);
	const auto lit = _commercial_banks.loans.find(CBankLoanKey(town, _current_company.base()));
	if (lit != _commercial_banks.loans.end() && lit->second.in_default) return CommandCost(STR_CBANK_ERROR_DEFAULT);
	if (amount > CBankAvailable(town, _current_company)) return CommandCost(STR_CBANK_ERROR_LIMIT);

	if (flags.Test(DoCommandFlag::Execute)) {
		_commercial_banks.banks[town].sheet.liquid -= amount;
		_commercial_banks.loans[CBankLoanKey(town, _current_company.base())].debt += amount;
		Company::Get(_current_company)->money += Money(amount);
		RecordLedger(_current_company, LedgerItem::BankLoan, amount);
		InvalidateBankData();
	}
	return CommandCost();
}

/** Repay a loan of the commercial bank of a town from the cash of the company. */
CommandCost CmdCBankRepay(DoCommandFlags flags, uint16_t town, Money amount_in)
{
	const int64_t amount = amount_in;
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;
	if (amount <= 0) return CommandCost(STR_STOCK_ERROR_BAD_AMOUNT);
	const int64_t debt = GetCommercialDebtAt(town, _current_company);
	if (debt <= 0) return CommandCost(STR_CBANK_ERROR_NO_DEBT);
	if (amount > debt) return CommandCost(STR_STOCK_ERROR_BAD_AMOUNT);
	if (static_cast<int64_t>(Company::Get(_current_company)->money) < amount) return CommandCost(STR_STOCK_ERROR_NO_FUNDS);

	if (flags.Test(DoCommandFlag::Execute)) {
		Company::Get(_current_company)->money -= Money(amount); // A repayment is not an expense.
		_commercial_banks.banks[town].sheet.liquid += amount;
		_commercial_banks.loans[CBankLoanKey(town, _current_company.base())].debt -= amount;
		RecordLedger(_current_company, LedgerItem::BankLoan, -amount);
		InvalidateBankData();
	}
	return CommandCost();
}

/** The towns that have a bank building of the game (the bank industry). The labels of the spec are what gold_industry.cpp uses to find a bank. */
static std::set<uint32_t> TownsWithBankBuilding()
{
	std::set<uint32_t> out;
	for (const Industry *i : Industry::Iterate()) {
		if (i->town == nullptr) continue;
		const IndustrySpec *spec = GetIndustrySpec(i->type);
		for (const auto &v : spec->accepts_cargo_label) {
			if (const MixedCargoType *m = std::get_if<MixedCargoType>(&v); m != nullptr && *m == MCT_VALUABLES_GOLD_DIAMONDS) { out.insert(TownKey(i->town)); break; }
		}
	}
	return out;
}

/** True: every town gets a real bank building (the bank industry of the game) when it has none. False: only the virtual branch. */
static constexpr bool BANK_BUILD_BUILDINGS = true;
static constexpr int BANK_BUILD_TOWNS_PER_DAY = 4;   ///< At most this many towns are tried per game day (a failed town waits BANK_BUILD_RETRY_DAYS).
static constexpr int64_t BANK_BUILD_RETRY_DAYS = 30;
static constexpr int BANK_BUILD_RADIUS = 12;         ///< The bank is looked for this many tiles around the town centre.

/** Industry type of the bank of this climate (found by what it accepts, like gold_industry.cpp), or nothing. */
static std::optional<IndustryType> BankIndustryType()
{
	for (IndustryType it = 0; it < NUM_INDUSTRYTYPES; it++) {
		const IndustrySpec *spec = GetIndustrySpec(it);
		if (!spec->enabled || spec->layouts.empty()) continue;
		for (const auto &v : spec->accepts_cargo_label) {
			if (const MixedCargoType *m = std::get_if<MixedCargoType>(&v); m != nullptr && *m == MCT_VALUABLES_GOLD_DIAMONDS) return it;
		}
	}
	return std::nullopt;
}

/** A house tile whose neighbour to the east is a house too (the bank has two tiles side by side). */
static bool IsBankSite(int x, int y)
{
	if (x < 1 || y < 1 || x + 2 >= static_cast<int>(Map::SizeX()) || y + 1 >= static_cast<int>(Map::SizeY())) return false;
	return IsTileType(TileXY(x, y), MP_HOUSE) && IsTileType(TileXY(x + 1, y), MP_HOUSE);
}

/**
 * Build the bank building of the game in a town: the nearest pair of houses to the centre is replaced, as the game does for a bank in a town (the houses are cleared
 * as the town, the bank is founded by the deity, like a game script does). Runs the same on every client (no Random(), the seed comes from the town and the day).
 * @return True when the bank stands now.
 */
static bool BuildBankBuilding(const Town *t, IndustryType type, int64_t day)
{
	const int cx = static_cast<int>(TileX(t->xy)), cy = static_cast<int>(TileY(t->xy));
	for (int r = 0; r <= BANK_BUILD_RADIUS; r++) {
		for (int dy = -r; dy <= r; dy++) {
			for (int dx = -r; dx <= r; dx++) {
				if (std::max(std::abs(dx), std::abs(dy)) != r || !IsBankSite(cx + dx, cy + dy)) continue;
				const TileIndex tile = TileXY(cx + dx, cy + dy);
				if (ClosestTownFromTile(tile, UINT_MAX) != t) continue; // The game would give this bank to the nearer town.
				const uint32_t seed = static_cast<uint32_t>(t->index.base() * 2654435761u) ^ static_cast<uint32_t>(day * 40503);
				Backup<CompanyID> cur_company(_current_company, OWNER_DEITY);
				/* The temperate bank is refused by the game in towns under 1200 people. Every town gets one here, so the population is
				 * lifted for the check and restored at once (same on every client, nothing else runs in between). */
				Town *mt = const_cast<Town *>(t);
				const auto real_population = mt->cache.population;
				if (mt->cache.population < 1200) mt->cache.population = 1200;
				const CommandCost test = Command<CMD_BUILD_INDUSTRY>::Do({}, tile, type, 0, true, seed);
				const bool ok = test.Succeeded() && Command<CMD_BUILD_INDUSTRY>::Do(DoCommandFlag::Execute, tile, type, 0, true, seed).Succeeded();
				mt->cache.population = real_population;
				if (ok) return true;
			}
		}
	}
	return false;
}

/** The loans of a bank that belong to a company that is gone are lost against its capital. */
static void WriteOffGoneCompanies()
{
	for (auto it = _commercial_banks.loans.begin(); it != _commercial_banks.loans.end();) {
		const uint32_t town = it->first >> 4;
		const CompanyID c{static_cast<uint8_t>(it->first & 15)};
		const auto bank = _commercial_banks.banks.find(town);
		if (Company::IsValidID(c) && bank != _commercial_banks.banks.end()) { ++it; continue; }
		if (bank != _commercial_banks.banks.end()) bank->second.sheet.written_off += it->second.debt;
		it = _commercial_banks.loans.erase(it);
	}
}

/** The interest of the loans of one bank to companies at this midnight (the same account rules as the central bank). */
static void SettleFirmLoans(uint32_t town, CommercialBank &bank, int64_t day)
{
	const int64_t margin = MarginOf(town);
	for (auto it = _commercial_banks.loans.lower_bound(CBankLoanKey(town, 0)); it != _commercial_banks.loans.end() && (it->first >> 4) == town; ++it) {
		const CompanyID company{static_cast<uint8_t>(it->first & 15)};
		Company *c = Company::GetIfValid(company);
		if (c == nullptr) continue;
		BankAccount &a = it->second;
		const int64_t news_bp = _market_events.CreditSpreadBp(company.base(), day * GameTimeManager::MINUTES_PER_DAY);
		const BankDayResult r = BankSettleDay(a, static_cast<int64_t>(c->money), GetCentralBankEquity(company), day, BankOfCompany(company).ShiftBp(), news_bp, margin);
		if (r.paid_from_cash > 0) {
			AutoRestoreBackup cur_company(_current_company, company);
			SubtractMoneyFromCompany(CommandCost(EXPENSES_LOAN_INTEREST, Money(r.paid_from_cash)));
			bank.sheet.liquid += r.paid_from_cash;
			bank.sheet.loan_interest += r.paid_from_cash;
		}
		if (r.interest_due > 0) RecordLedger(company, LedgerItem::BankInterest, -r.interest_due);
	}
}

/** One game day of one bank. */
static void UpdateBank(uint32_t town, CommercialBank &bank, int64_t day, int64_t index_ppm)
{
	const Town *t = TownOf(town);
	if (t == nullptr) return;
	CentralBank &central = BankOfTile(t->xy);
	const int64_t upper_bp = PolicyUpperBp(central.ShiftBp());
	const int64_t central_free = std::max<int64_t>(0, central.trading_revenue_cents / 100 - BANK_IORB_BUDGET_FLOOR);
	const int rating = CBankRating(town);

	/* 1. The citizens move money between their cash and their deposits (a withdrawal is limited by the liquid assets of the bank). */
	auto acc = _town_treasury.accounts.find(town);
	const bool has_citizens = acc != _town_treasury.accounts.end() && acc->second.started;
	if (has_citizens) {
		for (int g = 0; g < cbank::GROUPS; g++) {
			citizengold::Book &book = acc->second.books[static_cast<size_t>(g)];
			int64_t mv = cbank::DepositMove(book.cash, bank.sheet.deposits[static_cast<size_t>(g)], cbank::DepositTargetPercent(g, bank.has_building));
			if (mv < 0) mv = -std::min(-mv, std::max<int64_t>(0, bank.sheet.liquid));
			book.cash -= mv;
			bank.sheet.deposits[static_cast<size_t>(g)] += mv;
			bank.sheet.liquid += mv;
		}
	}

	/* 2. Loans to companies: interest and default, like the central bank. */
	SettleFirmLoans(town, bank, day);

	/* 3. The loan of the town treasury: interest, then repayment from the treasury above its floor (the central loan and the bonds were served first). */
	if (has_citizens) {
		TownAccount &a = acc->second;
		const int64_t sp = std::max<int64_t>(1, static_cast<int64_t>(t->cache.population) * index_ppm / townwallet::PPM);
		const int64_t li = townwallet::DailyInterest(a.cb_debt, cbank::TownLoanRateBp(upper_bp, rating, bank.has_building), a.cb_carry);
		const int64_t pay = std::min(std::max<int64_t>(a.treasury, 0), li);
		a.treasury -= pay;
		bank.sheet.liquid += pay;
		bank.sheet.loan_interest += pay;
		a.cb_interest += pay;
		a.cb_debt += li - pay; // what the town can not pay is added to the debt
		const townwallet::Repay r = townwallet::RepayDay(a.treasury, sp, a.cb_debt, 0);
		a.treasury -= r.loan;
		bank.sheet.liquid += r.loan;
		a.cb_debt -= r.loan;
	}

	/* 4. The balance sheet: reserve interest, deposit interest, central bank loan, reserve requirement. */
	cbank::DayIn in;
	in.policy_upper_bp = upper_bp;
	in.iorb_bp = PolicyIorbBp(central.ShiftBp());
	in.cb_free = central_free;
	const cbank::DayOut out = cbank::Settle(bank.sheet, in);
	central.trading_revenue_cents += (out.cb_interest_paid + out.repaid - out.borrowed) * 100; // the central bank lends from its own money and gets it back with interest

	/* 5. A bank with negative equity is rescued by the central bank as far as its own money goes; the rest is taken from the deposits. */
	const cbank::Rescue rescue = cbank::Resolve(bank.sheet, CBankLoansTotal(town), std::max<int64_t>(0, central.trading_revenue_cents / 100));
	if (rescue.covered > 0 || rescue.haircut > 0) {
		central.trading_revenue_cents -= rescue.covered * 100;
		AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, fmt::format("The bank of {} was in trouble. The central bank paid {} to rescue it{}.",
				GetString(STR_TOWN_NAME, t->index), GetString(STR_JUST_CURRENCY_LONG, rescue.covered),
				rescue.haircut > 0 ? fmt::format(" and the depositors lost {}", GetString(STR_JUST_CURRENCY_LONG, rescue.haircut)) : std::string())),
				NewsType::General, NewsStyle::Small, {});
	}
}

void CommercialBankManager::UpdateDaily(int64_t day)
{
	const int64_t index_ppm = GetPriceIndexPpm();

	/* A town that is gone takes its bank with it; the loans of companies there are forgiven (the companies keep the cash). */
	for (auto it = this->banks.begin(); it != this->banks.end();) {
		if (TownOf(it->first) == nullptr) it = this->banks.erase(it); else ++it;
	}
	WriteOffGoneCompanies();

	/* Every town has a bank. A new one gets its capital by the size of the town. */
	for (const Town *t : Town::Iterate()) {
		const uint32_t key = TownKey(t);
		if (this->banks.find(key) != this->banks.end()) continue;
		CommercialBank b;
		b.sheet.liquid = cbank::StartCapital(t->cache.population, index_ppm);
		this->banks[key] = b;
	}

	std::set<uint32_t> with_building = TownsWithBankBuilding();
	if (BANK_BUILD_BUILDINGS) {
		const std::optional<IndustryType> bank_type = BankIndustryType();
		int tried = 0;
		for (auto &[key, b] : this->banks) {
			if (!bank_type.has_value() || tried >= BANK_BUILD_TOWNS_PER_DAY) break;
			if (with_building.count(key) != 0) continue;
			const auto wait = this->next_build_try.find(key);
			if (wait != this->next_build_try.end() && wait->second > day) continue;
			const Town *t = TownOf(key);
			if (t == nullptr) continue;
			tried++;
			if (BuildBankBuilding(t, *bank_type, day)) { with_building.insert(key); this->next_build_try.erase(key); }
			else this->next_build_try[key] = day + BANK_BUILD_RETRY_DAYS;
		}
	}
	for (auto &[key, b] : this->banks) b.has_building = with_building.count(key) != 0;
	for (auto &[key, b] : this->banks) UpdateBank(key, b, day, index_ppm);
	InvalidateBankData();
}

static const GameTimeSubscription _commercial_bank_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &now)
{
	if (_game_mode != GM_NORMAL) return;
	_commercial_banks.UpdateDaily(static_cast<int64_t>(now.total_minutes / GameTimeManager::MINUTES_PER_DAY));
}, 22);
