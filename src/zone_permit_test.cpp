/* Stand-alone test of zone_permit_math.h (not part of the game build).
 * Build and run:  g++ -std=c++17 -Wall -Wextra -o /tmp/zpt zone_permit_test.cpp && /tmp/zpt */
#include <cstdio>
#include "zone_permit_math.h"
using namespace permitmath;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main()
{
	/* Fee follows the tax level of the zone, tax is kept inside 0 .. 300. */
	CHECK(Fee(100) == 50000 && Fee(120) == 60000 && Fee(80) == 40000 && Fee(0) == 0);
	CHECK(Fee(1000) == Fee(300) && Fee(-5) == 0);
	/* Validity: ends the day it expires. */
	CHECK(!IsValid(0, 0) && !IsValid(100, 100) && IsValid(101, 100) && !IsValid(50, 100));
	CHECK(DaysLeft(150, 100) == 50 && DaysLeft(100, 100) == 0 && DaysLeft(0, 100) == 0);
	/* Renew: from today when none or ended, adds to what is left, capped from today. */
	CHECK(Renew(0, 1000) == 1180 && Renew(900, 1000) == 1180);
	CHECK(Renew(1100, 1000) == 1280);
	CHECK(Renew(1300, 1000) == 1360 && Renew(1360, 1000) == 1360);
	/* Surcharge: with a permit 50% at level 100, scaled by tax level, at least 10%; without a permit always 400%. */
	CHECK(SurchargePercent(true, 100) == 50 && SurchargePercent(true, 140) == 70 && SurchargePercent(true, 80) == 40);
	CHECK(SurchargePercent(true, 0) == 10 && SurchargePercent(true, 10) == 10);
	CHECK(SurchargePercent(false, 0) == 400 && SurchargePercent(false, 300) == 400);
	CHECK(Surcharge(1000, true, 100) == 500 && Surcharge(1000, false, 100) == 4000);
	CHECK(Surcharge(0, false, 100) == 0 && Surcharge(-500, false, 100) == 0);
	CHECK(Surcharge(1000000000000LL, false, 300) == 4000000000000LL); // no overflow at 1e12
	/* A permit is always cheaper than none (the point of buying it) even for the dearest zone, for a building of any size >= the fee. */
	for (int tax = 0; tax <= 300; tax += 20) CHECK(SurchargePercent(true, tax) < SurchargePercent(false, tax));
	/* Which commands are charged. */
	CHECK(IsConstructionName("CmdBuildRailroadTrack") && IsConstructionName("CmdBuildRoad") && IsConstructionName("CmdBuildObject"));
	CHECK(IsConstructionName("CmdBuildRailStation") && IsConstructionName("CmdBuildBridge") && IsConstructionName("CmdBuildTunnel"));
	CHECK(IsConstructionName("CmdLandscapeClear") && IsConstructionName("CmdTerraformLand") && IsConstructionName("CmdPlantTree"));
	CHECK(!IsConstructionName("CmdBuildVehicle") && !IsConstructionName("CmdBuildShip") && !IsConstructionName("CmdBuildAircraft"));
	CHECK(!IsConstructionName("CmdBuildRailVehicle") && !IsConstructionName("CmdBuildRoadVehicle"));
	CHECK(!IsConstructionName("CmdZonePermit") && !IsConstructionName("CmdBankBorrow") && !IsConstructionName("CmdSellVehicle") && !IsConstructionName(""));
	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail != 0;
}
