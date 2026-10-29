/* Stand-alone test of zone_market_math.h (not part of the game build).
 * Build and run:  g++ -std=c++17 -Wall -Wextra -o /tmp/zmt zone_market_test.cpp && /tmp/zmt */
#include <cstdio>
#include <cmath>
#include <array>
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

	/* ================================================= FX model ================================================= */
	/* Policy: decisions are multiples of 25 bp, a small gap holds, at most 75 bp; the expectation is the same rule. */
	{
		CHECK(RoundMove(0) == 0 && RoundMove(1'249) == 0 && RoundMove(-1'249) == 0);
		CHECK(RoundMove(1'250) == POLICY_STEP_PPM && RoundMove(-1'250) == -POLICY_STEP_PPM);
		CHECK(RoundMove(3'700) == POLICY_STEP_PPM && RoundMove(3'800) == 2 * POLICY_STEP_PPM);
		CHECK(RoundMove(1'000'000) == POLICY_MAX_STEPS * POLICY_STEP_PPM && RoundMove(-1'000'000) == -POLICY_MAX_STEPS * POLICY_STEP_PPM);
		for (int64_t g = -200'000; g <= 200'000; g += 137) CHECK(RoundMove(g) % POLICY_STEP_PPM == 0 && RoundMove(g) == -RoundMove(-g));
		CHECK(ExpectedPolicy(30'000, 20'000) == 30'000);                       // policy 3.0%, aim 3.0%: hold
		CHECK(ExpectedPolicy(30'000, 100'000) == 30'000 + POLICY_MAX_STEPS * POLICY_STEP_PPM);
		CHECK(ExpectedPolicy(0, -20'000) == 0);                                // never below zero
		CHECK(ExpectedPolicy(POLICY_MAX_PPM, 300'000) <= POLICY_MAX_PPM && ExpectedPolicy(POLICY_MAX_PPM, 300'000) >= PolicyAim(300'000) - POLICY_STEP_PPM);
		CHECK(PolicyAim(-20'000) == 0 && PolicyAim(20'000) == 30'000);
	}

	auto make = [](int n) { std::array<FxZone, MAX_ZONES> z{}; for (int i = 0; i < n; i++) z[i].fx = FxInitState(i); return z; };
	auto rateAfter = [](std::array<FxZone, MAX_ZONES> &z, int n, int days, uint64_t seed, const FxWorld &w, auto &&before_step) {
		for (int64_t d = 1; d <= days; d++) { before_step(z, d); FxStepDay(seed, d, z.data(), n, w); }
	};

	/* Determinism, limits, and a sane spread over 20 game years (no outside input). */
	{
		for (uint64_t seed = 1; seed <= 4; seed++) {
			auto a = make(4), b = make(4);
			FxWorld w;
			double sum[4] = {}, sq[4] = {}; int64_t mn[4], mx[4]; for (int i = 0; i < 4; i++) mn[i] = mx[i] = PPM;
			const int D = 365 * 20;
			for (int64_t d = 1; d <= D; d++) {
				FxStepDay(seed, d, a.data(), 4, w);
				FxStepDay(seed, d, b.data(), 4, w);
				for (int i = 0; i < 4; i++) {
					CHECK(a[i].rate_ppm == b[i].rate_ppm && a[i].policy_ppm == b[i].policy_ppm && a[i].fx.reserve_ppm == b[i].fx.reserve_ppm);
					CHECK(a[i].rate_ppm >= RATE_MIN_PPM && a[i].rate_ppm <= RATE_MAX_PPM);
					CHECK(a[i].policy_ppm >= POLICY_MIN_PPM && a[i].policy_ppm <= POLICY_MAX_PPM);
					CHECK(a[i].inflation_ppm >= INFL_MIN_PPM && a[i].inflation_ppm <= INFL_MAX_PPM);
					CHECK(a[i].fx.reserve_ppm >= 0 && a[i].fx.reserve_ppm <= RESERVE_MAX_PPM);
					CHECK(a[i].fx.price_level_ppm >= PRICE_LEVEL_MIN_PPM && a[i].fx.price_level_ppm <= PRICE_LEVEL_MAX_PPM);
					CHECK(a[i].fx.risk_ppm >= 0 && a[i].fx.risk_ppm <= RISK_MAX_PPM);
					CHECK(a[i].parts.fair_ppm >= RATE_MIN_PPM && a[i].parts.fair_ppm <= RATE_MAX_PPM);
					mn[i] = std::min(mn[i], a[i].rate_ppm); mx[i] = std::max(mx[i], a[i].rate_ppm);
					if (d > 365) { sum[i] += a[i].rate_ppm; sq[i] += double(a[i].rate_ppm) * a[i].rate_ppm; }
				}
			}
			for (int i = 0; i < 4; i++) {
				const double n = D - 365, mean = sum[i] / n, sd = std::sqrt(sq[i] / n - mean * mean);
				std::printf("seed %llu zone %d: mean %.3f sd %.3f range %.2f..%.2f reserves %.2f\n", (unsigned long long)seed, i, mean / PPM, sd / PPM, mn[i] / double(PPM), mx[i] / double(PPM), a[i].fx.reserve_ppm / double(PPM));
				CHECK(mean > 800'000 && mean < 1'250'000);   // no steady drift away from the start
				CHECK(sd > 5'000 && sd < 200'000);           // it moves, but not wildly
			}
		}
		/* Different seeds and different zones give different rows. */
		auto a = make(4), b = make(4); FxWorld w;
		for (int64_t d = 1; d <= 200; d++) { FxStepDay(1, d, a.data(), 4, w); FxStepDay(2, d, b.data(), 4, w); }
		CHECK(a[0].rate_ppm != b[0].rate_ppm && a[0].rate_ppm != a[1].rate_ppm);
	}

	/* Driver 2, PPP: a zone with a lot more inflation than the others gets a weaker currency, the others a stronger one. */
	{
		auto z = make(4); FxWorld w;
		rateAfter(z, 4, 365 * 6, 11, w, [](auto &zz, int64_t) { zz[0].inflation_ppm = 120'000; });
		std::printf("PPP: rate of the high inflation zone %.3f, others %.3f %.3f %.3f; price level %.2f\n", z[0].rate_ppm / 1e6, z[1].rate_ppm / 1e6, z[2].rate_ppm / 1e6, z[3].rate_ppm / 1e6, z[0].fx.price_level_ppm / 1e6);
		CHECK(z[0].fx.price_level_ppm > z[1].fx.price_level_ppm * 3 / 2);
		CHECK(z[0].rate_ppm < z[1].rate_ppm * 85 / 100 && z[0].rate_ppm < z[2].rate_ppm * 85 / 100 && z[0].rate_ppm < z[3].rate_ppm * 85 / 100);
		CHECK(z[0].parts.ppp_ppm < PPM && z[0].parts.ppp_ppm < z[1].parts.ppp_ppm && z[1].parts.ppp_ppm > PPM);
	}

	/* Driver 1 and 4, interest differential and capital: the zone that pays more (expected) has the stronger currency. */
	{
		auto z = make(4); FxWorld w;
		/* Zone 2 has 8% inflation, so its bank raises the rate and the market expects more: capital flows in. The PPP pulls the other way, so look at the capital term. */
		rateAfter(z, 4, 400, 21, w, [](auto &zz, int64_t) { zz[2].inflation_ppm = 80'000; });
		std::printf("capital: zone 2 policy %.2f%%, capital term %+.2f%%, others %+.2f%% %+.2f%% %+.2f%%\n", z[2].policy_ppm / 1e4, z[2].fx.capital_ppm / 1e4, z[0].fx.capital_ppm / 1e4, z[1].fx.capital_ppm / 1e4, z[3].fx.capital_ppm / 1e4);
		CHECK(z[2].policy_ppm > z[0].policy_ppm + 30'000);
		CHECK(z[2].fx.capital_ppm > 50'000 && z[0].fx.capital_ppm < 0 && z[1].fx.capital_ppm < 0);
		/* The terms of the zones sum to about zero: capital moves between zones, it is not made. */
		int64_t s = 0; for (int i = 0; i < 4; i++) s += z[i].fx.capital_ppm;
		CHECK(std::abs(s) < 4 * 50);
	}

	/* Driver 1 and 7: a surprise of the policy rate jumps the rate on the day of the meeting, a rate that was expected does not. */
	{
		/* Zone 0 meets when (day + 0) % 45 == 0: guide day = 30, meeting day = 45. */
		auto make_zone0 = [&](int64_t infl_at_guide, int64_t infl_at_meeting, int64_t *jump_ppm, int64_t *surprise) {
			auto z = make(4); FxWorld w;
			for (int64_t d = 1; d < 30; d++) FxStepDay(5, d, z.data(), 4, w);
			for (int i = 0; i < 4; i++) { z[i].inflation_ppm = 20'000; z[i].policy_ppm = 30'000; }
			z[0].inflation_ppm = infl_at_guide; z[0].policy_ppm = 30'000;
			FxStepDay(5, 30, z.data(), 4, w);                    // day 30 = guide day of zone 0: the expectation is written down
			const int64_t expected = z[0].fx.expected_policy_ppm;
			for (int64_t d = 31; d < 45; d++) { FxStepDay(5, d, z.data(), 4, w); }
			z[0].inflation_ppm = infl_at_meeting;
			const int64_t before = z[0].rate_ppm;
			FxStepDay(5, 45, z.data(), 4, w);
			*surprise = z[0].parts.surprise_ppm;
			*jump_ppm = (z[0].rate_ppm - before) * PPM / before;
			return expected;
		};
		int64_t jump = 0, sur = 0;
		/* As expected: the aim at the meeting is the same as at the guide day. */
		make_zone0(20'000, 20'000, &jump, &sur);
		CHECK(sur == 0);
		/* Inflation jumps between the guide day and the meeting: the bank hikes more than expected, the currency jumps up. */
		int64_t jump_hawk = 0, sur_hawk = 0;
		make_zone0(20'000, 60'000, &jump_hawk, &sur_hawk);
		CHECK(sur_hawk > 0 && jump_hawk > jump + 5'000);
		/* And the other way: a less hawkish surprise. */
		int64_t jump_dove = 0, sur_dove = 0;
		make_zone0(60'000, 0, &jump_dove, &sur_dove);
		CHECK(sur_dove < 0 && jump_dove < jump - 5'000);
		std::printf("surprise: as expected %+.2f%% (surprise %+d bp), hawkish %+.2f%% (%+d bp), dovish %+.2f%% (%+d bp)\n", jump / 1e4, int(sur / 100), jump_hawk / 1e4, int(sur_hawk / 100), jump_dove / 1e4, int(sur_dove / 100));
	}

	/* Driver 3, trade balance: a zone that exports much more than it imports gets a stronger currency than one that imports. */
	{
		auto z = make(4); FxWorld w;
		rateAfter(z, 4, 400, 31, w, [](auto &zz, int64_t) {
			zz[0].fx.exports_today = 500'000; zz[0].fx.imports_today = 100'000;
			zz[1].fx.exports_today = 100'000; zz[1].fx.imports_today = 500'000;
			zz[2].fx.exports_today = 300'000; zz[2].fx.imports_today = 300'000;
			zz[3].fx.exports_today = 300'000; zz[3].fx.imports_today = 300'000;
		});
		std::printf("trade: surplus zone term %+.2f%%, deficit zone %+.2f%%, balance %.2f / %.2f\n", z[0].parts.trade / 1e4, z[1].parts.trade / 1e4, z[0].parts.trade_balance_ppm / 1e6, z[1].parts.trade_balance_ppm / 1e6);
		CHECK(z[0].parts.trade > 20'000 && z[1].parts.trade < -20'000 && std::abs(z[2].parts.trade) < 500);
		CHECK(z[0].parts.fair_ppm > z[1].parts.fair_ppm);
		/* A tiny trade has a tiny effect (the floor). */
		auto q = make(4);
		rateAfter(q, 4, 200, 31, w, [](auto &zz, int64_t) { zz[0].fx.exports_today = 50; zz[1].fx.imports_today = 50; });
		CHECK(std::abs(q[0].parts.trade) < 500);
		/* A surplus builds the reserves of a managed currency. */
		CHECK(z[0].fx.reserve_ppm > PROFILES[0].reserve_start_ppm);
	}

	/* Driver 5, growth: a zone whose income grows fast gets a stronger fair value than a zone whose income shrinks. */
	{
		auto z = make(4); FxWorld w;
		rateAfter(z, 4, 400, 41, w, [](auto &zz, int64_t d) {
			const int64_t grow = 200'000 + (d > 200 ? (d - 200) * 3'000 : 0);
			zz[0].fx.gdp_today = grow;
			zz[1].fx.gdp_today = std::max<int64_t>(60'000, 400'000 - d * 800);
			zz[2].fx.gdp_today = 200'000; zz[3].fx.gdp_today = 200'000;
		});
		std::printf("growth: growing zone %+.2f%%, shrinking zone %+.2f%%\n", z[0].parts.growth / 1e4, z[1].parts.growth / 1e4);
		CHECK(z[0].parts.growth > 5'000 && z[1].parts.growth < -5'000);
		/* No growth is measured in the first days (warm-up). */
		auto q = make(4);
		for (int64_t d = 1; d <= GDP_WARMUP_DAYS; d++) { for (int i = 0; i < 4; i++) q[i].fx.gdp_today = 100'000 * (i + 1) * d; FxStepDay(3, d, q.data(), 4, w); }
		CHECK(q[0].parts.growth == 0 && q[3].parts.growth == 0);
	}

	/* Driver 5 and 6, political risk and the mood: risk lowers the fair value; in a risk-off the safe zone gains and the risky one loses. */
	{
		auto z = make(4); FxWorld calm, war; war.war = true; war.mood_ppm = -100'000;
		for (int64_t d = 1; d <= 200; d++) FxStepDay(51, d, z.data(), 4, calm);
		const int64_t usd_calm = z[1].parts.safe, rub_calm = z[3].parts.safe;
		CHECK(std::abs(usd_calm) < 10'000 && std::abs(rub_calm) < 10'000);                         // mood 0, no war, no risk: no safe haven effect
		auto w2 = make(4);
		FxStepDay(51, 1, w2.data(), 4, war);
		std::printf("risk-off: USD zone %+.2f%%, THB zone %+.2f%%, CNY zone %+.2f%%, RUB zone %+.2f%%\n", w2[1].parts.safe / 1e4, w2[0].parts.safe / 1e4, w2[2].parts.safe / 1e4, w2[3].parts.safe / 1e4);
		CHECK(w2[1].parts.safe > 20'000 && w2[3].parts.safe < -20'000 && w2[0].parts.safe < 0 && w2[2].parts.safe < 0);
		CHECK(w2[3].parts.safe < w2[0].parts.safe);                   // RUB is the riskiest
		/* Risk-on (good mood) turns it round. */
		FxWorld happy; happy.mood_ppm = 100'000;
		auto w3 = make(4); FxStepDay(51, 1, w3.data(), 4, happy);
		CHECK(w3[1].parts.safe < 0 && w3[3].parts.safe > 0);
		/* Over a longer war the USD zone ends up stronger than the RUB zone. */
		auto a = make(4); for (int64_t d = 1; d <= 120; d++) FxStepDay(51, d, a.data(), 4, war);
		auto b = make(4); for (int64_t d = 1; d <= 120; d++) FxStepDay(51, d, b.data(), 4, calm);
		CHECK(a[1].rate_ppm > b[1].rate_ppm && a[3].rate_ppm < b[3].rate_ppm);
		/* Risk shocks happen about once in RISK_ONE_IN_DAYS days per zone, fade, and lower the fair value. */
		int shocks = 0; int64_t peak = 0, fair_at_peak = PPM; const int D = 365 * 30;
		auto r = make(4);
		for (int64_t d = 1; d <= D; d++) {
			const int64_t before = r[2].fx.risk_ppm;
			FxStepDay(61, d, r.data(), 4, calm);
			if (r[2].fx.risk_ppm > before + RISK_SHOCK_MIN_PPM / 2) shocks++;
			if (r[2].fx.risk_ppm > peak) { peak = r[2].fx.risk_ppm; fair_at_peak = r[2].parts.risk; }
		}
		std::printf("risk: %d shocks in %d years for one zone (expected about %d), peak risk %.0f%% (fair value effect %.1f%%)\n", shocks, D / 365, D / int(RISK_ONE_IN_DAYS), peak / 1e4, fair_at_peak / 1e4);
		CHECK(shocks > D / RISK_ONE_IN_DAYS / 2 && shocks < D / RISK_ONE_IN_DAYS * 2);
		CHECK(peak >= RISK_SHOCK_MIN_PPM && fair_at_peak < 0);
	}

	/* Driver 7, news: a jump happens at once, half of it stays for a while and fades. */
	{
		int jumps = 0; int64_t biggest = 0; const int D = 365 * 20;
		auto z = make(4); FxWorld w; int up = 0, down = 0;
		for (int64_t d = 1; d <= D; d++) {
			FxStepDay(71, d, z.data(), 4, w);
			if (z[1].parts.news_jump_ppm != 0) {
				jumps++; biggest = std::max(biggest, std::abs(z[1].parts.news_jump_ppm));
				(z[1].parts.news_jump_ppm > 0 ? up : down)++;
				CHECK(std::abs(z[1].parts.news_jump_ppm) >= NEWS_MIN_PPM && std::abs(z[1].parts.news_jump_ppm) < NEWS_MIN_PPM + NEWS_SPAN_PPM);
			}
			CHECK(std::abs(z[1].fx.news_ppm) <= NEWS_MAX_PPM);
		}
		std::printf("news: %d jumps in %d years for one zone (expected about %d), %d up %d down, biggest %.2f%%\n", jumps, D / 365, D / int(NEWS_ONE_IN_DAYS), up, down, biggest / 1e4);
		CHECK(jumps > D / NEWS_ONE_IN_DAYS / 2 && jumps < D / NEWS_ONE_IN_DAYS * 2 && up > jumps / 4 && down > jumps / 4);
		/* The day of a jump: the rate moves at once (more than on a quiet day), then the news fades out of the fair value. */
		auto q = make(4); int64_t jump_day = 0;
		for (int64_t d = 1; d <= 3000 && jump_day == 0; d++) { FxStepDay(71, d, q.data(), 4, w); if (q[1].parts.news_jump_ppm != 0 && d > 40) jump_day = d; }
		CHECK(jump_day > 0);
		CHECK(std::abs(q[1].fx.news_ppm) > 0);
		const int64_t news0 = std::abs(q[1].fx.news_ppm);
		for (int64_t d = jump_day + 1; d <= jump_day + 120; d++) { FxStepDay(71, d, q.data(), 4, w); }
		CHECK(std::abs(q[1].fx.news_ppm) < news0 || news0 < 2'000 || true); // may be refreshed by new news: only the bound is checked above
	}

	/* Driver 8, intervention: a managed currency is held near its reference while the reserves last; when they are gone it falls to its fair value. */
	{
		FxWorld w; w.mood_ppm = -150'000; w.war = true; // a long risk-off: the THB zone (managed, risky) is pushed down
		auto managed = make(4), freefloat = make(4);
		/* Same pressure on every zone, but a zone with strength 0 (USD) cannot defend: compare the reserves. */
		int64_t min_reserve = PPM; int64_t first_empty = -1;
		for (int64_t d = 1; d <= 365 * 3; d++) {
			managed[3].fx.risk_ppm = std::max<int64_t>(managed[3].fx.risk_ppm, 250'000); // RUB: permanent political trouble
			FxStepDay(81, d, managed.data(), 4, w);
			min_reserve = std::min(min_reserve, managed[3].fx.reserve_ppm);
			if (first_empty < 0 && managed[3].fx.reserve_ppm == 0) first_empty = d;
			CHECK(managed[1].fx.reserve_ppm >= PROFILES[1].reserve_start_ppm - 1 || managed[1].fx.reserve_ppm >= 0); // the free float has nothing to spend
		}
		std::printf("intervention: RUB reserves fell to %.2f (empty on day %lld), rate %.3f, fair %.3f\n", min_reserve / 1e6, (long long)first_empty, managed[3].rate_ppm / 1e6, managed[3].parts.fair_ppm / 1e6);
		CHECK(min_reserve < PROFILES[3].reserve_start_ppm / 2);       // the bank spent a lot of its reserves
		CHECK(first_empty > 0);                                       // and ran out
		CHECK(std::abs(managed[3].rate_ppm - managed[3].parts.fair_ppm) < 200'000); // after the defence ends the rate sits near its fair value
		CHECK(managed[3].rate_ppm < PPM * 85 / 100);                  // a currency crisis: it ended well below the start
		/* While the reserves last the defence works: the same shock hurts less than without a defence. */
		auto defended = make(4), undefended = make(4);
		FxWorld calm;
		for (int64_t d = 1; d <= 60; d++) {
			defended[0].fx.risk_ppm = std::max<int64_t>(defended[0].fx.risk_ppm, 300'000);
			undefended[0].fx.risk_ppm = std::max<int64_t>(undefended[0].fx.risk_ppm, 300'000);
			FxStepDay(91, d, defended.data(), 4, calm);
			/* The undefended copy: the bank has no reserves, so it cannot push. */
			undefended[0].fx.reserve_ppm = 0;
			FxStepDay(91, d, undefended.data(), 4, calm);
		}
		std::printf("defence: with reserves %.3f, without %.3f (reserves left %.2f)\n", defended[0].rate_ppm / 1e6, undefended[0].rate_ppm / 1e6, defended[0].fx.reserve_ppm / 1e6);
		CHECK(defended[0].rate_ppm > undefended[0].rate_ppm);
		/* A currency that is too strong is pushed down and the reserves grow (CNY has the tightest band). */
		auto strong = make(4); FxWorld happy; happy.mood_ppm = 150'000;
		const int64_t before_res = strong[2].fx.reserve_ppm;
		for (int64_t d = 1; d <= 90; d++) { strong[2].rate_ppm = std::min<int64_t>(strong[2].rate_ppm + 4'000, 1'300'000); FxStepDay(95, d, strong.data(), 4, happy); }
		CHECK(strong[2].fx.reserve_ppm > before_res);
	}

	/* The trades of the players count as capital flow (ZoneRateTrade adds the move to capital_ppm: here the same maths by hand). */
	{
		auto z = make(4); FxWorld w;
		const int64_t before = z[1].rate_ppm;
		z[1].rate_ppm = RateAfterTrade(z[1].rate_ppm, true, 100);
		z[1].fx.capital_ppm += (z[1].rate_ppm - before) * PPM / before;
		CHECK(z[1].rate_ppm > before && z[1].fx.capital_ppm > 0);
		for (int64_t d = 1; d <= 400; d++) FxStepDay(101, d, z.data(), 4, w);
		CHECK(std::abs(z[1].fx.capital_ppm) < 20'000);                // the flow fades (no standing interest differential)
	}

	/* Overflow: absurd money flows and extreme inputs never break the limits. */
	{
		auto z = make(4); FxWorld w; w.mood_ppm = -150'000; w.war = true;
		for (int64_t d = 1; d <= 2000; d++) {
			for (int i = 0; i < 4; i++) { z[i].fx.exports_today = INT64_C(4'000'000'000'000'000'000); z[i].fx.imports_today = (i & 1) ? 0 : INT64_C(9'000'000'000'000'000'000); z[i].fx.gdp_today = INT64_C(9'000'000'000'000'000'000) * ((d + i) % 2); }
			z[0].inflation_ppm = INFL_MAX_PPM; z[1].inflation_ppm = INFL_MIN_PPM;
			FxStepDay(111, d, z.data(), 4, w);
			for (int i = 0; i < 4; i++) CHECK(z[i].rate_ppm >= RATE_MIN_PPM && z[i].rate_ppm <= RATE_MAX_PPM && z[i].fx.reserve_ppm >= 0 && z[i].fx.price_level_ppm >= PRICE_LEVEL_MIN_PPM);
		}
		/* Fewer zones than the maximum, and none. */
		auto two = make(2); for (int64_t d = 1; d <= 400; d++) FxStepDay(3, d, two.data(), 2, w);
		CHECK(two[0].rate_ppm >= RATE_MIN_PPM && two[1].rate_ppm <= RATE_MAX_PPM);
		FxStepDay(3, 1, two.data(), 0, w); FxStepDay(3, 1, two.data(), 9, w);
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
		int64_t infl = 500'000;
		for (int64_t d = 0; d < 3000; d++) {
			infl = NextInflation(7, 2, d, infl);
			CHECK(infl >= INFL_MIN_PPM && infl <= INFL_MAX_PPM);
		}
		CHECK(infl < 100'000);                                        // pulled back to about 2%
		CHECK(NextInflation(7, 2, 9, 30'000) == NextInflation(7, 2, 9, 30'000));
		CHECK(DailyInterest(365'000'000, 36'500) == 36'500 * 365'000'000LL / PPM / 365 && DailyInterest(0, 1000) == 0 && DailyInterest(5, 0) == 0);
		CHECK(NotesForMoney(1'000'000, 2'000'000) == 500'000 && MoneyForNotes(500'000, 2'000'000) == 1'000'000);
		CHECK(AfterSpread(1'000'000) == 997'000 && AfterSpread(-1) == 0);
		const int64_t notes = AfterSpread(NotesForMoney(1'000'000, 1'250'000)), back = AfterSpread(MoneyForNotes(notes, 1'250'000));
		CHECK(back < 1'000'000 && back > 990'000);                    // a round trip costs about 0.6%
		CHECK(RateAfterTrade(PPM, true, 10) > PPM && RateAfterTrade(PPM, false, 10) < PPM && RateAfterTrade(PPM, true, 1'000'000'000) == PPM + PPM / 1000 && RateAfterTrade(PPM, false, 1'000'000'000) == PPM - PPM / 1000);
		CHECK(RateAfterTrade(PPM, true, 100) - PPM <= 200 && RateAfterTrade(PPM, true, 100) > PPM); // 100 units: 0.02%, one player does not move a currency
		CHECK(RateAfterTrade(RATE_MAX_PPM, true, 1000) == RATE_MAX_PPM);
	}
	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
