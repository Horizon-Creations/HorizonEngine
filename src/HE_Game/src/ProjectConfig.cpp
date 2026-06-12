#include "ProjectConfig.h"

bool ProjectConfigLoader::load(const std::filesystem::path& exeDir, ProjectConfig& outConfig)
{
    (void)exeDir;
    (void)outConfig;
    return false;
}

bool ProjectConfigLoader::save(const std::filesystem::path& exeDir, const ProjectConfig& config)
{
    (void)exeDir;
    (void)config;
    return false;
}
