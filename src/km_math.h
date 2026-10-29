/*
 * Kilometres on the map: how long is a tile, how far apart are two places, how long does a vehicle need.
 *
 * The scale is NOT a free choice. The clock of the mod (game_time/sim_time.h) runs 60 times faster than real time, and the vehicles of
 * OpenTTD still move per game tick. With the movement rules of OpenTTD 15.3 (checked in the sources) a vehicle that shows S km/h covers
 *
 *     S / 2048 tiles per game tick
 *
 * - trains: the loco handler runs twice per tick (`TrainLocoHandler(this, false)` and `(this, true)`), each time S * 3/4 progress units;
 *   one step needs 192 units (`TILE_AXIAL_DISTANCE`), 16 steps are one tile: 2 * S * 3/4 / 192 / 16 = S / 2048.
 * - road vehicles and ships: the internal speed is twice the shown one (`GetDisplayMaxSpeed() = cached_max_speed / 2`), the handler runs once:
 *   (2 * S) * 3/4 / 192 / 16 = S / 2048, the same figure.
 * - aircraft are NOT covered: the setting `plane_speed` divides their speed and their unit differs.
 *
 * One game tick is 27 real ms = 1620 clock ms. A vehicle at S km/h moves S * 1620 / 3600 = 0.45 * S metres per tick, so one tile is
 * 2048 * 0.45 = 921.6 metres. With this length a speed shown in km/h agrees with the game clock: a train that shows 100 km/h needs
 * 33 clock seconds for a tile and covers 2,604 tiles (2,400 km) in one game day. So the vehicles need no change of speed at all;
 * only the length of the tile had to be fixed and shown. (OpenTTD itself has no length of a tile: the wiki figure of about 668 km
 * is what the vanilla clock of 2.2 real seconds per day gives.)
 *
 * All maths is integer (never `double`), pure, no OpenTTD include, so km_math_test.cpp compiles it on its own.
 */

#ifndef STOCK_KM_MATH_H
#define STOCK_KM_MATH_H

#include <cstdint>

namespace kmmath {

/** Length of one game tick of OpenTTD in real milliseconds (`MILLISECONDS_PER_TICK`), and the clock speed-up of the mod. */
inline constexpr int64_t REAL_MS_PER_TICK = 27;
inline constexpr int64_t CLOCK_SCALE = 60;
inline constexpr int64_t CLOCK_MS_PER_TICK = REAL_MS_PER_TICK * CLOCK_SCALE; ///< 1620

/** A vehicle that shows S km/h covers S / TICK_DIVISOR tiles per game tick (see the top of the file). */
inline constexpr int64_t TICK_DIVISOR = 2048;

/** Length of a tile in millimetres: 0.45 m per tick per km/h, times 2048 ticks per tile and km/h. Exactly 921,600. */
inline constexpr int64_t MM_PER_TILE = CLOCK_MS_PER_TICK * 1000 * TICK_DIVISOR / 3600;
static_assert(MM_PER_TILE == 921'600, "a tile must be 921.6 m");
static_assert(CLOCK_MS_PER_TICK * 1000 * TICK_DIVISOR % 3600 == 0, "the tile length must be exact");

inline constexpr int64_t CLOCK_MS_PER_DAY = 86'400'000;

/** Tiles to metres, rounded down. */
constexpr int64_t TilesToMeters(int64_t tiles) { return tiles * MM_PER_TILE / 1000; }

/** Tiles to kilometres in tenths (123 = 12.3 km), rounded down. */
constexpr int64_t TilesToKmTenths(int64_t tiles) { return tiles * MM_PER_TILE / 100'000; }

/** Whole kilometres in a distance given in tenths. */
constexpr int64_t KmWhole(int64_t km_tenths) { return km_tenths / 10; }
constexpr int64_t KmFraction(int64_t km_tenths) { return km_tenths % 10; }

/** Integer square root, rounded down (for the straight-line distance from `DistanceSquare`). */
constexpr uint64_t IntSqrt(uint64_t n)
{
	uint64_t lo = 0, hi = 4'294'967'296ULL; // sqrt of a 64 bit number is below 2^32
	while (lo + 1 < hi) {
		const uint64_t mid = lo + (hi - lo) / 2;
		if (mid * mid <= n) lo = mid; else hi = mid;
	}
	return lo;
}

/** Straight-line distance in tiles, from the squared distance of OpenTTD (`DistanceSquare`), rounded down. */
constexpr int64_t TilesFromSquare(uint64_t square) { return static_cast<int64_t>(IntSqrt(square)); }

/** Game ticks that a vehicle with the shown speed (km/h) needs for a distance in tiles. 0 when the speed is 0 (it never arrives). */
constexpr int64_t TravelTicks(int64_t tiles, int64_t speed_kmh)
{
	if (speed_kmh <= 0 || tiles <= 0) return 0;
	return (tiles * TICK_DIVISOR + speed_kmh - 1) / speed_kmh; // rounded up: the vehicle is there at the end of the tick
}

/** The same in clock minutes (what the HH:MM clock of the game shows). */
constexpr int64_t TravelClockMinutes(int64_t tiles, int64_t speed_kmh)
{
	return TravelTicks(tiles, speed_kmh) * CLOCK_MS_PER_TICK / 60'000;
}

/** The same in real seconds at normal game speed (the clock is driven by ticks, so this is the wall-clock time without fast forward). */
constexpr int64_t TravelRealSeconds(int64_t tiles, int64_t speed_kmh)
{
	return TravelTicks(tiles, speed_kmh) * REAL_MS_PER_TICK / 1000;
}

/** Tiles that a vehicle covers in one game day (24 clock hours). */
constexpr int64_t TilesPerGameDay(int64_t speed_kmh)
{
	return speed_kmh <= 0 ? 0 : speed_kmh * (CLOCK_MS_PER_DAY / CLOCK_MS_PER_TICK) / TICK_DIVISOR;
}

/** Kilometres that a vehicle covers in one game day (24 h at that speed: exact, independent of the tile length). */
constexpr int64_t KmPerGameDay(int64_t speed_kmh) { return speed_kmh <= 0 ? 0 : speed_kmh * 24; }

/** The speed (km/h) that a vehicle needs to cover a distance in tiles in the given clock minutes, rounded up. 0 when there is no time. */
constexpr int64_t RequiredSpeedKmh(int64_t tiles, int64_t clock_minutes)
{
	if (clock_minutes <= 0 || tiles <= 0) return 0;
	const int64_t meters = TilesToMeters(tiles);
	return (meters * 60 + clock_minutes * 1000 - 1) / (clock_minutes * 1000);
}

/** A duration in clock minutes split for display. */
struct Duration {
	int64_t days;
	int hours;
	int minutes;
};

constexpr Duration SplitMinutes(int64_t clock_minutes)
{
	if (clock_minutes < 0) clock_minutes = 0;
	return Duration{clock_minutes / (24 * 60), static_cast<int>(clock_minutes % (24 * 60) / 60), static_cast<int>(clock_minutes % 60)};
}

} // namespace kmmath

#endif /* STOCK_KM_MATH_H */
