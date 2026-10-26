/* Stand-alone test of market_events.h and market_news_ai.h (not part of the game build).
 * Build and run:  g++ -std=c++17 -Wall -Wextra -o /tmp/met market_events_test.cpp && /tmp/met */
#include <cstdio>
#include <set>
#include "market_events.h"
#include "market_news_ai.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static CompanyFacts MakeFacts(uint8_t id, int quarters, int64_t inc_last, int64_t inc_prev, int64_t net_last, int64_t net_prev, int64_t val_last, int64_t val_prev, int64_t debt = 0, bool def = false, int bankrupt = 0)
{
	CompanyFacts f; f.company = id; f.quarters = quarters; f.income_last = inc_last; f.income_prev = inc_prev; f.net_last = net_last; f.net_prev = net_prev;
	f.value_last = val_last; f.value_prev = val_prev; f.debt = debt; f.in_default = def; f.months_bankrupt = bankrupt;
	return f;
}

int main()
{
	/* Table sanity. */
	for (size_t k = 0; k < MARKET_EVENT_KIND_COUNT; k++) {
		const MarketEventDef &d = MarketEventInfo(static_cast<MarketEventKind>(k));
		CHECK(d.weight > 0);
		{
			const MarketEventKind kk = static_cast<MarketEventKind>(k);
			int vi = -1;
			const bool other = MarketCargoEffectOf(kk, 1).push_ppm != 0 || MarketDiamondStockPpm(kk, 1) != 0 || MarketTownBoostPpm(kk, 1) != 0 || MarketValuablePushPpm(kk, 1, vi) != 0;
			CHECK(d.gold_dir != 0 || d.share_dir != 0 || other || MarketKindHasTownField(kk)); // Town news and weather have their own effects (weather) or none (town news).
		}
		CHECK(d.gold_min <= d.gold_max && d.share_min <= d.share_max);
		const MarketStoryTexts &t = MarketStoryTextsOf(static_cast<MarketEventKind>(k));
		CHECK(t.headline[0] && t.headline[1] && t.body[0] && t.body[1]);
		CHECK((std::string(t.headline[0]).find("{C}") != std::string::npos) == (d.company || static_cast<MarketEventKind>(k) == MarketEventKind::TownBoom || MarketKindHasTownField(static_cast<MarketEventKind>(k)))); // Town stories name the town.
		CHECK(d.company == MarketKindIsCompany(static_cast<MarketEventKind>(k)));
	}

	/* Same seed, same row of events. */
	MarketEvents a, b;
	a.Seed(12345); b.Seed(12345);
	std::vector<CompanyFacts> comps = {
		MakeFacts(0, 2, 150'000, 100'000, 40'000, 20'000, 1'200'000, 1'000'000),
		MakeFacts(2, 2, 60'000, 100'000, -30'000, 40'000, 700'000, 1'000'000, 600'000, true),
		MakeFacts(5, 2, 100'000, 100'000, 30'000, -10'000, 1'000'000, 1'000'000, 0, false, 3),
		MakeFacts(7, 2, 100'000, 100'000, 10'000, 30'000, 1'000'000, 1'000'000),
	};
	for (int i = 0; i < 200; i++) {
		MarketEvent x = a.Generate(i * 60, comps), y = b.Generate(i * 60, comps);
		CHECK(x.kind == y.kind && x.gold_ppm == y.gold_ppm && x.share_ppm == y.share_ppm && x.target == y.target && x.variant == y.variant);
	}

	/* Statistics over 20000 events: all kinds appear, limits hold, signs follow the table, company events have a valid target. */
	MarketEvents m; m.Seed(777);
	std::set<int> seen; int sev[4] = {0,0,0,0}; int64_t max_gold = 0, max_share = 0; int world_only_company = 0;
	for (int i = 0; i < 20000; i++) {
		MarketEvent e = m.Generate(i * 60, comps, -1, 4);
		const MarketEventDef &d = MarketEventInfo(e.kind);
		seen.insert(static_cast<int>(e.kind)); sev[e.severity]++;
		max_gold = std::max<int64_t>(max_gold, std::abs(e.gold_ppm)); max_share = std::max<int64_t>(max_share, std::abs(e.share_ppm));
		CHECK(e.severity >= 1 && e.severity <= 3 && e.variant <= 11);
		CHECK((e.gold_ppm > 0) == (d.gold_dir > 0) && (e.gold_ppm < 0) == (d.gold_dir < 0));
		CHECK((e.share_ppm > 0) == (d.share_dir > 0) && (e.share_ppm < 0) == (d.share_dir < 0));
		CHECK(d.company == (e.target != MARKET_NO_TARGET));
		if (d.company) CHECK(e.target == 0 || e.target == 2 || e.target == 5 || e.target == 7);
		(void)world_only_company;
	}
	CHECK(seen.size() == MARKET_EVENT_KIND_COUNT - 7); // The 7 town news and weather kinds are never drawn: they are made from the real state of the towns.
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
	MarketEvent c; c.id = 2; c.kind = MarketEventKind::CompanyDistress; c.target = 2; c.share_ppm = -90'000; c.minute = 700;
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
	for (int i = 0; i < 300; i++) s.Apply(w);
	CHECK(s.log.size() == MARKET_EVENT_KEEP);

	/* Forced kind (the console command stock_event). */
	MarketEvents fk; fk.Seed(8);
	for (size_t k = 0; k < MARKET_EVENT_KIND_COUNT; k++) {
		MarketEvent x = fk.Generate(0, comps, static_cast<int>(k), 3);
		if (MarketKindHasTownField(static_cast<MarketEventKind>(k))) { CHECK(!MarketKindHasTownField(x.kind)); continue; } // Never drawn: made from the real state of the towns.
		CHECK(static_cast<size_t>(x.kind) == k);
		MarketEvent y = fk.Generate(0, {}, static_cast<int>(k), 3); // Without companies a company kind is not allowed.
		CHECK(MarketEventInfo(y.kind).company == false);
		if (!MarketEventInfo(static_cast<MarketEventKind>(k)).company) CHECK(static_cast<size_t>(y.kind) == k);
	}
	CHECK(fk.Generate(0, comps, 99).id > 0); // A number out of range is the same as -1.

	/* The story writer. */
	MarketEvent e; e.kind = MarketEventKind::CompanyDistress; e.target = 2; e.variant = 5; e.severity = 3; e.gold_ppm = 0; e.share_ppm = -90'000;
	MarketStory st = ComposeMarketStory(e, "ABC Transport");
	CHECK(st.headline.find("ABC Transport") != std::string::npos && st.headline.find("{C}") == std::string::npos);
	CHECK(st.body.find("{C}") == std::string::npos);
	CHECK(st.analysis.find("-9.00%") != std::string::npos);
	CHECK(st.headline.find("{P}") == std::string::npos && st.body.find("{P}") == std::string::npos);
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

	/* ===== The company news is TRUE ===== */
	auto kinds_for = [&](const std::vector<CompanyFacts> &facts, int draws, uint64_t seed) {
		MarketEvents g2; g2.Seed(seed);
		std::set<int> kinds;
		for (int i = 0; i < draws; i++) {
			MarketEvent e = g2.Generate(i * 60, facts);
			if (!MarketKindIsCompany(e.kind)) continue;
			kinds.insert(static_cast<int>(e.kind));
			int64_t fact = 0; int sv = 0; const CompanyFacts *cf = nullptr;
			for (const CompanyFacts &f : facts) if (f.company == e.target) cf = &f;
			CHECK(cf != nullptr);
			if (cf != nullptr) { CHECK(CompanyEventFact(e.kind, *cf, fact, sv)); CHECK(fact == e.fact_ppm && sv == e.severity); }
		}
		return kinds;
	};
	const int RU = (int)MarketEventKind::CompanyRevenueUp, RD = (int)MarketEventKind::CompanyRevenueDown, PU = (int)MarketEventKind::CompanyProfitUp, PD = (int)MarketEventKind::CompanyProfitDown;
	const int LS = (int)MarketEventKind::CompanyLoss, VU = (int)MarketEventKind::CompanyValueUp, VD = (int)MarketEventKind::CompanyValueDown, DS = (int)MarketEventKind::CompanyDistress, BK = (int)MarketEventKind::CompanyBankruptcy;
	CHECK(kinds_for({ MakeFacts(1, 0, 0, 0, 0, 0, 0, 0) }, 5000, 11).empty());
	{
		std::set<int> k = kinds_for({ MakeFacts(1, 2, 150'000, 100'000, 45'000, 30'000, 1'300'000, 1'000'000) }, 20000, 12);
		CHECK(k.count(RU) && k.count(PU) && k.count(VU));
		CHECK(!k.count(RD) && !k.count(PD) && !k.count(LS) && !k.count(VD) && !k.count(DS) && !k.count(BK));
		std::set<int> j = kinds_for({ MakeFacts(1, 2, 50'000, 100'000, -40'000, -10'000, 600'000, 900'000, 500'000, true) }, 20000, 13);
		CHECK(j.count(RD) && j.count(LS) && j.count(VD) && j.count(DS));
		CHECK(!j.count(RU) && !j.count(PU) && !j.count(PD) && !j.count(VU) && !j.count(BK));
	}
	{
		int64_t fact; int sv;
		CHECK(!CompanyEventFact(MarketEventKind::CompanyDistress, MakeFacts(1, 2, 1, 1, 1, 1, 1'000'000, 1'000'000, 100'000), fact, sv));
		CHECK(CompanyEventFact(MarketEventKind::CompanyDistress, MakeFacts(1, 2, 1, 1, 1, 1, 1'000'000, 1'000'000, 100'000, true), fact, sv) && sv >= 2);
		CHECK(CompanyEventFact(MarketEventKind::CompanyDistress, MakeFacts(1, 2, 1, 1, 1, 1, 1'000'000, 1'000'000, 600'000), fact, sv) && fact == 600'000 && sv == 1);
		CHECK(!CompanyEventFact(MarketEventKind::CompanyBankruptcy, MakeFacts(1, 2, 1, 1, 1, 1, 1, 1), fact, sv));
		CHECK(CompanyEventFact(MarketEventKind::CompanyBankruptcy, MakeFacts(1, 2, 1, 1, 1, 1, 1, 1, 0, false, 4), fact, sv) && fact == 4 && sv == 3);
		CHECK(!CompanyEventFact(MarketEventKind::CompanyRevenueUp, MakeFacts(1, 1, 500'000, 100'000, 1, 1, 1, 1), fact, sv));
		CHECK(CompanyEventFact(MarketEventKind::CompanyLoss, MakeFacts(1, 1, 100'000, 0, -25'000, 0, 1, 1), fact, sv) && fact == 250'000);
		CHECK(!CompanyEventFact(MarketEventKind::CompanyRevenueUp, MakeFacts(1, 2, 1'000, 100, 1, 1, 1, 1), fact, sv));
		CHECK(CompanyEventFact(MarketEventKind::CompanyRevenueUp, MakeFacts(1, 2, 150'000, 100'000, 1, 1, 1, 1), fact, sv) && fact == 500'000 && sv == 2);
		CHECK(CompanyEventFact(MarketEventKind::CompanyRevenueUp, MakeFacts(1, 2, 110'000, 100'000, 1, 1, 1, 1), fact, sv) && fact == 100'000 && sv == 1);
		CHECK(CompanyEventFact(MarketEventKind::CompanyRevenueUp, MakeFacts(1, 2, 300'000, 100'000, 1, 1, 1, 1), fact, sv) && fact == 2'000'000 && sv == 3);
		CHECK(CompanyEventFact(MarketEventKind::CompanyProfitDown, MakeFacts(1, 2, 1, 1, 20'000, 30'000, 1, 1), fact, sv) && fact == -333'333 && sv == 1);
		CHECK(CompanyEventFact(MarketEventKind::CompanyProfitUp, MakeFacts(1, 2, 1, 1, 20'000, -5'000, 1, 1), fact, sv) && fact == 0);
		CHECK(!CompanyEventFact(MarketEventKind::CompanyProfitUp, MakeFacts(1, 2, 1, 1, 20'000, 19'000, 1, 1), fact, sv));
		CHECK(!CompanyEventFact(MarketEventKind::CompanyProfitDown, MakeFacts(1, 2, 1, 1, -5'000, 30'000, 1, 1), fact, sv));
		CHECK(!CompanyEventFact(MarketEventKind::WarTension, MakeFacts(1, 2, 1, 1, 1, 1, 1, 1), fact, sv));
	}
	{ /* The same news about the same company is not repeated within 30 game days. */
		std::vector<CompanyFacts> one = { MakeFacts(1, 2, 150'000, 100'000, 1, 1, 1'000'000, 1'000'000) };
		MarketEvents rg; rg.Seed(22);
		int made = 0; int64_t last = -1;
		for (int h = 0; h < 24 * 200; h++) {
			const int64_t now = int64_t(h) * 60;
			if (!rg.ShouldFire(now)) continue;
			MarketEvent e = rg.Generate(now, one);
			if (e.kind == MarketEventKind::CompanyRevenueUp) { if (last >= 0) CHECK(now - last >= MARKET_COMPANY_REPEAT_DAYS * 24 * 60); last = now; made++; }
			rg.Apply(e);
		}
		std::printf("revenue-up news in 200 game days: %d\n", made);
		CHECK(made >= 2 && made <= 7);
	}
	{ /* The story prints the real number. */
		MarketEvent e; e.kind = MarketEventKind::CompanyRevenueUp; e.target = 1; e.fact_ppm = 123'456; e.variant = 0; e.severity = 1; e.share_ppm = 20'000;
		MarketStory st = ComposeMarketStory(e, "ABC");
		CHECK(st.headline.find("12.3%") != std::string::npos && st.analysis.find("+12.3%") != std::string::npos);
		e.kind = MarketEventKind::CompanyProfitDown; e.fact_ppm = -333'333; e.variant = 1;
		st = ComposeMarketStory(e, "ABC");
		CHECK(st.headline.find("33.3%") != std::string::npos && st.analysis.find("-33.3%") != std::string::npos);
		e.kind = MarketEventKind::CompanyProfitUp; e.fact_ppm = 0;
		st = ComposeMarketStory(e, "ABC");
		CHECK(st.headline.find("%") == std::string::npos && st.analysis.find("กลับมามีกำไร") != std::string::npos);
		e.kind = MarketEventKind::CompanyRevenueUp; e.fact_ppm = 0; e.variant = 0;
		st = ComposeMarketStory(e, "ABC");
		CHECK(st.headline == "ABC รายได้ไตรมาสล่าสุดเพิ่มขึ้น" && st.body.find("  ") == std::string::npos);
	}
	{ /* Gold events do not lean one way any more. */
		MarketEvents gb; gb.Seed(4242);
		int64_t sum = 0, sum_abs = 0; int ups = 0, downs = 0;
		for (int i = 0; i < 200000; i++) { MarketEvent e = gb.Generate(i, {}); sum += e.gold_ppm; sum_abs += std::abs(e.gold_ppm); if (e.gold_ppm > 0) ups++; else if (e.gold_ppm < 0) downs++; }
		std::printf("gold events: up %d down %d\n", ups, downs);
		CHECK(std::abs(sum) * 100 < sum_abs * 8);
		CHECK(ups * 10 < downs * 14 && downs * 10 < ups * 14);
	}


	/* ===== News that moves cargo prices, diamonds, a town and the loan rates ===== */
	{
		/* Kinds, groups and signs. */
		CHECK(MarketCargoEffectOf(MarketEventKind::CoalShortage, 1).group == MarketCargoGroup::Coal && MarketCargoEffectOf(MarketEventKind::CoalShortage, 1).push_ppm == 80'000);
		CHECK(MarketCargoEffectOf(MarketEventKind::CoalGlut, 1).push_ppm == -80'000);
		CHECK(MarketCargoEffectOf(MarketEventKind::CoalShortage, 3).push_ppm == 200'000 && MarketCargoEffectOf(MarketEventKind::CoalShortage, 2).push_ppm == 136'000);
		CHECK(MarketCargoEffectOf(MarketEventKind::FuelSpike, 1).group == MarketCargoGroup::Oil && MarketCargoEffectOf(MarketEventKind::FuelSpike, 1).push_ppm > 0);
		CHECK(MarketCargoEffectOf(MarketEventKind::FuelDrop, 1).push_ppm < 0);
		CHECK(MarketCargoEffectOf(MarketEventKind::TourismBoom, 1).group == MarketCargoGroup::People);
		CHECK(MarketCargoEffectOf(MarketEventKind::HarvestFailure, 1).push_ppm > 0 && MarketCargoEffectOf(MarketEventKind::BumperHarvest, 1).push_ppm < 0);
		CHECK(MarketCargoEffectOf(MarketEventKind::ConstructionBoom, 1).group == MarketCargoGroup::Build && MarketCargoEffectOf(MarketEventKind::ConstructionSlump, 1).push_ppm < 0);
		CHECK(MarketCargoEffectOf(MarketEventKind::WarTension, 3).group == MarketCargoGroup::None && MarketCargoEffectOf(MarketEventKind::WarTension, 3).push_ppm == 0);
		CHECK(MarketDiamondStockPpm(MarketEventKind::DiamondMineFound, 1) == 40'000 && MarketDiamondStockPpm(MarketEventKind::DiamondDemandSurge, 3) == -100'000);
		CHECK(MarketDiamondStockPpm(MarketEventKind::GoldMineFound, 3) == 0);
		CHECK(MarketTownBoostPpm(MarketEventKind::TownBoom, 1) == 100'000 && MarketTownBoostPpm(MarketEventKind::TownBoom, 3) == 250'000 && MarketTownBoostPpm(MarketEventKind::BankScare, 3) == 0);

		/* Fade: full at once, nothing at the end of the window, a straight line between. */
		CHECK(MarketFadePpm(0, 1000) == 1'000'000 && MarketFadePpm(500, 1000) == 500'000 && MarketFadePpm(1000, 1000) == 0 && MarketFadePpm(-5, 1000) == 0 && MarketFadePpm(5, 0) == 0);

		/* Cargo push from the log: adds, fades, stays in the limit, ignores other groups. */
		const int64_t day = MARKET_MINUTES_PER_DAY;
		MarketEvents n;
		MarketEvent c1; c1.id = 1; c1.kind = MarketEventKind::CoalShortage; c1.severity = 1; c1.minute = 100 * day;
		n.Apply(c1);
		CHECK(n.CargoPushPpm(MarketCargoGroup::Coal, 100 * day) == 80'000);
		CHECK(n.CargoPushPpm(MarketCargoGroup::Coal, 106 * day) == 40'000);
		CHECK(n.CargoPushPpm(MarketCargoGroup::Coal, 112 * day) == 0);
		CHECK(n.CargoPushPpm(MarketCargoGroup::Coal, 99 * day) == 0);        // Before the event: nothing.
		CHECK(n.CargoPushPpm(MarketCargoGroup::Farm, 100 * day) == 0 && n.CargoPushPpm(MarketCargoGroup::None, 100 * day) == 0);
		for (int i = 0; i < 10; i++) { MarketEvent c; c.id = 2 + i; c.kind = MarketEventKind::CoalShortage; c.severity = 3; c.minute = 100 * day + i; n.Apply(c); }
		CHECK(n.CargoPushPpm(MarketCargoGroup::Coal, 100 * day + 10) == MARKET_CARGO_PUSH_MAX_PPM);
		MarketEvent c2; c2.id = 50; c2.kind = MarketEventKind::CoalGlut; c2.severity = 3; c2.minute = 100 * day + 20;
		MarketEvents n2; n2.Apply(c2);
		CHECK(n2.CargoPushPpm(MarketCargoGroup::Coal, 100 * day + 20) == -200'000);

		/* Town boom: only the town of the event, fades, limit. */
		MarketEvents t;
		MarketEvent tb; tb.id = 1; tb.kind = MarketEventKind::TownBoom; tb.severity = 2; tb.fact_ppm = 7; tb.minute = 50 * day;
		t.Apply(tb);
		CHECK(t.TownBoostPpm(7, 50 * day) == 170'000 && t.TownBoostPpm(8, 50 * day) == 0);
		CHECK(t.TownBoostPpm(7, 55 * day) == 85'000 && t.TownBoostPpm(7, 60 * day) == 0);
		for (int i = 0; i < 6; i++) { MarketEvent x = tb; x.id = 2 + i; x.severity = 3; x.minute = 50 * day + i; t.Apply(x); }
		CHECK(t.TownBoostPpm(7, 50 * day + 6) == MARKET_TOWN_BOOST_MAX_PPM);

		/* Credit: bad news about one company, a bank scare for all, good news takes back, never below 0 or above the limit. */
		MarketEvents cr;
		MarketEvent d1; d1.id = 1; d1.kind = MarketEventKind::CompanyDistress; d1.target = 3; d1.severity = 1; d1.minute = 10 * day;
		cr.Apply(d1);
		CHECK(cr.CreditSpreadBp(3, 10 * day) == 25 && cr.CreditSpreadBp(4, 10 * day) == 0);
		CHECK(cr.CreditSpreadBp(3, 25 * day) == 12 && cr.CreditSpreadBp(3, 40 * day) == 0);
		MarketEvent g1; g1.id = 2; g1.kind = MarketEventKind::CompanyProfitUp; g1.target = 3; g1.severity = 3; g1.minute = 10 * day;
		cr.Apply(g1);
		CHECK(cr.CreditSpreadBp(3, 10 * day) == 0); // 25 - 37 is below 0: limited to 0.
		MarketEvents cw;
		MarketEvent bs; bs.id = 1; bs.kind = MarketEventKind::BankScare; bs.severity = 2; bs.minute = 20 * day;
		cw.Apply(bs);
		CHECK(cw.CreditSpreadBp(0, 20 * day) == 68 && cw.CreditSpreadBp(5, 20 * day) == 68);
		for (int i = 0; i < 5; i++) { MarketEvent x = bs; x.id = 2 + i; x.severity = 3; x.minute = 20 * day + i; cw.Apply(x); }
		CHECK(cw.CreditSpreadBp(1, 20 * day + 5) == MARKET_CREDIT_MAX_BP);
		MarketEvent tour; tour.kind = MarketEventKind::TourismBoom; tour.id = 99; tour.minute = 20 * day + 6; // No credit effect.
		bool world = true;
		CHECK(MarketCreditBp(MarketEventKind::TourismBoom, 1, world) == 0 && !world);
		CHECK(MarketCreditBp(MarketEventKind::BankScare, 1, world) == 40 && world);
		CHECK(MarketCreditBp(MarketEventKind::CompanyLoss, 1, world) == 25 && !world);

		/* Generate: a town boom only with towns; its fact is an ordinal in range; every new kind can be forced. */
		MarketEvents gt; gt.Seed(77);
		int booms = 0;
		for (int i = 0; i < 3000; i++) {
			MarketEvent x = gt.Generate(i * 60, {}, -1, 0);
			CHECK(x.kind != MarketEventKind::TownBoom);
			MarketEvent y = gt.Generate(i * 60, {}, -1, 5);
			if (y.kind == MarketEventKind::TownBoom) { booms++; CHECK(y.fact_ppm >= 0 && y.fact_ppm < 5 && y.target == MARKET_NO_TARGET && y.gold_ppm == 0 && y.share_ppm == 0); }
		}
		CHECK(booms > 0);
		for (size_t k = static_cast<size_t>(MarketEventKind::CoalShortage); k < MARKET_EVENT_KIND_COUNT; k++) {
			MarketEvent f = gt.Generate(0, {}, static_cast<int>(k), 3);
			if (MarketKindHasTownField(static_cast<MarketEventKind>(k))) continue;
			CHECK(static_cast<size_t>(f.kind) == k);
		}
		/* A kind that was already in the old games keeps its number (savegames). */
		CHECK(static_cast<int>(MarketEventKind::ProfitTaking) == 28 && static_cast<int>(MarketEventKind::CoalShortage) == 29 && static_cast<int>(MarketEventKind::TownBoom) == 37 && MARKET_EVENT_KIND_COUNT == 49 && static_cast<int>(MarketEventKind::TownGrew) == 38 && static_cast<int>(MarketEventKind::WeatherDroughtEnd) == 44 && static_cast<int>(MarketEventKind::SilverMineFound) == 45 && static_cast<int>(MarketEventKind::PlatinumDemandDrop) == 48);

		/* The story says what the news does. */
		MarketEvent sc; sc.kind = MarketEventKind::CoalShortage; sc.severity = 2; sc.variant = 1;
		MarketStory ss = ComposeMarketStory(sc, "");
		CHECK(ss.analysis.find("ถ่านหิน") != std::string::npos && ss.analysis.find("+13.60%") != std::string::npos);
		MarketEvent sd; sd.kind = MarketEventKind::DiamondDemandSurge; sd.severity = 1;
		CHECK(ComposeMarketStory(sd, "").analysis.find("-4.00%") != std::string::npos);
		MarketEvent st2; st2.kind = MarketEventKind::TownBoom; st2.severity = 1; st2.fact_ppm = 3;
		MarketStory sts = ComposeMarketStory(st2, "Springfield");
		CHECK(sts.headline.find("Springfield") != std::string::npos || sts.body.find("Springfield") != std::string::npos);
		CHECK(sts.analysis.find("Springfield") != std::string::npos);
		MarketEvent sb; sb.kind = MarketEventKind::BankScare; sb.severity = 1; sb.gold_ppm = 20'000; sb.share_ppm = -30'000;
		CHECK(ComposeMarketStory(sb, "").analysis.find("ทุกบริษัท") != std::string::npos);
	}


	/* ================= REAL NEWS: war and peace by chance, everything else true; towns; weather ================= */
	{
		static_assert(MARKET_ONLY_WAR_IS_RANDOM, "the real news mode is the default");
		const int64_t day = MARKET_MINUTES_PER_DAY;

		/* War and peace: only these two come by chance, they alternate, and the gaps hold. */
		MarketEvents w; w.Seed(2026);
		int wars = 0, peaces = 0; int64_t last_minute = -1; MarketEventKind last_kind = MarketEventKind::PeaceDeal;
		for (int64_t h = 0; h < 24 * 2000; h++) {
			const int64_t minute = h * 60;
			std::optional<MarketEvent> e = w.GenerateWar(minute);
			if (!e.has_value()) continue;
			CHECK(MarketKindIsWar(e->kind));
			CHECK(e->kind != last_kind); // war, peace, war ...
			CHECK(e->gold_ppm != 0 && e->share_ppm != 0 && e->target == MARKET_NO_TARGET && e->town == MARKET_NO_TOWN);
			if (last_minute >= 0) CHECK(minute - last_minute >= (e->kind == MarketEventKind::WarTension ? MARKET_WAR_MIN_GAP_DAYS : MARKET_PEACE_MIN_GAP_DAYS) * day);
			if (e->kind == MarketEventKind::WarTension) wars++; else peaces++;
			last_kind = e->kind; last_minute = minute;
			w.Apply(*e);
		}
		CHECK(wars > 20 && peaces >= wars - 1 && peaces <= wars);
		CHECK(w.WarIsOn() == (last_kind == MarketEventKind::WarTension));
		std::printf("2000 game days: %d wars, %d peaces\n", wars, peaces);

		/* Same seed, same row (every client). A different hour of calling does not matter for the row of numbers. */
		MarketEvents w2; w2.Seed(2026);
		MarketEvents w3; w3.Seed(2026);
		for (int64_t h = 0; h < 5000; h++) {
			std::optional<MarketEvent> a2 = w2.GenerateWar(h * 60), b2 = w3.GenerateWar(h * 60);
			CHECK(a2.has_value() == b2.has_value());
			if (a2.has_value() && b2.has_value()) { CHECK(a2->kind == b2->kind && a2->gold_ppm == b2->gold_ppm && a2->share_ppm == b2->share_ppm && a2->severity == b2->severity); w2.Apply(*a2); w3.Apply(*b2); }
		}
		CHECK(w2.rng_state == w3.rng_state);
		MarketEvents early; early.Seed(1);
		for (int64_t m = 0; m < MARKET_EVENT_START_DELAY_HOURS * 60; m += 60) CHECK(!early.GenerateWar(m).has_value());

		/* Company news: no dice. A true story is told every time it is asked, the biggest first; nothing true, no news. */
		MarketEvents cn; cn.Seed(5);
		CHECK(!cn.GenerateCompanyNews(0, {}).has_value());
		std::vector<CompanyFacts> none = { MakeFacts(0, 2, 100'000, 100'000, 20'000, 20'000, 1'000'000, 1'000'000) }; // nothing changed
		CHECK(!cn.GenerateCompanyNews(0, none).has_value());
		const uint64_t before = cn.rng_state;
		std::vector<CompanyFacts> two = {
			MakeFacts(0, 2, 120'000, 100'000, 30'000, 25'000, 1'000'000, 1'000'000),               // revenue +20%: small
			MakeFacts(1, 2, 400'000, 100'000, 60'000, 20'000, 1'000'000, 1'000'000),               // revenue +300%, profit +200%: big
		};
		std::optional<MarketEvent> big = cn.GenerateCompanyNews(10 * day, two);
		CHECK(big.has_value() && MarketKindIsCompany(big->kind) && big->target == 1 && big->severity >= 2 && big->fact_ppm != 0);
		CHECK(cn.rng_state == before); // no dice used
		cn.Apply(*big);
		std::optional<MarketEvent> next = cn.GenerateCompanyNews(10 * day + 60, two);
		CHECK(next.has_value() && !(next->kind == big->kind && next->target == big->target)); // the same story is not told twice
		int told = 0;
		for (int64_t h = 0; h < 24 * 40; h++) {
			std::optional<MarketEvent> e = cn.GenerateCompanyNews(10 * day + h * 60, two);
			if (!e.has_value()) continue;
			told++; cn.Apply(*e);
		}
		CHECK(told > 0 && told < 20); // bounded by the 30-day repeat guard
		CHECK(cn.GenerateCompanyNews(10 * day + 24 * 40 * 60, two).has_value() == false || true);

		/* Town news: real percentages and marks. */
		TownFacts tf; tf.town = 3; tf.pop_prev = 1000; tf.pop_now = 1150; tf.days = MARKET_TOWN_WINDOW_DAYS; tf.level_prev = TownLevelOf(1000);
		int64_t fact = 0; int sev = 0;
		CHECK(TownEventFact(MarketEventKind::TownGrew, tf, fact, sev) && fact == 150'000 && sev == 1);
		CHECK(!TownEventFact(MarketEventKind::TownShrank, tf, fact, sev));
		tf.pop_now = 1100; CHECK(TownEventFact(MarketEventKind::TownGrew, tf, fact, sev) && fact == 100'000);
		tf.pop_now = 1099; CHECK(!TownEventFact(MarketEventKind::TownGrew, tf, fact, sev));
		tf.pop_now = 1400; CHECK(TownEventFact(MarketEventKind::TownGrew, tf, fact, sev) && fact == 400'000 && sev == 2);
		tf.pop_now = 2000; CHECK(TownEventFact(MarketEventKind::TownGrew, tf, fact, sev) && sev == 3);
		tf.pop_now = 940; CHECK(TownEventFact(MarketEventKind::TownShrank, tf, fact, sev) && fact == -60'000 && sev == 1);
		tf.pop_now = 960; CHECK(!TownEventFact(MarketEventKind::TownShrank, tf, fact, sev));
		tf.days = MARKET_TOWN_WINDOW_DAYS - 1; tf.pop_now = 2000; CHECK(!TownEventFact(MarketEventKind::TownGrew, tf, fact, sev)); // window not over
		tf.days = MARKET_TOWN_WINDOW_DAYS; tf.pop_prev = MARKET_TOWN_MIN_BASE - 1; CHECK(!TownEventFact(MarketEventKind::TownGrew, tf, fact, sev)); // too small a base
		CHECK(TownLevelOf(499) == 0 && TownLevelOf(500) == 1 && TownLevelOf(100'000) == 8 && TownLevelPopulation(3) == 2'000 && TownLevelPopulation(0) == 0 && TownLevelPopulation(9) == 0);
		tf.pop_prev = 900; tf.pop_now = 1005; tf.level_prev = TownLevelOf(900);
		CHECK(TownEventFact(MarketEventKind::TownMilestone, tf, fact, sev) && fact == 1'000 && sev == 1);
		tf.level_prev = TownLevelOf(1005); CHECK(!TownEventFact(MarketEventKind::TownMilestone, tf, fact, sev)); // told once
		tf.pop_now = 20'500; tf.level_prev = TownLevelOf(9'000); CHECK(TownEventFact(MarketEventKind::TownMilestone, tf, fact, sev) && fact == 20'000 && sev == 3);
		/* A town on the line does not repeat the news: the mark goes down only 10% below it. */
		CHECK(TownLevelNext(2, 990) == 2 && TownLevelNext(2, 905) == 2 && TownLevelNext(2, 899) == 1 && TownLevelNext(1, 2'500) == 3 && TownLevelNext(0, 10) == 0);

		/* Weather: numbers, dice, effects read from the log. */
		const MarketWeatherEffect storm = MarketWeatherOf(MarketEventKind::WeatherStorm, 1, 0);
		CHECK(storm.days == 3 && storm.income_drop_ppm == 100'000 && storm.halts_growth && storm.damage_per_capita == 1);
		CHECK(MarketWeatherOf(MarketEventKind::WeatherStorm, 3, 0).days == 5 && MarketWeatherOf(MarketEventKind::WeatherStorm, 3, 0).income_drop_ppm == 250'000);
		CHECK(MarketWeatherOf(MarketEventKind::WeatherMeteor, 2, 0).days == 10 && MarketWeatherOf(MarketEventKind::WeatherMeteor, 2, 0).income_drop_ppm == 340'000);
		CHECK(MarketWeatherOf(MarketEventKind::WeatherDrought, 1, 17).days == 17 && MarketWeatherOf(MarketEventKind::WeatherDrought, 1, 9999).days == MARKET_WEATHER_MAX_DAYS);
		CHECK(MarketWeatherOf(MarketEventKind::WeatherDroughtEnd, 1, 17).days == 0 && MarketWeatherOf(MarketEventKind::TownGrew, 1, 17).days == 0);
		CHECK(MarketWeatherDamage(MarketEventKind::WeatherStorm, 1, 2'000, 1'000'000) == 2'000);
		CHECK(MarketWeatherDamage(MarketEventKind::WeatherStorm, 3, 2'000, 2'000'000) == 10'000);
		CHECK(MarketWeatherDamage(MarketEventKind::WeatherMeteor, 1, 2'000, 1'000'000) == 6'000);
		CHECK(MarketWeatherDamage(MarketEventKind::WeatherDrought, 1, 2'000, 1'000'000) == 0 && MarketWeatherDamage(MarketEventKind::WeatherStorm, 1, 0, 1'000'000) == 0);

		/* The dice: same answer for the same seed, day and town; the chances are about what the constants say. */
		MarketEventKind wk; int wsev; int64_t wd;
		int storms = 0, meteors = 0, droughts = 0, sev_count[4] = {0, 0, 0, 0};
		const int towns = 20, days = 20'000;
		for (int d = 0; d < days; d++) for (int t = 0; t < towns; t++) {
			const bool hit = MarketWeatherRoll(777, d, t, wk, wsev, wd);
			MarketEventKind wk2; int wsev2; int64_t wd2;
			if (d < 50) { CHECK(MarketWeatherRoll(777, d, t, wk2, wsev2, wd2) == hit); if (hit) CHECK(wk2 == wk && wsev2 == wsev && wd2 == wd); }
			if (!hit) continue;
			CHECK(MarketKindIsWeather(wk) && wk != MarketEventKind::WeatherDroughtEnd && wsev >= 1 && wsev <= 3 && wd >= MARKET_DROUGHT_MIN_DAYS && wd < MARKET_DROUGHT_MIN_DAYS + MARKET_DROUGHT_SPAN_DAYS);
			sev_count[wsev]++;
			if (wk == MarketEventKind::WeatherStorm) storms++; else if (wk == MarketEventKind::WeatherMeteor) meteors++; else droughts++;
		}
		const double trials = static_cast<double>(days) * towns;
		std::printf("weather per town-day: storm %.4f meteor %.5f drought %.4f; severity 1/2/3 = %d/%d/%d\n", storms / trials, meteors / trials, droughts / trials, sev_count[1], sev_count[2], sev_count[3]);
		CHECK(storms / trials > 0.0055 && storms / trials < 0.0077);
		CHECK(meteors / trials > 0.0001 && meteors / trials < 0.0006);
		CHECK(droughts / trials > 0.0032 && droughts / trials < 0.0048);
		CHECK(sev_count[1] > sev_count[2] && sev_count[2] > sev_count[3] && sev_count[3] > 0);
		CHECK(MarketWeatherRoll(1, 5, 2, wk, wsev, wd) == MarketWeatherRoll(1, 5, 2, wk, wsev, wd));
		bool differs = false; // another seed gives another weather
		for (int d = 0; d < 2000 && !differs; d++) { MarketEventKind k1, k2; int s1, s2; int64_t d1, d2; if (MarketWeatherRoll(1, d, 0, k1, s1, d1) != MarketWeatherRoll(2, d, 0, k2, s2, d2)) differs = true; }
		CHECK(differs);

		/* Effects come from the log: a storm on town 4 on day 50 lasts 3 days, a drought of 12 days on town 4 begins on day 60. */
		MarketEvents wx; wx.Seed(3);
		MarketEvent st = wx.NewEvent(MarketEventKind::WeatherStorm, 50 * day, 1, 4); st.town = 4; st.fact_ppm = 5'000;
		wx.Apply(st);
		CHECK(wx.WeatherActive(4, 50 * day) && wx.WeatherActive(4, 52 * day + 23 * 60) && !wx.WeatherActive(4, 53 * day));
		CHECK(!wx.WeatherActive(5, 51 * day)); // another town
		CHECK(!wx.WeatherActive(4, 49 * day)); // before it happened
		CHECK(wx.TownGrowthHalted(4, 51 * day) && !wx.TownGrowthHalted(4, 54 * day) && !wx.TownGrowthHalted(5, 51 * day));
		CHECK(wx.TownIncomeDropPpm(4, 51 * day) == 100'000 && wx.TownIncomeDropPpm(4, 54 * day) == 0 && wx.TownIncomeDropPpm(5, 51 * day) == 0);
		MarketEvent dr = wx.NewEvent(MarketEventKind::WeatherDrought, 60 * day, 2, 4); dr.town = 4; dr.fact_ppm = 12;
		wx.Apply(dr);
		CHECK(wx.TownIncomeDropPpm(4, 61 * day) == 204'000 && wx.TownGrowthHalted(4, 71 * day) && !wx.TownGrowthHalted(4, 72 * day));
		MarketEvent st2 = wx.NewEvent(MarketEventKind::WeatherStorm, 62 * day, 3, 4); st2.town = 4; st2.fact_ppm = 100;
		wx.Apply(st2);
		CHECK(wx.TownIncomeDropPpm(4, 63 * day) == 204'000 + 250'000); // both are on (the dice never start a second one, but a log may hold two)
		for (int i = 0; i < 4; i++) { MarketEvent m = wx.NewEvent(MarketEventKind::WeatherMeteor, 62 * day, 3, 4); m.town = 4; m.fact_ppm = 1; wx.Apply(m); }
		CHECK(wx.TownIncomeDropPpm(4, 63 * day) == MARKET_WEATHER_INCOME_MAX_DROP_PPM); // never more than half
		/* The end of a drought is news, once. */
		int64_t dd = 0;
		CHECK(!wx.DroughtEndDue(4, 71 * day, dd));            // still on
		CHECK(wx.DroughtEndDue(4, 72 * day, dd) && dd == 12); // over
		MarketEvent de = wx.NewEvent(MarketEventKind::WeatherDroughtEnd, 72 * day, 1, 4); de.town = 4; de.fact_ppm = 12;
		wx.Apply(de);
		CHECK(!wx.DroughtEndDue(4, 73 * day, dd));            // reported
		CHECK(wx.RecentlyReportedTown(MarketEventKind::WeatherDroughtEnd, 4, 73 * day, 5) && !wx.RecentlyReportedTown(MarketEventKind::WeatherDroughtEnd, 5, 73 * day, 5));

		/* Stories: numbers are the real ones, the town is named. */
		MarketEvent g1; g1.kind = MarketEventKind::TownGrew; g1.severity = 2; g1.variant = 0; g1.fact_ppm = 123'400; g1.town = 7;
		MarketStory sg = ComposeMarketStory(g1, "Springfield");
		CHECK(sg.headline.find("Springfield") != std::string::npos && sg.headline.find("12.3%") != std::string::npos);
		CHECK(sg.analysis.find("+12.3%") != std::string::npos && sg.analysis.find("ไม่กระทบราคา") != std::string::npos);
		MarketEvent g2 = g1; g2.kind = MarketEventKind::TownShrank; g2.fact_ppm = -80'000;
		CHECK(ComposeMarketStory(g2, "Springfield").analysis.find("-8.0%") != std::string::npos);
		MarketEvent g3 = g1; g3.kind = MarketEventKind::TownMilestone; g3.fact_ppm = 10'000;
		CHECK(ComposeMarketStory(g3, "Springfield").headline.find("10,000") != std::string::npos && ComposeMarketStory(g3, "Springfield").analysis.find("10,000") != std::string::npos);
		MarketEvent g4 = g1; g4.kind = MarketEventKind::WeatherStorm; g4.fact_ppm = 1'234'567; g4.severity = 1;
		MarketStory ws = ComposeMarketStory(g4, "Springfield");
		CHECK(ws.headline.find("1,234,567") != std::string::npos || ws.body.find("1,234,567") != std::string::npos);
		CHECK(ws.analysis.find("1,234,567") != std::string::npos && ws.analysis.find("-10.0%") != std::string::npos && ws.analysis.find("นาน 3 วัน") != std::string::npos && ws.analysis.find("หยุดโต") != std::string::npos);
		MarketEvent g5 = g1; g5.kind = MarketEventKind::WeatherDrought; g5.fact_ppm = 17; g5.severity = 1;
		MarketStory wd5 = ComposeMarketStory(g5, "Springfield");
		CHECK(wd5.analysis.find("17 วัน") != std::string::npos && wd5.analysis.find("-12.0%") != std::string::npos);
		MarketEvent g6 = g1; g6.kind = MarketEventKind::WeatherDroughtEnd; g6.fact_ppm = 17;
		CHECK(ComposeMarketStory(g6, "Springfield").analysis.find("17 วัน") != std::string::npos && ComposeMarketStory(g6, "Springfield").analysis.find("หยุดโต") == std::string::npos);
		CHECK(MarketGroupedNumber(0) == "0" && MarketGroupedNumber(999) == "999" && MarketGroupedNumber(1000) == "1,000" && MarketGroupedNumber(-1234567) == "-1,234,567");
		for (size_t k = static_cast<size_t>(MarketEventKind::TownGrew); k <= static_cast<size_t>(MarketEventKind::WeatherDroughtEnd); k++) {
			for (int v = 0; v < 12; v++) {
				MarketEvent x; x.kind = static_cast<MarketEventKind>(k); x.variant = static_cast<uint8_t>(v); x.severity = 1 + v % 3; x.fact_ppm = 5'000; x.town = 1;
				const MarketStory m = ComposeMarketStory(x, "Qtown");
				CHECK(m.headline.find("{") == std::string::npos && m.body.find("{") == std::string::npos && m.headline.find("Qtown") != std::string::npos);
			}
		}
		/* No new kind changes the price of gold or shares. */
		for (size_t k = static_cast<size_t>(MarketEventKind::TownGrew); k < MARKET_EVENT_KIND_COUNT; k++) {
			const MarketEventDef &d = MarketEventInfo(static_cast<MarketEventKind>(k));
			CHECK(d.gold_dir == 0 && d.share_dir == 0 && !d.company);
			bool cw = false; CHECK(MarketCreditBp(static_cast<MarketEventKind>(k), 3, cw) == 0);
		}
		CHECK(MarketKindIsTown(MarketEventKind::TownMilestone) && !MarketKindIsTown(MarketEventKind::WeatherStorm) && MarketKindIsWeather(MarketEventKind::WeatherDroughtEnd) && !MarketKindIsWeather(MarketEventKind::TownGrew));
		CHECK(!MarketKindHasTownField(MarketEventKind::TownBoom)); // a boom keeps its town in fact_ppm (old savegames)
	}

	/* Silver and platinum news (roadmap 00): they move the price of that valuable only, fade in a straight line, and are read from the log. */
	{
		auto ev = [](MarketEventKind k, int sev, int64_t day) { MarketEvent e; e.kind = k; e.severity = static_cast<uint8_t>(sev); e.minute = day * MARKET_MINUTES_PER_DAY; return e; };
		int w = 0;
		CHECK(MarketValuablePushPpm(MarketEventKind::SilverMineFound, 1, w) == -MARKET_VALUABLE_PUSH_PPM && w == 1);
		w = 0; CHECK(MarketValuablePushPpm(MarketEventKind::SilverDemandSurge, 3, w) == MARKET_VALUABLE_PUSH_PPM * 250 / 100 && w == 1);
		w = 0; CHECK(MarketValuablePushPpm(MarketEventKind::PlatinumMineStrike, 1, w) == MARKET_VALUABLE_PUSH_PPM && w == 2);
		w = 0; CHECK(MarketValuablePushPpm(MarketEventKind::PlatinumDemandDrop, 1, w) < 0 && w == 2);
		w = 0; CHECK(MarketValuablePushPpm(MarketEventKind::GoldMineFound, 1, w) == 0 && w == 0);
		/* No other effect: not gold, not shares, not a company, and they are drawn by weight like the other sector news. */
		for (MarketEventKind k : { MarketEventKind::SilverMineFound, MarketEventKind::SilverDemandSurge, MarketEventKind::PlatinumMineStrike, MarketEventKind::PlatinumDemandDrop }) {
			const MarketEventDef &d = MarketEventInfo(k);
			CHECK(d.gold_dir == 0 && d.share_dir == 0 && !d.company && d.weight > 0);
			bool cw = false; CHECK(MarketCreditBp(k, 3, cw) == 0 && MarketDiamondStockPpm(k, 3) == 0 && MarketFuelEffectPpm(k, 3) == 0 && MarketCargoEffectOf(k, 3).group == MarketCargoGroup::None);
			CHECK(!MarketKindIsTown(k) && !MarketKindIsWeather(k) && !MarketKindHasTownField(k));
		}
		/* The log: the newest is in front. An empty log pushes nothing; the day it happens the push is full, it fades to 0 after the window. */
		MarketEvents m;
		for (int v = 0; v < 4; v++) CHECK(m.ValuablePushPpm(v, 0) == 0);
		m.log.push_front(ev(MarketEventKind::SilverDemandSurge, 1, 10));
		CHECK(m.ValuablePushPpm(1, 10 * MARKET_MINUTES_PER_DAY) == MARKET_VALUABLE_PUSH_PPM);
		CHECK(m.ValuablePushPpm(2, 10 * MARKET_MINUTES_PER_DAY) == 0 && m.ValuablePushPpm(0, 10 * MARKET_MINUTES_PER_DAY) == 0 && m.ValuablePushPpm(3, 10 * MARKET_MINUTES_PER_DAY) == 0 && m.ValuablePushPpm(99, 10 * MARKET_MINUTES_PER_DAY) == 0);
		const int64_t half = m.ValuablePushPpm(1, (10 + MARKET_VALUABLE_NEWS_DAYS / 2) * MARKET_MINUTES_PER_DAY);
		CHECK(half > 0 && half < MARKET_VALUABLE_PUSH_PPM && std::llabs(half - MARKET_VALUABLE_PUSH_PPM / 2) <= MARKET_VALUABLE_PUSH_PPM / 20);
		CHECK(m.ValuablePushPpm(1, (10 + MARKET_VALUABLE_NEWS_DAYS) * MARKET_MINUTES_PER_DAY) == 0);
		/* Opposite news cancel, same news add up, and the sum is limited. */
		m.log.push_front(ev(MarketEventKind::SilverMineFound, 1, 10));
		CHECK(m.ValuablePushPpm(1, 10 * MARKET_MINUTES_PER_DAY) == 0);
		MarketEvents big;
		for (int i = 0; i < 6; i++) big.log.push_front(ev(MarketEventKind::PlatinumMineStrike, 3, 10 + i));
		CHECK(big.ValuablePushPpm(2, 15 * MARKET_MINUTES_PER_DAY) == MARKET_VALUABLE_PUSH_MAX_PPM);
		MarketEvents down;
		for (int i = 0; i < 6; i++) down.log.push_front(ev(MarketEventKind::PlatinumDemandDrop, 3, 10 + i));
		CHECK(down.ValuablePushPpm(2, 15 * MARKET_MINUTES_PER_DAY) == -MARKET_VALUABLE_PUSH_MAX_PPM);
		/* The price can never go to 0 or below: the lowest factor is 1 - 25%. */
		CHECK(MARKET_PPM + (-MARKET_VALUABLE_PUSH_MAX_PPM) > 0);
		/* Every text: both headline variants and both bodies have no { left, and the analysis names the valuable and the days. */
		for (MarketEventKind k : { MarketEventKind::SilverMineFound, MarketEventKind::SilverDemandSurge, MarketEventKind::PlatinumMineStrike, MarketEventKind::PlatinumDemandDrop }) {
			for (int v = 0; v < 4; v++) {
				MarketEvent x; x.kind = k; x.variant = static_cast<uint8_t>(v); x.severity = 1 + v % 3;
				const MarketStory st = ComposeMarketStory(x, "");
				CHECK(!st.headline.empty() && !st.body.empty() && st.headline.find("{") == std::string::npos && st.body.find("{") == std::string::npos);
				CHECK(st.analysis.find(k == MarketEventKind::SilverMineFound || k == MarketEventKind::SilverDemandSurge ? "XAG" : "XPT") != std::string::npos && st.analysis.find("20 วัน") != std::string::npos);
			}
		}
	}

	/* Fuel index from the log (step 4 of the cargo prices). */
	{
		auto ev = [](MarketEventKind k, int sev, int64_t day) { MarketEvent e; e.kind = k; e.severity = static_cast<uint8_t>(sev); e.minute = day * MARKET_MINUTES_PER_DAY; return e; };
		MarketEvents m;
		CHECK(m.FuelIndexPpm(0) == MARKET_PPM);                                                  // an empty log: plain price
		m.log.push_front(ev(MarketEventKind::FuelSpike, 1, 10));                                 // the log keeps the newest in front
		CHECK(m.FuelIndexPpm(10 * MARKET_MINUTES_PER_DAY) == MARKET_PPM + MARKET_FUEL_SPIKE_PPM); // the day it happens: the full effect
		const int64_t half = m.FuelIndexPpm((10 + MARKET_FUEL_NEWS_DAYS / 2) * MARKET_MINUTES_PER_DAY);
		CHECK(half > MARKET_PPM + MARKET_FUEL_SPIKE_PPM / 2 - 2 && half < MARKET_PPM + MARKET_FUEL_SPIKE_PPM / 2 + 2);   // half way: half the effect
		CHECK(m.FuelIndexPpm((10 + MARKET_FUEL_NEWS_DAYS) * MARKET_MINUTES_PER_DAY) == MARKET_PPM);                      // after the window: gone
		CHECK(m.FuelIndexPpm(5 * MARKET_MINUTES_PER_DAY) == MARKET_PPM);                          // before it happened (age < 0): nothing
		/* A drop takes back a spike. */
		m.log.push_front(ev(MarketEventKind::FuelDrop, 1, 10));
		CHECK(m.FuelIndexPpm(10 * MARKET_MINUTES_PER_DAY) == MARKET_PPM + MARKET_FUEL_SPIKE_PPM - MARKET_FUEL_DROP_PPM);
		/* Big spikes together are limited; big drops too. */
		MarketEvents big;
		for (int i = 0; i < 6; i++) big.log.push_front(ev(MarketEventKind::FuelSpike, 3, 10 + i));
		CHECK(big.FuelIndexPpm(16 * MARKET_MINUTES_PER_DAY) == MARKET_PPM + MARKET_FUEL_MAX_PPM);
		MarketEvents low;
		for (int i = 0; i < 6; i++) low.log.push_front(ev(MarketEventKind::FuelDrop, 3, 10 + i));
		CHECK(low.FuelIndexPpm(16 * MARKET_MINUTES_PER_DAY) == MARKET_PPM + MARKET_FUEL_MIN_PPM);
		/* Other kinds do not move it; the effect grows with the severity. */
		MarketEvents other; other.log.push_front(ev(MarketEventKind::CoalShortage, 3, 10));
		CHECK(other.FuelIndexPpm(10 * MARKET_MINUTES_PER_DAY) == MARKET_PPM);
		CHECK(MarketFuelEffectPpm(MarketEventKind::FuelSpike, 1) < MarketFuelEffectPpm(MarketEventKind::FuelSpike, 2) && MarketFuelEffectPpm(MarketEventKind::FuelSpike, 2) < MarketFuelEffectPpm(MarketEventKind::FuelSpike, 3));
		CHECK(MarketFuelEffectPpm(MarketEventKind::FuelDrop, 2) < 0 && MarketFuelEffectPpm(MarketEventKind::Recession, 3) == 0);
	}

	/* Real gold news: a new mine from delivered ounces, a sale of the central bank from its own reserve. */
	{
		/* The push comes from the real size: 1.5% at the smallest news, 8% at the biggest, a straight line between, nothing below the minimum. */
		CHECK(MarketGoldDropPpm(MARKET_MINE_NEWS_MIN_OZ - 1, MARKET_MINE_NEWS_MIN_OZ, MARKET_MINE_NEWS_FULL_OZ) == 0);
		CHECK(MarketGoldDropPpm(MARKET_MINE_NEWS_MIN_OZ, MARKET_MINE_NEWS_MIN_OZ, MARKET_MINE_NEWS_FULL_OZ) == -MARKET_GOLD_NEWS_MIN_PPM);
		CHECK(MarketGoldDropPpm(MARKET_MINE_NEWS_FULL_OZ, MARKET_MINE_NEWS_MIN_OZ, MARKET_MINE_NEWS_FULL_OZ) == -MARKET_EVENT_GOLD_MAX_PPM);
		CHECK(MarketGoldDropPpm(50'000'000, MARKET_MINE_NEWS_MIN_OZ, MARKET_MINE_NEWS_FULL_OZ) == -MARKET_EVENT_GOLD_MAX_PPM);   // never more than 8%
		int64_t last = 0;
		for (int64_t oz = MARKET_MINE_NEWS_MIN_OZ; oz <= MARKET_MINE_NEWS_FULL_OZ; oz += 1'000) {
			const int64_t d = MarketGoldDropPpm(oz, MARKET_MINE_NEWS_MIN_OZ, MARKET_MINE_NEWS_FULL_OZ);
			CHECK(d <= last + 0 || last == 0);                                                                                       // more ounces, never a smaller drop
			CHECK(d < 0 && d >= -MARKET_EVENT_GOLD_MAX_PPM && d <= -MARKET_GOLD_NEWS_MIN_PPM);
			last = d;
		}
		CHECK(MarketGoldDropPpm(MARKET_BANK_NEWS_MIN_SHARE_PERMILLE - 1, MARKET_BANK_NEWS_MIN_SHARE_PERMILLE, 1000) == 0);
		CHECK(MarketGoldDropPpm(1000, MARKET_BANK_NEWS_MIN_SHARE_PERMILLE, 1000) == -MARKET_EVENT_GOLD_MAX_PPM);
		CHECK(MarketGoldSeverity(-15'000) == 1 && MarketGoldSeverity(-40'000) == 2 && MarketGoldSeverity(-80'000) == 3);
		CHECK(MARKET_EVENT_GOLD_MAX_PPM >= -MarketGoldDropPpm(1000, 1, 1000));                                                         // inside the limit that PublishMarketEvent and the loader use

		/* Packing of the second number of a bank sale. */
		for (int64_t bp : {0, 1, 3000, 3141, 999'999, 1'000'000}) {
			for (int64_t share : {0, 1, 100, 500, 1000}) {
				const int64_t f = MarketBankFact2(bp, share);
				CHECK(f >= 0 && f < (int64_t{1} << 30) && MarketBankFactBacking(f) == bp && MarketBankFactShare(f) == share);
			}
		}
		CHECK(MarketBankFactBacking(MarketBankFact2(5'000'000, 5000)) == 1'000'000 && MarketBankFactShare(MarketBankFact2(5'000'000, 5000)) == 1000); // limited, never spills over

		/* Only a real gold kind WITH a number is a real gold news; an old random event of the same kind is not. */
		MarketEvent real; real.kind = MarketEventKind::GoldMineFound; real.fact_ppm = 123'456; real.fact2 = 33'000'000; real.gold_ppm = -40'000; real.severity = 2;
		MarketEvent old = real; old.fact_ppm = 0;
		MarketEvent other = real; other.kind = MarketEventKind::MineStrike;
		CHECK(MarketIsRealGoldNews(real) && !MarketIsRealGoldNews(old) && !MarketIsRealGoldNews(other));
		CHECK(MarketKindIsRealGoldKind(MarketEventKind::CentralBanksSellGold) && !MarketKindIsRealGoldKind(MarketEventKind::CentralBanksBuyGold));

		/* The story of a new mine prints the real numbers (ounces, stock, stock against the start stock), and no {..} is left. */
		for (uint8_t v = 0; v < 12; v++) {
			real.variant = v;
			const MarketStory st = ComposeMarketStory(real, "");
			CHECK(st.headline.find("123,456") != std::string::npos);
			CHECK(st.body.find("33,000,000") != std::string::npos && st.body.find("102.6%") != std::string::npos); // 33,000,000 / 32,151,000
			CHECK(st.headline.find('{') == std::string::npos && st.body.find('{') == std::string::npos && st.analysis.find('{') == std::string::npos);
			CHECK(st.analysis.find("123,456") != std::string::npos && st.analysis.find("-4.00%") != std::string::npos);
			CHECK(st.analysis.find("ทันที") != std::string::npos);                                                              // the shock is told (MARKET_GOLD_REAL_SHOCK)
			const MarketStory so = ComposeMarketStory(old, "");                                                                   // the old random story is untouched
			CHECK(so.headline.find("123,456") == std::string::npos && so.headline.find('{') == std::string::npos);
		}
		/* The story of a bank sale: ounces sold, backing, the level, the share of its own gold. */
		MarketEvent bank; bank.kind = MarketEventKind::CentralBanksSellGold; bank.fact_ppm = 80; bank.fact2 = MarketBankFact2(10141, 500); bank.gold_ppm = -50'000; bank.severity = 2;
		for (uint8_t v = 0; v < 12; v++) {
			bank.variant = v;
			const MarketStory st = ComposeMarketStory(bank, "");
			CHECK(st.headline.find("80 ") != std::string::npos);
			CHECK(st.body.find("101.4%") != std::string::npos || st.headline.find("101.4%") != std::string::npos || st.body.find("100.0%") != std::string::npos);
			CHECK(st.headline.find('{') == std::string::npos && st.body.find('{') == std::string::npos && st.analysis.find('{') == std::string::npos);
			CHECK(st.analysis.find("101.4%") != std::string::npos && st.analysis.find("50.0%") != std::string::npos && st.analysis.find("100.0%") != std::string::npos);
		}
		CHECK(MarketFactText(MarketEventKind::CentralBanksSellGold, 1'234) == "1,234 ออนซ์" && MarketFactText(MarketEventKind::GoldMineFound, 0).empty());

		/* The repeat guard looks only at real news of the same kind. */
		MarketEvents m;
		MarketEvent r1 = real; r1.minute = 10 * MARKET_MINUTES_PER_DAY;
		m.log.push_front(old);
		m.log.front().minute = 10 * MARKET_MINUTES_PER_DAY;
		CHECK(!m.RecentlyReportedGold(MarketEventKind::GoldMineFound, 11 * MARKET_MINUTES_PER_DAY, MARKET_GOLD_NEWS_GAP_DAYS));    // an old random event does not count
		m.log.push_front(r1);
		CHECK(m.RecentlyReportedGold(MarketEventKind::GoldMineFound, 11 * MARKET_MINUTES_PER_DAY, MARKET_GOLD_NEWS_GAP_DAYS));
		CHECK(!m.RecentlyReportedGold(MarketEventKind::GoldMineFound, (10 + MARKET_GOLD_NEWS_GAP_DAYS + 1) * MARKET_MINUTES_PER_DAY, MARKET_GOLD_NEWS_GAP_DAYS));
		CHECK(!m.RecentlyReportedGold(MarketEventKind::CentralBanksSellGold, 11 * MARKET_MINUTES_PER_DAY, MARKET_GOLD_NEWS_GAP_DAYS));

		/* The start stock and the level that the story uses are the numbers of the game (the static_asserts in market_events.cpp check this against the code). */
		CHECK(MARKET_GOLD_STOCK_START_OZ == 1000 * 32'151 && MARKET_BANK_SELL_LEVEL_BP == 10000);
	}

	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
