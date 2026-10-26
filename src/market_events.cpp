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
#include "town_news.h"
#include "stock_base.h"
#include "gold_reserve.h"
#include "central_bank.h"
#include "company_base.h"
#include "town.h"
#include "valuables.h"
#include "news_func.h"
#include "openttd.h"
#include "settings_type.h"
#include "strings_func.h"
#include "window_func.h"
#include "window_type.h"
#include "game_time/game_time_manager.h"

#include "table/strings.h"

#include "safeguards.h"

MarketEvents _market_events;

static_assert(STOCK_SYMBOLS <= MARKET_SENTIMENT_SLOTS, "MARKET_SENTIMENT_SLOTS must have a slot for every symbol");
static_assert(MARKET_GOLD_STOCK_START_OZ == GoldReserveManager::MARKET_STOCK_START, "market_events.h must know the start stock of the gold market");
static_assert(MARKET_BANK_SELL_LEVEL_BP == BANK_GOLD_SELL_ABOVE_BP, "market_events.h must know the reserve level at which the central bank sells gold");

int64_t MarketEventSentimentPpm(size_t symbol)
{
	if (symbol >= MAX_COMPANIES) return 0; // The valuables (gold, silver ...) follow the gold price, which the events move through the demand pressure.
	return _market_events.SentimentPpm(symbol);
}

std::string MarketEventSubjectName(const MarketEvent &e)
{
	if (MarketKindHasTownField(e.kind)) {
		if (e.town != MARKET_NO_TOWN && Town::IsValidID(TownID{e.town})) return GetString(STR_TOWN_NAME, TownID{e.town});
		return std::string("เมืองที่หายไปแล้ว"); // The town is gone (a story in the log may be older than the town).
	}
	if (e.kind == MarketEventKind::TownBoom) {
		if (e.fact_ppm >= 0 && e.fact_ppm < 0xFFFF && Town::IsValidID(TownID{static_cast<uint16_t>(e.fact_ppm)})) return GetString(STR_TOWN_NAME, TownID{static_cast<uint16_t>(e.fact_ppm)});
		return std::string();
	}
	return e.target != MARKET_NO_TARGET ? GetStockSymbolName(e.target) : std::string();
}

/** Number of towns that exist (a town boom needs one). */
static size_t CountTowns()
{
	size_t n = 0;
	for (const Town *t : Town::Iterate()) { (void)t; n++; }
	return n;
}

/** A town boom is drawn with the ORDINAL of a town (see MarketEvents::Generate); turn it into the index of that town. -1 when there is no such town. */
static void ResolveTownBoom(MarketEvent &e)
{
	if (e.kind != MarketEventKind::TownBoom) return;
	const int64_t ordinal = e.fact_ppm;
	int64_t i = 0;
	for (const Town *t : Town::Iterate()) {
		if (i++ == ordinal) { e.fact_ppm = t->index.base(); return; }
	}
	e.fact_ppm = -1;
}

/** Put an event into the game: the sentiment, the gold pressure, the diamond stock and the news item. */
void PublishMarketEvent(const MarketEvent &e)
{
	_market_events.Apply(e);

	/* Gold: the same way as the Transport Payrolls report. */
	if (e.gold_ppm != 0 && _gold_reserve.GetPriceCents() > 0) {
		_gold_reserve.pressure_ppm = std::clamp<int64_t>(_gold_reserve.pressure_ppm + e.gold_ppm,
				-GoldReserveManager::PRESSURE_MAX_PPM, GoldReserveManager::PRESSURE_MAX_PPM);
	}

	/* Real gold news (the bank really sold gold, the mines really delivered gold): the price does not only glide towards the new goal, it jumps at once. */
	if (MARKET_GOLD_REAL_SHOCK && MarketIsRealGoldNews(e) && e.gold_ppm < 0) _gold_reserve.ApplyShock(e.gold_ppm);

	/* Diamonds: news about a new mine or a rush of buyers changes the stock of the diamond market once (the price follows the stock, see DiamondScarcityPpm).
	 * The cargo prices, the growth of a town and the loan rates read the news from the log when they need it (MarketEvents::CargoPushPpm, TownBoostPpm, CreditSpreadBp). */
	const int64_t diamond_ppm = MarketDiamondStockPpm(e.kind, e.severity);
	if (diamond_ppm != 0) {
		const int64_t delta = ValuablesManager::DIAMOND_STOCK_START / MARKET_PPM * diamond_ppm + ValuablesManager::DIAMOND_STOCK_START % MARKET_PPM * diamond_ppm / MARKET_PPM;
		_valuables.diamond_stock_carats = std::clamp<int64_t>(_valuables.diamond_stock_carats + delta, 0, ValuablesManager::DIAMOND_STOCK_CAP);
	}

	/* A big crisis in the news can make the central bank meet at once (see MonetaryPolicy::CheckUnscheduled). Same event on every client, so the same decision. */
	const int64_t news_day = e.minute / GameTimeManager::MINUTES_PER_DAY;
	if ((e.kind == MarketEventKind::BankScare && e.severity >= 2) || (e.kind == MarketEventKind::Recession && e.severity >= 3)) {
		_central_bank.policy.RequestNewsMeeting(PolicyNewsTrigger::Crisis, news_day);
	} else if (e.kind == MarketEventKind::InflationScare && e.severity >= 3) {
		_central_bank.policy.RequestNewsMeeting(PolicyNewsTrigger::InflationShock, news_day);
	}

	/* Every event is a news item. */
	const std::string company = MarketEventSubjectName(e);
	const MarketStory story = ComposeMarketStory(e, company);
	AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, "[" + story.impact + "] " + story.headline), NewsType::General, NewsStyle::Small, {});
}

/* ---- Real gold news -------------------------------------------------------------------------------------------------------------------------- */

void PublishBankGoldSaleNews(int64_t ounces_sold, int64_t reserve_before_oz, int64_t backing_bp, bool force)
{
	if (!MARKET_GOLD_REAL_NEWS || ounces_sold < MARKET_BANK_NEWS_MIN_OZ || reserve_before_oz <= 0) return;
	const int64_t share = std::min<int64_t>(1000, ounces_sold * 1000 / reserve_before_oz); // How much of its own gold the bank sold: this is the size of the news.
	if (share < MARKET_BANK_NEWS_MIN_SHARE_PERMILLE) return;
	const int64_t minute = static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes());
	if (!force && _market_events.RecentlyReportedGold(MarketEventKind::CentralBanksSellGold, minute, MARKET_GOLD_NEWS_GAP_DAYS)) return;

	const int64_t push = MarketGoldDropPpm(share, MARKET_BANK_NEWS_MIN_SHARE_PERMILLE, 1000);
	MarketEvent e = _market_events.NewEvent(MarketEventKind::CentralBanksSellGold, minute, MarketGoldSeverity(push));
	e.gold_ppm = push;
	e.fact_ppm = ounces_sold;
	e.fact2 = MarketBankFact2(backing_bp, share);
	PublishMarketEvent(e);
}

/** Once per game day: the ounces that the mines put into the market stock become news when they are many enough; without news they fade. */
static void RealGoldNewsDaily(const GameTimeSnapshot &now)
{
	if (!MARKET_GOLD_REAL_NEWS || _game_mode != GM_NORMAL) return;
	if (_gold_reserve.GetPriceCents() <= 0) return;
	int64_t &ounces = _gold_reserve.mine_news_oz;
	const int64_t minute = static_cast<int64_t>(now.total_minutes);
	if (ounces >= MARKET_MINE_NEWS_MIN_OZ && !_market_events.RecentlyReportedGold(MarketEventKind::GoldMineFound, minute, MARKET_GOLD_NEWS_GAP_DAYS)) {
		const int64_t push = MarketGoldDropPpm(ounces, MARKET_MINE_NEWS_MIN_OZ, MARKET_MINE_NEWS_FULL_OZ);
		MarketEvent e = _market_events.NewEvent(MarketEventKind::GoldMineFound, minute, MarketGoldSeverity(push));
		e.gold_ppm = push;
		e.fact_ppm = ounces;
		e.fact2 = std::clamp<int64_t>(_gold_reserve.market_stock_oz, 0, 2'000'000'000);
		ounces = 0;
		PublishMarketEvent(e);
		return;
	}
	ounces -= ounces * MARKET_MINE_NEWS_DECAY_PERCENT / 100;
	if (ounces < 100) ounces = 0;
}

/** Once per game day: an oil stock far above or far below its start level becomes news (the price follows the stock and the news, see MarketValuablePushPpm). */
static void RealOilNewsDaily(const GameTimeSnapshot &now)
{
	if (_game_mode != GM_NORMAL || _gold_reserve.GetPriceCents() <= 0) return; // The valuables have no price before the market has started.
	bool glut = false;
	int severity = 1;
	const int64_t stock = _valuables.oil_stock_barrels;
	if (!oilmath::NewsFromStock(oilmath::StockPermille(stock), glut, severity)) return;
	const MarketEventKind kind = glut ? MarketEventKind::OilWellsFound : MarketEventKind::OilSupplyShort;
	const int64_t minute = static_cast<int64_t>(now.total_minutes);
	if (_market_events.RecentlyReportedGold(kind, minute, oilmath::NEWS_GAP_DAYS)) return; // Reads the log: fact_ppm > 0 (a real story).
	MarketEvent e = _market_events.NewEvent(kind, minute, severity);
	e.fact_ppm = std::max<int64_t>(1, stock * MARKET_PPM / oilmath::STOCK_START);
	e.fact2 = std::clamp<int64_t>(stock, 0, 2'000'000'000);
	PublishMarketEvent(e);
}

static const GameTimeSubscription _real_oil_news_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &now) { RealOilNewsDaily(now); }, 31);

static const GameTimeSubscription _real_gold_news_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &now) { RealGoldNewsDaily(now); }, 30);

/**
 * Test tool (stock_event 7 or 26): make the REAL gold news now from the real state of the gold market, with its real effects.
 * 7 (new mine): the ounces that the mines delivered so far, but at least MARKET_MINE_NEWS_MIN_OZ so that there is a story.
 * 26 (bank sale): the bank really sells half of its gold (gold reserve, market stock and demand pressure change like in CentralBank::TradeGold; no money moves).
 * @return false when it cannot be made (the gold market has not started, or the bank holds too little gold).
 */
static bool RealGoldNewsForce(MarketEventKind kind, MarketEvent &out)
{
	if (!MARKET_GOLD_REAL_NEWS || _gold_reserve.GetPriceCents() <= 0) return false;
	const int64_t minute = static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes());
	if (kind == MarketEventKind::GoldMineFound) {
		const int64_t ounces = std::max<int64_t>(_gold_reserve.mine_news_oz, MARKET_MINE_NEWS_MIN_OZ);
		const int64_t push = MarketGoldDropPpm(ounces, MARKET_MINE_NEWS_MIN_OZ, MARKET_MINE_NEWS_FULL_OZ);
		MarketEvent e = _market_events.NewEvent(kind, minute, MarketGoldSeverity(push));
		e.gold_ppm = push;
		e.fact_ppm = ounces;
		e.fact2 = std::clamp<int64_t>(_gold_reserve.market_stock_oz, 0, 2'000'000'000);
		_gold_reserve.mine_news_oz = 0;
		PublishMarketEvent(e);
		out = e;
		return true;
	}
	/* The bank sale. */
	const int64_t before = _gold_reserve.bank_reserve_oz;
	const int64_t ounces = before / 2;
	if (ounces < MARKET_BANK_NEWS_MIN_OZ) return false;
	const int64_t backing = _gold_reserve.GetBackingBp();
	_gold_reserve.bank_reserve_oz -= ounces;
	_gold_reserve.market_stock_oz += ounces;
	_gold_reserve.pressure_ppm = std::max<int64_t>(-GoldReserveManager::PRESSURE_MAX_PPM,
			_gold_reserve.pressure_ppm - ounces * GoldReserveManager::PPM / GoldReserveManager::DEPTH_OUNCES);
	const uint32_t first_id = _market_events.next_id;
	PublishBankGoldSaleNews(ounces, before, backing, true);
	if (_market_events.next_id == first_id) return false; // Nothing was made (cannot happen with 50% of the gold).
	out = _market_events.log.front();
	return true;
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
	/* Town news and weather are made from the real state of a town (town_news.cpp), also by the test tool. */
	if (kind >= 0 && static_cast<size_t>(kind) < MARKET_EVENT_KIND_COUNT && MarketKindHasTownField(static_cast<MarketEventKind>(kind))) {
		MarketEvent t;
		if (TownNewsForce(static_cast<MarketEventKind>(kind), t)) return t;
		kind = -1; // No town with a treasury: make an ordinary event instead.
	}
	/* New mine and bank sale are made from the real gold market (also by the test tool). */
	if (kind == static_cast<int>(MarketEventKind::GoldMineFound) || kind == static_cast<int>(MarketEventKind::CentralBanksSellGold)) {
		MarketEvent g;
		if (RealGoldNewsForce(static_cast<MarketEventKind>(kind), g)) return g;
		kind = -1; // The gold market has not started, or the bank holds no gold: make an ordinary event instead.
	}
	MarketEvent e = ev.Generate(static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes()), CollectCompanyFacts(), kind, CountTowns());
	ResolveTownBoom(e);
	PublishMarketEvent(e);
	return e;
}

/**
 * Every game hour: the sentiment fades, then
 *  - MARKET_ONLY_WAR_IS_RANDOM: the dice of war and peace (the only chance news), and the biggest TRUE company story, if there is one (no dice).
 *    Town news and weather come once per game day from town_news.cpp.
 *  - otherwise: the old dice for every kind.
 */
static const GameTimeSubscription _market_events_hour(GameTimeEvent::Hour, [](const GameTimeSnapshot &now)
{
	MarketEvents &ev = _market_events;
	ev.Seed(static_cast<uint64_t>(_settings_game.game_creation.generation_seed));
	ev.DecayHour();

	const int64_t minute = static_cast<int64_t>(now.total_minutes);
	if (MARKET_ONLY_WAR_IS_RANDOM) {
		if (const std::optional<MarketEvent> war = ev.GenerateWar(minute); war.has_value()) PublishMarketEvent(*war);
		if (const std::optional<MarketEvent> news = ev.GenerateCompanyNews(minute, CollectCompanyFacts()); news.has_value()) PublishMarketEvent(*news);
	} else if (ev.ShouldFire(minute)) {
		MarketEvent e = ev.Generate(minute, CollectCompanyFacts(), -1, CountTowns());
		ResolveTownBoom(e);
		PublishMarketEvent(e);
	}
});
