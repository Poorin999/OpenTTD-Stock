/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file central_bank.h The central bank: loans that are paid out to, and repaid from, a wallet of a company. */

#ifndef CENTRAL_BANK_H
#define CENTRAL_BANK_H

#include "command_type.h"
#include "company_type.h"
#include "economy_type.h"
#include <array>
#include <map>

/** Interest of the central bank per economy day, in parts per million of the debt (about 10% a year). The interest is added to the debt. */
static constexpr int64_t CENTRAL_BANK_RATE_PPM_PER_DAY = 274;

/** A company can borrow as long as its debt stays below this percentage of its net equity (company value plus wallets minus the debt). */
static constexpr int64_t CENTRAL_BANK_CREDIT_PERCENT = 50;

/** The loan of one company at the central bank. */
struct BankAccount {
	int64_t debt = 0;            ///< Money owed to the central bank, including the interest that has been added.
	int64_t interest_paid = 0;   ///< Total interest that has been added to the debt, for the statistics.
};

/** The central bank: one account per company. Saved in the 'STCK' chunk together with the wallets. */
struct CentralBank {
	std::array<BankAccount, MAX_COMPANIES> accounts{};

	void Reset();
	void UpdateDaily();
};

extern CentralBank _central_bank;

/**
 * The lines of the account statement that the game books itself. The costs of vehicles, tracks and stations are read from
 * the yearly expenses of the company (see the Central Bank window), they are not in the ledger.
 */
enum class LedgerItem : uint8_t {
	BankInterest,    ///< Interest that is added to the debt at the central bank (expense).
	TradeCommission, ///< Commission of a trade, taken from the trading wallet (expense).
	MarginSwap,      ///< Daily swap of the open positions, taken from the trading wallet (expense).
	Dividends,       ///< Dividends of the shares of other companies (income) and the dividend that the company pays itself (expense).
	RealizedProfit,  ///< Profit (income) and loss (expense) of closed trades.
	WalletTransfer,  ///< Money moved between the company cash and another wallet: into the cash (income) and out of the cash (expense).
	BankLoan,        ///< Money that the central bank or the original loan paid out (income) and what was repaid (expense).
	End,
};
static constexpr size_t LEDGER_ITEMS = static_cast<size_t>(LedgerItem::End);

/** What one line of the ledger booked in one economy year. */
struct LedgerEntry {
	int64_t income = 0;  ///< Money that came in.
	int64_t expense = 0; ///< Money that went out.
};

/** The ledgers of the companies: per company and economy year the entry of every line. Only this year and the two before it are kept. Saved in the 'STCK' chunk. */
struct Ledger {
	std::array<std::map<int32_t, std::array<LedgerEntry, LEDGER_ITEMS>>, MAX_COMPANIES> years;

	void Reset() { for (auto &y : this->years) y.clear(); }
};

extern Ledger _ledger;

void RecordLedger(CompanyID company, LedgerItem item, int64_t amount);
LedgerEntry GetLedgerEntry(CompanyID company, LedgerItem item, int32_t year);
int32_t GetLedgerCurrentYear();

int64_t GetCentralBankDebt(CompanyID company);
int64_t GetCentralBankCreditLimit(CompanyID company);
int64_t GetCentralBankAvailable(CompanyID company);
int64_t GetUnborrowedLiquidCash(CompanyID company);
CommandCost CheckTradingCapital(CompanyID company, int64_t amount);

CommandCost CmdBankBorrow(DoCommandFlags flags, uint8_t wallet, Money amount);
CommandCost CmdBankRepay(DoCommandFlags flags, uint8_t wallet, Money amount);
CommandCost CmdBankRepayLegacy(DoCommandFlags flags, Money amount);

DEF_CMD_TRAIT(CMD_BANK_BORROW, CmdBankBorrow, {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_BANK_REPAY,  CmdBankRepay,  {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_BANK_REPAY_LEGACY, CmdBankRepayLegacy, {}, CommandType::MoneyManagement)

#endif /* CENTRAL_BANK_H */
