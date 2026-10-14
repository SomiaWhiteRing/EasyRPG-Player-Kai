/* EasyRPG Player, GPL v3 or later. 3007's complete 250921 record. */
#include "game_interpreter.h"
#include "game_windows.h"
#include "maniac_text_picture.h"
#include "bitmap.h"
#include "main_data.h"
#include "output.h"
#include "utils.h"
#include "player.h"
#include <algorithm>
#include <cmath>

bool Game_Interpreter::CommandManiacShowStringPicture(const lcf::rpg::EventCommand& com) {
    using namespace ManiacTextPicture;
    if (!Player::IsPatchManiac()) return true;
    if (!maniac_text_picture_pending) {
        const auto argument = [&](size_t index) -> int32_t { return index < com.parameters.size() ? com.parameters[index] : 0; };
        const uint32_t modes = argument(0);
        const auto numeric = [&](int32_t value, unsigned mode) {
            return mode == 0 ? value : mode == 1 ? ReadVariable(value) : ReadVariableIndirect(value);
        };
        const auto mode = [&](unsigned index) { return (modes >> (index * 4)) & 15; };
        const int32_t id = numeric(argument(1), mode(0));
        if (uint32_t(id) - 1 >= uint32_t(Main_Data::game_pictures->GetMaxPictureId())) {
            Output::Error("Invalid Maniac text picture ID {}", id);
            return true;
        }
        auto operation = std::make_shared<Operation>();
        // Capture width/height first; native validates the allocated ID before
        // any of these lookups. No 23-parameter CmdSetup normalization occurs.
        operation->format.width = numeric(argument(18), mode(4));
        operation->format.height = numeric(argument(19), mode(4));
        operation->arguments.assign(com.parameters.begin(), com.parameters.end());
        const uint32_t original_flags = argument(13);
        operation->format.flags = (original_flags & 0x00ffffff) | 0x01000000;
        const int size = std::clamp(numeric(argument(20), mode(5)), 1, 255);
        operation->format.font = (uint32_t(argument(12)) << 8) | uint32_t(size);
        operation->format.spacing = argument(14);
        Game_Pictures::ShowParams params;
        params.flags = argument(17); // Missing is zero, not ShowParams' 0x61.
        params.magnify_width = numeric(argument(4), mode(2));
        params.magnify_height = mode(7) == 0 ? params.magnify_width
            : numeric(argument(23), mode(7) - 1);
        params.top_trans = numeric(argument(5), mode(3));
        params.bottom_trans = 0;
        params.red = argument(6);
        params.green = argument(7);
        params.blue = argument(8);
        params.saturation = argument(9);
        params.effect_mode = argument(10);
        int32_t numerator = params.effect_mode == 0 ? 0 : argument(11);
        double effect = numerator, rotation = 0;
        if (params.effect_mode == lcf::rpg::SavePicture::Effect_maniac_fixed_angle) {
            numerator = numeric(argument(11), (original_flags >> 24) & 15);
            const int32_t divisor = numeric(argument(21), (original_flags >> 28) & 15);
            const double quotient = double(numerator) / double(divisor);
            const double remainder = std::fmod(quotient, 360.0);
            // Native's negative rule is 360 - the signed remainder. Preserve
            // fractions and NaN; zero divisor has no replacement value.
            rotation = effect = quotient < 0 ? 360.0 - remainder : remainder;
        }
        params.effect_power = numerator;
        params.fixed_angle = rotation;
        params.maniac_style = (uint32_t(argument(12)) << 8) | uint32_t(size);
        params.maniac_image_flags = operation->format.flags;
        params.maniac_transparency_flags = operation->format.spacing;
        const uint32_t packed = argument(12);
        switch (packed & 15) {
        case 1: params.blend_mode = int(Bitmap::BlendMode::Multiply); break;
        case 2: params.blend_mode = int(Bitmap::BlendMode::Additive); break;
        case 3: params.blend_mode = int(Bitmap::BlendMode::Overlay); break;
        default: break;
        }
        const auto flip = (packed >> 4) & 15;
        params.flip_x = flip == 1 || flip == 3;
        params.flip_y = flip == 2 || flip == 3;
        params.origin = 0; // Apply native integer offsets after generation.
        params.map_layer = argument(15);
        params.battle_layer = argument(16);
        params.fixed_to_map = (original_flags & 255) != 0;
        params.use_transparent_color = (operation->format.spacing & 255) != 0;
        auto picture = Main_Data::game_pictures->GetPictureRef(id);
        picture->Erase();
        picture->data = {};
        picture->data.ID = id;
        picture->Show(params); // Direct state setup: canonical is not a filename.
        picture->data.effect_mode = params.effect_mode;
        picture->data.current_effect_power = effect;
        picture->data.finish_effect_power = numerator;
        picture->data.current_rotation = rotation;
        picture->data.maniac_image_flags = operation->format.flags;
        picture->data.maniac_transparency_flags = operation->format.spacing;
        picture->data.flags.unused_bit = (uint32_t(argument(17)) & 4) != 0;
        picture->data.flags.unused_bit2 = (uint32_t(argument(17)) & 8) != 0;
        picture->data.finish_bot_trans = SignedBits(operation->format.font);
        picture->data.current_bot_trans = 0;
        picture->data.spritesheet_cols = picture->data.spritesheet_rows = 1;
        picture->data.spritesheet_frame = picture->data.spritesheet_speed = picture->data.frames = 0;
        picture->data.current_waver = 0;
        picture->data.easyrpg_type = lcf::rpg::SavePicture::EasyRpgType_window;
        operation->picture = picture;
        operation->parsed = Parse(*this, Utils::DecodeUTF16(com.string, true), argument(22), false);
        picture->data.name = Utils::EncodeUTF(operation->parsed.canonical, true);
        maniac_text_picture_pending = operation;
        Main_Data::game_windows->CreateManiacTextPicture(id, operation);
    }
    auto operation = maniac_text_picture_pending;
    if (!operation->ready && !operation->cancelled) {
        _async_op = AsyncOp::MakeYieldRepeat();
        return true;
    }
    const auto picture = operation->picture.lock();
    if (!operation->cancelled && operation->bitmap && picture) {
        const auto argument = [&](size_t index) -> int32_t { return index < operation->arguments.size() ? operation->arguments[index] : 0; };
        const uint32_t modes = argument(0);
        const auto numeric = [&](int32_t value) {
            const uint32_t mode = (modes >> 4) & 15;
            return mode == 0 ? value : mode == 1 ? ReadVariable(value) : ReadVariableIndirect(value);
        };
        const auto offset = OriginOffset((modes >> 24) & 15, operation->bitmap->GetWidth(), operation->bitmap->GetHeight());
        // XY is read here, after asset completion and bitmap generation. The
        // stored weak object follows 3017 slot exchanges without re-reading ID.
        const int32_t x = SignedBits(uint32_t(numeric(argument(2))) + uint32_t(offset[0]));
        const int32_t y = SignedBits(uint32_t(numeric(argument(3))) + uint32_t(offset[1]));
        picture->data.start_x = picture->data.current_x = picture->data.finish_x = x;
        picture->data.start_y = picture->data.current_y = picture->data.finish_y = y;
        picture->AttachManiacTextBitmap(operation->bitmap);
    }
    operation->arguments.clear();
    maniac_text_picture_pending.reset();
    return true;
}
