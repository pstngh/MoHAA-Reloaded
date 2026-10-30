/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// bg_speed.h -- a player's top speed times a factor, truncated as MOHAA 1.11 does it

#pragma once

//
// The original game multiplies at full precision before truncating. In float, 250 x 0.96 rounds up to
// exactly 240 (an SMG runs at 240 instead of 239), and at sv_dmspeedmult 1.1 that gives 264 instead of 262.
//
static inline int BG_SpeedTimes(int speed, float mult)
{
    return (int)((double)speed * (double)mult);
}
