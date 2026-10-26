/* Stand-alone test of oil_market_math.h and the oil news kinds (not part of the game build).
 * Build and run:  g++ -std=c++17 -Wall -Wextra -o /tmp/omt oil_market_test.cpp && /tmp/omt */
#include <cstdio>
#include <cstdlib>
#include "oil_market_math.h"
#include "market_events.h"
#include "market_news_ai.h"
using namespace oilmath;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main()
{
	/* Scarcity: x1 at the start, x2 at half, never below x0.5 or above x4, and never falling when the stock falls. */
	CHECK(ScarcityPpm(STOCK_START) == PPM);
	CHECK(ScarcityPpm(STOCK_START / 2) == 2 * PPM);
	CHECK(ScarcityPpm(STOCK_START * 10) == PPM / 2);
	CHECK(ScarcityPpm(0) == SCARCITY_FLOOR_DIV * PPM && ScarcityPpm(-5) == SCARCITY_FLOOR_DIV * PPM);
	{ bool mono = true; int64_t last = 0; for (int64_t st = STOCK_CAP; st >= 0; st -= 10'000) { const int64_t v = ScarcityPpm(st); if (v < last) mono = false; last = v; } CHECK(mono); }

	/* Yield: full up to the start stock, a straight line to 0 at the cap. */
	CHECK(YieldPpm(0) == PPM && YieldPpm(STOCK_START) == PPM);
	CHECK(YieldPpm(STOCK_CAP) == 0 && YieldPpm(STOCK_CAP + 1000) == 0);
	CHECK(YieldPpm(STOCK_START + (STOCK_CAP - STOCK_START) / 2) == PPM / 2);

	/* Delivery: limited by yield, room and allowance; never negative; no overflow. */
	CHECK(DeliveredBarrels(0, STOCK_START, WELL_DAILY_MAX) == 0 && DeliveredBarrels(-3, STOCK_START, WELL_DAILY_MAX) == 0);
	CHECK(DeliveredBarrels(10, STOCK_START, WELL_DAILY_MAX) == 10 * BARRELS_PER_UNIT);
	CHECK(DeliveredBarrels(1'000'000, STOCK_START, WELL_DAILY_MAX) == WELL_DAILY_MAX);  // the daily bucket
	CHECK(DeliveredBarrels(1'000'000, STOCK_START, 123) == 123);
	CHECK(DeliveredBarrels(10, STOCK_START, 0) == 0 && DeliveredBarrels(10, STOCK_START, -5) == 0);
	CHECK(DeliveredBarrels(10, STOCK_CAP, WELL_DAILY_MAX) == 0 && DeliveredBarrels(10, STOCK_CAP + 5, WELL_DAILY_MAX) == 0);
	CHECK(DeliveredBarrels(1'000'000, STOCK_CAP - 7, 1'000'000'000) <= 7);               // room
	CHECK(DeliveredBarrels(INT32_MAX, STOCK_START, WELL_DAILY_MAX) == WELL_DAILY_MAX);

	/* Rebalance: moves towards the start, never past it, never stuck, and a game day of steps does about 0.8% of the gap. */
	CHECK(RebalanceStep(STOCK_START, 24) == 0);
	CHECK(RebalanceStep(STOCK_START - 5, 24) == 1 && RebalanceStep(STOCK_START + 5, 24) == -1);
	CHECK(RebalanceStep(0, 24) > 0 && RebalanceStep(STOCK_CAP, 24) < 0);
	{
		int64_t st = STOCK_START / 2;
		for (int i = 0; i < 24; i++) st += RebalanceStep(st, 24);
		const int64_t moved = st - STOCK_START / 2, wanted = (STOCK_START / 2) * REBALANCE_PERMILLE / 1000;
		CHECK(moved > wanted * 90 / 100 && moved <= wanted);
		bool over = false; st = 3; for (int i = 0; i < 5'000'000; i++) { st += RebalanceStep(st, 24); if (st > STOCK_START) { over = true; break; } } CHECK(!over && st >= STOCK_START - 24 * 125);
		over = false; st = STOCK_CAP; for (int i = 0; i < 5'000'000; i++) { st += RebalanceStep(st, 24); if (st < STOCK_START) { over = true; break; } } CHECK(!over);
	}
	CHECK(RebalanceStep(100, 0) != 0); // steps_per_day 0 is treated as 1

	/* Price index and fuel index. */
	CHECK(PriceIndexPpm(0, 60'000) == PPM && PriceIndexPpm(60'000, 0) == PPM);
	CHECK(PriceIndexPpm(60'000, 60'000) == PPM && PriceIndexPpm(90'000, 60'000) == 1'500'000);
	CHECK(PriceIndexPpm(1, 60'000) == PRICE_INDEX_MIN_PPM && PriceIndexPpm(6'000'000, 60'000) == PRICE_INDEX_MAX_PPM);
	CHECK(FuelIndexPpm(PPM, PPM) == PPM);
	CHECK(FuelIndexPpm(PPM, 1'400'000) == 1'200'000);                 // 50% of +40%
	CHECK(FuelIndexPpm(PPM, 600'000) == 800'000);                     // below 1.0 (the fuel cost ignores that, FuelExcessPpm)
	CHECK(FuelIndexPpm(1'100'000, 1'400'000) == 1'320'000);           // news 1.1 x oil 1.2
	CHECK(FuelIndexPpm(INT64_MAX, 3 * PPM) > 0 && FuelIndexPpm(INT64_MIN, PPM) == 0); // no overflow, no negative
	{ bool mono = true; int64_t last = 0; for (int64_t o = PRICE_INDEX_MIN_PPM; o <= PRICE_INDEX_MAX_PPM; o += 5'000) { const int64_t f = FuelIndexPpm(PPM, o); if (f < last) mono = false; last = f; } CHECK(mono); }

	/* News from the stock. */
	bool glut = false; int sev = 0;
	CHECK(!NewsFromStock(1000, glut, sev) && !NewsFromStock(1249, glut, sev) && !NewsFromStock(701, glut, sev));
	CHECK(NewsFromStock(1250, glut, sev) && glut && sev == 1);
	CHECK(NewsFromStock(1380, glut, sev) && glut && sev == 2);
	CHECK(NewsFromStock(1500, glut, sev) && glut && sev == 3);
	CHECK(NewsFromStock(700, glut, sev) && !glut && sev == 1);
	CHECK(NewsFromStock(550, glut, sev) && !glut && sev == 2);
	CHECK(NewsFromStock(0, glut, sev) && !glut && sev == 3);
	CHECK(StockPermille(STOCK_START) == 1000 && StockPermille(STOCK_START * 5 / 4) == 1250);
	CHECK(StockPermille(STOCK_CAP) >= GLUT_PERMILLE_3); // a full market always reaches the biggest glut story

	/* The kinds: numbers saved in a savegame, direction of the push, never drawn by weight, a story with real numbers. */
	CHECK(static_cast<int>(MarketEventKind::OilWellsFound) == 49 && static_cast<int>(MarketEventKind::OilSupplyShort) == 50);
	CHECK(MarketKindIsRealOilKind(MarketEventKind::OilWellsFound) && MarketKindIsRealOilKind(MarketEventKind::OilSupplyShort) && !MarketKindIsRealOilKind(MarketEventKind::FuelSpike));
	for (int s = 1; s <= 3; s++) {
		int v = -1; const int64_t down = MarketValuablePushPpm(MarketEventKind::OilWellsFound, s, v);
		CHECK(down < 0 && v == 4);
		v = -1; const int64_t up = MarketValuablePushPpm(MarketEventKind::OilSupplyShort, s, v);
		CHECK(up > 0 && v == 4 && up == -down);
	}
	CHECK(MarketEventInfo(MarketEventKind::OilWellsFound).weight == 0 && MarketEventInfo(MarketEventKind::OilSupplyShort).weight == 0);
	{
		MarketEvents m; m.Seed(5);
		const int64_t minute = 100 * MARKET_MINUTES_PER_DAY;
		MarketEvent e = m.NewEvent(MarketEventKind::OilSupplyShort, minute, 2);
		e.fact_ppm = 520'000; e.fact2 = 10'400'000;
		CHECK(e.gold_ppm == 0 && e.share_ppm == 0);
		m.Apply(e);
		CHECK(m.ValuablePushPpm(4, minute) > 0);                                   // the oil price is pushed up ...
		CHECK(m.ValuablePushPpm(1, minute) == 0 && m.ValuablePushPpm(0, minute) == 0); // ... and nothing else
		CHECK(m.RecentlyReportedGold(MarketEventKind::OilSupplyShort, minute + 5 * MARKET_MINUTES_PER_DAY, NEWS_GAP_DAYS));
		CHECK(!m.RecentlyReportedGold(MarketEventKind::OilSupplyShort, minute + (NEWS_GAP_DAYS + 2) * MARKET_MINUTES_PER_DAY, NEWS_GAP_DAYS));
		CHECK(!m.RecentlyReportedGold(MarketEventKind::OilWellsFound, minute + 5 * MARKET_MINUTES_PER_DAY, NEWS_GAP_DAYS));
		const MarketStory story = ComposeMarketStory(e, std::string());
		CHECK(!story.headline.empty() && story.headline.find("{P}") == std::string::npos);
		for (int v = 0; v < 4; v++) { MarketEvent ev = e; ev.variant = static_cast<uint8_t>(v); const MarketStory sv = ComposeMarketStory(ev, std::string()); CHECK(sv.body.find("52.0%") != std::string::npos && sv.body.find("{P}") == std::string::npos); }
		CHECK(story.body.find("{P}") == std::string::npos);
		CHECK(MarketFactLine(e).find("10,400,000") != std::string::npos);
		MarketEvent g = m.NewEvent(MarketEventKind::OilWellsFound, minute, 3);
		g.fact_ppm = 1'480'000; g.fact2 = 29'600'000;
		m.Apply(g);
		const MarketStory story2 = ComposeMarketStory(g, std::string());
		CHECK(!story2.headline.empty() && story2.headline.find("{P}") == std::string::npos);
		CHECK(m.ValuablePushPpm(4, minute) < 2 * MARKET_VALUABLE_PUSH_MAX_PPM);
	}

	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
