/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file desert_industries.cpp Industries of the Sub-tropical (desert) economy, built into the game. No NewGRF is needed. */

#include "stdafx.h"
#include "industrytype.h"
#include "cargotype.h"
#include "settings_type.h"
#include "landscape_type.h"
#include "debug.h"

#include "table/strings.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <type_traits>
#include <variant>

#include "safeguards.h"

/*
 * ApplyDesertIndustries() runs at the end of ResetIndustries() (industry_cmd.cpp, hooked by patch_desert_industries in apply_overlay.py), after
 * ApplyGoldIndustries(), in the Sub-tropical climate only. It adds 21 industries for the cargo of firs_desert_cargo.cpp.
 *
 * How: every new industry is a COPY of an industry of the base game (the "source"): same buildings, same tile layouts, same behaviour (a plantation
 * plants fields, a mine can close), with its own name, cargo labels, production, map colour, chances and news texts. The copies go into the LAST slots of
 * the industry table; NewGRFs fill the table from the first free slot upward, so they only meet when there are more than ~70 NewGRF industries.
 * The sources are found by CARGO LABEL (a plantation = produces FRUT and accepts nothing, a factory = accepts RUBR and makes GOOD ...), not by name, so
 * the code does not depend on the exact name of the Sub-tropical factory string. A source that is not found is skipped and the log says so.
 * Like gold_industry.cpp it works on cargo LABELS only: ResetIndustries() runs before the cargo table of the climate is built, and the labels are
 * turned into cargo numbers later by FinaliseIndustriesArray() (newgrf.cpp).
 *
 * Industries that ACCEPT cargo need their own copies of the building tiles: a station asks the TILES what they accept, and the tiles of a factory accept
 * rubber, copper ore and wood. A tile copy accepts "what its industry accepts" (special flag AcceptsAllCargo) and is drawn like the original tile
 * (grf_prop.subst_id), so it looks the same. The copies use the LAST tile slots (the animation of the original tile is lost).
 * Raw industries accept nothing and keep the original tiles.
 *
 * The Fishing Grounds of the NewGRF stood on water. The Sub-tropical graphics have no sea industry (the oil rig sprites belong to Temperate), so the
 * fish line starts with a FISH FARM on land (it takes the look of a plantation).
 *
 * Every field that is not sure to exist in this version of OpenTTD is reached through a generic lambda with `requires`, so a different name gives a
 * skipped step and not a compile error. The result is written to the log (debug category misc, level 0): how many industries and tiles were added.
 */

namespace {

/** The industry of the base game that is copied. */
enum class DesertSource : uint8_t {
	Plantation, ///< Fruit plantation: plants fields, produces one cargo.
	FishFarm,   ///< Fruit plantation (the fish farm: it looks like a plantation).
	Mine,       ///< Copper ore mine: produces one cargo.
	Factory,    ///< Sub-tropical factory: accepts cargo, produces one cargo.
	FoodPlant,  ///< Food processing plant: accepts cargo, produces one cargo.
};

struct DesertIndustry {
	DesertSource source;
	uint32_t out;           ///< Label of the cargo it produces, 0 for none.
	uint32_t in[2];         ///< Labels of the cargo it accepts, 0 for none.
	uint8_t rate;           ///< Production of a raw industry per production step (0 = keep the one of the source).
	uint8_t colour;         ///< Colour on the industry map.
	uint8_t creation;       ///< Chance on a new map.
	uint8_t ingame;         ///< Chance during the game.
	uint8_t cost;           ///< Cost multiplier.
	StringID name;
	StringID news_new;
	StringID news_close;
	StringID news_up;
	StringID news_down;
};

static const DesertIndustry _desert_industries[] = {
	{DesertSource::Plantation, 'SGCN', {0, 0}, 9, 0x6C, 5, 4, 55,
		STR_DESERT_IND_SUGAR_PLANTATION_NAME, STR_DESERT_IND_SUGAR_PLANTATION_NEW, STR_DESERT_IND_SUGAR_PLANTATION_CLOSE, STR_DESERT_IND_SUGAR_PLANTATION_UP, STR_DESERT_IND_SUGAR_PLANTATION_DOWN}, // Sugar Cane Plantation
	{DesertSource::Plantation, 'NUTS', {0, 0}, 8, 0x75, 5, 4, 55,
		STR_DESERT_IND_NUT_GROVE_NAME, STR_DESERT_IND_NUT_GROVE_NEW, STR_DESERT_IND_NUT_GROVE_CLOSE, STR_DESERT_IND_NUT_GROVE_UP, STR_DESERT_IND_NUT_GROVE_DOWN}, // Nut Grove
	{DesertSource::Plantation, 'COTN', {0, 0}, 10, 0x0F, 5, 4, 55,
		STR_DESERT_IND_COTTON_PLANTATION_NAME, STR_DESERT_IND_COTTON_PLANTATION_NEW, STR_DESERT_IND_COTTON_PLANTATION_CLOSE, STR_DESERT_IND_COTTON_PLANTATION_UP, STR_DESERT_IND_COTTON_PLANTATION_DOWN}, // Cotton Plantation
	{DesertSource::Factory, 'SUGR', {'SGCN', 0}, 0, 0x0A, 4, 3, 70,
		STR_DESERT_IND_SUGAR_MILL_NAME, STR_DESERT_IND_SUGAR_MILL_NEW, STR_DESERT_IND_SUGAR_MILL_CLOSE, STR_DESERT_IND_SUGAR_MILL_UP, STR_DESERT_IND_SUGAR_MILL_DOWN}, // Sugar Mill
	{DesertSource::Factory, 'EOIL', {'NUTS', 0}, 0, 0x7C, 4, 3, 60,
		STR_DESERT_IND_OIL_PRESS_NAME, STR_DESERT_IND_OIL_PRESS_NEW, STR_DESERT_IND_OIL_PRESS_CLOSE, STR_DESERT_IND_OIL_PRESS_UP, STR_DESERT_IND_OIL_PRESS_DOWN}, // Oil Press
	{DesertSource::Factory, 'GOOD', {'COTN', 0}, 0, 0xB8, 4, 3, 70,
		STR_DESERT_IND_TEXTILE_MILL_NAME, STR_DESERT_IND_TEXTILE_MILL_NEW, STR_DESERT_IND_TEXTILE_MILL_CLOSE, STR_DESERT_IND_TEXTILE_MILL_UP, STR_DESERT_IND_TEXTILE_MILL_DOWN}, // Textile Mill
	{DesertSource::FoodPlant, 'FOOD', {'SUGR', 'EOIL'}, 0, 0x0C, 5, 4, 65,
		STR_DESERT_IND_BAKERY_NAME, STR_DESERT_IND_BAKERY_NEW, STR_DESERT_IND_BAKERY_CLOSE, STR_DESERT_IND_BAKERY_UP, STR_DESERT_IND_BAKERY_DOWN}, // Bakery
	{DesertSource::FishFarm, 'FISH', {0, 0}, 8, 0x9B, 4, 3, 60,
		STR_DESERT_IND_FISHING_GROUNDS_NAME, STR_DESERT_IND_FISHING_GROUNDS_NEW, STR_DESERT_IND_FISHING_GROUNDS_CLOSE, STR_DESERT_IND_FISHING_GROUNDS_UP, STR_DESERT_IND_FISHING_GROUNDS_DOWN}, // Fish Farm
	{DesertSource::Factory, 'FOOD', {'FISH', 0}, 0, 0x95, 4, 3, 60,
		STR_DESERT_IND_FISH_CANNERY_NAME, STR_DESERT_IND_FISH_CANNERY_NEW, STR_DESERT_IND_FISH_CANNERY_CLOSE, STR_DESERT_IND_FISH_CANNERY_UP, STR_DESERT_IND_FISH_CANNERY_DOWN}, // Fish Cannery
	{DesertSource::Plantation, 'CASS', {0, 0}, 9, 0x70, 5, 4, 55,
		STR_DESERT_IND_CASSAVA_FARM_NAME, STR_DESERT_IND_CASSAVA_FARM_NEW, STR_DESERT_IND_CASSAVA_FARM_CLOSE, STR_DESERT_IND_CASSAVA_FARM_UP, STR_DESERT_IND_CASSAVA_FARM_DOWN}, // Cassava Farm
	{DesertSource::Factory, 'FOOD', {'CASS', 0}, 0, 0x1C, 4, 3, 60,
		STR_DESERT_IND_CASSAVA_MILL_NAME, STR_DESERT_IND_CASSAVA_MILL_NEW, STR_DESERT_IND_CASSAVA_MILL_CLOSE, STR_DESERT_IND_CASSAVA_MILL_UP, STR_DESERT_IND_CASSAVA_MILL_DOWN}, // Cassava Mill
	{DesertSource::Mine, 'CLAY', {0, 0}, 9, 0xBF, 4, 3, 60,
		STR_DESERT_IND_CLAY_PIT_NAME, STR_DESERT_IND_CLAY_PIT_NEW, STR_DESERT_IND_CLAY_PIT_CLOSE, STR_DESERT_IND_CLAY_PIT_UP, STR_DESERT_IND_CLAY_PIT_DOWN}, // Clay Pit
	{DesertSource::Factory, 'GOOD', {'CLAY', 0}, 0, 0xC2, 4, 3, 65,
		STR_DESERT_IND_POTTERY_WORKS_NAME, STR_DESERT_IND_POTTERY_WORKS_NEW, STR_DESERT_IND_POTTERY_WORKS_CLOSE, STR_DESERT_IND_POTTERY_WORKS_UP, STR_DESERT_IND_POTTERY_WORKS_DOWN}, // Pottery Works
	{DesertSource::Plantation, 'COFF', {0, 0}, 8, 0x6D, 5, 4, 55,
		STR_DESERT_IND_COFFEE_PLANTATION_NAME, STR_DESERT_IND_COFFEE_PLANTATION_NEW, STR_DESERT_IND_COFFEE_PLANTATION_CLOSE, STR_DESERT_IND_COFFEE_PLANTATION_UP, STR_DESERT_IND_COFFEE_PLANTATION_DOWN}, // Coffee Plantation
	{DesertSource::Factory, 'FOOD', {'COFF', 0}, 0, 0x7E, 4, 3, 60,
		STR_DESERT_IND_COFFEE_ROASTERY_NAME, STR_DESERT_IND_COFFEE_ROASTERY_NEW, STR_DESERT_IND_COFFEE_ROASTERY_CLOSE, STR_DESERT_IND_COFFEE_ROASTERY_UP, STR_DESERT_IND_COFFEE_ROASTERY_DOWN}, // Coffee Roastery
	{DesertSource::Factory, 'PETR', {'OIL_', 0}, 0, 0x0E, 4, 3, 80,
		STR_DESERT_IND_PETROL_REFINERY_NAME, STR_DESERT_IND_PETROL_REFINERY_NEW, STR_DESERT_IND_PETROL_REFINERY_CLOSE, STR_DESERT_IND_PETROL_REFINERY_UP, STR_DESERT_IND_PETROL_REFINERY_DOWN}, // Petrol Refinery
	{DesertSource::Factory, 'DIES', {'OIL_', 0}, 0, 0x10, 4, 3, 80,
		STR_DESERT_IND_DIESEL_DISTILLERY_NAME, STR_DESERT_IND_DIESEL_DISTILLERY_NEW, STR_DESERT_IND_DIESEL_DISTILLERY_CLOSE, STR_DESERT_IND_DIESEL_DISTILLERY_UP, STR_DESERT_IND_DIESEL_DISTILLERY_DOWN}, // Diesel Distillery
	{DesertSource::Factory, 'FERT', {'PETR', 0}, 0, 0x5C, 4, 3, 70,
		STR_DESERT_IND_FERTILISER_PLANT_NAME, STR_DESERT_IND_FERTILISER_PLANT_NEW, STR_DESERT_IND_FERTILISER_PLANT_CLOSE, STR_DESERT_IND_FERTILISER_PLANT_UP, STR_DESERT_IND_FERTILISER_PLANT_DOWN}, // Fertiliser Plant
	{DesertSource::Factory, 'FMSP', {'GOOD', 0}, 0, 0x3E, 4, 3, 65,
		STR_DESERT_IND_FARM_SUPPLY_WORKS_NAME, STR_DESERT_IND_FARM_SUPPLY_WORKS_NEW, STR_DESERT_IND_FARM_SUPPLY_WORKS_CLOSE, STR_DESERT_IND_FARM_SUPPLY_WORKS_UP, STR_DESERT_IND_FARM_SUPPLY_WORKS_DOWN}, // Farm Supply Works
	{DesertSource::FoodPlant, 0, {'FERT', 'FMSP'}, 0, 0x66, 4, 3, 60,
		STR_DESERT_IND_AGRO_DEPOT_NAME, STR_DESERT_IND_AGRO_DEPOT_NEW, STR_DESERT_IND_AGRO_DEPOT_CLOSE, STR_DESERT_IND_AGRO_DEPOT_UP, STR_DESERT_IND_AGRO_DEPOT_DOWN}, // Agro Depot
	{DesertSource::FoodPlant, 0, {'PETR', 'DIES'}, 0, 0x12, 4, 3, 60,
		STR_DESERT_IND_FUEL_DEPOT_NAME, STR_DESERT_IND_FUEL_DEPOT_NEW, STR_DESERT_IND_FUEL_DEPOT_CLOSE, STR_DESERT_IND_FUEL_DEPOT_UP, STR_DESERT_IND_FUEL_DEPOT_DOWN}, // Fuel Depot
};

/** An empty cargo label slot. */
static CargoLabel NoLabel() { return CargoLabel{CT_INVALID}; }

static CargoLabel LabelOf(uint32_t label) { return label == 0 ? NoLabel() : CargoLabel{label}; }

/** True if the layouts are lists of tiles that have a `gfx` (the layout format of OpenTTD 15). */
template <class S>
concept HasTileGfx = requires(S &s) { s.layouts.begin()->begin()->gfx; };

static bool ProducesLabel(const IndustrySpec &spec, uint32_t label)
{
	bool found = false;
	[&](const auto &s) {
		if constexpr (requires { s.produced_cargo_label.begin(); }) {
			for (const auto &l : s.produced_cargo_label) {
				if (l == CargoLabel{label}) found = true;
			}
		}
	}(spec);
	return found;
}

static bool AcceptsAnyLabel(const IndustrySpec &spec, uint32_t label)
{
	for (const auto &v : spec.accepts_cargo_label) {
		if (const CargoLabel *l = std::get_if<CargoLabel>(&v); l != nullptr && *l == CargoLabel{label}) return true;
	}
	return false;
}

static bool AcceptsNothing(const IndustrySpec &spec)
{
	for (const auto &v : spec.accepts_cargo_label) {
		if (const CargoLabel *l = std::get_if<CargoLabel>(&v); l != nullptr && *l == NoLabel()) continue;
		return false;
	}
	return true;
}

/** The enabled industry of the climate that is the source of `source`, or nullptr. */
static const IndustrySpec *FindSource(DesertSource source)
{
	const IndustrySpec *found = nullptr;
	for (const IndustrySpec &spec : _industry_specs) {
		if (!spec.enabled) continue;
		switch (source) {
			case DesertSource::Plantation:
			case DesertSource::FishFarm:
				if (ProducesLabel(spec, 'FRUT') && AcceptsNothing(spec)) found = &spec;
				break;
			case DesertSource::Mine:
				if (spec.name == STR_INDUSTRY_NAME_COPPER_ORE_MINE || (ProducesLabel(spec, 'CORE') && AcceptsNothing(spec))) found = &spec;
				break;
			case DesertSource::Factory:
				if (AcceptsAnyLabel(spec, 'RUBR') && ProducesLabel(spec, 'GOOD')) found = &spec;
				break;
			case DesertSource::FoodPlant:
				if (AcceptsAnyLabel(spec, 'FRUT') && ProducesLabel(spec, 'FOOD')) found = &spec;
				break;
		}
		if (found != nullptr) break;
	}
	return found;
}

/** State of one run: which original tile was copied to which new tile. */
struct TileState {
	std::map<uint8_t, uint8_t> map;
	int next = static_cast<int>(NUM_INDUSTRYTILES) - 2; ///< Next free tile slot, from the top (the last slot is the invalid tile).
	int added = 0;
	int skipped = 0;
	bool flag_ok = true;
};

/** A copy of a tile: accepts what its industry accepts, draws like the original. */
static void MakeAcceptAllTile(IndustryTileSpec &tile, uint8_t original, TileState &st)
{
	[&](auto &s) {
		if constexpr (requires { s.acceptance[0] = 0; }) {
			for (auto &a : s.acceptance) a = 0;
		}
		if constexpr (requires { s.accepts_cargo[0] = INVALID_CARGO; }) {
			for (auto &c : s.accepts_cargo) c = INVALID_CARGO;
		}
		if constexpr (requires { typename std::remove_cvref_t<decltype(s.special_flags)>::EnumType; }) {
			using E = typename std::remove_cvref_t<decltype(s.special_flags)>::EnumType;
			s.special_flags.Set(static_cast<E>(1)); // 1 = AcceptsAllCargo (0 = NextFrameIsNotProduction)
		} else if constexpr (requires { s.special_flags |= 2; }) {
			s.special_flags |= 2;
		} else {
			st.flag_ok = false;
		}
		if constexpr (requires { s.grf_prop.subst_id = original; }) s.grf_prop.subst_id = original;
		if constexpr (requires { s.enabled = true; }) s.enabled = true;
	}(tile);
}

static uint8_t CloneTile(uint8_t original, TileState &st)
{
	auto it = st.map.find(original);
	if (it != st.map.end()) return it->second;
	if (st.next < static_cast<int>(NEW_INDUSTRYTILEOFFSET) || original >= NEW_INDUSTRYTILEOFFSET) {
		/* No free slot (or a special layout value): keep the original tile. */
		st.map[original] = original;
		st.skipped++;
		return original;
	}
	const uint8_t id = static_cast<uint8_t>(st.next--);
	_industry_tile_specs[id] = _industry_tile_specs[original];
	MakeAcceptAllTile(_industry_tile_specs[id], original, st);
	st.map[original] = id;
	++st.added;
	return id;
}

static void ApplyEntry(IndustrySpec &dst, const DesertIndustry &e, TileState &st)
{
	const bool accepts = e.in[0] != 0 || e.in[1] != 0;
	[&](auto &s) {
		if constexpr (requires { s.name = e.name; }) s.name = e.name;
		if constexpr (requires { s.new_industry_text = e.news_new; }) s.new_industry_text = e.news_new;
		if constexpr (requires { s.closure_text = e.news_close; }) s.closure_text = e.news_close;
		if constexpr (requires { s.production_up_text = e.news_up; }) s.production_up_text = e.news_up;
		if constexpr (requires { s.production_down_text = e.news_down; }) s.production_down_text = e.news_down;
		if constexpr (requires { s.cost_multiplier = e.cost; }) s.cost_multiplier = e.cost;
		if constexpr (requires { s.map_colour = e.colour; }) s.map_colour = e.colour;
		if constexpr (requires { s.appear_creation[0] = e.creation; }) std::fill(std::begin(s.appear_creation), std::end(s.appear_creation), e.creation);
		if constexpr (requires { s.appear_ingame[0] = e.ingame; }) std::fill(std::begin(s.appear_ingame), std::end(s.appear_ingame), e.ingame);
		if constexpr (requires { s.enabled = true; }) s.enabled = true;

		/* What it produces and what it accepts: labels only, the numbers are filled in later. */
		if constexpr (requires { s.produced_cargo[0] = INVALID_CARGO; }) {
			for (auto &c : s.produced_cargo) c = INVALID_CARGO;
		}
		if constexpr (requires { s.accepts_cargo[0] = INVALID_CARGO; }) {
			for (auto &c : s.accepts_cargo) c = INVALID_CARGO;
		}
		if constexpr (requires { s.produced_cargo_label[0] = NoLabel(); }) {
			for (auto &v : s.produced_cargo_label) v = NoLabel();
			s.produced_cargo_label[0] = LabelOf(e.out);
		}
		for (auto &v : s.accepts_cargo_label) v = NoLabel();
		s.accepts_cargo_label[0] = LabelOf(e.in[0]);
		if (std::size(s.accepts_cargo_label) > 1) s.accepts_cargo_label[1] = LabelOf(e.in[1]);

		/* Every accepted cargo gives the first product, 1 to 1. */
		if constexpr (requires { s.input_cargo_multiplier[0][0] = 256; }) {
			for (auto &row : s.input_cargo_multiplier) {
				for (auto &m : row) m = 0;
				row[0] = 256;
			}
		}
		if (e.rate != 0) {
			if constexpr (requires { s.production_rate[0] = e.rate; }) s.production_rate[0] = e.rate;
		}
		if constexpr (HasTileGfx<std::remove_reference_t<decltype(s)>>) {
			if (accepts) {
				for (auto &layout : s.layouts) {
					for (auto &tile : layout) tile.gfx = CloneTile(static_cast<uint8_t>(tile.gfx), st);
				}
			}
		}
	}(dst);
}

} // namespace

/** Called at the end of ResetIndustries(). */
void ApplyDesertIndustries()
{
	if (_settings_game.game_creation.landscape != LandscapeType::Tropic) return;

	TileState st;
	int placed = 0;
	const size_t count = std::size(_desert_industries);
	for (size_t n = 0; n < count; n++) {
		const size_t slot = static_cast<size_t>(NUM_INDUSTRYTYPES) - 1 - n;
		if (slot < static_cast<size_t>(NEW_INDUSTRYOFFSET)) break;
		const DesertIndustry &e = _desert_industries[n];
		const IndustrySpec *source = FindSource(e.source);
		if (source == nullptr) {
			Debug(misc, 0, "Desert industries: no source industry for entry {}, skipped", n);
			continue;
		}
		IndustrySpec copy = *source; // a copy, because the source stays as it is
		ApplyEntry(copy, e, st);
		_industry_specs[slot] = copy;
		placed++;
	}
	Debug(misc, 0, "Desert industries: {} of {} added, {} building tiles copied, {} kept", placed, count, st.added, st.skipped);
	if (!st.flag_ok) Debug(misc, 0, "Desert industries: the tile flag AcceptsAllCargo could not be set, the processors will not accept cargo");
}
