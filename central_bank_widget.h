/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file central_bank_widget.h Types related to the central bank window widgets. */

#ifndef WIDGETS_CENTRAL_BANK_WIDGET_H
#define WIDGETS_CENTRAL_BANK_WIDGET_H

/** Widgets of the #CentralBankWindow class (window number 0) and of the #CentralBankStatementWindow class (window number 1) and of the #CentralBankArchiveWindow class (window number 2) and of the buy gold window (window number 3) and of the monetary policy window (window number 4) and of the news window (window number 5). */
enum CentralBankWidgets : WidgetID {
	WID_CB_CAPTION,         ///< Caption of the bank window.
	WID_CB_SUMMARY,         ///< Left panel: cash, deposits, debt, credit limit, interest rate, credit rating.
	WID_CB_LOG,             ///< Right panel: the account log with the latest bookings.
	WID_CB_BORROW,          ///< Button: borrow from the central bank into the company cash.
	WID_CB_REPAY,           ///< Button: repay a part of the debt from the company cash.
	WID_CB_REPAY_ALL,       ///< Button: repay as much of the debt as the company cash allows.
	WID_CB_OPEN_STATEMENT,  ///< Button: open the account statement in its own window.
	WID_CB_OPEN_POLICY,     ///< Button: open the monetary policy (price index, inflation, policy rate) in its own window.
	WID_CB_SKIP_SEL,        ///< Selection: the test buttons (after the console command) or nothing.
	WID_CB_SKIP_1H,         ///< Test button: move the game clock forward by 1 hour (single player only).
	WID_CB_SKIP_1D,         ///< Test button: move the game clock forward by 1 day (single player only).
	WID_CB_SKIP_7D,         ///< Test button: move the game clock forward by 7 days (single player only).
	WID_CB_SKIP_30D,        ///< Test button: move the game clock forward by 30 days (single player only).
	WID_CB_SKIP_90D,        ///< Test button: move the game clock forward by 90 days, one quarter (single player only).
	WID_CBS_CAPTION,        ///< Caption of the account statement window.
	WID_CBS_PANEL,          ///< Panel with the account statement.
	WID_CBS_NEWER,          ///< Button: show the statement of a newer period.
	WID_CBS_OLDER,          ///< Button: open the past statements window (10 years of statements).
	WID_CBA_CAPTION,        ///< Caption of the past statements window.
	WID_CBA_YEARS,          ///< Left list: the last 10 years, one row each.
	WID_CBA_PANEL,          ///< Right panel: the statement of the chosen year, in its sections.
	WID_GB_CAPTION,         ///< Caption of the buy gold window.
	WID_GB_PANEL,           ///< Panel of the buy gold window: price, quantity, cost and buttons are drawn and clicked here.
	WID_CBP_CAPTION,        ///< Caption of the monetary policy window.
	WID_CBP_PANEL,          ///< Panel of the monetary policy window: price index, inflation, policy shift and the loan rates.
	WID_PN_CAPTION,         ///< Caption of the news window (Transport Payrolls).
	WID_PN_PANEL,           ///< Panel of the news window: the reports of the Transport Payrolls and their effect on gold.
	WID_MN_CAPTION,         ///< Caption of the market news window.
	WID_MN_PANEL,           ///< Panel of the market news window: the lead story and the list of the latest events.
};

#endif /* WIDGETS_CENTRAL_BANK_WIDGET_H */
