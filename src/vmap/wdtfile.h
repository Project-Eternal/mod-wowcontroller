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

// Copied from AzerothCore's vmap extractor (src/tools/vmap4_extractor) for mod-wowcontroller: the building and doodad collision of a tile is
// extracted from the game's files on first use (VmapTiles.cpp). Changes: the namespace WoWVmap, header guards WOWVMAP_*, the files are read
// through the running game (mpq_libmpq04.h), Define.h for the integer types.

#ifndef WOWVMAP_WDTFILE_H
#define WOWVMAP_WDTFILE_H

#include "mpq_libmpq04.h"
#include "wmo.h"
#include <string>

namespace WoWVmap
{

class ADTFile;

class WDTFile
{
public:
    WDTFile(char* file_name, char* file_name1);
    ~WDTFile(void);

    bool init(uint32 mapId);
    ADTFile* GetMap(int x, int z);

    std::vector<std::string> _wmoNames;

private:
    MPQFile _file;
    std::string filename;
};

}

#endif
