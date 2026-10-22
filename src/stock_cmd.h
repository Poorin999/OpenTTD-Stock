/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file stock_cmd.h Command definitions related to the trading terminal. */

#ifndef STOCK_CMD_H
#define STOCK_CMD_H

#include "command_type.h"
#include "company_type.h"
#include "economy_type.h"
#include "stock_base.h"
/* The commands of the wallets are made known to the command system through this header. */
#include "wallet_cmd.h"

CommandCost CmdStockOpen(DoCommandFlags flags, CompanyID target, StockOrderType type, uint32_t volume, Money price, Money sl, Money tp, uint32_t expiry_days, uint8_t wallet);
CommandCost CmdStockClose(DoCommandFlags flags, uint32_t id, uint32_t close_volume);
CommandCost CmdStockModify(DoCommandFlags flags, uint32_t id, Money sl, Money tp, Money price, Money trail);
CommandCost CmdStockTransfer(DoCommandFlags flags, bool deposit, Money amount);
CommandCost CmdStockEnableDaily(DoCommandFlags flags, bool enable);

DEF_CMD_TRAIT(CMD_STOCK_OPEN,   CmdStockOpen,   {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_STOCK_CLOSE,  CmdStockClose,  {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_STOCK_MODIFY, CmdStockModify, {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_STOCK_TRANSFER, CmdStockTransfer, {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_STOCK_ENABLE_DAILY, CmdStockEnableDaily, {}, CommandType::MoneyManagement)

#endif /* STOCK_CMD_H */

