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

// Copied from AzerothCore's map extractor (src/tools/map_extractor) for mod-wowcontroller: an ADT read from the game is converted into the
// server's .map format in memory (AdtTerrain.cpp). Changes: the namespace WoWTerrain; FileLoader::loadData takes the file's bytes (no MPQ).


#define _CRT_SECURE_NO_DEPRECATE

#include "loadlib.h"
#include <algorithm>
#include <cstdio>

namespace WoWTerrain
{


u_map_fcc MverMagic = { {'R', 'E', 'V', 'M'} };

FileLoader::FileLoader()
{
    data = nullptr;
    data_size = 0;
    version = nullptr;
}

FileLoader::~FileLoader()
{
    free();
}

bool FileLoader::loadData(std::vector<uint8> const& bytes)
{
    free();
    data_size = uint32(bytes.size());
    data = new uint8[data_size ? data_size : 1];
    std::copy(bytes.begin(), bytes.end(), data);
    if (data_size >= sizeof(file_MVER) && prepareLoadedData())
        return true;
    free();
    return false;
}

bool FileLoader::prepareLoadedData()
{
    // Check version
    version = (file_MVER*) data;
    if (version->fcc != MverMagic.fcc)
        return false;
    if (version->ver != FILE_FORMAT_VERSION)
        return false;
    return true;
}

void FileLoader::free()
{
    delete[] data;
    data = nullptr;
    data_size = 0;
    version = nullptr;
}

} // namespace WoWTerrain
