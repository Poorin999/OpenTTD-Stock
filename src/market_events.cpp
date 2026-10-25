/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file market_events.cpp The random events: the dice once per game hour, the effect on gold and shares, the news item. See market_events.h. */

#include "stdafx.h"
#include "market_events.h"
#include "market_news_ai.h"
#include "stock_base.h"
#include "gold_reserve.h"
#include "central_bank.h"
#include "company_base.h"
#include "news_func.h"
#include "settings_type.h"
#include "strings_func.h"
#include "window_func.h"
#include "window_type.h"
#include "game_time/game_time_manager.h"

#include "table/strings.h"

#include "safeguards.h"

MarketEvents _market_events;

static_assert(STOCK_SYMBOLS <= MARKET_SENTIMENT_SLOTS, "MARKET_SENTIMENT_SLOTS must have a slot for every symbol");

int64_t MarketEventSentimentPpm(size_t symbol)
{
	if (symbol >= MAX_COMPANIES) return 0; // The valuables (gold, silver ...) follow the gold price, which the events move through the demand pressure.
	return _market_events.SentimentPpm(symbol);
}

/** Put an event into the game: the sentiment, the gold pressure and the news item. */
static void PublishMarketEvent(const MarketEvent &e)
{
	_market_events.Apply(e);

	/* Gold: the same way as the Transport Payrolls report. */
	if (e.gold_ppm != 0 && _gold_reserve.GetPriceCents() > 0) {
		_gold_reserve.pressure_ppm = std::clamp<int64_t>(_gold_reserve.pressure_ppm + e.gold_ppm,
				-GoldReserveManager::PRESSURE_MAX_PPM, GoldReserveManager::PRESSURE_MAX_PPM);
	}

	/* Every event is a news item. */
	const std::string company = e.target != MARKET_NO_TARGET ? GetStockSymbolName(e.target) : std::string();
	const MarketStory story = ComposeMarketStory(e, company);
	AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, "[" + story.impact + "] " + story.headline), NewsType::General, NewsStyle::Small, {});
}

/** The REAL figures of every company (old_economy[0] = last completed quarter, [1] = the one before; an empty quarter has not happened yet). */
static std::vector<CompanyFacts> CollectCompanyFacts()
{
	std::vector<CompanyFacts> facts;
	for (const Company *c : Company::Iterate()) {
		const CompanyEconomyEntry &q0 = c->old_economy[0];
		const CompanyEconomyEntry &q1 = c->old_economy[1];
		const auto happened = [](const CompanyEconomyEntry &e) { return e.income != 0 || e.expenses != 0; };
		CompanyFacts f;
		f.company = static_cast<uint8_t>(c->index.base());
		f.quarters = !happened(q0) ? 0 : (happened(q1) ? 2 : 1);
		if (f.quarters >= 1) {
			f.income_last = static_cast<int64_t>(q0.income);
			f.net_last = static_cast<int64_t>(q0.income) + static_cast<int64_t>(q0.expenses);
			f.value_last = static_cast<int64_t>(q0.company_value);
		}
		if (f.quarters >= 2) {
			f.income_prev = static_cast<int64_t>(q1.income);
			f.net_prev = static_cast<int64_t>(q1.income) + static_cast<int64_t>(q1.expenses);
			f.value_prev = static_cast<int64_t>(q1.company_value);
		}
		f.debt = GetCentralBankDebt(c->index);
		f.in_default = _central_bank.accounts[c->index.base()].in_default;
		f.months_bankrupt = static_cast<int>(c->months_of_bankruptcy);
		facts.push_back(f);
	}
	return facts;
}

MarketEvent FireMarketEventNow(int kind)
{
	MarketEvents &ev = _market_events;
	ev.Seed(static_cast<uint64_t>(_settings_game.game_creation.generation_seed));
	const MarketEvent e = ev.Generate(static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes()), CollectCompanyFacts(), kind);
	PublishMarketEvent(e);
	return e;
}

/** Every game hour: the sentiment fades, then the dice. */
static const GameTimeSubscription _market_events_hour(GameTimeEvent::Hour, [](const GameTimeSnapshot &now)
{
	MarketEvents &ev = _market_events;
	ev.Seed(static_cast<uint64_t>(_settings_game.game_creation.generation_seed));
	ev.DecayHour();

	const int64_t minute = static_cast<int64_t>(now.total_minutes);
	if (!ev.ShouldFire(minute)) return;
	PublishMarketEvent(ev.Generate(minute, CollectCompanyFacts()));
});
