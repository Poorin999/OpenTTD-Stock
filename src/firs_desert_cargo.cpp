/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file firs_desert_cargo.cpp Cargo types of the FIRS sub-tropical (desert) economy that the base game does not have. */

#include "stdafx.h"
#include "cargotype.h"

#include "table/strings.h"

#include <algorithm>
#include <iterator>

#include "safeguards.h"

/*
 * Called by the hook of patch_firs_desert_cargo (apply_overlay.py) inside SetupCargoForClimate() of cargotype.cpp, in the Sub-tropical climate only
 * (OpenTTD has no separate desert climate: the desert is a terrain of Sub-tropical). Only cargo whose label is NOT in the climate yet is added, so
 * nothing is duplicated. A new cargo is a COPY of an existing cargo of the climate (the "base": same sprite, same town effect) with its own label,
 * colour, weight, payment, transit days, classes and names. The labels are the FIRS labels, so a FIRS industry GRF that is loaded later finds them.
 * No industry is added here: these cargos only exist (price, refit by class, cargo payment list) until a NewGRF produces them.
 */

namespace {

struct FirsDesertCargo {
	CargoLabel label;        ///< FIRS label.
	CargoLabel base;         ///< Cargo of the climate that is copied (sprite, town effect).
	uint8_t colour;          ///< Legend colour (palette index).
	uint8_t weight;          ///< Weight of one unit in 1/16 ton.
	uint16_t multiplier;     ///< Capacity multiplier, 8 fractional bits.
	int64_t payment;         ///< Initial payment.
	uint8_t days1;           ///< Transit period 1.
	uint8_t days2;           ///< Transit period 2.
	bool freight;            ///< Counts as freight for trains.
	StringID name;           ///< Plural name.
	StringID name_single;    ///< Singular name.
	StringID quantity;       ///< "{WEIGHT_LONG} of ..." string.
	StringID abbrev;         ///< Abbreviation.
	StringID units;          ///< Unit of a single unit of cargo (STR_TONS, STR_LITERS).
	bool liquid;             ///< Liquid (class Liquid) or bulk.
};

static const FirsDesertCargo FIRS_DESERT[] = {
	{ CargoLabel{'CASS'}, CargoLabel{'MAIZ'},  60, 16, 0x100, 4437, 4, 40, true,  STR_FIRS_CARGO_PLURAL_CASSAVA,   STR_FIRS_CARGO_SINGULAR_CASSAVA,   STR_FIRS_QUANTITY_CASSAVA,   STR_FIRS_ABBREV_CASSAVA,   STR_TONS,   false },
	{ CargoLabel{'COFF'}, CargoLabel{'FRUT'}, 151, 16, 0x100, 6200, 5, 30, true,  STR_FIRS_CARGO_PLURAL_COFFEE,    STR_FIRS_CARGO_SINGULAR_COFFEE,    STR_FIRS_QUANTITY_COFFEE,    STR_FIRS_ABBREV_COFFEE,    STR_TONS,   false },
	{ CargoLabel{'NUTS'}, CargoLabel{'FRUT'}, 117, 16, 0x100, 5500, 5, 35, true,  STR_FIRS_CARGO_PLURAL_NUTS,      STR_FIRS_CARGO_SINGULAR_NUTS,      STR_FIRS_QUANTITY_NUTS,      STR_FIRS_ABBREV_NUTS,      STR_TONS,   false },
	{ CargoLabel{'SGCN'}, CargoLabel{'MAIZ'}, 106, 16, 0x100, 4200, 6, 45, true,  STR_FIRS_CARGO_PLURAL_SUGAR_CANE, STR_FIRS_CARGO_SINGULAR_SUGAR_CANE, STR_FIRS_QUANTITY_SUGAR_CANE, STR_FIRS_ABBREV_SUGAR_CANE, STR_TONS,   false },
	{ CargoLabel{'SUGR'}, CargoLabel{'FOOD'},  10, 16, 0x100, 5200, 8, 60, true,  STR_FIRS_CARGO_PLURAL_SUGAR,     STR_FIRS_CARGO_SINGULAR_SUGAR,     STR_FIRS_QUANTITY_SUGAR,     STR_FIRS_ABBREV_SUGAR,     STR_TONS,   false },
	{ CargoLabel{'COTN'}, CargoLabel{'MAIZ'},   1, 16, 0x100, 5600, 6, 45, true,  STR_FIRS_CARGO_PLURAL_COTTON,    STR_FIRS_CARGO_SINGULAR_COTTON,    STR_FIRS_QUANTITY_COTTON,    STR_FIRS_ABBREV_COTTON,    STR_TONS,   false },
	{ CargoLabel{'CLAY'}, CargoLabel{'CORE'}, 187, 16, 0x100, 4100, 7, 255, true, STR_FIRS_CARGO_PLURAL_CLAY,      STR_FIRS_CARGO_SINGULAR_CLAY,      STR_FIRS_QUANTITY_CLAY,      STR_FIRS_ABBREV_CLAY,      STR_TONS,   false },
	{ CargoLabel{'FERT'}, CargoLabel{'CORE'}, 176, 16, 0x100, 5100, 6, 60, true,  STR_FIRS_CARGO_PLURAL_FERTILISER, STR_FIRS_CARGO_SINGULAR_FERTILISER, STR_FIRS_QUANTITY_FERTILISER, STR_FIRS_ABBREV_FERTILISER, STR_TONS,  false },
	{ CargoLabel{'PETR'}, CargoLabel{'OIL_'},  17, 16, 0x100, 5400, 20, 255, true, STR_FIRS_CARGO_PLURAL_PETROL,   STR_FIRS_CARGO_SINGULAR_PETROL,    STR_FIRS_QUANTITY_PETROL,    STR_FIRS_ABBREV_PETROL,    STR_LITERS, true  },
	{ CargoLabel{'DIES'}, CargoLabel{'OIL_'},  38, 16, 0x100, 5300, 20, 255, true, STR_FIRS_CARGO_PLURAL_DIESEL,   STR_FIRS_CARGO_SINGULAR_DIESEL,    STR_FIRS_QUANTITY_DIESEL,    STR_FIRS_ABBREV_DIESEL,    STR_LITERS, true  },
	{ CargoLabel{'EOIL'}, CargoLabel{'OIL_'}, 124, 16, 0x100, 5700, 10, 60, true,  STR_FIRS_CARGO_PLURAL_EDIBLE_OIL, STR_FIRS_CARGO_SINGULAR_EDIBLE_OIL, STR_FIRS_QUANTITY_EDIBLE_OIL, STR_FIRS_ABBREV_EDIBLE_OIL, STR_LITERS, true },
	{ CargoLabel{'FMSP'}, CargoLabel{'GOOD'}, 163, 8,  0x200, 5800, 5, 28, true,  STR_FIRS_CARGO_PLURAL_FARM_SUPPLIES, STR_FIRS_CARGO_SINGULAR_FARM_SUPPLIES, STR_FIRS_QUANTITY_FARM_SUPPLIES, STR_FIRS_ABBREV_FARM_SUPPLIES, STR_CRATES, false },
	{ CargoLabel{'FISH'}, CargoLabel{'FOOD'}, 208, 16, 0x100, 4550, 5, 25, true,  STR_FIRS_CARGO_PLURAL_FISH,       STR_FIRS_CARGO_SINGULAR_FISH,      STR_FIRS_QUANTITY_FISH,      STR_FIRS_ABBREV_FISH,      STR_TONS,   false },
};

}

/** How many FIRS desert cargo types the table has. */
size_t FirsDesertCargoCount()
{
	return std::size(FIRS_DESERT);
}

/**
 * Fill \a out with the n-th FIRS desert cargo.
 * @param n Index in the table, below FirsDesertCargoCount().
 * @param out Free cargo slot (it is the first free slot of CargoSpec::array).
 * @param bitnum Cargo bit number to give it (unique, 32 or higher: no default cargo and no NewGRF 32-bit cargo mask uses it).
 * @param first Start of CargoSpec::array (private, so SetupCargoForClimate() passes it in).
 * @param last End of the filled part of CargoSpec::array (the slot where \a out is): the slots after it still hold the cargo of the previous climate and must not be searched.
 * @return True when the slot was filled; false when the cargo is already in the climate or its base cargo is missing (the slot is left as it was).
 */
bool MakeFirsDesertCargo(size_t n, CargoSpec &out, uint8_t bitnum, const CargoSpec *first, const CargoSpec *last)
{
	if (n >= std::size(FIRS_DESERT)) return false;
	const FirsDesertCargo &d = FIRS_DESERT[n];

	if (std::find_if(first, last, [&](const CargoSpec &c) { return c.label == d.label; }) != last) return false;
	const CargoSpec *base = std::find_if(first, last, [&](const CargoSpec &c) { return c.label == d.base; });
	if (base == last) return false;

	out = *base;
	out.label = d.label;
	out.bitnum = bitnum;
	out.legend_colour = PixelColour{d.colour};
	out.rating_colour = PixelColour{d.colour};
	out.weight = d.weight;
	out.multiplier = d.multiplier;
	out.classes = d.liquid ? CargoClasses({CargoClass::Liquid, CargoClass::NonPotable}) : CargoClasses({CargoClass::Bulk, CargoClass::NonPotable});
	out.initial_payment = d.payment;
	out.transit_periods[0] = d.days1;
	out.transit_periods[1] = d.days2;
	out.is_freight = d.freight;
	out.name = d.name;
	out.name_single = d.name_single;
	out.units_volume = d.units;
	out.quantifier = d.quantity;
	out.abbrev = d.abbrev;
	return true;
}
