/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file town_wallet.cpp City wallet, see town_wallet_math.h and town_wallet.h. Integer maths only, the same on every client. */

#include "stdafx.h"
#include "town_wallet.h"
#include "town_wallet_math.h"
#include "town_treasury.h"
#include "3rdparty/fmt/format.h"
#include "openttd.h"
#include "central_bank.h"
#include "zone_market.h"
#include "cargo_market.h"
#include "cargotype.h"
#include "station_base.h"
#include "game_time/game_time_manager.h"
#include "news_func.h"
#include "strings_func.h"
#include "town.h"
#include "table/strings.h"

#include <algorithm>
#include <vector>

#include "safeguards.h"

/** Goods, Food and Water are bought by the citizens of the town. */
static bool IsConsumerCargo(CargoType cargo)
{
	const TownAcceptanceEffect e = CargoSpec::Get(cargo)->town_acceptance_effect;
	return e == TAE_GOODS || e == TAE_FOOD || e == TAE_WATER;
}

/** The size of the town in money terms: citizens x the price index, so that the limits follow the inflation of the game. */
static int64_t ScaledPopulation(const Town *t)
{
	return std::max<int64_t>(1, static_cast<int64_t>(t->cache.population) * GetPriceIndexPpm() / townwallet::PPM);
}

/** The town is short of `need`: it sells bonds to the saving citizens (their cash), then borrows from the central bank of its zone (the money of the bank). */
static void TownBorrow(TownAccount &a, const Town *t, int64_t need)
{
	CentralBank &bank = BankOfTile(t->xy);
	citizengold::Book &saver = a.books[static_cast<size_t>(citizengold::Style::Balanced)];
	const int64_t bank_free = bank.trading_revenue_cents / 100 - BANK_IORB_BUDGET_FLOOR;
	const townwallet::Funding f = townwallet::Fund(need, ScaledPopulation(t), a.bond_debt, a.loan_debt, saver.cash, bank_free);
	if (f.from_bonds > 0) {
		saver.cash -= f.from_bonds;
		a.bond_debt += f.from_bonds;
		a.treasury += f.from_bonds;
	}
	if (f.from_loan > 0) {
		bank.trading_revenue_cents -= f.from_loan * 100;
		a.loan_debt += f.from_loan;
		a.treasury += f.from_loan;
	}
}

/** The account of the town of a station, or nullptr (no treasury yet: the base game pays). */
static TownAccount *AccountOfStation(StationID station)
{
	if (!Station::IsValidID(station)) return nullptr;
	const Town *t = Station::Get(station)->town;
	if (t == nullptr) return nullptr;
	auto it = _town_treasury.accounts.find(t->index.base());
	if (it == _town_treasury.accounts.end() || !it->second.started) return nullptr;
	return &it->second;
}

/** Wages: the citizens of the town that paid get a part of the payment back as cash (split by the usual shares of the four groups; the last group takes the rest). */
static void PayWages(TownAccount &a, int64_t wage)
{
	if (wage <= 0) return;
	int64_t given = 0;
	for (size_t i = 0; i < citizengold::STYLE_COUNT; i++) {
		const int64_t part = i + 1 == citizengold::STYLE_COUNT ? wage - given : wage * citizengold::BASE_SHARE_PPM[i] / citizengold::PPM;
		a.books[i].cash += part;
		given += part;
	}
	a.wages_back += wage;
}

int64_t TownPayForDelivery(CargoType cargo, StationID source, StationID dest, int64_t income)
{
	if (income <= 0 || !IsValidCargoType(cargo) || !Station::IsValidID(dest)) return income;
	if (CargoMarketIsGoldCargo(cargo)) return income; // the gold market prices gold
	const TownAcceptanceEffect effect = CargoSpec::Get(cargo)->town_acceptance_effect;
	if (CargoMarketIsPeopleCargo(cargo) || effect == TAE_PASSENGERS || effect == TAE_MAIL) {
		/* The town of ORIGIN pays: passengers from the cash of its citizens (all four groups, in proportion), mail from its treasury. */
		TownAccount *src = AccountOfStation(source);
		if (src == nullptr) return income;
		if (effect == TAE_MAIL) {
			const townwallet::Payment p = townwallet::PayCapped(income, src->treasury, townwallet::MAIL_TREASURY_CAP_PERCENT);
			src->treasury -= p.from_town;
			src->mail_paid += p.paid;
			src->fare_shortfall += p.shortfall;
			const int64_t wage = townwallet::WageOf(p.paid); // the company gets the rest, the citizens of the town of origin get the wages
			PayWages(*src, wage);
			return p.paid - wage;
		}
		int64_t cash = 0;
		for (const citizengold::Book &b : src->books) cash += std::max<int64_t>(b.cash, 0);
		const townwallet::Payment p = townwallet::PayCapped(income, cash, townwallet::FARE_CASH_CAP_PERCENT);
		int64_t left = p.from_town;
		for (size_t i = 0; i < citizengold::STYLE_COUNT && left > 0; i++) {
			citizengold::Book &b = src->books[i];
			const int64_t have = std::max<int64_t>(b.cash, 0);
			const int64_t take = i + 1 == citizengold::STYLE_COUNT ? std::min(left, have) : std::min(left, have * p.from_town / std::max<int64_t>(cash, 1));
			b.cash -= take;
			left -= take;
		}
		for (size_t i = 0; i < citizengold::STYLE_COUNT && left > 0; i++) { // a rest from rounding: take it where there is cash
			citizengold::Book &b = src->books[i];
			const int64_t take = std::min(left, std::max<int64_t>(b.cash, 0));
			b.cash -= take;
			left -= take;
		}
		const int64_t paid = p.from_town - left;
		src->fares_paid += paid;
		src->fare_shortfall += income - paid;
		const int64_t wage = townwallet::WageOf(paid);
		PayWages(*src, wage);
		return paid - wage;
	}
	const Town *t = Station::Get(dest)->town;
	TownAccount *dst = AccountOfStation(dest); // a village without a treasury: the base game pays
	if (t == nullptr || dst == nullptr) return income;
	TownAccount &a = *dst;

	if (IsConsumerCargo(cargo)) {
		if (a.treasury < income) TownBorrow(a, t, income - std::max<int64_t>(a.treasury, 0));
		const townwallet::Payment p = townwallet::PayConsumer(income, a.treasury);
		a.treasury -= p.from_town;
		a.goods_paid += p.paid;
		a.shortfall += p.shortfall;
		const int64_t wage = townwallet::WageOf(p.paid);
		PayWages(a, wage);
		return p.paid - wage;
	}
	const townwallet::Payment p = townwallet::PayIndustrial(income);
	a.treasury += p.tax;
	a.taxed += p.tax;
	a.business_tax += p.tax;
	return p.paid;
}

/** One game day: interest and coupons, repayment, and once a year the grant. */
static void TownWalletDaily(const GameTimeSnapshot &now)
{
	if (_game_mode != GM_NORMAL) return;
	const int64_t day = static_cast<int64_t>(now.total_minutes / GameTimeManager::MINUTES_PER_DAY);

	struct Candidate { TownAccount *account; int64_t weight; int64_t population; };
	std::vector<Candidate> by_bank[BANK_ZONES];
	int64_t total_weight[BANK_ZONES] = {};

	for (auto &[key, a] : _town_treasury.accounts) {
		if (!a.started) continue;
		const TownID id{static_cast<uint16_t>(key)};
		if (!Town::IsValidID(id)) continue;
		const Town *t = Town::Get(id);
		const int64_t sp = ScaledPopulation(t);
		CentralBank &bank = BankOfTile(t->xy);
		citizengold::Book &saver = a.books[static_cast<size_t>(citizengold::Style::Balanced)];
		const int64_t upper_bp = PolicyUpperBp(bank.ShiftBp());

		/* Interest on the loan goes to the bank, the coupon of the bonds to the saving citizens. What the treasury cannot pay is added to the debt. */
		const int64_t li = townwallet::DailyInterest(a.loan_debt, townwallet::LoanRateBp(upper_bp), a.loan_carry);
		const int64_t pay_l = std::min(std::max<int64_t>(a.treasury, 0), li);
		a.treasury -= pay_l;
		bank.trading_revenue_cents += pay_l * 100;
		a.loan_debt += li - pay_l;
		a.interest_paid += pay_l;
		const int64_t bi = townwallet::DailyInterest(a.bond_debt, townwallet::BondRateBp(upper_bp), a.bond_carry);
		const int64_t pay_b = std::min(std::max<int64_t>(a.treasury, 0), bi);
		a.treasury -= pay_b;
		saver.cash += pay_b;
		a.bond_debt += bi - pay_b;
		a.interest_paid += pay_b;

		/* Repayment from the treasury above its floor: the loan first. */
		const townwallet::Repay r = townwallet::RepayDay(a.treasury, sp, a.loan_debt, a.bond_debt);
		a.treasury -= r.loan + r.bonds;
		bank.trading_revenue_cents += r.loan * 100;
		a.loan_debt -= r.loan;
		saver.cash += r.bonds;
		a.bond_debt -= r.bonds;

		if (day > 0 && day % townwallet::DAYS_PER_YEAR == 0 && bank.zone >= 0 && bank.zone < BANK_ZONES) {
			const int64_t w = townwallet::GrantWeight(sp, a.treasury);
			by_bank[bank.zone].push_back({&a, w, sp});
			total_weight[bank.zone] += w;
		}
	}

	/* The yearly grant: every central bank gives a share of its own money (above its floor) to the towns of its zone, by size and need. */
	if (day > 0 && day % townwallet::DAYS_PER_YEAR == 0) {
		for (int z = 0; z < BANK_ZONES; z++) {
			CentralBank &bank = _banks[z];
			const int64_t pool = townwallet::GrantPool(bank.trading_revenue_cents / 100, BANK_IORB_BUDGET_FLOOR);
			if (pool <= 0 || total_weight[z] <= 0) continue;
			int64_t paid = 0;
			size_t count = 0;
			for (const Candidate &c : by_bank[z]) {
				const int64_t g = townwallet::GrantOf(pool, c.weight, total_weight[z], c.population);
				if (g <= 0) continue;
				c.account->treasury += g;
				c.account->grants += g;
				paid += g;
				count++;
			}
			bank.trading_revenue_cents -= paid * 100;
			if (paid > 0) {
				AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, fmt::format("The central bank of zone {} paid {} in grants to {} towns.",
						ZoneCurrencyName(z), GetString(STR_JUST_CURRENCY_LONG, paid), count)), NewsType::General, NewsStyle::Small, {});
			}
		}
	}
}

static const GameTimeSubscription _town_wallet_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &now) { TownWalletDaily(now); }, 21);
