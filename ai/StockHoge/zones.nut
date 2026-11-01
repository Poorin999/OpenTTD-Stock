/*
 * StockHoge: the money zones, building permits and taxes of the OpenTTD-Stock mod.
 * This file is new (the rest is AAAHogEx by Rei Ishibashi, GPL v3). It only talks to the script API that apply_overlay.py adds to ScriptCompany
 * (GetZoneCount, GetZoneOfTile, GetHomeZone, GetPermitDaysLeft, GetPermitFee, BuyBuildingPermit, GetZoneBorderFeePermille, GetUnborrowedCash,
 * GetTransportTaxPerYear, GetLandTaxPerTileYear). On a build of the mod without them the AI logs one warning and plays as before.
 */
class StockZones {
	static state = {api = null, lastMaintain = 0};
	static MIN_DAYS_LEFT = 45;        // a permit with fewer game days left is renewed before a route in that zone is built
	static MAX_BORDER_PERMILLE = 300; // a route whose delivery fee across a zone border is this share of the income or more is skipped
	static SAMPLE_STEP = 8;           // tiles between the points that are looked at on the line between the two ends of a route
	static MAINTAIN_EVERY_DAYS = 15;  // MaintainPermits looks at the stations of the company this often (game days)
	static MAINTAIN_RENEW_BELOW = 30; // a permit with fewer game days left than this is renewed when the company has a station in that zone
	static MAINTAIN_CASH_SHARE = 20;  // ... and only when the fee is at most 1/20 of the unborrowed cash

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

	static hq = {tries = 0, gaveUp = false};
	static HQ_MAX_TRIES = 30;       // game days (one try per call of the main loop, then a short sleep); after that the AI plays on without an HQ
	static HQ_SEARCH_RADIUS = 10;   // tiles around the centre of a town where a free 2x2 place is looked for
	static HQ_MAX_TOWNS = 5;        // the biggest towns that are tried in one call

	/** The zone with the best score as a home: the people living in it divided by its tax level (the tax of the company goes to the bank of the home zone, and the building in the home zone costs no surcharge). -1 when there is no zone. */
	static function BestHomeZone(towns) {
		local zones = StockZones.Count();
		local best = -1;
		local bestScore = -1;
		for(local zone = 0; zone < zones; zone++) {
			local pop = 0;
			foreach(town, _ in towns) {
				if(StockZones.ZoneOfTile(AITown.GetLocation(town)) == zone) pop += AITown.GetPopulation(town);
			}
			local tax = AICompany.GetZoneTaxPercent(zone);
			if(tax <= 0) tax = 100;
			local score = pop * 100 / tax;
			if(score > bestScore) { bestScore = score; best = zone; }
		}
		return best;
	}

	/** OpenTTD-Stock: the headquarters is compulsory. A company must place its HQ before it builds anything (the human player is held by the
	 *  HQ window, the AI by this function: it is called at the start of every turn of the main loop). The HQ goes next to the biggest towns.
	 *  Returns true when the AI may build (it has an HQ, the mod has no zones, or HQ_MAX_TRIES tries failed), false when it must wait. */
	static function EnsureHq() {
		if(!StockZones.ApiOk() || StockZones.Count() <= 0) return true;
		if(StockZones.hq.gaveUp) return true;
		if(AIMap.IsValidTile(AICompany.GetCompanyHQ(AICompany.COMPANY_SELF))) return true;
		StockZones.hq.tries++;
		if(StockZones.hq.tries > StockZones.HQ_MAX_TRIES) {
			StockZones.hq.gaveUp = true;
			HgLog.Warning("hq: no place for the headquarters found, the AI plays on without it");
			return true;
		}
		local towns = AITownList();
		// The towns of the best home zone come first (people / tax level of the zone), the biggest town first inside a zone.
		local best = StockZones.BestHomeZone(towns);
		towns.Valuate(function(town, best) {
			return AITown.GetPopulation(town) + (best >= 0 && StockZones.ZoneOfTile(AITown.GetLocation(town)) == best ? 100000000 : 0);
		}, best);
		towns.Sort(AIList.SORT_BY_VALUE, false);
		local r = StockZones.HQ_SEARCH_RADIUS;
		local maxX = AIMap.GetMapSizeX() - 2;
		local maxY = AIMap.GetMapSizeY() - 2;
		local townsTried = 0;
		local execMode = AIExecMode();
		foreach(town, _ in towns) {
			if(townsTried++ >= StockZones.HQ_MAX_TOWNS) break;
			local center = AITown.GetLocation(town);
			local cx = AIMap.GetTileX(center);
			local cy = AIMap.GetTileY(center);
			local candidates = AIList();
			for(local dy = -r; dy <= r; dy++) {
				for(local dx = -r; dx <= r; dx++) {
				local x = cx + dx;
					local y = cy + dy;
					if(x < 1 || y < 1 || x > maxX || y > maxY) continue;
					candidates.AddItem(AIMap.GetTileIndex(x, y), abs(dx) + abs(dy));
				}
			}
			candidates.Sort(AIList.SORT_BY_VALUE, true); // nearest to the town centre first
			foreach(tile, _ in candidates) {
				if(!AITile.IsBuildableRectangle(tile, 2, 2)) continue;
				if(AICompany.BuildCompanyHQ(tile)) {
					HgLog.Info("hq: headquarters placed near "+AITown.GetName(town)+" zone:"+StockZones.Home());
					return true;
				}
				if(AIError.GetLastError() == AIError.ERR_NOT_ENOUGH_CASH) {
					HgLog.Info("hq: not enough cash for the headquarters, waiting");
					return false;
				}
			}
		}
		HgLog.Info("hq: no free place near the biggest towns (try "+StockZones.hq.tries+"/"+StockZones.HQ_MAX_TRIES+")");
		return false;
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

	static diagDate = {last = 0};

	/** A few lines in the AI log every 30 game days that say WHY the AI may find no route: the income of 50 tiles of passengers, the taxes that the estimate adds,
	 *  the engines that can be built, the industries and towns on the map, the zone, the permits and the money. Diagnostics only, changes nothing. */
	static function LogDiagnostics() {
		local today = AIDate.GetCurrentDate();
		if(today - StockZones.diagDate.last < 30) return;
		StockZones.diagDate.last = today;
		try {
			local pass = null;
			foreach(c, _ in AICargoList()) {
				if(AICargo.GetCargoLabel(c) == "PASS") { pass = c; break; }
			}
			local income = pass != null ? AICargo.GetCargoIncome(pass, 50, 20) : -1;
			HgLog.Info("diag: date:"+AIDate.GetYear(today)+" towns:"+AITownList().Count()+" industries:"+AIIndustryList().Count()+" passIncome(50 tiles, 20 days, 1 unit):"+income);
			local engines = "";
			foreach(vt in [AIVehicle.VT_ROAD, AIVehicle.VT_RAIL, AIVehicle.VT_WATER, AIVehicle.VT_AIR]) {
				local l = AIEngineList(vt);
				l.Valuate(AIEngine.IsBuildable);
				l.KeepValue(1);
				engines += " vt"+vt+":"+l.Count();
			}
			HgLog.Info("diag: buildable engines"+engines);
			if(StockZones.ApiOk() && StockZones.Count() > 0) {
				local perm = "";
				for(local z = 0; z < StockZones.Count(); z++) perm += " z"+z+":"+AICompany.GetPermitDaysLeft(AICompany.COMPANY_SELF, z)+"d/fee"+AICompany.GetPermitFee(z);
				HgLog.Info("diag: home zone:"+StockZones.Home()+" hq:"+AIMap.IsValidTile(AICompany.GetCompanyHQ(AICompany.COMPANY_SELF))+" transportTax/year road:"+AICompany.GetTransportTaxPerYear(1)+" rail:"+AICompany.GetTransportTaxPerYear(0)+" landTax/tile/year:"+AICompany.GetLandTaxPerTileYear()+" unborrowed:"+AICompany.GetUnborrowedCash(AICompany.COMPANY_SELF)+" permits"+perm);
			}
		} catch(e) {
			HgLog.Warning("diag failed: "+e);
		}
	}

	/** Keep the permits of the zones where the company already has stations. Later work on an existing route (a new station, a depot, a longer rail or road,
	 *  a repair) is a construction in that zone too, and without a valid permit the mod charges the 400% price. Looks at the stations every
	 *  MAINTAIN_EVERY_DAYS game days; renews a permit that has fewer than MAINTAIN_RENEW_BELOW days left, when the fee is small against the unborrowed cash.
	 *  Never buys a permit for a zone where the company has no station (PrepareCandidate does that before a new route). */
	static function MaintainPermits() {
		if(!StockZones.ApiOk() || StockZones.Count() <= 0) return;
		local home = StockZones.Home();
		if(home < 0) return;
		local today = AIDate.GetCurrentDate();
		if(today - StockZones.state.lastMaintain < StockZones.MAINTAIN_EVERY_DAYS) return;
		StockZones.state.lastMaintain = today;

		local zones = {};
		foreach(station, _ in AIStationList(AIStation.STATION_ANY)) {
			local z = StockZones.ZoneOfTile(AIStation.GetLocation(station));
			if(z >= 0 && z != home) zones.rawset(z, true);
		}
		foreach(z, _ in zones) {
			if(AICompany.GetPermitDaysLeft(AICompany.COMPANY_SELF, z) >= StockZones.MAINTAIN_RENEW_BELOW) continue;
			local fee = AICompany.GetPermitFee(z);
			if(fee < 0) continue;
			if(AICompany.GetUnborrowedCash(AICompany.COMPANY_SELF) < fee * StockZones.MAINTAIN_CASH_SHARE) continue;
			local execMode = AIExecMode();
			if(AICompany.BuyBuildingPermit(z)) {
				HgLog.Info("zone: renewed the building permit of zone "+z+" fee:"+fee);
			} else {
				HgLog.Warning("zone: could not renew the permit of zone "+z+" "+AIError.GetLastErrorString());
			}
		}
	}
}
