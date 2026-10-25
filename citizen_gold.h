/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file citizen_gold.h How the citizens of a town deal with gold: four groups with four different habits (pure logic, standard library only, so it
 * can be tested outside the game, see citizen_gold_test.cpp). The game side (prices, market stock, pressure, taxes, saving) is in town_treasury.cpp.
 *
 *  - Hoarder      (กักตุน):        buys a little gold every hour and keeps it; more when the price is low or the news is scary; sells a little only at a big gain.
 *  - Speculator   (เก็งกำไร):      follows the trend; take-profit, stop-loss, trend break. Chases rises, so it often loses.
 *  - Overtrader   (เทรดถี่เกินไป):  trades in many hours, both ways, on noise and the last move. Pays spread and commission again and again.
 *  - Balanced     (สมดุล):         keeps a target share of its wealth in gold and rebalances.
 * Nobody is guaranteed to win. The books keep their results (wins, losses, profit).
 *
 * All maths is integer. Randomness is a hash of (game seed, town, group, game hour): nothing has to be saved, every client decides the same.
 * Gold is counted in hundredths of an ounce ("centi-ounces", cz).
 */

#ifndef CITIZEN_GOLD_H
#define CITIZEN_GOLD_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace citizengold {

static constexpr int64_t PPM = 1'000'000;
static constexpr int64_t CZ_PER_OZ = 100;                ///< Hundredths of an ounce in one ounce.
static constexpr int64_t COMMISSION_PPM = 1'000;         ///< Commission of a citizen order: 0.1% of its value.

/** The four groups. The numbers are saved: add new groups at the END, before Count. */
enum class Style : uint8_t {
	Hoarder,
	Speculator,
	Overtrader,
	Balanced,
	Count,
};
static constexpr size_t STYLE_COUNT = static_cast<size_t>(Style::Count);

/** Share of the town in each group at an average town, in ppm. Sums to PPM. */
static constexpr std::array<int64_t, STYLE_COUNT> BASE_SHARE_PPM = { 250'000, 200'000, 150'000, 400'000 };
static constexpr int64_t SHARE_JITTER_PPM = 80'000;      ///< A town moves each share by up to +-8 points.
static constexpr int64_t START_WEALTH_PER_CAPITA = 400;  ///< Wealth of one citizen when the town gets its account (money units, price index 1.0).
static constexpr int64_t START_GOLD_SHARE_PPM = 100'000; ///< Part of that wealth already in gold at the start (10%).

static constexpr int64_t HOARD_BUY_PPM_PER_HOUR = 6'000; ///< Hoarder: buys 0.6% of its cash every hour.
static constexpr int64_t HOARD_DIP_PPM = 30'000;         ///< Price 3% below the 30 day average: buys 2.5 times as much.
static constexpr int64_t HOARD_RICH_PPM = 80'000;        ///< Price 8% above the average: buys only a fifth.
static constexpr int64_t HOARD_FEAR_PPM = 30'000;        ///< Gold pressure this high: buys 1.5 times as much.
static constexpr int64_t HOARD_SELL_GAIN_PPM = 600'000;  ///< Sells a little only when the gold is worth 60% more than it cost ...
static constexpr int64_t HOARD_SELL_PPM_PER_HOUR = 10'000; ///< ... 1% of the holding per hour.

static constexpr int64_t SPEC_ENTER_MOMENTUM_PPM = 12'000; ///< Speculator: price 1.2% above its 7 day average and above the 30 day average: buys.
static constexpr int64_t SPEC_ENTER_SHARE_PERCENT = 60;  ///< Puts 60% of its cash into one entry.
static constexpr int64_t SPEC_TAKE_PROFIT_PPM = 80'000;  ///< Sells everything at +8% over its average cost.
static constexpr int64_t SPEC_STOP_LOSS_PPM = 50'000;    ///< Sells everything at -5% under its average cost.
static constexpr int64_t SPEC_TREND_BREAK_PPM = 15'000;  ///< Sells everything when the price falls 1.5% under the 7 day average.
static constexpr int64_t SPEC_COOLDOWN_HOURS = 24;       ///< Waits a game day after selling.

static constexpr int64_t OVER_TRADE_PERMILLE = 150;      ///< Overtrader: acts in 15% of the hours.
static constexpr int64_t OVER_CHASE_PERMILLE = 600;      ///< 60% of its choices follow the last day's move, the rest is a coin.
static constexpr int64_t OVER_BUY_CASH_PERCENT = 40;
static constexpr int64_t OVER_SELL_GOLD_PERCENT = 50;

static constexpr int64_t BAL_TARGET_PPM = 150'000;       ///< Balanced: wants 15% of its wealth in gold.
static constexpr int64_t BAL_BAND_PPM = 20'000;          ///< Does nothing while the gold share is within +-2 points of the target.
static constexpr int64_t BAL_STEP_PERCENT = 20;          ///< Closes 20% of the gap per hour.

/** Signals that the group sees. Prices in 1/100 of a money unit per ounce. */
struct MarketView {
	int64_t price_cents = 0;
	int64_t ask_cents = 0;
	int64_t bid_cents = 0;
	int64_t avg7_cents = 0;       ///< Average of the last 7 daily closes; 0 = not enough history.
	int64_t avg30_cents = 0;      ///< Average of the last 30 daily closes; 0 = not enough history.
	int64_t prev_close_cents = 0; ///< Close of the last day; 0 = unknown.
	int64_t pressure_ppm = 0;     ///< Gold demand pressure: + = scared / buying.
	int64_t stock_cz = INT64_MAX; ///< Gold the market can still sell, in centi-ounces.
};

/** One group of citizens of one town. */
struct Book {
	int64_t gold_cz = 0;
	int64_t cost = 0;          ///< What that gold cost, money units, without commission.
	int64_t cash = 0;
	int64_t profit = 0;        ///< Profit of all closed sales (after commission, before tax); negative = loss.
	int32_t wins = 0;
	int32_t losses = 0;
	int32_t cooldown = 0;      ///< Hours before the group may enter again (speculators).
	int64_t turnover = 0;      ///< Money that went through the orders (buys + sells).
};

struct GoldOrder {
	bool buy = false;
	int64_t cz = 0;
};

/** A deterministic 64 bit hash (splitmix64) of the inputs: the 'dice' of the citizens. */
inline uint64_t Mix(uint64_t a, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0)
{
	uint64_t x = a * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL;
	for (uint64_t v : {b, c, d}) {
		x ^= v + 0x9E3779B97F4A7C15ULL + (x << 6) + (x >> 2);
		x += 0x9E3779B97F4A7C15ULL;
		uint64_t z = x;
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		x = z ^ (z >> 31);
	}
	return x;
}

/** A number 0 .. n-1 from the hash. */
inline int64_t Pick(uint64_t hash, int64_t n) { return n <= 0 ? 0 : static_cast<int64_t>((hash >> 11) % static_cast<uint64_t>(n)); }

/** The share of a town in a group, in ppm. The four shares of a town sum to exactly PPM. */
inline std::array<int64_t, STYLE_COUNT> GroupSharePpm(uint64_t seed, uint32_t town)
{
	std::array<int64_t, STYLE_COUNT> share{};
	int64_t sum = 0;
	for (size_t i = 0; i < STYLE_COUNT; i++) {
		const int64_t jitter = Pick(Mix(seed, town, i, 0xC17), 2 * SHARE_JITTER_PPM + 1) - SHARE_JITTER_PPM;
		share[i] = std::max<int64_t>(50'000, BASE_SHARE_PPM[i] + jitter);
		sum += share[i];
	}
	int64_t used = 0;
	for (size_t i = 0; i + 1 < STYLE_COUNT; i++) { share[i] = share[i] * PPM / sum; used += share[i]; }
	share[STYLE_COUNT - 1] = PPM - used;
	return share;
}

inline int64_t Commission(int64_t value) { return value <= 0 ? 0 : (value * COMMISSION_PPM + PPM - 1) / PPM; }

/** The most centi-ounces that this cash buys at the ask price including the commission. */
inline int64_t MaxBuyCz(int64_t cash, int64_t ask_cents)
{
	if (cash <= 0 || ask_cents <= 0) return 0;
	int64_t n = cash * CZ_PER_OZ * 100 / ask_cents;
	auto total = [&](int64_t k) { const int64_t cost = (k * ask_cents + 9'999) / 10'000; return cost + Commission(cost); };
	while (n > 0 && total(n) > cash) n--;
	while (total(n + 1) <= cash) n++;
	return n;
}

inline int64_t GoldValue(const Book &b, int64_t bid_cents) { return b.gold_cz * bid_cents / 10'000; }
inline int64_t Wealth(const Book &b, int64_t bid_cents) { return b.cash + GoldValue(b, bid_cents); }

/** The outcome of an order that was filled, for the game to account (spread, commission, impact). */
struct Fill {
	int64_t cz = 0;
	int64_t gross = 0;
	int64_t commission = 0;
	int64_t gain = 0;          ///< Realised result of a sell before tax; 0 for a buy.
};

inline Fill ApplyBuy(Book &b, int64_t cz, int64_t ask_cents, int64_t stock_cz)
{
	Fill f;
	cz = std::min({cz, MaxBuyCz(b.cash, ask_cents), stock_cz});
	if (cz <= 0) return f;
	const int64_t cost = (cz * ask_cents + 9'999) / 10'000;
	const int64_t com = Commission(cost);
	if (cost + com > b.cash) return f;
	b.cash -= cost + com;
	b.gold_cz += cz;
	b.cost += cost;
	b.turnover += cost;
	f.cz = cz; f.gross = cost; f.commission = com;
	return f;
}

inline Fill ApplySell(Book &b, int64_t cz, int64_t bid_cents)
{
	Fill f;
	cz = std::min(cz, b.gold_cz);
	if (cz <= 0) return f;
	const int64_t gross = cz * bid_cents / 10'000;
	const int64_t com = std::min(gross, Commission(gross));
	const int64_t basis = b.cost > 0 ? (cz == b.gold_cz ? b.cost : b.cost * cz / b.gold_cz) : gross;
	b.gold_cz -= cz;
	b.cost = b.gold_cz == 0 ? 0 : std::max<int64_t>(0, b.cost - basis);
	b.cash += gross - com;
	b.turnover += gross;
	f.cz = cz; f.gross = gross; f.commission = com; f.gain = gross - com - basis;
	b.profit += f.gain;
	if (f.gain > 0) b.wins++; else if (f.gain < 0) b.losses++;
	return f;
}

inline int64_t AverageCostCents(const Book &b) { return b.gold_cz > 0 ? b.cost * 10'000 / b.gold_cz : 0; }

/** What a group wants to do in this game hour. @param dice The hash of this group in this hour (see Mix). */
inline GoldOrder Decide(Style style, const Book &b, const MarketView &m, uint64_t dice)
{
	GoldOrder o;
	if (m.price_cents <= 0 || m.ask_cents <= 0 || m.bid_cents <= 0) return o;
	const bool have_avg = m.avg30_cents > 0 && m.avg7_cents > 0;
	const int64_t dev30 = m.avg30_cents > 0 ? (m.price_cents - m.avg30_cents) * PPM / m.avg30_cents : 0;
	const int64_t dev7 = m.avg7_cents > 0 ? (m.price_cents - m.avg7_cents) * PPM / m.avg7_cents : 0;
	const int64_t avg_cost = AverageCostCents(b);

	switch (style) {
		case Style::Hoarder: {
			if (b.gold_cz > 0 && avg_cost > 0 && (m.bid_cents - avg_cost) * PPM / avg_cost >= HOARD_SELL_GAIN_PPM) {
				o.buy = false;
				o.cz = std::max<int64_t>(1, b.gold_cz * HOARD_SELL_PPM_PER_HOUR / PPM);
				return o;
			}
			int64_t rate = HOARD_BUY_PPM_PER_HOUR;
			if (have_avg && dev30 <= -HOARD_DIP_PPM) rate = rate * 5 / 2;
			else if (have_avg && dev30 >= HOARD_RICH_PPM) rate = rate / 5;
			if (m.pressure_ppm >= HOARD_FEAR_PPM) rate = rate * 3 / 2;
			o.buy = true;
			o.cz = MaxBuyCz(b.cash * rate / PPM, m.ask_cents);
			return o;
		}
		case Style::Speculator: {
			if (!have_avg) return o;
			if (b.gold_cz > 0) {
				const int64_t pnl = avg_cost > 0 ? (m.bid_cents - avg_cost) * PPM / avg_cost : 0;
				if (pnl >= SPEC_TAKE_PROFIT_PPM || pnl <= -SPEC_STOP_LOSS_PPM || dev7 <= -SPEC_TREND_BREAK_PPM) {
					o.buy = false; o.cz = b.gold_cz; return o;
				}
				return o;
			}
			if (b.cooldown > 0) return o;
			if (dev7 >= SPEC_ENTER_MOMENTUM_PPM && dev30 > 0) {
				o.buy = true;
				o.cz = MaxBuyCz(b.cash * SPEC_ENTER_SHARE_PERCENT / 100, m.ask_cents);
			}
			return o;
		}
		case Style::Overtrader: {
			if (Pick(dice, 1000) >= OVER_TRADE_PERMILLE) return o;
			bool up = Pick(Mix(dice, 1), 2) == 0;
			if (m.prev_close_cents > 0 && Pick(Mix(dice, 2), 1000) < OVER_CHASE_PERMILLE) up = m.price_cents >= m.prev_close_cents;
			if (up) {
				o.buy = true;
				o.cz = MaxBuyCz(b.cash * OVER_BUY_CASH_PERCENT / 100, m.ask_cents);
			} else if (b.gold_cz > 0) {
				o.buy = false;
				o.cz = std::max<int64_t>(1, b.gold_cz * OVER_SELL_GOLD_PERCENT / 100);
			}
			return o;
		}
		case Style::Balanced: {
			const int64_t wealth = Wealth(b, m.bid_cents);
			if (wealth <= 0) return o;
			const int64_t share = GoldValue(b, m.bid_cents) * PPM / wealth;
			const int64_t gap = BAL_TARGET_PPM - share;
			if (gap > -BAL_BAND_PPM && gap < BAL_BAND_PPM) return o;
			const int64_t money = (gap < 0 ? -gap : gap) * wealth / PPM * BAL_STEP_PERCENT / 100;
			if (gap > 0) {
				o.buy = true;
				o.cz = MaxBuyCz(std::min(money, b.cash), m.ask_cents);
			} else {
				o.buy = false;
				o.cz = std::min(b.gold_cz, std::max<int64_t>(1, money * 10'000 / m.bid_cents));
			}
			return o;
		}
		default:
			return o;
	}
}

} // namespace citizengold

#endif /* CITIZEN_GOLD_H */
