// saturn-recomp runtime — the room's card (online_card.h): a pill at the top of the window.
#include "online_card.h"
#include "host.h"
#include "saturn.h"
#include "ui.h"

void online_card_draw(int out_w, float scale) {
    const OnlineCard card = online_card();
    if (card.text.empty()) return;
    const float size = 16 * scale, pad = 18 * scale, h = 38 * scale, top = 16 * scale;
    const float w = ui_text_width(size, card.text, true) + 2 * pad;
    ui_fill_rounded({(out_w - w) / 2, top, w, h}, h / 2, card.again ? SDL_FColor{0.27f, 0.47f, 0.95f, 0.95f}
                                                                     : SDL_FColor{0.09f, 0.1f, 0.13f, 0.88f});
    ui_text(out_w / 2.0f, top + h / 2, size, card.text, {0.97f, 0.98f, 1, 1}, true);
}

bool online_card_takes_start(uint16_t pressed) {
    if (!(pressed & B_START) || !online_card().again) return false;
    online_again();
    return true;
}
