/* Standalone test of km_math.h: g++ -std=c++17 -o km_math_test km_math_test.cpp && ./km_math_test */
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "km_math.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

int main()
{
	using namespace kmmath;
	CHECK(MM_PER_TILE == 921600);
	CHECK(TilesToMeters(1) == 921);
	CHECK(TilesToMeters(1000) == 921600);
	CHECK(TilesToKmTenths(100) == 921);   // 92.1 km
	CHECK(TilesToKmTenths(0) == 0);
	CHECK(KmWhole(921) == 92 && KmFraction(921) == 1);

	CHECK(IntSqrt(0) == 0 && IntSqrt(1) == 1 && IntSqrt(15) == 3 && IntSqrt(16) == 4 && IntSqrt(17) == 4);
	CHECK(IntSqrt(18446744073709551615ULL) == 4294967295ULL);
	CHECK(TilesFromSquare(3 * 3 + 4 * 4) == 5);

	/* 100 km/h: a tile in 2048/100 = 20.48 ticks -> 21 ticks (rounded up). */
	CHECK(TravelTicks(1, 100) == 21);
	CHECK(TravelTicks(100, 100) == 2048);
	CHECK(TravelTicks(100, 0) == 0 && TravelTicks(0, 100) == 0 && TravelTicks(-5, 100) == 0);
	/* 1000 tiles at 100 km/h: 20480 ticks = 33,177,600 clock ms = 552.96 min -> 552. 921.6 km at 100 km/h = 9h12.96m. */
	CHECK(TravelClockMinutes(1000, 100) == 552);
	/* The same trip, real seconds at normal speed: 20480 ticks * 27 ms = 552.96 s. */
	CHECK(TravelRealSeconds(1000, 100) == 552);
	/* Consistency: the clock minutes equal the real seconds (1 clock minute = 1 real second). */
	for (int64_t t : {1, 7, 100, 1000, 12345}) for (int64_t s : {10, 48, 100, 160, 250}) {
		const int64_t m = TravelClockMinutes(t, s), r = TravelRealSeconds(t, s);
		CHECK(m == r || m == r + 1 || m + 1 == r);
	}
	/* Physics check against km: km = tiles * 0.9216; hours = km / speed. */
	for (int64_t s : {48, 100, 160}) {
		const int64_t tiles = 5000;
		const int64_t km_tenths = TilesToKmTenths(tiles);                 // 4608.0 km
		const int64_t minutes_by_km = km_tenths * 60 / (10 * s);          // minutes from the distance
		const int64_t minutes = TravelClockMinutes(tiles, s);
		CHECK(minutes >= minutes_by_km - 1 && minutes <= minutes_by_km + 1);
	}

	CHECK(TilesPerGameDay(100) == 2604);   // 100 * 53333 / 2048
	CHECK(KmPerGameDay(100) == 2400);
	CHECK(TilesPerGameDay(0) == 0 && KmPerGameDay(-1) == 0);

	/* 1000 tiles (921.6 km) in 10 hours needs 93 km/h (rounded up from 92.16 -> uses 921 m -> 92.1 -> 93). */
	CHECK(RequiredSpeedKmh(1000, 600) == 93);
	CHECK(RequiredSpeedKmh(1000, 0) == 0 && RequiredSpeedKmh(0, 600) == 0);
	/* A vehicle at the required speed arrives within the time (round trip of the two functions). */
	for (int64_t t : {10, 250, 1000, 4000}) for (int64_t min : {30, 600, 3000}) {
		const int64_t need = RequiredSpeedKmh(t, min);
		CHECK(need == 0 || TravelClockMinutes(t, need) <= min + 1);
	}

	const Duration d = SplitMinutes(2 * 24 * 60 + 5 * 60 + 7);
	CHECK(d.days == 2 && d.hours == 5 && d.minutes == 7);
	const Duration z = SplitMinutes(-3);
	CHECK(z.days == 0 && z.hours == 0 && z.minutes == 0);

	if (g_fail == 0) std::printf("km_math_test: all checks passed\n");
	return g_fail == 0 ? 0 : 1;
}
