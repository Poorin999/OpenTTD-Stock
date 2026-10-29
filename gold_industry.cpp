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
#include "settings_type.h"
#include "landscape_type.h"

#include "table/strings.h"

#include <algorithm>
#include <iterator>
#include <variant>

#include "safeguards.h"

/*
 * OpenTTD has the gold mine in Sub-arctic only. ApplyGoldIndustries() runs at the end of ResetIndustries() (industry_cmd.cpp, hooked by
 * patch_gold_industries in apply_overlay.py), after OpenTTD has decided which industries are enabled for the climate.
 *
 * What it does, in Temperate and Sub-tropical (in Sub-arctic the gold mine is left as it is):
 *  1. the gold mine is enabled and appears on a new map and in the game like in Sub-arctic;
 *  2. it produces the cargo Gold (GOLD) in every climate. OpenTTD has that cargo in Sub-arctic only; patch_gold_cargo (cargotype.cpp) adds it to the other
 *     climates (it is carried by armoured trucks, like Valuables and Diamonds). If that patch is not there (anchor missing) the mine falls back to what
 *     it did before: Valuables (VALU) in Temperate, Diamonds (DIAM) in Sub-tropical;
 *  3. it looks like a local mine (Iron ore mine in Temperate, Copper ore mine in Sub-tropical): the graphics of the Sub-arctic gold mine are not
 *     loaded in the other climates;
 *  4. the bank (in every climate) accepts that cargo (found by what it accepts);
 *  5. a bank exists at the start of every new map in Temperate too (OpenTTD builds the Temperate bank only in the game, in a town of 1200 people or more,
 *     so a new Temperate map had a gold mine and no bank to deliver to).
 * Not covered: Toyland (neither the cargo nor the mine is added there). It has no gold-like cargo and no bank, and the delivery of ANY cargo with the label GOLD / VALU / DIAM feeds the gold market
 * (GoldFromDelivery), so a Toyland cargo can not stand in for gold without a check of where it is delivered (see ROADMAP).
 * A new map gets at least one gold mine and one bank without any code here: OpenTTD forces one industry of every enabled type with an appear chance
 * above 0 at map creation (`force_at_least_one`, unless the type has IndustryBehaviour::NoBuildMapCreation). Step 1 gives the mine that chance in every
 * climate, step 5 gives the Temperate bank one; the Sub-arctic and Sub-tropical banks already have it.
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

/**
 * Let a bank that OpenTTD builds only in towns of 1200 people or more be built in any town, with the same rule that the Sub-arctic and Sub-tropical bank
 * have (the tile must be a house): the bank can then also be placed when a map is created, when the towns are still small.
 * Reached through a generic lambda with `requires`, so a different name in another version of OpenTTD gives a skipped step and not a compile error.
 */
static void MakeBankTownOnly(IndustrySpec &spec)
{
	[&](auto &s) {
		if constexpr (requires { s.behaviour.Test(IndustryBehaviour::Town1200More); s.behaviour.Reset(IndustryBehaviour::Town1200More); s.behaviour.Set(IndustryBehaviour::OnlyInTown); }) {
			if (s.behaviour.Test(IndustryBehaviour::Town1200More)) {
				s.behaviour.Reset(IndustryBehaviour::Town1200More);
				s.behaviour.Set(IndustryBehaviour::OnlyInTown);
			}
		}
	}(spec);
}

/* Labels, not CargoType numbers: ResetIndustries() runs BEFORE SetupCargoForClimate() (ResetNewGRFData in newgrf.cpp), so at this point the cargo
 * table and the label map still belong to the PREVIOUS climate (or are empty at start-up). A CargoType number stored here points into the wrong table
 * (a slot that is invalid in the new climate -> "<invalid cargo>"). The labels are translated into numbers later, by FinaliseIndustriesArray()
 * in newgrf.cpp, when the cargo table of the new climate is ready. */
static bool AcceptsLabel(const IndustrySpec &spec, CargoLabel label)
{
	for (const auto &v : spec.accepts_cargo_label) {
		if (const CargoLabel *l = std::get_if<CargoLabel>(&v); l != nullptr && *l == label) return true;
	}
	return false;
}

static void AddAcceptedLabel(IndustrySpec &spec, CargoLabel label)
{
	if (AcceptsLabel(spec, label)) return;
	for (auto &v : spec.accepts_cargo_label) {
		if (const CargoLabel *l = std::get_if<CargoLabel>(&v); l != nullptr && *l == CT_INVALID) { v = label; return; }
	}
}

/** Called at the end of ResetIndustries(). Works on cargo LABELS only (see AddAcceptedLabel). */
void ApplyGoldIndustries()
{
	IndustrySpec *gold_mine = nullptr;
	const IndustrySpec *iron_mine = nullptr;
	const IndustrySpec *copper_mine = nullptr;
	for (IndustrySpec &spec : _industry_specs) {
		if (spec.name == STR_INDUSTRY_NAME_GOLD_MINE) gold_mine = &spec;
		if (spec.name == STR_INDUSTRY_NAME_IRON_ORE_MINE) iron_mine = &spec;
		if (spec.name == STR_INDUSTRY_NAME_COPPER_ORE_MINE) copper_mine = &spec;
	}

	/* Temperate and Sub-tropical: the graphics of the Sub-arctic gold mine are not loaded, so the mine takes the look of a local mine. Sub-arctic keeps its own,
	 * and Toyland gets no gold mine (no tile layout of a local mine, no bank). */
	const LandscapeType landscape = _settings_game.game_creation.landscape;
	if (gold_mine != nullptr && (landscape == LandscapeType::Temperate || landscape == LandscapeType::Tropic)) {
		const IndustrySpec *look = landscape == LandscapeType::Temperate ? iron_mine : copper_mine;
		if (look != nullptr) {
			/* The mine produces Gold in every climate (the cargo is added by patch_gold_cargo). Only the label is set; the number is filled in later. */
			gold_mine->produced_cargo[0] = INVALID_CARGO;
			SetOutputLabel(*gold_mine, CargoLabel{'GOLD'});
			CopyLayouts(*gold_mine, *look);
			gold_mine->enabled = true;
			/* Appears like in Sub-arctic: the same chance on a new map and in the game, for every climate. */
			const uint8_t creation = *std::max_element(std::begin(gold_mine->appear_creation), std::end(gold_mine->appear_creation));
			const uint8_t ingame = *std::max_element(std::begin(gold_mine->appear_ingame), std::end(gold_mine->appear_ingame));
			std::fill(std::begin(gold_mine->appear_creation), std::end(gold_mine->appear_creation), creation);
			std::fill(std::begin(gold_mine->appear_ingame), std::end(gold_mine->appear_ingame), ingame);
		}
	}

	/* Banks accept Gold in every climate. A bank is found by what it accepts, not by its name: the original banks accept the mixed cargo
	 * Valuables / Gold / Diamonds (MCT_VALUABLES_GOLD_DIAMONDS), which resolves to the first one that exists (Valuables, then Gold, then Diamonds).
	 * With Gold added to every climate that is Valuables in Temperate (so Gold is added as an extra label) and Gold in Sub-tropical (so Diamonds
	 * are added, the diamonds of the real diamond mine). Sub-arctic already has Gold. */
	for (IndustrySpec &spec : _industry_specs) {
		bool is_bank = false;
		for (const auto &v : spec.accepts_cargo_label) {
			if (const MixedCargoType *m = std::get_if<MixedCargoType>(&v); m != nullptr && *m == MCT_VALUABLES_GOLD_DIAMONDS) is_bank = true;
		}
		if (!is_bank) continue;
		if (landscape == LandscapeType::Temperate) AddAcceptedLabel(spec, CargoLabel{'GOLD'});
		if (landscape == LandscapeType::Tropic) AddAcceptedLabel(spec, CargoLabel{'DIAM'});

		/* A bank that has no chance at map creation but one in the game (the Temperate bank): give it the same chance at creation, so that OpenTTD forces
		 * one at the start of a new map, and let it be built in any town (not only in one of 1200 people or more). */
		if (!spec.enabled) continue;
		const uint8_t creation = *std::max_element(std::begin(spec.appear_creation), std::end(spec.appear_creation));
		const uint8_t ingame = *std::max_element(std::begin(spec.appear_ingame), std::end(spec.appear_ingame));
		if (creation == 0 && ingame > 0) {
			std::fill(std::begin(spec.appear_creation), std::end(spec.appear_creation), ingame);
			MakeBankTownOnly(spec);
		}
	}
}
