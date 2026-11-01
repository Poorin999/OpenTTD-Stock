/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file contract.h Contracts between a buyer and an industry (step 3 of the contract system). The rules and the table are in contract_math.h.
 * The SELLER is the owner of the industry (step 1). A company answers with the commands below; for an industry of the SYSTEM (NPC) the automatic answer is ContractAutoAnswer (step 4).
 * The table is saved as SRK_CONTRACT. */

#ifndef CONTRACT_H
#define CONTRACT_H

#include <cstdint>
#include "command_type.h"
#include "company_type.h"
#include "economy_type.h"
#include "industry_type.h"
#include "cargo_type.h"
#include "contract_math.h"

struct Industry;

/** All contracts. Saved as SRK_CONTRACT; reset by StockMarket::Reset (an old savegame has none). */
extern contract::Table _contracts;

/** The error of the rules as a command error (its message is a string of english_contract_strings.txt). Ok gives a success. The contracts window uses it to say why a button is grey. */
CommandCost ContractErrorOf(contract::Result r);

/** The buyer (the company that gives the command) offers: `qty` units a month of `cargo` at `price_cents` (1/100 of a money unit) per unit, for `months` months. Round 1. */
CommandCost CmdContractOffer(DoCommandFlags flags, IndustryID industry, uint8_t cargo, uint32_t qty, uint32_t price_cents, uint8_t months);
/** The side whose turn it is answers with new terms (round 2 or 3). */
CommandCost CmdContractCounter(DoCommandFlags flags, uint32_t contract_id, uint32_t qty, uint32_t price_cents, uint8_t months);
/** The side whose turn it is accepts. The BUYER pays the escrow from unborrowed cash, whoever accepts. */
CommandCost CmdContractAccept(DoCommandFlags flags, uint32_t contract_id);
/** The side whose turn it is refuses. */
CommandCost CmdContractRefuse(DoCommandFlags flags, uint32_t contract_id);
/** Withdraw an open offer (either side), or the buyer ends an active contract (a fine unless 2 months in a row were short). */
CommandCost CmdContractCancel(DoCommandFlags flags, uint32_t contract_id);

DEF_CMD_TRAIT(CMD_CONTRACT_OFFER,   CmdContractOffer,   {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_CONTRACT_COUNTER, CmdContractCounter, {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_CONTRACT_ACCEPT,  CmdContractAccept,  {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_CONTRACT_REFUSE,  CmdContractRefuse,  {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_CONTRACT_CANCEL,  CmdContractCancel,  {}, CommandType::MoneyManagement)

/** Output of a month of this cargo at the industry: the larger of this month so far and last month (0 when the industry does not produce it). */
int64_t ContractMonthlyOutput(const Industry *i, CargoType cargo);

/** The identity of an industry (tile | type << 32, construction date): the game re-uses industry numbers, so a contract keeps this. */
bool ContractIndustryIdent(const Industry *i, int64_t &ident, int32_t &built);

/** The seller of a contract NOW: for an open contract the present owner of the industry (it may have been bought since the offer), for an active one the seller that is stored. */
void ContractCurrentSeller(const contract::Contract &c, uint8_t &seller, int32_t &seller_year);

/** Give a company money (a positive amount) or take money from it (a negative amount), for the escrow. Booked on the ledger line "Contract escrow". Nothing happens for a company that is gone. */
void ContractPay(uint8_t company, int64_t amount);

/** The same for a fine of a shortfall (step 6): booked on the ledger line "Contract fines". */
void ContractPayFine(uint8_t company, int64_t amount);

/** Close the contracts of companies that are gone and of industries that are gone (runs every game hour). Returns how many contracts changed. */
int ContractSweep();

struct CargoPacket;

/**
 * A final delivery of cargo (called at the start of CargoPayment::PayFinalDelivery, patched by apply_overlay.py). When the cargo came from an industry and the company that
 * is paid has an ACTIVE contract with that industry for this cargo, the units are counted as delivered (step 5) and remembered for ContractAdjustProfit.
 */
void ContractOnDelivery(CompanyID payer, const CargoPacket *cp, CargoType cargo, uint count);

/**
 * The pay of that final delivery (called right after the game worked out `normal`, patched by apply_overlay.py; step 5 of the spec). The units counted by ContractOnDelivery
 * pay the CONTRACT price; the other units pay 70% of the normal pay when the industry has an owner (a company other than the payer), else the normal pay.
 * The yearly income of the company is corrected by the difference. Returns the pay to use.
 */
Money ContractAdjustProfit(CompanyID payer, const CargoPacket *cp, CargoType cargo, uint count, Money normal);

/** What happened to a contract (for the news). */
enum class ContractEvent : uint8_t { Countered, Accepted, Refused, Cancelled, Expired, Offered };

/**
 * Tell the company that plays on this computer (_local_company) what happened to a contract of its own, as a news item.
 * @param by_buyer Who did it: true = the buyer, false = the seller (the owner or the SYSTEM). The news goes to the OTHER side; Expired goes to both.
 * Offered, Accepted and Refused are also published as market events (kinds ContractOffered, ContractSigned, ContractRefused, spec step 4): that is game state (the saved
 * news log), made the same way on every client, and it is the only news of those three. The other events are news of the local company only.
 */
void ContractNews(const contract::Contract &c, ContractEvent ev, bool by_buyer);

/** The SYSTEM (NPC) answers the open contracts that wait for it, after its delay of 1 .. 3 days (runs every game day before the expiry). Returns how many it answered. */
int ContractAutoAnswer();

/** Step 6: the monthly settlement. Runs inside ContractDaily when the game month changes. Returns how many contracts were settled. */
int ContractMonthly(int64_t today);

/** Close the open offers that nobody answered in 7 game days, settle the month when it ended, warn about low output and forget old closed contracts (runs every game day). Returns how many offers expired. */
int ContractDaily();

#endif /* CONTRACT_H */
