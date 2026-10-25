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
#include "cargotype.h"
#include "economy_func.h"
#include "game_time/game_time_manager.h"
#include "safeguards.h"

CargoMarket _cargo_market;

namespace {

/** How strongly the price of a cargo answers to supply, in ppm. Passengers and food react a lot, coal and ore little; unknown cargo 0.5. */
int64_t ElasticityPpm(const CargoSpec *cs)
{
	const CargoLabel l = cs->label;
	if (l == CargoLabel{'PASS'} || l == CargoLabel{'FOOD'} || l == CargoLabel{'FRUT'} || l == CargoLabel{'MILK'}) return 800'000;
	if (l == CargoLabel{'MAIL'} || l == CargoLabel{'GOOD'}) return 550'000;
	if (l == CargoLabel{'COAL'} || l == CargoLabel{'IORE'} || l == CargoLabel{'CORE'} || l == CargoLabel{'OIL_'} || l == CargoLabel{'WOOD'}) return 350'000;
	return 500'000;
}

/** Gold and valuables are priced by the gold market, not here. */
bool IsGoldMarketCargo(const CargoSpec *cs)
{
	return cs->label == CT_GOLD || cs->label == CargoLabel{'VALU'};
}

/** The group of cargo that the news can move (market_events.h) for a cargo type; None for the others. */
MarketCargoGroup NewsGroupOf(const CargoSpec *cs)
{
	const CargoLabel l = cs->label;
	if (l == CargoLabel{'PASS'} || l == CargoLabel{'MAIL'}) return MarketCargoGroup::People;
	if (l == CargoLabel{'OIL_'} || l == CargoLabel{'RFPR'}) return MarketCargoGroup::Oil;
	if (l == CargoLabel{'COAL'}) return MarketCargoGroup::Coal;
	if (l == CargoLabel{'GRAI'} || l == CargoLabel{'WHEA'} || l == CargoLabel{'MAIZ'} || l == CargoLabel{'LVST'} || l == CargoLabel{'MILK'} ||
			l == CargoLabel{'FRUT'} || l == CargoLabel{'FOOD'} || l == CargoLabel{'FISH'}) return MarketCargoGroup::Farm;
	if (l == CargoLabel{'WOOD'} || l == CargoLabel{'STEL'} || l == CargoLabel{'IORE'} || l == CargoLabel{'CORE'}) return MarketCargoGroup::Build;
	return MarketCargoGroup::None;
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

void CargoMarket::UpdateDaily()
{
	const int64_t outside = cargomarket::OutsideFactor(_market_events.market_sentiment_ppm, MoneyFactorPpm());
	const int64_t minute = static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes());
	for (const CargoSpec *cs : CargoSpec::Iterate()) {
		const size_t i = static_cast<size_t>(cs->Index());
		if (i >= this->cargo.size()) continue;
		if (IsGoldMarketCargo(cs)) { this->cargo[i] = {}; continue; }
		/* News about one kind of cargo (a coal shortage, a bad harvest ...) moves the price goal of that cargo on top of the outside factor. */
		const int64_t news_push = _market_events.CargoPushPpm(NewsGroupOf(cs), minute);
		const int64_t outside_cargo = std::clamp<int64_t>(outside * (cargomarket::PPM + news_push) / cargomarket::PPM, cargomarket::PPM / 2, 2 * cargomarket::PPM);
		cargomarket::UpdateDay(this->cargo[i], ElasticityPpm(cs), outside_cargo);
	}
}

void CargoMarketOnDelivery(CargoType cargo_type, uint count)
{
	const size_t i = static_cast<size_t>(cargo_type);
	if (count == 0 || i >= _cargo_market.cargo.size()) return;
	_cargo_market.cargo[i].delivered_today += count;
}

Money CargoMarketAdjustIncome(CargoType cargo_type, Money income)
{
	return static_cast<Money>(cargomarket::ApplyPrice(static_cast<int64_t>(income), _cargo_market.GetPricePpm(cargo_type)));
}

static const GameTimeSubscription _cargo_market_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &) { _cargo_market.UpdateDaily(); }, 25);
