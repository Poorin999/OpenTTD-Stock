/*
 * StockHoge: the money zones, building permits and taxes of the OpenTTD-Stock mod.
 * This file is new (the rest is AAAHogEx by Rei Ishibashi, GPL v3). It only talks to the script API that apply_overlay.py adds to ScriptCompany
 * (GetZoneCount, GetZoneOfTile, GetHomeZone, GetPermitDaysLeft, GetPermitFee, BuyBuildingPermit, GetZoneBorderFeePermille, GetUnborrowedCash,
 * GetTransportTaxPerYear, GetLandTaxPerTileYear). On a build of the mod without them the AI logs one warning and plays as before.
 */
class StockZones {
	static state = {api = null};
	static MIN_DAYS_LEFT = 45;        // a permit with fewer game days left is renewed before a route in that zone is built
	static MAX_BORDER_PERMILLE = 300; // a route whose delivery fee across a zone border is this share of the income or more is skipped
	static SAMPLE_STEP = 8;           // tiles between the points that are looked at on the line between the two ends of a route

	/** True when the script API of the game has the zone functions (checked once). */
	static function ApiOk() {
		if(StockZones.state.api == null) {
			try {
				AICompany.GetZoneCount();
				StockZones.state.api = true;
			} catch(e) {
				StockZones.state.api = false;
				HgLog.Warning("OpenTTD-Stock: the script API has no zone functions (build the mod with the newest apply_overlay.py). The AI ignores zones, permits and taxes.");
			}
		}
		return StockZones.state.api;
	}

	/** Number of zones, 0 when there are none (yet) or the API is missing. */
	static function Count() {
		if(!StockZones.ApiOk()) return 0;
		return AICompany.GetZoneCount();
	}

	static function ZoneOfTile(tile) {
		if(tile == null || !AIMap.IsValidTile(tile)) return -1;
		return AICompany.GetZoneOfTile(tile);
	}

	/** The zone of the headquarters, -1 when there is none. */
	static function Home() {
		return AICompany.GetHomeZone(AICompany.COMPANY_SELF);
	}

	/** Transport tax of one vehicle of this engine for a year (game year, zone level 100%). 0 without the API. */
	static function TransportTax(engine) {
		if(!StockZones.ApiOk()) return 0;
		local t = AIEngine.GetVehicleType(engine);
		local type = -1;
		if(t == AIVehicle.VT_RAIL) type = 0;
		else if(t == AIVehicle.VT_ROAD) type = 1;
		else if(t == AIVehicle.VT_WATER) type = 2;
		else if(t == AIVehicle.VT_AIR) type = 3;
		if(type < 0) return 0;
		return AICompany.GetTransportTaxPerYear(type);
	}

	/** Land tax of one plain tile for a year. 0 without the API. */
	static function LandTaxPerTile() {
		if(!StockZones.ApiOk()) return 0;
		return AICompany.GetLandTaxPerTileYear();
	}

	/** The tiles to look at for a route from a to b: both ends and a point every SAMPLE_STEP tiles on the straight line between them. */
	static function SamplePoints(a, b) {
		local result = [a];
		local ax = AIMap.GetTileX(a);
		local ay = AIMap.GetTileY(a);
		local dx = AIMap.GetTileX(b) - ax;
		local dy = AIMap.GetTileY(b) - ay;
		local len = max(abs(dx), abs(dy));
		local n = len / StockZones.SAMPLE_STEP;
		for(local i = 1; i < n; i++) {
			result.push(AIMap.GetTileIndex(ax + dx * i / n, ay + dy * i / n));
		}
		result.push(b);
		return result;
	}

	/** Look at one route candidate of the main loop (a table with src, dest and estimate). Returns false when the route must not be built:
	 *  it crosses a zone border with a high exchange fee, it needs permits that cannot be paid, or the extra cost is more than half of the project.
	 *  Permits that are needed (none, or ending soon) are bought here, before anything is built. Adds the extra cost to estimate.buildingCost once. */
	static function PrepareCandidate(t) {
		if(!StockZones.ApiOk() || StockZones.Count() <= 0) return true;
		if(!("src" in t) || !("dest" in t) || t.src == null || t.dest == null) return true;
		local home = StockZones.Home();
		if(home < 0) return true;
		local a = t.src.GetLocation();
		local b = t.dest.GetLocation();
		if(a == null || b == null) return true;
		if(("zoneChecked" in t) && t.zoneChecked) return true;
		t.zoneChecked <- true;

		/* The exchange fee of a delivery across a zone border takes a part of the income of every delivery for as long as the route runs. */
		local zoneA = StockZones.ZoneOfTile(a);
		local zoneB = StockZones.ZoneOfTile(b);
		local permille = AICompany.GetZoneBorderFeePermille(zoneA, zoneB);
		if(permille >= StockZones.MAX_BORDER_PERMILLE) {
			HgLog.Info("zone: skip, border fee "+permille+"/1000 of the income "+zoneA+"->"+zoneB);
			return false;
		}
		if(permille > 0 && ("estimate" in t) && ("value" in t.estimate)) {
			t.estimate.value = t.estimate.value * (1000 - permille) / 1000;
		}

		/* Which zones does the route touch, and how much of it lies outside the home zone. */
		local points = StockZones.SamplePoints(a, b);
		local zones = {};
		local foreignPoints = 0;
		foreach(p in points) {
			local z = StockZones.ZoneOfTile(p);
			if(z < 0) continue;
			zones.rawset(z, true);
			if(z != home) foreignPoints++;
		}
		if(foreignPoints == 0) return true;

		local fees = 0;
		local surcharge = 0;
		local buildingCost = (("estimate" in t) && ("buildingCost" in t.estimate)) ? t.estimate.buildingCost : 0;
		local need = [];
		foreach(z, _ in zones) {
			if(z == home) continue;
			if(AICompany.GetPermitDaysLeft(AICompany.COMPANY_SELF, z) >= StockZones.MIN_DAYS_LEFT) {
				// the permit holds: only the 50% surcharge of this zone (at the tax level of the zone, at least 10%)
			} else {
				local fee = AICompany.GetPermitFee(z);
				if(fee < 0) continue;
				fees += fee;
				need.push(z);
			}
			local pct = max(10, 50 * AICompany.GetZoneTaxPercent(z) / 100);
			local share = 0;
			foreach(p in points) if(StockZones.ZoneOfTile(p) == z) share++;
			surcharge += buildingCost * share / points.len() * pct / 100;
		}
		local extra = fees + surcharge;
		local project = buildingCost + (("estimate" in t && "price" in t.estimate) ? t.estimate.price : 0);
		if(project > 0 && extra > project / 2) {
			HgLog.Info("zone: skip, permits and surcharge "+extra+" are more than half of the project "+project);
			return false;
		}
		if(fees > 0) {
			local own = AICompany.GetUnborrowedCash(AICompany.COMPANY_SELF);
			if(own < fees) {
				HgLog.Info("zone: skip, permits cost "+fees+" but the unborrowed cash is "+own);
				return false;
			}
			local execMode = AIExecMode();
			foreach(z in need) {
				if(!AICompany.BuyBuildingPermit(z)) {
					HgLog.Warning("zone: could not buy the permit of zone "+z+" "+AIError.GetLastErrorString());
					return false;
				}
				HgLog.Info("zone: bought the building permit of zone "+z+" fee:"+AICompany.GetPermitFee(z));
			}
		}
		if(("estimate" in t) && ("buildingCost" in t.estimate)) t.estimate.buildingCost += extra;
		return true;
	}
}
