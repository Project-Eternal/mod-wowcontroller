/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// The server's .map format from an ADT (AdtConvert.cpp: AzerothCore's map extractor in memory). Not thread safe (the extractor keeps its work in
// globals): one conversion at a time.
#ifndef WOWTERRAIN_ADT_CONVERT_H
#define WOWTERRAIN_ADT_CONVERT_H

#include "Define.h"
#include <string>
#include <vector>

namespace WoWTerrain
{
    // inputPath: only for messages; build: written into the header
    bool ConvertADT(std::string const& inputPath, std::vector<uint8> const& adtData, std::vector<uint8>& output, uint32 build);
}

#endif
