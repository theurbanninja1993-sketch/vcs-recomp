// Runtime prompt art: the button prompts follow the live bindings.
//
// See ps2_prompt_art.h. Two families are generated:
//  - keyboard: keycaps labelled with the key bound to the action each PS2
//    button serves (same labels as the shipped art: W/LMB/S/E, arrows,
//    SPACE/Q/RMB for the map icons);
//  - controller sets: the glyph of the host button that currently reaches
//    each PS2 button (hostButtonForPs2), so Classic / Modern / Custom
//    layouts - and the on-foot / in-vehicle contexts - are reflected in the
//    art. Face buttons are letter or PlayStation-symbol discs, shoulders /
//    sticks / menu buttons are pills, host D-pad inputs are arrows.
//
// The runtime cannot reuse the Python tools, so everything is drawn with
// raylib's image API and exported as PNGs. Alpha is written in the GS scale
// (0x80 = opaque) the replacement pipeline expects.
#include "runtime/gs/gs_texture_capture.h"
#include "runtime/ps2_prompt_art.h"

#include "runtime/ps2_platform.h"

#include "runtime/ps2_host_settings.h"
#include "runtime/ps2_mods.h"

#include "raylib.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
    using namespace ps2controls;

    // The prompts are runtime art, so their resolution is ours to choose. At the
    // internal scale of 3x or 4x a 64 px prompt is stretched to two or three
    // hundred pixels on screen and reads as low-res next to the rest of the UI
    // (especially with a texture pack that upscales everything else). They are
    // generated at the size the display actually needs instead; the game maps
    // them onto its own quad with its own uv, so a larger texture costs nothing
    // but memory. PS2X_PROMPT_ART_SIZE overrides it for testing.
    int promptArtSize()
    {
        static const int s_size = []()
        {
            const char *env = std::getenv("PS2X_PROMPT_ART_SIZE");
            const int size = env ? std::atoi(env) : 256;
            return size >= 32 ? size : 256;
        }();
        return s_size;
    }

    // ---------------------------------------------------------------- tables
    // Which PS2 button each shipped prompt texture shows (ps2controls order).
    struct TextureArt
    {
        const char *fileName;
        int ps2Button;
    };
    const TextureArt kTextures[] = {
        {"60a3a644467171e5-acbe9ae672fed823-00001113.png", 14}, // Cross
        {"d0d9f396c36b7411-bd20758f94447ef8-00001113.png", 12}, // Triangle
        {"a1b7cc92002291c5-b97a81b80c22e4c8-00001113.png", 4},  // D-pad Up
        {"47e8e408d74357d8-b97a81b80c22e4c8-00001113.png", 6},  // D-pad Down
        {"784c33b4a3241b19-b97a81b80c22e4c8-00001113.png", 7},  // D-pad Left
        {"3c60f292c5015937-b97a81b80c22e4c8-00001113.png", 5},  // D-pad Right
        {"c075546801b74bf1-6b7c470bee19b607-00001113.png", 15}, // map Target (Square)
        {"1c274bfc4808cd11-21fc390fe03ad8c0-00001113.png", 10}, // map Zoom L (L1)
        {"23ece69c64b71407-f6ce098a91a7ac7b-00001113.png", 11}, // map Zoom R (R1)
    };

    // The font-atlas cells the tutorial prompts live in (make_font_sheet.py).
    const char *kSheetName = "3ece7b57667e958b-f663bcea3bca5023-00002213.png";
    struct SheetCell
    {
        int x, y, w, h;
        int ps2Button;
    };
    const SheetCell kSheetCells[] = {
        {0, 230, 24, 22, 15},   // square
        {24, 230, 22, 22, 14},  // cross
        {46, 230, 22, 22, 13},  // circle
        {212, 206, 22, 22, 12}, // triangle
    };

    // Keyboard: the action whose bound key each PS2 button's keycap shows.
    int keyboardActionFor(int ps2Button)
    {
        // In the game's menus the keyboard uses the fixed menu actions, not
        // the gameplay bindings.
        if (ps2controls::currentContext() == ps2controls::Menus)
        {
            switch (ps2Button)
            {
            case 14: return MenuAccept; // Cross
            case 13:
            case 12: return MenuBack; // Circle / Triangle
            case 4: return MenuUp;
            case 6: return MenuDown;
            case 7: return MenuLeft;
            case 5: return MenuRight;
            default: break;
            }
        }
        switch (ps2Button)
        {
        case 14: return MoveForward; // Cross
        case 13: return Fire; // Circle
        case 12: return EnterVehicle; // Triangle
        case 15: return MoveBackward; // Square
        case 10: return Interact; // L1
        case 11: return Aim; // R1
        case 8:
        case 9: return NextWeapon;
        case 0: return ps2controls::CameraMode; // (raylib also names a type CameraMode)
        case 4: return MenuUp;
        case 6: return MenuDown;
        case 7: return MenuLeft;
        case 5: return MenuRight;
        default: return kNone;
        }
    }

    // The context the art is drawn for: the pad's prediction on a menu
    // toggle, or the real pad context (-1 = follow it).
    std::atomic<int> g_artContext{-1};

    int artContextValue()
    {
        const int c = g_artContext.load(std::memory_order_relaxed);
        return c >= 0 ? c : currentContext();
    }

    // ------------------------------------------------------- controller sets
    struct SetStyle
    {
        bool psSymbols;          // faces are the PlayStation shapes
        bool whiteStyle;         // DualSense: white disc with gray symbol
        Color ring;              // 0 alpha = no ring
        const char *face[4];     // PadA..PadY labels
        Color faceColor[4];
        const char *shoulder[4]; // PadLB, PadRB, PadLT, PadRT
        const char *stick[2];    // PadLS, PadRS
        const char *back;
        const char *start;
    };
    const SetStyle kSetXbox = {
        false, false, {0, 0, 0, 0},
        {"A", "B", "X", "Y"},
        {{16, 124, 16, 255}, {211, 47, 47, 255}, {0, 120, 215, 255}, {242, 196, 0, 255}},
        {"LB", "RB", "LT", "RT"}, {"LS", "RS"}, "VIEW", "MENU"};
    const SetStyle kSetSteam = {
        false, false, {102, 192, 244, 255},
        {"A", "B", "X", "Y"},
        {{245, 245, 245, 255}, {245, 245, 245, 255}, {245, 245, 245, 255}, {245, 245, 245, 255}},
        {"LB", "RB", "LT", "RT"}, {"LS", "RS"}, "VIEW", "MENU"};
    const SetStyle kSetSwitch = {
        false, false, {230, 0, 18, 255},
        {"B", "A", "Y", "X"},
        {{245, 245, 245, 255}, {245, 245, 245, 255}, {245, 245, 245, 255}, {245, 245, 245, 255}},
        {"L", "R", "ZL", "ZR"}, {"LS", "RS"}, "MINUS", "PLUS"};
    const SetStyle kSetDualsense = {
        true, true, {0, 0, 0, 0},
        {nullptr, nullptr, nullptr, nullptr}, {},
        {"L1", "R1", "L2", "R2"}, {"L3", "R3"}, "CREATE", "OPTIONS"};
    const SetStyle kSetDualshock = {
        true, false, {0, 0, 0, 0},
        {nullptr, nullptr, nullptr, nullptr}, {},
        {"L1", "R1", "L2", "R2"}, {"L3", "R3"}, "SHARE", "OPTIONS"};

    const SetStyle *setStyle(const std::string &set)
    {
        if (set == "xbox")
            return &kSetXbox;
        if (set == "steam")
            return &kSetSteam;
        if (set == "switch")
            return &kSetSwitch;
        if (set == "dualsense")
            return &kSetDualsense;
        if (set == "dualshock")
            return &kSetDualshock;
        return nullptr;
    }

    // The pack's PlayStation symbol colours (sampled from the Color SVGs):
    // cross blue, circle red, square pink, triangle teal.
    const Color kPsSymbolColor[4] = {{118, 126, 225, 255}, {247, 95, 102, 255}, {246, 157, 200, 255}, {107, 228, 223, 255}};
    const Color kDiscDark = {28, 28, 32, 255};
    const Color kPillFill = {32, 38, 46, 255};
    const Color kPillRim = {138, 148, 166, 255};
    const Color kLabelLight = {232, 236, 242, 255};
    const Color kArrowColor = {245, 245, 245, 255};

    // ------------------------------------------------------------- utilities
    const Font &promptFont()
    {
        static const Font s_font = []()
        {
            static const char *kPaths[] = {
                "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
                "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                "/usr/share/fonts/TTF/DejaVuSans.ttf",
                "C:/Windows/Fonts/segoeuib.ttf",
                "C:/Windows/Fonts/arialbd.ttf",
                "C:/Windows/Fonts/segoeui.ttf",
            };
            Font none{};
            if (!IsWindowReady())
                return none; // raylib not initialised yet
            // The default set is ASCII only; the d-pad keycaps want arrows.
            int codepoints[99];
            int count = 0;
            for (int c = 32; c <= 126; ++c)
                codepoints[count++] = c;
            for (int c = 0x2190; c <= 0x2193; ++c)
                codepoints[count++] = c;
            for (const char *path : kPaths)
                if (FileExists(path))
                {
                    Font font = LoadFontEx(path, promptArtSize(), codepoints, count);
                    if (font.texture.id != 0)
                        return font;
                }
            return none;
        }();
        return s_font;
    }

    // rounded rectangle = inner rectangles + four corner circles
    void roundedRect(Image *img, Rectangle rec, float radius, Color color)
    {
        ImageDrawRectangleRec(img, (Rectangle){rec.x + radius, rec.y, rec.width - 2 * radius, rec.height}, color);
        ImageDrawRectangleRec(img, (Rectangle){rec.x, rec.y + radius, rec.width, rec.height - 2 * radius}, color);
        ImageDrawCircleV(img, (Vector2){rec.x + radius, rec.y + radius}, radius, color);
        ImageDrawCircleV(img, (Vector2){rec.x + rec.width - radius, rec.y + radius}, radius, color);
        ImageDrawCircleV(img, (Vector2){rec.x + radius, rec.y + rec.height - radius}, radius, color);
        ImageDrawCircleV(img, (Vector2){rec.x + rec.width - radius, rec.y + rec.height - radius}, radius, color);
    }

    void drawCenteredText(Image *img, const Font &font, const char *text, float fontSize, int cx, int cy, float maxWidth,
                          Color color)
    {
        if (font.texture.id == 0 || text == nullptr || *text == 0)
            return;
        float fs = fontSize;
        Vector2 m = MeasureTextEx(font, text, fs, 0);
        if (m.x > maxWidth && m.x > 0.0f)
        {
            fs *= maxWidth / m.x;
            m = MeasureTextEx(font, text, fs, 0);
        }
        ImageDrawTextEx(img, font, text, (Vector2){cx - m.x / 2, cy - m.y / 2}, fs, 0, color);
    }

    Image makeDiscRaw(int big, Color fill, Color ring)
    {
        Image img = GenImageColor(big, big, BLANK);
        const float c = big * 0.5f;
        const float r = big * 0.47f;
        if (ring.a != 0)
            ImageDrawCircleV(&img, (Vector2){c, c}, r, ring);
        ImageDrawCircleV(&img, (Vector2){c, c}, ring.a != 0 ? r - std::max(2.0f, big / 12.0f) : r, fill);
        return img;
    }

    Image makeLabelDisc(const SetStyle &style, const char *label, Color color, int size, const Font &font)
    {
        const int big = size * 4;
        Image img = makeDiscRaw(big, kDiscDark, style.ring);
        drawCenteredText(&img, font, label, big * 0.5f, big / 2, big / 2, big * 0.6f, color);
        ImageResize(&img, size, size);
        return img;
    }

    Image makeKeycap(const std::string &label, int size, const Font &font)
    {
        Image img = GenImageColor(size, size, BLANK);
        const float pad = static_cast<float>(std::max(2, size / 16));
        const float rim = static_cast<float>(std::max(2, size / 16));
        // Two-word labels ("MOUSE WHEEL") stack so each line stays legible.
        std::string line1 = label;
        std::string line2;
        const size_t space = label.find(' ');
        if (space != std::string::npos && label.size() > 6)
        {
            line1 = label.substr(0, space);
            line2 = label.substr(space + 1);
        }
        // Long labels ("ENTER", "BKSP", "RMB") read better on a wide key, like
        // a real Enter / Backspace key: the key uses the full tile width and
        // the label gets the key's full height instead of a square's.
        const bool wide = line2.empty() && line1.size() >= 3;
        const float keyH = wide ? size * 0.58f : static_cast<float>(size) - 2 * pad - 1;
        const float keyY = (size - keyH) * 0.5f;
        const Rectangle rec = {pad, keyY, static_cast<float>(size) - 2 * pad - 1, keyH};
        roundedRect(&img, rec, wide ? keyH * 0.3f : rec.width * 0.22f, kPillRim);
        const Rectangle inner = {rec.x + rim, rec.y + rim, rec.width - 2 * rim, rec.height - 2 * rim};
        roundedRect(&img, inner, wide ? inner.height * 0.3f : inner.width * 0.2f, kPillFill);
        if (!line2.empty())
        {
            float fs = inner.height * 0.5f;
            for (const std::string &line : {line1, line2})
            {
                const Vector2 m = MeasureTextEx(font, line.c_str(), fs, 0);
                if (m.x > inner.width * 0.9f && m.x > 0.0f)
                    fs *= inner.width * 0.9f / m.x;
            }
            drawCenteredText(&img, font, line1.c_str(), fs, size / 2, static_cast<int>(size * 0.5f - fs * 0.55f),
                             inner.width * 0.9f, kLabelLight);
            drawCenteredText(&img, font, line2.c_str(), fs, size / 2, static_cast<int>(size * 0.5f + fs * 0.55f),
                             inner.width * 0.9f, kLabelLight);
        }
        else
        {
            drawCenteredText(&img, font, line1.c_str(), wide ? inner.height * 1.2f : size * 0.5f, size / 2, size / 2,
                             inner.width * (wide ? 0.96f : 0.8f), kLabelLight);
        }
        return img;
    }

    // fill: the fraction of the canvas height the pill occupies. The flat
    // 0.44 band is the shoulder-button look for the map icons; the font-atlas
    // cells pass a taller fill so the label stays readable in the tutorials.
    Image makePill(const char *label, int size, const Font &font, float fill = 0.44f)
    {
        Image img = GenImageColor(size, size, BLANK);
        const float pad = static_cast<float>(std::max(2, size / 16));
        const float rim = static_cast<float>(std::max(2, size / 16));
        const float top = size * (1.0f - fill) * 0.5f;
        const float bot = size - top - 1;
        const Rectangle rec = {pad, top, size - 2 * pad - 1, bot - top};
        roundedRect(&img, rec, (bot - top) * 0.5f, kPillRim);
        const Rectangle inner = {rec.x + rim, rec.y + rim, rec.width - 2 * rim, rec.height - 2 * rim};
        roundedRect(&img, inner, inner.height * 0.45f, kPillFill);
        drawCenteredText(&img, font, label, (bot - top) * 0.62f, size / 2, size / 2, inner.width * 0.72f, kLabelLight);
        return img;
    }

    Image makeArrow(int host, int size)
    {
        const int big = size * 4;
        Image img = GenImageColor(big, big, BLANK);
        const float c = big * 0.5f;
        const float head = big * 0.30f; // half-size of the arrow head
        const float stem = big * 0.09f;
        const float len = big * 0.34f;
        switch (host)
        {
        case PadUp:
            ImageDrawTriangle(&img, (Vector2){c, c - len}, (Vector2){c - head, c - len + head},
                              (Vector2){c + head, c - len + head}, kArrowColor);
            ImageDrawRectangleRec(&img, (Rectangle){c - stem / 2, c - len + head, stem, len + head}, kArrowColor);
            break;
        case PadDown:
            ImageDrawTriangle(&img, (Vector2){c, c + len}, (Vector2){c - head, c + len - head},
                              (Vector2){c + head, c + len - head}, kArrowColor);
            ImageDrawRectangleRec(&img, (Rectangle){c - stem / 2, c - len, stem, len + head}, kArrowColor);
            break;
        case PadLeft:
            ImageDrawTriangle(&img, (Vector2){c - len, c}, (Vector2){c - len + head, c - head},
                              (Vector2){c - len + head, c + head}, kArrowColor);
            ImageDrawRectangleRec(&img, (Rectangle){c - len + head, c - stem / 2, len + head, stem}, kArrowColor);
            break;
        default:
            ImageDrawTriangle(&img, (Vector2){c + len, c}, (Vector2){c + len - head, c - head},
                              (Vector2){c + len - head, c + head}, kArrowColor);
            ImageDrawRectangleRec(&img, (Rectangle){c - len, c - stem / 2, len + head, stem}, kArrowColor);
            break;
        }
        ImageResize(&img, size, size);
        return img;
    }

    Image makePsSymbolDisc(const SetStyle &style, int symbol, int size)
    {
        const int big = size * 4;
        const Color fill = style.whiteStyle ? Color{255, 255, 255, 255} : Color{51, 51, 51, 255};
        const Color sym = style.whiteStyle ? Color{128, 128, 128, 255} : kPsSymbolColor[symbol];
        Image img = makeDiscRaw(big, fill, {0, 0, 0, 0});
        const float c = big * 0.5f;
        const float t = big * 0.11f; // symbol stroke
        const float r = big * 0.27f; // symbol extent
        switch (symbol)
        {
        case 0: // cross
        {
            const float d = r * 0.78f;
            ImageDrawLineEx(&img, (Vector2){c - d, c - d}, (Vector2){c + d, c + d}, static_cast<int>(t), sym);
            ImageDrawLineEx(&img, (Vector2){c - d, c + d}, (Vector2){c + d, c - d}, static_cast<int>(t), sym);
            break;
        }
        case 1: // circle
            ImageDrawCircleV(&img, (Vector2){c, c}, r, sym);
            ImageDrawCircleV(&img, (Vector2){c, c}, r - t, fill);
            break;
        case 2: // square
        {
            const float s = r * 1.8f;
            ImageDrawRectangleRec(&img, (Rectangle){c - s / 2, c - s / 2, s, s}, sym);
            ImageDrawRectangleRec(&img, (Rectangle){c - s / 2 + t, c - s / 2 + t, s - 2 * t, s - 2 * t}, fill);
            break;
        }
        default: // triangle
        {
            const Vector2 a = {c, c - r * 1.15f}, b = {c - r, c + r * 0.85f}, d = {c + r, c + r * 0.85f};
            ImageDrawTriangle(&img, a, b, d, sym);
            const Vector2 g = {(a.x + b.x + d.x) / 3, (a.y + b.y + d.y) / 3};
            const float f = std::min(0.6f, t / (r * 0.7f));
            auto toward = [&](Vector2 p)
            { return Vector2{p.x + (g.x - p.x) * f, p.y + (g.y - p.y) * f}; };
            ImageDrawTriangle(&img, toward(a), toward(b), toward(d), fill);
            break;
        }
        }
        ImageResize(&img, size, size);
        return img;
    }

    Image makeHostGlyph(const SetStyle &style, int host, int size, const Font &font, float pillFill = 0.44f)
    {
        switch (host)
        {
        case PadA:
        case PadB:
        case PadX:
        case PadY:
        {
            const int i = host - PadA;
            if (style.psSymbols)
                return makePsSymbolDisc(style, i, size);
            return makeLabelDisc(style, style.face[i], style.faceColor[i], size, font);
        }
        case PadLB:
        case PadRB:
        case PadLT:
        case PadRT:
            return makePill(style.shoulder[host - PadLB], size, font, pillFill);
        case PadBack:
            return makePill(style.back, size, font, pillFill);
        case PadStart:
            return makePill(style.start, size, font, pillFill);
        case PadLS:
        case PadRS:
            return makeLabelDisc(style, style.stick[host - PadLS], {245, 245, 245, 255}, size, font);
        default: // host D-pad directions
            return makeArrow(host, size);
        }
    }

    std::string bindingLabel(int action)
    {
        const auto &keys = ps2HostSettings().keys[action];
        const int binding = keys[0] != kNone ? keys[0] : keys[1];
        switch (binding)
        {
        case KEY_UP: return "\u2191"; // the d-pad keycaps use arrows, like the
        case KEY_DOWN: return "\u2193"; // shipped art (promptBindingName spells
        case KEY_LEFT: return "\u2190"; // them out for menu text instead)
        case KEY_RIGHT: return "\u2192";
        default: break;
        }
        std::string s = promptBindingName(binding);
        return s.empty() ? std::string("?") : s;
    }

    // 0..255 authored alpha -> the GS scale the replacement pipeline uses.
    void toGsAlpha(Image &img)
    {
        if (img.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
            ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        unsigned char *p = static_cast<unsigned char *>(img.data);
        for (int i = 0; i < img.width * img.height; ++i, p += 4)
            p[3] = static_cast<unsigned char>((p[3] * 128 + 127) / 255);
    }

    void mix(uint64_t &h, int v)
    {
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(v));
        h *= 1099511628211ull;
    }

    // The effective content signature: only changes when the drawn art would
    // change (Classic layouts hash the same in both pad contexts, so riding a
    // bike does not rewrite anything).
    uint64_t effectiveSignature(const std::string &set)
    {
        uint64_t h = 1469598103934665603ull;
        for (char c : set)
            mix(h, c);
        mix(h, static_cast<uint64_t>(promptArtSize())); // a size change must rewrite the art
        if (set == "keyboard")
        {
            for (const TextureArt &t : kTextures)
                for (int i = 0; i < 2; ++i)
                    mix(h, ps2HostSettings().keys[keyboardActionFor(t.ps2Button)][i]);
            for (const SheetCell &cell : kSheetCells)
                for (int i = 0; i < 2; ++i)
                    mix(h, ps2HostSettings().keys[keyboardActionFor(cell.ps2Button)][i]);
            return h;
        }
        const int context = artContextValue();
        for (const TextureArt &t : kTextures)
            mix(h, hostButtonForPs2(t.ps2Button, context));
        for (const SheetCell &cell : kSheetCells)
            mix(h, hostButtonForPs2(cell.ps2Button, context));
        return h;
    }
} // namespace

void ps2PromptArtSetContext(int context)
{
    g_artContext.store(context, std::memory_order_relaxed);
}

std::string ps2PromptArtDirectory()
{
    return (ps2ConfigDirectory() / "prompts_rt").string();
}

bool ps2PromptArtRefreshIfNeeded()
{
    const std::string set = ps2CurrentPromptSet();
    if (set.empty())
        return false; // Original: nothing to do
    const Font &font = promptFont();
    if (font.texture.id == 0)
        return false; // no usable font: the shipped art stays in use
    const bool keyboard = set == "keyboard";
    const SetStyle *style = keyboard ? nullptr : setStyle(set);
    if (!keyboard && style == nullptr)
        return false;

    static uint64_t s_appliedSig = 0;
    static std::string s_appliedSet;
    static int s_appliedContext = -1;
    static uint64_t s_pendingSig = 0;
    static std::string s_pendingSet;
    static int s_pendingFrames = 0;

    const int context = artContextValue();
    uint64_t sig = effectiveSignature(set);
    // The HUD tutorial sheet is drawn over the game's own font texture, captured
    // from the running game (it comes from the disc, so it cannot ship with the
    // source). Until the capture lands the sheet is skipped; when it does, the
    // art is written again.
    const fs::path capturedSheet = fs::path(ps2PromptArtDirectory()) / "_base" / kSheetName;
    static bool s_sheetBaseSeen = false;
    {
        std::error_code ec;
        const bool haveBase = fs::exists(fs::path(ps2PromptsDirectory()) / set / kSheetName, ec) ||
                              fs::exists(capturedSheet, ec);
        if (!haveBase)
        {
            static bool s_requested = false;
            if (!s_requested)
            {
                s_requested = true;
                gsTextureCaptureRequest(kSheetName, capturedSheet.string());
            }
        }
        else if (!s_sheetBaseSeen)
        {
            s_sheetBaseSeen = true;
            s_appliedSig = 0; // write the art again, now with the sheet
        }
        sig ^= s_sheetBaseSeen ? 0x5EE7ull : 0u;
    }
    if (sig == s_appliedSig && set == s_appliedSet)
    {
        s_pendingFrames = 0;
        return false;
    }
    // Debounce: bindings and the pad context can settle a moment after a
    // change (settings UI, vehicle entry); rewrite once the state is stable.
    // The menu context is a deliberate, stable change (the pause flag), so
    // entering or leaving the game's menus rewrites on the very first frame
    // that sees it - otherwise the map shows the gameplay keys for a moment.
    const int debounce = (context == ps2controls::Menus || s_appliedContext == ps2controls::Menus) ? 1 : 45;
    if (sig != s_pendingSig || set != s_pendingSet)
    {
        s_pendingSig = sig;
        s_pendingSet = set;
        s_pendingFrames = 0;
    }
    if (++s_pendingFrames < debounce)
        return false;

    std::error_code ec;
    const fs::path outDir = fs::path(ps2PromptArtDirectory()) / set;
    fs::create_directories(outDir, ec);
    // The folder is authoritative: remove stale files so dropped mappings
    // fall back to the shipped art instead of an outdated override.
    for (const fs::directory_entry &entry : fs::directory_iterator(outDir, ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".png")
            fs::remove(entry.path(), ec);

    auto glyphFor = [&](int ps2Button, int size, bool cell = false) -> Image
    {
        if (keyboard)
        {
            // The map zoom is the mouse wheel for the keyboard set: one prompt
            // on the first icon, nothing on the second (the pair collapses).
            if (ps2Button == 10)
                return makeKeycap("MOUSE WHEEL", size, font);
            if (ps2Button == 11)
                return GenImageColor(size, size, BLANK);
            return makeKeycap(bindingLabel(keyboardActionFor(ps2Button)), size, font);
        }
        const int host = hostButtonForPs2(ps2Button, context);
        if (host == kNone)
        {
            Image none{};
            return none; // keep the shipped art for this button
        }
        // The font-atlas cells are drawn small in the HUD tutorials, so a pill
        // that lands there (a shoulder bound to a face button) fills the cell
        // instead of keeping the flat map-icon band.
        return makeHostGlyph(*style, host, size, font, cell ? 0.8f : 0.44f);
    };

    for (const TextureArt &t : kTextures)
    {
        Image img = glyphFor(t.ps2Button, promptArtSize());
        if (img.data == nullptr)
            continue;
        toGsAlpha(img);
        ExportImage(img, (outDir / t.fileName).string().c_str());
        UnloadImage(img);
    }

    fs::path shipped = fs::path(ps2PromptsDirectory()) / set / kSheetName;
    {
        std::error_code ec;
        if (!fs::exists(shipped, ec))
            shipped = capturedSheet; // the game's own, captured while playing
    }
    if (Image sheet = LoadImage(shipped.string().c_str()); sheet.data != nullptr)
    {
        for (const SheetCell &cell : kSheetCells)
        {
            Image cap = glyphFor(cell.ps2Button, 40, true);
            if (cap.data == nullptr)
                continue; // leave the cell as shipped
            ImageDrawRectangleRec(&sheet, {(float)cell.x, (float)cell.y, (float)cell.w, (float)cell.h}, BLANK);
            ImageResize(&cap, cell.w, cell.h);
            toGsAlpha(cap);
            ImageDraw(&sheet, cap, {0, 0, (float)cap.width, (float)cap.height},
                      {(float)cell.x, (float)cell.y, (float)cell.w, (float)cell.h}, WHITE);
            UnloadImage(cap);
        }
        ExportImage(sheet, (outDir / kSheetName).string().c_str());
        UnloadImage(sheet);
    }

    s_appliedSig = sig;
    s_appliedSet = set;
    s_appliedContext = context;
    s_pendingFrames = 0;
    if (std::getenv("PS2X_PROMPT_TRACE") != nullptr)
        std::cerr << "[prompts] runtime art written for " << set << " (" << outDir.string() << ")" << std::endl;
    return true;
}
