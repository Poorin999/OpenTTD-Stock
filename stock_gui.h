/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file stock_gui.h Functions related to the trading terminal windows. */

#ifndef STOCK_GUI_H
#define STOCK_GUI_H

#include "company_type.h"
#include "stock_base.h"
#include "strings_type.h"
#include <string>
#include <string_view>

void ShowStockWindow();
void ShowStockOrderWindow(CompanyID symbol);
void SetStockOrderSymbol(CompanyID symbol);

std::string StockVolumeToString(uint32_t volume);
bool ParseStockNumber(std::string_view text, double &value);
int64_t StockDisplayToMoney(double value);
std::string StockMoneyToEditString(int64_t money);
StringID GetStockOrderTypeString(StockOrderType type);
uint8_t GetStockActiveWallet();
void SetStockActiveWallet(uint8_t wallet);

#endif /* STOCK_GUI_H */
