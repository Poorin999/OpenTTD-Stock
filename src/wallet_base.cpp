/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file wallet_base.cpp Implementation of the wallets of a company. */

#include "stdafx.h"
#include "wallet_base.h"
#include "company_base.h"
#include "company_func.h"
#include "economy_func.h"
#include "strings_func.h"
#include "window_func.h"
#include "core/backup_type.hpp"

#include "table/strings.h"

#include "safeguards.h"

WalletRegistry _wallets;

/** Forget all wallets, e.g. when a new game is started or a savegame is loaded. */
void WalletRegistry::Reset()
{
	this->wallets.clear();
}

/** The stored wallet with this number, or nullptr. The company cash (#WALLET_CASH) is not stored. */
Wallet *WalletRegistry::Find(CompanyID owner, uint8_t id)
{
	if (id == WALLET_CASH) return nullptr;
	for (Wallet &w : this->wallets) {
		if (w.owner == owner.base() && w.id == id) return &w;
	}
	return nullptr;
}

const Wallet *WalletRegistry::Find(CompanyID owner, uint8_t id) const
{
	return const_cast<WalletRegistry *>(this)->Find(owner, id);
}

/** The wallet of a kind with the lowest number, or nullptr. */
Wallet *WalletRegistry::FindFirst(CompanyID owner, WalletKind kind)
{
	for (Wallet &w : this->wallets) {
		if (w.owner == owner.base() && w.kind == kind) return &w;
	}
	return nullptr;
}

/** Number of stored wallets of a kind that a company has. */
size_t WalletRegistry::Count(CompanyID owner, WalletKind kind) const
{
	size_t n = 0;
	for (const Wallet &w : this->wallets) n += (w.owner == owner.base() && w.kind == kind);
	return n;
}

/** Whether a company has any stored wallet. */
bool WalletRegistry::HasOwner(CompanyID owner) const
{
	for (const Wallet &w : this->wallets) {
		if (w.owner == owner.base()) return true;
	}
	return false;
}

/**
 * Create a wallet. Only trading wallets and the fuel reserve can be created; the number of each kind is limited.
 * The new wallet gets the lowest number that is free.
 * @param owner Company that gets the wallet.
 * @param kind Kind of the wallet.
 * @return The number of the new wallet, or #WALLET_CASH (0) when it could not be created.
 */
uint8_t WalletRegistry::Create(CompanyID owner, WalletKind kind)
{
	size_t limit = 0;
	switch (kind) {
		case WalletKind::Trading: limit = WALLET_MAX_TRADING; break;
		case WalletKind::Fuel:    limit = WALLET_MAX_FUEL; break;
		default: return WALLET_CASH;
	}
	if (this->Count(owner, kind) >= limit) return WALLET_CASH;

	uint8_t id = 1;
	while (this->Find(owner, id) != nullptr) id++;

	Wallet w;
	w.id = id;
	w.owner = owner.base();
	w.kind = kind;
	this->wallets.push_back(w);
	std::sort(this->wallets.begin(), this->wallets.end(), [](const Wallet &a, const Wallet &b) {
		return a.owner != b.owner ? a.owner < b.owner : a.id < b.id;
	});
	return id;
}

/** Remove a wallet. The money in it is lost, so move it first. */
void WalletRegistry::Remove(CompanyID owner, uint8_t id)
{
	std::erase_if(this->wallets, [&](const Wallet &w) { return w.owner == owner.base() && w.id == id; });
}

/** Remove all wallets of a company, e.g. because the company is gone. */
void WalletRegistry::RemoveOwner(CompanyID owner)
{
	std::erase_if(this->wallets, [&](const Wallet &w) { return w.owner == owner.base(); });
}

/** Every company has at least one trading wallet and the fuel reserve. Create what is missing. */
void WalletRegistry::EnsureDefaults(CompanyID owner)
{
	if (this->Count(owner, WalletKind::Trading) == 0) this->Create(owner, WalletKind::Trading);
	if (this->Count(owner, WalletKind::Fuel) == 0) this->Create(owner, WalletKind::Fuel);
}

/** The numbers of the stored wallets of a company, the lowest first. */
std::vector<uint8_t> WalletRegistry::GetIds(CompanyID owner) const
{
	std::vector<uint8_t> ids;
	for (const Wallet &w : this->wallets) {
		if (w.owner == owner.base()) ids.push_back(w.id);
	}
	return ids;
}

/** Kind of a wallet; WalletKind::End when it does not exist. */
WalletKind GetWalletKind(CompanyID owner, uint8_t id)
{
	if (id == WALLET_CASH) return Company::IsValidID(owner) ? WalletKind::Cash : WalletKind::End;
	const Wallet *w = _wallets.Find(owner, id);
	return w != nullptr ? w->kind : WalletKind::End;
}

/** Whether this is a wallet that positions can be opened with. */
bool IsTradingWallet(CompanyID owner, uint8_t id)
{
	return GetWalletKind(owner, id) == WalletKind::Trading;
}

/** Money in a wallet; for #WALLET_CASH the money of the company. 0 when the wallet does not exist. */
int64_t GetWalletBalance(CompanyID owner, uint8_t id)
{
	if (id == WALLET_CASH) {
		const Company *c = Company::GetIfValid(owner);
		return c != nullptr ? static_cast<int64_t>(c->money) : 0;
	}
	const Wallet *w = _wallets.Find(owner, id);
	return w != nullptr ? w->balance : 0;
}

/** Money in all stored wallets of a company, not counting the cash. */
int64_t GetWalletsTotal(CompanyID owner)
{
	int64_t sum = 0;
	for (const Wallet &w : _wallets.wallets) {
		if (w.owner == owner.base()) sum += w.balance;
	}
	return sum;
}

/**
 * Add a result (a profit, a loss, a fee, a dividend) to a wallet. Money of the cash is booked as other expenses,
 * so it shows in the finances of the company. A stored wallet never goes below zero: the part of a loss that is
 * more than the balance is absorbed (negative balance protection).
 * @param owner Company that owns the wallet.
 * @param id Number of the wallet.
 * @param amount Money to add; negative to take money away.
 */
void AddToWallet(CompanyID owner, uint8_t id, int64_t amount)
{
	if (amount == 0) return;

	if (id == WALLET_CASH) {
		if (!Company::IsValidID(owner)) return;
		AutoRestoreBackup cur_company(_current_company, owner);
		SubtractMoneyFromCompany(CommandCost(EXPENSES_OTHER, Money(-amount)));
		InvalidateWindowData(WC_FINANCES, owner.base());
		return;
	}

	Wallet *w = _wallets.Find(owner, id);
	if (w == nullptr) return;
	w->balance = std::max<int64_t>(0, w->balance + amount);
}

/**
 * Move money from one wallet of a company to another. This is a transfer, not an expense: the total money of the company
 * does not change. Nothing happens when a wallet does not exist or the source has less money than the amount.
 * @param owner Company that owns both wallets.
 * @param from Number of the source wallet.
 * @param to Number of the destination wallet.
 * @param amount Money to move, more than zero.
 * @return True when the money was moved.
 */
bool MoveWalletMoney(CompanyID owner, uint8_t from, uint8_t to, int64_t amount)
{
	Company *c = Company::GetIfValid(owner);
	if (c == nullptr || amount <= 0 || from == to) return false;
	if (GetWalletKind(owner, from) == WalletKind::End || GetWalletKind(owner, to) == WalletKind::End) return false;
	if (GetWalletBalance(owner, from) < amount) return false;

	if (from == WALLET_CASH) {
		c->money -= Money(amount);
	} else {
		_wallets.Find(owner, from)->balance -= amount;
	}

	if (to == WALLET_CASH) {
		c->money += Money(amount);
	} else {
		_wallets.Find(owner, to)->balance += amount;
	}

	InvalidateWindowData(WC_FINANCES, owner.base());
	return true;
}

/**
 * Pay something from the first wallet of a kind, e.g. fuel from the fuel reserve. This is the function that a
 * future feature calls to spend the reserve. It is an expense, so the money is gone afterwards.
 * @param owner Company that pays.
 * @param kind Kind of the wallet to pay from.
 * @param amount Money to pay, more than zero.
 * @return True when the wallet had enough money and it was paid; false when nothing was paid.
 */
bool PayFromWallet(CompanyID owner, WalletKind kind, int64_t amount)
{
	if (amount <= 0) return false;
	if (kind == WalletKind::Cash) {
		if (GetWalletBalance(owner, WALLET_CASH) < amount) return false;
		AddToWallet(owner, WALLET_CASH, -amount);
		return true;
	}
	Wallet *w = _wallets.FindFirst(owner, kind);
	if (w == nullptr || w->balance < amount) return false;
	w->balance -= amount;
	return true;
}

/** Name of a wallet, to show in the interface. */
std::string GetWalletName(CompanyID owner, uint8_t id)
{
	switch (GetWalletKind(owner, id)) {
		case WalletKind::Cash:    return GetString(STR_WALLET_NAME_CASH);
		case WalletKind::Trading: return GetString(STR_WALLET_NAME_TRADING, id);
		case WalletKind::Fuel:    return GetString(STR_WALLET_NAME_FUEL);
		default:                  return GetString(STR_WALLET_NAME_UNKNOWN);
	}
}
