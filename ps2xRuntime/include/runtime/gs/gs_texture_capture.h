#pragma once

#include <string>

// One-time captures of game textures by replacement key, for art built on top of
// the game's own texture (the HUD font sheet the tutorial prompts live in): that
// texture comes from the player's disc, so it is captured from the running game
// instead of shipping with the source. The renderer writes the decoded texture as
// a PNG to `path` the first time a texture with this key is drawn.
// keyName: "<tex0Hash>-<clutHash>-<bits>.png", as in a texture pack.
void gsTextureCaptureRequest(const std::string &keyName, const std::string &path);
