#include "Application/GameLogicLoader.h"

namespace HE {

GameLogicLoader::GameLogicLoader()  = default;
GameLogicLoader::~GameLogicLoader() = default;

bool GameLogicLoader::load(const std::filesystem::path& dllPath)
{
    (void)dllPath;
    return false;
}

void GameLogicLoader::unload(HorizonWorld& world)
{
    (void)world;
}

bool GameLogicLoader::reload(const std::filesystem::path& dllPath, HorizonWorld& world)
{
    (void)dllPath;
    (void)world;
    return false;
}

bool GameLogicLoader::isLoaded() const { return lib_.isLoaded(); }
IGameLogic* GameLogicLoader::logic() const { return logic_; }

} // namespace HE
