// saturn-recomp runtime — drawing for the host's menu and touch pad (ui.cpp): rounded shapes, and text in
// Inter (third_party/inter, cut down to the characters these use), baked by stb_truetype at each size drawn.
#pragma once
#include <SDL3/SDL.h>
#include <string>

enum class UiAlign { Left, Centre, Right };

void ui_open(SDL_Renderer* ren);
void ui_fill_rounded(SDL_FRect r, float radius, SDL_FColor c);
void ui_fill_circle(float x, float y, float r, SDL_FColor c);
// `size` is the text's height in pixels, cap to descender; `y` its middle.
void ui_text(float x, float y, float size, const std::string& s, SDL_FColor c, bool bold = false, UiAlign align = UiAlign::Centre);
float ui_text_width(float size, const std::string& s, bool bold = false);
