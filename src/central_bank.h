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

int64_t GetCentralBankDebt(CompanyID company);
int64_t GetCentralBankCreditLimit(CompanyID company);
int64_t GetCentralBankAvailable(CompanyID company);

CommandCost CmdBankBorrow(DoCommandFlags flags, uint8_t wallet, Money amount);
CommandCost CmdBankRepay(DoCommandFlags flags, uint8_t wallet, Money amount);

DEF_CMD_TRAIT(CMD_BANK_BORROW, CmdBankBorrow, {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_BANK_REPAY,  CmdBankRepay,  {}, CommandType::MoneyManagement)

#endif /* CENTRAL_BANK_H */
