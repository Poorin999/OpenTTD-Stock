# OpenTTD-Stock: Roadmap and notes for AI assistants

Read this file first. It says what the project is, how it is built, what exists, what is planned and which mistakes have already cost a failed build.
Keep it up to date: when you finish or start something, change the lists below.

## 1. What this is
A mod of OpenTTD (the build uses OpenTTD 15.x, Windows MSVC, GitHub Actions `build-windows.yml`) that adds a finance layer to the game:
- **Trading Terminal** (`stock_gui.cpp`, `stock_base.*`, `stock_cmd.*`): company shares as candlestick charts, long/short positions, margin, SL/TP, pending orders.
- **Wallets** (`wallet_*`): cash plus trading wallets. Money can only go into a trading wallet from unborrowed cash.
- **Central Bank** (`central_bank*.{h,cpp}`): the only lender. Loans, daily interest, penalties, account log, account statement (ledger).
- **Past statements window** (`central_bank_gui.cpp`): 10 years, years on the left, the statement of the chosen year on the right.
- **Gold reserve** (`gold_reserve.{h,cpp}`): organic gold price, limited market stock, gold held by companies, backing ratio, credit standing.
- **Gold panel** (`gold_panel.{h,cpp}`): drawing and Buy/Sell actions. It is shown in the **operating profit graph window** (OpenTTD `graph_gui.cpp`), in the place of the graph; the Gold / Graph button in the title bar switches back to the graph. It is NOT in the Trading Terminal any more.
- **Game clock** (`game_time/`): the game has an HH:MM clock; days, candles and timeframes are derived from one number.
- Retail investors (`RetailMarketManager.*`), stock AI (`stock_ai.cpp`).

The repository is an **overlay**, not a full OpenTTD tree. `apply_overlay.py <openttd-tree>` copies the files in, registers commands, CMake files and strings, and patches some original files.

## 2. How to work in this repository
- Files live in the folder named in `FILES` of `apply_overlay.py` (for example `src/saveload/stock_sl.cpp`, `src/widgets/stock_widget.h`). **Edit those.**
  `src/stock_sl.cpp` and `src/stock_widget.h` are old copies and are NOT used. Do not edit them (they can be deleted).
- A new source file needs entries in: `FILES`, `NEW_SOURCES` (CMake), and `STRING_FILES` if it has strings, all in `apply_overlay.py`.
- A new command needs: the name in `NEW_COMMANDS` (apply_overlay.py), a `DEF_CMD_TRAIT` in a header that `wallet_cmd.h` includes, and the function.
  Commands run on every client of a multiplayer game, so they must give the same result everywhere.
- A new saved value needs a record kind in `saveload/stock_sl.cpp` (`StockRecordKind`: used so far 0-25; next free is 26), written in `Save`, read in `Load`, and reset in `StockMarket::Reset` (`stock_base.cpp`).
- We cannot compile here. The only compiler is the GitHub Actions build. Read its log (the first error is the real one; later ones follow from it).

## 2b. Whole OpenTTD files that the overlay replaces
`replace/src/graph_gui.cpp` and `replace/src/widgets/graph_widget.h` are modified copies of the OpenTTD 15.3 files (marker: `GOLD_PANEL_PATCH`). `apply_overlay.py` checks the sha256 of the original in the tree (`REPLACED_FILES`) and stops if it differs, so another OpenTTD version is never overwritten silently. To port to another version: take that version's two files, redo the small edits (search `GOLD_PANEL_PATCH` and `WID_PHG_GOLD_`), and update the hashes.
Note: the 15.3 files use `Colours::Brown`, `WindowClass::...`, `FontSize::...`; the older mod files use `COLOUR_GREY`, `WC_...`, `FS_NORMAL`. If the build says one of those names does not exist, that is the cause.

The files are in `replace/`, NOT in `src/`: the workflow step "Add stock market source files" (.github/workflows/build-windows.yml) refuses to copy any file from `src/` of this repository that OpenTTD 15.3 already has, and fails the build. `apply_overlay.py` copies them (`install_replaced_files`) after checking the original.
The CI order is: checkout OpenTTD 15.3, `git apply stock-market.patch`, copy `src/` (no clashes allowed), `python3 mod/apply_overlay.py openttd`, build.

## 3. Rules that already cost a failed build
1. **Comments after `///<` in `src/widgets/*_widget.h` must be plain ASCII with no double quotes.** The script API generator copies them into `script_window.hpp` as strings. A quote or Thai text there gives `C2001 newline in constant` followed by dozens of `enum type redefinition` errors.
2. All game maths is **integer** (no `double`, `pow`, `sqrt` in anything that changes game state). Floating point can desync multiplayer. `double` is fine for drawing and for parsing what the player types.
3. Money is `Money` (int64). Convert with `static_cast<int64_t>`. The gold price is kept in 1/100 of a money unit (`price_cents`).
4. Drawing: keep every `DrawString` inside `[left, right]` and skip lines that do not fit above `bottom` (see `DrawGoldLine`). Never draw outside the widget rectangle.
5. Fund isolation: nothing may be bought with borrowed money. Use `GetUnborrowedLiquidCash(company)` (central_bank.h).
6. Thai text is allowed in `.cpp` files (see `CbText("ไทย", "English")` in `central_bank_gui.cpp`), not in widget headers.
7. **Never put a copy of an OpenTTD file in `src/` or `src/widgets/` of this repository.** The build step "Add stock market source files" fails with `src/<file> already exists in OpenTTD 15.3 ... refusing to overwrite it` (happened with `graph_gui.cpp` and `widgets/graph_widget.h`, which were duplicated in `src/` and `replace/src/`). Modified copies of OpenTTD files go ONLY in `replace/` and are listed in `REPLACED_FILES` of `apply_overlay.py`.
8. Do not mention or rely on window numbers twice: `WC_CENTRAL_BANK` uses number 0 (bank), 1 (statement), 2 (past statements).

## 4. Gold system (current design)
Price per ounce, updated once per game day, moves 5% of the way to a target. Target = base price (2000) x:
1. inflation of the game (`_economy.inflation_prices`),
2. cash in circulation against the start (half of the change counts; loans make cash bigger),
3. demand: companies buying push the price up, selling pushes it down; the effect fades 3% a day; max +-30%,
4. scarcity: the market has a limited stock (`MARKET_STOCK_START` = 200,000 oz). Buying takes from it, selling gives back. Half the stock left = price x2, capped at x4,
5. safe haven: when the summed company value of the map falls from one quarter to the next, up to +20%.

Backing ratio = value of all gold reserves / cash in circulation. The central bank starts with gold worth 8% of the cash. Credit standing: C < 6%, B, A (12%), AA (20%), AAA (30%).
Buy is paid from unborrowed liquid cash only. No spread, no commission yet. Gold is not a position: no margin, no SL/TP.
Source of gold: a world market with a limited stock. It is **not** connected to mines or cargo yet.

## 5. Done
- [x] Central bank, loans, interest, account log, ledger and account statement.
- [x] Past statements window (10 years). Yearly expenses of the game (3 years only) are copied into the ledger every day so 10 years exist from now on; older years show n/a.
- [x] Gold reserve: price engine, commands `CMD_GOLD_BUY` / `CMD_GOLD_SELL`, savegame records, gold panel in the Trading Terminal (toggle with the Gold button, replaces the chart area while on).
- [x] Limited market stock for gold (scarcity price).
- [x] Gold panel moved from the Trading Terminal into the operating profit graph window (replaces the graph; Gold / Graph button switches).
- [x] Build fix: removed the duplicate `src/graph_gui.cpp` and `src/widgets/graph_widget.h` (identical to the files in `replace/`), which made the "Add stock market source files" step fail.

## 6. Planned (in a sensible order)
1. **Gold in the account statement**: add `LedgerItem` lines for gold purchases and sales, and a section in both statement windows. Realised profit of gold belongs in "Capital Gains / Realized Profit".
2. **Spread and commission for gold**: buy a little above, sell a little below the spot price; commission into the ledger (`TradeCommission`).
3. **Gold from the map (source 3)**: gold or valuables delivered to a station/bank feed the market stock (and the reserve). Needs a hook where cargo is delivered (`economy.cpp`, `CargoPayment`); decide how much of the delivery counts. Keep the start stock so the market works on maps without a gold mine.
4. **Optional random layer**: daily noise of +-1-3 percent and world news events (war +15% in 3 days, big new mine -20%, central bank rate rise slowly lowers the price). Must use the game's synchronised random (`Random()` / `RandomRange`), never `std::rand`.
5. **Credit standing with an effect**: for example the tier changes the central bank interest rate or the loan limit.
6. **Script API**: `ScriptCompany::GetGoldHoldings`, `GetGoldPrice`, so game scripts and AIs can see it (see how `GetCentralBankDebt` was added in `apply_overlay.py`).
7. **Translations**: the gold panel is English only.
8. **Gold price chart**: a history of the daily price (needs a saved ring buffer).

## 6b. Evaluated and NOT done: three real climates in one map
The owner asked for Temperate + Sub-arctic + Sub-tropical in one map, chosen at world generation. Checked against OpenTTD 15.3 sources (genworld.cpp, genworld_gui.cpp, industry_cmd.cpp, landscape.cpp, tgp.cpp, settings/*.ini):
- `game_creation.landscape` is one value for the whole game, read in about 17 places in those files alone (e.g. `industry_cmd.cpp:93` enables industries by `climate_availability`; `:2312` and `:2343` use `appear_creation[landscape]` and `appear_ingame[landscape]`; `landscape.cpp:1643` and `:1702` choose the generator step per climate).
- Not in those files, but known from OpenTTD: cargo types, houses, vehicles and the ground/tree sprite sets are also loaded per climate and share ids, so they would collide if two climates were active. This cannot be done with an overlay and cannot be tested without a compiler.
- Native alternative that already exists: Sub-tropical has three zones in one map (Normal, Desert, Rainforest; `TropicZone`, `CreateDesertOrRainForest`, Desert coverage in Generate World).
Possible work: a Generate World option for the three Sub-tropical zones (rainforest coverage slider, guarantee that all three exist). Needs edits of landscape.cpp, genworld_gui.cpp, world_settings.ini.

## 7. Open questions for the owner
- Should the gold panel replace the chart for good, or stay a toggle (now: toggle)?
- Should the market stock be per game or refill slowly (mines opening)?
- Which currency scale is right for the base price (2000 per ounce suits a game that starts with about 100k)?
