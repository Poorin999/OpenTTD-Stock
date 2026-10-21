/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file wallet_cmd.cpp Commands to create and delete wallets and to move money between them. */

#include "stdafx.h"
#include "wallet_cmd.h"
#include "wallet_base.h"
#include "stock_base.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "window_func.h"

#include "table/strings.h"

#include "safeguards.h"

/** Tell the windows that show wallets that something changed. */
static void InvalidateWalletWindows()
{
	InvalidateWindowData(WC_STOCK_MARKET, 0);
	InvalidateWindowData(WC_STOCK_ORDER, 0);
	InvalidateWindowClassesData(WC_FINANCES);
}

/**
 * Create a new trading wallet. The fuel reserve is created by the game, one for every company, so only trading
 * wallets can be created here.
 * @param flags Type of operation.
 * @param kind Kind of the wallet; must be WalletKind::Trading.
 * @return Zero cost, or an error.
 */
CommandCost CmdWalletCreate(DoCommandFlags flags, WalletKind kind)
{
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;
	if (kind != WalletKind::Trading) return CommandCost(STR_WALLET_ERROR_BAD_KIND);
	if (_wallets.Count(_current_company, kind) >= WALLET_MAX_TRADING) return CommandCost(STR_WALLET_ERROR_LIMIT);

	if (flags.Test(DoCommandFlag::Execute)) {
		_wallets.Create(_current_company, kind);
		InvalidateWalletWindows();
	}
	return CommandCost();
}

/**
 * Delete a trading wallet. The money in it goes to the company cash. A wallet with open positions or pending orders
 * can not be deleted, and neither can the last trading wallet or the fuel reserve.
 * @param flags Type of operation.
 * @param wallet Number of the wallet.
 * @return Zero cost, or an error.
 */
CommandCost CmdWalletDelete(DoCommandFlags flags, uint8_t wallet)
{
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;
	const WalletKind kind = GetWalletKind(_current_company, wallet);
	if (kind == WalletKind::End || kind == WalletKind::Cash) return CommandCost(STR_WALLET_ERROR_BAD_WALLET);
	if (kind != WalletKind::Trading) return CommandCost(STR_WALLET_ERROR_BAD_KIND);
	if (_wallets.Count(_current_company, WalletKind::Trading) <= 1) return CommandCost(STR_WALLET_ERROR_LAST);
	if (_stock_market.CountWalletOrders(_current_company, wallet) > 0) return CommandCost(STR_WALLET_ERROR_IN_USE);

	if (flags.Test(DoCommandFlag::Execute)) {
		const int64_t balance = GetWalletBalance(_current_company, wallet);
		if (balance > 0) MoveWalletMoney(_current_company, wallet, WALLET_CASH, balance);
		_wallets.Remove(_current_company, wallet);
		InvalidateWalletWindows();
	}
	return CommandCost();
}

/**
 * Move money between two wallets of the company, one of which can be the company cash. Neither is an expense,
 * so the cost of the command is zero. Money can only leave a trading wallet when the free margin covers it,
 * so the open positions of that wallet stay covered.
 * @param flags Type of operation.
 * @param from Number of the wallet that the money is taken from; #WALLET_CASH for the company cash.
 * @param to Number of the wallet that gets the money.
 * @param amount Money to move, more than zero.
 * @return Zero cost, or an error.
 */
CommandCost CmdWalletTransfer(DoCommandFlags flags, uint8_t from, uint8_t to, Money amount_in)
{
	const int64_t amount = amount_in;
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;
	if (amount <= 0) return CommandCost(STR_STOCK_ERROR_BAD_AMOUNT);
	if (from == to) return CommandCost(STR_WALLET_ERROR_BAD_WALLET);
	if (GetWalletKind(_current_company, from) == WalletKind::End || GetWalletKind(_current_company, to) == WalletKind::End) {
		return CommandCost(STR_WALLET_ERROR_BAD_WALLET);
	}

	if (GetWalletBalance(_current_company, from) < amount) return CommandCost(STR_STOCK_ERROR_NO_FUNDS);
	if (IsTradingWallet(_current_company, from) && GetStockFreeMargin(_current_company, from) < amount) {
		return CommandCost(STR_STOCK_ERROR_NO_MARGIN);
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		MoveWalletMoney(_current_company, from, to, amount);
		InvalidateWalletWindows();
	}
	return CommandCost();
}
