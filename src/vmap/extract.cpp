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

// For mod-wowcontroller: the parts of AzerothCore's vmap extractor's main file (src/tools/vmap4_extractor/vmapexport.cpp: ExtractSingleWmo,
// the unique ids; gameobject_extract.cpp: ExtractSingleModel) the module uses, and its MPQ file (mpq_libmpq.cpp: MPQFile) reading through the
// running game. Not thread safe (the extractor keeps its work in globals): one extraction at a time.

#include "adtfile.h"
#include "model.h"
#include "mpq_libmpq04.h"
#include "vmapexport.h"
#include "wmo.h"
#include <cstdio>
#include <cstring>
#include <map>

namespace WoWVmap
{
    namespace
    {
        GameFileReader g_reader;
    }

    void SetGameFileReader(GameFileReader reader)
    {
        g_reader = std::move(reader);
    }

    MPQFile::MPQFile(char const* filename) : eof(false), buffer(nullptr), pointer(0), size(0)
    {
        std::vector<uint8> contents;
        // (as the archives: a file of size 1 or less counts as none)
        if (!g_reader || !g_reader(filename, contents) || contents.size() <= 1)
        {
            eof = true;
            return;
        }
        size = contents.size();
        buffer = new char[size];
        memcpy(buffer, contents.data(), size);
    }

    std::size_t MPQFile::read(void* dest, std::size_t bytes)
    {
        if (eof) return 0;

        std::size_t rpos = pointer + bytes;
        if (rpos > std::size_t(size))
        {
            bytes = size - pointer;
            eof = true;
        }

        memcpy(dest, &(buffer[pointer]), bytes);

        pointer = rpos;

        return bytes;
    }

    void MPQFile::seek(int offset)
    {
        pointer = offset;
        eof = (pointer >= size);
    }

    void MPQFile::seekRelative(int offset)
    {
        pointer += offset;
        eof = (pointer >= size);
    }

    void MPQFile::close()
    {
        delete[] buffer;
        buffer = nullptr;
        eof = true;
    }

    // ---- from vmapexport.cpp ----

    bool preciseVectorData = false;
    std::unordered_map<std::string, WMODoodadData> WmoDoodads;
    char const* szWorkDirWmo = "./Buildings";   // (set by the module)
    std::map<std::pair<uint32, uint16>, uint32> uniqueObjectIds;

    uint32 GenerateUniqueObjectId(uint32 clientId, uint16 clientDoodadId)
    {
        return uniqueObjectIds.emplace(std::make_pair(clientId, clientDoodadId), uint32(uniqueObjectIds.size() + 1)).first->second;
    }

    bool FileExists(char const* file)
    {
        if (FILE* n = fopen(file, "rb"))
        {
            fclose(n);
            return true;
        }
        return false;
    }

    void strToLower(char* str)
    {
        while (*str)
        {
            *str = tolower(*str);
            ++str;
        }
    }

bool ExtractSingleWmo(std::string& fname)
{
    // Copy files from archive
    std::string originalName = fname;

    char szLocalFile[1024];
    char* plain_name = GetPlainName(&fname[0]);
    fixnamen(plain_name, strlen(plain_name));
    fixname2(plain_name, strlen(plain_name));
    sprintf(szLocalFile, "%s/%s", szWorkDirWmo, plain_name);

    if (FileExists(szLocalFile))
        return true;

    int p = 0;
    // Select root wmo files
    char const* rchr = strrchr(plain_name, '_');
    if (rchr != nullptr)
    {
        char cpy[4];
        memcpy(cpy, rchr, 4);
        for (int m : cpy)
        {
            if (isdigit(m))
                p++;
        }
    }

    if (p == 3)
        return true;

    bool file_ok = true;
    // (no progress line per building: the module logs its tiles)
    WMORoot froot(originalName);
    if (!froot.open())
    {
        printf("Couldn't open RootWmo!!!\n");
        return false;
    }
    FILE* output = fopen(szLocalFile, "wb");
    if (!output)
    {
        printf("couldn't open %s for writing!\n", szLocalFile);
        return false;
    }
    froot.ConvertToVMAPRootWmo(output);
    WMODoodadData& doodads = WmoDoodads[plain_name];
    std::swap(doodads, froot.DoodadData);
    int Wmo_nVertices = 0;
    uint32 groupCount = 0;
    //printf("root has %d groups\n", froot->nGroups);
    if (froot.nGroups != 0)
    {
        for (uint32 i = 0; i < froot.nGroups; ++i)
        {
            char temp[1024];
            strncpy(temp, fname.c_str(), 1024);
            temp[fname.length() - 4] = 0;
            char groupFileName[1024];
            int ret = snprintf(groupFileName, 1024, "%s_%03u.wmo", temp, i);
            if (ret < 0)
            {
                printf("Error when formatting string");
                return false;
            }
            //printf("Trying to open groupfile %s\n",groupFileName);

            string s = groupFileName;
            WMOGroup fgroup(s);
            if (!fgroup.open(&froot))
            {
                printf("Could not open all Group file for: %s\n", plain_name);
                file_ok = false;
                break;
            }

            if (fgroup.ShouldSkip(&froot))
                continue;

            Wmo_nVertices += fgroup.ConvertToVMAPGroupWmo(output, preciseVectorData);
            ++groupCount;
            for (uint16 groupReference : fgroup.DoodadReferences)
            {
                if (groupReference >= doodads.Spawns.size())
                    continue;

                uint32 doodadNameIndex = doodads.Spawns[groupReference].NameIndex;
                if (froot.ValidDoodadNames.find(doodadNameIndex) == froot.ValidDoodadNames.end())
                    continue;

                doodads.References.insert(groupReference);
            }
        }
    }

    fseek(output, 8, SEEK_SET); // store the correct no of vertices
    fwrite(&Wmo_nVertices, sizeof(int), 1, output);
    // store the correct no of groups
    fwrite(&groupCount, sizeof(uint32), 1, output);
    fclose(output);

    // Delete the extracted file in the case of an error
    if (!file_ok)
        remove(szLocalFile);
    return true;
}

    // ---- from gameobject_extract.cpp ----

bool ExtractSingleModel(std::string& fname)
{
    if (fname.length() < 4)
        return false;

    std::string extension = fname.substr(fname.length() - 4, 4);
    if (extension == ".mdx" || extension == ".MDX" || extension == ".mdl" || extension == ".MDL")
    {
        // replace .mdx -> .m2
        fname.erase(fname.length() - 2, 2);
        fname.append("2");
    }
    // >= 3.1.0 ADT MMDX section store filename.m2 filenames for corresponded .m2 file
    // nothing do

    std::string originalName = fname;

    char* name = GetPlainName((char*)fname.c_str());
    fixnamen(name, strlen(name));
    fixname2(name, strlen(name));

    std::string output(szWorkDirWmo);
    output += "/";
    output += name;

    if (FileExists(output.c_str()))
        return true;

    Model mdl(originalName);
    if (!mdl.open())
        return false;

    return mdl.ConvertToVMAPModel(output.c_str());
}
}
