/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file stock_ai.cpp The money manager and trader of the AI companies: central bank loans, trading wallet and the trading terminal. */

/*
 * What this does, in order of priority (every decision hour of the game clock, per AI company):
 *
 *   1. Survive.   Keep a cash reserve (a minimum plus 30 days of interest at the worst rate) so the bank never puts the
 *                 company in default. When the cash is low: first take money back from the trading wallet, and only then
 *                 borrow, and never beyond 80% of the credit limit (so the debt-to-equity ratio stays in the cheap part of the
 *                 risk curve). Borrowing stops while the account is in default.
 *   2. Be cheap.  Cash above the reserve repays the central bank loan.
 *   3. Invest.    Own (unborrowed) money above the reserve is moved into the first trading wallet, up to a share of the own capital.
 *                 The bank guards in CmdWalletTransfer / CmdStockOpen make it impossible to trade loan money anyway.
 *   4. Trade.     The price of a share glides to the valuation (StockMarket::base_fp), multiplied by the accumulated market impact of the
 *                 orders. So a price that is below the valuation (somebody sold a lot) will come back up, and one that is above it will
 *                 come back down. The bot buys the cheap ones and sells (shorts) the expensive ones, when the gain after spread, commission,
 *                 swap and the impact of its own order is at least STOCK_AI_MIN_EDGE_PPM. Every trade gets a Stop Loss, a Take Profit at the
 *                 valuation and a trailing stop. Companies whose last four quarters show a loss are never bought.
 *   5. Be safe.   At most STOCK_AI_MAX_POSITIONS positions, each at most STOCK_AI_MAX_POSITION_PERCENT of the wallet equity, all together at most
 *                 STOCK_AI_MAX_EXPOSURE_PERCENT (1x, while the terminal allows 10x). Positions are closed when the target price is
 *                 reached, when they are held too long, and when the margin warning shows up.
 *
 * Valuables: gold, silver, platinum and diamonds are traded by the same rule as the shares. Their fair price is the price of the price model
 *                 (StockMarket::base_fp), there are no earnings to look at, the edge that is needed is smaller (STOCK_AI_VALUABLE_MIN_EDGE_PPM),
 *                 and at most STOCK_AI_MAX_VALUABLE_POSITIONS positions are open in them. Switch off with STOCK_AI_TRADE_VALUABLES.
 *
 * Vault gold: the AI companies also keep some real gold in the company vault (CmdGoldBuy / CmdGoldSell, the same as the Buy gold window of a human
 *                 player), up to STOCK_AI_VAULT_TARGET_PERCENT of their own capital. They buy when the gold price is below the average of the last
 *                 30 days, sell half when it is well above it, and sell first (before borrowing) when the cash falls under the reserve.
 *                 Only unborrowed cash buys gold and only when there is no debt at the central bank. Switch off with STOCK_AI_TRADE_VAULT_GOLD.
 *
 * Growth cash: a share of the credit limit is kept as working cash, so a transport AI (for example StockHoge) can pay for a new line at once.
 *
 * Network games: only the saved game state is read, only integers are used, and nothing is stored in the bot itself, so every client takes the same
 * decisions at the same moment. The commands of the terminal are called directly, with _current_company set to the AI company.
 */

#include "stdafx.h"
#include "stock_cmd.h"
#include "stock_base.h"
#include "valuables.h"
#include "gold_reserve.h"
#include "wallet_base.h"
#include "wallet_cmd.h"
#include "central_bank.h"
#include "company_base.h"
#include "company_func.h"
#include "core/backup_type.hpp"
#include "game_time/game_time_manager.h"

#include <algorithm>
#include <vector>

#include "safeguards.h"

namespace {

/* ---- Switches ---- */

/** True: the bot also plays the human companies. Only for testing alone in a game without AI companies; it takes over your money! */
constexpr bool STOCK_AI_PLAY_ALL_COMPANIES = false;
/** False: the bot only buys (long). True: it also sells (short) shares that are too expensive. */
constexpr bool STOCK_AI_ALLOW_SHORTS = true;
/** True: the bot also trades the valuables (gold, silver, platinum, diamonds) of the terminal, with the same rule as the shares (see below). */
constexpr bool STOCK_AI_TRADE_VALUABLES = true;
/** True: the bot also buys and sells real gold in the company vault (see the header). */
constexpr bool STOCK_AI_TRADE_VAULT_GOLD = true;

/* ---- Timing ---- */

/** The bot decides once per this many game hours (of the HH:MM clock of the mod, 1 game hour = 1 real minute) per company. The companies are spread over the hours. */
constexpr int64_t STOCK_AI_DECISION_INTERVAL_HOURS = 1;

/* ---- Money management ---- */

/** The smallest cash reserve of a company. */
constexpr int64_t STOCK_AI_MIN_RESERVE = 150'000;
/** Working capital for growth: this share of the credit limit is kept as extra cash (borrowed when needed), so that a transport AI can pay for a new
 *  line at once instead of waiting for months. It is idle cash only while the account is in good standing, and it costs only the interest on it. */
constexpr int64_t STOCK_AI_GROWTH_CASH_PERCENT = 40;
/** The reserve also covers this many days of interest at the highest penalty rate. */
constexpr int64_t STOCK_AI_RESERVE_INTEREST_DAYS = 30;
/** Below this share of the whole reserve (minimum, interest and growth cash) the bot borrows. */
constexpr int64_t STOCK_AI_BORROW_BELOW_PERCENT = 50;
/** The bot borrows at most this share of the credit that is still available. */
constexpr int64_t STOCK_AI_BORROW_SAFETY_PERCENT = 80;
/** The trading wallet is filled up to this share of the own capital (unborrowed cash plus the equity of the wallet). */
constexpr int64_t STOCK_AI_WALLET_TARGET_PERCENT = 30;
/** Money moves between the cash and the wallet only in pieces of at least this size, so there is no endless shuffling. */
constexpr int64_t STOCK_AI_MIN_TRANSFER = 20'000;
/** When money is taken out of the wallet, this share of its equity stays as free margin. */
constexpr int64_t STOCK_AI_KEEP_FREE_PERCENT = 30;

/* ---- Trading ---- */

constexpr size_t STOCK_AI_MAX_POSITIONS = 6;            ///< Open positions per company.
constexpr int64_t STOCK_AI_MAX_POSITION_PERCENT = 20;   ///< One position, as a share of the wallet equity (value of the position, not margin).
constexpr int64_t STOCK_AI_MAX_EXPOSURE_PERCENT = 100;  ///< All positions together, as a share of the wallet equity.
constexpr int64_t STOCK_AI_MIN_NOTIONAL = 5'000;        ///< Smallest position worth opening.
constexpr int64_t STOCK_AI_MIN_PRICE = 30;              ///< Shares below this price are not traded: whole money units make the stops too coarse.
constexpr uint32_t STOCK_AI_MIN_VOLUME = 10;            ///< Smallest volume, in shares.
constexpr int64_t STOCK_AI_MIN_EDGE_PPM = 70'000;       ///< Gain that is expected after all costs, in ppm of the price (7%).
constexpr int64_t STOCK_AI_VALUABLE_MIN_EDGE_PPM = 25'000; ///< The same for a valuable (2.5%): its price stays closer to the fair price than the price of a share, and it has no earnings to be wrong about.
constexpr size_t STOCK_AI_MAX_VALUABLE_POSITIONS = 2;   ///< Open positions in valuables per company, so the shares stay the main business.
constexpr int64_t STOCK_AI_VAULT_TARGET_PERCENT = 10;   ///< Real gold in the vault, as a share of the own capital (unborrowed cash + wallet + gold).
constexpr int64_t STOCK_AI_VAULT_BUY_BELOW_PPM = 20'000; ///< Buy gold when the price is this far (2%) below the average of the last days.
constexpr int64_t STOCK_AI_VAULT_SELL_ABOVE_PPM = 40'000; ///< Sell gold when the price is this far (4%) above that average.
constexpr int64_t STOCK_AI_VAULT_SELL_SHARE_PERCENT = 50; ///< A sale gives this share of the gold (at least 1 ounce).
constexpr int64_t STOCK_AI_VAULT_MIN_ORDER = 5'000;      ///< Smallest purchase of vault gold worth making.
constexpr size_t STOCK_AI_VAULT_REF_DAYS = 30;          ///< Days of the average price.
constexpr size_t STOCK_AI_VAULT_MIN_HISTORY = 10;       ///< No gold trade before the chart has this many days.
constexpr int64_t STOCK_AI_EXPECTED_HOLD_DAYS = 30;     ///< Game days that a trade is expected to be open, for the swap.
constexpr int64_t STOCK_AI_STOP_LOSS_PPM = 100'000;     ///< Stop Loss distance from the entry price (10%).
constexpr int64_t STOCK_AI_TRAIL_PPM = 80'000;          ///< Trailing stop distance (8%).
constexpr int32_t STOCK_AI_MAX_HOLD_MONTHS = 3;         ///< A position that is open this many game months is closed.
constexpr int64_t STOCK_AI_FALLING_KNIFE_PPM = 150'000; ///< A share that lost this much in three months is not bought.
constexpr int64_t STOCK_AI_BAD_DRIFT_PPM = -50'000;     ///< Expected fall of the valuation at the next quarter, in ppm; below this a share is not bought.

/** Parts per million of a fraction. */
static int64_t Ppm(int64_t num, int64_t den)
{
	return den > 0 ? num * STOCK_PPM / den : 0;
}

/**
 * Run a command in two steps: a test first, and only when that works, the real thing.
 * @param cmd Function that takes the flags and calls the command.
 * @return True when the command was executed.
 */
template <typename F>
static bool TryRun(F &&cmd)
{
	if (cmd(DoCommandFlags{}).Failed()) return false;
	return cmd(DoCommandFlags{DoCommandFlag::Execute}).Succeeded();
}

/** Cash that the company keeps: a minimum, a month of interest at the highest penalty rate, and the working capital for growth. */
static int64_t CashReserve(CompanyID id)
{
	const int64_t daily = BankDailyInterest(GetCentralBankDebt(id), BankPenaltyRateBp(BANK_PENALTY_LEVEL_MAX, _central_bank.policy.shift_bp), 0).interest; // The penalty rate is higher in emergency mode.
	/* No growth cash for an account in default: then only the minimum counts and the bot does not borrow at all. */
	const int64_t growth = _central_bank.accounts[id.base()].in_default ? 0 : GetCentralBankCreditLimit(id) * STOCK_AI_GROWTH_CASH_PERCENT / 100;
	return STOCK_AI_MIN_RESERVE + STOCK_AI_RESERVE_INTEREST_DAYS * daily + growth;
}

/** Average gold price of the last days in 1/100 money units; 0 when the market has not started or the chart is too short. */
static int64_t GoldReferenceCents()
{
	const std::vector<int64_t> &h = _gold_reserve.history_cents;
	if (_gold_reserve.price_cents <= 0 || h.size() < STOCK_AI_VAULT_MIN_HISTORY) return 0;
	const size_t n = std::min(h.size(), STOCK_AI_VAULT_REF_DAYS);
	int64_t sum = 0;
	for (size_t i = h.size() - n; i < h.size(); i++) sum += h[i];
	return sum / static_cast<int64_t>(n);
}

/** Sell this many ounces of the vault gold (at most what the company holds). True when the sale was made. */
static bool SellVaultGold(CompanyID id, int64_t ounces)
{
	ounces = std::min(ounces, _gold_reserve.GetHoldings(id));
	if (ounces <= 0 || ounces > 1'000'000) return false;
	const uint32_t amount = static_cast<uint32_t>(ounces);
	return TryRun([&](DoCommandFlags f) { return CmdGoldSell(f, amount); });
}

/** The fair price of a share (the valuation), times #STOCK_PRICE_SCALE. 0 when it is not known yet. */
static int64_t FairFixed(CompanyID target)
{
	return _stock_market.base_fp[target.base()];
}

/**
 * The next valuation change that the earnings already decide, in ppm of the price. At the end of the quarter the oldest of
 * the four quarters of earnings drops out and the newest enters; assuming the new quarter looks like the last one, the
 * earnings change by (last - oldest) and the price by STOCK_EARNINGS_MULTIPLE times that. Only with four known quarters.
 */
static int64_t EarningsDriftPpm(CompanyID target)
{
	const Company *c = Company::GetIfValid(target);
	if (c == nullptr) return 0;
	const StockValuation &v = _stock_market.valuation[target.base()];
	if (v.quarters < STOCK_EARNINGS_QUARTERS || v.book <= 0) return 0;
	const CompanyEconomyEntry &last = c->old_economy[0];
	const CompanyEconomyEntry &oldest = c->old_economy[STOCK_EARNINGS_QUARTERS - 1];
	const int64_t q_last = static_cast<int64_t>(last.income) + static_cast<int64_t>(last.expenses);
	const int64_t q_oldest = static_cast<int64_t>(oldest.income) + static_cast<int64_t>(oldest.expenses);
	const int64_t cap = std::max<int64_t>(1, GetStockMarketCap(target));
	return std::clamp<int64_t>(Ppm((q_last - q_oldest) * STOCK_EARNINGS_MULTIPLE, cap), -STOCK_PPM, STOCK_PPM);
}

/** Change of the monthly close, from three candles ago to now, in ppm. 0 when there is not enough history. */
static int64_t TrendPpm(CompanyID target)
{
	const auto &candles = _stock_market.candles[target.base()];
	if (candles.size() < 3) return 0;
	const int64_t old_close = candles[candles.size() - 3].close;
	return Ppm(GetStockPrice(target) - old_close, old_close);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* 1 + 2 + 3: the money                                                                                               */
/* ------------------------------------------------------------------------------------------------------------------ */

/** Keep the company out of default: take money out of the trading wallet, and borrow when that is not enough. */
static void EnsureCash(Company *c, uint8_t wallet)
{
	const CompanyID id = c->index;
	const int64_t reserve = CashReserve(id);
	int64_t money = static_cast<int64_t>(c->money);
	if (money >= reserve) return;

	/* First the own money in the wallet: it costs no interest. Keep some free margin for the open positions. */
	const int64_t equity = GetStockEquity(id, wallet);
	const int64_t free_margin = GetStockFreeMargin(id, wallet) - equity * STOCK_AI_KEEP_FREE_PERCENT / 100;
	const int64_t take = std::min({reserve - money, GetWalletBalance(id, wallet), free_margin});
	if (take >= STOCK_AI_MIN_TRANSFER) {
		if (TryRun([&](DoCommandFlags f) { return CmdWalletTransfer(f, wallet, WALLET_CASH, Money(take)); })) money += take;
	}

	/* Then the gold of the vault: it is own money too and costs no interest (a sale only pays the spread and the tax on the profit). */
	money = static_cast<int64_t>(c->money);
	if (STOCK_AI_TRADE_VAULT_GOLD && money < reserve && _gold_reserve.price_cents > 0 && _gold_reserve.GetHoldings(id) > 0) {
		const int64_t net_per_ounce = std::max<int64_t>(1, _gold_reserve.GetSellNet(1));
		if (SellVaultGold(id, (reserve - money + net_per_ounce - 1) / net_per_ounce)) money = static_cast<int64_t>(c->money);
	}

	/* Then the central bank, only when the cash is really low and the account is in good standing. */
	if (money >= reserve * STOCK_AI_BORROW_BELOW_PERCENT / 100) return;
	if (_central_bank.accounts[id.base()].in_default) return;
	const int64_t amount = std::min(reserve - money, GetCentralBankAvailable(id) * STOCK_AI_BORROW_SAFETY_PERCENT / 100);
	if (amount > 0) TryRun([&](DoCommandFlags f) { return CmdBankBorrow(f, WALLET_CASH, Money(amount)); });
}

/** Repay the loan with cash above the reserve, then fill the trading wallet with own money. */
static void PlaceSpareMoney(Company *c, uint8_t wallet)
{
	const CompanyID id = c->index;
	const int64_t reserve = CashReserve(id);

	/* Cash above the reserve pays the loan back: the interest is 3.5% to 12%, and it does not change what the company owns. */
	const int64_t debt = GetCentralBankDebt(id);
	int64_t spare = static_cast<int64_t>(c->money) - reserve;
	if (debt > 0 && spare >= STOCK_AI_MIN_TRANSFER) {
		const int64_t amount = std::min(debt, spare);
		if (TryRun([&](DoCommandFlags f) { return CmdBankRepay(f, WALLET_CASH, Money(amount)); })) spare -= amount;
	}

	/* Own money above the reserve goes to the wallet, up to a share of the own capital. Never loan money (the bank checks it too). */
	const int64_t unborrowed = GetUnborrowedLiquidCash(id);
	const int64_t equity = GetStockEquity(id, wallet);
	const int64_t target = (unborrowed + equity) * STOCK_AI_WALLET_TARGET_PERCENT / 100;
	const int64_t amount = std::min({target - equity, unborrowed, spare});
	if (amount >= STOCK_AI_MIN_TRANSFER) {
		TryRun([&](DoCommandFlags f) { return CmdWalletTransfer(f, WALLET_CASH, wallet, Money(amount)); });
	}
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* 4: the positions                                                                                                   */
/* ------------------------------------------------------------------------------------------------------------------ */

static bool ClosePositionNow(CompanyID owner, uint32_t id)
{
	const StockOrder *p = _stock_market.FindPosition(id, owner);
	if (p == nullptr) return false;
	const uint32_t volume = p->volume;
	return TryRun([&](DoCommandFlags f) { return CmdStockClose(f, id, volume); });
}

/**
 * Look at one open position: close it when the target is reached, when it is held too long or when the company turned bad,
 * otherwise bring the Take Profit to the (moved) valuation and switch the trailing stop on.
 */
static void ReviewPosition(CompanyID owner, uint32_t id)
{
	StockOrder *p = _stock_market.FindPosition(id, owner);
	if (p == nullptr) return; // Closed by its Stop Loss or Take Profit.

	const CompanyID target{p->target};
	const bool valuable = IsValuableSymbol(p->target); // A valuable has no company, no earnings and no valuation: its fair price is the price of the price model.
	if ((!valuable && !Company::IsValidID(target)) || _stock_market.price_fp[target.base()] <= 0) return;

	const bool buy = IsStockBuy(p->type);
	const int64_t bid = GetStockBid(target);
	const int64_t ask = GetStockAsk(target);
	const int64_t fair_fixed = FairFixed(target);

	/* The target of the trade: the valuation, rounded against ourselves so a take profit at that price is a real gain. */
	const int64_t tp = buy ? fair_fixed / STOCK_PRICE_SCALE : (fair_fixed + STOCK_PRICE_SCALE - 1) / STOCK_PRICE_SCALE;
	const bool reached = buy ? (bid >= tp) : (ask <= tp);
	const bool too_old = p->open_ym != 0 && GetStockCurrentMonth() - p->open_ym >= STOCK_AI_MAX_HOLD_MONTHS;
	bool turned_bad = false;
	if (!valuable && buy) {
		const StockSummary summary = GetStockSummary(target);
		turned_bad = summary.has_earnings && summary.eps < 0;
	}
	if (!buy && !STOCK_AI_ALLOW_SHORTS) { ClosePositionNow(owner, id); return; }

	if (reached || too_old || turned_bad || tp <= 0) {
		ClosePositionNow(owner, id);
		return;
	}

	const int64_t trail = std::max<int64_t>(1, p->price * STOCK_AI_TRAIL_PPM / STOCK_PPM);
	if (p->tp == tp && p->trail == trail) return;
	const int64_t sl = p->sl;
	TryRun([&](DoCommandFlags f) { return CmdStockModify(f, id, Money(sl), Money(tp), Money(0), Money(trail)); });
}

/** Close the worst positions while the margin of the wallet is in danger. */
static void GuardMargin(CompanyID owner, uint8_t wallet)
{
	for (size_t guard = 0; guard < STOCK_AI_MAX_POSITIONS + 1; guard++) {
		const StockRisk risk = GetStockRisk(owner, wallet);
		if (risk.level != StockRiskLevel::Warning && risk.level != StockRiskLevel::MarginCall) return;

		const StockOrder *worst = nullptr;
		int64_t worst_profit = 0;
		for (const StockOrder &p : _stock_market.positions) {
			if (p.owner != owner.base() || p.wallet != wallet) continue;
			const int64_t profit = GetStockProfit(p);
			if (worst == nullptr || profit < worst_profit) { worst = &p; worst_profit = profit; }
		}
		if (worst == nullptr || !ClosePositionNow(owner, worst->id)) return;
	}
}

/** One share that the bot would trade. */
struct Candidate {
	CompanyID target{};
	bool buy = true;
	int64_t score = 0;       ///< Net edge plus a small tilt for the earnings, in ppm.
	int64_t fair_fixed = 0;  ///< Valuation price times #STOCK_PRICE_SCALE.
};

/** Net gain in ppm of the entry price for a trade at the execution price @p exec, after the exit spread, commission and swap. */
static int64_t NetEdgePpm(CompanyID target, bool buy, int64_t exec, int64_t fair_fixed)
{
	const int64_t spread_fixed = std::max<int64_t>(0, GetStockAsk(target) - GetStockBid(target)) * STOCK_PRICE_SCALE;
	const int64_t exec_fixed = exec * STOCK_PRICE_SCALE;
	/* A long position is closed at the Bid, which is half a spread below the valuation; a short one at the Ask, half a spread above it. */
	const int64_t gain_fixed = buy ? (fair_fixed - spread_fixed / 2 - exec_fixed) : (exec_fixed - fair_fixed - spread_fixed / 2);
	return Ppm(gain_fixed, exec_fixed) - STOCK_COMMISSION_PPM - STOCK_SWAP_PPM_PER_DAY * STOCK_AI_EXPECTED_HOLD_DAYS;
}

/** Real gold in the company vault: sell half when the price is high, buy up to a target share of the own capital when it is low. */
static void TradeVaultGold(Company *c, uint8_t wallet)
{
	if (!STOCK_AI_TRADE_VAULT_GOLD) return;
	const CompanyID id = c->index;
	const int64_t ref = GoldReferenceCents();
	if (ref <= 0) return;
	const int64_t price = _gold_reserve.price_cents;
	const int64_t held = _gold_reserve.GetHoldings(id);

	if (held > 0 && price * GoldReserveManager::PPM > ref * (GoldReserveManager::PPM + STOCK_AI_VAULT_SELL_ABOVE_PPM)) {
		SellVaultGold(id, std::max<int64_t>(1, held * STOCK_AI_VAULT_SELL_SHARE_PERCENT / 100));
		return;
	}
	if (price * GoldReserveManager::PPM >= ref * (GoldReserveManager::PPM - STOCK_AI_VAULT_BUY_BELOW_PPM)) return;
	if (GetCentralBankDebt(id) > 0) return; // The loan is repaid first.

	const int64_t unborrowed = GetUnborrowedLiquidCash(id);
	const int64_t held_value = held * _gold_reserve.GetBidCents() / 100;
	const int64_t capital = unborrowed + GetStockEquity(id, wallet) + held_value;
	const int64_t want = capital * STOCK_AI_VAULT_TARGET_PERCENT / 100 - held_value;
	const int64_t spare = static_cast<int64_t>(c->money) - CashReserve(id);
	const int64_t budget = std::min({want, spare, unborrowed});
	if (budget < STOCK_AI_VAULT_MIN_ORDER) return;

	/* At most 1% of the stock of the market in one order, so the bot does not make gold scarce. */
	const int64_t ounces = std::min(_gold_reserve.GetMaxBuyOunces(budget), _gold_reserve.market_stock_oz / 100);
	if (ounces <= 0 || ounces > 1'000'000) return;
	const uint32_t amount = static_cast<uint32_t>(ounces);
	TryRun([&](DoCommandFlags f) { return CmdGoldBuy(f, amount); });
}

/** Look for the best share to trade, size it so the impact of the order does not eat the gain, and open the position. */
static void OpenBestTrade(CompanyID owner, uint8_t wallet)
{
	size_t open_positions = 0;
	size_t valuable_positions = 0;
	int64_t exposure = 0;
	for (const StockOrder &p : _stock_market.positions) {
		if (p.owner != owner.base() || p.wallet != wallet) continue;
		open_positions++;
		if (IsValuableSymbol(p.target)) valuable_positions++;
		exposure += p.price * static_cast<int64_t>(p.volume);
	}
	if (open_positions >= STOCK_AI_MAX_POSITIONS || _stock_market.CountOrders(owner) >= STOCK_MAX_ORDERS_PER_COMPANY) return;

	const int64_t equity = GetStockEquity(owner, wallet);
	if (equity <= 0) return;
	const int64_t room = equity * STOCK_AI_MAX_EXPOSURE_PERCENT / 100 - exposure;
	const int64_t notional = std::min(room, equity * STOCK_AI_MAX_POSITION_PERCENT / 100);
	if (notional < STOCK_AI_MIN_NOTIONAL) return;

	Candidate best;
	bool found = false;
	for (size_t symbol = 0; symbol < STOCK_SYMBOLS; symbol++) {
		const CompanyID target{static_cast<uint8_t>(symbol)};
		const bool valuable = IsValuableSymbol(symbol);
		if (valuable) {
			if (!STOCK_AI_TRADE_VALUABLES || valuable_positions >= STOCK_AI_MAX_VALUABLE_POSITIONS) continue;
		} else if (!Company::IsValidID(target) || target == owner) {
			continue;
		}
		if (!IsStockSymbolValid(symbol)) continue;
		if (_stock_market.price_fp[target.base()] <= 0 || FairFixed(target) <= 0) continue;
		if (GetStockPrice(target) < STOCK_AI_MIN_PRICE) continue;

		bool held = false;
		for (const StockOrder &p : _stock_market.positions) held = held || (p.owner == owner.base() && p.target == target.base());
		for (const StockOrder &p : _stock_market.pending) held = held || (p.owner == owner.base() && p.target == target.base());
		if (held) continue;

		const int64_t fair_fixed = FairFixed(target);
		const int64_t drift = valuable ? 0 : EarningsDriftPpm(target);
		const int64_t trend = TrendPpm(target);
		const StockSummary summary = valuable ? StockSummary{} : GetStockSummary(target);
		const int64_t min_edge = valuable ? STOCK_AI_VALUABLE_MIN_EDGE_PPM : STOCK_AI_MIN_EDGE_PPM;

		/* The price at which a small order would be executed shows how far the price is from the valuation. */
		for (int side = 0; side < 2; side++) {
			const bool buy = side == 0;
			if (!buy && !STOCK_AI_ALLOW_SHORTS) continue;
			if (buy && ((summary.has_earnings && summary.eps <= 0) || drift < STOCK_AI_BAD_DRIFT_PPM || trend < -STOCK_AI_FALLING_KNIFE_PPM)) continue;
			if (!buy && (drift > -STOCK_AI_BAD_DRIFT_PPM || trend > STOCK_AI_FALLING_KNIFE_PPM)) continue; // Do not short a share that is about to rise.

			const int64_t exec = GetStockExecutionPrice(target, buy, STOCK_AI_MIN_VOLUME);
			if (exec <= 0) continue;
			const int64_t edge = NetEdgePpm(target, buy, exec, fair_fixed);
			if (edge < min_edge) continue;

			const int64_t score = edge + (buy ? drift : -drift) / 4;
			if (!found || score > best.score) {
				best = {target, buy, score, fair_fixed};
				found = true;
			}
		}
	}
	if (!found) return;

	/* The order moves the price against us, so make it smaller until the gain is still enough at the price that it really gets. */
	const int64_t start_price = std::max<int64_t>(1, GetStockPrice(best.target));
	uint32_t volume = static_cast<uint32_t>(std::min<int64_t>(notional / start_price, STOCK_MAX_VOLUME));
	int64_t exec = 0;
	const int64_t best_min_edge = IsValuableSymbol(best.target.base()) ? STOCK_AI_VALUABLE_MIN_EDGE_PPM : STOCK_AI_MIN_EDGE_PPM;
	while (volume >= STOCK_AI_MIN_VOLUME) {
		exec = GetStockExecutionPrice(best.target, best.buy, volume);
		if (exec > 0 && NetEdgePpm(best.target, best.buy, exec, best.fair_fixed) >= best_min_edge) break;
		volume /= 2;
	}
	if (volume < STOCK_AI_MIN_VOLUME || exec <= 0) return;

	/* Stop Loss on the wrong side, Take Profit at the valuation (rounded so that it is a real gain). */
	const int64_t stop_distance = std::max<int64_t>(1, exec * STOCK_AI_STOP_LOSS_PPM / STOCK_PPM);
	const int64_t sl = best.buy ? exec - stop_distance : exec + stop_distance;
	const int64_t tp = best.buy ? best.fair_fixed / STOCK_PRICE_SCALE : (best.fair_fixed + STOCK_PRICE_SCALE - 1) / STOCK_PRICE_SCALE;
	if (sl <= 0 || (best.buy ? tp <= exec : tp >= exec)) return;

	const uint32_t new_id = _stock_market.next_id;
	const StockOrderType type = best.buy ? StockOrderType::Buy : StockOrderType::Sell;
	const CompanyID target = best.target;
	if (!TryRun([&](DoCommandFlags f) { return CmdStockOpen(f, target, type, volume, Money(0), Money(sl), Money(tp), 0, wallet); })) return;

	/* Switch the trailing stop on right away. (The position may already be gone when the new price reached a level.) */
	ReviewPosition(owner, new_id);
}

/** Everything that the bot does for one company. */
static void PlayCompany(Company *c)
{
	AutoRestoreBackup cur_company(_current_company, c->index);

	_wallets.EnsureDefaults(c->index);
	const Wallet *w = _wallets.FindFirst(c->index, WalletKind::Trading);
	if (w == nullptr) return;
	const uint8_t wallet = w->id;

	/* 1: never get into default. */
	EnsureCash(c, wallet);

	/* 4a: positions first, so that results and risk are known before new trades. */
	std::vector<uint32_t> ids;
	for (const StockOrder &p : _stock_market.positions) {
		if (p.owner == c->index.base() && p.wallet == wallet) ids.push_back(p.id);
	}
	for (uint32_t id : ids) ReviewPosition(c->index, id);
	GuardMargin(c->index, wallet);

	/* 2 + 3: loan and wallet. */
	PlaceSpareMoney(c, wallet);

	/* 4a2: real gold in the vault. */
	TradeVaultGold(c, wallet);

	/* 4b: at most one new trade per decision, so the market impact of the bot stays small. */
	OpenBestTrade(c->index, wallet);
}

} // namespace

/** Let the AI companies manage their money and trade, every game hour of the clock of the mod. */
static const GameTimeSubscription _stock_ai_hour(GameTimeEvent::Hour, [](const GameTimeSnapshot &now)
{
	const int64_t hour = static_cast<int64_t>(now.total_hours);
	for (Company *c : Company::Iterate()) {
		if (!STOCK_AI_PLAY_ALL_COMPANIES && !c->is_ai) continue;
		if ((hour + static_cast<int64_t>(c->index.base())) % STOCK_AI_DECISION_INTERVAL_HOURS != 0) continue;
		PlayCompany(c);
	}
});
