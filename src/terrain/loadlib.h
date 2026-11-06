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


#ifndef WOWTERRAIN_LOAD_LIB_H
#define WOWTERRAIN_LOAD_LIB_H

#include "Define.h"
#include <string>
#include <vector>

namespace WoWTerrain
{

constexpr auto FILE_FORMAT_VERSION = 18;

union u_map_fcc
{
    char   fcc_txt[4];
    uint32 fcc;
};

//
// File version chunk
//
// cppcheck-suppress ctuOneDefinitionRuleViolation
struct file_MVER
{
    union
    {
        uint32 fcc;
        char   fcc_txt[4];
    };
    uint32 size;
    uint32 ver;
};

class FileLoader
{
    uint8*  data;
    uint32  data_size;
public:
    virtual bool prepareLoadedData();
    uint8* GetData()     {return data;}
    uint32 GetDataSize() {return data_size;}

    file_MVER* version;
    FileLoader();
    ~FileLoader();
    bool loadData(std::vector<uint8> const& bytes);
    virtual void free();
};


} // namespace WoWTerrain

#endif
