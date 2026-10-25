/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file market_events.h Random events of the world (boom, war, a new gold mine, an accident of a company ...). Every event is a news item. It moves the
 * gold price and the share prices for a while.
 *
 * Rules (integer only, the same on every client):
 *  - Once per game hour (60 real seconds at normal speed) there is a chance of MARKET_EVENT_CHANCE_PERMILLE that an event happens (never in the first
 *    MARKET_EVENT_START_DELAY_HOURS hours, and never closer than MARKET_EVENT_MIN_GAP_HOURS hours to the last one).
 *  - The kind is drawn by weight from #MarketEventInfo. The severity is 1 (60%), 2 (30%) or 3 (10%) and makes the effect 1.0, 1.7 or 2.5 times bigger
 *    (world, gold and sector events: pure chance).
 *  - COMPANY events are NOT random. A company event is only made when the real figures of that company say so (see #CompanyFacts and
 *    #CompanyEventFact): revenue up / down, profit up / down, a loss, company value up / down, heavy debt or default, bankruptcy. The size of the news
 *    comes from the size of the real change, the number in the story is the real number, and the same company does not get the same kind of news
 *    again within MARKET_COMPANY_REPEAT_DAYS game days.
 *  - Gold: the effect is added to the demand pressure of the gold market (like the Transport Payrolls report). Silver, platinum and diamonds follow gold.
 *  - Shares: the effect is added to a "sentiment" in ppm that moves the goal price of the shares. A world event moves the sentiment of the whole
 *    market, a company event only the sentiment of that company. Every game hour the sentiment fades by one MARKET_SENTIMENT_DECAY_DIV-th.
 *  - The random numbers come from an own generator that is saved in the savegame, so a load goes on with the same row of events.
 * This file needs only the standard library (so the logic can be tested outside of the game, see market_events_test.cpp).
 */

#ifndef MARKET_EVENTS_H
#define MARKET_EVENTS_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <vector>

static constexpr int MARKET_EVENT_CHANCE_PERMILLE = 130;      ///< Chance per game hour that an event happens (13%: about one event every 8 real minutes).
static constexpr int MARKET_EVENT_MIN_GAP_HOURS = 2;          ///< At least this many game hours between two events.
static constexpr int MARKET_EVENT_START_DELAY_HOURS = 3;      ///< No event in the first game hours of a game.
static constexpr int64_t MARKET_EVENT_GOLD_MAX_PPM = 80'000;  ///< One event moves the gold demand by 8% at most.
static constexpr int64_t MARKET_EVENT_SHARE_MAX_PPM = 100'000;///< One event moves the sentiment by 10% at most.
static constexpr int64_t MARKET_SENTIMENT_MAX_PPM = 150'000;  ///< The sentiment of a share (market plus company) is never more than +-15%.
static constexpr int MARKET_SENTIMENT_DECAY_DIV = 10;         ///< Every game hour the sentiment fades by one tenth (half life about 6.6 game hours).
static constexpr size_t MARKET_EVENT_KEEP = 120;              ///< Events that are kept and saved (the company news guard needs this much history).
static constexpr int64_t MARKET_COMPANY_REPEAT_DAYS = 30;     ///< The same kind of company news about the same company is not made again within this many game days.
static constexpr int64_t MARKET_FACT_MIN_BASE = 2'000;        ///< A change is only reported when the old figure was at least this many money units.
static constexpr int64_t MARKET_FACT_CHANGE_MIN_PPM = 100'000;///< Revenue and company value: a change of 10% or more is news.
static constexpr int64_t MARKET_FACT_PROFIT_MIN_PPM = 150'000;///< Profit: a change of 15% or more is news.
static constexpr int64_t MARKET_FACT_DEBT_MIN_PPM = 500'000;  ///< Debt at the central bank of 50% of the company value or more is news.
static constexpr int64_t MARKET_FACT_CAP_PPM = 9'990'000;     ///< A percentage in a story never says more than 999%.
static constexpr size_t MARKET_SENTIMENT_SLOTS = 32;          ///< Room for the sentiment of the symbols (companies come first).
static constexpr uint8_t MARKET_NO_TARGET = 255;              ///< Target of a world event.

/** The kinds of events. The numbers are saved in savegames: add new kinds at the END, before Count. */
enum class MarketEventKind : uint8_t {
	EconomicBoom,        ///< World: shares up, gold down.
	Recession,           ///< World: shares down, gold up.
	InflationScare,      ///< World: shares down, gold up.
	RateCutHope,         ///< World: shares up, gold up.
	WarTension,          ///< World: shares down, gold way up.
	PeaceDeal,           ///< World: shares up, gold down.
	BankScare,           ///< World: shares way down, gold up.
	GoldMineFound,       ///< Gold: gold down.
	MineStrike,          ///< Gold: gold up.
	CentralBanksBuyGold, ///< Gold: gold up.
	JewelleryDemand,     ///< Gold: gold up.
	FuelSpike,           ///< Sector: shares down, gold a little up.
	FuelDrop,            ///< Sector: shares up.
	TechBreakthrough,    ///< Sector: shares up.
	TourismBoom,         ///< Sector: shares up.
	StormDisruption,     ///< Sector: shares down.
	CompanyRevenueUp,    ///< One company, REAL: the revenue of its last quarter grew.
	CompanyProfitDown,   ///< One company, REAL: the profit of its last quarter fell but is still a profit.
	CompanyDistress,     ///< One company, REAL: heavy debt at the central bank, or in default.
	CompanyProfitUp,     ///< One company, REAL: the profit grew, or turned from a loss into a profit.
	CompanyRevenueDown,  ///< One company, REAL: the revenue of its last quarter fell.
	CompanyLoss,         ///< One company, REAL: its last quarter ended with a loss.
	CompanyValueUp,      ///< One company, REAL: the company value grew from one quarter to the next.
	CompanyValueDown,    ///< One company, REAL: the company value fell.
	CompanyBankruptcy,   ///< One company, REAL: the company is in bankruptcy.
	RateHike,            ///< World: shares down, gold down (money gets dearer).
	CentralBanksSellGold,///< Gold: gold down.
	JewelleryDemandDrop, ///< Gold: gold down (after the festival).
	ProfitTaking,        ///< Gold: gold down (investors sell after a rise).
	Count,
};

inline constexpr size_t MARKET_EVENT_KIND_COUNT = static_cast<size_t>(MarketEventKind::Count);

/** How a kind of event works. Sizes are for severity 1, in parts per million. */
struct MarketEventDef {
	int8_t gold_dir;     ///< +1 gold up, -1 gold down, 0 no effect on gold.
	int32_t gold_min;    ///< Smallest push on gold.
	int32_t gold_max;    ///< Biggest push on gold.
	int8_t share_dir;    ///< +1 shares up, -1 shares down, 0 no effect on shares.
	int32_t share_min;   ///< Smallest push on the sentiment.
	int32_t share_max;   ///< Biggest push on the sentiment.
	uint16_t weight;     ///< Relative chance.
	bool company;        ///< True: the event is about one company (a target is drawn).
};

/** The table of the kinds, in the order of #MarketEventKind. */
inline const MarketEventDef &MarketEventInfo(MarketEventKind kind)
{
	static constexpr MarketEventDef table[MARKET_EVENT_KIND_COUNT] = {
		/* gold_dir  gold_min gold_max  share_dir  share_min share_max  weight company */
		{ -1,  6'000, 15'000,  +1, 15'000, 35'000, 10, false }, // EconomicBoom
		{ +1,  6'000, 15'000,  -1, 15'000, 40'000,  8, false }, // Recession
		{ +1, 10'000, 22'000,  -1,  8'000, 20'000,  8, false }, // InflationScare
		{ +1,  5'000, 12'000,  +1, 10'000, 25'000,  6, false }, // RateCutHope
		{ +1, 15'000, 35'000,  -1, 12'000, 30'000,  5, false }, // WarTension
		{ -1, 10'000, 25'000,  +1, 12'000, 28'000,  5, false }, // PeaceDeal
		{ +1, 15'000, 35'000,  -1, 25'000, 60'000,  3, false }, // BankScare
		{ -1, 15'000, 35'000,   0,      0,      0,  5, false }, // GoldMineFound
		{ +1, 10'000, 25'000,   0,      0,      0,  5, false }, // MineStrike
		{ +1, 15'000, 30'000,   0,      0,      0,  4, false }, // CentralBanksBuyGold
		{ +1,  8'000, 18'000,   0,      0,      0,  5, false }, // JewelleryDemand
		{ +1,  3'000,  8'000,  -1, 10'000, 25'000,  6, false }, // FuelSpike
		{  0,      0,      0,  +1, 10'000, 25'000,  6, false }, // FuelDrop
		{  0,      0,      0,  +1, 12'000, 30'000,  5, false }, // TechBreakthrough
		{  0,      0,      0,  +1,  8'000, 20'000,  5, false }, // TourismBoom
		{  0,      0,      0,  -1,  8'000, 22'000,  5, false }, // StormDisruption
		{  0,      0,      0,  +1, 30'000, 70'000,  6, true  }, // CompanyRevenueUp
		{  0,      0,      0,  -1, 25'000, 60'000,  5, true  }, // CompanyProfitDown
		{  0,      0,      0,  -1, 40'000, 90'000,  4, true  }, // CompanyDistress
		{  0,      0,      0,  +1, 20'000, 50'000,  6, true  }, // CompanyProfitUp
		{  0,      0,      0,  -1, 25'000, 60'000,  4, true  }, // CompanyRevenueDown
		{  0,      0,      0,  -1, 40'000, 90'000,  4, true  }, // CompanyLoss
		{  0,      0,      0,  +1, 20'000, 50'000,  4, true  }, // CompanyValueUp
		{  0,      0,      0,  -1, 20'000, 50'000,  4, true  }, // CompanyValueDown
		{  0,      0,      0,  -1, 60'000,100'000,  3, true  }, // CompanyBankruptcy
		{ -1,  6'000, 15'000,  -1, 10'000, 28'000,  7, false }, // RateHike
		{ -1, 12'000, 28'000,   0,      0,      0,  5, false }, // CentralBanksSellGold
		{ -1,  6'000, 15'000,   0,      0,      0,  6, false }, // JewelleryDemandDrop
		{ -1,  8'000, 20'000,   0,      0,      0, 10, false }, // ProfitTaking
	};
	const size_t i = static_cast<size_t>(kind);
	return table[i < MARKET_EVENT_KIND_COUNT ? i : 0];
}

/** Effect of the severity (1, 2, 3) in percent. */
inline int MarketSeverityPercent(int severity)
{
	return severity >= 3 ? 250 : (severity == 2 ? 170 : 100);
}

/** One event. The text of the story is made from these numbers when it is shown (see market_news_ai.h), so only the numbers are saved. */
struct MarketEvent {
	uint32_t id = 0;                     ///< Number of the event, counted from 1.
	MarketEventKind kind = MarketEventKind::EconomicBoom;
	uint8_t variant = 0;                 ///< Chooses the wording (headline, body, reporter) of the story.
	uint8_t severity = 1;                ///< 1 small, 2 medium, 3 big.
	uint8_t target = MARKET_NO_TARGET;   ///< The company of a company event, MARKET_NO_TARGET for a world event.
	int64_t minute = 0;                  ///< Game minute (since the start of the game) at which it happened.
	int64_t gold_ppm = 0;                ///< Push on the gold demand (negative: down).
	int64_t share_ppm = 0;               ///< Push on the sentiment of the shares (negative: down).
	int64_t fact_ppm = 0;                ///< Company events: the REAL number of the story (a change or a ratio in ppm; months for a bankruptcy). 0 for world events and old savegames.
};

/** The real figures of one company, taken from the game when the dice ask for company news. Money units. */
struct CompanyFacts {
	uint8_t company = 0;
	int quarters = 0;             ///< Completed quarters on record (0, 1 or 2; two are needed to compare).
	int64_t income_last = 0;
	int64_t income_prev = 0;
	int64_t net_last = 0;         ///< Income plus expenses of the last completed quarter.
	int64_t net_prev = 0;
	int64_t value_last = 0;       ///< Company value at the end of the last completed quarter.
	int64_t value_prev = 0;
	int64_t debt = 0;             ///< Debt at the central bank now.
	bool in_default = false;
	int months_bankrupt = 0;
};

/** (now - before) / |before| in ppm, limited to +-MARKET_FACT_CAP_PPM. */
inline int64_t MarketChangePpm(int64_t now, int64_t before)
{
	const int64_t base = before < 0 ? -before : before;
	if (base <= 0) return 0;
	const int64_t scaled = (now - before) / base * 1'000'000 + (now - before) % base * 1'000'000 / base;
	return std::clamp<int64_t>(scaled, -MARKET_FACT_CAP_PPM, MARKET_FACT_CAP_PPM);
}

/** True for the kinds that are about one company (the real news). */
inline bool MarketKindIsCompany(MarketEventKind kind)
{
	const size_t k = static_cast<size_t>(kind);
	return k >= static_cast<size_t>(MarketEventKind::CompanyRevenueUp) && k <= static_cast<size_t>(MarketEventKind::CompanyBankruptcy);
}

/**
 * Whether a kind of company news is TRUE for a company now, and the real number for the story.
 * @param[out] fact_ppm The change of the figure in ppm, the debt share of the company value, the loss share of the revenue, or the months of bankruptcy. 0 for 'turned into a profit'.
 * @param[out] severity 1, 2 or 3 from the size of the real change.
 */
inline bool CompanyEventFact(MarketEventKind kind, const CompanyFacts &f, int64_t &fact_ppm, int &severity)
{
	fact_ppm = 0;
	severity = 1;
	auto sev_of = [](int64_t mag, int64_t mid, int64_t big) { return mag >= big ? 3 : (mag >= mid ? 2 : 1); };
	const bool two = f.quarters >= 2;
	switch (kind) {
		case MarketEventKind::CompanyRevenueUp:
		case MarketEventKind::CompanyRevenueDown: {
			if (!two || f.income_prev < MARKET_FACT_MIN_BASE) return false;
			const int64_t ch = MarketChangePpm(f.income_last, f.income_prev);
			if (kind == MarketEventKind::CompanyRevenueUp ? ch < MARKET_FACT_CHANGE_MIN_PPM : ch > -MARKET_FACT_CHANGE_MIN_PPM) return false;
			fact_ppm = ch;
			severity = sev_of(ch < 0 ? -ch : ch, 300'000, 800'000);
			return true;
		}
		case MarketEventKind::CompanyProfitUp: {
			if (!two || f.net_last <= 0) return false;
			if (f.net_prev <= 0) {
				severity = 2;
				return f.net_last >= MARKET_FACT_MIN_BASE / 4;
			}
			const int64_t ch = MarketChangePpm(f.net_last, f.net_prev);
			if (ch < MARKET_FACT_PROFIT_MIN_PPM) return false;
			fact_ppm = ch;
			severity = sev_of(ch, 400'000, 1'000'000);
			return true;
		}
		case MarketEventKind::CompanyProfitDown: {
			if (!two || f.net_last < 0 || f.net_prev < MARKET_FACT_MIN_BASE / 4) return false;
			const int64_t ch = MarketChangePpm(f.net_last, f.net_prev);
			if (ch > -MARKET_FACT_PROFIT_MIN_PPM) return false;
			fact_ppm = ch;
			severity = sev_of(-ch, 400'000, 700'000);
			return true;
		}
		case MarketEventKind::CompanyLoss: {
			if (f.quarters < 1 || f.net_last >= -MARKET_FACT_MIN_BASE / 4) return false;
			const int64_t loss = -f.net_last;
			const int64_t denom = std::max<int64_t>(f.income_last, 1);
			fact_ppm = std::min<int64_t>(MARKET_FACT_CAP_PPM, loss / denom * 1'000'000 + loss % denom * 1'000'000 / denom);
			severity = sev_of(fact_ppm, 300'000, 1'000'000);
			return true;
		}
		case MarketEventKind::CompanyValueUp:
		case MarketEventKind::CompanyValueDown: {
			if (!two || f.value_prev < MARKET_FACT_MIN_BASE * 5) return false;
			const int64_t ch = MarketChangePpm(f.value_last, f.value_prev);
			if (kind == MarketEventKind::CompanyValueUp ? ch < MARKET_FACT_CHANGE_MIN_PPM : ch > -MARKET_FACT_CHANGE_MIN_PPM) return false;
			fact_ppm = ch;
			severity = sev_of(ch < 0 ? -ch : ch, 250'000, 600'000);
			return true;
		}
		case MarketEventKind::CompanyDistress: {
			if (f.debt <= 0) return false;
			const int64_t denom = std::max<int64_t>(f.value_last, 1);
			const int64_t ratio = std::min<int64_t>(MARKET_FACT_CAP_PPM, f.debt / denom * 1'000'000 + f.debt % denom * 1'000'000 / denom);
			if (!f.in_default && ratio < MARKET_FACT_DEBT_MIN_PPM) return false;
			fact_ppm = ratio;
			severity = std::max(sev_of(ratio, 800'000, 1'200'000), f.in_default ? 2 : 1);
			return true;
		}
		case MarketEventKind::CompanyBankruptcy:
			if (f.months_bankrupt <= 0) return false;
			fact_ppm = static_cast<int64_t>(f.months_bankrupt);
			severity = 3;
			return true;
		default:
			return false;
	}
}

/** The state of the events. One object, see _market_events. Saved in the 'STCK' chunk (SRK_EVENT, SRK_EVENT_STATE). */
struct MarketEvents {
	std::deque<MarketEvent> log{};                           ///< The events, the newest in front, at most MARKET_EVENT_KEEP.
	uint64_t rng_state = 0;                                  ///< Random number generator; 0 = not seeded yet.
	uint32_t next_id = 1;                                    ///< Number of the next event.
	int64_t last_minute = -1;                                ///< Game minute of the last event (-1 = none yet).
	int64_t market_sentiment_ppm = 0;                        ///< Sentiment of the whole market.
	std::array<int64_t, MARKET_SENTIMENT_SLOTS> sentiment_ppm{}; ///< Sentiment of one symbol.

	void Reset() { *this = MarketEvents{}; }

	/** Start the generator (only when it is not started). @param seed Any number that is the same on every client, for example the generation seed. */
	void Seed(uint64_t seed)
	{
		if (this->rng_state != 0) return;
		const uint64_t s = seed * 0x9E3779B97F4A7C15ULL + 0x5851F42D4C957F2DULL;
		this->rng_state = s != 0 ? s : 0x2545F4914F6CDD1DULL;
	}

	/** xorshift64*, the same as the price model of the valuables. */
	uint64_t Next()
	{
		if (this->rng_state == 0) this->Seed(1);
		uint64_t x = this->rng_state;
		x ^= x >> 12;
		x ^= x << 25;
		x ^= x >> 27;
		this->rng_state = x;
		return x * 0x2545F4914F6CDD1DULL;
	}

	/** A number from lo to hi (both included). */
	int64_t Range(int64_t lo, int64_t hi)
	{
		if (hi <= lo) return lo;
		return lo + static_cast<int64_t>((this->Next() >> 11) % static_cast<uint64_t>(hi - lo + 1));
	}

	/** Roll the dice once per game hour. @param minute Game minute now. @return True when an event has to happen now. */
	bool ShouldFire(int64_t minute)
	{
		if (minute < static_cast<int64_t>(MARKET_EVENT_START_DELAY_HOURS) * 60) return false;
		if (this->last_minute >= 0 && minute - this->last_minute < static_cast<int64_t>(MARKET_EVENT_MIN_GAP_HOURS) * 60) return false;
		return static_cast<int>((this->Next() >> 11) % 1000) < MARKET_EVENT_CHANCE_PERMILLE;
	}

	/** True when the log already has this kind of news about this company within MARKET_COMPANY_REPEAT_DAYS game days. */
	bool RecentlyReported(MarketEventKind kind, uint8_t company, int64_t minute) const
	{
		for (const MarketEvent &e : this->log) {
			if (minute - e.minute > MARKET_COMPANY_REPEAT_DAYS * 24 * 60) break; // newest first
			if (e.kind == kind && e.target == company) return true;
		}
		return false;
	}

	/**
	 * Draw an event. World, gold and sector events are chance. A company event is only possible when it is TRUE for at least one company (see
	 * #CompanyEventFact); the company is drawn among those, severity and number come from the real figures.
	 * @param facts Real figures of the companies that exist.
	 * @param force_kind Test tool: kind number, -1 = by weight. A company kind that is not true for any company is drawn by weight instead.
	 */
	MarketEvent Generate(int64_t minute, const std::vector<CompanyFacts> &facts, int force_kind = -1)
	{
		std::array<std::vector<size_t>, MARKET_EVENT_KIND_COUNT> candidates{};
		for (size_t k = 0; k < MARKET_EVENT_KIND_COUNT; k++) {
			const MarketEventKind kind = static_cast<MarketEventKind>(k);
			if (!MarketEventInfo(kind).company) continue;
			for (size_t i = 0; i < facts.size(); i++) {
				int64_t fact = 0; int sev = 1;
				if (!CompanyEventFact(kind, facts[i], fact, sev)) continue;
				if (force_kind < 0 && this->RecentlyReported(kind, facts[i].company, minute)) continue;
				candidates[k].push_back(i);
			}
		}
		auto allowed = [&](size_t k) { return !MarketEventInfo(static_cast<MarketEventKind>(k)).company || !candidates[k].empty(); };

		uint32_t total = 0;
		for (size_t k = 0; k < MARKET_EVENT_KIND_COUNT; k++) if (allowed(k)) total += MarketEventInfo(static_cast<MarketEventKind>(k)).weight;
		uint32_t pick = static_cast<uint32_t>(this->Next() >> 11) % std::max<uint32_t>(total, 1);
		size_t chosen = 0;
		for (size_t k = 0; k < MARKET_EVENT_KIND_COUNT; k++) {
			if (!allowed(k)) continue;
			const uint32_t w = MarketEventInfo(static_cast<MarketEventKind>(k)).weight;
			if (pick < w) { chosen = k; break; }
			pick -= w;
		}
		if (force_kind >= 0 && static_cast<size_t>(force_kind) < MARKET_EVENT_KIND_COUNT && allowed(static_cast<size_t>(force_kind))) chosen = static_cast<size_t>(force_kind);

		const MarketEventDef &def = MarketEventInfo(static_cast<MarketEventKind>(chosen));
		MarketEvent e;
		e.id = this->next_id++;
		e.kind = static_cast<MarketEventKind>(chosen);
		e.minute = minute;
		const uint32_t sev_roll = static_cast<uint32_t>(this->Next() >> 11) % 100;
		e.severity = sev_roll < 60 ? 1 : (sev_roll < 90 ? 2 : 3);
		e.variant = static_cast<uint8_t>(this->Range(0, 11));
		if (def.company) {
			const std::vector<size_t> &c = candidates[chosen];
			const CompanyFacts &f = facts[c[static_cast<size_t>(this->Range(0, static_cast<int64_t>(c.size()) - 1))]];
			int sev = 1;
			CompanyEventFact(e.kind, f, e.fact_ppm, sev);
			e.target = f.company;
			e.severity = static_cast<uint8_t>(sev); // The size of the news is the size of the real change.
		}
		const int pct = MarketSeverityPercent(e.severity);
		if (def.gold_dir != 0) {
			const int64_t mag = this->Range(def.gold_min, def.gold_max) * pct / 100;
			e.gold_ppm = def.gold_dir * std::min<int64_t>(mag, MARKET_EVENT_GOLD_MAX_PPM);
		}
		if (def.share_dir != 0) {
			const int64_t mag = this->Range(def.share_min, def.share_max) * pct / 100;
			e.share_ppm = def.share_dir * std::min<int64_t>(mag, MARKET_EVENT_SHARE_MAX_PPM);
		}
		return e;
	}

	/** Put the share effect of an event into the sentiment and into the log. The gold effect is applied by the caller (it needs the gold market). */
	void Apply(const MarketEvent &e)
	{
		if (e.share_ppm != 0) {
			if (e.target != MARKET_NO_TARGET && e.target < MARKET_SENTIMENT_SLOTS) {
				int64_t &s = this->sentiment_ppm[e.target];
				s = std::clamp<int64_t>(s + e.share_ppm, -MARKET_SENTIMENT_MAX_PPM, MARKET_SENTIMENT_MAX_PPM);
			} else {
				this->market_sentiment_ppm = std::clamp<int64_t>(this->market_sentiment_ppm + e.share_ppm, -MARKET_SENTIMENT_MAX_PPM, MARKET_SENTIMENT_MAX_PPM);
			}
		}
		this->last_minute = e.minute;
		this->log.push_front(e);
		while (this->log.size() > MARKET_EVENT_KEEP) this->log.pop_back();
	}

	/** Once per game hour: the sentiment fades. */
	void DecayHour()
	{
		auto fade = [](int64_t &v) {
			v -= v / MARKET_SENTIMENT_DECAY_DIV;
			if (v > -MARKET_SENTIMENT_DECAY_DIV && v < MARKET_SENTIMENT_DECAY_DIV) v = 0;
		};
		fade(this->market_sentiment_ppm);
		for (int64_t &s : this->sentiment_ppm) fade(s);
	}

	/** The sentiment that moves the goal price of a symbol (market plus own), limited. 0 for a symbol that is not a company. */
	int64_t SentimentPpm(size_t symbol) const
	{
		if (symbol >= MARKET_SENTIMENT_SLOTS) return 0;
		return std::clamp<int64_t>(this->market_sentiment_ppm + this->sentiment_ppm[symbol], -MARKET_SENTIMENT_MAX_PPM, MARKET_SENTIMENT_MAX_PPM);
	}
};

extern MarketEvents _market_events;

/** The sentiment of a share in ppm, for the price formula of stock_base.cpp. Companies only: valuables follow the gold price. */
int64_t MarketEventSentimentPpm(size_t symbol);

/** Test tool (console command stock_event): make an event now. @param kind Number of the kind, -1 = by weight. @return The event that happened. */
MarketEvent FireMarketEventNow(int kind);

#endif /* MARKET_EVENTS_H */
