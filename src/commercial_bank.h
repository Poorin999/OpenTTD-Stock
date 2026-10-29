/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/**
 * @file commercial_bank.h The commercial banks: one in every town. They take deposits from the citizens, lend to the town treasury and to companies, keep reserves at the
 * central bank (which is their lender of last resort) and can fail. They use the model of the central bank (rating C..AAA, risk rate curve, default and penalty, write-off).
 * See commercial_bank_math.h for the rules. Saved as SRK_CBANK / SRK_CBANK_LOAN (saveload/stock_sl.cpp), loan of a town in SRK_TOWN_WALLET b = 4.
 */

#ifndef COMMERCIAL_BANK_H
#define COMMERCIAL_BANK_H

#include <cstdint>
#include <map>
#include <optional>
#include "command_type.h"
#include "company_type.h"
#include "economy_type.h"
#include "central_bank.h"
#include "commercial_bank_math.h"

/** One commercial bank (the key in the manager is the index of its town). */
struct CommercialBank {
	cbank::Sheet sheet;          ///< Reserves, deposits, central bank loan, totals. Saved.
	bool has_building = false;   ///< A bank building of the game (the bank industry) stands in the town. Found again every game day, not saved.
};

static_assert(MAX_COMPANIES <= 16, "the key of a loan packs the company into 4 bits");

/** The key of the loan of a company at the bank of a town. */
constexpr uint32_t CBankLoanKey(uint32_t town, uint32_t company) { return (town << 4) | company; }

/** All the commercial banks and their loans to companies. */
struct CommercialBankManager {
	std::map<uint32_t, CommercialBank> banks;   ///< Key = town index. A map, so every client walks the banks in the same order.
	std::map<uint32_t, BankAccount> loans;      ///< Loans to companies, key = CBankLoanKey(town, company). Same account struct and rules as the central bank.

	std::map<uint32_t, int64_t> next_build_try; ///< Game day before which no new bank building is tried in a town (it had no site). Not saved.

	void Reset() { this->banks.clear(); this->loans.clear(); this->next_build_try.clear(); }
	void UpdateDaily(int64_t day);
};

extern CommercialBankManager _commercial_banks;

/** Money that a company owes to all the commercial banks together. */
int64_t GetCommercialDebt(CompanyID company);
/** Money that a company owes to the commercial bank of one town. */
int64_t GetCommercialDebtAt(uint32_t town, CompanyID company);
/** How much more a company may borrow in total (central bank + commercial banks): the credit pool (central limit + 25%) minus every debt. */
int64_t GetCreditPoolRoom(CompanyID company);

/** What a bank shows: 0 when the town has no bank yet. */
int64_t CBankLoansTotal(uint32_t town);                 ///< Everything it has lent (town + companies).
int64_t CBankEquity(uint32_t town);                     ///< Assets minus liabilities.
int CBankRating(uint32_t town);                         ///< 0 = C ... 4 = AAA.
int64_t CBankLoanRateBp(uint32_t town, CompanyID company); ///< The rate that the company would pay now.
int64_t CBankAvailable(uint32_t town, CompanyID company);  ///< How much the company could borrow there now.

/** The commercial bank that a company uses: the one whose town is nearest to its headquarters, inside the money zone of the headquarters. Nothing without a headquarters. */
std::optional<uint32_t> CBankNearestToHQ(CompanyID company);
/** The loan account of a company at the bank of a town (an empty account when there is no loan). */
BankAccount CBankAccountOf(uint32_t town, CompanyID company);

struct TownAccount;
/**
 * The town treasury is short: the commercial bank of the town lends what it can (within its capacity and the limit of the town).
 * @return The money that was lent (already added to the treasury and to the debt of the town).
 */
int64_t CBankLendToTown(uint32_t town, TownAccount &account, int64_t scaled_population, int64_t want);

CommandCost CmdCBankBorrow(DoCommandFlags flags, uint16_t town, Money amount);
CommandCost CmdCBankRepay(DoCommandFlags flags, uint16_t town, Money amount);

DEF_CMD_TRAIT(CMD_CBANK_BORROW, CmdCBankBorrow, {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_CBANK_REPAY,  CmdCBankRepay,  {}, CommandType::MoneyManagement)

#endif /* COMMERCIAL_BANK_H */
