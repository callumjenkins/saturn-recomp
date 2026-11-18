// saturn-recomp runtime — what the room has to tell the player (online.cpp's online_card), over the picture.
#pragma once
#include <cstdint>

void online_card_draw(int out_w, float scale);
// True when player 1's `pressed` Start took the card's offer of another match, so the game must not see it.
bool online_card_takes_start(uint16_t pressed);
