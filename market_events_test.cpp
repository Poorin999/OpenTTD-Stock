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
			const bool other = MarketCargoEffectOf(kk, 1).push_ppm != 0 || MarketDiamondStockPpm(kk, 1) != 0 || MarketTownBoostPpm(kk, 1) != 0;
			CHECK(d.gold_dir != 0 || d.share_dir != 0 || other);
		}
		CHECK(d.gold_min <= d.gold_max && d.share_min <= d.share_max);
		const MarketStoryTexts &t = MarketStoryTextsOf(static_cast<MarketEventKind>(k));
		CHECK(t.headline[0] && t.headline[1] && t.body[0] && t.body[1]);
		CHECK((std::string(t.headline[0]).find("{C}") != std::string::npos) == (d.company || static_cast<MarketEventKind>(k) == MarketEventKind::TownBoom)); // A town boom names the town.
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
			CHECK(static_cast<size_t>(f.kind) == k);
		}
		/* A kind that was already in the old games keeps its number (savegames). */
		CHECK(static_cast<int>(MarketEventKind::ProfitTaking) == 28 && static_cast<int>(MarketEventKind::CoalShortage) == 29 && static_cast<int>(MarketEventKind::TownBoom) == 37 && MARKET_EVENT_KIND_COUNT == 38);

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

	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
