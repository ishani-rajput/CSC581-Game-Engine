#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include "entity.h"


const int WINDOW_WIDTH = 1920;  /*  Width of the game window to be created*/
const int WINDOW_HEIGHT = 1080; /*  Height of the game window to be created*/
const int FRAME_COUNT = 8;     /*  Number of frames in the spritesheet */
const int FRAME_WIDTH = 512;   /*  Width of the frame in the spritesheet */
const int FRAME_HEIGHT = 512;  /*  Height of the frame in the spritesheet */
const int ANIMATION_DELAY = 500;/* Number of iterations between the animation frames (determines delay) */

int main(int argc, char *argv[])
{
    // Init SDL
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;

    if (!SDL_CreateWindowAndRenderer("Feeling Loopy", WINDOW_WIDTH, WINDOW_HEIGHT, 0, &window, &renderer)) {
        SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Create an Entity object (centered bottom)
    Entity player(
        renderer,
        "../assets/skelly.png",
        (WINDOW_WIDTH - FRAME_WIDTH) / 2.0f,
        (WINDOW_HEIGHT - FRAME_HEIGHT),
        FRAME_WIDTH,
        FRAME_HEIGHT,
        FRAME_COUNT,
        ANIMATION_DELAY
    );

    bool running = true;
    SDL_Event event;

    while (running)
    {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }

        // Update entity
        player.update();

        // Clear screen with blue
        SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
        SDL_RenderClear(renderer);

        // Render entity
        player.render(renderer);

        SDL_RenderPresent(renderer);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}