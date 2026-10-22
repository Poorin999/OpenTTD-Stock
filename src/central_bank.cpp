/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file central_bank.cpp Implementation of the central bank loans. */

#include "stdafx.h"
#include "game_time/game_time_manager.h"
#include "central_bank.h"
#include "wallet_base.h"
#include "stock_base.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "window_func.h"
#include "timer/timer.h"
#include "timer/timer_game_economy.h"
#include "timer/timer_game_tick.h"
#include "news_func.h"
#include "strings_func.h"
#include "core/format.hpp"

#include "table/strings.h"

#include "safeguards.h"

CentralBank _central_bank;
Ledger _ledger;

/** Forget all loans, e.g. when a new game is started or a savegame is loaded. */
void CentralBank::Reset()
{
	_ledger.Reset(); // The ledger is saved and loaded together with the loans.
	this->accounts.fill({});
}

/** Money that a company owes to the central bank; 0 for a company that does not exist. */
int64_t GetCentralBankDebt(CompanyID company)
{
	return company.base() < MAX_COMPANIES ? _central_bank.accounts[company.base()].debt : 0;
}

/**
 * The most that a company may owe: #CENTRAL_BANK_CREDIT_PERCENT of its net equity. The net equity is the company value
 * (which counts the cash), plus the money in the wallets, minus the debt at the central bank. A loan raises the cash
 * and the debt by the same amount, so borrowing does not make a company able to borrow more.
 * @param company The company.
 * @return The credit limit, 0 or more.
 */
int64_t GetCentralBankCreditLimit(CompanyID company)
{
	const Company *c = Company::GetIfValid(company);
	if (c == nullptr) return 0;
	const int64_t equity = static_cast<int64_t>(CalculateCompanyValue(c)) + GetWalletsTotal(company) - GetCentralBankDebt(company);
	return std::max<int64_t>(0, equity) * CENTRAL_BANK_CREDIT_PERCENT / 100;
}

/** How much more a company may borrow now. */
int64_t GetCentralBankAvailable(CompanyID company)
{
	return std::max<int64_t>(0, GetCentralBankCreditLimit(company) - GetCentralBankDebt(company));
}

/**
 * The money of a company that may go to the trading wallets: its cash minus everything that is borrowed. That is the cash
 * minus the debt at the central bank, because a central bank loan is paid into the cash. As long as the cash is not more
 * than the borrowed money, none of it is the company's own money.
 * @param company The company.
 * @return The unborrowed liquid cash, 0 or more.
 */
int64_t GetUnborrowedLiquidCash(CompanyID company)
{
	const Company *c = Company::GetIfValid(company);
	if (c == nullptr) return 0;
	const int64_t liquid = static_cast<int64_t>(c->money) - GetCentralBankDebt(company);
	return std::max<int64_t>(0, liquid);
}

/**
 * Guard for every command that puts company money into a trading wallet: the amount must come out of the unborrowed
 * liquid cash, so money of the central bank can never be traded.
 * @param company The company that pays.
 * @param amount The money that is to enter a trading wallet.
 * @return An error when the amount is more than #GetUnborrowedLiquidCash, otherwise a zero cost.
 */
CommandCost CheckTradingCapital(CompanyID company, int64_t amount)
{
	if (amount > GetUnborrowedLiquidCash(company)) return CommandCost(STR_BANK_ERROR_BORROWED_FUNDS);
	return CommandCost();
}

/** The game year (365 game days of the HH:MM clock) in which new ledger lines are booked. */
int32_t GetLedgerCurrentYear()
{
	return GameTimeManager::Get().GetCurrentGameYear();
}

/**
 * Book a line of the account statement of a company in the current economy year.
 * @param company The company.
 * @param item The line of the statement.
 * @param amount Money that came in (positive) or went out (negative). Zero is not booked.
 */
void RecordLedger(CompanyID company, LedgerItem item, int64_t amount)
{
	if (amount == 0 || company.base() >= MAX_COMPANIES || item >= LedgerItem::End) return;

	const int32_t year = GetLedgerCurrentYear();
	auto &years = _ledger.years[company.base()];
	LedgerEntry &entry = years[year][static_cast<size_t>(item)];
	if (amount > 0) {
		entry.income += amount;
	} else {
		entry.expense += -amount;
	}

	/* Keep this year and the two before it. */
	years.erase(years.begin(), years.lower_bound(year - 2));
}

/**
 * What a line of the statement booked in an economy year.
 * @param company The company.
 * @param item The line of the statement.
 * @param year The economy year.
 * @return The entry; empty when nothing was booked.
 */
LedgerEntry GetLedgerEntry(CompanyID company, LedgerItem item, int32_t year)
{
	if (company.base() >= MAX_COMPANIES || item >= LedgerItem::End) return {};
	const auto &years = _ledger.years[company.base()];
	const auto it = years.find(year);
	if (it == years.end()) return {};
	return it->second[static_cast<size_t>(item)];
}

/** Tell the windows that show the loan that something changed. */
static void InvalidateBankWindows()
{
	InvalidateWindowData(WC_STOCK_MARKET, 0);
	InvalidateWindowData(WC_STOCK_ORDER, 0);
	InvalidateWindowData(WC_CENTRAL_BANK, 0);
	InvalidateWindowClassesData(WC_FINANCES);
}

/**
 * Borrow money from the central bank. The money is paid out to the company cash only: loan money may never enter a
 * trading wallet, so a trading wallet as destination is refused. The loan is not booked as income, so the cash and the
 * income statistics of the company only show what the company earned.
 * @param flags Type of operation.
 * @param wallet Wallet that gets the money: must be #WALLET_CASH.
 * @param amount Money to borrow, more than zero and at most the available credit.
 * @return Zero cost, or an error.
 */
CommandCost CmdBankBorrow(DoCommandFlags flags, uint8_t wallet, Money amount_in)
{
	const int64_t amount = amount_in;
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;
	if (amount <= 0) return CommandCost(STR_STOCK_ERROR_BAD_AMOUNT);
	const WalletKind kind = GetWalletKind(_current_company, wallet);
	if (kind == WalletKind::Trading) return CommandCost(STR_BANK_ERROR_NO_TRADING_LOAN);
	if (kind != WalletKind::Cash) return CommandCost(STR_WALLET_ERROR_BAD_WALLET);
	if (amount > GetCentralBankAvailable(_current_company)) return CommandCost(STR_BANK_ERROR_LIMIT);

	if (flags.Test(DoCommandFlag::Execute)) {
		_central_bank.accounts[_current_company.base()].debt += amount;
		Company::Get(_current_company)->money += Money(amount);
		RecordLedger(_current_company, LedgerItem::BankLoan, amount);
		InvalidateBankWindows();
	}
	return CommandCost();
}

/**
 * Repay a loan with money from the company cash or from a trading wallet. From a trading wallet only the free margin can
 * be used, so the open positions of that wallet stay covered.
 * @param flags Type of operation.
 * @param wallet Wallet that pays: #WALLET_CASH or a trading wallet.
 * @param amount Money to repay, more than zero and at most the debt.
 * @return Zero cost, or an error.
 */
CommandCost CmdBankRepay(DoCommandFlags flags, uint8_t wallet, Money amount_in)
{
	const int64_t amount = amount_in;
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;
	if (amount <= 0) return CommandCost(STR_STOCK_ERROR_BAD_AMOUNT);
	const WalletKind kind = GetWalletKind(_current_company, wallet);
	if (kind != WalletKind::Cash && kind != WalletKind::Trading) return CommandCost(STR_WALLET_ERROR_BAD_WALLET);
	if (GetCentralBankDebt(_current_company) <= 0) return CommandCost(STR_BANK_ERROR_NO_DEBT);
	if (amount > GetCentralBankDebt(_current_company)) return CommandCost(STR_STOCK_ERROR_BAD_AMOUNT);
	if (GetWalletBalance(_current_company, wallet) < amount) return CommandCost(STR_STOCK_ERROR_NO_FUNDS);
	if (kind == WalletKind::Trading && GetStockFreeMargin(_current_company, wallet) < amount) return CommandCost(STR_STOCK_ERROR_NO_MARGIN);

	if (flags.Test(DoCommandFlag::Execute)) {
		if (kind == WalletKind::Cash) {
			Company::Get(_current_company)->money -= Money(amount); // A repayment is not an expense either.
		} else {
			AddToWallet(_current_company, wallet, -amount);
		}
		_central_bank.accounts[_current_company.base()].debt -= amount;
		RecordLedger(_current_company, LedgerItem::BankLoan, -amount);
		InvalidateBankWindows();
	}
	return CommandCost();
}

/**
 * The original OpenTTD loan does not exist any more: the central bank is the only lender. A loan that a company still has (an
 * old savegame, a script, a cheat) is moved to the central bank at once: the debt grows by the loan, the loan becomes 0, and
 * the cash stays as it is (it already holds that money). From then on it is repaid with the central bank commands, and it
 * costs the interest of the central bank. Only game state is used, so every client of a network game does the same.
 */
static void MoveOriginalLoansToCentralBank()
{
	for (Company *c : Company::Iterate()) {
		if (c->current_loan <= 0) continue;
		const int64_t loan = static_cast<int64_t>(c->current_loan);
		_central_bank.accounts[c->index.base()].debt += loan;
		c->current_loan = 0;
		if (c->index == _local_company) {
			AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, fmt::format("Your original loan of {} was moved to the central bank. It is repaid there, and it costs the interest of the central bank.",
					GetString(STR_JUST_CURRENCY_LONG, loan))), NewsType::General, NewsStyle::Small, {});
		}
		InvalidateBankWindows();
	}
}

/** Check every game tick, so there is never a moment in which the original loan is in use. */
static const IntervalTimer<TimerGameTick> _bank_move_original_loans({TimerGameTick::Priority::NONE, 1}, [](auto)
{
	MoveOriginalLoansToCentralBank();
});

/**
 * Once per game day the interest is added to every debt. At least one money unit is added to a debt that exists,
 * so a small loan does not stay free of interest because of rounding. A company that is gone has no debt any more.
 */
void CentralBank::UpdateDaily()
{
	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		BankAccount &a = this->accounts[i];
		if (!Company::IsValidID(CompanyID{i})) {
			a = {};
			_ledger.years[i].clear();
			continue;
		}
		if (a.debt <= 0) continue;
		const int64_t interest = std::max<int64_t>(1, a.debt * CENTRAL_BANK_RATE_PPM_PER_DAY / STOCK_PPM);
		a.debt += interest;
		a.interest_paid += interest;
		RecordLedger(CompanyID{i}, LedgerItem::BankInterest, -interest);
	}
	InvalidateWindowData(WC_STOCK_MARKET, 0);
}

/** Add the interest of the central bank once per game day of the HH:MM clock (at 00:00, when the date of the game changes). */
static const GameTimeSubscription _bank_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &)
{
	_central_bank.UpdateDaily();
});
