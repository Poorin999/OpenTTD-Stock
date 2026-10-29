/* Stand-alone test of citizen_gold.h (not part of the game build).
 * Build and run:  g++ -std=c++17 -Wall -Wextra -o /tmp/cgt citizen_gold_test.cpp && /tmp/cgt */
#include <cstdio>
#include <cmath>
#include "gold_market_math.h"
#include "citizen_gold.h"
using namespace citizengold;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Result { std::array<Book, STYLE_COUNT> b; std::array<int64_t, STYLE_COUNT> start_wealth{}; std::array<int64_t, STYLE_COUNT> end_wealth{}; int64_t final_bid = 0; int64_t bought_cz = 0, sold_cz = 0; };

/* One town of 1000 people for `days` game days on a price made by gold_market_math. */
static Result Sim(uint64_t seed, int days, bool print_share = false)
{
	Result r;
	const auto share = GroupSharePpm(seed, 1);
	int64_t sum = 0; for (auto s : share) { CHECK(s > 0); sum += s; }
	CHECK(sum == PPM);
	std::vector<int64_t> hist; int64_t price = 200'000;
	const int64_t spread_ppm = 3'500;
	for (size_t i = 0; i < STYLE_COUNT; i++) {
		const int64_t w = 1000 * START_WEALTH_PER_CAPITA * share[i] / PPM;
		const int64_t gold_money = w * START_GOLD_SHARE_PPM / PPM;
		r.b[i].cash = w - gold_money;
		r.b[i].gold_cz = gold_money * 10'000 / price;
		r.b[i].cost = gold_money;
		r.start_wealth[i] = Wealth(r.b[i], price);
	}
	for (int d = 0; d < days; d++) {
		const int64_t target = 200'000 * (PPM + goldmath::CyclePpm(seed, d)) / PPM;
		int64_t step = (target - price) * 5 / 100;
		const int64_t day_start = price;
		const int64_t day_end = (day_start + step) * (PPM + goldmath::DailyNoisePpm(seed, d)) / PPM;
		hist.push_back(day_start);
		for (int h = 0; h < 24; h++) {
			const int64_t p = day_start + (day_end - day_start) * (h * 60 + 60) / 1440;
			MarketView m; m.price_cents = p; m.ask_cents = (p * (PPM + spread_ppm / 2) + PPM - 1) / PPM; m.bid_cents = p * (PPM - spread_ppm / 2) / PPM;
			const size_t n = hist.size();
			auto avg = [&](size_t k) { int64_t s = 0; for (size_t i = n - k; i < n; i++) s += hist[i]; return s / (int64_t)k; };
			m.avg7_cents = n >= 3 ? avg(std::min<size_t>(7, n)) : 0;
			m.avg30_cents = n >= 10 ? avg(std::min<size_t>(30, n)) : 0;
			m.prev_close_cents = hist.back();
			for (size_t i = 0; i < STYLE_COUNT; i++) {
				Book &b = r.b[i];
				if (b.cooldown > 0) b.cooldown--;
				GoldOrder o = Decide(static_cast<Style>(i), b, m, Mix(seed, 1, i, d * 24 + h));
				if (o.cz <= 0) continue;
				if (o.buy) { Fill f = ApplyBuy(b, o.cz, m.ask_cents, INT64_MAX); r.bought_cz += f.cz; CHECK(f.cz >= 0); }
				else { Fill f = ApplySell(b, o.cz, m.bid_cents); r.sold_cz += f.cz; if (static_cast<Style>(i) == Style::Speculator && b.gold_cz == 0 && f.cz > 0) b.cooldown = SPEC_COOLDOWN_HOURS; }
				CHECK(b.cash >= 0 && b.gold_cz >= 0 && b.cost >= 0);
			}
		}
		price = day_end;
		for (size_t i = 0; i < STYLE_COUNT; i++) r.b[i].cash += 1000 * 50 * share[i] / PPM / 100 * 75 / 100; // income
		if (print_share && d % 200 == 0) std::printf("  day %d price %lld\n", d, (long long)(price / 100));
	}
	r.final_bid = price * (PPM - spread_ppm / 2) / PPM;
	for (size_t i = 0; i < STYLE_COUNT; i++) r.end_wealth[i] = Wealth(r.b[i], r.final_bid);
	return r;
}

int main()
{
	/* Shares: sum to 1.0, vary by town, same for the same (seed, town). */
	for (uint32_t t = 0; t < 500; t++) { auto s = GroupSharePpm(9, t); int64_t sum = 0; for (auto v : s) { CHECK(v >= 40'000); sum += v; } CHECK(sum == PPM); CHECK(s == GroupSharePpm(9, t)); }
	CHECK(GroupSharePpm(9, 1) != GroupSharePpm(9, 2));

	/* Buy / sell mechanics: money and gold are conserved, profit and win / loss recorded. */
	{
		Book b; b.cash = 300'000;
		const int64_t ask = 200'000, bid = 199'000;
		Fill f = ApplyBuy(b, 5'000, ask, INT64_MAX); // 50 oz
		CHECK(f.cz == 5'000 && f.gross == (5'000 * ask + 9'999) / 10'000 && b.cash == 300'000 - f.gross - f.commission && b.gold_cz == 5'000);
		CHECK(ApplyBuy(b, 500'000, ask, INT64_MAX).cz < 500'000); // limited by the cash
		Book c; c.cash = 1'000; CHECK(ApplyBuy(c, 100'000, ask, INT64_MAX).cz < 100'000 && c.cash >= 0);
		Book d; d.cash = 1'000'000; CHECK(ApplyBuy(d, 50'000, ask, 1'000).cz == 1'000); // the market stock limits
		Fill s = ApplySell(b, 999'999, bid);
		CHECK(s.cz > 0 && b.gold_cz == 0 && b.cost == 0 && s.gain < 0 && b.losses == 1 && b.wins == 0 && b.profit == s.gain); // bid below ask: a loss
		Book e; e.gold_cz = 1'000; e.cost = 10'000; e.cash = 0;
		Fill w = ApplySell(e, 400, 200'000); // 4 oz at 2000
		CHECK(w.cz == 400 && e.gold_cz == 600 && e.cost == 6'000 && w.gain > 0 && e.wins == 1);
		CHECK(MaxBuyCz(0, ask) == 0 && MaxBuyCz(500, ask) >= 0);
		for (int64_t cash : {10, 999, 12'345, 1'000'000}) { int64_t n = MaxBuyCz(cash, 207'123); Book z; z.cash = cash; CHECK(ApplyBuy(z, n, 207'123, INT64_MAX).cz == n); Book y; y.cash = cash; CHECK(ApplyBuy(y, n + 1, 207'123, INT64_MAX).cz == n); }
	}

	/* Determinism. */
	{ Result a = Sim(5, 400), b = Sim(5, 400); for (size_t i = 0; i < STYLE_COUNT; i++) CHECK(a.b[i].cash == b.b[i].cash && a.b[i].gold_cz == b.b[i].gold_cz && a.b[i].profit == b.b[i].profit); }

	/* Four styles over 3 game years, 40 different games. */
	const char *names[STYLE_COUNT] = { "hoarder", "speculator", "overtrader", "balanced" };
	double ret_sum[STYLE_COUNT] = {}; int wins_tot[STYLE_COUNT] = {}, loss_tot[STYLE_COUNT] = {}; double turn_sum[STYLE_COUNT] = {}; int lost_games[STYLE_COUNT] = {}, won_games[STYLE_COUNT] = {};
	double share_end_bal = 0; int games = 0; double prof_sum[STYLE_COUNT] = {};
	for (uint64_t seed = 1; seed <= 40; seed++) {
		Result r = Sim(seed * 7919, 1095); games++;
		for (size_t i = 0; i < STYLE_COUNT; i++) {
			const double ret = r.start_wealth[i] > 0 ? double(r.end_wealth[i] - r.start_wealth[i]) / r.start_wealth[i] : 0; // includes income, so compare styles with each other
			ret_sum[i] += ret; wins_tot[i] += r.b[i].wins; loss_tot[i] += r.b[i].losses; turn_sum[i] += double(r.b[i].turnover) / std::max<int64_t>(1, r.start_wealth[i]);
			(r.b[i].profit < 0 ? lost_games[i] : won_games[i])++; prof_sum[i] += r.start_wealth[i] > 0 ? double(r.b[i].profit) / r.start_wealth[i] : 0;
		}
		share_end_bal += double(GoldValue(r.b[3], r.final_bid)) / std::max<int64_t>(1, Wealth(r.b[3], r.final_bid));
	}
	for (size_t i = 0; i < STYLE_COUNT; i++)
		std::printf("%-10s avg wealth change %+7.1f%%  trades won/lost %d/%d  turnover x%.1f  realised trading result %+6.1f%% of start wealth  games with a trading profit/loss %d/%d\n", names[i], ret_sum[i] / games * 100, wins_tot[i], loss_tot[i], turn_sum[i] / games, prof_sum[i] / games * 100, won_games[i], lost_games[i]);
	std::printf("balanced: gold share at the end %.1f%% (target %lld%%)\n", share_end_bal / games * 100, (long long)(BAL_TARGET_PPM / 10'000));

	CHECK(turn_sum[2] > 3 * turn_sum[3] && turn_sum[2] > 5 * turn_sum[0]);                 // overtraders are by far the busiest
	CHECK(loss_tot[2] > 0 && wins_tot[2] > 0);                                              // they win some, lose many
	CHECK(lost_games[2] > won_games[2] * 3);                                                // and mostly end with a trading loss
	CHECK(wins_tot[1] > 0 && loss_tot[1] > 0 && loss_tot[1] * 10 > wins_tot[1]);             // speculators: wins AND losses happen, nobody is guaranteed
	CHECK(turn_sum[0] > 0 && turn_sum[3] > 0);                                              // hoarders and balanced traders trade as well
	CHECK(std::fabs(share_end_bal / games - BAL_TARGET_PPM / 1e6) < 0.06);                  // balanced keeps near its target
	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
