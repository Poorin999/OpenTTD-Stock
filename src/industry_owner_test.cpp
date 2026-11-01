/* Stand-alone test of industry_owner_math.h (not part of the game build).
 * Build and run:  g++ -std=c++17 -Wall -Wextra -o /tmp/iot industry_owner_test.cpp && /tmp/iot */
#include <cstdio>
#include "industry_owner_math.h"
using namespace indowner;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static Identity Id(uint8_t type, uint32_t tile, int32_t built) { Identity i; i.type = type; i.tile = tile; i.built = built; return i; }

int main()
{
	/* ---- Price: output per month x market price x 12, never below the floor, no overflow. ---- */
	CHECK(OwnershipPrice(10'000) == 120'000);          // 10,000 a month -> 120,000 (pays back in a year)
	CHECK(OwnershipPrice(0) == PRICE_FLOOR);           // nothing produced yet
	CHECK(OwnershipPrice(-5) == PRICE_FLOOR);
	CHECK(OwnershipPrice(1'000) == PRICE_FLOOR);       // 12,000 is below the floor
	CHECK(OwnershipPrice(1'667) == 20'004);            // just above the floor
	CHECK(OwnershipPrice(INT64_MAX / 2) == INT64_MAX); // saturates
	CHECK(SaturatingMul(3, 4) == 12 && SaturatingMul(0, 9) == 0 && SaturatingMul(-3, 9) == 0 && SaturatingMul(INT64_MAX, 2) == INT64_MAX);
	/* The price never falls when the output grows. */
	{
		int64_t last = 0;
		bool mono = true;
		for (int64_t v = 0; v < 2'000'000; v += 997) { const int64_t p = OwnershipPrice(v); if (p < last) mono = false; last = p; }
		CHECK(mono);
	}

	const Identity coal = Id(2, 5000, 100);
	const uint32_t IND = 7;

	/* ---- Every industry is the system's at the start. ---- */
	Table t;
	CHECK(t.OwnerOf(IND, coal) == SYSTEM && t.Level(IND, coal) == 0 && t.rows.empty());

	/* ---- Buying: checks. ---- */
	CHECK(t.CanBuy(IND, coal, 3, 1'000'000, 120'000, true) == BuyResult::Ok);
	CHECK(t.CanBuy(IND, coal, 3, 119'999, 120'000, true) == BuyResult::NoCash);   // one short
	CHECK(t.CanBuy(IND, coal, 3, 120'000, 120'000, true) == BuyResult::Ok);       // exactly enough
	CHECK(t.CanBuy(IND, coal, 3, 1'000'000, 120'000, false) == BuyResult::NotForSale); // produces nothing
	CHECK(t.CanBuy(IND, coal, 3, 1'000'000, 0, true) == BuyResult::NotForSale);
	CHECK(t.rows.empty() && t.system_income == 0); // CanBuy changes nothing

	/* A refused purchase changes nothing. */
	CHECK(t.Buy(IND, coal, 3, 1950, 100, 120'000, true, 40) == BuyResult::NoCash);
	CHECK(t.rows.empty() && t.system_income == 0);

	/* ---- Buying: transfer. ---- */
	CHECK(t.Buy(IND, coal, 3, 1950, 500'000, 120'000, true, 40) == BuyResult::Ok);
	CHECK(t.OwnerOf(IND, coal) == 3 && t.Level(IND, coal) == 0);
	CHECK(t.system_income == 120'000);
	{
		const Row *r = t.Find(IND, coal);
		CHECK(r != nullptr && r->acquired_day == 40 && r->price_paid == 120'000 && r->owner_year == 1950 && r->level == 0);
	}
	CHECK(t.CanBuy(IND, coal, 3, 9'999'999, 120'000, true) == BuyResult::AlreadyYours);
	CHECK(t.CanBuy(IND, coal, 4, 9'999'999, 120'000, true) == BuyResult::OwnedByOther);
	CHECK(t.Buy(IND, coal, 4, 1950, 9'999'999, 120'000, true, 41) == BuyResult::OwnedByOther);
	CHECK(t.OwnerOf(IND, coal) == 3 && t.system_income == 120'000); // the second buyer changed nothing
	/* Another industry is still the system's. */
	CHECK(t.OwnerOf(8, coal) == SYSTEM);

	/* ---- Stale row: the industry was closed and a new one got the same index. ---- */
	const Identity other = Id(5, 9000, 480);
	CHECK(t.OwnerOf(IND, other) == SYSTEM);                       // the new industry is the system's
	CHECK(t.CanBuy(IND, other, 4, 1'000'000, 50'000, true) == BuyResult::Ok);
	CHECK(t.Buy(IND, other, 4, 1951, 1'000'000, 50'000, true, 500) == BuyResult::Ok); // replaces the stale row
	CHECK(t.OwnerOf(IND, other) == 4 && t.rows.size() == 1 && t.system_income == 170'000);
	CHECK(t.OwnerOf(IND, coal) == SYSTEM);                        // the old identity is gone

	/* Same type and tile but another construction date is another industry too. */
	CHECK(t.OwnerOf(IND, Id(5, 9000, 481)) == SYSTEM && t.OwnerOf(IND, Id(5, 9001, 480)) == SYSTEM && t.OwnerOf(IND, Id(6, 9000, 480)) == SYSTEM);

	/* ---- Sweep: industry closed, company gone. ---- */
	{
		Table s;
		CHECK(s.Buy(1, Id(1, 10, 1), 0, 1950, 1e9, 30'000, true, 1) == BuyResult::Ok);
		CHECK(s.Buy(2, Id(1, 20, 1), 1, 1950, 1e9, 30'000, true, 1) == BuyResult::Ok);
		CHECK(s.Buy(3, Id(1, 30, 1), 1, 1950, 1e9, 30'000, true, 1) == BuyResult::Ok);
		CHECK(s.Buy(4, Id(1, 40, 1), 2, 1950, 1e9, 30'000, true, 1) == BuyResult::Ok);
		/* Industry 1 was closed, company 1 went bankrupt, company 0 and 2 are fine. */
		const SweepResult r = s.Sweep([](uint32_t ind, const Row &row) {
			if (ind == 1) return Verdict::IndustryGone;
			if (row.owner == 1) return Verdict::OwnerGone;
			return Verdict::Keep;
		});
		CHECK(r.industry_gone == 1 && r.owner_gone == 2 && r.Total() == 3);
		CHECK(s.rows.size() == 1 && s.OwnerOf(4, Id(1, 40, 1)) == 2);
		CHECK(s.OwnerOf(2, Id(1, 20, 1)) == SYSTEM && s.OwnerOf(3, Id(1, 30, 1)) == SYSTEM && s.OwnerOf(1, Id(1, 10, 1)) == SYSTEM);
		CHECK(s.system_income == 120'000); // what was paid stays paid to the system
		/* A sweep that keeps everything removes nothing. */
		CHECK(s.Sweep([](uint32_t, const Row &) { return Verdict::Keep; }).Total() == 0 && s.rows.size() == 1);
	}

	/* ---- ReleaseOwner: bankruptcy. ---- */
	{
		Table s;
		s.Buy(1, Id(1, 10, 1), 5, 1950, 1e9, 30'000, true, 1);
		s.Buy(2, Id(1, 20, 1), 5, 1950, 1e9, 30'000, true, 1);
		s.Buy(3, Id(1, 30, 1), 6, 1950, 1e9, 30'000, true, 1);
		CHECK(s.ReleaseOwner(5) == 2 && s.rows.size() == 1 && s.OwnerOf(3, Id(1, 30, 1)) == 6);
		CHECK(s.ReleaseOwner(5) == 0 && s.ReleaseOwner(9) == 0);
	}

	/* ---- A new company that takes the slot of a bankrupt one (same number, other founding year) is told apart by the game through owner_year: the table keeps the year. ---- */
	{
		Table s;
		s.Buy(1, Id(1, 10, 1), 5, 1950, 1e9, 30'000, true, 1);
		const Row *r = s.Find(1, Id(1, 10, 1));
		CHECK(r != nullptr && r->owner_year == 1950 && r->owner_year != 1953);
	}

	/* ---- Save and load: every value comes back. ---- */
	{
		Table s;
		s.Buy(7, Id(2, 5000, 100), 3, 1950, 1e9, 120'000, true, 40);
		s.Buy(900, Id(255, 0x3FFFFF, 700000), 14, 1999, 1000000000000LL, 99'999'999'999LL, true, 123456);
		s.Buy(65534, Id(0, 1, 0), 0, 1950, 1e9, 20'000, true, 0);
		s.rows[7].level = 3; // as if upgraded
		const std::vector<Record> recs = s.ToRecords();
		CHECK(recs.size() == 4 && recs[0].ym == SUMMARY_KEY);
		Table l;
		for (const Record &r : recs) CHECK(l.FromRecord(r));
		CHECK(l.rows.size() == 3 && l.system_income == s.system_income);
		for (const auto &kv : s.rows) {
			const auto it = l.rows.find(kv.first);
			CHECK(it != l.rows.end());
			if (it == l.rows.end()) continue;
			CHECK(it->second.owner == kv.second.owner && it->second.level == kv.second.level && it->second.acquired_day == kv.second.acquired_day);
			CHECK(it->second.price_paid == kv.second.price_paid && it->second.owner_year == kv.second.owner_year && it->second.id == kv.second.id);
		}
		CHECK(l.OwnerOf(7, Id(2, 5000, 100)) == 3 && l.Level(7, Id(2, 5000, 100)) == 3);
		CHECK(l.OwnerOf(900, Id(255, 0x3FFFFF, 700000)) == 14);
		/* Records come out in the order of the industry index. */
		CHECK(recs[1].ym == 7 && recs[2].ym == 900 && recs[3].ym == 65534);
		/* Save again from the loaded table: the same records. */
		const std::vector<Record> again = l.ToRecords();
		CHECK(again.size() == recs.size());
		for (size_t i = 0; i < again.size() && i < recs.size(); i++) {
			CHECK(again[i].ym == recs[i].ym && again[i].a == recs[i].a && again[i].b == recs[i].b && again[i].v1 == recs[i].v1 && again[i].v2 == recs[i].v2 && again[i].v3 == recs[i].v3 && again[i].v4 == recs[i].v4 && again[i].v5 == recs[i].v5);
		}
	}

	/* ---- An old savegame has no record of this kind: nothing is read, every industry is the system's. ---- */
	{
		Table l;
		l.system_income = 5; // as if something was there
		l.Reset();           // what StockMarket::Reset does before a load
		CHECK(l.rows.empty() && l.system_income == 0);
		CHECK(l.OwnerOf(7, Id(2, 5000, 100)) == SYSTEM && l.OwnerOf(0, Id(0, 0, 0)) == SYSTEM);
	}

	/* ---- Records that make no sense are ignored. ---- */
	{
		Table l;
		Record bad;
		bad.ym = 5; bad.a = 15; // company 15 does not exist (0 .. 14)
		CHECK(!l.FromRecord(bad));
		bad.a = 200;
		CHECK(!l.FromRecord(bad));
		bad.a = SYSTEM;
		CHECK(!l.FromRecord(bad));
		bad.a = 2; bad.ym = -7;
		CHECK(!l.FromRecord(bad));
		bad.ym = 70000;
		CHECK(!l.FromRecord(bad));
		CHECK(l.rows.empty());
		/* A level that is too high is cut, negative money is cut. */
		Record ok;
		ok.ym = 5; ok.a = 2; ok.b = 200; ok.v1 = -4; ok.v2 = -9;
		CHECK(l.FromRecord(ok));
		CHECK(l.rows[5].level == MAX_LEVEL && l.rows[5].acquired_day == 0 && l.rows[5].price_paid == 0);
		Record sum;
		sum.ym = SUMMARY_KEY; sum.v1 = -50;
		CHECK(l.FromRecord(sum) && l.system_income == 0);
	}

	/* ---- A company number is never SYSTEM, and the last company (14) works. ---- */
	{
		Table s;
		CHECK(s.Buy(1, Id(1, 1, 1), 14, 1950, 1e9, 30'000, true, 1) == BuyResult::Ok && s.OwnerOf(1, Id(1, 1, 1)) == 14);
		CHECK(s.CanBuy(2, Id(1, 2, 1), 0, 1e9, 30'000, true) == BuyResult::Ok);
	}

	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
