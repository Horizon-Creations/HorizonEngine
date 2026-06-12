#include "GameApplication.h"
#include <SDL3/SDL_main.h>

int main(int argc, char** argv) 
{
    GameApplication app(argv[0]);
    return app.Run(argc, argv);
}

