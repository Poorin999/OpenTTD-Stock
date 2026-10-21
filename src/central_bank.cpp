/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file central_bank.cpp Implementation of the central bank loans. */

#include "stdafx.h"
#include "central_bank.h"
#include "wallet_base.h"
#include "stock_base.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "window_func.h"
#include "timer/timer.h"
#include "timer/timer_game_economy.h"

#include "table/strings.h"

#include "safeguards.h"

CentralBank _central_bank;

/** Forget all loans, e.g. when a new game is started or a savegame is loaded. */
void CentralBank::Reset()
{
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

/** Tell the windows that show the loan that something changed. */
static void InvalidateBankWindows()
{
	InvalidateWindowData(WC_STOCK_MARKET, 0);
	InvalidateWindowData(WC_STOCK_ORDER, 0);
	InvalidateWindowClassesData(WC_FINANCES);
}

/**
 * Borrow money from the central bank. The money is paid out to the company cash or to one of its trading wallets.
 * @param flags Type of operation.
 * @param wallet Wallet that gets the money: #WALLET_CASH or a trading wallet.
 * @param amount Money to borrow, more than zero and at most the available credit.
 * @return Zero cost, or an error.
 */
CommandCost CmdBankBorrow(DoCommandFlags flags, uint8_t wallet, Money amount_in)
{
	const int64_t amount = amount_in;
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;
	if (amount <= 0) return CommandCost(STR_STOCK_ERROR_BAD_AMOUNT);
	const WalletKind kind = GetWalletKind(_current_company, wallet);
	if (kind != WalletKind::Cash && kind != WalletKind::Trading) return CommandCost(STR_WALLET_ERROR_BAD_WALLET);
	if (amount > GetCentralBankAvailable(_current_company)) return CommandCost(STR_BANK_ERROR_LIMIT);

	if (flags.Test(DoCommandFlag::Execute)) {
		_central_bank.accounts[_current_company.base()].debt += amount;
		AddToWallet(_current_company, wallet, amount);
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
		AddToWallet(_current_company, wallet, -amount);
		_central_bank.accounts[_current_company.base()].debt -= amount;
		InvalidateBankWindows();
	}
	return CommandCost();
}

/**
 * Once per economy day the interest is added to every debt. At least one money unit is added to a debt that exists,
 * so a small loan does not stay free of interest because of rounding. A company that is gone has no debt any more.
 */
void CentralBank::UpdateDaily()
{
	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		BankAccount &a = this->accounts[i];
		if (!Company::IsValidID(CompanyID{i})) {
			a = {};
			continue;
		}
		if (a.debt <= 0) continue;
		const int64_t interest = std::max<int64_t>(1, a.debt * CENTRAL_BANK_RATE_PPM_PER_DAY / STOCK_PPM);
		a.debt += interest;
		a.interest_paid += interest;
	}
	InvalidateWindowData(WC_STOCK_MARKET, 0);
}

/** Add the interest of the central bank once per economy day. */
static const IntervalTimer<TimerGameEconomy> _bank_daily({TimerGameEconomy::DAY, TimerGameEconomy::Priority::COMPANY}, [](auto)
{
	_central_bank.UpdateDaily();
});
