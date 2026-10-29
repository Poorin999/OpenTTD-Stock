/* Stand-alone test of the market players in RetailMath.h (not part of the game build).
 * Build and run:  g++ -std=c++17 -Wall -Wextra -o /tmp/rmt retail_math_test.cpp && /tmp/rmt */
#include <cstdio>
#include <set>
#include "RetailMath.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

using namespace retail;

static CompanySignals Base()
{
	CompanySignals s;
	s.valid = true; s.price = 100; s.earnings = 1000; s.equity = 10000; s.has_value = true;
	return s;
}

int main()
{
	int64_t cap = 0;
	for (int k = 0; k < COHORT_COUNT; k++) cap += COHORT_CAPITAL_PERCENT[k];
	CHECK(cap == 100);
	int64_t bots = 0;
	for (int k = 0; k < BOT_STYLE_COUNT; k++) bots += BOT_STYLE_PERCENT[k];
	CHECK(bots == 100);

	/* The groups disagree about the same company: below the model value the bargain hunters buy while the momentum group (falling price) does not. */
	CompanySignals s = Base();
	s.value_gap_ppm = 300'000; s.has_momentum = true; s.momentum_ppm = -150'000;
	Sentiment a = ScoreCompany(s);
	CHECK(a.cohort_score[COHORT_VALUE] >= BUY_THRESHOLD_PPM);
	CHECK(a.cohort_score[COHORT_MOMENTUM] < BUY_THRESHOLD_PPM);
	/* Far above the model value: the bargain hunters sell, passive savers still hold. */
	s = Base(); s.value_gap_ppm = -400'000;
	a = ScoreCompany(s);
	CHECK(a.cohort_score[COHORT_VALUE] <= -PPM + 1);
	CHECK(a.cohort_score[COHORT_PASSIVE] >= BUY_THRESHOLD_PPM);
	CHECK(SellShares(100, a.cohort_score[COHORT_PASSIVE], 0) == 0);
	CHECK(SellShares(100, a.cohort_score[COHORT_VALUE], 0) > 0);
	/* Fair price: nobody buys it new, nobody sells it (hold level). */
	s = Base(); s.value_gap_ppm = 0;
	a = ScoreCompany(s);
	CHECK(a.cohort_score[COHORT_VALUE] < BUY_THRESHOLD_PPM && a.cohort_score[COHORT_VALUE] >= SELL_EXIT_PPM);
	/* Falling knife and no earnings: no value buying. */
	s = Base(); s.value_gap_ppm = 300'000; s.has_momentum = true; s.momentum_ppm = -300'000;
	CHECK(ScoreCompany(s).cohort_score[COHORT_VALUE] == 0);
	s = Base(); s.value_gap_ppm = 300'000; s.earnings = -5;
	CHECK(ScoreCompany(s).cohort_score[COHORT_VALUE] == 0);
	/* No model value known: the bargain hunters have no opinion. */
	s = Base(); s.has_value = false; s.value_gap_ppm = 300'000;
	CHECK(ScoreCompany(s).cohort_score[COHORT_VALUE] == 0);
	/* The gamblers follow their tip; the mood counts double; a cheap share gets a bonus. */
	s = Base(); s.noise_ppm = 500'000;
	CHECK(NoiseScore(s) == 400'000);
	s.mood_ppm = 100'000;
	CHECK(NoiseScore(s) == 600'000);
	s = Base(); s.price = 10;
	CHECK(NoiseScore(s) == NOISE_LOTTERY_BONUS_PPM);
	/* Panic: the gamblers sell more of their holding than the passive savers. */
	CHECK(SellShares(1000, 0, PPM, COHORT_PANIC_SELL_PERCENT[COHORT_NOISE]) > SellShares(1000, 0, PPM, COHORT_PANIC_SELL_PERCENT[COHORT_PASSIVE]));

	/* Routing: both sides reach the terminal, only a part is crossed. */
	std::array<int64_t, COHORT_COUNT> buy{}, sell{};
	buy[COHORT_VALUE] = 1000; buy[COHORT_PASSIVE] = 1000;
	sell[COHORT_MOMENTUM] = 1500; sell[COHORT_NOISE] = 500;
	Routing r = RouteOrders(buy, sell, 1'000'000, 1);
	CHECK(r.crossed == 2000 * CROSS_PERCENT / 100);
	int64_t cb = 0, cs = 0, mb = 0, ms = 0;
	for (int k = 0; k < COHORT_COUNT; k++) { cb += r.cross_buy[k]; cs += r.cross_sell[k]; mb += r.market_buy[k]; ms += r.market_sell[k]; }
	CHECK(cb <= r.crossed && cs <= r.crossed && cb >= r.crossed - COHORT_COUNT && cs >= r.crossed - COHORT_COUNT);
	CHECK(mb > 0 && ms > 0);                         // not one net number: both sides are in the market
	CHECK(cb + mb == 2000 && cs + ms == 2000);       // nothing is lost
	/* The daily flow limit. */
	buy.fill(0); sell.fill(0); buy[COHORT_MOMENTUM] = 100'000;
	r = RouteOrders(buy, sell, 100'000, 24);
	CHECK(r.market_buy[COHORT_MOMENTUM] == std::max<int64_t>(1, 100'000 * MAX_DAILY_FLOW_PERCENT / 100 / 24));

	/* The AI styles: deterministic, and all the styles exist in a crowd of companies with their own numbers. */
	std::set<int> styles; std::set<int64_t> buys;
	int count[BOT_STYLE_COUNT] = {};
	for (int c = 0; c < 2000; c++) {
		const BotParams p = BotParamsOf(77, c % 8 == 0 ? c : c);
		const BotParams q = BotParamsOf(77, c);
		CHECK(p.style == q.style && p.buy_ppm == q.buy_ppm);
		styles.insert(static_cast<int>(p.style)); count[static_cast<int>(p.style)]++;
		if (p.style == BotStyle::Value) buys.insert(p.buy_ppm);
		CHECK(p.buy_ppm > 0 && p.stop_ppm > 0 && p.take_ppm > 0 && p.max_hold_months >= 1 && p.risk_percent >= 5);
	}
	CHECK(styles.size() == static_cast<size_t>(BOT_STYLE_COUNT));
	CHECK(buys.size() > 10);
	for (int k = 0; k < BOT_STYLE_COUNT; k++) CHECK(std::abs(count[k] - BOT_STYLE_PERCENT[k] * 20) < 120);

	/* Different bots, different opinions about the SAME company at the SAME moment. */
	BotInputs in; in.valid = true; in.value_gap_ppm = 150'000; in.trend30_ppm = -120'000; in.has_trend30 = true; in.trend_month_ppm = -100'000;
	in.avg_dev_ppm = -150'000; in.earnings_drift_ppm = 20'000; in.tip_ppm = -300'000;
	BotParams pv, pm, pc, pn, pg;
	pv = BotParamsOf(1, 0); pv.style = BotStyle::Value; pm = pv; pm.style = BotStyle::Momentum; pc = pv; pc.style = BotStyle::Contrarian; pn = pv; pn.style = BotStyle::Noise; pg = pv; pg.style = BotStyle::Growth;
	CHECK(BotOpinion(pv, in).score > 0);   // cheap against the value: buy
	CHECK(BotOpinion(pm, in).score < 0);   // falling: sell short
	CHECK(BotOpinion(pc, in).score > 0);   // fell far below its average: buy
	CHECK(BotOpinion(pn, in).score < 0);   // a bad tip
	/* A growth trader never shorts; a noise trader may buy a company that cannot be bought. */
	pg.short_ppm = 2 * PPM;
	CHECK(!BotOpinion(pg, in).can_short);
	in.can_long = false;
	CHECK(!BotOpinion(pv, in).can_long && BotOpinion(pn, in).can_long);
	/* The private error: two value traders do not value a company the same. */
	std::set<int64_t> errs;
	for (int b = 0; b < 40; b++) errs.insert(BotValueError(5, b, 3, 7, 100'000));
	CHECK(errs.size() > 30);
	CHECK(BotValueError(5, 1, 3, 7, 100'000) == BotValueError(5, 1, 3, 7, 100'000));
	CHECK(BotValueError(5, 1, 3, 7, 0) == 0);
	int64_t mx = 0; for (int b = 0; b < 500; b++) mx = std::max(mx, std::abs(BotValueError(5, b, 3, 7, 100'000)));
	CHECK(mx <= 200'000);
	/* The tips are in range and change from week to week. */
	bool changes = false;
	for (int w = 0; w < 50; w++) { const int64_t t = BotTip(3, 1, 2, w); CHECK(t >= -PPM && t <= PPM); changes = changes || t != BotTip(3, 1, 2, w + 1); }
	CHECK(changes);

	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
