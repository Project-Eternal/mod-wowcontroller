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

// For mod-wowcontroller: in place of AzerothCore's vmap extractor's MPQ layer (src/tools/vmap4_extractor/mpq_libmpq04.h). The extractor's
// MPQFile is kept (same interface), but it reads the whole file through the reader the module sets (SetGameFileReader: the running game)
// instead of the archives.
#ifndef WOWVMAP_MPQ_LIBMPQ04_H
#define WOWVMAP_MPQ_LIBMPQ04_H

#include "Define.h"
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace WoWVmap
{
    using std::string;
    using std::vector;

    // the bytes of a game file by its name in the game ("World\\wmo\\...\\x.wmo"); false when there is none
    using GameFileReader = std::function<bool(std::string const& name, std::vector<uint8>& contents)>;
    void SetGameFileReader(GameFileReader reader);

    class MPQFile
    {
        bool eof;
        char* buffer;
        std::size_t pointer, size;

        // disable copying
        MPQFile(MPQFile const& /*f*/) {}
        void operator=(MPQFile const& /*f*/) {}

    public:
        MPQFile(char const* filename);    // filenames are not case sensitive
        ~MPQFile() { close(); }
        std::size_t read(void* dest, std::size_t bytes);
        std::size_t getSize() { return size; }
        std::size_t getPos() { return pointer; }
        char* getBuffer() { return buffer; }
        char* getPointer() { return buffer + pointer; }
        bool isEof() { return eof; }
        void seek(int offset);
        void seekRelative(int offset);
        void close();
    };

    inline void flipcc(char* fcc)
    {
        std::swap(fcc[0], fcc[3]);
        std::swap(fcc[1], fcc[2]);
    }
}

#endif
