// saturn-recomp runtime — the build's achievements (g_achievements): rcheevos's conditions on work RAM,
// tested at each VBlank-IN, and the unlocks kept in achievements.json beside the saves.
//
// The file, saturn-recomp-achievements/1:
//   format        "saturn-recomp-achievements/1"
//   game          {product, version, title}: IP.BIN's product number and version, as the save
//                 directory is named from them, and the game's name
//   achievements  [{id, title, description, points}]: every achievement of the build
//   unlocked      {id: UTC time, as 2026-11-08T21:04:11Z}, kept for ids the build no longer has
//
// A run whose saves last for it alone, or that something else plays or changes, still tests every
// trigger and logs those that trigger, and leaves the file alone. Its cards say why the unlock was
// not recorded.
#pragma once
#include "saturn/sh2.h"
#include <string>

// For the saves bios_boot chose: activates every achievement, less those the file already has when
// this run records to it. Returns why a trigger cannot be tested, naming its achievement, or "" if
// every one can.
std::string achievements_start(const SH2Achievement* defs, int n, const char* title);
void achievements_frame();          // once a VBlank-IN
// Sends each unlock's card, title and text, to `show` rather than host_notify; nullptr puts host_notify back.
void achievements_cards_for_tests(void (*show)(const char* title, const char* text));
