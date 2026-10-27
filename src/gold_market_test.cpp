/* Stand-alone test of gold_market_math.h (not part of the game build).
 * Build and run:  g++ -std=c++17 -Wall -Wextra -o /tmp/gmt gold_market_test.cpp && /tmp/gmt */
#include <cstdio>
#include <cmath>
#include <vector>
#include "gold_market_math.h"
using namespace goldmath;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* Same as GoldReserveManager::UpdateGlide: one 2 minute step of the random walk, then the change of the level. */
static int64_t Step(int64_t price, uint64_t seed, int64_t minute, int64_t level, int64_t &last_level)
{
	const int64_t noise = StepNoisePpm(seed, minute);
	int64_t p = price / PPM * (PPM + noise) + price % PPM * (PPM + noise) / PPM;
	if (last_level > 0 && level > 0 && level != last_level) p = p * level / last_level;
	if (level > 0) last_level = level;
	return std::max<int64_t>(1, p);
}

int main()
{
	/* Determinism and bounds. */
	for (uint64_t seed = 1; seed <= 50; seed++) for (int64_t d = 0; d < 2000; d++) {
		CHECK(DailyNoisePpm(seed, d) == DailyNoisePpm(seed, d));
		CHECK(std::abs(DailyNoisePpm(seed, d)) <= 3 * NOISE_SIGMA_PPM * 22 / 10 + 1);
		CHECK(std::abs(CyclePpm(seed, d)) <= CYCLE_LONG_AMP_PPM + CYCLE_SHORT_AMP_PPM);
	}
	CHECK(CyclePpm(1, 100) != CyclePpm(2, 100) || CyclePpm(1, 200) != CyclePpm(2, 200)); // seeds differ

	/* Noise: mean about 0, sd about the wanted size. */
	double sum = 0, sq = 0; const int N = 200000;
	for (int d = 0; d < N; d++) { double v = DailyNoisePpm(7, d); sum += v; sq += v * v; }
	double mean = sum / N, sd = std::sqrt(sq / N - mean * mean);
	std::printf("noise mean %.0f ppm, sd %.0f ppm (target sigma %lld x vol mix)\n", mean, sd, (long long)NOISE_SIGMA_PPM);
	CHECK(std::abs(mean) < 150 && sd > 4'000 && sd < 9'500);

	/* Cycle: mean about 0 over a long time (no steady push), and it really swings. */
	for (uint64_t seed : {1ull, 2ull, 3ull, 99ull}) {
		double cs = 0; int64_t mn = 0, mx = 0;
		for (int d = 0; d < 20000; d++) { int64_t c = CyclePpm(seed, d); cs += c; mn = std::min(mn, c); mx = std::max(mx, c); }
		std::printf("seed %llu cycle mean %.0f ppm, min %lld, max %lld\n", (unsigned long long)seed, cs / 20000, (long long)mn, (long long)mx);
		CHECK(std::abs(cs / 20000) < 8'000 && mx > 60'000 && mn < -60'000);
	}

	/* Rebalance: a stock that mines filled comes back; a stock that was bought down comes back. */
	{
		const int64_t start = 32'151'000;
		int64_t hi = start * 3 / 2; for (int d = 0; d < 365; d++) hi = RebalancedStock(hi, start);
		int64_t lo = start / 4; for (int d = 0; d < 365; d++) lo = RebalancedStock(lo, start);
		CHECK(std::abs(hi - start) < start / 20 && std::abs(lo - start) < start / 8);
		/* Mines at the daily maximum: the stock settles about 25% above the start, not at the cap. */
		int64_t st = start; for (int d = 0; d < 3000; d++) { st = std::min(start * 3 / 2, st + 64'300); st = RebalancedStock(st, start); }
		std::printf("stock with full mine flow settles at %.2f x start\n", (double)st / start);
		CHECK(st < start * 13 / 10 && st > start);
		CHECK(RebalancedStock(start, start) == start);
	}

	/* Step noise: mean 0, and 720 steps give the daily sd of the spell (a random walk). */
	{
		double sum = 0, sq = 0; const int N = 720 * 2000;
		for (int m = 1; m <= N; m++) { double v = StepNoisePpm(11, m); sum += v; sq += v * v; CHECK(StepNoisePpm(11, m) == StepNoisePpm(11, m)); }
		const double mean = sum / N, sd = std::sqrt(sq / N - mean * mean);
		std::printf("step noise mean %.2f ppm, sd %.1f ppm (x sqrt(720) = %.0f ppm a day, mix of spells)\n", mean, sd, sd * std::sqrt(720.0));
		CHECK(std::abs(mean) < 3 && sd * std::sqrt(720.0) > 6'000 && sd * std::sqrt(720.0) < 11'500);
		CHECK(StepNoisePpm(1, 5000) != StepNoisePpm(2, 5000) || StepNoisePpm(1, 5002) != StepNoisePpm(2, 5002)); // seeds differ
	}

	/* The price over 5 game years: random walk every 2 minutes, level constant, the weak pull once a day. No glide, no cycle. */
	for (uint64_t seed : {1ull, 2ull, 3ull, 4ull}) {
		const int64_t level = 200'000; int64_t last_level = 0;
		int64_t price = level; double ssum = 0, ssq = 0; int64_t mn = price, mx = price, maxmove = 0; int n = 0;
		double sp_calm = 0, sp_storm = 0; int n_calm = 0, n_storm = 0; double mid_dev = 0;
		for (int64_t d = 1; d <= 1825; d++) {
			const int64_t start = price; int64_t noon = start;
			for (int64_t m = 2; m < 1440; m += 2) { price = Step(price, seed, d * 1440 + m, level, last_level); if (m == 720) noon = price; }
			price = RevertedPrice(price, level);
			const double r = double(price - start) / start;
			ssum += r; ssq += r * r; n++; maxmove = std::max<int64_t>(maxmove, std::llabs(price - start) * 10000 / start);
			const int64_t vol = SmoothVolatilityPpm(seed, d * 1440 + 720);
			if (vol <= 700'000) { sp_calm += r * r; n_calm++; } else if (vol >= 1'600'000) { sp_storm += r * r; n_storm++; }
			/* The middle of the day is not on the straight line to the end of the day (the old glide was). */
			mid_dev += std::abs(double(noon - start) - double(price - start) / 2) / start;
			mn = std::min(mn, price); mx = std::max(mx, price);
		}
		const double m = ssum / n, s = std::sqrt(ssq / n - m * m);
		std::printf("seed %llu: range %lld..%lld  daily sd %.2f%%  annualised %.1f%%  max day move %.2f%%  calm sd %.2f%% storm sd %.2f%%  noon off the line %.3f%%\n", (unsigned long long)seed, (long long)(mn / 100), (long long)(mx / 100), s * 100, s * std::sqrt(365) * 100, maxmove / 100.0, std::sqrt(sp_calm / std::max(1, n_calm)) * 100, std::sqrt(sp_storm / std::max(1, n_storm)) * 100, mid_dev / n * 100);
		CHECK(s > 0.006 && s < 0.014);                         // 0.6% .. 1.4% a day, like real gold
		CHECK(mn > 200'000 * 40 / 100 && mx < 200'000 * 250 / 100); // the weak pull keeps it from running away for good
		CHECK(mx - mn > 200'000 * 15 / 100);                    // but it really moves
		CHECK(std::abs(m) < 0.0015);                            // no steady push up or down
		CHECK(n_calm > 0 && n_storm > 0 && std::sqrt(sp_storm / n_storm) > 1.8 * std::sqrt(sp_calm / n_calm)); // calm and stormy spells exist
		CHECK(mid_dev / n > 0.002);                             // a real path through the day, not a straight line
	}

	/* Smooth volatility: no step at the block change, always between calm (0.6) and stormy (2.2). */
	{
		const int64_t block = VOL_BLOCK_DAYS * 1440; int64_t biggest = 0;
		for (uint64_t seed = 1; seed <= 20; seed++) for (int64_t m = 1; m < block * 200; m++) {
			const int64_t a = SmoothVolatilityPpm(seed, m - 1), b = SmoothVolatilityPpm(seed, m);
			CHECK(b >= 600'000 && b <= 2'200'000);
			biggest = std::max<int64_t>(biggest, std::llabs(b - a));
		}
		std::printf("volatility: biggest change in one game minute %lld ppm (of 1000000)\n", (long long)biggest);
		CHECK(biggest < 1'600'000 / (VOL_BLOCK_DAYS * 1440) + 2);
		CHECK(SmoothVolatilityPpm(7, 0) == VolatilityPpm(7, 0)); // at the start of a spell it is the value of the spell
	}

	/* Jumps: about one in JUMP_ONE_IN_STEPS steps, 2% .. 4.5% x spell, both directions. */
	{
		int64_t jumps = 0, up = 0; const int64_t N = 720 * 365 * 40; int64_t biggest = 0;
		for (int64_t m = 1; m <= N; m++) {
			const int64_t v = StepNoisePpm(5, m * 2);
			if (std::llabs(v) >= JUMP_MIN_PPM * 55 / 100) { jumps++; if (v > 0) up++; biggest = std::max<int64_t>(biggest, std::llabs(v)); }
		}
		const double per_year = double(jumps) / 40;
		std::printf("jumps: %.2f a year (%lld up of %lld), biggest %.2f%%\n", per_year, (long long)up, (long long)jumps, biggest / 10'000.0);
		CHECK(per_year > 2.0 && per_year < 6.5);
		CHECK(up > jumps * 35 / 100 && up < jumps * 65 / 100);
		CHECK(biggest <= (JUMP_MIN_PPM + JUMP_SPAN_PPM) * 22 / 10 / 1 + 3 * STEP_SIGMA_DAY_PPM * 22 / 10 / STEP_SIGMA_DIVISOR + 1);
	}

	/* The impact of a trade fades: always towards 0, never past it, half after about 1.6 game days. */
	{
		CHECK(FadedImpact(0) == 0);
		for (int64_t v : {1ll, -1ll, 5ll, -5ll, 123456ll, -123456ll}) {
			int64_t x = v; int steps = 0;
			while (x != 0 && steps < 10'000'000) { const int64_t n = FadedImpact(x); CHECK((v > 0 ? n >= 0 && n < x : n <= 0 && n > x)); x = n; steps++; }
			CHECK(x == 0);
		}
		int64_t x = 100'000; int steps = 0; while (x > 50'000) { x = FadedImpact(x); steps++; }
		const double days = steps * 2.0 / 1440;
		std::printf("trade impact: half gone after %.2f game days\n", days);
		CHECK(days > 1.4 && days < 1.8);
	}

	/* The cash average: the real cash jumps by +-5% from day to day, the average that the price sees moves far less, and it still follows a lasting change. */
	{
		CHECK(SmoothedCash(0, 1'000'000) == 1'000'000 && SmoothedCash(-5, 700) == 700 && SmoothedCash(0, -9) == 0);
		int64_t avg = 0, mn = INT64_MAX, mx = 0; double worst_step = 0;
		for (int64_t m = 0; m < 720 * 400; m++) {
			const int64_t day = m / 720;
			const int64_t cash = 10'000'000 + (int64_t)(Gauss(Hash(9, day, 0xCA5)) * 500'000 / PPM); // sd 5% a day, constant for the day
			const int64_t before = avg; avg = SmoothedCash(avg, cash);
			if (before > 0) worst_step = std::max(worst_step, std::abs(double(avg - before)) / before);
			if (day > 20) { mn = std::min(mn, avg); mx = std::max(mx, avg); }
		}
		std::printf("cash average: range %.2f%% .. %.2f%% of 10 million with sd 5%% a day, biggest 2 minute step %.4f%%\n", (mn - 1e7) / 1e5, (mx - 1e7) / 1e5, worst_step * 100);
		CHECK(mx - mn < 10'000'000 * 10 / 100 && worst_step < 0.0003); // over 380 days the average stays within about +-4.5% (the cash alone swings 5% EVERY day)
		int64_t a2 = 10'000'000; for (int64_t m = 0; m < 720 * 30; m++) a2 = SmoothedCash(a2, 20'000'000);
		std::printf("after 30 days of a doubled cash the average is at %.0f%%\n", a2 / 1e5);
		CHECK(a2 > 19'000'000 && a2 <= 20'000'000);              // a lasting change comes through
		int64_t a3 = 1000; for (int i = 0; i < 100'000; i++) a3 = SmoothedCash(a3, 1003); CHECK(a3 == 1003); // no stall on a tiny gap
	}

	/* A change of the level moves the price at once and by the same ratio (news, a trade, inflation). */
	{
		int64_t last = 0; int64_t price = 200'000;
		price = Step(price, 5, 2, 200'000, last);                 // first step: only learns the level
		const int64_t before = price;
		price = Step(price, 5, 4, 200'000 * 92 / 100, last);      // news: the level falls 8%
		const double ratio = double(price) / before;
		std::printf("level -8%% moves the price by %.2f%% in one step\n", (ratio - 1) * 100);
		CHECK(ratio < 0.935 && ratio > 0.905);
		int64_t l2 = 0, p2 = Step(200'000, 5, 2, 0, l2);          // level 0 (not known): no jump
		CHECK(l2 == 0 && std::abs(p2 - 200'000) < 2'000);
	}

	/* The pull is weak: a price 20% over the level is still about 19% over it after one day, and 50% of the gap is gone after about 4.5 months. */
	{
		CHECK(RevertedPrice(240'000, 200'000) == 240'000 - 200);
		CHECK(RevertedPrice(200'000, 200'000) == 200'000 && RevertedPrice(5, 0) == 5);
		int64_t p = 240'000; int days = 0; while (p > 220'000) { p = RevertedPrice(p, 200'000); days++; }
		std::printf("half of a +20%% gap is gone after %d days\n", days);
		CHECK(days > 120 && days < 160);
	}
	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
