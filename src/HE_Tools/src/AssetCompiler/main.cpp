// asset_compiler.exe entrypoint
// Usage: asset_compiler <source_dir> <output_dir>
//
// Walks <source_dir>, dispatches each source file to the appropriate
// importer (Mesh/Texture/Audio/Material) and writes .hasset files
// to <output_dir>.

#include "../AssetImporter/AudioImporter.h"
#include "../AssetImporter/MaterialImporter.h"
#include "../AssetImporter/MeshImporter.h"
#include "../AssetImporter/TextureImporter.h"

int main(int /*argc*/, char** /*argv*/)
{

    return 0;
}
