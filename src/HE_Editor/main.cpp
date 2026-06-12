#include "EditorApplication.h"
#include <SDL3/SDL_main.h>

int main(int argc, char* argv[])
{
	std::string startupPath = argv[0];
    EditorApplication app(startupPath);
    return app.Run(argc, argv);
}
