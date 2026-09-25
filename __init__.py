"""saturnkit — game-agnostic building blocks for Sega Saturn reverse engineering and ports.

Each module handles one thing the Saturn, its BIOS or Sega's libraries
impose on every game, independent of any particular title:

    disc     disc images (.cue/.bin, .iso): tracks, IP.BIN, ISO 9660, extraction, CD-DA to WAV
    sh2      SH-2 (SH7604) decoding, disassembly, literal-pool cross-references, load-address finder
    hw       the address map and register names (VDP1, VDP2, SCU, SMPC, CD block, SH-2 on-chip, BIOS)
    sh2emu   an SH-2 interpreter for running isolated guest functions (the recompiler's oracle)
    recomp   layer 4: discover (functions in stripped code), match (the same code across programs)

Layers 1-3 are pure Python 3.8+ with no dependencies. Game-specific knowledge
belongs in the game's own tools/, not here.
"""
