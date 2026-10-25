/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file cargo_market_test.cpp Stand-alone test of CargoMarketMath.h (no OpenTTD needed).
 * Build and run: g++ -std=c++17 -Wall -o cargo_market_test cargo_market_test.cpp && ./cargo_market_test
 */

#include "CargoMarketMath.h"
#include <cstdio>

using namespace cargomarket;

static int _fail = 0, _checks = 0;
#define CHECK(c) do { _checks++; if (!(c)) { _fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/** Run `days` days with `per_day` units delivered each day. */
static void Run(CargoState &s, int days, int64_t per_day, int64_t elasticity = 500'000, int64_t outside = PPM)
{
	for (int i = 0; i < days; i++) { s.delivered_today = per_day; UpdateDay(s, elasticity, outside); }
}

int main()
{
	/* 1. A steady trade pays the normal price. */
	{
		CargoState s; Run(s, 400, 200);
		CHECK(s.price_ppm >= 990'000 && s.price_ppm <= 1'010'000);
	}
	/* 2. During the baseline the price follows the outside factor only, even for a wild start. */
	{
		CargoState s; Run(s, BASELINE_DAYS, 5000);
		CHECK(s.price_ppm == PPM);
	}
	/* 3. A glut (double the normal) lowers the price; the elasticity decides how much. */
	{
		CargoState a, b; Run(a, 200, 200, 800'000); Run(b, 200, 200, 300'000);
		Run(a, 40, 400, 800'000); Run(b, 40, 400, 300'000);
		CHECK(a.price_ppm < 900'000);
		CHECK(b.price_ppm > a.price_ppm);
		CHECK(b.price_ppm < PPM);
	}
	/* 4. A shortage (nobody delivers) raises the price, at most to the limit. */
	{
		CargoState s; Run(s, 200, 200); Run(s, 60, 0, 800'000);
		CHECK(s.price_ppm > 1'100'000);
		CHECK(s.price_ppm <= SUPPLY_MAX_PPM);
	}
	/* 5. The price comes back after a glut ends. */
	{
		CargoState s; Run(s, 200, 200); Run(s, 40, 600, 800'000);
		const int64_t low = s.price_ppm; Run(s, 200, 200, 800'000);
		CHECK(low < 900'000); CHECK(s.price_ppm > low); CHECK(s.price_ppm > 900'000);
	}
	/* 6. A tiny trade does not swing: 3 units a day against 0 normal stays near 1.0 (the floor of the normal level is 50 units a day). */
	{
		CargoState s; Run(s, 100, 0); Run(s, 30, 3);
		CHECK(s.price_ppm >= 950'000 && s.price_ppm <= 1'050'000);
	}
	/* 7. Limits: absurd amounts and an extreme outside factor stay inside MIN .. MAX and do not overflow. */
	{
		CargoState s; Run(s, 100, 100); Run(s, 100, INT64_MAX / 2, 1'000'000, 2 * PPM);
		CHECK(s.price_ppm >= PRICE_MIN_PPM && s.price_ppm <= PRICE_MAX_PPM);
		CargoState t; Run(t, 50, 100, 1'000'000, PPM / 2);
		CHECK(t.price_ppm >= PRICE_MIN_PPM);
		CHECK(t.price_ppm <= PPM);
	}
	/* 8. The outside factor: mood and money move the price a little, both limited. */
	{
		CHECK(OutsideFactor(0, PPM) == PPM);
		CHECK(OutsideFactor(150'000, PPM) == PPM + 50'000);
		CHECK(OutsideFactor(-150'000, PPM) == PPM - 50'000);
		CHECK(OutsideFactor(0, 2 * PPM) == PPM + MONEY_MAX_PPM);          // +100% money = +25%, limited to +10%
		CHECK(OutsideFactor(0, PPM / 2) == PPM - MONEY_MAX_PPM);          // -50% money = -12.5%, limited to -10%
		CHECK(OutsideFactor(10'000'000, 100 * PPM) <= PPM + 100'000 + 100'000);
		CargoState s; Run(s, 300, 200, 500'000, OutsideFactor(150'000, PPM));
		CHECK(s.price_ppm > 1'040'000 && s.price_ppm <= 1'051'000);
	}
	/* 9. ApplyPrice: exact for small incomes, no overflow for big ones, same sign. */
	{
		CHECK(ApplyPrice(1000, PPM) == 1000);
		CHECK(ApplyPrice(1000, 1'500'000) == 1500);
		CHECK(ApplyPrice(1000, 500'000) == 500);
		CHECK(ApplyPrice(-1000, 500'000) == -500);
		CHECK(ApplyPrice(0, 1'234'567) == 0);
		CHECK(ApplyPrice(4'000'000'000'000'000LL, 2'000'000) == 8'000'000'000'000'000LL);
	}
	/* 10. Same input, same result (every client of a network game must agree). */
	{
		CargoState a, b; Run(a, 365, 123); Run(b, 365, 123); Run(a, 50, 700, 800'000); Run(b, 50, 700, 800'000);
		CHECK(a.price_ppm == b.price_ppm && a.ref_x100 == b.ref_x100 && a.fast_x100 == b.fast_x100 && a.days == b.days);
	}
	/* 11. A saved state loads back and goes on in the same way. */
	{
		CargoState a; Run(a, 100, 150);
		CargoState b; b.price_ppm = a.price_ppm; b.ref_x100 = a.ref_x100; b.fast_x100 = a.fast_x100; b.days = a.days;
		Run(a, 30, 400, 800'000); Run(b, 30, 400, 800'000);
		CHECK(a.price_ppm == b.price_ppm);
	}

	/* 12. Local demand: the town of the delivery. */
	{
		CHECK(LocalFactor(LOCAL_WEIGHT_PEOPLE_PPM, 0, PROSPERITY_NEUTRAL_PPM) == PPM);              // average town, no boost = plain price
		CHECK(LocalFactor(0, 150'000, PPM) == PPM);                                                  // weight 0 (gold): never changes
		CHECK(LocalFactor(LOCAL_WEIGHT_PEOPLE_PPM, 0, PPM) == PPM + 100'000);                        // richest town +10%
		CHECK(LocalFactor(LOCAL_WEIGHT_PEOPLE_PPM, 0, 0) == PPM - 100'000);                          // tiny town -10%
		CHECK(LocalFactor(LOCAL_WEIGHT_PEOPLE_PPM, 100'000, PROSPERITY_NEUTRAL_PPM) == PPM + 100'000); // boost +10% counts fully for people
		CHECK(LocalFactor(LOCAL_WEIGHT_OTHER_PPM, 100'000, PROSPERITY_NEUTRAL_PPM) == PPM + 25'000);   // raw material: a quarter
		CHECK(LocalFactor(LOCAL_WEIGHT_PEOPLE_PPM, 150'000, PPM) == LOCAL_MAX_PPM);                  // 15% + 10% = 25%, limited to +20%
		CHECK(LocalFactor(LOCAL_WEIGHT_PEOPLE_PPM, 0, 0) >= LOCAL_MIN_PPM);
		CHECK(LocalFactor(LOCAL_WEIGHT_PEOPLE_PPM, INT64_MAX / 4, 5 * PPM) == LOCAL_MAX_PPM);         // absurd input stays limited, no overflow
		CHECK(LocalFactor(10 * PPM, -5, -PPM) >= LOCAL_MIN_PPM);                                     // weight and negative boost are clamped
		/* Monotonic in the boost and in the prosperity; consumer cargo between people and raw material. */
		int64_t last = 0; bool mono = true;
		for (int64_t b = 0; b <= 200'000; b += 10'000) { const int64_t f = LocalFactor(LOCAL_WEIGHT_PEOPLE_PPM, b, 700'000); if (f < last) mono = false; last = f; }
		CHECK(mono);
		last = 0; mono = true;
		for (int64_t pr = 0; pr <= PPM; pr += 50'000) { const int64_t f = LocalFactor(LOCAL_WEIGHT_CONSUMER_PPM, 50'000, pr); if (f < last) mono = false; last = f; }
		CHECK(mono);
		CHECK(LocalFactor(LOCAL_WEIGHT_OTHER_PPM, 100'000, PPM) < LocalFactor(LOCAL_WEIGHT_CONSUMER_PPM, 100'000, PPM));
		CHECK(LocalFactor(LOCAL_WEIGHT_CONSUMER_PPM, 100'000, PPM) < LocalFactor(LOCAL_WEIGHT_PEOPLE_PPM, 100'000, PPM));
		/* Global price x local factor stays inside a known band: 0.5 x 0.9 .. 2.0 x 1.2 of the plain pay. */
		CHECK(ApplyPrice(ApplyPrice(1000, PRICE_MIN_PPM), LOCAL_MIN_PPM) == 450);
		CHECK(ApplyPrice(ApplyPrice(1000, PRICE_MAX_PPM), LOCAL_MAX_PPM) == 2400);
	}

	/* 13. SupplyState (display only). */
	{
		CHECK(SupplyState(100'000, 10'000, 5) == 0);              // still measuring
		CHECK(SupplyState(20'000, 10'000, 100) == 1);             // double the normal = glut
		CHECK(SupplyState(2'000, 10'000, 100) == -1);             // a fifth of the normal = shortage
		CHECK(SupplyState(10'500, 10'000, 100) == 0);             // +5% is inside the band
		CHECK(SupplyState(300, 100, 100) == 0);                   // tiny trade: the floor of the normal level keeps it quiet
		CHECK(SupplyState(INT64_MAX / 4, 10'000, 100) == 1);      // no overflow (levels are capped)
		CHECK(SupplyState(INT64_MAX / 4, INT64_MAX / 4, 100) == 0);
		CHECK(ExcessPpm(INT64_MAX, 0) == PPM);
		CHECK(ExcessPpm(-5, INT64_MAX) == -PPM);
		CHECK(ExcessPpm(LEVEL_CAP_X100, LEVEL_CAP_X100) == 0);
		CHECK(SupplyFactor(INT64_MAX, 0, PPM) == SUPPLY_MIN_PPM);  // the old code overflowed here
	}

	std::printf("%d checks, %d failed\n", _checks, _fail);
	return _fail == 0 ? 0 : 1;
}
