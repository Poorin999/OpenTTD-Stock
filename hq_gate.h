/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file hq_gate.h The headquarters is compulsory: a company without one gets the HQ tool and can use no other building tool until it has placed the HQ. */

#ifndef HQ_GATE_H
#define HQ_GATE_H

#include "company_type.h"

/** Whether this company has no headquarters yet (a valid company only). */
bool CompanyNeedsHq(CompanyID company);

#endif /* HQ_GATE_H */
