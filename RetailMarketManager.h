/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file RetailMarketManager.h The retail investors of the towns: they put the savings of the inhabitants into the shares of the companies.
 *
 * Once per game day, at 00:00, the manager
 *   1. counts the savings that the towns put in the market (more when the transport of a town works),
 *   2. forms an opinion about every company: trend following (FOMO) and recent IPOs, dividend yield with a strong balance sheet, and panic,
 *   3. turns the opinions into buy and sell orders of two groups of investors, matches them against each other,
 *      and sends only what is left over to the trading terminal as pressure on the price (StockMarket::ApplyImpact),
 *   4. pays the dividends of the shares that the retail investors own (the company pays, from its cash),
 *   5. gives the dividends and capital gains back to the towns: they grow faster and want more transport.
 *
 * The numbers are in RetailMath.h. There is no UI in this file.
 */

#ifndef RETAIL_MARKET_MANAGER_H
#define RETAIL_MARKET_MANAGER_H

#include "RetailMath.h"
#include "company_type.h"
#include "town_type.h"
#include "game_time/game_time_manager.h"
#include <array>
#include <cstdint>
#include <vector>

/** What the retail investors did with one company on the last day. For a later UI, and for tuning. */
struct RetailCompanyReport {
	retail::Sentiment sentiment{};    ///< The opinion.
	int64_t held_shares = 0;          ///< Shares that both groups own together.
	int64_t bought = 0;               ///< Shares bought on the last day (matched ones included).
	int64_t sold = 0;                 ///< Shares sold on the last day (matched ones included).
	int64_t matched = 0;              ///< Shares that were bought and sold between the two groups, without any effect on the price.
	int64_t net_pressure = 0;         ///< Shares sent to the trading terminal: positive = buying pressure, negative = selling pressure.
};

class RetailMarketManager {
public:
	static RetailMarketManager &Get();

	void Reset();
	void OnNewGameDay(const GameTimeSnapshot &now);
	void OnNewGameHour(const GameTimeSnapshot &now);

	/* Read only access, for the trading terminal and for the later UI. */
	int64_t GetCash() const;
	int64_t GetCapitalWanted() const { return this->capital_wanted; }
	int64_t GetHoldingsValue() const;
	int64_t GetHeldShares(CompanyID company) const;
	const RetailCompanyReport &GetReport(CompanyID company) const;
	int64_t GetTownGrowthBoostPpm(TownID town) const;
	int64_t GetTownDemandBoostPpm(TownID town) const;

private:
	/** The data of one company. */
	struct CompanyRecord {
		bool seen = false;                                    ///< False until the first day that the company has a price; the IPO price is taken then.
		int age_days = 0;                                     ///< Game days since the first day that the retail investors saw the company.
		int64_t first_price = 0;                              ///< Price (money units) of one share on that day.
		std::array<int64_t, retail::MOMENTUM_LOOKBACK_DAYS + 1> price_fp{}; ///< The latest daily prices (times STOCK_PRICE_SCALE), a ring buffer.
		int price_count = 0;                                  ///< Number of valid entries of #price_fp.
		int price_head = 0;                                   ///< Where the next price is written.
		int panic_cooldown = 0;                               ///< Game days left in which nobody buys the company after a panic.
		std::array<int64_t, retail::COHORT_COUNT> held{};     ///< Shares owned per group.
		std::array<int64_t, retail::COHORT_COUNT> basis{};    ///< What the shares of each group cost together, money units.
		RetailCompanyReport report{};                         ///< What happened on the last day.
	};

	/** The wealth effect of one town. */
	struct TownRecord {
		retail::TownInput input{};          ///< Population and service coverage of today.
		int64_t boost_ppm = 0;              ///< Growth boost, 0 .. retail::MAX_BOOST_PPM.
		int64_t base_growth_rate = 0;       ///< The growth rate that OpenTTD calculated, before the boost.
		int64_t applied_growth_rate = 0;    ///< The growth rate that this manager wrote; when the town has another one, OpenTTD has calculated a new one.
	};

	std::array<CompanyRecord, MAX_COMPANIES> companies{};
	std::array<int64_t, retail::COHORT_COUNT> cash{};  ///< Money that each group has not invested.
	int64_t capital_wanted = 0;                        ///< Total money that the towns want to have in the market (all the towns together).
	int64_t wealth_pool = 0;                           ///< Money that was spent out of dividends and gains and has not been given to the towns yet.
	int64_t last_dividend_day = 0;                     ///< Number of the game day of the last dividend payment.
	std::vector<TownRecord> towns{};                   ///< Per town, indexed by the number of the town.
	/** The orders that the investors decided on at 00:00; they are carried out in 24 equal parts, one per game hour. Not saved: after a load the rest of the day is skipped. */
	struct DayPlan {
		bool valid = false;
		std::array<int64_t, retail::COHORT_COUNT> buy{};
		std::array<int64_t, retail::COHORT_COUNT> sell{};
	};
	std::array<DayPlan, MAX_COMPANIES> day_plan{};
	bool plan_active = false;
	bool initialized = false;                          ///< False until the first day. The companies that exist on the first day are established, not IPOs.

	void UpdateCompanyRecords();
	void RefreshCapital(const GameTimeSnapshot &now);
	retail::CompanySignals GatherSignals(CompanyID company) const;
	void TradeDay();
	void ExecuteSlice(int hour);
	void PayDividends(int64_t day);
	void ApplyMacroFeedback();
};

#endif /* RETAIL_MARKET_MANAGER_H */
