/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file company_info_gui.h The company window of the mod. ShowCompany() of OpenTTD opens it instead of the original company window (apply_overlay.py, patch_company_info). */

#ifndef COMPANY_INFO_GUI_H
#define COMPANY_INFO_GUI_H

#include <array>
#include <cstdint>
#include <map>
#include "company_type.h"

/** While this is true, ShowCompany() opens the ORIGINAL company window (face, headquarters, shares). Only ShowOriginalCompanyWindow() sets it. */
extern bool _company_info_use_original;

/** Open (or bring to the front) the company window of the mod. */
void ShowCompanyInfoWindow(CompanyID company);

/** Open the original OpenTTD company window, for the things that only it can do (new face, build or move the headquarters). */
void ShowOriginalCompanyWindow(CompanyID company);

/**
 * Open the infrastructure window of OpenTTD. ShowCompanyInfrastructure() is `static` in company_gui.cpp of OpenTTD 15.3, so it can not be called from
 * here; apply_overlay.py (patch_company_info) adds this function to company_gui.cpp, where it calls it.
 */
void ShowCompanyInfrastructureOfMod(CompanyID company);
/** Open the face window of OpenTTD for this company (wrapper in company_gui.cpp, added by patch_company_info). */
void ShowCompanyFaceOfMod(CompanyID company);

/** What one company earned and spent in one game month (the chart of the company window). Saved as SRK_COMPANY_MONTH (saveload/stock_sl.cpp). */
struct CompanyMonthBucket {
	int64_t income = 0;  ///< Income of the month.
	int64_t expense = 0; ///< Costs of the month (positive).
};

constexpr size_t COMPANY_MONTHS_KEPT = 12; ///< How many months are kept per company.

/** The buckets, per company, key = companyinfo::MonthKey (year and month). Ordered, so the oldest month is the first. */
extern std::array<std::map<int32_t, CompanyMonthBucket>, MAX_COMPANIES> _company_month_buckets;

/** Forget the buckets and the running totals (a new game or the start of a load, called by StockMarket::Reset). */
void ResetCompanyMonthBuckets();

#endif /* COMPANY_INFO_GUI_H */
