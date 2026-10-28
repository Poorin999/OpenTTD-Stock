/* Stand-alone test of zone_market_math.h (not part of the game build).
 * Build and run:  g++ -std=c++17 -Wall -Wextra -o /tmp/zmt zone_market_test.cpp && /tmp/zmt */
#include <cstdio>
#include <cmath>
#include "zone_market_math.h"
using namespace zonemath;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main()
{
	/* Capitals: the biggest town first, then far and big towns; none closer than MIN_CAPITAL_DISTANCE; no town = none. */
	{
		CHECK(PickCapitals({}, {}, 3).empty());
		std::vector<Pt> pts = {{10, 10}, {20, 12}, {200, 30}, {60, 200}, {205, 35}, {30, 30}};
		std::vector<int64_t> pop = {5000, 900, 3000, 1500, 400, 100};
		const std::vector<int> c = PickCapitals(pts, pop, 3);
		CHECK(c.size() == 3 && c[0] == 0 && c[1] == 2 && c[2] == 3);
		for (size_t i = 0; i < c.size(); i++) for (size_t j = i + 1; j < c.size(); j++) CHECK(Distance(pts[c[i]], pts[c[j]]) >= MIN_CAPITAL_DISTANCE);
		CHECK(PickCapitals(pts, pop, 3) == c); // deterministic
		/* All towns close together: one capital only. */
		std::vector<Pt> near_pts = {{10, 10}, {12, 11}, {15, 9}};
		CHECK(PickCapitals(near_pts, {100, 200, 300}, 3).size() == 1);
		CHECK(PickCapitals(pts, pop, 99).size() <= static_cast<size_t>(MAX_ZONES));
	}
	/* Zone of a point: the nearest capital, the first on a tie. */
	{
		const std::vector<Pt> caps = {{10, 10}, {100, 10}};
		CHECK(ZoneOf({20, 20}, caps) == 0 && ZoneOf({90, 5}, caps) == 1 && ZoneOf({55, 10}, caps) == 0);
		CHECK(ZoneOf({1, 1}, {}) == -1);
	}
	/* Level: an equal share is 1.0, bigger is stronger, limits hold. */
	{
		CHECK(LevelPpm(500, 1500, 3) == PPM - 1 || LevelPpm(500, 1500, 3) == PPM || LevelPpm(500, 1500, 3) == PPM + 1);
		CHECK(LevelPpm(1000, 1500, 3) > PPM && LevelPpm(100, 1500, 3) < PPM);
		CHECK(LevelPpm(0, 1500, 3) == LEVEL_MIN_PPM && LevelPpm(1500, 1500, 3) == LEVEL_MAX_PPM);
		CHECK(LevelPpm(5, 0, 3) == PPM && LevelPpm(5, 10, 0) == PPM);
	}
	/* The rate: deterministic, bounded, mean reverting, moves like 0.6% a day with a pull. */
	{
		for (uint64_t seed = 1; seed <= 5; seed++) {
			int64_t rate = PPM; double sum = 0, sq = 0; int64_t mn = rate, mx = rate;
			const int D = 365 * 20;
			for (int64_t d = 1; d <= D; d++) {
				const int64_t before = rate;
				rate = NextRate(seed, 1, d, rate, 1'200'000);
				CHECK(rate == NextRate(seed, 1, d, before, 1'200'000));
				CHECK(rate >= RATE_MIN_PPM && rate <= RATE_MAX_PPM);
				mn = std::min(mn, rate); mx = std::max(mx, rate);
				if (d > 365) { sum += rate; sq += double(rate) * rate; }
			}
			const double n = D - 365, mean = sum / n, sd = std::sqrt(sq / n - mean * mean);
			std::printf("seed %llu: level 1.20, mean %.3f, sd %.3f, range %.2f..%.2f\n", (unsigned long long)seed, mean / PPM, sd / PPM, mn / double(PPM), mx / double(PPM));
			CHECK(mean > 1'100'000 && mean < 1'300'000); // follows its level
			CHECK(sd > 20'000 && sd < 250'000);          // but it moves
		}
		CHECK(NextRate(1, 0, 5, PPM, PPM) != NextRate(1, 1, 5, PPM, PPM) || NextRate(1, 0, 6, PPM, PPM) != NextRate(1, 1, 6, PPM, PPM)); // zones differ
		CHECK(NextRate(1, 0, 5, 300'000, 1'600'000) > 300'000);
		CHECK(NextRate(1, 0, 5, 3'900'000, 600'000) < 3'900'000);
	}
	/* The fee: 1% for equal rates, grows with the difference, 12% at most, symmetric, no fee for a bad rate. */
	{
		CHECK(FeePpm(PPM, PPM) == FEE_BASE_PPM);
		CHECK(FeePpm(PPM, 1'200'000) > FEE_BASE_PPM && FeePpm(PPM, 1'200'000) == FeePpm(1'200'000, PPM));
		CHECK(FeePpm(PPM, 2'000'000) > FeePpm(PPM, 1'200'000));
		CHECK(FeePpm(RATE_MIN_PPM, RATE_MAX_PPM) <= FEE_MAX_PPM && FeePpm(RATE_MIN_PPM, RATE_MAX_PPM) > FEE_BASE_PPM);
		CHECK(FeePpm(0, PPM) == 0 && FeePpm(PPM, -1) == 0);
		CHECK(FeeAmount(1'000'000, 10'000) == 10'000 && FeeAmount(0, 10'000) == 0 && FeeAmount(-5, 10'000) == 0 && FeeAmount(100, 0) == 0);
		CHECK(FeeAmount(4'000'000'000'000'000'000LL, FEE_MAX_PPM) == 480'000'000'000'000'000LL); // no overflow
		CHECK(FeeAmount(123'456, 12'345) == 123'456LL * 12'345 / PPM);
	}
	/* Step C: inflation, policy rate, banknotes. */
	{
		int64_t infl = 500'000, pol = 0;
		for (int64_t d = 0; d < 3000; d++) {
			infl = NextInflation(7, 2, d, infl); pol = NextPolicy(pol, infl);
			CHECK(infl >= INFL_MIN_PPM && infl <= INFL_MAX_PPM && pol >= POLICY_MIN_PPM && pol <= POLICY_MAX_PPM);
		}
		CHECK(infl < 100'000);                                        // pulled back to about 2%
		CHECK(NextInflation(7, 2, 9, 30'000) == NextInflation(7, 2, 9, 30'000));
		CHECK(DailyInterest(365'000'000, 36'500) == 36'500 * 365'000'000LL / PPM / 365 && DailyInterest(0, 1000) == 0 && DailyInterest(5, 0) == 0);
		CHECK(NotesForMoney(1'000'000, 2'000'000) == 500'000 && MoneyForNotes(500'000, 2'000'000) == 1'000'000);
		CHECK(AfterSpread(1'000'000) == 997'000 && AfterSpread(-1) == 0);
		const int64_t notes = AfterSpread(NotesForMoney(1'000'000, 1'250'000)), back = AfterSpread(MoneyForNotes(notes, 1'250'000));
		CHECK(back < 1'000'000 && back > 990'000);                    // a round trip costs about 0.6%
		CHECK(RateAfterTrade(PPM, true, 10) > PPM && RateAfterTrade(PPM, false, 10) < PPM && RateAfterTrade(PPM, true, 1'000'000'000) == PPM + PPM / 50);
		CHECK(RateAfterTrade(RATE_MAX_PPM, true, 1000) == RATE_MAX_PPM);
	}
	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
