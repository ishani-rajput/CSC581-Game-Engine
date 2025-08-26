# Feeling Loopy 🔂

Welcome to your first dive into engine programming using **SDL3** and **C++**! In this class assignment, you'll build a small application that opens an SDL window and animates a sprite sheet, frame by frame, looped indefinitely.

##  Objectives

By the end of this assignment, you'll be able to:

- Open and manage an SDL window
- Implement and use SDL renderers
- Build a game-style animation loop
- Load and animate a sprite sheet

##  Setup Instructions

Follow these steps to clone, build, and run the project on your machine:

### 1. Clone the repository

```bash
git clone https://github.com/AlexanderCard/CSC481-581-M1.git
```

----------

### 2. Install SDL3

> SDL3 is the latest major release. You’ll need both SDL3 and optionally `SDL3_image` if the sprite sheet is in PNG format.

#### On **macOS** (with Homebrew):

```bash
brew install sdl3

```

#### On **Ubuntu/Debian**:

```bash
sudo apt install libsdl3-dev

```

#### On **Arch Linux**:

```bash
sudo pacman -S sdl3

```

#### On **Windows**:

Use [MSYS2](https://www.msys2.org/) or [vcpkg](https://vcpkg.io/) for easiest installation.

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL3

```

Then launch the MinGW64 shell and continue.

----------

### 3. Build the project

```bash
mkdir build && cd build
cmake ..
make

```

> Ensure `SDL3_DIR` is correctly set if CMake cannot find SDL.

----------

### 4. Fill out the code

----------

### 5. Run the animation

```bash
./main

```

You should see a ran successfully in the console.

----------

## Core Concepts

### What is SDL?

[Simple DirectMedia Layer (SDL)](https://www.libsdl.org/) is a cross-platform library that provides low-level access to audio, keyboard, mouse, and graphics hardware.

----------

### 🪟 SDL Window & Renderer

-   **[Window](https://wiki.libsdl.org/SDL3/SDL_CreateWindow)**: Created with `SDL_CreateWindow`, it’s your application’s display surface.
    
-   **[Renderer](https://wiki.libsdl.org/SDL3/SDL_CreateRenderer)**: Created with `SDL_CreateRenderer`, it's what we draw with; it also clears the screen, draws sprites, and updates the display.
    

```cpp
SDL_Window* window = SDL_CreateWindow("Feeling Loopy",640, 480, 0);
SDL_Renderer* renderer = SDL_CreateRenderer(window, NULL, 0);

```

----------

###  What is a Sprite Sheet?

A **sprite sheet** is a single image containing multiple frames of an animation, when rendered in sequence, these frames create an animation.

A simple analogy is that of a flip book animation, containing multiple images that we flip through(or in our case swap through).

Example:

```
+---------+---------+---------+---------+
| Frame 1 | Frame 2 | Frame 3 | Frame 4 |
+---------+---------+---------+---------+
```

Each frame has the same dimensions. You’ll display one frame at a time by changing the which part of the image is drawn onto the screen.

Combining the small images in one big image improves game performance, reduces memory usage, and speeds up loading time

----------

## Animation Loop

Your game loop will look like this:

```cpp
bool running = true;
while (running) {
	if(Event == userQuits){
		break;
	}
    // Update frame

    // Draw current frame
}

```

-   You use the number of iterations to pick the correct animation frame
    
-   When the last frame is reached, `%` loops back to the first
    

----------

## Assignment Requirements

Your task is to modify the code to:

✅ Open an SDL3 window  
✅ Enter an update loop  
✅ Display a sprite sheet and animate it  
✅ Advance animation every `N` frames or `M` milliseconds  
✅ Loop back to the beginning after the last frame  
✅ Terminate cleanly when the window is closed

> Bonus: Try adding keyboard input to speed up or slow down the animation.

----------

## 📎 Helpful Resources

-   [SDL3 Docs (official)](https://wiki.libsdl.org/SDL3/FrontPage)
    
-   [SDL3 Image Docs](https://wiki.libsdl.org/SDL3_image/CategoryAPI)
    
-   [Mike Shah SDL3 installation tutorial](https://www.youtube.com/watch?v=kyD5H6w1x-o&list=PLvv0ScY6vfd-RZSmGbLkZvkgec6lJ0BfX)
    

----------

## Final Output 
<img src="https://github.com/ATHARVA47/CSC481-581_Intro_to_SDL/blob/master/media/loopy.gif" width="auto" height="256" />

----------

## Final Notes

Keep your code modular and clean. Add comments where necessary. This assignment is your intro to SDL3, understand it well, and the rest of the assignments will make a lot more sense!

----------
