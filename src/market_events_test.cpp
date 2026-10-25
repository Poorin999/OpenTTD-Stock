/* Stand-alone test of market_events.h and market_news_ai.h (not part of the game build).
 * Build and run:  g++ -std=c++17 -Wall -Wextra -o /tmp/met market_events_test.cpp && /tmp/met */
#include <cstdio>
#include <set>
#include "market_events.h"
#include "market_news_ai.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main()
{
	/* Table sanity. */
	for (size_t k = 0; k < MARKET_EVENT_KIND_COUNT; k++) {
		const MarketEventDef &d = MarketEventInfo(static_cast<MarketEventKind>(k));
		CHECK(d.weight > 0);
		CHECK(d.gold_dir != 0 || d.share_dir != 0);
		CHECK(d.gold_min <= d.gold_max && d.share_min <= d.share_max);
		const MarketStoryTexts &t = MarketStoryTextsOf(static_cast<MarketEventKind>(k));
		CHECK(t.headline[0] && t.headline[1] && t.body[0] && t.body[1]);
		CHECK((std::string(t.headline[0]).find("{C}") != std::string::npos) == d.company);
	}

	/* Same seed, same row of events. */
	MarketEvents a, b;
	a.Seed(12345); b.Seed(12345);
	std::vector<uint8_t> comps = {0, 2, 5};
	for (int i = 0; i < 200; i++) {
		MarketEvent x = a.Generate(i * 60, comps), y = b.Generate(i * 60, comps);
		CHECK(x.kind == y.kind && x.gold_ppm == y.gold_ppm && x.share_ppm == y.share_ppm && x.target == y.target && x.variant == y.variant);
	}

	/* Statistics over 20000 events: all kinds appear, limits hold, signs follow the table, company events have a valid target. */
	MarketEvents m; m.Seed(777);
	std::set<int> seen; int sev[4] = {0,0,0,0}; int64_t max_gold = 0, max_share = 0; int world_only_company = 0;
	for (int i = 0; i < 20000; i++) {
		MarketEvent e = m.Generate(i * 60, comps);
		const MarketEventDef &d = MarketEventInfo(e.kind);
		seen.insert(static_cast<int>(e.kind)); sev[e.severity]++;
		max_gold = std::max<int64_t>(max_gold, std::abs(e.gold_ppm)); max_share = std::max<int64_t>(max_share, std::abs(e.share_ppm));
		CHECK(e.severity >= 1 && e.severity <= 3 && e.variant <= 11);
		CHECK((e.gold_ppm > 0) == (d.gold_dir > 0) && (e.gold_ppm < 0) == (d.gold_dir < 0));
		CHECK((e.share_ppm > 0) == (d.share_dir > 0) && (e.share_ppm < 0) == (d.share_dir < 0));
		CHECK(d.company == (e.target != MARKET_NO_TARGET));
		if (d.company) CHECK(e.target == 0 || e.target == 2 || e.target == 5);
		(void)world_only_company;
	}
	CHECK(seen.size() == MARKET_EVENT_KIND_COUNT);
	CHECK(max_gold <= MARKET_EVENT_GOLD_MAX_PPM && max_share <= MARKET_EVENT_SHARE_MAX_PPM);
	CHECK(sev[1] > sev[2] && sev[2] > sev[3] && sev[3] > 0);
	std::printf("severity 1/2/3 = %d/%d/%d, max gold %lld ppm, max share %lld ppm\n", sev[1], sev[2], sev[3], (long long)max_gold, (long long)max_share);

	/* Without companies, no company event. */
	MarketEvents n; n.Seed(5);
	for (int i = 0; i < 3000; i++) { MarketEvent e = n.Generate(i, {}); CHECK(!MarketEventInfo(e.kind).company && e.target == MARKET_NO_TARGET); }

	/* The dice: no event in the start delay, none inside the gap, about 13% per hour otherwise. */
	MarketEvents f; f.Seed(99);
	for (int h = 0; h < MARKET_EVENT_START_DELAY_HOURS; h++) CHECK(!f.ShouldFire(h * 60));
	f.last_minute = 1000;
	CHECK(!f.ShouldFire(1000 + 60)); // inside the gap
	MarketEvents g; g.Seed(31337); int fired = 0; const int hours = 20000;
	for (int h = 10; h < hours; h++) if (g.ShouldFire(h * 60)) { fired++; g.last_minute = h * 60; }
	std::printf("fired %d of %d hours (gap %d h)\n", fired, hours - 10, MARKET_EVENT_MIN_GAP_HOURS);
	CHECK(fired > 0.05 * hours && fired < 0.2 * hours);

	/* Sentiment: market and company, limit, fading, only the company of the event. */
	MarketEvents s; s.Seed(1);
	MarketEvent w; w.id = 1; w.kind = MarketEventKind::EconomicBoom; w.share_ppm = 30'000; w.minute = 600;
	s.Apply(w);
	CHECK(s.SentimentPpm(0) == 30'000 && s.SentimentPpm(7) == 30'000);
	MarketEvent c; c.id = 2; c.kind = MarketEventKind::CompanyScandal; c.target = 2; c.share_ppm = -90'000; c.minute = 700;
	s.Apply(c);
	CHECK(s.SentimentPpm(2) == -60'000 && s.SentimentPpm(3) == 30'000);
	for (int i = 0; i < 5; i++) { MarketEvent big = c; big.share_ppm = 100'000; s.Apply(big); }
	CHECK(s.SentimentPpm(2) <= MARKET_SENTIMENT_MAX_PPM && s.SentimentPpm(2) >= -MARKET_SENTIMENT_MAX_PPM);
	CHECK(s.log.size() == 7 && s.log.front().id == c.id && s.last_minute == 700);
	int64_t before = std::abs(s.SentimentPpm(2));
	for (int h = 0; h < 7; h++) s.DecayHour();
	CHECK(std::abs(s.SentimentPpm(2)) < before);
	for (int h = 0; h < 400; h++) s.DecayHour();
	CHECK(s.SentimentPpm(2) == 0 && s.SentimentPpm(0) == 0);
	for (int i = 0; i < 100; i++) s.Apply(w);
	CHECK(s.log.size() == MARKET_EVENT_KEEP);

	/* Forced kind (the console command stock_event). */
	MarketEvents fk; fk.Seed(8);
	for (size_t k = 0; k < MARKET_EVENT_KIND_COUNT; k++) {
		MarketEvent x = fk.Generate(0, comps, static_cast<int>(k));
		CHECK(static_cast<size_t>(x.kind) == k);
		MarketEvent y = fk.Generate(0, {}, static_cast<int>(k)); // Without companies a company kind is not allowed.
		CHECK(MarketEventInfo(y.kind).company == false);
		if (!MarketEventInfo(static_cast<MarketEventKind>(k)).company) CHECK(static_cast<size_t>(y.kind) == k);
	}
	CHECK(fk.Generate(0, comps, 99).id > 0); // A number out of range is the same as -1.

	/* The story writer. */
	MarketEvent e; e.kind = MarketEventKind::CompanyScandal; e.target = 2; e.variant = 5; e.severity = 3; e.gold_ppm = 0; e.share_ppm = -90'000;
	MarketStory st = ComposeMarketStory(e, "ABC Transport");
	CHECK(st.headline.find("ABC Transport") != std::string::npos && st.headline.find("{C}") == std::string::npos);
	CHECK(st.body.find("{C}") == std::string::npos);
	CHECK(st.analysis.find("-9.00%") != std::string::npos);
	CHECK(st.reporter == MarketReporterName(5));
	CHECK(MarketPpmText(-5'000) == "-0.50%" && MarketPpmText(12'345) == "+1.23%" && MarketPpmText(0) == "+0.00%" && MarketPpmText(-100) == "-0.01%");
	MarketEvent wr; wr.kind = MarketEventKind::WarTension; wr.gold_ppm = 25'000; wr.share_ppm = -15'000;
	MarketStory sw = ComposeMarketStory(wr, "");
	CHECK(sw.analysis.find("+2.50%") != std::string::npos && sw.analysis.find("-1.50%") != std::string::npos);
	for (size_t k = 0; k < MARKET_EVENT_KIND_COUNT; k++) for (int v = 0; v < 12; v++) {
		MarketEvent x; x.kind = static_cast<MarketEventKind>(k); x.variant = static_cast<uint8_t>(v); x.target = MarketEventInfo(x.kind).company ? 1 : MARKET_NO_TARGET;
		MarketStory y = ComposeMarketStory(x, "Co");
		CHECK(!y.headline.empty() && !y.body.empty() && y.headline.find("{C}") == std::string::npos);
	}

	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
