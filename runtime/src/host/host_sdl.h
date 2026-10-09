// saturn-recomp runtime — host.cpp's window, settings and controllers, for the launcher
// (launcher.cpp), which draws in the same window before the run starts.
#pragma once
#include "saturn.h"
#include "settings.h"
#include <SDL3/SDL.h>
#include <string>
#include <vector>

bool host_window(const SaturnConfig& cfg);           // SDL, the settings and the window, opened once
SDL_Window* host_sdl_window();
SDL_Renderer* host_sdl_renderer();

Settings& host_settings();
bool host_settings_save();                           // into the file, its comments kept; false if it cannot be written
void host_rebind();                                  // the keyboard and the open gamepads bound by the settings again

void host_pad_event(const SDL_Event& e);             // a gamepad connecting or leaving
struct HostController { std::string name; int slot; bool held; };   // held: any of its bound buttons
std::vector<HostController> host_controllers();      // the open gamepads, by player
