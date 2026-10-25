/* Stand-alone test of citizen_terminal.h (not part of the game build).
 * Build and run:  g++ -std=c++17 -Wall -Wextra -o /tmp/ctt citizen_terminal_test.cpp && /tmp/ctt */
#include <cstdio>
#include "citizen_terminal.h"
using namespace citizengold;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* A small terminal: 3 symbols with a random walk (hourly) and the 3 / 10 day averages worked out from daily closes. */
struct Sim {
	static constexpr size_t N = 3;
	int64_t price[N] = {2000, 50, 900};
	int64_t divisor[N] = {10, 10, 5};
	std::vector<int64_t> closes[N];
	TermQuote q[N];
	void Update(uint64_t seed, uint64_t hour)
	{
		for (size_t s = 0; s < N; s++) {
			const int64_t step = Pick(Mix(seed, s, hour, 99), 601) - 300;             // +-0.3% per hour
			const int64_t drift = (seed % 2 == 0) ? 0 : (Pick(Mix(seed, s, hour / 240, 7), 3) - 1) * 40; // odd seeds: a slow trend per 10 days
			price[s] = std::max<int64_t>(2, price[s] + price[s] * (step + drift) / 100'000);
			if (hour % 24 == 23) closes[s].push_back(price[s]);
			TermQuote &x = q[s];
			x.valid = true; x.mid = price[s]; x.bid = std::max<int64_t>(1, price[s] - 1); x.ask = price[s] + 1; x.divisor = divisor[s];
			x.fast = x.slow = 0;
			if (closes[s].size() >= 10) {
				int64_t a3 = 0, a10 = 0;
				for (size_t i = 0; i < 10; i++) { const int64_t v = closes[s][closes[s].size() - 1 - i]; a10 += v; if (i < 3) a3 += v; }
				x.fast = a3 / 3; x.slow = a10 / 10;
			}
		}
	}
};

struct Group { Style style; int64_t cash = 100'000; int32_t cooldown = 0; std::vector<TermPos> pos; int64_t profit = 0, fees = 0; int opens = 0, closes = 0, max_open = 0; };

static void Close(Group &g, size_t idx, const Sim &sim)
{
	const TermPos p = g.pos[idx];
	g.pos.erase(g.pos.begin() + static_cast<ptrdiff_t>(idx));
	const TermQuote &q = sim.q[p.symbol];
	const int64_t pr = TermProfit(p, q);
	g.cash = std::max<int64_t>(0, g.cash + pr);
	g.profit += pr;
	g.closes++;
	if (g.style == Style::Speculator) g.cooldown = SPEC_COOLDOWN_HOURS;
}

static Group Run(Style style, uint64_t seed, int hours)
{
	Sim sim; Group g; g.style = style;
	for (int h = 0; h < hours; h++) {
		sim.Update(seed, static_cast<uint64_t>(h));
		if (g.cooldown > 0) g.cooldown--;
		/* levels */
		for (size_t i = g.pos.size(); i-- > 0;) if (TermLevelHit(g.pos[i], sim.q[g.pos[i].symbol])) Close(g, i, sim);
		int64_t used = 0, fl = 0;
		for (;;) {   /* stop out, as town_treasury.cpp does */
			used = fl = 0;
			for (const TermPos &p : g.pos) { used += TermMargin(p, sim.q[p.symbol].divisor); fl += TermProfit(p, sim.q[p.symbol]); }
			if (used <= 0 || (g.cash + fl) * 100 >= used * TERM_STOP_OUT_PERCENT) break;
			size_t worst = 0; int64_t wp = INT64_MAX;
			for (size_t i = 0; i < g.pos.size(); i++) { const int64_t v = TermProfit(g.pos[i], sim.q[g.pos[i].symbol]); if (v < wp) { wp = v; worst = i; } }
			Close(g, worst, sim);
		}
		const TermAction a = DecideTerminal(style, g.pos, g.cash + fl - used, g.cooldown, sim.q, Sim::N, Mix(seed, 1, static_cast<uint64_t>(style), static_cast<uint64_t>(h)));
		if (a.kind == TermAction::Kind::Close) {
			CHECK(a.close_index < g.pos.size());
			if (a.close_index < g.pos.size()) Close(g, a.close_index, sim);
		} else if (a.kind == TermAction::Kind::Open) {
			CHECK(a.symbol < Sim::N); CHECK(a.units > 0 && a.units <= TERM_MAX_VOLUME);
			const TermQuote &q = sim.q[a.symbol];
			TermPos p; p.style = style; p.symbol = a.symbol; p.is_long = a.is_long; p.volume = a.units;
			p.entry = a.is_long ? q.ask : q.bid;
			const int64_t margin = TermMargin(p, q.divisor);
			CHECK(margin <= g.cash + fl - used);               // never more margin than is free
			if (a.sl_ppm > 0) p.sl = a.is_long ? p.entry - p.entry * a.sl_ppm / PPM : p.entry + p.entry * a.sl_ppm / PPM;
			if (a.tp_ppm > 0) p.tp = a.is_long ? p.entry + p.entry * a.tp_ppm / PPM : p.entry - p.entry * a.tp_ppm / PPM;
			const int64_t com = (p.entry * p.volume * TERM_COMMISSION_PPM + PPM - 1) / PPM;
			g.cash = std::max<int64_t>(0, g.cash - com); g.fees += com;
			g.pos.push_back(p); g.opens++;
		}
		g.max_open = std::max<int>(g.max_open, static_cast<int>(g.pos.size()));
	}
	return g;
}

int main()
{
	/* The other two groups do nothing here. */
	{
		Sim s; s.Update(1, 0);
		std::vector<TermPos> none;
		CHECK(DecideTerminal(Style::Hoarder, none, 1000, 0, s.q, Sim::N, 5).kind == TermAction::Kind::None);
		CHECK(DecideTerminal(Style::Balanced, none, 1000, 0, s.q, Sim::N, 5).kind == TermAction::Kind::None);
	}
	/* No history, no speculator entry. */
	{
		Sim s; s.Update(1, 0);
		std::vector<TermPos> none;
		int opened = 0;
		for (uint64_t d = 0; d < 2000; d++) if (DecideTerminal(Style::Speculator, none, 100000, 0, s.q, Sim::N, Mix(d)).kind == TermAction::Kind::Open) opened++;
		CHECK(opened == 0);
	}
	/* P/L maths and levels. */
	{
		TermQuote q; q.valid = true; q.bid = 110; q.ask = 111; q.mid = 110; q.divisor = 10;
		TermPos l; l.is_long = true; l.volume = 10; l.entry = 100; l.sl = 95; l.tp = 120;
		CHECK(TermProfit(l, q) == 100); CHECK(!TermLevelHit(l, q));
		q.bid = 120; CHECK(TermLevelHit(l, q));
		q.bid = 94; CHECK(TermLevelHit(l, q));
		TermPos s; s.is_long = false; s.volume = 10; s.entry = 100; s.sl = 105; s.tp = 90;
		q.ask = 89; CHECK(TermLevelHit(s, q)); CHECK(TermProfit(s, q) == 110);
		q.ask = 106; CHECK(TermLevelHit(s, q));
		CHECK(TermMargin(l, 10) == 100);
	}
	/* Many seeds: invariants, determinism, and what the groups do on the whole. */
	int64_t spec_profit = 0, over_profit = 0, over_fees = 0, spec_opens = 0, over_opens = 0;
	for (uint64_t seed = 1; seed <= 200; seed++) {
		const Group a = Run(Style::Speculator, seed, 24 * 120);
		const Group b = Run(Style::Overtrader, seed, 24 * 120);
		CHECK(a.max_open <= static_cast<int>(TERM_SPEC_MAX_POSITIONS));
		CHECK(b.max_open <= static_cast<int>(TERM_OVER_MAX_POSITIONS));
		CHECK(a.cash >= 0 && b.cash >= 0);
		const Group a2 = Run(Style::Speculator, seed, 24 * 120);
		CHECK(a2.profit == a.profit && a2.opens == a.opens);   // same inputs, same decisions
		spec_profit += a.profit; over_profit += b.profit; over_fees += b.fees; spec_opens += a.opens; over_opens += b.opens;
	}
	std::printf("200 towns x 120 days: speculators opened %lld positions, result %+lld; overtraders opened %lld, result %+lld, fees %lld\n",
			static_cast<long long>(spec_opens), static_cast<long long>(spec_profit), static_cast<long long>(over_opens), static_cast<long long>(over_profit), static_cast<long long>(over_fees));
	CHECK(spec_opens > 200);   // they do trade
	CHECK(over_opens > spec_opens); // overtraders trade more
	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
