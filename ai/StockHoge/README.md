# StockHoge

**AAAHogEx adapted to the OpenTTD-Stock mod.** AAAHogEx is the AI by Rei Ishibashi (GPL v3, see LICENSE). This copy only changes how it handles money;
everything else (route search, trains, road vehicles, ships, aircraft, cargo chains) is the original code.

## Why it had to be changed
AAAHogEx pays for its growth with the original OpenTTD loan (`AICompany.SetMinimumLoanAmount`). The OpenTTD-Stock mod removes that loan (the commands are
refused); the only lender is the central bank, driven by the bot in `src/stock_ai.cpp`. Run unchanged, AAAHogEx would wait for money that never comes
(`WaitForPrice` has no end) and would count the vanilla maximum loan as money it can spend.

## Changes (all in main.nut, plus info.nut for the name)
- `GetUsableMoney()`: only the cash counts (no vanilla loan limit).
- `WaitForPrice()`: no loan requests; a wait that did not give a maximum now ends after 150 days (callers already handle `false`).
- `DoInterval()`: the loan request is removed.
- info.nut: name `StockHoge`, short name `SHGE`.
- No call of the original loan system is left in the code (`GetLoanAmount`, `GetMaxLoanAmount`, `SetMinimumLoanAmount` and the setting `difficulty.max_loan`):
  - `GetInflationRate()` used the maximum loan to measure inflation. It now compares the cost of clearing a grass tile with its cost when the AI first looked; the base value is saved with the game.
  - `_IsRich()` no longer asks whether the old loan is being repaid. A company counts as rich only when it has the cash and its transport earned money over the last quarter.
  - The main loop no longer tracks the old loan amount.
- The AI sees the money system of the mod: `AICompany.GetCentralBankDebt()` is used (the mod's `apply_overlay.py` adds `GetCentralBankDebt` and `GetCentralBankAvailable` to the script API).
  `_IsRich()` counts only the company's own money (cash minus the debt at the central bank), and the log of every turn shows the debt and the own money.
  On a build of the mod without these functions the AI logs a warning once and works with the cash only.

## Money zones, building permits and taxes (version 117)
New file `zones.nut` (class `StockZones`) and a few small hooks. All of it uses the script API that `apply_overlay.py` adds to `AICompany`
(`GetZoneCount`, `GetZoneOfTile`, `GetHomeZone`, `GetZoneTaxPercent`, `GetZoneBorderFeePermille`, `GetPermitDaysLeft`, `GetPermitFee`, `BuyBuildingPermit`,
`GetUnborrowedCash`, `GetTransportTaxPerYear`, `GetLandTaxPerTileYear`). On a build without them the AI logs one warning and plays as before.
- **Permits (main.nut, `_ScanPlaces`)**: every route candidate goes through `StockZones.PrepareCandidate(t)` before it is built. The AI looks at both ends and a point
  every 8 tiles on the line between them. For every zone that is not the zone of its headquarters and where the permit has fewer than 45 game days left, it buys the
  permit (`AICompany.BuyBuildingPermit`, paid with unborrowed cash) BEFORE building, so it never pays the 400% price of a build without a permit. The fee and the 50%
  surcharge of the zone are added to `estimate.buildingCost`. A route is skipped when the permits cannot be paid, or when fees plus surcharge are more than half of the project.
- **Keeping permits (`StockZones.MaintainPermits`, version 117)**: once per turn of the main loop, at most every 15 game days, the AI looks at the zone of every station it owns.
  For a foreign zone where the permit has fewer than 30 game days left it renews the permit, but only when the fee is at most 1/20 of its unborrowed cash. Later work on an
  existing route (new station, depot, longer track, repair) is a construction too and would cost 400% without a permit. It never buys a permit for a zone where it has no station.
  The permit gate of the game (a human company cannot build without a permit) does NOT apply to the AI: the AI is only charged the higher price.
- **Zone border**: a delivery across a border pays an exchange fee. The value of the route is lowered by that fee, and a route whose fee is 30% or more of the income is skipped.
- **Taxes**: `HogeAI.GetFullRunningCost(engine)` = running cost + transport tax of the vehicle (used where estimator.nut judged a vehicle by its running cost);
  `InfrastructureCost.GetCostPerDistanceRail/Road` add the land tax of one tile per distance unit.
- The game clock needs nothing new here: the AI already measures time in game days (`GetDayLengthFactor`), and the zones, permits and taxes run on the same game days.
- Tested: the 17 files compile (the Squirrel compiler of OpenTTD 15.3), `StockZones` passes a test with a mock API (buys, skips, no double work), the C++ side compiles
  against OpenTTD 15.3 with the overlay. NOT tested in a running game.

## Where it lives
This folder is part of the OpenTTD-Stock repository (`ai/StockHoge`), so one push updates the game and the AI together. The CI build copies it into the
build output (`ai/StockHoge` next to `openttd.exe`), so the AI is ready to use after unpacking the build. `src/stock_ai.cpp` is the bot of the mod
(trading, growth cash, central bank); there is no second copy of it any more (the old `mod-patch/` folder is gone).

## Install (only when you run your own build)
Copy the folder `ai/StockHoge` to `Documents/OpenTTD/ai/`, start a new game, add `StockHoge` under AI settings.

## Status
Not tested in a running game. Please send the AI log if something fails.
