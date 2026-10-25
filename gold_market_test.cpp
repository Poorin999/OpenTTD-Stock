/* Stand-alone test of gold_market_math.h (not part of the game build).
 * Build and run:  g++ -std=c++17 -Wall -Wextra -o /tmp/gmt gold_market_test.cpp && /tmp/gmt */
#include <cstdio>
#include <cmath>
#include <vector>
#include "gold_market_math.h"
using namespace goldmath;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* Same as GoldReserveManager::GlideCents with the noise of the day. */
static int64_t Glide(int64_t start, int64_t target, int64_t minute, int64_t noise)
{
	int64_t step = (target - start) * 5 / 100;
	if (step == 0 && target != start) step = target > start ? 1 : -1;
	int64_t end = (start + step) * (PPM + noise) / PPM;
	return std::max<int64_t>(1, start + (end - start) * minute / 1440);
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

	/* The price over 5 game years: target = line x (1 + cycle); price glides 5%/day with the noise. */
	for (uint64_t seed : {1ull, 2ull, 3ull, 4ull}) {
		int64_t price = 200'000; double ssum = 0, ssq = 0; int64_t mn = price, mx = price, maxmove = 0; int n = 0;
		for (int64_t d = 0; d < 1825; d++) {
			const int64_t target = 200'000 * (PPM + CyclePpm(seed, d)) / PPM;
			const int64_t next = Glide(price, target, 1440, DailyNoisePpm(seed, d));
			const double r = double(next - price) / price;
			ssum += r; ssq += r * r; n++; maxmove = std::max<int64_t>(maxmove, std::llabs(next - price) * 10000 / price);
			price = next; mn = std::min(mn, price); mx = std::max(mx, price);
		}
		const double m = ssum / n, s = std::sqrt(ssq / n - m * m);
		std::printf("seed %llu: range %lld..%lld  daily sd %.2f%%  annualised %.1f%%  max day move %.2f%%\n", (unsigned long long)seed, (long long)(mn / 100), (long long)(mx / 100), s * 100, s * std::sqrt(365) * 100, maxmove / 100.0);
		CHECK(s > 0.004 && s < 0.014);              // 0.4% .. 1.4% a day
		CHECK(mn > 200'000 * 70 / 100 && mx < 200'000 * 135 / 100); // inside +-30%
		CHECK(mx - mn > 200'000 * 12 / 100);          // but it does move: bull and bear
	}
	/* The glide inside the day is monotonic and ends at the day-end value. */
	{
		int64_t prev = 200'000; bool mono = true;
		for (int m = 1; m <= 1440; m++) { int64_t v = Glide(200'000, 230'000, m, 4'000); if (v < prev) mono = false; prev = v; }
		CHECK(mono && prev == Glide(200'000, 230'000, 1440, 4'000) && Glide(200'000, 200'000, 0, 5'000) == 200'000);
	}
	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
