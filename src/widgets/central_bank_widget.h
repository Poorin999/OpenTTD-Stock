/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file central_bank_widget.h Types related to the central bank window widgets. */

#ifndef WIDGETS_CENTRAL_BANK_WIDGET_H
#define WIDGETS_CENTRAL_BANK_WIDGET_H

/** Widgets of the #CentralBankWindow class (window number 0) and of the #CentralBankStatementWindow class (window number 1). */
enum CentralBankWidgets : WidgetID {
	WID_CB_CAPTION,         ///< Caption of the bank window.
	WID_CB_SUMMARY,         ///< Left panel: cash, deposits, debt, credit limit, interest rate, credit rating.
	WID_CB_LOG,             ///< Right panel: the account log with the latest bookings.
	WID_CB_BORROW,          ///< Button: borrow from the central bank into the company cash.
	WID_CB_REPAY,           ///< Button: repay a part of the debt from the company cash.
	WID_CB_REPAY_ALL,       ///< Button: repay as much of the debt as the company cash allows.
	WID_CB_OPEN_STATEMENT,  ///< Button: open the account statement in its own window.
	WID_CBS_CAPTION,        ///< Caption of the account statement window.
	WID_CBS_PANEL,          ///< Panel with the account statement.
	WID_CBS_NEWER,          ///< Button: show the statement of a newer period.
	WID_CBS_OLDER,          ///< Button: show the statement of an older period (past statements).
};

#endif /* WIDGETS_CENTRAL_BANK_WIDGET_H */
