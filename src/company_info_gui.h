/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file company_info_gui.h The company window of the mod. ShowCompany() of OpenTTD opens it instead of the original company window (apply_overlay.py, patch_company_info). */

#ifndef COMPANY_INFO_GUI_H
#define COMPANY_INFO_GUI_H

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

#endif /* COMPANY_INFO_GUI_H */
