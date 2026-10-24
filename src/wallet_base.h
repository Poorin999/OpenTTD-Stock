/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file wallet_base.h Wallets of a company: the company cash, trading wallets and the fuel reserve. */

#ifndef WALLET_BASE_H
#define WALLET_BASE_H

#include "company_type.h"
#include <iterator>
#include <string>
#include <vector>

/**
 * Number of the company cash. The cash is the money of the company itself (Company::money). It is not stored
 * in the wallet registry, it exists for every company and can not be created or deleted.
 */
static constexpr uint8_t WALLET_CASH = 0;

/** Largest number of trading wallets of one company. */
static constexpr size_t WALLET_MAX_TRADING = 5;

/** Largest number of fuel reserve wallets of one company. */
static constexpr size_t WALLET_MAX_FUEL = 1;

/**
 * Kind of a wallet. A new financial system (central bank, gold reserve, ...) adds its kind here and gets
 * its own header and source file; the registry, the transfer command and the saving stay the same.
 */
enum class WalletKind : uint8_t {
	Cash,     ///< The company cash. Never stored in the registry.
	Trading,  ///< Money for trading shares. Positions are opened with, and settle in, one such wallet.
	Fuel,     ///< Reserve that is kept to pay for fuel.
	End,      ///< End marker.
};

/** The leverage levels of a trading wallet, as the divisor of the margin (the margin of a position is its value divided by this). 0 = unlimited: no margin is held. */
static constexpr int64_t WALLET_LEVERAGE_DIVISORS[] = {1, 10, 100, 1000, 2000, 0};
static constexpr uint8_t WALLET_LEVERAGE_COUNT = static_cast<uint8_t>(std::size(WALLET_LEVERAGE_DIVISORS)); ///< Number of leverage levels.
static constexpr uint8_t WALLET_LEVERAGE_DEFAULT = 1;  ///< Index of the level of a new wallet: 1:10, like before the levels existed.

/** One stored wallet. */
struct Wallet {
	uint8_t id = 0;                 ///< Number of the wallet within its company; 1 or higher.
	uint8_t owner = 0;              ///< Company that owns the wallet.
	WalletKind kind = WalletKind::Trading; ///< What the money may be used for.
	int64_t balance = 0;            ///< Money in the wallet; never negative.
	uint8_t leverage = WALLET_LEVERAGE_DEFAULT; ///< Leverage level: an index in #WALLET_LEVERAGE_DIVISORS. Only trading wallets use it.
};

/** All stored wallets of all companies. Saved together with the trading terminal in the 'STCK' chunk. */
struct WalletRegistry {
	std::vector<Wallet> wallets{}; ///< Sorted by company and number.

	void Reset();
	Wallet *Find(CompanyID owner, uint8_t id);
	const Wallet *Find(CompanyID owner, uint8_t id) const;
	Wallet *FindFirst(CompanyID owner, WalletKind kind);
	size_t Count(CompanyID owner, WalletKind kind) const;
	bool HasOwner(CompanyID owner) const;
	uint8_t Create(CompanyID owner, WalletKind kind);
	void Remove(CompanyID owner, uint8_t id);
	void RemoveOwner(CompanyID owner);
	void EnsureDefaults(CompanyID owner);
	std::vector<uint8_t> GetIds(CompanyID owner) const;
};

extern WalletRegistry _wallets;

WalletKind GetWalletKind(CompanyID owner, uint8_t id);
int64_t GetWalletLeverageDivisor(CompanyID owner, uint8_t id);
bool IsTradingWallet(CompanyID owner, uint8_t id);
int64_t GetWalletBalance(CompanyID owner, uint8_t id);
int64_t GetWalletsTotal(CompanyID owner);
void AddToWallet(CompanyID owner, uint8_t id, int64_t amount);
bool MoveWalletMoney(CompanyID owner, uint8_t from, uint8_t to, int64_t amount);
bool PayFromWallet(CompanyID owner, WalletKind kind, int64_t amount);
std::string GetWalletName(CompanyID owner, uint8_t id);

#endif /* WALLET_BASE_H */
