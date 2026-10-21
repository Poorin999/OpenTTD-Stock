/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file wallet_cmd.h Command definitions related to the wallets of a company. */

#ifndef WALLET_CMD_H
#define WALLET_CMD_H

#include "command_type.h"
#include "company_type.h"
#include "economy_type.h"
#include "wallet_base.h"
/* The commands of the central bank are made known to the command system through this header. */
#include "central_bank.h"

CommandCost CmdWalletCreate(DoCommandFlags flags, WalletKind kind);
CommandCost CmdWalletDelete(DoCommandFlags flags, uint8_t wallet);
CommandCost CmdWalletTransfer(DoCommandFlags flags, uint8_t from, uint8_t to, Money amount);

DEF_CMD_TRAIT(CMD_WALLET_CREATE,   CmdWalletCreate,   {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_WALLET_DELETE,   CmdWalletDelete,   {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_WALLET_TRANSFER, CmdWalletTransfer, {}, CommandType::MoneyManagement)

#endif /* WALLET_CMD_H */

