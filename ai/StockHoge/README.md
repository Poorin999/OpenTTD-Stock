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

## Where it lives
This folder is part of the OpenTTD-Stock repository (`ai/StockHoge`), so one push updates the game and the AI together. The CI build copies it into the
build output (`ai/StockHoge` next to `openttd.exe`), so the AI is ready to use after unpacking the build. `src/stock_ai.cpp` is the bot of the mod
(trading, growth cash, central bank); there is no second copy of it any more (the old `mod-patch/` folder is gone).

## Install (only when you run your own build)
Copy the folder `ai/StockHoge` to `Documents/OpenTTD/ai/`, start a new game, add `StockHoge` under AI settings.

## Status
Not tested in a running game. Please send the AI log if something fails.
