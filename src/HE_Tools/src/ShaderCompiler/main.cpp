#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdlib>

namespace fs = std::filesystem;

struct CompileOptions
{
    fs::path inputPath;
    fs::path outputPath;
    std::string shaderStage; // "vert", "frag", "comp", etc.
    bool optimize = true;
    bool verbose = false;
};

bool compileShader(const CompileOptions& options)
{
    // Build glslc command line
    std::string cmd = "glslc";

    // Add shader stage
    if (!options.shaderStage.empty())
    {
        cmd += " -fshader-stage=" + options.shaderStage;
    }

    // Add optimization
    if (options.optimize)
    {
        cmd += " -O";
    }

    // Add input/output
    cmd += " \"" + options.inputPath.string() + "\"";
    cmd += " -o \"" + options.outputPath.string() + "\"";

    if (options.verbose)
    {
        std::cout << "Running: " << cmd << std::endl;
    }

    int result = std::system(cmd.c_str());

    if (result != 0)
    {
        std::cerr << "Error: glslc failed with code " << result << std::endl;
        return false;
    }

    if (options.verbose)
    {
        std::cout << "Successfully compiled: " << options.outputPath << std::endl;
    }

    return true;
}

void printUsage()
{
    std::cout << "Horizon Engine - Vulkan Shader Compiler\n";
    std::cout << "Usage: shader_compiler [options] <input.glsl>\n\n";
    std::cout << "Options:\n";
    std::cout << "  -o <output>      Output SPIR-V file (default: <input>.spv)\n";
    std::cout << "  -stage <type>    Shader stage: vert, frag, comp, geom, tesc, tese\n";
    std::cout << "  --no-optimize    Disable optimization\n";
    std::cout << "  -v, --verbose    Verbose output\n";
    std::cout << "  -h, --help       Show this help\n\n";
    std::cout << "Example:\n";
    std::cout << "  shader_compiler -stage vert -o shader.vert.spv shader.vert\n";
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        printUsage();
        return 1;
    }

    CompileOptions options;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help")
        {
            printUsage();
            return 0;
        }
        else if (arg == "-o" && i + 1 < argc)
        {
            options.outputPath = argv[++i];
        }
        else if (arg == "-stage" && i + 1 < argc)
        {
            options.shaderStage = argv[++i];
        }
        else if (arg == "--no-optimize")
        {
            options.optimize = false;
        }
        else if (arg == "-v" || arg == "--verbose")
        {
            options.verbose = true;
        }
        else if (arg[0] != '-')
        {
            options.inputPath = arg;
        }
        else
        {
            std::cerr << "Unknown option: " << arg << std::endl;
            return 1;
        }
    }

    if (options.inputPath.empty())
    {
        std::cerr << "Error: No input file specified" << std::endl;
        printUsage();
        return 1;
    }

    if (!fs::exists(options.inputPath))
    {
        std::cerr << "Error: Input file not found: " << options.inputPath << std::endl;
        return 1;
    }

    // Auto-detect shader stage from extension if not specified
    if (options.shaderStage.empty())
    {
        std::string ext = options.inputPath.extension().string();
        if (ext == ".vert") options.shaderStage = "vert";
        else if (ext == ".frag") options.shaderStage = "frag";
        else if (ext == ".comp") options.shaderStage = "comp";
        else if (ext == ".geom") options.shaderStage = "geom";
        else if (ext == ".tesc") options.shaderStage = "tesc";
        else if (ext == ".tese") options.shaderStage = "tese";
        else
        {
            std::cerr << "Warning: Could not auto-detect shader stage from extension '" 
                      << ext << "'. Use -stage option." << std::endl;
        }
    }

    // Generate output path if not specified
    if (options.outputPath.empty())
    {
        options.outputPath = options.inputPath;
        options.outputPath += ".spv";
    }

    // Create output directory if needed
    if (options.outputPath.has_parent_path())
    {
        fs::create_directories(options.outputPath.parent_path());
    }

    if (!compileShader(options))
    {
        return 1;
    }

    std::cout << "Shader compilation successful: " << options.outputPath << std::endl;
    return 0;
}
