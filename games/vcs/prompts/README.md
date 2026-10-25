# Button-prompt art for Vice City Stories

One folder per prompt set (`xbox`, `steam`, `dualsense`, `dualshock`, `switch`,
`keyboard`). Each file replaces a game texture by its texture-pack key; the
`preview_*.png` files and `manifest.txt` are what the settings menu shows. The
launcher copies this folder beside the built game as `prompts/`.

Made with `games/vcs/tools/make_prompt_packs.py` from AL2009man's Gamepad
Prompt Asset Pack and Controller Asset Pack (MIT, see `LICENSE-AL2009man.txt`).

The HUD tutorial font sheet (`3ece7b57667e958b-f663bcea3bca5023-00002213.png`)
is not here: it is the game's own font texture with the glyphs drawn in, so the
runtime captures the original from the running game and draws the glyphs into
it (`ps2_prompt_art.cpp`).
