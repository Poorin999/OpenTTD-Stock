/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gold_industry.cpp Gold mines in every climate, and banks that take the gold cargo of the climate. */

#include "stdafx.h"
#include "industrytype.h"
#include "cargotype.h"

#include "table/strings.h"

#include "safeguards.h"

/*
 * OpenTTD has the gold mine in Sub-arctic only. ApplyGoldIndustries() runs at the end of ResetIndustries() (industry_cmd.cpp, hooked by
 * patch_gold_industries in apply_overlay.py), after OpenTTD has decided which industries are enabled for the climate.
 *
 * What it does, in Temperate and Sub-tropical (in Sub-arctic the gold mine is left as it is):
 *  1. the gold mine is enabled and appears on a new map and in the game like in Sub-arctic;
 *  2. it produces the "gold" cargo of the climate: Valuables (VALU) in Temperate, Diamonds (DIAM) in Sub-tropical (the cargo GOLD does not
 *     exist in those climates and no vehicle of those climates can carry it);
 *  3. it looks like a local mine (Iron ore mine in Temperate, Copper ore mine in Sub-tropical): the graphics of the Sub-arctic gold mine are not
 *     loaded in the other climates;
 *  4. the bank (in every climate) accepts that cargo (found by what it accepts).
 * Every field that is not sure to exist in this version of OpenTTD is reached through a generic lambda with `requires`, so a different
 * name gives a skipped step and not a compile error.
 */

static void CopyLayouts(IndustrySpec &dst, const IndustrySpec &src)
{
	[&](auto &d, const auto &s) {
		if constexpr (requires { d.layouts = s.layouts; }) {
			d.layouts = s.layouts;
		} else if constexpr (requires { d.table = s.table; d.num_table = s.num_table; }) {
			d.table = s.table;
			d.num_table = s.num_table;
		}
	}(dst, src);
}

static void SetOutputLabel(IndustrySpec &spec, CargoLabel label)
{
	[&](auto &s) {
		if constexpr (requires { s.produced_cargo_label[0] = label; }) s.produced_cargo_label[0] = label;
	}(spec);
}

static void AddAcceptedCargo(IndustrySpec &spec, CargoType cargo)
{
	for (CargoType c : spec.accepts_cargo) {
		if (c == cargo) return;
	}
	for (CargoType &c : spec.accepts_cargo) {
		if (c == INVALID_CARGO) { c = cargo; return; }
	}
}

/** Called at the end of ResetIndustries(). */
void ApplyGoldIndustries()
{
	const CargoType valuables = GetCargoTypeByLabel(CargoLabel{'VALU'});
	const CargoType diamonds = GetCargoTypeByLabel(CargoLabel{'DIAM'});
	const CargoType gold = GetCargoTypeByLabel(CargoLabel{'GOLD'});

	IndustrySpec *gold_mine = nullptr;
	const IndustrySpec *iron_mine = nullptr;
	const IndustrySpec *copper_mine = nullptr;
	for (IndustrySpec &spec : _industry_specs) {
		if (spec.name == STR_INDUSTRY_NAME_GOLD_MINE) gold_mine = &spec;
		if (spec.name == STR_INDUSTRY_NAME_IRON_ORE_MINE) iron_mine = &spec;
		if (spec.name == STR_INDUSTRY_NAME_COPPER_ORE_MINE) copper_mine = &spec;
	}

	if (gold_mine != nullptr && gold == INVALID_CARGO) {
		/* Not Sub-arctic: the cargo of the climate that takes the place of gold, and the graphics of a local mine. */
		const IndustrySpec *look = nullptr;
		CargoType out = INVALID_CARGO;
		CargoLabel out_label{0};
		if (valuables != INVALID_CARGO) {
			out = valuables; out_label = CargoLabel{'VALU'}; look = iron_mine;
		} else if (diamonds != INVALID_CARGO) {
			out = diamonds; out_label = CargoLabel{'DIAM'}; look = copper_mine;
		}
		if (out != INVALID_CARGO && look != nullptr) {
			gold_mine->produced_cargo[0] = out;
			SetOutputLabel(*gold_mine, out_label);
			CopyLayouts(*gold_mine, *look);
			gold_mine->enabled = true;
			/* Appears like in Sub-arctic: the same chance on a new map and in the game, for every climate. */
			const uint8_t creation = *std::max_element(std::begin(gold_mine->appear_creation), std::end(gold_mine->appear_creation));
			const uint8_t ingame = *std::max_element(std::begin(gold_mine->appear_ingame), std::end(gold_mine->appear_ingame));
			std::fill(std::begin(gold_mine->appear_creation), std::end(gold_mine->appear_creation), creation);
			std::fill(std::begin(gold_mine->appear_ingame), std::end(gold_mine->appear_ingame), ingame);
		}
	}

	/* Banks take the gold cargo of the climate (Valuables, Gold or Diamonds, whichever exists). A bank is found by what it accepts, not by its
	 * name: an industry that accepts one of these cargos is a bank. In the original economy this already holds in every climate (the Temperate
	 * bank takes Valuables, the Sub-arctic / Sub-tropical bank takes Gold / Diamonds), so this only fills a free input slot when one is missing. */
	for (IndustrySpec &spec : _industry_specs) {
		bool is_bank = false;
		for (const CargoType c : spec.accepts_cargo) {
			if (c != INVALID_CARGO && (c == valuables || c == gold || c == diamonds)) is_bank = true;
		}
		if (!is_bank) continue;
		for (const CargoType c : {valuables, gold, diamonds}) {
			if (c != INVALID_CARGO) AddAcceptedCargo(spec, c);
		}
	}
}
