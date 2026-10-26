/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file cargo_market.cpp The price of cargo follows supply and demand. See cargo_market.h and CargoMarketMath.h. */

#include "stdafx.h"
#include "cargo_market.h"
#include "central_bank.h"
#include "market_events.h"
#include "valuables.h"
#include "cargotype.h"
#include "station_base.h"
#include "town.h"
#include "RetailMarketManager.h"
#include "RetailMath.h"
#include "economy_func.h"
#include "economy_news.h"
#include "openttd.h"
#include "game_time/game_time_manager.h"
#include "safeguards.h"

CargoMarket _cargo_market;

namespace {

/**
 * What the mod knows about a kind of cargo, found by its LABEL (never by its number: the numbers depend on the climate and on the NewGRFs that are loaded).
 * One table instead of many `if` lines, so that a new cargo (for example from an industry NewGRF) is added in ONE place. A cargo that is not in the table
 * gets the defaults: no news group, elasticity DEFAULT_ELASTICITY_PPM, local weight LOCAL_WEIGHT_OTHER_PPM. Its price still follows supply and demand
 * (that part needs no label); only the news of a group and the special reaction of the cargo are missing.
 */
struct CargoRule {
	CargoLabel label;
	MarketCargoGroup group;   ///< The group of cargo that the news can move (market_events.h); None for none.
	int64_t elasticity_ppm;   ///< How strongly the price answers to supply, in ppm (food a lot, coal and ore little).
	int64_t local_weight_ppm; ///< How much the cargo follows the town that it is delivered to, in ppm.
	bool people;              ///< Passengers and mail: people are not goods (elasticity 0, the fare follows the crowd).
};

static constexpr int64_t DEFAULT_ELASTICITY_PPM = 500'000; ///< Elasticity of a cargo that is not in the table.

/** The rule of a cargo, or nullptr when the label is not in the table. */
const CargoRule *FindCargoRule(const CargoSpec *cs)
{
	static const CargoRule rules[] = {
		/* label            news group              elasticity  local weight                               people */
		{ CargoLabel{'PASS'}, MarketCargoGroup::People, 0,        cargomarket::LOCAL_WEIGHT_PEOPLE_PPM,   true  },
		{ CargoLabel{'MAIL'}, MarketCargoGroup::People, 0,        cargomarket::LOCAL_WEIGHT_PEOPLE_PPM,   true  },
		{ CargoLabel{'FOOD'}, MarketCargoGroup::Farm,   800'000,  cargomarket::LOCAL_WEIGHT_CONSUMER_PPM, false },
		{ CargoLabel{'FRUT'}, MarketCargoGroup::Farm,   800'000,  cargomarket::LOCAL_WEIGHT_OTHER_PPM,    false },
		{ CargoLabel{'MILK'}, MarketCargoGroup::Farm,   800'000,  cargomarket::LOCAL_WEIGHT_OTHER_PPM,    false },
		{ CargoLabel{'GOOD'}, MarketCargoGroup::None,   550'000,  cargomarket::LOCAL_WEIGHT_CONSUMER_PPM, false },
		{ CargoLabel{'COAL'}, MarketCargoGroup::Coal,   350'000,  cargomarket::LOCAL_WEIGHT_OTHER_PPM,    false },
		{ CargoLabel{'IORE'}, MarketCargoGroup::Build,  350'000,  cargomarket::LOCAL_WEIGHT_OTHER_PPM,    false },
		{ CargoLabel{'CORE'}, MarketCargoGroup::Build,  350'000,  cargomarket::LOCAL_WEIGHT_OTHER_PPM,    false },
		{ CargoLabel{'OIL_'}, MarketCargoGroup::Oil,    350'000,  cargomarket::LOCAL_WEIGHT_OTHER_PPM,    false },
		{ CargoLabel{'WOOD'}, MarketCargoGroup::Build,  350'000,  cargomarket::LOCAL_WEIGHT_OTHER_PPM,    false },
		{ CargoLabel{'RFPR'}, MarketCargoGroup::Oil,    DEFAULT_ELASTICITY_PPM, cargomarket::LOCAL_WEIGHT_OTHER_PPM, false },
		{ CargoLabel{'STEL'}, MarketCargoGroup::Build,  DEFAULT_ELASTICITY_PPM, cargomarket::LOCAL_WEIGHT_OTHER_PPM, false },
		{ CargoLabel{'GRAI'}, MarketCargoGroup::Farm,   DEFAULT_ELASTICITY_PPM, cargomarket::LOCAL_WEIGHT_OTHER_PPM, false },
		{ CargoLabel{'WHEA'}, MarketCargoGroup::Farm,   DEFAULT_ELASTICITY_PPM, cargomarket::LOCAL_WEIGHT_OTHER_PPM, false },
		{ CargoLabel{'MAIZ'}, MarketCargoGroup::Farm,   DEFAULT_ELASTICITY_PPM, cargomarket::LOCAL_WEIGHT_OTHER_PPM, false },
		{ CargoLabel{'LVST'}, MarketCargoGroup::Farm,   DEFAULT_ELASTICITY_PPM, cargomarket::LOCAL_WEIGHT_OTHER_PPM, false },
		{ CargoLabel{'FISH'}, MarketCargoGroup::Farm,   DEFAULT_ELASTICITY_PPM, cargomarket::LOCAL_WEIGHT_OTHER_PPM, false },
		/* FIRS sub-tropical (desert) cargo, firs_desert_cargo.cpp */
		{ CargoLabel{'CASS'}, MarketCargoGroup::Farm,   800'000,  cargomarket::LOCAL_WEIGHT_OTHER_PPM,    false },
		{ CargoLabel{'COFF'}, MarketCargoGroup::Farm,   800'000,  cargomarket::LOCAL_WEIGHT_OTHER_PPM,    false },
		{ CargoLabel{'NUTS'}, MarketCargoGroup::Farm,   800'000,  cargomarket::LOCAL_WEIGHT_OTHER_PPM,    false },
		{ CargoLabel{'SGCN'}, MarketCargoGroup::Farm,   800'000,  cargomarket::LOCAL_WEIGHT_OTHER_PPM,    false },
		{ CargoLabel{'SUGR'}, MarketCargoGroup::Farm,   800'000,  cargomarket::LOCAL_WEIGHT_CONSUMER_PPM, false },
		{ CargoLabel{'COTN'}, MarketCargoGroup::Farm,   800'000,  cargomarket::LOCAL_WEIGHT_OTHER_PPM,    false },
		{ CargoLabel{'CLAY'}, MarketCargoGroup::Build,  350'000,  cargomarket::LOCAL_WEIGHT_OTHER_PPM,    false },
		{ CargoLabel{'FERT'}, MarketCargoGroup::Farm,   DEFAULT_ELASTICITY_PPM, cargomarket::LOCAL_WEIGHT_OTHER_PPM, false },
		{ CargoLabel{'PETR'}, MarketCargoGroup::Oil,    350'000,  cargomarket::LOCAL_WEIGHT_OTHER_PPM,    false },
		{ CargoLabel{'DIES'}, MarketCargoGroup::Oil,    350'000,  cargomarket::LOCAL_WEIGHT_OTHER_PPM,    false },
		{ CargoLabel{'EOIL'}, MarketCargoGroup::Farm,   800'000,  cargomarket::LOCAL_WEIGHT_CONSUMER_PPM, false },
		{ CargoLabel{'FMSP'}, MarketCargoGroup::None,   DEFAULT_ELASTICITY_PPM, cargomarket::LOCAL_WEIGHT_OTHER_PPM, false },
	};
	for (const CargoRule &r : rules) {
		if (r.label == cs->label) return &r;
	}
	return nullptr;
}

/** Passengers and mail: people are not goods. */
bool IsPeopleCargo(const CargoSpec *cs)
{
	const CargoRule *r = FindCargoRule(cs);
	return r != nullptr && r->people;
}

/** How strongly the price of a cargo answers to supply, in ppm. Food reacts a lot, coal and ore little; unknown cargo 0.5.
 *  People (passengers, mail) 0: carrying more people does not lower the fare; the backlog of people waiting moves it (freight rate). */
int64_t ElasticityPpm(const CargoSpec *cs)
{
	const CargoRule *r = FindCargoRule(cs);
	return r != nullptr ? r->elasticity_ppm : DEFAULT_ELASTICITY_PPM;
}

/** Units of cargo waiting in one GoodsEntry (OpenTTD 15: the cargo list is in GetData(), which exists only when something is stored). */
uint WaitingUnits(const GoodsEntry &ge)
{
	return ge.HasData() ? ge.GetData().cargo.TotalCount() : 0;
}

/** Gold and valuables are priced by the gold market, not here. */
bool IsGoldMarketCargo(const CargoSpec *cs)
{
	return cs->label == CT_GOLD || cs->label == CargoLabel{'VALU'};
}

/** The group of cargo that the news can move (market_events.h) for a cargo type; None for the others. */
MarketCargoGroup NewsGroupOf(const CargoSpec *cs)
{
	const CargoRule *r = FindCargoRule(cs);
	return r != nullptr ? r->group : MarketCargoGroup::None;
}

/** How much a cargo follows the town that it is delivered to, in ppm: people fully, goods and food a lot, raw material a little, gold and valuables not at all. */
int64_t LocalWeightPpm(const CargoSpec *cs)
{
	if (IsGoldMarketCargo(cs)) return 0;
	const CargoRule *r = FindCargoRule(cs);
	return r != nullptr ? r->local_weight_ppm : cargomarket::LOCAL_WEIGHT_OTHER_PPM;
}

/** The money factor of the price index (cash in circulation against the start), without the inflation of the game: OpenTTD pays inflated cargo already. */
int64_t MoneyFactorPpm()
{
	const int64_t game = std::clamp<int64_t>((static_cast<int64_t>(_economy.inflation_prices) * cargomarket::PPM) >> 16, cargomarket::PPM / 2, 20 * cargomarket::PPM);
	return GetPriceIndexPpm() * cargomarket::PPM / game;
}

} // namespace

int64_t CargoMarket::GetPricePpm(CargoType cargo_type) const
{
	const size_t i = static_cast<size_t>(cargo_type);
	if (i >= this->cargo.size()) return cargomarket::PPM;
	return this->cargo[i].price_ppm;
}

int64_t CargoMarket::GetFreightPpm(CargoType cargo_type) const
{
	const size_t i = static_cast<size_t>(cargo_type);
	if (i >= this->cargo.size()) return cargomarket::PPM;
	return this->cargo[i].freight_ppm;
}

/**
 * REAL cargo news, once per game day (called at the end of CargoMarket::UpdateDaily). The story is made only when the deliveries of a group of cargo
 * (coal, farm, building material) really are far from their normal level: much less = shortage (coal shortage, harvest failure, construction boom),
 * much more = glut (coal glut, bumper harvest, construction slump). The number in the story is that real change. No dice. At most one story per day (the
 * biggest change), and the same kind is not repeated within CARGO_NEWS_REPEAT_DAYS game days. The usual push of cargo news on the price goal comes with the story.
 */
static void PublishRealCargoNews(int64_t minute)
{
	if (_game_mode != GM_NORMAL) return;
	static constexpr int64_t CARGO_NEWS_REPEAT_DAYS = 30;       ///< The same kind of cargo news is not made again within this many game days.
	static constexpr int64_t CARGO_NEWS_SEV1_PPM = 250'000;     ///< Deliveries 25% away from normal: a small story.
	static constexpr int64_t CARGO_NEWS_SEV2_PPM = 400'000;     ///< 40%: a medium story.
	static constexpr int64_t CARGO_NEWS_SEV3_PPM = 600'000;     ///< 60%: a big story.

	struct GroupInfo { MarketCargoGroup group; MarketEventKind shortage; MarketEventKind glut; };
	static const GroupInfo groups[] = {
		{ MarketCargoGroup::Coal,  MarketEventKind::CoalShortage,     MarketEventKind::CoalGlut },
		{ MarketCargoGroup::Farm,  MarketEventKind::HarvestFailure,   MarketEventKind::BumperHarvest },
		{ MarketCargoGroup::Build, MarketEventKind::ConstructionBoom, MarketEventKind::ConstructionSlump },
	};

	bool have_best = false;
	MarketEventKind best_kind = MarketEventKind::CoalShortage;
	int best_sev = 1;
	int64_t best_excess = 0;
	uint32_t best_group = 0;

	for (const GroupInfo &g : groups) {
		int64_t fast = 0, ref = 0;
		bool measured = false;
		for (const CargoSpec *cs : CargoSpec::Iterate()) {
			const size_t i = static_cast<size_t>(cs->Index());
			if (i >= _cargo_market.cargo.size() || IsGoldMarketCargo(cs) || NewsGroupOf(cs) != g.group) continue;
			const cargomarket::CargoState &st = _cargo_market.cargo[i];
			if (st.days <= cargomarket::BASELINE_DAYS) continue; // Still measuring the normal level.
			measured = true;
			fast += std::clamp<int64_t>(st.fast_x100, 0, cargomarket::LEVEL_CAP_X100);
			ref += std::clamp<int64_t>(st.ref_x100, 0, cargomarket::LEVEL_CAP_X100);
		}
		if (!measured || ref < cargomarket::REF_FLOOR_X100) continue; // No real trade yet: no news from nothing.

		const int64_t excess = cargomarket::ExcessPpm(fast, ref);
		const int64_t size = excess < 0 ? -excess : excess;
		if (size < CARGO_NEWS_SEV1_PPM) continue;
		const int sev = size >= CARGO_NEWS_SEV3_PPM ? 3 : (size >= CARGO_NEWS_SEV2_PPM ? 2 : 1);
		const MarketEventKind kind = excess < 0 ? g.shortage : g.glut;
		if (_market_events.RecentlyReportedTown(kind, MARKET_NO_TOWN, minute, CARGO_NEWS_REPEAT_DAYS)) continue;
		if (_market_events.RecentlyReportedTown(excess < 0 ? g.glut : g.shortage, MARKET_NO_TOWN, minute, MARKET_CARGO_NEWS_DAYS)) continue; // No contradicting news while the last one still counts.
		if (!have_best || size > (best_excess < 0 ? -best_excess : best_excess)) {
			have_best = true; best_kind = kind; best_sev = sev; best_excess = excess; best_group = static_cast<uint32_t>(g.group);
		}
	}
	if (!have_best) return;

	MarketEvent e = _market_events.NewEvent(best_kind, minute, best_sev, best_group);
	e.fact_ppm = best_excess; // The real change of the deliveries against the normal level, in ppm (negative: fewer).
	PublishMarketEvent(e);
}

void CargoMarket::UpdateDaily()
{
	const int64_t outside = cargomarket::OutsideFactor(_market_events.market_sentiment_ppm, MoneyFactorPpm());
	const int64_t minute = static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes());

	/* Freight rate, part 1: the backlog (cargo that waits at all the stations, per cargo) and the size of the fleet, counted once per day. */
	std::array<int64_t, NUM_CARGO> waiting{};
	for (const Station *st : Station::Iterate()) {
		for (const CargoSpec *cs : CargoSpec::Iterate()) {
			const size_t i = static_cast<size_t>(cs->Index());
			if (i >= waiting.size()) continue;
			waiting[i] += WaitingUnits(st->goods[i]);
		}
	}
	cargomarket::UpdateFleet(this->fleet, CountAllVehicles());
	const int64_t fleet_growth = cargomarket::FleetGrowthPpm(this->fleet);

	for (const CargoSpec *cs : CargoSpec::Iterate()) {
		const size_t i = static_cast<size_t>(cs->Index());
		if (i >= this->cargo.size()) continue;
		if (IsGoldMarketCargo(cs)) { this->cargo[i] = {}; continue; }
		/* News about one kind of cargo (a coal shortage, a bad harvest ...) moves the price goal of that cargo on top of the outside factor. */
		const int64_t news_push = _market_events.CargoPushPpm(NewsGroupOf(cs), minute);
		const int64_t outside_cargo = std::clamp<int64_t>(outside * (cargomarket::PPM + news_push) / cargomarket::PPM, cargomarket::PPM / 2, 2 * cargomarket::PPM);
		cargomarket::UpdateDay(this->cargo[i], ElasticityPpm(cs), outside_cargo);
		/* Freight rate, part 2: after the day of the price, because it uses the updated normal level and the day count. */
		this->cargo[i].waiting_today = waiting[i];
		cargomarket::UpdateFreight(this->cargo[i], fleet_growth, IsPeopleCargo(cs) ? cargomarket::BACKLOG_WEIGHT_PEOPLE_PPM : cargomarket::BACKLOG_WEIGHT_PPM);
	}

	PublishRealCargoNews(minute);
}

void CargoMarketOnDelivery(CargoType cargo_type, uint count)
{
	const size_t i = static_cast<size_t>(cargo_type);
	if (count == 0 || i >= _cargo_market.cargo.size()) return;
	_cargo_market.cargo[i].delivered_today += count;
}

Money CargoMarketAdjustIncome(CargoType cargo_type, Money income)
{
	/* The price (supply and demand) and the freight rate (the service) are one factor: 35% .. 300% of the plain pay together.
	 * The fuel surcharge (60% of the part of the fuel index above 1.0, at most +24%) comes on top, for every cargo except gold and valuables. */
	int64_t factor = _cargo_market.GetPricePpm(cargo_type) * _cargo_market.GetFreightPpm(cargo_type) / cargomarket::PPM;
	if (!CargoMarketIsGoldCargo(cargo_type)) {
		factor = factor * cargomarket::FuelIncomeFactorPpm(FuelIndexWithOilPpm(_market_events.FuelIndexPpm(static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes())))) / cargomarket::PPM;
	}
	return static_cast<Money>(cargomarket::ApplyPrice(static_cast<int64_t>(income), factor));
}

int64_t CargoMarketLocalFactorPpm(CargoType cargo_type, TownID town)
{
	if (!IsValidCargoType(cargo_type) || !Town::IsValidID(town)) return cargomarket::PPM;
	const int64_t weight = LocalWeightPpm(CargoSpec::Get(cargo_type));
	if (weight == 0) return cargomarket::PPM;
	const Town *t = Town::Get(town);
	const int64_t boost = RetailMarketManager::Get().GetTownDemandBoostPpm(town);
	const int64_t prosperity = retail::TownProsperityPpm(t->cache.population);
	return cargomarket::LocalFactor(weight, boost, prosperity);
}

Money CargoMarketAdjustLocalIncome(CargoType cargo_type, StationID dest, Money income)
{
	if (!Station::IsValidID(dest)) return income;
	return static_cast<Money>(cargomarket::ApplyPrice(static_cast<int64_t>(income), CargoMarketLocalFactorPpm(cargo_type, Station::Get(dest)->town->index)));
}

bool CargoMarketIsPeopleCargo(CargoType cargo_type)
{
	return IsValidCargoType(cargo_type) && IsPeopleCargo(CargoSpec::Get(cargo_type));
}

bool CargoMarketIsGoldCargo(CargoType cargo_type)
{
	return IsValidCargoType(cargo_type) && IsGoldMarketCargo(CargoSpec::Get(cargo_type));
}

int64_t CargoMarketNewsPushPpm(CargoType cargo_type)
{
	if (!IsValidCargoType(cargo_type)) return 0;
	return _market_events.CargoPushPpm(NewsGroupOf(CargoSpec::Get(cargo_type)), static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes()));
}

static const GameTimeSubscription _cargo_market_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &) { _cargo_market.UpdateDaily(); }, 25);
