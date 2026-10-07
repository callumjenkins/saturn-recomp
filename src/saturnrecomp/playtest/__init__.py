"""Builds for playtesters, and their sessions sent back for review.

A tester runs the launcher (launcher.py), which plays the game and sends each session to the
playtest Worker (saturn-recomp/playtest/worker) as it goes and when it ends. The maintainer reviews
them with `python -m saturnrecomp.playtest GAME.toml ...` (__main__.py). A game opts in with a
[playtest] table in its game.toml (saturnrecomp.config).
"""
