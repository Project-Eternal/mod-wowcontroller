/*
 * An extracted game in a directory (WoWController.Directory): the files under their names in the game ("DBFilesClient\\Spell.dbc" is
 * DIRECTORY/DBFilesClient/Spell.dbc). It takes no commands. Useful where no game runs next to the server (a native server, tests).
 */
#include "WoWProcess.h"
#include <filesystem>
#include <fstream>
#include <iterator>

namespace
{
    class DirectoryProcess : public WoWProcess
    {
    public:
        explicit DirectoryProcess(std::filesystem::path directory) : _directory(std::move(directory)) { }

        std::string Describe() const override { return "the extracted game in " + _directory.string(); }

        bool ReadFile(std::string const& name, std::vector<uint8>& contents, bool /*patient*/) override
        {
            std::string relative = name;
            for (char& c : relative)
                if (c == '\\')
                    c = '/';
            std::ifstream in(_directory / relative, std::ios::binary);
            if (!in)
                return false;
            contents.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return !in.bad();
        }

        std::optional<std::string> Command(std::string const& /*line*/) override
        {
            return std::nullopt;
        }

    private:
        std::filesystem::path _directory;
    };
}

std::unique_ptr<WoWProcess> CreateDirectoryProcess(std::string const& directory)
{
    std::error_code error;
    if (directory.empty() || !std::filesystem::is_directory(directory, error))
        return nullptr;
    return std::make_unique<DirectoryProcess>(directory);
}
