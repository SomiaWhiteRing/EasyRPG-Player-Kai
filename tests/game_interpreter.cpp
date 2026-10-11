#include <string>
#include <sstream>
#include <fstream>
#include <utility>
#include <vector>
#include <cstdlib>
#include <filesystem>

#include "doctest.h"
#include "cmdline_parser.h"
#include "game_interpreter.h"
#include "game_interpreter_map.h"
#include "game_interpreter_battle.h"
#include "filefinder.h"
#include "game_maniac_self_variables.h"
#include "maniac_original_save.h"
#include "game_strings.h"
#include "game_system.h"
#include "game_actor.h"
#include "game_actors.h"
#include "window_message.h"
#include "bitmap.h"
#include "baseui.h"
#include "maniac_patch.h"
#include <lcf/lsd/reader.h>
#include <lcf/lmt/reader.h>
#include <lcf/lmu/reader.h>
#include <lcf/ldb/reader.h>
#include "graphics.h"
#include "game_clock.h"
#include "input_source.h"
#include "main_data.h"
#include "mock_game.h"
#include "output.h"
#include "player.h"
#include "scene_save.h"
#include "game_targets.h"
#include "game_windows.h"
#include "game_dynrpg.h"

namespace {

using CapturedLog = std::pair<LogLevel, std::string>;

void CaptureLog(LogLevel lvl, std::string const& message, LogCallbackUserData userdata) {
	auto* logs = static_cast<std::vector<CapturedLog>*>(userdata);
	logs->emplace_back(lvl, message);
}

} // namespace

TEST_SUITE_BEGIN("Game_Interpreter");

TEST_CASE("Maniac global save copies UTF16 strings and short close discards cached banks") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	const auto previous_version = Player::game_config.maniac_version.Get();
	const bool previous_opened = Main_Data::global_save_opened;
	auto previous_switches = std::move(Main_Data::game_switches_global);
	auto previous_variables = std::move(Main_Data::game_variables_global);
	auto previous_strings = std::move(Main_Data::game_strings_global);
	auto previous_self = std::move(Main_Data::game_maniac_self_variables);
	Player::game_config.patch_maniac.Set(1);
	Player::game_config.maniac_version.Set(250921);
	Main_Data::game_switches_global = std::make_unique<Game_Switches>();
	Main_Data::game_variables_global = std::make_unique<Game_Variables>(Game_Variables::min_2k3, Game_Variables::max_2k3);
	Main_Data::game_strings_global = std::make_unique<Game_Strings>();
	Main_Data::game_strings = std::make_unique<Game_Strings>();
	Main_Data::global_save_opened = true;
	const std::string text = "\xe7\x95\x8c\xf0\x9f\x98\x80";
	Main_Data::game_strings->SetManiacRawSlot(10, text, true);
	lcf::rpg::EventCommand command;
	command.code = 3016;
	command.parameters = {5, 0, 2, 10, 1, 1};
	Game_Interpreter interpreter;
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_strings_global->Get(1) == text);
	Main_Data::game_strings->SetManiacRawSlot(10, "CLEARED", true);
	command.parameters = {4, 0, 2, 11, 1, 1};
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_strings->Get(11) == text);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
	bank->numbers.resize(12);
	bank->numbers[10] = 2000000000;
	bank->numbers[11] = -2000000000;
	bank->switches.resize(12);
	bank->switches[10] = 128;
	bank->switches[11] = 2;
	bank->strings.resize(12);
	bank->strings[10] = lcf::DBString(text);
	bank->strings[11] = lcf::DBString("SECOND");
	interpreter.Push({command}, 1);
	for (int type = 0; type < 3; ++type) {
		command.parameters = {5, 0, type, -10, 1, 2};
		CHECK(interpreter.ExecuteCommand(command));
		command.parameters = {4, 0, type, -20, 1, 2};
		CHECK(interpreter.ExecuteCommand(command));
	}
	CHECK(bank->numbers[20] == 2000000000);
	CHECK(bank->numbers[21] == -2000000000);
	CHECK(bank->switches[20] == 128);
	CHECK(bank->switches[21] == 2);
	CHECK(ToString(bank->strings[20]) == text);
	CHECK(ToString(bank->strings[21]) == "SECOND");
	// Raw int32 copies bypass the deliberately narrower configured ordinary bounds.
	command.parameters = {4, 0, 1, 40, 1, 2};
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(40) == 2000000000);
	CHECK(Main_Data::game_variables->Get(41) == -2000000000);
	command.parameters = {4, 0, 0, 40, 1, 2};
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_switches->GetInt(40) == 128);
	CHECK(Main_Data::game_switches->GetInt(41) == 2);
	Main_Data::game_switches_global->Set(1, true);
	Main_Data::game_variables_global->Set(1, 7);
	command.parameters = {1}; // Normal editor records for open/close have one argument.
	CHECK(interpreter.ExecuteCommand(command));
	CHECK_FALSE(Main_Data::global_save_opened);
	CHECK(Main_Data::game_strings_global->GetData().empty());
	CHECK(Main_Data::game_switches_global->GetData().empty());
	CHECK(Main_Data::game_variables_global->GetData().empty());
	Main_Data::game_switches_global = std::move(previous_switches);
	Main_Data::game_variables_global = std::move(previous_variables);
	Main_Data::game_strings_global = std::move(previous_strings);
	Main_Data::game_maniac_self_variables = std::move(previous_self);
	Main_Data::global_save_opened = previous_opened;
	Player::game_config.patch_maniac.Set(previous_patch);
	Player::game_config.maniac_version.Set(previous_version);
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 map range reads row-major GLOBAL and signed SELF sources") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	const auto previous_version = Player::game_config.maniac_version.Get();
	Player::game_config.patch_maniac.Set(1);
	Player::game_config.maniac_version.Set(250921);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	Main_Data::game_variables->SetData({0, 1, 2, 3, 4});
	auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
	REQUIRE(bank != nullptr);
	bank->numbers = {0, 3, 0, 18, 19, 20, 21};
	lcf::rpg::EventCommand rewrite;
	rewrite.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_RewriteMap);
	rewrite.parameters = {0, 1, 1, 2, 2, 3, 2, 2, 1};
	Game_Interpreter interpreter;
	interpreter.Push({rewrite}, 1);
	REQUIRE(interpreter.ExecuteCommand(rewrite));
	CHECK(Game_Map::GetTilesIdAt(Rect(2, 3, 2, 2), 1) == std::vector<int>{1, 2, 3, 4});
	rewrite.parameters = {1, 1, 0, -1, 5, 3, 2, 2, 1};
	REQUIRE(interpreter.ExecuteCommand(rewrite));
	CHECK(Game_Map::GetTilesIdAt(Rect(5, 3, 2, 2), 0) == std::vector<int>{18, 19, 20, 21});
	Main_Data::game_variables->Set(10, 4);
	rewrite.parameters = {0, 1, 0, 10, 9, 5, 1, 1, 1};
	REQUIRE(interpreter.ExecuteCommand(rewrite));
	CHECK(Game_Map::GetMapDataDown()[5 * 20 + 9] == 3074); // Fixed 250921 lower table, not generic 3050.
	Player::game_config.patch_maniac.Set(previous_patch);
	Player::game_config.maniac_version.Set(previous_version);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 single lower direct writes raw startY and selects layer bit zero") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	const auto previous_version = Player::game_config.maniac_version.Get();
	Player::game_config.patch_maniac.Set(1);
	Player::game_config.maniac_version.Set(250921);
	lcf::rpg::EventCommand rewrite;
	rewrite.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_RewriteMap);
	rewrite.parameters = {0, 0, 0, 21, 2, 3, 1, 1, 1};
	Game_Interpreter interpreter;
	REQUIRE(interpreter.ExecuteCommand(rewrite));
	CHECK(Game_Map::GetMapDataDown()[3 * 20 + 2] == 3);
	rewrite.parameters = {0, 0, 2, 20, 5, 4, 1, 1, 1};
	REQUIRE(interpreter.ExecuteCommand(rewrite));
	CHECK(Game_Map::GetMapDataDown()[4 * 20 + 5] == 4);
	CHECK(Game_Map::GetMapDataUp()[4 * 20 + 5] == BLOCK_F);
	rewrite.parameters = {0, 0, 1, 143, 8, 5, 1, 1, 1};
	REQUIRE(interpreter.ExecuteCommand(rewrite));
	CHECK(Game_Map::GetMapDataUp()[5 * 20 + 8] == BLOCK_F + 143);
	Player::game_config.patch_maniac.Set(previous_patch);
	Player::game_config.maniac_version.Set(previous_version);
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 actor info uses bit zero numeric subject and signed string scalar outputs") {
	lcf::Data::actors.resize(2);
	for (size_t i = 0; i < lcf::Data::actors.size(); ++i) {
		lcf::Data::actors[i].ID = static_cast<int>(i + 1);
		lcf::Data::actors[i].Setup(true);
	}
	lcf::Data::actors[1].face_name = lcf::DBString("base_face2");
	lcf::Data::actors[1].face_index = 3;
	lcf::Data::actors[1].character_name = lcf::DBString("base_body2");
	lcf::Data::actors[1].character_index = 2;
	Graphics::Init();
	MockGame game(MockMap::eNone);
	lcf::Data::chipsets[0].ID = 1; // Mock chipset defaults to ID0; this test uses an explicit valid ID1.
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	const auto previous_version = Player::game_config.maniac_version.Get();
	Player::game_config.patch_maniac.Set(1);
	Player::game_config.maniac_version.Set(250921);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	Main_Data::game_strings = std::make_unique<Game_Strings>();
	Main_Data::game_variables->Set(6, 2);
	Main_Data::game_actors->GetActor(2)->SetFace("dyn_face2", 5);
	Main_Data::game_actors->GetActor(2)->SetSprite("dyn_body2", 7, true);
	lcf::rpg::EventCommand info;
	info.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_GetGameInfo);
	info.parameters = {1, 6, -10, -20, 0, 6, 3};
	Game_Interpreter interpreter;
	interpreter.Push({info}, 1);
	REQUIRE(interpreter.ExecuteCommand(info));
	info.parameters = {1, 7, -11, -21, 0, 6, 3};
	REQUIRE(interpreter.ExecuteCommand(info));
	info.parameters = {0, 5, -22};
	REQUIRE(interpreter.ExecuteCommand(info));
	const auto* bank = Main_Data::game_maniac_self_variables->Find(2, 1, 1);
	REQUIRE(bank != nullptr);
	REQUIRE(bank->strings.size() >= 12);
	CHECK(ToString(bank->strings[10]) == "dyn_face2");
	CHECK(ToString(bank->strings[11]) == "dyn_body2");
	REQUIRE(bank->numbers.size() >= 23);
	CHECK(std::vector<int>(bank->numbers.begin() + 20, bank->numbers.begin() + 23) == std::vector<int>{5, 7, 1});
	info.parameters = {0, 6, 10, 20, 0, 2, 2};
	REQUIRE(interpreter.ExecuteCommand(info));
	CHECK(ToString(Main_Data::game_strings->Get(10)) == "base_face2");
	CHECK(Main_Data::game_variables->Get(20) == 3);
	info.parameters = {0, 7, 11, 21, 0, 2, 2};
	REQUIRE(interpreter.ExecuteCommand(info));
	CHECK(ToString(Main_Data::game_strings->Get(11)) == "base_body2");
	CHECK(Main_Data::game_variables->Get(21) == 2);
	Main_Data::game_strings->SetManiacRawSlot(12, "stale");
	Main_Data::game_variables->Set(22, 99);
	info.parameters = {0, 7, 12, 22, 99, 2, 3};
	REQUIRE(interpreter.ExecuteCommand(info));
	CHECK(ToString(Main_Data::game_strings->Get(12)).empty());
	CHECK(Main_Data::game_variables->Get(22) == 0);
	*Main_Data::game_variables->GetManiacWritable(0) = 777;
	info.parameters = {0, 5, 0};
	REQUIRE(interpreter.ExecuteCommand(info));
	CHECK(Main_Data::game_variables->GetManiacRawSlot(0) == 777);
	Player::game_config.patch_maniac.Set(previous_patch);
	Player::game_config.maniac_version.Set(previous_version);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 BGM keeps raw settings and independently skips zero destinations") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	const auto previous_version = Player::game_config.maniac_version.Get();
	const auto previous_no_audio = Player::no_audio_flag;
	Player::no_audio_flag = true;
	Player::game_config.patch_maniac.Set(1);
	Player::game_config.maniac_version.Set(250921);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	Main_Data::game_strings = std::make_unique<Game_Strings>();
	Main_Data::game_strings->SetManiacRawSlot(0, "zero");
	Main_Data::game_variables->SetData({91, 92, 93});
	lcf::rpg::Music music;
	music.name = "(OFF)";
	music.fadein = 12345;
	music.volume = 127;
	music.tempo = 450;
	music.balance = -7;
	Main_Data::game_system->BgmPlay(music);
	Main_Data::game_system->BgmPlay(Main_Data::game_system->GetCurrentBGM()); // Load may pass this same object.
	CHECK(Main_Data::game_system->GetCurrentBGM() == music);
	lcf::rpg::EventCommand info;
	info.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_GetGameInfo);
	info.parameters = {0, 10, -10, -20};
	Game_Interpreter interpreter;
	interpreter.Push({info}, 1);
	REQUIRE(interpreter.ExecuteCommand(info));
	const auto* bank = Main_Data::game_maniac_self_variables->Find(2, 1, 1);
	REQUIRE(bank != nullptr);
	REQUIRE(bank->strings.size() > 10);
	CHECK(ToString(bank->strings[10]) == "(OFF)");
	REQUIRE(bank->numbers.size() >= 24);
	CHECK(std::vector<int>(bank->numbers.begin() + 20, bank->numbers.begin() + 24) == std::vector<int>{12345, 127, 450, -7});
	info.parameters = {0, 10, 15, 0};
	REQUIRE(interpreter.ExecuteCommand(info));
	CHECK(ToString(Main_Data::game_strings->Get(15)) == "(OFF)");
	info.parameters = {0, 10, 0, 20};
	REQUIRE(interpreter.ExecuteCommand(info));
	CHECK(Main_Data::game_variables->GetRange(20, 4) == std::vector<int>{12345, 127, 450, -7});
	info.parameters = {0, 10, 0, 0};
	REQUIRE(interpreter.ExecuteCommand(info));
	CHECK(Main_Data::game_variables->GetRange(1, 3) == std::vector<int>{91, 92, 93});
	CHECK(ToString(Main_Data::game_strings->GetManiacRawSlot(0)) == "zero");
	Player::game_config.patch_maniac.Set(previous_patch);
	Player::game_config.maniac_version.Set(previous_version);
	Player::no_audio_flag = previous_no_audio;
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 pairs write global zero SELF and truncate negative camera toward zero") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	const auto previous_version = Player::game_config.maniac_version.Get();
	Player::game_config.patch_maniac.Set(1);
	Player::game_config.maniac_version.Set(250921);
	lcf::rpg::SaveMapInfo map_state;
	map_state.position_x = -17;
	map_state.position_y = -31;
	map_state.chipset_id = 1;
	Game_Map::SetupFromSave(MakeMockMap(MockMap::eNone), map_state, {}, {}, {}, {}, {}, {});
	lcf::rpg::SaveScreen screen_state;
	screen_state.shake_position = 7;
	screen_state.shake_position_y = -5;
	Main_Data::game_screen->SetSaveData(screen_state);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	lcf::rpg::EventCommand info;
	info.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_GetGameInfo);
	info.parameters = {0, 0, 0};
	Game_Interpreter interpreter;
	interpreter.Push({info}, 1);
	REQUIRE(interpreter.ExecuteCommand(info));
	CHECK(Main_Data::game_variables->GetManiacRawSlot(0) == 20);
	CHECK(Main_Data::game_variables->Get(1) == 15);
	for (auto operation : std::array<std::pair<int, int>, 3>{{{2, -10}, {8, -12}, {9, -14}}}) {
		info.parameters = {0, operation.first, operation.second};
		REQUIRE(interpreter.ExecuteCommand(info));
	}
	const auto* bank = Main_Data::game_maniac_self_variables->Find(2, 1, 1);
	REQUIRE(bank != nullptr);
	REQUIRE(bank->numbers.size() >= 16);
	CHECK(std::vector<int>(bank->numbers.begin() + 10, bank->numbers.begin() + 16) == std::vector<int>{Player::screen_width, Player::screen_height, -1, -1, 7, -5});
	Player::game_config.patch_maniac.Set(previous_patch);
	Player::game_config.maniac_version.Set(previous_version);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 tiles use runtime nibbles wrapped uint16 lookup and signed ranges") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	const auto previous_version = Player::game_config.maniac_version.Get();
	Player::game_config.patch_maniac.Set(1);
	Player::game_config.maniac_version.Set(250921);
	auto map = MakeMockMap(MockMap::eNone);
	map->scroll_type = lcf::rpg::Map::ScrollType_both;
	Game_Map::Setup(std::move(map));
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
	REQUIRE(bank != nullptr);
	bank->numbers = {0, 2, 18, 2};
	Main_Data::game_variables->Set(6, 22);
	const std::array<int, 4> positions{3 * 20 + 2, 3 * 20 + 3, 4 * 20 + 2, 4 * 20 + 3};
	const std::array<uint16_t, 4> lower{1000, 3050, 4500, 65535};
	for (size_t i = 0; i < positions.size(); ++i) {
		Game_Map::GetMapDataDown()[positions[i]] = static_cast<int16_t>(lower[i]);
		Game_Map::GetMapDataUp()[positions[i]] = static_cast<int16_t>(10070 + i);
	}
	lcf::rpg::EventCommand info;
	info.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_GetGameInfo);
	info.parameters = {0x121, 1, 0, 6, -1, -3, 2, 0};
	Game_Interpreter interpreter;
	interpreter.Push({info}, 1);
	REQUIRE(interpreter.ExecuteCommand(info));
	CHECK(Main_Data::game_variables->GetManiacRawSlot(0) == 1);
	CHECK(Main_Data::game_variables->GetRange(1, 3) == std::vector<int>{4, 16, 143});
	info.parameters = {0, 1, -1, -18, -12, 2, 2, -10};
	REQUIRE(interpreter.ExecuteCommand(info));
	bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
	REQUIRE(bank->numbers.size() >= 14);
	CHECK(std::vector<int>(bank->numbers.begin() + 10, bank->numbers.begin() + 14) == std::vector<int>{70, 71, 72, 73});
	Game_Map::Setup(MakeMockMap(MockMap::eNone));
	Main_Data::game_variables->SetRange(20, 23, 9);
	info.parameters = {0, 1, 1, 22, 18, 2, 2, 20};
	REQUIRE(interpreter.ExecuteCommand(info));
	CHECK(Main_Data::game_variables->GetRange(20, 4) == std::vector<int>{0, 0, 0, 0});
	Player::game_config.patch_maniac.Set(previous_patch);
	Player::game_config.maniac_version.Set(previous_version);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 interpreter tuple reads current and negative nest into zero and SELF ranges") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	const auto previous_version = Player::game_config.maniac_version.Get();
	Player::game_config.patch_maniac.Set(1);
	Player::game_config.maniac_version.Set(250921);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	lcf::rpg::SaveEventExecState frames;
	frames.stack.resize(2);
	frames.stack[0].maniac_event_info = 0x13;
	frames.stack[0].maniac_event_id = 11;
	frames.stack[0].maniac_event_page_id = 7;
	frames.stack[0].current_command = 91;
	frames.stack[1].maniac_event_info = 0x25;
	frames.stack[1].maniac_event_id = 22;
	frames.stack[1].maniac_event_page_id = 99; // Common context must return zero here.
	frames.stack[1].current_command = 73;
	frames.stack[1].easyrpg_self_scope = 1;
	frames.stack[1].easyrpg_self_event_id = 1;
	Game_Interpreter_Map interpreter;
	interpreter.SetState(frames);
	lcf::rpg::EventCommand info;
	info.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_GetGameInfo);
	info.parameters = {0, 4, 0, 0, 0};
	REQUIRE(interpreter.ExecuteCommand(info));
	CHECK(Main_Data::game_variables->GetManiacRawSlot(0) == 2);
	CHECK(Main_Data::game_variables->GetRange(1, 4) == std::vector<int>{22, 0, 5, 73});
	Main_Data::game_variables->Set(6, -10);
	info.parameters = {1, 4, 6, 0, -1};
	REQUIRE(interpreter.ExecuteCommand(info));
	const auto* bank = Main_Data::game_maniac_self_variables->Find(1, 0, 1);
	REQUIRE(bank != nullptr);
	REQUIRE(bank->numbers.size() >= 15);
	CHECK(std::vector<int>(bank->numbers.begin() + 10, bank->numbers.begin() + 15) == std::vector<int>{1, 11, 7, 3, 91});
	Main_Data::game_variables->SetRange(20, 24, 9);
	info.parameters = {0, 4, 20, 0, 2};
	REQUIRE(interpreter.ExecuteCommand(info));
	CHECK(Main_Data::game_variables->GetRange(20, 5) == std::vector<int>{0, 0, 0, 0, 0});
	Player::game_config.patch_maniac.Set(previous_patch);
	Player::game_config.maniac_version.Set(previous_version);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 pixels preserve RGB row order and mask alpha in signed ranges") {
	class PixelUi final : public BaseUi {
	public:
		explicit PixelUi(BitmapRef bitmap) : BaseUi(Game_Config{}) { GetDisplaySurface() = std::move(bitmap); }
		bool ProcessEvents() override { return true; }
		void UpdateDisplay() override {}
#ifdef SUPPORT_AUDIO
		AudioInterface& GetAudio() override { std::abort(); }
#endif
	protected:
		void vGetConfig(Game_ConfigVideo& cfg) const override { cfg = vcfg; }
	};
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Bitmap::SetFormat(format_R8G8B8A8_a().format());
	auto bitmap = Bitmap::Create(2, 2, true);
	bitmap->SetColorAt(0, 0, Color(10, 20, 30, 7));
	bitmap->SetColorAt(1, 0, Color(40, 50, 60, 0));
	bitmap->SetColorAt(0, 1, Color(70, 80, 90, 128));
	bitmap->SetColorAt(1, 1, Color(100, 110, 120, 255));
	auto previous_ui = std::move(DisplayUi);
	DisplayUi = std::make_shared<PixelUi>(bitmap);
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	const auto previous_version = Player::game_config.maniac_version.Get();
	Player::game_config.patch_maniac.Set(1);
	Player::game_config.maniac_version.Set(250921);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	lcf::rpg::EventCommand info;
	info.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_GetGameInfo);
	info.parameters = {0, 3, 0, 0, 0, 2, 2, 0};
	Game_Interpreter interpreter;
	interpreter.Push({info}, 1);
	REQUIRE(interpreter.ExecuteCommand(info));
	CHECK(Main_Data::game_variables->GetManiacRawSlot(0) == static_cast<int32_t>(0xff0a141e));
	CHECK(Main_Data::game_variables->GetRange(1, 3) == std::vector<int>{static_cast<int32_t>(0xff28323c), static_cast<int32_t>(0xff46505a), static_cast<int32_t>(0xff646e78)});
	info.parameters = {0, 3, 1, 0, 0, 2, 2, -10};
	REQUIRE(interpreter.ExecuteCommand(info));
	const auto* bank = Main_Data::game_maniac_self_variables->Find(2, 1, 1);
	REQUIRE(bank != nullptr);
	REQUIRE(bank->numbers.size() >= 14);
	CHECK(std::vector<int>(bank->numbers.begin() + 10, bank->numbers.begin() + 14) == std::vector<int>{0x0a141e, 0x28323c, 0x46505a, 0x646e78});
	info.parameters = {0, 3, 0, -1, 0, 2, 2, 20};
	REQUIRE(interpreter.ExecuteCommand(info));
	CHECK(Main_Data::game_variables->GetRange(20, 4) == std::vector<int>{0, static_cast<int32_t>(0xff0a141e), 0, static_cast<int32_t>(0xff46505a)});
	// Opaque XRGB has depth24 but occupies four bytes per pixel.
	auto opaque = Bitmap::Create(2, 2, false);
	// Set known raw XRGB cells independently of the pixel accessors under test.
	std::fill_n(static_cast<uint32_t*>(opaque->pixels()), 4,
		Bitmap::opaque_pixel_format.rgba_to_uint32_t(10, 20, 30, 255));
	DisplayUi->GetDisplaySurface() = opaque;
	info.parameters = {0, 3, 0, 0, 0, 2, 2, 30};
	REQUIRE(interpreter.ExecuteCommand(info));
	CHECK(Main_Data::game_variables->GetRange(30, 4) == std::vector<int>(4, static_cast<int32_t>(0xff0a141e)));
	opaque->SetColorAt(1, 0, Color(40, 50, 60, 255));
	uint32_t stored_pixel;
	std::memcpy(&stored_pixel, static_cast<const uint8_t*>(opaque->pixels()) + Bitmap::opaque_pixel_format.bytes, sizeof(stored_pixel));
	CHECK(stored_pixel == Bitmap::opaque_pixel_format.rgba_to_uint32_t(40, 50, 60, 255));
	Player::game_config.patch_maniac.Set(previous_patch);
	Player::game_config.maniac_version.Set(previous_version);
	Main_Data::game_maniac_self_variables.reset();
	DisplayUi = std::move(previous_ui);
	Graphics::Quit();
}

TEST_CASE("Maniac original comparison fixture serializes deterministically") {
	lcf::rpg::TreeMap tree;
	tree.maps.emplace_back();
	tree.maps.back().type = lcf::rpg::TreeMap::MapType_root;
	tree.maps.emplace_back();
	tree.maps.back().ID = 1;
	tree.maps.back().name = lcf::DBString("Maniac comparison");
	tree.maps.back().type = lcf::rpg::TreeMap::MapType_map;
	tree.tree_order = {1};
	tree.active_node = 1;
	tree.start.party_map_id = 1;
	tree.start.party_x = 10;
	tree.start.party_y = 7;
	lcf::rpg::Map map;
	map.lower_layer.assign(20 * 15, 10000);
	map.upper_layer.assign(20 * 15, 10000);
	map.events.emplace_back();
	auto& event = map.events.back();
	event.ID = 1;
	event.name = lcf::DBString("Slot zero probe");
	event.x = 11;
	event.y = 7;
	event.pages.emplace_back();
	auto& page = event.pages.back();
	page.ID = 1;
	page.trigger = lcf::rpg::EventPage::Trigger_auto_start;
	lcf::rpg::EventCommand set_zero;
	set_zero.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Selfvar);
	set_zero.parameters = {0x1000, 0, 0, 0, 0, 0, 7, 0, 1, 0};
	page.event_commands.push_back(set_zero);
	lcf::rpg::EventCommand copy_zero;
	copy_zero.code = set_zero.code;
	copy_zero.parameters = {0x10001000, 0, 0, 1, 0, 0, 0, 0x100, 1, 0};
	page.event_commands.push_back(copy_zero);
	lcf::rpg::EventCommand set_self;
	set_self.code = set_zero.code;
	set_self.parameters = {0x1003, 0, 0, 1, 0, 0, 42, 0, 1, 0};
	page.event_commands.push_back(set_self);
	lcf::rpg::EventCommand copy_self;
	copy_self.code = set_zero.code;
	copy_self.parameters = {0x10031000, 0, 0, 2, 0, 0, 1, 0x100, 1, 0};
	page.event_commands.push_back(copy_self);
	lcf::rpg::EventCommand read_self;
	read_self.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars);
	read_self.parameters = {0, 3, 0, 0, 1, -1, 0};
	page.event_commands.push_back(read_self);
	lcf::rpg::EventCommand write_self;
	write_self.code = read_self.code;
	write_self.parameters = {0, -2, 0, 0, 0, 99, 0};
	page.event_commands.push_back(write_self);
	lcf::rpg::EventCommand read_written_self;
	read_written_self.code = read_self.code;
	read_written_self.parameters = {0, 4, 0, 0, 1, -2, 0};
	page.event_commands.push_back(read_written_self);
	lcf::rpg::EventCommand clear_self;
	clear_self.code = set_zero.code;
	clear_self.parameters = {0x1003, 0, 0, 1, 0, 0, 0, 0, 1, 1};
	page.event_commands.push_back(clear_self);
	lcf::rpg::EventCommand read_cleared_self;
	read_cleared_self.code = read_self.code;
	read_cleared_self.parameters = {0, 5, 0, 0, 1, -1, 0};
	page.event_commands.push_back(read_cleared_self);
	lcf::rpg::EventCommand read_untouched_self;
	read_untouched_self.code = read_self.code;
	read_untouched_self.parameters = {0, 6, 0, 0, 1, -2, 0};
	page.event_commands.push_back(read_untouched_self);
	lcf::rpg::EventCommand init_self;
	init_self.code = set_zero.code;
	init_self.parameters = {0x1003, 0, 0, 0, 0, 0, 0, 0, 1, 2};
	page.event_commands.push_back(init_self);
	lcf::rpg::EventCommand read_initialized_self;
	read_initialized_self.code = read_self.code;
	read_initialized_self.parameters = {0, 7, 0, 0, 1, -2, 0};
	page.event_commands.push_back(read_initialized_self);
	lcf::rpg::EventCommand set_global_switch_zero;
	set_global_switch_zero.code = set_zero.code;
	set_global_switch_zero.parameters = {0x1000, 0, 0, 0, 0, 0, 258, 0x20, 1, 0};
	page.event_commands.push_back(set_global_switch_zero);
	lcf::rpg::EventCommand read_global_switch_zero;
	read_global_switch_zero.code = read_self.code;
	read_global_switch_zero.parameters = {0, 8, 0, 0, 10, 0, 0};
	page.event_commands.push_back(read_global_switch_zero);
	lcf::rpg::EventCommand set_self_switch;
	set_self_switch.code = set_zero.code;
	set_self_switch.parameters = {0x1003, 0, 0, 1, 0, 0, 259, 0x20, 1, 0};
	page.event_commands.push_back(set_self_switch);
	lcf::rpg::EventCommand read_self_switch;
	read_self_switch.code = read_self.code;
	read_self_switch.parameters = {0, 9, 0, 0, 10, -1, 0};
	page.event_commands.push_back(read_self_switch);
	lcf::rpg::EventCommand copy_global_switch_zero;
	copy_global_switch_zero.code = set_zero.code;
	copy_global_switch_zero.parameters = {0x10001000, 0, 0, 1, 0, 0, 0, 0x120, 1, 0};
	page.event_commands.push_back(copy_global_switch_zero);
	lcf::rpg::EventCommand read_copied_global_switch;
	read_copied_global_switch.code = read_self.code;
	read_copied_global_switch.parameters = {0, 10, 0, 0, 10, 1, 0};
	page.event_commands.push_back(read_copied_global_switch);
	lcf::rpg::EventCommand copy_self_switch;
	copy_self_switch.code = set_zero.code;
	copy_self_switch.parameters = {0x10031000, 0, 0, 2, 0, 0, 1, 0x120, 1, 0};
	page.event_commands.push_back(copy_self_switch);
	lcf::rpg::EventCommand read_copied_self_switch;
	read_copied_self_switch.code = read_self.code;
	read_copied_self_switch.parameters = {0, 11, 0, 0, 10, 2, 0};
	page.event_commands.push_back(read_copied_self_switch);
	lcf::rpg::EventCommand clear_global_switch_zero;
	clear_global_switch_zero.code = set_zero.code;
	clear_global_switch_zero.parameters = {0x1000, 0, 0, 0, 0, 0, 0, 0x20, 1, 1};
	page.event_commands.push_back(clear_global_switch_zero);
	lcf::rpg::EventCommand read_switch_after_clear;
	read_switch_after_clear.code = read_self.code;
	read_switch_after_clear.parameters = {0, 12, 0, 0, 10, 1, 0};
	page.event_commands.push_back(read_switch_after_clear);
	lcf::rpg::EventCommand call_common;
	call_common.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
	call_common.parameters = {0, 1, 0, 0x20, 0, 13, 2, 1, 77, 7, 259};
	page.event_commands.push_back(call_common);
	lcf::rpg::EventCommand read_caller_after_return;
	read_caller_after_return.code = read_self.code;
	read_caller_after_return.parameters = {0, 15, 0, 0, 1, -1, 0};
	page.event_commands.push_back(read_caller_after_return);
	lcf::rpg::EventCommand call_again;
	call_again.code = call_common.code;
	call_again.parameters = {0, 1, 0, 0x20, 0, -3, 2, 1, 88, 7, 258};
	page.event_commands.push_back(call_again);
	lcf::rpg::EventCommand read_caller_return;
	read_caller_return.code = read_self.code;
	read_caller_return.parameters = {0, 17, 0, 0, 1, -3, 0};
	page.event_commands.push_back(read_caller_return);
	lcf::rpg::EventCommand call_switch_global;
	call_switch_global.code = call_common.code;
	call_switch_global.parameters = {0, 2, 0, 0x80, 0, 3, 1, 7, 259};
	page.event_commands.push_back(call_switch_global);
	lcf::rpg::EventCommand read_returned_global_switch;
	read_returned_global_switch.code = read_self.code;
	read_returned_global_switch.parameters = {0, 18, 0, 0, 10, 3, 0};
	page.event_commands.push_back(read_returned_global_switch);
	lcf::rpg::EventCommand call_switch_caller;
	call_switch_caller.code = call_common.code;
	call_switch_caller.parameters = {0, 2, 0, 0x80, 0, -2, 1, 7, 259};
	page.event_commands.push_back(call_switch_caller);
	lcf::rpg::EventCommand read_returned_caller_switch;
	read_returned_caller_switch.code = read_self.code;
	read_returned_caller_switch.parameters = {0, 19, 0, 0, 10, -2, 0};
	page.event_commands.push_back(read_returned_caller_switch);
	lcf::rpg::EventCommand set_indirect_global_address;
	set_indirect_global_address.code = read_self.code;
	set_indirect_global_address.parameters = {0, 20, 0, 0, 0, 2, 0};
	page.event_commands.push_back(set_indirect_global_address);
	lcf::rpg::EventCommand call_switch_global_indirect;
	call_switch_global_indirect.code = call_common.code;
	call_switch_global_indirect.parameters = {0, 2, 0, 0x90, 0, 20, 1, 7, 259};
	page.event_commands.push_back(call_switch_global_indirect);
	lcf::rpg::EventCommand read_indirect_global_switch;
	read_indirect_global_switch.code = read_self.code;
	read_indirect_global_switch.parameters = {0, 21, 0, 0, 10, 4, 0};
	page.event_commands.push_back(read_indirect_global_switch);
	lcf::rpg::EventCommand set_indirect_caller_address;
	set_indirect_caller_address.code = read_self.code;
	set_indirect_caller_address.parameters = {0, -4, 0, 0, 0, 5, 0};
	page.event_commands.push_back(set_indirect_caller_address);
	lcf::rpg::EventCommand call_switch_caller_indirect;
	call_switch_caller_indirect.code = call_common.code;
	call_switch_caller_indirect.parameters = {0, 2, 0, 0x90, 0, -4, 1, 7, 259};
	page.event_commands.push_back(call_switch_caller_indirect);
	lcf::rpg::EventCommand read_indirect_caller_switch;
	read_indirect_caller_switch.code = read_self.code;
	read_indirect_caller_switch.parameters = {0, 22, 0, 0, 10, -5, 0};
	page.event_commands.push_back(read_indirect_caller_switch);
	lcf::rpg::EventCommand set_common_id_reference;
	set_common_id_reference.code = read_self.code;
	set_common_id_reference.parameters = {0, 23, 0, 0, 0, 1, 0};
	page.event_commands.push_back(set_common_id_reference);
	lcf::rpg::EventCommand set_common_id_indirect_reference;
	set_common_id_indirect_reference.code = read_self.code;
	set_common_id_indirect_reference.parameters = {0, 24, 0, 0, 0, 23, 0};
	page.event_commands.push_back(set_common_id_indirect_reference);
	lcf::rpg::EventCommand call_common_by_reference;
	call_common_by_reference.code = call_common.code;
	call_common_by_reference.parameters = {3, 23, 0, 0x20, 0, 25, 1, 1, 66};
	page.event_commands.push_back(call_common_by_reference);
	lcf::rpg::EventCommand call_common_by_indirect_reference;
	call_common_by_indirect_reference.code = call_common.code;
	call_common_by_indirect_reference.parameters = {4, 24, 0, 0x20, 0, 26, 1, 1, 67};
	page.event_commands.push_back(call_common_by_indirect_reference);
	lcf::rpg::EventCommand call_common_by_literal_name;
	call_common_by_literal_name.code = call_common.code;
	call_common_by_literal_name.parameters = {5, 12, 0, 0x20, 0, 28, 1, 1, 68};
	call_common_by_literal_name.string = lcf::DBString("Return probe");
	page.event_commands.push_back(call_common_by_literal_name);
	lcf::rpg::EventCommand set_common_name_string;
	set_common_name_string.code = set_zero.code;
	set_common_name_string.parameters = {0x1000, 0, 0, 1, 0, 0, 12, 0x10, 1, 0};
	set_common_name_string.string = lcf::DBString("Return probe");
	page.event_commands.push_back(set_common_name_string);
	lcf::rpg::EventCommand call_common_by_string_reference;
	call_common_by_string_reference.code = call_common.code;
	call_common_by_string_reference.parameters = {6, 1, 0, 0x20, 0, 29, 1, 1, 69};
	page.event_commands.push_back(call_common_by_string_reference);
	lcf::rpg::EventCommand set_common_name_string_id;
	set_common_name_string_id.code = read_self.code;
	set_common_name_string_id.parameters = {0, 30, 0, 0, 0, 1, 0};
	page.event_commands.push_back(set_common_name_string_id);
	lcf::rpg::EventCommand call_common_by_indirect_string_reference;
	call_common_by_indirect_string_reference.code = call_common.code;
	call_common_by_indirect_string_reference.parameters = {7, 30, 0, 0x20, 0, 31, 1, 1, 70};
	page.event_commands.push_back(call_common_by_indirect_string_reference);
	lcf::rpg::EventCommand call_common_by_literal_offset;
	call_common_by_literal_offset.code = call_common.code;
	call_common_by_literal_offset.parameters = {0, 1, 0, 0x20, 1, 32, 1, 7, 259};
	page.event_commands.push_back(call_common_by_literal_offset);
	lcf::rpg::EventCommand set_offset_reference;
	set_offset_reference.code = read_self.code;
	set_offset_reference.parameters = {0, 33, 0, 0, 0, 1, 0};
	page.event_commands.push_back(set_offset_reference);
	lcf::rpg::EventCommand call_common_by_variable_offset;
	call_common_by_variable_offset.code = call_common.code;
	call_common_by_variable_offset.parameters = {0, 1, 0, 0x21, 33, 34, 1, 7, 259};
	page.event_commands.push_back(call_common_by_variable_offset);
	lcf::rpg::EventCommand set_indirect_offset_reference;
	set_indirect_offset_reference.code = read_self.code;
	set_indirect_offset_reference.parameters = {0, 35, 0, 0, 0, 33, 0};
	page.event_commands.push_back(set_indirect_offset_reference);
	lcf::rpg::EventCommand call_common_by_indirect_offset;
	call_common_by_indirect_offset.code = call_common.code;
	call_common_by_indirect_offset.parameters = {0, 1, 0, 0x22, 35, 36, 1, 7, 259};
	page.event_commands.push_back(call_common_by_indirect_offset);
	lcf::rpg::CommonEvent common;
	common.ID = 1;
	common.name = lcf::DBString("Return probe");
	lcf::rpg::EventCommand read_argument;
	read_argument.code = read_self.code;
	read_argument.parameters = {0, 14, 0, 0, 1, -1, 0};
	common.event_commands.push_back(read_argument);
	lcf::rpg::EventCommand read_switch_argument;
	read_switch_argument.code = read_self.code;
	read_switch_argument.parameters = {0, 16, 0, 0, 10, -1, 0};
	common.event_commands.push_back(read_switch_argument);
	lcf::rpg::EventCommand deliver_return;
	deliver_return.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::EndEventProcessing);
	deliver_return.parameters = {0, 2, -1};
	common.event_commands.push_back(deliver_return);
	lcf::rpg::EventCommand common_end;
	common_end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::END);
	common.event_commands.push_back(common_end);
	lcf::rpg::CommonEvent switch_common;
	switch_common.ID = 2;
	switch_common.name = lcf::DBString("Switch return probe");
	lcf::rpg::EventCommand redirect_indirect_global_address;
	redirect_indirect_global_address.code = read_self.code;
	redirect_indirect_global_address.parameters = {0, 20, 0, 0, 0, 4, 0};
	switch_common.event_commands.push_back(redirect_indirect_global_address);
	lcf::rpg::EventCommand deliver_switch_return;
	deliver_switch_return.code = deliver_return.code;
	deliver_switch_return.parameters = {0, 8, -1};
	switch_common.event_commands.push_back(deliver_switch_return);
	switch_common.event_commands.push_back(common_end);
	lcf::rpg::EventCommand show;
	show.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ShowMessage);
	show.string = lcf::DBString("slot0 copy = \\V[1]");
	page.event_commands.push_back(show);
	lcf::rpg::EventCommand erase;
	erase.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::EraseEvent);
	page.event_commands.push_back(erase);
	lcf::rpg::EventCommand end;
	end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::END);
	page.event_commands.push_back(end);
	std::stringstream tree_stream(std::ios::in | std::ios::out | std::ios::binary);
	std::stringstream map_stream(std::ios::in | std::ios::out | std::ios::binary);
	REQUIRE(lcf::LMT_Reader::Save(tree_stream, tree, lcf::EngineVersion::e2k3));
	REQUIRE(lcf::LMU_Reader::Save(map_stream, map, lcf::EngineVersion::e2k3));
	tree_stream.seekg(0);
	map_stream.seekg(0);
	auto loaded_tree = lcf::LMT_Reader::Load(tree_stream);
	auto loaded_map = lcf::LMU_Reader::Load(map_stream);
	REQUIRE(loaded_tree != nullptr);
	REQUIRE(loaded_map != nullptr);
	REQUIRE(loaded_tree->maps.size() == 2);
	CHECK(loaded_tree->maps.front().ID == 0);
	CHECK(loaded_tree->start.party_map_id == 1);
	REQUIRE(loaded_map->events.size() == 1);
	REQUIRE(loaded_map->events.front().pages.size() == 1);
	const auto& commands = loaded_map->events.front().pages.front().event_commands;
	REQUIRE(commands.size() == 53);
	CHECK(commands[0].parameters == set_zero.parameters);
	CHECK(commands[1].parameters == copy_zero.parameters);
	CHECK(commands[2].parameters == set_self.parameters);
	CHECK(commands[3].parameters == copy_self.parameters);
	CHECK(commands[4].parameters == read_self.parameters);
	CHECK(commands[5].parameters == write_self.parameters);
	CHECK(commands[6].parameters == read_written_self.parameters);
	CHECK(commands[7].parameters == clear_self.parameters);
	CHECK(commands[8].parameters == read_cleared_self.parameters);
	CHECK(commands[9].parameters == read_untouched_self.parameters);
	CHECK(commands[10].parameters == init_self.parameters);
	CHECK(commands[11].parameters == read_initialized_self.parameters);
	CHECK(commands[12].parameters == set_global_switch_zero.parameters);
	CHECK(commands[13].parameters == read_global_switch_zero.parameters);
	CHECK(commands[14].parameters == set_self_switch.parameters);
	CHECK(commands[15].parameters == read_self_switch.parameters);
	CHECK(commands[16].parameters == copy_global_switch_zero.parameters);
	CHECK(commands[17].parameters == read_copied_global_switch.parameters);
	CHECK(commands[18].parameters == copy_self_switch.parameters);
	CHECK(commands[19].parameters == read_copied_self_switch.parameters);
	CHECK(commands[20].parameters == clear_global_switch_zero.parameters);
	CHECK(commands[21].parameters == read_switch_after_clear.parameters);
	CHECK(commands[22].parameters == call_common.parameters);
	CHECK(commands[23].parameters == read_caller_after_return.parameters);
	CHECK(commands[24].parameters == call_again.parameters);
	CHECK(commands[25].parameters == read_caller_return.parameters);
	CHECK(commands[26].parameters == call_switch_global.parameters);
	CHECK(commands[27].parameters == read_returned_global_switch.parameters);
	CHECK(commands[28].parameters == call_switch_caller.parameters);
	CHECK(commands[29].parameters == read_returned_caller_switch.parameters);
	CHECK(commands[30].parameters == set_indirect_global_address.parameters);
	CHECK(commands[31].parameters == call_switch_global_indirect.parameters);
	CHECK(commands[32].parameters == read_indirect_global_switch.parameters);
	CHECK(commands[33].parameters == set_indirect_caller_address.parameters);
	CHECK(commands[34].parameters == call_switch_caller_indirect.parameters);
	CHECK(commands[35].parameters == read_indirect_caller_switch.parameters);
	CHECK(commands[36].parameters == set_common_id_reference.parameters);
	CHECK(commands[37].parameters == set_common_id_indirect_reference.parameters);
	CHECK(commands[38].parameters == call_common_by_reference.parameters);
	CHECK(commands[39].parameters == call_common_by_indirect_reference.parameters);
	CHECK(commands[40].parameters == call_common_by_literal_name.parameters);
	CHECK(commands[40].string == call_common_by_literal_name.string);
	CHECK(commands[41].parameters == set_common_name_string.parameters);
	CHECK(commands[41].string == set_common_name_string.string);
	CHECK(commands[42].parameters == call_common_by_string_reference.parameters);
	CHECK(commands[43].parameters == set_common_name_string_id.parameters);
	CHECK(commands[44].parameters == call_common_by_indirect_string_reference.parameters);
	CHECK(commands[45].parameters == call_common_by_literal_offset.parameters);
	CHECK(commands[46].parameters == set_offset_reference.parameters);
	CHECK(commands[47].parameters == call_common_by_variable_offset.parameters);
	CHECK(commands[48].parameters == set_indirect_offset_reference.parameters);
	CHECK(commands[49].parameters == call_common_by_indirect_offset.parameters);
	lcf::Data::commonevents = {common, switch_common};
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		Player::game_config.patch_maniac.Set(1);
		Game_Interpreter interpreter;
		interpreter.Push(std::vector<lcf::rpg::EventCommand>(commands.begin(), commands.begin() + 22), 1);
		for (size_t index = 0; index < 22; ++index) {
			REQUIRE(interpreter.ExecuteCommand(commands[index]));
			if (index == 5) {
				const auto* self_bank = Main_Data::game_maniac_self_variables->Find(2, 1, 1);
				REQUIRE(self_bank != nullptr);
				CHECK(self_bank->numbers.size() == 4); // 10220 prepares one slot after self destination -2.
			}
		}
		CHECK(Main_Data::game_variables->GetManiacRawSlot(0) == 7);
		CHECK(Main_Data::game_variables->Get(1) == 7);
		CHECK(Main_Data::game_variables->Get(2) == 42);
		CHECK(Main_Data::game_variables->Get(3) == 42);
		CHECK(Main_Data::game_variables->Get(4) == 99);
		CHECK(Main_Data::game_variables->Get(5) == 0);
		CHECK(Main_Data::game_variables->Get(6) == 99);
		CHECK(Main_Data::game_variables->Get(7) == 0);
		CHECK(Main_Data::game_variables->Get(8) == 0);
		CHECK(Main_Data::game_variables->Get(9) == 3);
		CHECK(Main_Data::game_variables->Get(10) == 2);
		CHECK(Main_Data::game_variables->Get(11) == 3);
		CHECK(Main_Data::game_variables->Get(12) == 2);
		CHECK(Main_Data::game_variables->GetSize() == 13); // Native physical length is 14 including slot zero.
		CHECK(Main_Data::game_switches->GetManiacRawSlot(0) == 0);
		CHECK(Main_Data::game_switches->GetManiacRawSlot(1) == 2);
		CHECK(Main_Data::game_switches->GetManiacRawSlot(2) == 3);
		CHECK(Main_Data::game_switches->GetSize() == 3); // Native physical length is four.
		Game_Interpreter_Map running;
		running.Push(std::vector<lcf::rpg::EventCommand>(commands.begin() + 22, commands.begin() + 50), 1);
		running.Update();
		CHECK(Main_Data::game_variables->Get(13) == 77);
		CHECK(Main_Data::game_variables->Get(14) == 70);
		CHECK(Main_Data::game_variables->Get(15) == 0);
		CHECK(Main_Data::game_variables->Get(16) == 2);
		CHECK(Main_Data::game_variables->Get(17) == 88);
		CHECK(Main_Data::game_variables->Get(18) == 1);
		CHECK(Main_Data::game_variables->Get(19) == 1);
		CHECK(Main_Data::game_variables->Get(20) == 4);
		CHECK(Main_Data::game_variables->Get(21) == 1);
		CHECK(Main_Data::game_variables->Get(22) == 1);
		CHECK(Main_Data::game_variables->Get(23) == 1);
		CHECK(Main_Data::game_variables->Get(24) == 23);
		CHECK(Main_Data::game_variables->Get(25) == 66);
		CHECK(Main_Data::game_variables->Get(26) == 67);
		CHECK(Main_Data::game_variables->Get(27) == 0);
		CHECK(Main_Data::game_variables->Get(28) == 68);
		CHECK(Main_Data::game_variables->Get(29) == 69);
		CHECK(Main_Data::game_variables->Get(30) == 1);
		CHECK(Main_Data::game_variables->Get(31) == 70);
		CHECK(Main_Data::game_variables->Get(32) == 3);
		CHECK(Main_Data::game_variables->Get(33) == 1);
		CHECK(Main_Data::game_variables->Get(34) == 3);
		CHECK(Main_Data::game_variables->Get(35) == 33);
		CHECK(Main_Data::game_variables->Get(36) == 3);
		CHECK(Main_Data::game_strings->Get(1) == "Return probe");
		CHECK(Main_Data::game_switches->GetManiacRawSlot(3) == 1);
		CHECK(Main_Data::game_switches->GetManiacRawSlot(4) == 1);
		const auto* caller_bank = Main_Data::game_maniac_self_variables->Find(2, 1, 1);
		REQUIRE(caller_bank != nullptr);
		CHECK(Game_ManiacSelfVariables::ReadSwitch(caller_bank, -2) == 1);
		CHECK(Game_ManiacSelfVariables::ReadNumber(caller_bank, -4) == 5);
		CHECK(Game_ManiacSelfVariables::ReadSwitch(caller_bank, -5) == 1);
		const auto* callee_bank = Main_Data::game_maniac_self_variables->Find(1, 0, 1);
		REQUIRE(callee_bank != nullptr);
		CHECK(Game_ManiacSelfVariables::ReadNumber(callee_bank, -1) == 70);
		CHECK(Game_ManiacSelfVariables::ReadSwitch(callee_bank, -1) == 2);
		const auto* switch_callee_bank = Main_Data::game_maniac_self_variables->Find(1, 0, 2);
		REQUIRE(switch_callee_bank != nullptr);
		CHECK(Game_ManiacSelfVariables::ReadSwitch(switch_callee_bank, -1) == 3);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_maniac_self_variables.reset();
		Main_Data::game_strings.reset();
	}
	Graphics::Quit();
	lcf::Data::commonevents.clear();
	const char* fixture_path = std::getenv("MANIAC_WRITE_ORIGINAL_FIXTURE");
	if (fixture_path && *fixture_path) {
		const auto output = std::filesystem::path(fixture_path);
		const bool save_probe = std::getenv("MANIAC_ORIGINAL_SAVE_PROBE") != nullptr;
		const char* save_variant = std::getenv("MANIAC_ORIGINAL_SAVE_VARIANT");
		const bool noop_long_skip = save_variant && std::string_view(save_variant) == "noop-long-skip";
		const bool end_nest_long_skip = save_variant && std::string_view(save_variant) == "end-nest-long-skip";
		const bool end_nest_auto_common = save_variant && std::string_view(save_variant) == "end-nest-auto-common";
		const bool end_nest_battle = save_variant && std::string_view(save_variant) == "end-nest-battle";
		const bool opcode40_long_skip = save_variant && std::string_view(save_variant) == "opcode40-long-skip";
		const bool opcode40_auto_common = save_variant && std::string_view(save_variant) == "opcode40-auto-common";
		const bool opcode40_battle = save_variant && std::string_view(save_variant) == "opcode40-battle";
		const bool common1005_wait = save_variant && (std::string_view(save_variant) == "common1005-wait-auto"
			|| std::string_view(save_variant) == "common1005-wait-parallel"
			|| std::string_view(save_variant) == "common1005-wait-nested");
		const bool call12330_wait = save_variant && std::string_view(save_variant).substr(0, 15) == "call12330-wait-";
		const bool wait_parallel = save_variant && (std::string_view(save_variant) == "common1005-wait-parallel"
			|| std::string_view(save_variant) == "call12330-wait-parallel");
		const bool wait_nested = save_variant && (std::string_view(save_variant) == "common1005-wait-nested"
			|| std::string_view(save_variant) == "call12330-wait-nested");
		const bool wait_battle = save_variant && (std::string_view(save_variant) == "common1005-wait-battle"
			|| std::string_view(save_variant) == "call12330-wait-battle");
		const bool map_wait = common1005_wait || (call12330_wait && !wait_battle);
		const bool common1005 = save_variant && std::string_view(save_variant).substr(0, 11) == "common1005-" && !common1005_wait && !wait_battle;
		const bool common1005_battle = save_variant && std::string_view(save_variant) == "common1005-battle";
		const bool end_command_battle = save_variant && std::string_view(save_variant) == "end-command-battle";
		const bool noop_battle = save_variant && std::string_view(save_variant) == "noop-battle";
		const bool save_boundaries = save_variant && std::string_view(save_variant) == "save-boundaries";
		const bool load_seed = save_variant && std::string_view(save_variant) == "load-boundaries-seed";
		const bool load_boundaries = save_variant && std::string_view(save_variant) == "load-boundaries";
		const bool end_command_cycle = save_variant && std::string_view(save_variant) == "end-command-cycle";
		const bool end_command_frames_auto = save_variant && std::string_view(save_variant) == "end-command-frames-auto";
		const bool end_command_frames_parallel = save_variant && std::string_view(save_variant) == "end-command-frames-parallel";
		std::filesystem::create_directories(output);
		lcf::rpg::Map output_map = map;
		if (save_probe) {
			const bool reduced_save_probe = save_variant && std::string_view(save_variant) != "negative-result";
			const bool map_self_only = save_variant && std::string_view(save_variant).substr(0, 8) == "map-self";
			const bool map_self_string = save_variant && std::string_view(save_variant) == "map-self-string";
			const bool map_self_strings = save_variant && std::string_view(save_variant) == "map-self-strings";
			const bool map_self_resume = save_variant && std::string_view(save_variant) == "map-self-resume";
			const bool map_self_two_events = save_variant && std::string_view(save_variant) == "map-self-two-events";
			const bool map_self_two_maps = save_variant && std::string_view(save_variant) == "map-self-two-maps";
			const bool map_switch = save_variant && std::string_view(save_variant) == "map-switch";
			const bool title_cycle = save_variant && std::string_view(save_variant) == "title-cycle";
			const bool title_cycle_save = save_variant && std::string_view(save_variant) == "title-cycle-save";
			const bool js_title_cycle = save_variant && std::string_view(save_variant) == "js-title-cycle";
			const bool js_load_cycle = save_variant && std::string_view(save_variant) == "js-load-cycle";
			const bool load_overwrite = save_variant && std::string_view(save_variant) == "load-overwrite";
			const bool parallel_self = save_variant && std::string_view(save_variant) == "parallel-self";
			const bool parallel_self_save = save_variant && std::string_view(save_variant) == "parallel-self-save";
			const bool recursive_self = save_variant && std::string_view(save_variant) == "recursive-self";
			const bool recursive_self_save = save_variant && std::string_view(save_variant) == "recursive-self-save";
			const bool legacy_controlvars = save_variant && std::string_view(save_variant) == "legacy-controlvars";
			const bool legacy_negative_self = save_variant && std::string_view(save_variant) == "legacy-negative-self";
			const bool string_self_source = save_variant && std::string_view(save_variant) == "string-self-source";
			const bool string_to_num_radix = save_variant && std::string_view(save_variant) == "string-to-num-radix";
			const bool string_pop_line = save_variant && std::string_view(save_variant) == "string-pop-line";
			const bool string_pop_line_range = save_variant && std::string_view(save_variant) == "string-pop-line-range";
			const bool js_host_types = save_variant && std::string_view(save_variant) == "js-host-types";
			const bool js_host_accessors = save_variant && std::string_view(save_variant) == "js-host-accessors";
			const bool js_host_negative = save_variant && std::string_view(save_variant) == "js-host-negative";
			const bool js_host_edges = save_variant && std::string_view(save_variant) == "js-host-edges";
			const bool js_exception_continue = save_variant && std::string_view(save_variant) == "js-exception-continue";
			const bool js_host_inventory = save_variant && std::string_view(save_variant) == "js-host-inventory";
			const bool js_host_symbols = save_variant && std::string_view(save_variant) == "js-host-symbols";
			const bool reserved2_noop = save_variant && std::string_view(save_variant) == "reserved2-noop";
			const bool reserved_noop = save_variant && std::string_view(save_variant) == "reserved-noop";
			const bool drawshape_noop = save_variant && std::string_view(save_variant) == "drawshape-noop";
			const bool load_resume = save_variant && (std::string_view(save_variant) == "load-resume"
				|| std::string_view(save_variant) == "load-export");
			const bool event_id_two = save_variant && std::string_view(save_variant) == "map-self-event2";
			const bool negative_result = save_variant && std::string_view(save_variant) == "negative-result";
			if (event_id_two) output_map.events.front().ID = 2;
			lcf::rpg::EventCommand save_command;
			save_command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save);
			save_command.parameters = {0, 1, reduced_save_probe ? 0 : 1,
				reduced_save_probe ? 0 : (negative_result ? -6 : 37)};
			auto& output_commands = output_map.events.front().pages.front().event_commands;
			if (end_command_frames_auto || end_command_frames_parallel) {
				if (end_command_frames_parallel) {
					output_map.events.front().pages.front().trigger = lcf::rpg::EventPage::Trigger_parallel;
				}
				lcf::rpg::EventCommand count = read_self;
				count.parameters = {0, 1, 0, 1, 0, 1, 0};
				lcf::rpg::EventCommand self_count = count;
				self_count.parameters[1] = -1;
				lcf::rpg::EventCommand call;
				call.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
				call.parameters = {0, 3, 0};
				lcf::rpg::EventCommand copy_map = read_self;
				copy_map.parameters = {0, 2, 0, 0, 1, -1, 0};
				lcf::rpg::EventCommand wait;
				wait.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
				wait.parameters = {15};
				lcf::rpg::EventCommand save = save_command;
				save.parameters = {1, 1, 0, 0};
				output_commands = {count, self_count, call, copy_map, wait, save, wait};
			} else if (end_command_cycle) {
				output_map.events.front().pages.front().trigger = lcf::rpg::EventPage::Trigger_parallel;
				lcf::rpg::EventCommand count = read_self;
				count.parameters = {0, 1, 0, 1, 0, 1, 0};
				lcf::rpg::EventCommand self_count = count;
				self_count.parameters[1] = -1;
				lcf::rpg::EventCommand copy = read_self;
				copy.parameters = {0, 2, 0, 0, 1, -1, 0};
				lcf::rpg::EventCommand wait;
				wait.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
				wait.parameters = {15};
				lcf::rpg::EventCommand save = save_command;
				save.parameters = {1, 1, 0, 0};
				// liblcf writes the four-byte file terminator. Original runtime
				// materializes it as opcode 0; Player finishes at the list boundary.
				output_commands = {count, self_count, copy, wait, save, wait};
			} else if (reserved2_noop || reserved_noop || drawshape_noop) {
				lcf::rpg::EventCommand noop;
				noop.code = drawshape_noop ? 3035 : reserved_noop ? 3034 : 3024;
				noop.parameters = {7, 99, -3};
				noop.string = lcf::DBString("DO_NOT_EXECUTE");
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 1, 0, 0, 0, 99, 0};
				output_commands = {noop, marker, save_command, end};
			} else if (load_seed || load_boundaries) {
				output_commands.clear();
				const auto value = [&](int id, int number) {
					lcf::rpg::EventCommand command = read_self;
					command.parameters = {0, id, 0, 0, 0, number, 0};
					output_commands.push_back(command);
				};
				const auto save = [&](int slot) {
					lcf::rpg::EventCommand command = save_command;
					command.parameters = {0, slot, 0, 0};
					output_commands.push_back(command);
				};
				const auto wait = [&](int tenths) {
					lcf::rpg::EventCommand command;
					command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
					command.parameters = {tenths};
					output_commands.push_back(command);
				};
				if (load_seed) {
					output_map.events.front().ID = 2;
					const char* seed_slot = std::getenv("MANIAC_ORIGINAL_LOAD_SEED_SLOT");
					const int slot = seed_slot && std::string_view(seed_slot) == "9" ? 9 : 7;
					value(1, slot * 101);
					save(slot);
					// The collector stops the seed run before this continuation.
					// Loading the seed later must resume and produce slot 8.
					wait(50);
					value(10, 99);
					save(8);
				} else {
					value(1, 111);
					value(2, 7);
					value(7, 9);
					value(-1, 7);
					const std::vector<std::vector<int32_t>> skipped = {
						{}, {0}, {1}, {0, 0}, {0, -1}, {0, 666}, {1, 0},
						{0, 0, 0}, {0, 0, 0, 0}, {2, -2, 0}
					};
					for (size_t i = 0; i < skipped.size(); ++i) {
						lcf::rpg::EventCommand load;
						load.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Load);
						load.parameters = lcf::DBArray<int32_t>(skipped[i].begin(), skipped[i].end());
						load.string = lcf::DBString("IGNORED_LOAD_TEXT");
						output_commands.push_back(load);
						value(3, static_cast<int>(i + 1));
					}
					save(5);
					wait(15);
					const std::vector<std::vector<int32_t>> selected = {
						{0, 7}, {0, 7, 0}, {0, 7, 0, 0}, {1, 2, 0},
						{2, 2, 0}, {91, -1, 0}, {-3, 2, 0}, {0, 9, 0}
					};
					const char* case_index = std::getenv("MANIAC_ORIGINAL_LOAD_CASE");
					const int index = case_index ? std::atoi(case_index) : 0;
					REQUIRE(index >= 0);
					REQUIRE(index < static_cast<int>(selected.size()));
					lcf::rpg::EventCommand load;
					load.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Load);
					load.parameters = lcf::DBArray<int32_t>(selected[index].begin(), selected[index].end());
					load.string = lcf::DBString("IGNORED_LOAD_TEXT");
					output_commands.push_back(load);
					value(10, -99); // A successful load must replace this frame.
					save(8);
				}
				wait(9999);
					output_commands.push_back(end);
			} else if (save_boundaries) {
				output_commands.clear();
				const auto value = [&](int id, int number, bool variable = false) {
					lcf::rpg::EventCommand command = read_self;
					command.parameters = {0, id, 0, 0, variable ? 1 : 0, number, 0};
					output_commands.push_back(command);
				};
				const auto save = [&](std::initializer_list<int32_t> parameters) {
					lcf::rpg::EventCommand command = save_command;
					command.parameters = parameters;
					command.string = lcf::DBString("IGNORED_SAVE_TEXT");
					output_commands.push_back(command);
					// Keep parameter-boundary evidence separate from the rate guard.
					lcf::rpg::EventCommand pause;
					pause.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
					pause.parameters = {15};
					output_commands.push_back(pause);
				};
				value(1, 3);
				value(2, 1);
				value(-1, 2);
				value(3, 99);
				save({0, 0, 1, 3});
				value(4, 3, true);
				value(-6, 99);
				save({0, -1, 1, -6});
				value(5, -6, true);
				value(6, 88);
				save({0, 0, 0, 6});
				value(7, 6, true);
				save({});
				save({0});
				save({1});
				save({0, 0});
				save({0, 0, 1});
				save({2, 2, 1, 8}); // Any nonzero A0 performs only one lookup: slot 1.
				save({1, -1, 1, -7}); // Self slot selection resolves to slot 2.
				value(10, -7, true);
				save({0, 1}); // Two fields: valid save, no result write.
				save({0, 2, 1}); // Missing destination is zero, an ignored write.
				value(9, 99);
				save({0, 9, 0, 0});
				lcf::rpg::EventCommand wait;
				wait.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
				wait.parameters = {9999};
				output_commands.push_back(wait);
				output_commands.push_back(end);
			} else if (noop_battle || end_nest_battle || opcode40_battle || common1005_battle || wait_battle || end_command_battle) {
				lcf::rpg::EventCommand encounter;
				encounter.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::EnemyEncounter);
				encounter.parameters = {0, 1, 0, 0, 0, 0, 0};
				lcf::rpg::EventCommand map_marker = read_self;
				map_marker.parameters = {0, 10, 0, 0, 0, 110, 0};
				if (wait_battle) {
					lcf::rpg::EventCommand load;
					load.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Load);
					load.parameters = {0, 1, 0};
					lcf::rpg::EventCommand after_battle;
					after_battle.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save);
					after_battle.parameters = {0, 3, 0, 0};
					lcf::rpg::EventCommand wait;
					wait.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
					wait.parameters = {9999};
					output_commands = {load, map_marker, encounter, after_battle, wait, end};
				} else {
					output_commands = {map_marker, encounter, end};
				}
			} else if (end_nest_auto_common || opcode40_auto_common) {
				lcf::rpg::EventCommand nested_end = end;
				if (opcode40_auto_common) nested_end.code = 40;
				nested_end.parameters = {7, 99, -3};
				nested_end.string = lcf::DBString("IGNORED_END_NEST_TEXT");
				lcf::rpg::EventCommand branch;
				branch.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ConditionalBranch);
				branch.parameters = {1, 3, 0, 1, 0, 0};
				lcf::rpg::EventCommand skipped_end = nested_end;
				skipped_end.indent = 1;
				lcf::rpg::EventCommand skipped_marker = read_self;
				skipped_marker.indent = 1;
			skipped_marker.parameters = {0, 4, 0, 0, 0, 44, 0};
				lcf::rpg::EventCommand branch_end;
				branch_end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::EndBranch);
				lcf::rpg::EventCommand true_branch = branch;
				true_branch.parameters[3] = 0;
				lcf::rpg::EventCommand active_marker = read_self;
				active_marker.indent = 1;
				active_marker.parameters = {0, 3, 0, 0, 0, 33, 0};
				lcf::rpg::EventCommand call;
				call.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
				call.parameters = {0, 3, 0};
				lcf::rpg::EventCommand parent_marker = read_self;
				parent_marker.parameters = {0, 1, 0, 0, 0, 99, 0};
				lcf::rpg::EventCommand wait;
				wait.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
				wait.parameters = {9999};
				output_commands = {nested_end, branch, skipped_end, skipped_marker, branch_end,
					true_branch, skipped_end, active_marker, branch_end, call, parent_marker,
					save_command, wait, end};
			} else if (end_nest_long_skip || opcode40_long_skip) {
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 1, 0, 0, 0, 99, 0};
				lcf::rpg::EventCommand wait;
				wait.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
				wait.parameters = {50};
				lcf::rpg::EventCommand nested_end = end;
				if (opcode40_long_skip) nested_end.code = 40;
				nested_end.parameters = {7, 99, -3};
				nested_end.string = lcf::DBString("IGNORED_END_NEST_TEXT");
				output_commands.assign(10050, nested_end);
				output_commands.insert(output_commands.end(), {marker, wait, end});
				output_map.events.front().pages.front().trigger = lcf::rpg::EventPage::Trigger_parallel;
				lcf::rpg::Event second_event = output_map.events.front();
				second_event.ID = 2;
				lcf::rpg::EventCommand snapshot = read_self;
				snapshot.parameters = {0, 2, 0, 0, 1, 1, 0};
				second_event.pages.front().event_commands = {snapshot, save_command, wait, end};
				output_map.events.push_back(std::move(second_event));
			} else if (noop_long_skip) {
				// 3024 and 3035 re-fetch inside the 250921 dispatcher and do not
				// consume the outer caller's command budget. The second parallel
				// event records whether the first has reached its marker this tick.
				lcf::rpg::EventCommand noop;
				noop.parameters = {7, 99, -3};
				noop.string = lcf::DBString("DO_NOT_EXECUTE");
				output_commands.clear();
				for (int index = 0; index < 10050; ++index) {
					noop.code = index % 2 == 0 ? 3024 : 3035;
					output_commands.push_back(noop);
				}
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 1, 0, 0, 0, 99, 0};
				lcf::rpg::EventCommand wait;
				wait.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
				wait.parameters = {50};
				lcf::rpg::EventCommand call;
				call.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
				call.parameters = {0, 1, 0};
				output_commands.insert(output_commands.end(), {marker, call, wait, end});
				output_map.events.front().pages.front().trigger = lcf::rpg::EventPage::Trigger_parallel;
				lcf::rpg::Event second_event = output_map.events.front();
				second_event.ID = 2;
				lcf::rpg::EventCommand snapshot = read_self;
				snapshot.parameters = {0, 2, 0, 0, 1, 1, 0};
				second_event.pages.front().event_commands = {snapshot, save_command, wait, end};
				output_map.events.push_back(std::move(second_event));
			} else if (js_host_symbols) {
				lcf::rpg::EventCommand script;
				script.code = 3030;
				script.parameters = {0, 0, 0, 0, 1, 4, 1};
				script.string = lcf::DBString(
				"(()=>{let p=rm2k3.getPicture(1);let a=[['global',globalThis],"
				"['rm2k3',rm2k3],['console',console],['picture',p],"
				"['pictureProto',Object.getPrototypeOf(p)],['getPicture',rm2k3.getPicture],"
				"['consoleLog',console.log],['consoleInput',console.input],"
				"['include',include],['getv',getv],['gets',gets],['gett',gett],"
				"['setv',setv],['sets',sets],['sett',sett]];"
				"return JSON.stringify(a.map(([n,o])=>[n,Reflect.ownKeys(o).map(k=>"
				"(typeof k==='symbol'?'symbol:':'string:')+String(k)).sort()]));})()");
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 1, 0, 0, 0, 99, 0};
				output_commands = {script, marker, save_command, end};
			} else if (js_host_inventory) {
				lcf::rpg::EventCommand script;
				script.code = 3030;
				script.parameters = {0, 0, 0, 0, 5, 4, 1, 4, 2, 4, 3, 4, 4, 4, 5};
				script.string = lcf::DBString(
					"[Object.getOwnPropertyNames(globalThis).sort().join('|'), "
					"Object.getOwnPropertyNames(rm2k3).sort().join('|'), "
					"Object.getOwnPropertyNames(console).sort().join('|'), "
					"typeof Rm2k3_Picture === 'function' ? "
					"Object.getOwnPropertyNames(Rm2k3_Picture.prototype).sort().join('|') : 'missing', "
					"(()=>{try{return Object.getOwnPropertyNames(Object.getPrototypeOf(rm2k3.getPicture(1))).sort().join('|')}"
					"catch(e){return 'ERR:'+e}})()]");
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 1, 0, 0, 0, 99, 0};
				output_commands = {script, marker, save_command, end};
			} else if (js_exception_continue) {
				lcf::rpg::EventCommand error_script;
				error_script.code = 3030;
				error_script.parameters = {0, 0, 0, 0, 2, 2, 1, 4, 1};
				error_script.string = lcf::DBString("throw new Error('boom')");
				lcf::rpg::EventCommand next_script = error_script;
				next_script.parameters = {0, 0, 0, 0, 1, 2, 2};
				next_script.string = lcf::DBString("40+2");
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 3, 0, 0, 0, 99, 0};
				output_commands = {error_script, next_script, marker, save_command, end};
			} else if (js_host_edges) {
				const char* source_path = std::getenv("MANIAC_ORIGINAL_JS_SOURCE");
				REQUIRE(source_path != nullptr);
				std::ifstream source(std::filesystem::path(source_path), std::ios::binary);
				REQUIRE(source.is_open());
				std::string script_text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
				REQUIRE(!script_text.empty());
				lcf::rpg::EventCommand script;
				script.code = 3030;
				script.parameters = {0, 0, 0, 0, 1, 4, 1};
				script.string = lcf::DBString(script_text);
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 1, 0, 0, 0, 99, 0};
				lcf::rpg::EventCommand pause;
				pause.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
				pause.parameters = {9999};
				output_commands = {script, marker, save_command, pause, end};
			} else if (js_host_negative) {
				lcf::rpg::EventCommand index = read_self;
				index.parameters = {0, 1, 0, 0, 0, -1, 0};
				const auto js = [](int destination, const char* expression) {
					lcf::rpg::EventCommand command;
					command.code = 3030;
					command.parameters = {0, 0, 0, 0, 1, 4, destination};
					command.string = lcf::DBString(expression);
					return command;
				};
				const auto absent =
					"(()=>{let r=[];for(let f of [getv,gets,gett])"
					"{try{r.push(f(-1))}catch(e){r.push('ERR:'+e)}}"
					"return JSON.stringify(r)})()";
				const auto mutate =
					"(()=>{let n=getv(1);setv(-1,42);sets(-1,1);sett(-1,'M');"
					"let a=[n,getv(-1),getv(n),gets(-1),gets(n),gett(-1),gett(n)];"
					"setv(n,44);sets(n,0);sett(n,'MI');"
					"a.push(getv(-1),gets(-1),gett(-1));return JSON.stringify(a)})()";
				lcf::rpg::EventCommand call;
				call.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
				call.parameters = {0, 3, 0};
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 2, 0, 0, 0, 99, 0};
				lcf::rpg::EventCommand pause;
				pause.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
				pause.parameters = {9999};
				output_commands = {index, js(1, absent), js(2, mutate), call, marker, save_command, pause, end};
			} else if (js_host_accessors) {
				lcf::rpg::EventCommand script;
				script.code = 3030;
				script.parameters = {0, 0, 0, 0, 5,
					2, 2, 0, 1, 4, 1, 4, 2, 4, 3};
				script.string = lcf::DBString(
					"[setv(10,42), sets(10,1), sett(10,'Host'), getv(10), gets(10)]");
				lcf::rpg::EventCommand script_read = script;
				script_read.parameters = {0, 0, 0, 0, 3, 4, 4, 4, 5, 4, 6};
				script_read.string = lcf::DBString("[getv(10), gets(10), gett(10)]");
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 7, 0, 0, 0, 99, 0};
				output_commands = {script, script_read, marker, save_command, end};
			} else if (js_host_types) {
				lcf::rpg::EventCommand script;
				script.code = 3030;
				script.parameters = {0, 0, 0, 0, 9,
					4, 1, 4, 2, 4, 3, 4, 4, 4, 5, 4, 6, 4, 7, 4, 8, 4, 9};
				script.string = lcf::DBString(
					"[typeof rm2k3, typeof console, typeof input, typeof include, "
					"typeof rm2k3.getv, typeof rm2k3.gets, typeof rm2k3.gett, "
					"typeof rm2k3.setv, typeof rm2k3.getPicture]");
				lcf::rpg::EventCommand global_script = script;
				global_script.parameters = {0, 0, 0, 0, 9,
					4, 10, 4, 11, 4, 12, 4, 13, 4, 14, 4, 15, 4, 16, 4, 17, 4, 18};
				global_script.string = lcf::DBString(
					"[typeof getv, typeof gets, typeof gett, typeof setv, "
					"typeof sets, typeof sett, typeof getPicture, typeof log, "
					"typeof rm2k3.getPicture]");
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 1, 0, 0, 0, 99, 0};
				output_commands = {script, global_script, marker, save_command, end};
			} else if (string_pop_line_range) {
				lcf::rpg::EventCommand assign;
				assign.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
				assign.parameters = {0, 1, 0, 0, 0, 0, 0, 0};
				assign.string = lcf::DBString("A\r\nB");
				lcf::rpg::EventCommand assign_second = assign;
				assign_second.parameters[1] = 2;
				assign_second.string = lcf::DBString("C\nD");
				lcf::rpg::EventCommand assign_third = assign;
				assign_third.parameters[1] = 3;
				assign_third.string = lcf::DBString("E");
				lcf::rpg::EventCommand pop_range = assign;
				pop_range.parameters = {1, 1, 3, 8, 5, 0, 0, 0};
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 1, 0, 0, 0, 99, 0};
				output_commands = {assign, assign_second, assign_third, pop_range,
					marker, save_command, end};
			} else if (string_pop_line) {
				lcf::rpg::EventCommand assign;
				assign.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
				assign.parameters = {0, 1, 0, 0, 0, 0, 0, 0};
				assign.string = lcf::DBString("A\r\nB");
				lcf::rpg::EventCommand first_pop = assign;
				first_pop.parameters = {0, 1, 0, 8, 2, 0, 0, 0};
				lcf::rpg::EventCommand second_pop = first_pop;
				second_pop.parameters[4] = 3;
				lcf::rpg::EventCommand assign_alias = assign;
				assign_alias.parameters[1] = 4;
				assign_alias.string = lcf::DBString("X");
				lcf::rpg::EventCommand alias_pop = first_pop;
				alias_pop.parameters[1] = 4;
				alias_pop.parameters[4] = 4;
				lcf::rpg::EventCommand assign_observer = assign;
				assign_observer.parameters[1] = 5;
				assign_observer.string = lcf::DBString("Y");
				lcf::rpg::EventCommand observe_alias = first_pop;
				observe_alias.parameters = {0x10, 5, 0, 1, 4, 0, 0, 0};
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 1, 0, 0, 0, 99, 0};
				output_commands = {assign, first_pop, second_pop, assign_alias, alias_pop,
					assign_observer, observe_alias, marker, save_command, end};
			} else if (string_to_num_radix) {
				lcf::rpg::EventCommand assign;
				assign.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
				assign.parameters = {0, 1, 0, 0, 0, 0, 0, 0};
				assign.string = lcf::DBString("08");
				lcf::rpg::EventCommand assign_decimal_hex = assign;
				assign_decimal_hex.parameters[1] = 2;
				assign_decimal_hex.string = lcf::DBString("0x10");
				lcf::rpg::EventCommand assign_explicit_hex = assign_decimal_hex;
				assign_explicit_hex.parameters[1] = 3;
				lcf::rpg::EventCommand decimal = assign;
				decimal.parameters = {0, 1, 0, 2, 1, 0, 0, 0};
				lcf::rpg::EventCommand decimal_hex_prefix = decimal;
				decimal_hex_prefix.parameters[1] = 2;
				decimal_hex_prefix.parameters[4] = 2;
				lcf::rpg::EventCommand explicit_hex = decimal;
				explicit_hex.parameters[1] = 3;
				explicit_hex.parameters[3] = (1 << 17) | 2;
				explicit_hex.parameters[4] = 3;
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 4, 0, 0, 0, 99, 0};
				output_commands = {assign, assign_decimal_hex, assign_explicit_hex,
					decimal, decimal_hex_prefix, explicit_hex, marker, save_command, end};
			} else if (string_self_source) {
				lcf::rpg::EventCommand set_self_string = set_common_name_string;
				set_self_string.parameters = {0x1003, 0, 0, 1, 0, 0, 11, 0x10, 1, 0};
				set_self_string.string = lcf::DBString("Bank sample");
				lcf::rpg::EventCommand set_indirect = read_self;
				set_indirect.parameters = {0, 3, 0, 0, 0, -1, 0};
				lcf::rpg::EventCommand copy_self_string;
				copy_self_string.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
				copy_self_string.parameters = {0x10, 1, 0, 0, -1, 0, 0, 0};
				copy_self_string.string = lcf::DBString("Fallback");
				lcf::rpg::EventCommand copy_indirect = copy_self_string;
				copy_indirect.parameters = {0x20, 3, 0, 0, 3, 0, 0, 0};
				lcf::rpg::EventCommand literal_string = copy_self_string;
				literal_string.parameters[0] = 0;
				literal_string.parameters[1] = 2;
				literal_string.string = lcf::DBString("Literal");
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 2, 0, 0, 0, 99, 0};
				output_commands = {set_self_string, set_indirect, copy_self_string, copy_indirect, literal_string, marker, save_command, end};
			} else if (legacy_negative_self) {
				lcf::rpg::EventCommand set_self_number = read_self;
				set_self_number.parameters = {0, -1, 0, 0, 0, 42, 0};
				lcf::rpg::EventCommand read_self_number = read_self;
				read_self_number.parameters = {0, 1, 0, 0, 1, -1, 0};
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 2, 0, 0, 0, 99, 0};
				output_commands = {set_self_number, read_self_number, marker, save_command, end};
			} else if (legacy_controlvars) {
				lcf::rpg::EventCommand set_first = read_self;
				set_first.parameters = {0, 1, 0, 0, 0, 7, 0};
				lcf::rpg::EventCommand copy_first = read_self;
				copy_first.parameters = {0, 2, 0, 0, 1, 1, 0};
				output_commands = {set_first, copy_first, save_command, end};
			} else if (recursive_self || recursive_self_save) {
				lcf::rpg::EventCommand recursive_call;
				recursive_call.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
				recursive_call.parameters = {0, 1, 0};
				if (recursive_self_save) {
					output_commands = {recursive_call, save_command, erase, end};
				} else {
					output_commands = {recursive_call, erase, end};
				}
			} else if (parallel_self || parallel_self_save) {
				lcf::rpg::EventCommand read_before = read_self;
				read_before.parameters[1] = 3;
				lcf::rpg::EventCommand read_after = read_self;
				read_after.parameters[1] = 5;
				lcf::rpg::EventCommand wait;
				wait.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
				wait.parameters = {50};
				output_map.events.front().pages.front().trigger = lcf::rpg::EventPage::Trigger_parallel;
				if (parallel_self_save) {
					lcf::rpg::EventCommand settle = wait;
					settle.parameters = {2};
					output_commands = {read_before, set_self, read_after, settle, save_command, wait, end};
				} else {
					output_commands = {read_before, set_self, read_after, wait, end};
				}
				lcf::rpg::Event second_event = output_map.events.front();
				second_event.ID = 2;
				auto& second_commands = second_event.pages.front().event_commands;
				lcf::rpg::EventCommand second_before = read_before;
				second_before.parameters[1] = 4;
				lcf::rpg::EventCommand second_set = set_self;
				second_set.parameters[6] = 43;
				lcf::rpg::EventCommand second_after = read_after;
				second_after.parameters[1] = 6;
				second_commands = {second_before, second_set, second_after, wait, end};
				output_map.events.push_back(std::move(second_event));
			} else if (load_overwrite) {
				lcf::rpg::EventCommand read_saved = read_self;
				read_saved.parameters[1] = 3;
				lcf::rpg::EventCommand read_mutated = read_self;
				read_mutated.parameters[1] = 4;
				lcf::rpg::EventCommand set_mutated = set_self;
				set_mutated.parameters[6] = 43;
				lcf::rpg::EventCommand wait;
				wait.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
				wait.parameters = {50};
				lcf::rpg::EventCommand load;
				load.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Load);
				load.parameters = {0, 1, 0};
				output_commands = {set_self, save_command, read_saved, wait, set_mutated, read_mutated, wait, load, end};
			} else if (js_load_cycle) {
				lcf::rpg::EventCommand assign;
				assign.code = 3030;
				assign.parameters = {0, 0, 0, 0, 0};
				assign.string = lcf::DBString("globalThis.__maniac_load_probe = 42");
				lcf::rpg::EventCommand read_saved = assign;
				read_saved.parameters = {0, 0, 0, 0, 1, 2, 3};
				read_saved.string = lcf::DBString("globalThis.__maniac_load_probe === undefined ? 99 : globalThis.__maniac_load_probe");
				lcf::rpg::EventCommand mutate = assign;
				mutate.string = lcf::DBString("globalThis.__maniac_load_probe = 43");
				lcf::rpg::EventCommand read_mutated = read_saved;
				read_mutated.parameters[6] = 4;
				lcf::rpg::EventCommand wait;
				wait.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
				wait.parameters = {10};
				lcf::rpg::EventCommand load;
				load.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Load);
				load.parameters = {0, 1, 0};
				output_commands = {assign, save_command, read_saved, wait, mutate, read_mutated, wait, load, end};
			} else if (js_title_cycle) {
				lcf::rpg::EventCommand first_read;
				first_read.code = 3030;
				first_read.parameters = {0, 0, 0, 0, 1, 2, 3};
				first_read.string = lcf::DBString(
					"globalThis.__maniac_context_probe === undefined ? 0 : globalThis.__maniac_context_probe");
				lcf::rpg::EventCommand set_probe = first_read;
				set_probe.parameters = {0, 0, 0, 0, 0};
				set_probe.string = lcf::DBString("globalThis.__maniac_context_probe = 42");
				lcf::rpg::EventCommand second_read = first_read;
				second_read.parameters[6] = 4;
				second_read.string = lcf::DBString("globalThis.__maniac_context_probe");
				lcf::rpg::EventCommand wait;
				wait.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
				wait.parameters = {5};
				lcf::rpg::EventCommand title;
				title.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ReturntoTitleScreen);
				output_commands = {first_read, wait, set_probe, second_read, wait, title, end};
			} else if (title_cycle || title_cycle_save) {
				lcf::rpg::EventCommand first_read = read_self;
				first_read.parameters[1] = 3;
				lcf::rpg::EventCommand second_read = read_self;
				second_read.parameters[1] = 4;
				lcf::rpg::EventCommand wait;
				wait.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
				wait.parameters = {title_cycle_save ? 5 : 50};
				lcf::rpg::EventCommand title;
				title.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ReturntoTitleScreen);
				if (title_cycle_save) {
					lcf::rpg::EventCommand save_second = save_command;
					save_second.parameters[1] = 2;
					output_commands = {first_read, save_command, wait, set_self,
						second_read, save_second, wait, title, end};
				} else {
					output_commands = {first_read, wait, set_self, second_read, wait, title, end};
				}
			} else if (load_resume) {
				lcf::rpg::EventCommand load;
				load.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Load);
				load.parameters = {0, 1, 0};
				output_commands = {load, end};
			} else if (map_switch) {
				lcf::rpg::EventCommand teleport;
				teleport.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Teleport);
				teleport.parameters = {2, 10, 7};
				output_commands = {output_commands[0], output_commands[1], output_commands[2], teleport, end};
			} else if (reduced_save_probe) {
				const auto prefix_end = output_commands.begin() + (map_self_only ? 3 : 2);
				std::vector<lcf::rpg::EventCommand> reduced(output_commands.begin(), prefix_end);
				if (map_self_string || map_self_strings) {
					lcf::rpg::EventCommand set_self_string = set_common_name_string;
					set_self_string.parameters = {0x1003, 0, 0, 1, 0, 0, 11, 0x10, 1, 0};
					set_self_string.string = lcf::DBString("Bank sample");
					reduced.push_back(set_self_string);
					if (map_self_strings) {
						set_self_string.parameters[3] = 3;
						set_self_string.parameters[6] = 1;
						set_self_string.string = lcf::DBString("Z");
						reduced.push_back(set_self_string);
					}
				}
				if (map_self_two_events || map_self_two_maps) {
					lcf::rpg::EventCommand set_second_event = set_self;
					set_second_event.parameters = {0x1002, 2, map_self_two_maps ? 2 : 1, 1, 0, 0, 43, 0, 1, 0};
					reduced.push_back(set_second_event);
				}
				if (map_self_resume) {
					lcf::rpg::EventCommand read_before_save = read_self;
					read_before_save.parameters = {0, 3, 0, 0, 1, -1, 0};
					reduced.push_back(read_before_save);
				}
				reduced.push_back(save_command);
				if (map_self_resume) {
					lcf::rpg::EventCommand read_after_save = read_self;
					read_after_save.parameters = {0, 4, 0, 0, 1, -1, 0};
					reduced.push_back(read_after_save);
					lcf::rpg::EventCommand wait_after_read;
					wait_after_read.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
					wait_after_read.parameters = {50};
					reduced.push_back(wait_after_read);
					reduced.push_back(end);
				} else {
					reduced.insert(reduced.end(), output_commands.end() - 3, output_commands.end());
				}
				output_commands = std::move(reduced);
			} else {
				const auto insertion_index = output_commands.size() - 3;
				output_commands.insert(output_commands.begin() + insertion_index, save_command);
				if (negative_result) {
					lcf::rpg::EventCommand read_result = read_self;
					read_result.parameters = {0, 37, 0, 0, 1, -6, 0};
					output_commands.insert(output_commands.begin() + insertion_index + 1, read_result);
					lcf::rpg::EventCommand save_after_result = save_command;
					save_after_result.parameters = {0, 2, 0, 0};
					output_commands.insert(output_commands.begin() + insertion_index + 2, save_after_result);
				}
			}
		}
		lcf::rpg::TreeMap output_tree = tree;
		if (load_boundaries) {
			lcf::rpg::Event restored_event = output_map.events.front();
			restored_event.ID = 2;
			restored_event.pages.front().trigger = lcf::rpg::EventPage::Trigger_action;
			lcf::rpg::EventCommand marker = read_self;
			marker.parameters = {0, 1, 0, 0, 0, 707, 0};
			lcf::rpg::EventCommand seed_save;
			seed_save.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save);
			seed_save.parameters = {0, 7, 0, 0};
			lcf::rpg::EventCommand pause;
			pause.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
			pause.parameters = {50};
			lcf::rpg::EventCommand resumed_marker = marker;
			resumed_marker.parameters[1] = 10;
			resumed_marker.parameters[5] = 99;
			lcf::rpg::EventCommand resumed_save = seed_save;
			resumed_save.parameters[1] = 8;
			lcf::rpg::EventCommand final_pause = pause;
			final_pause.parameters = {9999};
			restored_event.pages.front().event_commands =
				{marker, seed_save, pause, resumed_marker, resumed_save, final_pause, end};
			output_map.events.push_back(std::move(restored_event));
		}
		std::unique_ptr<lcf::rpg::Map> output_map2;
		if (save_probe && save_variant && std::string_view(save_variant) == "map-switch") {
			output_tree.maps.emplace_back();
			output_tree.maps.back().ID = 2;
			output_tree.maps.back().name = lcf::DBString("Second map");
			output_tree.maps.back().type = lcf::rpg::TreeMap::MapType_map;
			output_tree.tree_order.push_back(2);
			output_map2 = std::make_unique<lcf::rpg::Map>(map);
			output_map2->events.front().ID = 2;
			auto& second_commands = output_map2->events.front().pages.front().event_commands;
			lcf::rpg::EventCommand set_second = set_self;
			set_second.parameters[6] = 43;
			lcf::rpg::EventCommand save_second;
			save_second.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save);
			save_second.parameters = {0, 1, 0, 0};
			second_commands = {set_second, save_second, end};
		}
		lcf::rpg::Database database;
		database.actors.emplace_back();
		database.actors.back().Setup(true);
		database.actors.back().ID = 1;
		database.actors.back().name = lcf::DBString("Probe");
		database.chipsets.emplace_back();
		database.chipsets.back().ID = 1;
		database.chipsets.back().name = lcf::DBString("Probe tiles");
		database.chipsets.back().chipset_name = lcf::DBString("probe");
		database.system.ldb_id = 2003;
		database.system.party = {1};
		database.system.show_title = false;
		database.commonevents.push_back(common);
		database.commonevents.push_back(switch_common);
		if (end_command_frames_auto || end_command_frames_parallel || end_nest_auto_common || opcode40_auto_common) {
			lcf::rpg::CommonEvent nested;
			nested.ID = 3;
			nested.name = lcf::DBString("End Command nested frame");
			if (end_nest_auto_common || opcode40_auto_common) {
				lcf::rpg::EventCommand nested_end = common_end;
				if (opcode40_auto_common) nested_end.code = 40;
				nested_end.parameters = {7, 99, -3};
				nested_end.string = lcf::DBString("IGNORED_END_NEST_TEXT");
				nested.event_commands.assign(10050, nested_end);
				lcf::rpg::EventCommand nested_marker = read_self;
				nested_marker.parameters = {0, 2, 0, 0, 0, 77, 0};
				nested.event_commands.insert(nested.event_commands.end(), {nested_marker, common_end});
				database.commonevents.push_back(std::move(nested));
			} else {
				lcf::rpg::EventCommand nested_count = read_self;
				nested_count.parameters = {0, -1, 0, 1, 0, 1, 0};
				lcf::rpg::EventCommand copy_nested = read_self;
				copy_nested.parameters = {0, 3, 0, 0, 1, -1, 0};
				nested.event_commands = {nested_count, copy_nested, common_end};
				database.commonevents.push_back(std::move(nested));
			}
		}
		if (save_probe && save_variant && std::string_view(save_variant) == "js-host-negative") {
			lcf::rpg::CommonEvent probe;
			probe.ID = 3;
			probe.name = lcf::DBString("JS negative common");
			lcf::rpg::EventCommand absent;
			absent.code = 3030;
			absent.parameters = {0, 0, 0, 0, 1, 4, 3};
			absent.string = lcf::DBString(
				"(()=>{let r=[];for(let f of [getv,gets,gett])"
				"{try{r.push(f(-1))}catch(e){r.push('ERR:'+e)}}"
				"return JSON.stringify(r)})()");
			lcf::rpg::EventCommand mutate = absent;
			mutate.parameters[6] = 4;
			mutate.string = lcf::DBString(
				"(()=>{let n=getv(1);setv(-1,33);sets(-1,1);sett(-1,'C');"
				"let a=[n,getv(-1),getv(n),gets(-1),gets(n),gett(-1),gett(n)];"
				"setv(n,35);sets(n,0);sett(n,'CI');"
				"a.push(getv(-1),gets(-1),gett(-1));return JSON.stringify(a)})()");
			probe.event_commands = {absent, mutate, common_end};
			database.commonevents.push_back(std::move(probe));
		}
		if (noop_battle || end_nest_battle || opcode40_battle || common1005_battle || wait_battle || end_command_battle) {
			auto& attack_command = database.battlecommands.commands.emplace_back();
			attack_command.ID = 1;
			attack_command.name = lcf::DBString("Attack");
			database.actors.front().battle_commands = {1};
			auto& terrain = database.terrains.emplace_back();
			terrain.ID = 1;
			terrain.name = lcf::DBString("Probe terrain");
			terrain.background_name = lcf::DBString("probe");
			auto& animation = database.battleranimations.emplace_back();
			animation.ID = 1;
			// RPG_RT preloads all 32 poses before executing a troop page.
			for (int index = 1; index <= 32; ++index) {
				auto& pose = animation.poses.emplace_back();
				pose.ID = index;
				pose.battler_name = lcf::DBString("probe");
			}
			auto& enemy = database.enemies.emplace_back();
			enemy.ID = 1;
			enemy.name = lcf::DBString("Probe enemy");
			enemy.battler_name = lcf::DBString("probe");
			enemy.max_hp = 100;
			auto& troop = database.troops.emplace_back();
			troop.ID = 1;
			troop.name = lcf::DBString("No-op battle probe");
			auto& member = troop.members.emplace_back();
			member.ID = 1;
			member.enemy_id = 1;
			member.x = 160;
			member.y = 110;
			auto& battle_page = troop.pages.emplace_back();
			battle_page.ID = 1;
			battle_page.condition.flags.variable = true;
			battle_page.condition.variable_id = 10;
			battle_page.condition.variable_value = 110;
			for (int index = 0; index < ((end_nest_battle || opcode40_battle || end_command_battle) ? 0 : 9); ++index) {
				lcf::rpg::EventCommand noop;
				noop.code = index % 3 == 0 ? 3024 : index % 3 == 1 ? 3034 : 3035;
				if (index / 3 == 1) noop.parameters = {7};
				if (index / 3 == 2) {
					noop.parameters = {7, 99, -3};
					noop.string = lcf::DBString("DO_NOT_EXECUTE");
				}
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 1 + index, 0, 0, 0, 11 * (index + 1), 0};
				battle_page.event_commands.push_back(noop);
				battle_page.event_commands.push_back(marker);
			}
			if (end_nest_battle || opcode40_battle) {
				lcf::rpg::EventCommand nested_end = common_end;
				if (opcode40_battle) nested_end.code = 40;
				nested_end.parameters = {7, 99, -3};
				nested_end.string = lcf::DBString("IGNORED_END_NEST_TEXT");
				battle_page.event_commands.assign(10050, nested_end);
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 1, 0, 0, 0, 99, 0};
				battle_page.event_commands.push_back(marker);
			}
			lcf::rpg::EventCommand battle_save;
			battle_save.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save);
			battle_save.parameters = {0, 1, 0, 0};
			battle_page.event_commands.push_back(battle_save);
			if (!end_command_battle) {
				lcf::rpg::EventCommand terminate;
				terminate.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::TerminateBattle);
				battle_page.event_commands.push_back(terminate);
			}
			battle_page.event_commands.push_back(common_end);
			if (end_command_battle) {
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 1, 0, 0, 0, 99, 0};
				battle_page.event_commands.insert(battle_page.event_commands.begin(), marker);
			}
		}
		if (noop_long_skip) {
			auto& commands = database.commonevents.front().event_commands;
			commands.clear();
			for (int index = 0; index < 3; ++index) {
				lcf::rpg::EventCommand noop;
				noop.code = index == 0 ? 3024 : index == 1 ? 3034 : 3035;
				noop.parameters = {7, 99, -3};
				noop.string = lcf::DBString("DO_NOT_EXECUTE");
				lcf::rpg::EventCommand marker = read_self;
				marker.parameters = {0, 3 + index, 0, 0, 0, 33 + index * 11, 0};
				commands.push_back(noop);
				commands.push_back(marker);
			}
			commands.push_back(common_end);
		}
		if (save_probe && save_variant && (std::string_view(save_variant) == "recursive-self"
				|| std::string_view(save_variant) == "recursive-self-save")) {
			auto& commands = database.commonevents.front().event_commands;
			lcf::rpg::EventCommand inner_if;
			inner_if.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ConditionalBranch);
			inner_if.parameters = {1, 3, 0, 1, 0, 0};
			lcf::rpg::EventCommand outer_if = inner_if;
			outer_if.parameters[3] = 0;
			lcf::rpg::EventCommand branch_end;
			branch_end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::EndBranch);
			lcf::rpg::EventCommand inner_read = read_self;
			inner_read.indent = 1;
			inner_read.parameters[1] = 4;
			lcf::rpg::EventCommand inner_set = set_self;
			inner_set.indent = 1;
			inner_set.parameters[6] = 43;
			lcf::rpg::EventCommand outer_set = set_self;
			outer_set.indent = 1;
			lcf::rpg::EventCommand guard = read_self;
			guard.indent = 1;
			guard.parameters = {0, 3, 0, 0, 0, 1, 0};
			lcf::rpg::EventCommand recursive_call;
			recursive_call.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
			recursive_call.indent = 1;
			recursive_call.parameters = {0, 1, 0};
			lcf::rpg::EventCommand outer_read = read_self;
			outer_read.indent = 1;
			outer_read.parameters[1] = 5;
			commands = {inner_if, inner_read, inner_set, branch_end,
				outer_if, outer_set, guard, recursive_call, outer_read, branch_end, common_end};
		}
		if (common1005) {
			const auto value = [&](int id, int number, int operation = 0, int source_mode = 0) {
				lcf::rpg::EventCommand command = read_self;
				command.parameters = {0, id, 0, operation, source_mode, number, 0};
				return command;
			};
			lcf::rpg::EventCommand call;
			call.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallCommonEvent);
			call.string = lcf::DBString("IGNORED_COMMON1005_TEXT");
			lcf::rpg::EventCommand save;
			save.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save);
			save.parameters = {0, 1, 0, 0};
			lcf::rpg::EventCommand wait;
			wait.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
			wait.parameters = {9999};
			std::vector<lcf::rpg::EventCommand> sequence;
			for (const auto& [slot, number] : std::vector<std::pair<int, int>>{{1, 2}, {2, 1}, {3, -1}}) {
				lcf::rpg::EventCommand set = set_self;
				set.parameters[3] = slot;
				set.parameters[6] = number;
				sequence.push_back(set);
			}
			sequence.insert(sequence.end(), {value(30, 1), value(31, 30), value(32, -1)});
			std::vector<std::vector<int32_t>> records = {{1}, {2, 0xf0, 777, -3},
				{30, 1}, {-1, 1}, {30, 0x12345001}};
			for (int mode = 2; mode <= 15; ++mode) {
				for (const int operand : {31, 32, -2, -3}) records.push_back({operand, mode, 777, -3});
			}
			records.push_back({31, 0x12345002, 777, -3});
			for (size_t index = 0; index < records.size(); ++index) {
				call.parameters = lcf::DBArray<int32_t>(records[index].begin(), records[index].end());
				sequence.push_back(call);
				sequence.push_back(value(100 + index, 1, 0, 1));
			}
			sequence.push_back(value(4, 99));
			database.commonevents.clear();
			for (int id = 1; id <= 3; ++id) {
				lcf::rpg::CommonEvent probe;
				probe.ID = id;
				probe.name = lcf::DBString("1005 probe");
				probe.event_commands = id == 3 ? sequence
					: std::vector<lcf::rpg::EventCommand>{value(1, id, 1), value(id + 1, 1, 1)};
				probe.event_commands.push_back(common_end);
				database.commonevents.push_back(std::move(probe));
			}
			call.parameters = {3, 0xf0, 777, -3};
			if (common1005_battle) {
				auto& commands = database.troops.front().pages.front().event_commands;
				commands = {call, save, wait, common_end};
			} else {
				auto& page = output_map.events.front().pages.front();
				page.event_commands = std::string_view(save_variant) == "common1005-nested"
					? std::vector<lcf::rpg::EventCommand>{call} : sequence;
				page.event_commands.insert(page.event_commands.end(), {save, wait, end});
				if (std::string_view(save_variant) == "common1005-parallel") {
					page.trigger = lcf::rpg::EventPage::Trigger_parallel;
				}
			}
		}
		if (map_wait) {
			const auto value = [&](int id, int number, int operation = 0) {
				lcf::rpg::EventCommand command = read_self;
				command.parameters = {0, id, 0, operation, 0, number, 0};
				return command;
			};
			const auto wait = [&](int tenths) {
				lcf::rpg::EventCommand command;
				command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
				command.parameters = {tenths};
				return command;
			};
			const auto save = [&](int slot) {
				lcf::rpg::EventCommand command;
				command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save);
				command.parameters = {0, slot, 0, 0};
				return command;
			};
			lcf::rpg::EventCommand load;
			load.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Load);
			load.parameters = {0, 1, 0};
			lcf::rpg::EventCommand call;
			call.code = static_cast<int32_t>(call12330_wait
				? lcf::rpg::EventCommand::Code::CallEvent
				: lcf::rpg::EventCommand::Code::CallCommonEvent);
			call.parameters = call12330_wait ? lcf::DBArray<int32_t>{0, 1, 0, 0, 0, 0, 0}
				: lcf::DBArray<int32_t>{1};
			// The first run has no Save01, so this load is skipped. A fresh
			// process executes it and resumes the saved active common frame.
			output_map.events.front().pages.front().event_commands = {
				load, value(1, 10), call, value(1, 1, 1), value(2, 20),
				save(2), wait(9999), end};
			if (wait_nested) {
				output_map.events.front().pages.front().event_commands[2].parameters = call12330_wait
					? lcf::DBArray<int32_t>{0, 2, 0, 0, 0, 0, 0} : lcf::DBArray<int32_t>{2};
			}
			if (wait_parallel) {
				auto parent = output_map.events.front();
				parent.ID = 3;
				parent.pages.front().trigger = lcf::rpg::EventPage::Trigger_parallel;
				parent.pages.front().event_commands = {
					value(1, 10), call, value(1, 1, 1), value(2, 20),
					save(2), wait(9999), end};
				output_map.events.push_back(std::move(parent));
				output_map.events.front().pages.front().event_commands = {load, wait(9999), end};
			}
			auto checkpoint = output_map.events.front();
			checkpoint.ID = 2;
			checkpoint.pages.front().trigger = lcf::rpg::EventPage::Trigger_parallel;
			checkpoint.pages.front().event_commands = {wait(3), save(1), wait(9999), end};
			output_map.events.push_back(std::move(checkpoint));
			std::sort(output_map.events.begin(), output_map.events.end(),
				[](const auto& left, const auto& right) { return left.ID < right.ID; });
			lcf::rpg::EventCommand self = set_self;
			self.parameters = {0x1003, 0, 0, 1, 0, 0, 42, 0, 1, 0};
			database.commonevents.front().event_commands = {
				self, value(3, 3), wait(50), value(3, 4, 1),
				value(4, 99), common_end};
			if (wait_nested) {
				auto& wrapper = database.commonevents.back().event_commands;
				lcf::rpg::EventCommand wrapper_self = self;
				wrapper_self.parameters[6] = 24;
				wrapper = {wrapper_self, value(5, 5), call, value(5, 1, 1),
					value(6, 88), common_end};
			}
		}
		if (wait_battle) {
			lcf::rpg::EventCommand call;
			call.code = static_cast<int32_t>(call12330_wait
				? lcf::rpg::EventCommand::Code::CallEvent : lcf::rpg::EventCommand::Code::CallCommonEvent);
			call.parameters = call12330_wait ? lcf::DBArray<int32_t>{0, 1, 0, 0, 0, 0, 0}
				: lcf::DBArray<int32_t>{1};
			lcf::rpg::EventCommand save;
			save.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save);
			save.parameters = {0, 1, 0, 0};
			lcf::rpg::EventCommand save_parent = save;
			save_parent.parameters[1] = 4;
			lcf::rpg::EventCommand wait;
			wait.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
			wait.parameters = {50};
			lcf::rpg::EventCommand marker = read_self;
			marker.parameters = {0, 3, 0, 0, 0, 3, 0};
			lcf::rpg::EventCommand done = marker;
			done.parameters = {0, 4, 0, 1, 0, 1, 0};
			lcf::rpg::EventCommand parent_done = marker;
			parent_done.parameters = {0, 5, 0, 1, 0, 1, 0};
			lcf::rpg::EventCommand disable_page = marker;
			disable_page.parameters = {0, 10, 0, 0, 0, 109, 0};
			lcf::rpg::EventCommand post_wait_save = save;
			post_wait_save.parameters[1] = 2;
			database.commonevents.front().event_commands = {
				set_self, marker, save, wait, done, post_wait_save, common_end};
			database.troops.front().pages.front().event_commands = {
				disable_page, call, parent_done, save_parent, common_end};
		}

		REQUIRE(lcf::LDB_Reader::Save((output / "RPG_RT.ldb").string(), database));
		REQUIRE(lcf::LMT_Reader::Save((output / "RPG_RT.lmt").string(), output_tree, lcf::EngineVersion::e2k3));
		REQUIRE(lcf::LMU_Reader::Save((output / "Map0001.lmu").string(), output_map, lcf::EngineVersion::e2k3));
		if (output_map2) REQUIRE(lcf::LMU_Reader::Save((output / "Map0002.lmu").string(), *output_map2, lcf::EngineVersion::e2k3));
		auto saved_database = lcf::LDB_Reader::Load((output / "RPG_RT.ldb").string());
		REQUIRE(saved_database != nullptr);
		if (common1005) {
			REQUIRE(saved_database->commonevents.size() == 3);
			const auto& sequence = saved_database->commonevents.back().event_commands;
			REQUIRE(sequence.size() == 132);
			int calls = 0;
			for (const auto& command : sequence) {
				if (command.code != 1005) continue;
				++calls;
				CHECK(command.string == lcf::DBString("IGNORED_COMMON1005_TEXT"));
			}
			CHECK(calls == 62);
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			if (common1005_battle) {
				CHECK(saved_database->troops.front().pages.front().event_commands[0].code == 1005);
			} else if (std::string_view(save_variant) == "common1005-nested") {
				CHECK(saved_map->events.front().pages.front().event_commands[0].code == 1005);
			}
			return;
		}
		if (map_wait) {
			REQUIRE(saved_database->commonevents.size() == 2);
			REQUIRE(saved_database->commonevents.front().event_commands.size() == 6);
			CHECK(saved_database->commonevents.front().event_commands[2].code ==
				static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait));
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == (wait_parallel ? 3 : 2));
			CHECK(saved_map->events[0].pages.front().event_commands[2].code ==
				(wait_parallel ? static_cast<int32_t>(lcf::rpg::EventCommand::Code::END)
				: call12330_wait ? static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent)
				                     : static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallCommonEvent)));
			CHECK(saved_map->events[1].pages.front().trigger == lcf::rpg::EventPage::Trigger_parallel);
			if (wait_parallel) {
				REQUIRE(saved_map->events.size() == 3);
				CHECK(saved_map->events[2].pages.front().trigger == lcf::rpg::EventPage::Trigger_parallel);
				CHECK(saved_map->events[2].pages.front().event_commands[1].code ==
					static_cast<int32_t>(call12330_wait ? lcf::rpg::EventCommand::Code::CallEvent
						: lcf::rpg::EventCommand::Code::CallCommonEvent));
			}
			return;
		}
		if (wait_battle) {
			REQUIRE(saved_database->commonevents.front().event_commands.size() == 7);
			CHECK(saved_database->commonevents.front().event_commands[2].code ==
				static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
			CHECK(saved_database->commonevents.front().event_commands[3].code ==
				static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait));
			CHECK(saved_database->commonevents.front().event_commands[5].code ==
				static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
			CHECK(saved_database->troops.front().pages.front().event_commands[1].code ==
				static_cast<int32_t>(call12330_wait ? lcf::rpg::EventCommand::Code::CallEvent
					: lcf::rpg::EventCommand::Code::CallCommonEvent));
			return;
		}
		if (save_variant && std::string_view(save_variant) == "call12330-receiver") {
			REQUIRE(saved_database->commonevents.size() == 2);
			CHECK(saved_database->commonevents.front().event_commands[5].parameters == lcf::DBArray<int32_t>{7, 2, -1});
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			CHECK(saved_map->events.front().pages.front().event_commands.size() == 245);
			return;
		}
		if (end_command_battle) {
			REQUIRE(saved_database->troops.size() == 1);
			const auto& commands = saved_database->troops.front().pages.front().event_commands;
			REQUIRE(commands.size() == 3);
			CHECK(commands[0].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars));
			CHECK(commands[1].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
			CHECK(commands[2].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::END));
		}
		CHECK(saved_database->actors.size() == 1);
		CHECK(saved_database->system.ldb_id == 2003);
		CHECK(saved_database->system.party == std::vector<int16_t>{1});
		CHECK(saved_database->system.show_title == false);
		REQUIRE(saved_database->commonevents.size() == (save_variant && (std::string_view(save_variant) == "js-host-negative"
				|| end_command_frames_auto || end_command_frames_parallel || end_nest_auto_common || opcode40_auto_common) ? 3 : 2));
		if (save_probe && save_variant && (std::string_view(save_variant) == "recursive-self"
				|| std::string_view(save_variant) == "recursive-self-save")) {
			const auto& commands = saved_database->commonevents.front().event_commands;
			REQUIRE(commands.size() == 11);
			CHECK(commands[0].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::ConditionalBranch));
			CHECK(commands[7].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent));
			CHECK(commands[7].indent == 1);
		} else if (!noop_long_skip) {
			REQUIRE(saved_database->commonevents.front().event_commands.size() == 4);
			CHECK(saved_database->commonevents.front().event_commands[0].parameters == read_argument.parameters);
			CHECK(saved_database->commonevents.front().event_commands[1].parameters == read_switch_argument.parameters);
			CHECK(saved_database->commonevents.front().event_commands[2].parameters == deliver_return.parameters);
		}
		REQUIRE(saved_database->commonevents.back().event_commands.size() == ((end_nest_auto_common || opcode40_auto_common) ? 10052 : 3));
		if (save_variant && std::string_view(save_variant) == "js-host-negative") {
			CHECK(saved_database->commonevents.back().ID == 3);
			CHECK(saved_database->commonevents.back().event_commands[0].code == 3030);
			CHECK(saved_database->commonevents.back().event_commands[1].code == 3030);
		}
		if (noop_battle) {
			REQUIRE(saved_database->enemies.size() == 1);
			REQUIRE(saved_database->battleranimations.size() == 1);
			CHECK(saved_database->battleranimations.front().poses.size() == 32);
			REQUIRE(saved_database->troops.size() == 1);
			REQUIRE(saved_database->troops.front().pages.size() == 1);
			const auto& battle_page = saved_database->troops.front().pages.front();
			CHECK(battle_page.condition.flags.variable);
			REQUIRE(battle_page.event_commands.size() == 21);
			for (int index = 0; index < 9; ++index) {
				const auto& noop = battle_page.event_commands[index * 2];
				CHECK(noop.code == (index % 3 == 0 ? 3024 : index % 3 == 1 ? 3034 : 3035));
				CHECK(noop.parameters.size() == (index / 3 == 0 ? 0 : index / 3 == 1 ? 1 : 3));
				CHECK(noop.string == lcf::DBString(index / 3 == 2 ? "DO_NOT_EXECUTE" : ""));
			}
			CHECK(battle_page.event_commands[18].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
		}
		CHECK(saved_database->commonevents[1].event_commands[0].parameters == redirect_indirect_global_address.parameters);
		CHECK(saved_database->commonevents[1].event_commands[1].parameters == deliver_switch_return.parameters);
		if (save_probe && save_variant && (std::string_view(save_variant) == "parallel-self"
				|| std::string_view(save_variant) == "parallel-self-save")) {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 2);
			CHECK(saved_map->events[0].ID == 1);
			CHECK(saved_map->events[1].ID == 2);
			for (const auto& event : saved_map->events) {
				REQUIRE(event.pages.size() == 1);
				CHECK(event.pages[0].trigger == lcf::rpg::EventPage::Trigger_parallel);
				CHECK(event.pages[0].event_commands.size() ==
					(std::string_view(save_variant) == "parallel-self-save" && event.ID == 1 ? 7 : 5));
			}
		}
		if (save_probe && save_variant &&
				(std::string_view(save_variant) == "legacy-controlvars" || std::string_view(save_variant) == "legacy-negative-self")) {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 1);
			const auto& commands = saved_map->events.front().pages.front().event_commands;
			const bool negative_self = std::string_view(save_variant) == "legacy-negative-self";
			REQUIRE(commands.size() == (negative_self ? 5 : 4));
			CHECK(commands[0].parameters.size() == 7);
			CHECK(commands[0].parameters[1] == (negative_self ? -1 : 1));
			CHECK(commands[0].parameters[5] == (negative_self ? 42 : 7));
			CHECK(commands[1].parameters.size() == 7);
			CHECK(commands[1].parameters[1] == (negative_self ? 1 : 2));
			CHECK(commands[1].parameters[4] == 1);
			CHECK(commands[1].parameters[5] == (negative_self ? -1 : 1));
			if (negative_self) {
				CHECK(commands[2].parameters[1] == 2);
				CHECK(commands[2].parameters[5] == 99);
			}
			CHECK(commands[negative_self ? 3 : 2].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
		}
		if (save_probe && save_variant && std::string_view(save_variant) == "string-self-source") {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 1);
			const auto& commands = saved_map->events.front().pages.front().event_commands;
			REQUIRE(commands.size() == 8);
			CHECK(commands[0].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Selfvar));
			CHECK(commands[0].string == lcf::DBString("Bank sample"));
			CHECK(commands[1].parameters[5] == -1);
			CHECK(commands[2].parameters[0] == 0x10);
			CHECK(commands[2].parameters[4] == -1);
			CHECK(commands[3].parameters[0] == 0x20);
			CHECK(commands[3].parameters[4] == 3);
			CHECK(commands[4].string == lcf::DBString("Literal"));
			CHECK(commands[6].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
		}
		if (save_probe && save_variant && std::string_view(save_variant) == "string-to-num-radix") {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 1);
			const auto& commands = saved_map->events.front().pages.front().event_commands;
			REQUIRE(commands.size() == 9);
			CHECK(commands[0].string == lcf::DBString("08"));
			CHECK(commands[1].string == lcf::DBString("0x10"));
			CHECK(commands[2].parameters[1] == 3);
			CHECK(commands[3].parameters[3] == 2);
			CHECK(commands[4].parameters[4] == 2);
			CHECK(commands[5].parameters[3] == ((1 << 17) | 2));
			CHECK(commands[7].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
		}
		if (save_probe && save_variant && std::string_view(save_variant) == "string-pop-line") {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 1);
			const auto& commands = saved_map->events.front().pages.front().event_commands;
			REQUIRE(commands.size() == 10);
			CHECK(commands[0].string == lcf::DBString("A\r\nB"));
			CHECK(commands[1].parameters[3] == 8);
			CHECK(commands[1].parameters[4] == 2);
			CHECK(commands[2].parameters[4] == 3);
			CHECK(commands[4].parameters[1] == 4);
			CHECK(commands[4].parameters[4] == 4);
			CHECK(commands[5].string == lcf::DBString("Y"));
			CHECK(commands[6].parameters[0] == 0x10);
			CHECK(commands[6].parameters[4] == 4);
			CHECK(commands[8].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
		}
		if (save_probe && save_variant && std::string_view(save_variant) == "string-pop-line-range") {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 1);
			const auto& commands = saved_map->events.front().pages.front().event_commands;
			REQUIRE(commands.size() == 7);
			CHECK(commands[0].string == lcf::DBString("A\r\nB"));
			CHECK(commands[1].string == lcf::DBString("C\nD"));
			CHECK(commands[2].string == lcf::DBString("E"));
			REQUIRE(commands[3].parameters.size() == 8);
			CHECK(commands[3].parameters[0] == 1);
			CHECK(commands[3].parameters[1] == 1);
			CHECK(commands[3].parameters[2] == 3);
			CHECK(commands[3].parameters[3] == 8);
			CHECK(commands[3].parameters[4] == 5);
			CHECK(commands[5].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
		}
		if (save_probe && save_variant && std::string_view(save_variant) == "js-host-types") {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 1);
			const auto& commands = saved_map->events.front().pages.front().event_commands;
			REQUIRE(commands.size() == 5);
			CHECK(commands[0].code == 3030);
			CHECK(commands[0].parameters.size() == 23);
			CHECK(commands[0].parameters[4] == 9);
			CHECK(commands[0].string == lcf::DBString(
				"[typeof rm2k3, typeof console, typeof input, typeof include, "
				"typeof rm2k3.getv, typeof rm2k3.gets, typeof rm2k3.gett, "
				"typeof rm2k3.setv, typeof rm2k3.getPicture]"));
			CHECK(commands[1].code == 3030);
			CHECK(commands[1].parameters[6] == 10);
			CHECK(commands[1].parameters[22] == 18);
			CHECK(commands[1].string == lcf::DBString(
				"[typeof getv, typeof gets, typeof gett, typeof setv, "
				"typeof sets, typeof sett, typeof getPicture, typeof log, "
				"typeof rm2k3.getPicture]"));
			CHECK(commands[3].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
		}
		if (save_probe && save_variant && std::string_view(save_variant) == "js-host-accessors") {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 1);
			const auto& commands = saved_map->events.front().pages.front().event_commands;
			REQUIRE(commands.size() == 5);
			CHECK(commands[0].code == 3030);
			CHECK(commands[0].parameters[4] == 5);
			CHECK(commands[0].string == lcf::DBString(
				"[setv(10,42), sets(10,1), sett(10,'Host'), getv(10), gets(10)]"));
			CHECK(commands[1].code == 3030);
			CHECK(commands[1].parameters[4] == 3);
			CHECK(commands[1].string == lcf::DBString("[getv(10), gets(10), gett(10)]"));
			CHECK(commands[3].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
		}
		if (save_probe && save_variant && std::string_view(save_variant) == "js-exception-continue") {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 1);
			const auto& commands = saved_map->events.front().pages.front().event_commands;
			REQUIRE(commands.size() == 5);
			CHECK(commands[0].code == 3030);
			CHECK(commands[0].parameters[4] == 2);
			CHECK(commands[0].string == lcf::DBString("throw new Error('boom')"));
			CHECK(commands[1].code == 3030);
			CHECK(commands[1].string == lcf::DBString("40+2"));
			CHECK(commands[3].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
		}
		if (save_probe && save_variant && std::string_view(save_variant) == "js-host-inventory") {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 1);
			const auto& commands = saved_map->events.front().pages.front().event_commands;
			REQUIRE(commands.size() == 4);
			CHECK(commands[0].code == 3030);
			CHECK(commands[0].parameters.size() == 15);
			CHECK(commands[0].parameters[4] == 5);
			CHECK(std::string_view(commands[0].string).find("Object.getOwnPropertyNames(globalThis)") != std::string_view::npos);
			CHECK(commands[2].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
		}
		if (save_probe && save_variant && std::string_view(save_variant) == "js-host-symbols") {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 1);
			const auto& commands = saved_map->events.front().pages.front().event_commands;
			REQUIRE(commands.size() == 4);
			CHECK(commands[0].code == 3030);
			REQUIRE(commands[0].parameters.size() == 7);
			CHECK(commands[0].parameters[4] == 1);
			CHECK(commands[0].parameters[5] == 4);
			CHECK(commands[0].parameters[6] == 1);
			CHECK(std::string_view(commands[0].string).find("Reflect.ownKeys") != std::string_view::npos);
			CHECK(commands[2].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
		}
		if (save_probe && save_variant && std::string_view(save_variant) == "reserved2-noop") {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 1);
			const auto& commands = saved_map->events.front().pages.front().event_commands;
			REQUIRE(commands.size() == 4);
			CHECK(commands[0].code == 3024);
			CHECK(commands[0].parameters.size() == 3);
			CHECK(commands[0].string == lcf::DBString("DO_NOT_EXECUTE"));
			CHECK(commands[1].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars));
			CHECK(commands[2].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
		}
		if (save_probe && save_variant && std::string_view(save_variant) == "reserved-noop") {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 1);
			const auto& commands = saved_map->events.front().pages.front().event_commands;
			REQUIRE(commands.size() == 4);
			CHECK(commands[0].code == 3034);
			REQUIRE(commands[0].parameters.size() == 3);
			CHECK(commands[0].parameters[0] == 7);
			CHECK(commands[0].parameters[1] == 99);
			CHECK(commands[0].parameters[2] == -3);
			CHECK(commands[0].string == lcf::DBString("DO_NOT_EXECUTE"));
			CHECK(commands[1].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars));
			CHECK(commands[2].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
		}
		if (save_probe && save_variant && std::string_view(save_variant) == "drawshape-noop") {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 1);
			const auto& commands = saved_map->events.front().pages.front().event_commands;
			REQUIRE(commands.size() == 4);
			CHECK(commands[0].code == 3035);
			REQUIRE(commands[0].parameters.size() == 3);
			CHECK(commands[0].parameters[0] == 7);
			CHECK(commands[0].parameters[1] == 99);
			CHECK(commands[0].parameters[2] == -3);
			CHECK(commands[0].string == lcf::DBString("DO_NOT_EXECUTE"));
			CHECK(commands[1].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars));
			CHECK(commands[2].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
		}
		if (save_probe && save_variant && std::string_view(save_variant) == "noop-long-skip") {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 2);
			const auto& first = saved_map->events[0].pages.front();
			const auto& second = saved_map->events[1].pages.front();
			CHECK(first.trigger == lcf::rpg::EventPage::Trigger_parallel);
			CHECK(second.trigger == lcf::rpg::EventPage::Trigger_parallel);
			REQUIRE(first.event_commands.size() == 10054);
			CHECK(first.event_commands[0].code == 3024);
			CHECK(first.event_commands[1].code == 3035);
			CHECK(first.event_commands[10049].code == 3035);
			CHECK(first.event_commands[10050].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars));
			CHECK(first.event_commands[0].parameters.size() == 3);
			CHECK(first.event_commands[0].parameters[0] == 7);
			CHECK(first.event_commands[0].parameters[1] == 99);
			CHECK(first.event_commands[0].parameters[2] == -3);
			CHECK(first.event_commands[0].string == lcf::DBString("DO_NOT_EXECUTE"));
			REQUIRE(second.event_commands.size() == 4);
			CHECK(second.event_commands[0].parameters.size() == 7);
			CHECK(second.event_commands[0].parameters[1] == 2);
			CHECK(second.event_commands[0].parameters[4] == 1);
			CHECK(second.event_commands[0].parameters[5] == 1);
			CHECK(second.event_commands[1].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
			const auto& common_commands = saved_database->commonevents.front().event_commands;
			REQUIRE(common_commands.size() == 7);
			CHECK(common_commands[0].code == 3024);
			CHECK(common_commands[2].code == 3034);
			CHECK(common_commands[4].code == 3035);
		}
		if (save_probe && (end_nest_long_skip || opcode40_long_skip)) {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 2);
			const auto& first = saved_map->events[0].pages.front();
			const auto& second = saved_map->events[1].pages.front();
			CHECK(first.trigger == lcf::rpg::EventPage::Trigger_parallel);
			CHECK(second.trigger == lcf::rpg::EventPage::Trigger_parallel);
			REQUIRE(first.event_commands.size() == 10053);
			CHECK(first.event_commands[0].code == (opcode40_long_skip ? 40 : static_cast<int32_t>(lcf::rpg::EventCommand::Code::END)));
			CHECK(first.event_commands[10049].code == (opcode40_long_skip ? 40 : static_cast<int32_t>(lcf::rpg::EventCommand::Code::END)));
			REQUIRE(first.event_commands[0].parameters.size() == 3);
			CHECK(first.event_commands[0].parameters[0] == 7);
			CHECK(first.event_commands[0].parameters[1] == 99);
			CHECK(first.event_commands[0].parameters[2] == -3);
			CHECK(first.event_commands[0].string == lcf::DBString("IGNORED_END_NEST_TEXT"));
			CHECK(first.event_commands[10050].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars));
			REQUIRE(second.event_commands.size() == 4);
			CHECK(second.event_commands[1].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
		}
		if (save_probe && (end_nest_auto_common || opcode40_auto_common)) {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			const auto& parent = saved_map->events.front().pages.front().event_commands;
			REQUIRE(parent.size() == 14);
			CHECK(parent[0].code == (opcode40_auto_common ? 40 : static_cast<int32_t>(lcf::rpg::EventCommand::Code::END)));
			CHECK(parent[2].indent == 1);
			CHECK(parent[6].indent == 1);
			CHECK(parent[9].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent));
			CHECK(parent[11].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
			const auto& nested = saved_database->commonevents.back().event_commands;
			REQUIRE(nested.size() == 10052);
			CHECK(nested[0].code == (opcode40_auto_common ? 40 : static_cast<int32_t>(lcf::rpg::EventCommand::Code::END)));
			CHECK(nested[10049].code == (opcode40_auto_common ? 40 : static_cast<int32_t>(lcf::rpg::EventCommand::Code::END)));
			REQUIRE(nested[0].parameters.size() == 3);
			CHECK(nested[0].string == lcf::DBString("IGNORED_END_NEST_TEXT"));
			CHECK(nested[10050].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars));
		}
		if (save_probe && (end_nest_battle || opcode40_battle)) {
			REQUIRE(saved_database->troops.size() == 1);
			const auto& commands = saved_database->troops.front().pages.front().event_commands;
			REQUIRE(commands.size() == 10054);
			CHECK(commands[0].code == (opcode40_battle ? 40 : static_cast<int32_t>(lcf::rpg::EventCommand::Code::END)));
			CHECK(commands[10049].code == (opcode40_battle ? 40 : static_cast<int32_t>(lcf::rpg::EventCommand::Code::END)));
			REQUIRE(commands[0].parameters.size() == 3);
			CHECK(commands[0].string == lcf::DBString("IGNORED_END_NEST_TEXT"));
			CHECK(commands[10050].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars));
			CHECK(commands[10051].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
		}
		if (save_probe && save_boundaries) {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			const auto& commands = saved_map->events.front().pages.front().event_commands;
			std::vector<size_t> save_lengths;
			for (const auto& command : commands) {
				if (command.code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save)) {
					save_lengths.push_back(command.parameters.size());
					CHECK(command.string == lcf::DBString("IGNORED_SAVE_TEXT"));
				}
			}
			CHECK(save_lengths == std::vector<size_t>{4, 4, 4, 0, 1, 1, 2, 3, 4, 4, 2, 3, 4});
		}
		if (save_probe && noop_battle) {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 1);
			const auto& map_page = saved_map->events.front().pages.front();
			CHECK(map_page.trigger == lcf::rpg::EventPage::Trigger_auto_start);
			REQUIRE(map_page.event_commands.size() == 3);
			CHECK(map_page.event_commands[0].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars));
			CHECK(map_page.event_commands[0].parameters[1] == 10);
			CHECK(map_page.event_commands[0].parameters[5] == 110);
			CHECK(map_page.event_commands[1].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::EnemyEncounter));
		}
		if (save_probe && save_variant && std::string_view(save_variant) == "js-title-cycle") {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 1);
			const auto& commands = saved_map->events.front().pages.front().event_commands;
			REQUIRE(commands.size() == 7);
			CHECK(commands[0].code == 3030);
			CHECK(commands[0].string == lcf::DBString(
				"globalThis.__maniac_context_probe === undefined ? 0 : globalThis.__maniac_context_probe"));
			CHECK(commands[2].code == 3030);
			CHECK(commands[2].parameters[4] == 0);
			CHECK(commands[3].string == lcf::DBString("globalThis.__maniac_context_probe"));
			CHECK(commands[5].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::ReturntoTitleScreen));
		}
		if (save_probe && save_variant && std::string_view(save_variant) == "js-load-cycle") {
			auto saved_map = lcf::LMU_Reader::Load((output / "Map0001.lmu").string());
			REQUIRE(saved_map != nullptr);
			REQUIRE(saved_map->events.size() == 1);
			const auto& commands = saved_map->events.front().pages.front().event_commands;
			REQUIRE(commands.size() == 9);
			CHECK(commands[0].string == lcf::DBString("globalThis.__maniac_load_probe = 42"));
			CHECK(commands[1].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save));
			CHECK(commands[2].code == 3030);
			CHECK(commands[2].parameters[6] == 3);
			CHECK(commands[2].string == lcf::DBString(
				"globalThis.__maniac_load_probe === undefined ? 99 : globalThis.__maniac_load_probe"));
			CHECK(commands[4].string == lcf::DBString("globalThis.__maniac_load_probe = 43"));
			CHECK(commands[5].parameters[6] == 4);
			CHECK(commands[7].code == static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Load));
		}
	}
}

TEST_CASE("Maniac original self banks decode compressed switches and empty string runs") {
	// Frozen 250921 receiver probe: leading false/NULL slots are a signed run.
	const std::vector<uint8_t> switches = {0xff, 0xff, 0xff, 0xff, 0x7e, 1, 1, 1};
	const std::vector<uint8_t> strings = {0xff, 0xff, 0xff, 0xff, 0x7e, 2, 0x37, 0, 0x31, 0,
		0xff, 0xff, 0xff, 0xff, 0x7f, 2, 0x37, 0, 0x31, 0};
	std::vector<uint8_t> bytes;
	ManiacOriginalSave::PutField(bytes, 1, switches);
	ManiacOriginalSave::PutField(bytes, 3, strings);
	bytes.push_back(0);
	ManiacOriginalSave::Cursor cursor(bytes.data(), bytes.size());
	Game_ManiacSelfVariables::Bank bank;
	REQUIRE(ManiacOriginalSave::Leaf(cursor, bank));
	CHECK(bank.switches == std::vector<uint8_t>{0, 0, 0, 1, 0, 1});
	CHECK(bank.strings.size() == 6);
	CHECK(bank.strings[3] == lcf::DBString("71"));
	CHECK(bank.strings[5] == lcf::DBString("71"));
	CHECK_FALSE(Game_ManiacSelfVariables::IsStringPresent(&bank, 1));
	CHECK_FALSE(Game_ManiacSelfVariables::IsStringPresent(&bank, 2));
	std::vector<uint8_t> encoded;
	REQUIRE(ManiacOriginalSave::EncodeLeaf(bank, encoded));
	CHECK(encoded == bytes);
	// Original interchange normalizes raw bytes; Player's own bank is untouched.
	bank.switches.assign(260, 3);
	bank.switches[0] = 0;
	encoded.clear();
	REQUIRE(ManiacOriginalSave::EncodeLeaf(bank, encoded));
	ManiacOriginalSave::Cursor large_cursor(encoded.data(), encoded.size());
	Game_ManiacSelfVariables::Bank normalized;
	REQUIRE(ManiacOriginalSave::Leaf(large_cursor, normalized));
	CHECK(normalized.switches.size() == 260);
	CHECK(normalized.switches[259] == 1);
	CHECK(bank.switches[259] == 3);
	const std::vector<uint8_t> bad_run = {1, 1, 0, 0};
	ManiacOriginalSave::Cursor bad_cursor(bad_run.data(), bad_run.size());
	Game_ManiacSelfVariables::Bank bad_bank;
	CHECK_FALSE(ManiacOriginalSave::Leaf(bad_cursor, bad_bank));
}

TEST_CASE("Maniac original sparse receiver save imports and reencodes exactly") {
	const char* path = std::getenv("MANIAC_READ_ORIGINAL_RECEIVER_SAVE");
	if (!path || !*path) return;
	auto save = lcf::LSD_Reader::Load(path);
	REQUIRE(save != nullptr);
	REQUIRE(ManiacOriginalSave::ImportObservedBanks(*save));
	Game_ManiacSelfVariables state;
	state.SetSaveData(save->easyrpg_data.maniac_self_variable_banks);
	const auto* caller = state.Find(2, 1, 1);
	REQUIRE(caller != nullptr);
	CHECK(Game_ManiacSelfVariables::ReadNumber(caller, -3) == 71);
	CHECK(Game_ManiacSelfVariables::ReadSwitch(caller, -3) == 1);
	CHECK(Game_ManiacSelfVariables::ReadSwitch(caller, -5) == 1);
	CHECK(caller->strings[3] == lcf::DBString("71"));
	CHECK(caller->strings[5] == lcf::DBString("71"));
	std::vector<uint8_t> encoded;
	REQUIRE(ManiacOriginalSave::EncodeObservedBanks(state.GetSaveData(), encoded));
	CHECK(encoded == save->maniac_original_bank_bytes);
}

TEST_CASE("Maniac original save fixture exposes serialized state") {
	const char* save_path = std::getenv("MANIAC_READ_ORIGINAL_SAVE");
	if (!save_path || !*save_path) return;
	auto save = lcf::LSD_Reader::Load(save_path);
	REQUIRE(save != nullptr);
	REQUIRE(save->system.variables.size() == 36);
	CHECK(save->system.variables[0] == 7); // Ordinary ID 1; physical slot zero is not represented here.
	CHECK(save->system.variables[1] == 42);
	CHECK(save->system.variables[35] == 3); // ID 36 is the last pre-save variable.
	CHECK(save->system.switches.size() == 4);
	REQUIRE(save->foreground_event_execstate.stack.size() == 1);
	CHECK(save->foreground_event_execstate.stack[0].commands.size() == 54);
	CHECK(save->foreground_event_execstate.stack[0].current_command == 51);
	CHECK(save->foreground_event_execstate.stack[0].easyrpg_self_scope == 0);
	CHECK(save->easyrpg_data.maniac_self_variable_banks.empty()); // Raw original bytes are not runtime banks yet.
	REQUIRE(save->maniac_original_bank_bytes.size() == 53);
	CHECK(save->maniac_original_bank_bytes.front() == 1); // Common bank child ID.
	CHECK(save->maniac_original_bank_bytes[1] == 17); // Common bank child payload size.
	const char* roundtrip_path = std::getenv("MANIAC_WRITE_ORIGINAL_SAVE_ROUNDTRIP");
	if (roundtrip_path && *roundtrip_path) {
		REQUIRE(lcf::LSD_Reader::Save(roundtrip_path, *save, lcf::EngineVersion::e2k3));
	}
	REQUIRE(ManiacOriginalSave::ImportObservedBanks(*save));
	std::vector<uint8_t> reencoded;
	REQUIRE(ManiacOriginalSave::EncodeObservedBanks(
		save->easyrpg_data.maniac_self_variable_banks, reencoded));
	CHECK(reencoded == save->maniac_original_bank_bytes);
	Game_ManiacSelfVariables original_banks;
	original_banks.SetSaveData(std::move(save->easyrpg_data.maniac_self_variable_banks));
	CHECK(Game_ManiacSelfVariables::ReadNumber(original_banks.Find(1, 0, 1), -1) == 70);
	CHECK(Game_ManiacSelfVariables::ReadSwitch(original_banks.Find(1, 0, 1), -1) == 1);
	CHECK(Game_ManiacSelfVariables::ReadSwitch(original_banks.Find(2, 1, 1), -1) == 1);
	CHECK(Game_ManiacSelfVariables::ReadNumber(original_banks.Find(2, 1, 1), -3) == 88);
	CHECK(Game_ManiacSelfVariables::ReadNumber(original_banks.Find(2, 1, 1), -4) == 5);
	CHECK(save->foreground_event_execstate.stack[0].easyrpg_self_scope == 2);
	CHECK(save->foreground_event_execstate.stack[0].easyrpg_self_map_id == 1);
	CHECK(save->foreground_event_execstate.stack[0].easyrpg_self_event_id == 1);
}

TEST_CASE("Maniac original map bank samples import observed sparse strings") {
	const char* save_path = std::getenv("MANIAC_READ_ORIGINAL_BANK_SAMPLE");
	if (!save_path || !*save_path) return;
	auto save = lcf::LSD_Reader::Load(save_path);
	REQUIRE(save != nullptr);
	const char* roundtrip_path = std::getenv("MANIAC_WRITE_ORIGINAL_BANK_ROUNDTRIP");
	if (roundtrip_path && *roundtrip_path) {
		REQUIRE(lcf::LSD_Reader::Save(roundtrip_path, *save, lcf::EngineVersion::e2k3));
	}
	REQUIRE(ManiacOriginalSave::ImportObservedBanks(*save));
	std::vector<uint8_t> reencoded;
	REQUIRE(ManiacOriginalSave::EncodeObservedBanks(
		save->easyrpg_data.maniac_self_variable_banks, reencoded));
	CHECK(reencoded == save->maniac_original_bank_bytes);
	Game_ManiacSelfVariables banks;
	banks.SetSaveData(std::move(save->easyrpg_data.maniac_self_variable_banks));
	const std::string name = save_path;
	const int event_id = name.find("event2") != std::string::npos ? 2 : 1;
	const auto* bank = banks.Find(2, 1, event_id);
	REQUIRE(bank != nullptr);
	CHECK(Game_ManiacSelfVariables::ReadNumber(bank, -1) == 42);
	if (name.find("two-events") != std::string::npos) {
		const auto* second = banks.Find(2, 1, 2);
		REQUIRE(second != nullptr);
		CHECK(Game_ManiacSelfVariables::ReadNumber(second, -1) == 43);
	} else if (name.find("two-maps") != std::string::npos || name.find("map-switch") != std::string::npos) {
		const auto* second = banks.Find(2, 2, 2);
		REQUIRE(second != nullptr);
		CHECK(Game_ManiacSelfVariables::ReadNumber(second, -1) == 43);
		CHECK(banks.Find(2, 1, 2) == nullptr);
		CHECK(banks.Find(2, 2, 1) == nullptr);
		if (name.find("map-switch") != std::string::npos) {
			CHECK(save->party_location.map_id == 2);
		}
	} else {
		CHECK(banks.Find(2, 1, event_id == 1 ? 2 : 1) == nullptr);
	}
	if (name.find("resume") != std::string::npos) {
		REQUIRE(save->foreground_event_execstate.stack.size() == 1);
		CHECK(save->foreground_event_execstate.stack[0].event_id == 1);
		CHECK(save->foreground_event_execstate.stack[0].maniac_event_id == 1);
		CHECK(save->foreground_event_execstate.stack[0].current_command == 5);
	}
	if (name.find("strings") != std::string::npos) {
		REQUIRE(bank->strings.size() == 4);
		CHECK(static_cast<std::string_view>(bank->strings[1]) == "Bank sample");
		CHECK_FALSE(Game_ManiacSelfVariables::IsStringPresent(bank, 2));
		CHECK(static_cast<std::string_view>(bank->strings[3]) == "Z");
	} else if (name.find("string") != std::string::npos) {
		REQUIRE(bank->strings.size() == 2);
		CHECK(static_cast<std::string_view>(bank->strings[1]) == "Bank sample");
	} else {
		CHECK(bank->strings.empty());
	}
	auto corrupted = save->maniac_original_bank_bytes;
	REQUIRE(corrupted.size() > 2);
	corrupted[1] = 0x7f; // Child payload would exceed the original root extension.
	Game_ManiacSelfVariables::Banks rejected;
	CHECK_FALSE(ManiacOriginalSave::Decode(corrupted, rejected));
	CHECK(rejected.empty());
}

TEST_CASE("Maniac observed self bank exports a changed value to original format") {
	const char* source = std::getenv("MANIAC_READ_ORIGINAL_EXPORT_SOURCE");
	const char* destination = std::getenv("MANIAC_WRITE_ORIGINAL_BANK_EXPORT");
	if (!source || !*source || !destination || !*destination) return;
	auto save = lcf::LSD_Reader::Load(source);
	REQUIRE(save != nullptr);
	REQUIRE(ManiacOriginalSave::ImportObservedBanks(*save));
	std::vector<uint8_t> encoded;
	REQUIRE(ManiacOriginalSave::EncodeObservedBanks(
		save->easyrpg_data.maniac_self_variable_banks, encoded));
	CHECK(encoded == save->maniac_original_bank_bytes);
	Game_ManiacSelfVariables banks;
	banks.SetSaveData(save->easyrpg_data.maniac_self_variable_banks);
	auto* bank = banks.FindMutable(2, 1, 1);
	REQUIRE(bank != nullptr);
	REQUIRE(bank->numbers.size() > 1);
	bank->numbers[1] = 43;
	REQUIRE(ManiacOriginalSave::EncodeObservedBanks(banks.GetSaveData(), encoded));
	save->maniac_original_bank_bytes = encoded;
	Game_ManiacSelfVariables::Banks decoded;
	REQUIRE(ManiacOriginalSave::Decode(encoded, decoded));
	Game_ManiacSelfVariables decoded_banks;
	decoded_banks.SetSaveData(std::move(decoded));
	CHECK(Game_ManiacSelfVariables::ReadNumber(decoded_banks.Find(2, 1, 1), -1) == 43);
	REQUIRE(lcf::LSD_Reader::Save(destination, *save, lcf::EngineVersion::e2k3));
}

TEST_CASE("Maniac Player save frame preserves original map context") {
	const char* player_path = std::getenv("MANIAC_COMPARE_PLAYER_SAVE");
	if (!player_path || !*player_path) return;
	auto player_save = lcf::LSD_Reader::Load(player_path);
	auto original_save = lcf::LSD_Reader::Load("docs/evidence/maniac-250921-original-save-map-self-resume.lsd");
	REQUIRE(player_save != nullptr);
	REQUIRE(original_save != nullptr);
	const auto& original = original_save->foreground_event_execstate.stack;
	const auto& player = player_save->foreground_event_execstate.stack;
	REQUIRE(original.size() == 1);
	REQUIRE(player.size() == 1);
	CHECK(original[0].maniac_event_info == 0x13);
	CHECK(player[0].maniac_event_info == original[0].maniac_event_info);
	CHECK(player[0].maniac_event_id == original[0].maniac_event_id);
	CHECK(player[0].maniac_event_page_id == original[0].maniac_event_page_id);
	CHECK(player[0].current_command == original[0].current_command);
	CHECK(player[0].commands == original[0].commands);
	CHECK(player[0].easyrpg_self_scope == 2);
	CHECK(player_save->party_location.map_id == 1);
	REQUIRE(ManiacOriginalSave::ImportObservedBanks(*player_save));
	Game_ManiacSelfVariables banks;
	banks.SetSaveData(std::move(player_save->easyrpg_data.maniac_self_variable_banks));
	CHECK(Game_ManiacSelfVariables::ReadNumber(banks.Find(2, 1, 1), -1) == 42);
}

TEST_CASE("Maniac new game cleanup clears prior map self bank") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		Player::game_config.patch_maniac.Set(1);
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		auto* prior = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
		REQUIRE(prior != nullptr);
		prior->numbers.resize(2);
		prior->numbers[1] = 42;
		Main_Data::Cleanup();
		CHECK(Main_Data::game_maniac_self_variables == nullptr);
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		CHECK(Main_Data::game_maniac_self_variables->Find(2, 1, 1) == nullptr);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac explicit release selects observed 241028 negative ControlVars behavior") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		Player::game_config.patch_maniac.Set(1);
		lcf::rpg::EventCommand write;
		write.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars);
		write.parameters = {0, -1, 0, 0, 0, 42, 0};
		lcf::rpg::EventCommand read = write;
		read.parameters = {0, 1, 0, 0, 1, -1, 0};
		lcf::rpg::EventCommand marker = write;
		marker.parameters = {0, 2, 0, 0, 0, 99, 0};
		for (int version : {241028, 250921}) {
			Player::game_config.maniac_version.Set(version);
			Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
			Main_Data::game_variables->Set(1, 0);
			Main_Data::game_variables->Set(2, 0);
			Game_Interpreter interpreter;
			interpreter.Push({write, read, marker}, 1);
			REQUIRE(interpreter.ExecuteCommand(write));
			REQUIRE(interpreter.ExecuteCommand(read));
			REQUIRE(interpreter.ExecuteCommand(marker));
			CHECK(Main_Data::game_variables->Get(1) == (version == 241028 ? 0 : 42));
			CHECK(Main_Data::game_variables->Get(2) == 99);
			const auto* bank = Main_Data::game_maniac_self_variables->Find(2, 1, 1);
			if (version == 241028) CHECK(bank == nullptr);
			else CHECK(Game_ManiacSelfVariables::ReadNumber(bank, -1) == 42);
		}
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac release CLI selection stays separate from patch detection") {
	CmdlineParser old_args({"player", "--maniac-version", "241028"});
	Game_ConfigGame old_config;
	old_config.LoadFromArgs(old_args);
	CHECK(old_config.maniac_version.Get() == 241028);
	CHECK_FALSE(old_config.patch_override);
	CHECK(old_config.patch_maniac.Get() == 0);
	CmdlineParser new_args({"player", "--patch-maniac", "1", "--maniac-version", "250921"});
	Game_ConfigGame new_config;
	new_config.LoadFromArgs(new_args);
	CHECK(new_config.maniac_version.Get() == 250921);
	CHECK(new_config.patch_maniac.Get() == 1);
}

TEST_CASE("Maniac string target with an unknown mode leaves the string unchanged") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		Player::game_config.patch_maniac.Set(1);
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_strings->SetManiacRawSlot(1, "keep");
		lcf::rpg::EventCommand command;
		command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		command.parameters = {15, 1, 1, 0, 0, 0, 0, 0};
		command.string = lcf::DBString("changed");
		Game_Interpreter interpreter;
		CHECK(interpreter.ExecuteCommand(command));
		CHECK(Main_Data::game_strings->Get(1) == "keep");
		Player::game_config.patch_maniac.Set(previous_patch);
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 3020 reads direct and indirect map self string sources") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		Player::game_config.patch_maniac.Set(1);
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
		REQUIRE(bank != nullptr);
		bank->strings.resize(2);
		bank->strings[1] = lcf::DBString("Bank sample");
		Main_Data::game_variables->Set(3, -1);
		lcf::rpg::EventCommand direct;
		direct.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		direct.parameters = {0x10, 1, 0, 0, -1, 0, 0, 0};
		direct.string = lcf::DBString("Fallback");
		lcf::rpg::EventCommand indirect = direct;
		indirect.parameters = {0x20, 3, 0, 0, 3, 0, 0, 0};
		lcf::rpg::EventCommand literal = direct;
		literal.parameters[0] = 0;
		literal.parameters[1] = 2;
		literal.string = lcf::DBString("Literal");
		Game_Interpreter interpreter;
		interpreter.Push({direct, indirect, literal}, 1);
		REQUIRE(interpreter.ExecuteCommand(direct));
		REQUIRE(interpreter.ExecuteCommand(indirect));
		REQUIRE(interpreter.ExecuteCommand(literal));
		CHECK(Main_Data::game_strings->Get(1) == "Bank sample");
		CHECK(Main_Data::game_strings->Get(2) == "Literal");
		CHECK(Main_Data::game_strings->Get(3) == "Bank sample");
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac string to number uses explicit decimal or hexadecimal radix") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_strings->SetManiacRawSlot(1, "08");
		Main_Data::game_strings->SetManiacRawSlot(2, "0x10");
		Main_Data::game_strings->SetManiacRawSlot(3, "0x10");
		CHECK(Main_Data::game_strings->ToNum(Game_Strings::Str_Params(1), 1, *Main_Data::game_variables) == 8);
		CHECK(Main_Data::game_strings->ToNum(Game_Strings::Str_Params(2), 2, *Main_Data::game_variables) == 0);
		CHECK(Main_Data::game_strings->ToNum(Game_Strings::Str_Params(3, 1, 0), 3, *Main_Data::game_variables) == 16);
		CHECK(Main_Data::game_variables->Get(1) == 8);
		CHECK(Main_Data::game_variables->Get(2) == 0);
		CHECK(Main_Data::game_variables->Get(3) == 16);
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 join reads signed string number and switch ranges") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
		REQUIRE(bank != nullptr);
		bank->strings = {lcf::DBString{}, lcf::DBString("self-one"), lcf::DBString("self-two")};
		bank->numbers = {0, -5, 31, 7};
		bank->switches = {0, 0, 2};
		Main_Data::game_strings->SetManiacRawSlot(7, "A", true);
		Main_Data::game_strings->SetManiacRawSlot(8, "B", true);
		lcf::rpg::EventCommand strings;
		strings.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		strings.parameters = {0, 901, 0, 10 << 8, 0, -2, 2, 0};
		strings.string = lcf::DBString("|");
		auto numbers = strings;
		numbers.parameters[0] = 2 << 8;
		numbers.parameters[1] = 902;
		auto switches = strings;
		switches.parameters[0] = 4 << 8;
		switches.parameters[1] = 903;
		auto indirect = strings;
		indirect.parameters[0] = 1 << 8;
		indirect.parameters[1] = 904;
		indirect.parameters[5] = -3;
		Game_Interpreter interpreter;
		interpreter.Push({strings, numbers, switches, indirect}, 1);
		REQUIRE(interpreter.ExecuteCommand(strings));
		REQUIRE(interpreter.ExecuteCommand(numbers));
		REQUIRE(interpreter.ExecuteCommand(switches));
		REQUIRE(interpreter.ExecuteCommand(indirect));
		CHECK(Main_Data::game_strings->Get(901) == "self-two|self-one");
		CHECK(Main_Data::game_strings->Get(902) == "31|-5");
		CHECK(Main_Data::game_strings->Get(903) == "ON|OFF");
		CHECK(Main_Data::game_strings->Get(904) == "A|B");
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_strings.reset();
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 join formats numbers and expands each terminated input") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		const auto previous_escape = Player::escape_char;
		const auto previous_syntax = lcf::Data::system.maniac_text_syntax;
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Player::escape_char = '\\';
		lcf::Data::system.maniac_text_syntax = 0;
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
		REQUIRE(bank != nullptr);
		bank->strings = {lcf::DBString{}, lcf::DBString("\\SELFV[1]"),
			lcf::DBString("\\SELFT[3]"), lcf::DBString("\\V[1]")};
		bank->numbers = {0, -5, 31};
		Main_Data::game_variables->Set(1, 99);
		Main_Data::game_strings->SetManiacRawSlot(7, std::string("A\0tail", 6), true);
		Main_Data::game_strings->SetManiacRawSlot(8, std::string("B\0tail", 6), true);
		lcf::rpg::EventCommand numbers;
		numbers.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		numbers.parameters = {2 << 8, 901, 0, (10 << 8) | (1 << 17), 0, -2, 2, 0};
		numbers.string = lcf::DBString("|");
		auto expanded = numbers;
		expanded.parameters[0] = 0;
		expanded.parameters[1] = 902;
		expanded.parameters[3] = (10 << 8) | (1 << 18);
		expanded.string = lcf::DBString("\\V[-1]");
		auto terminated = expanded;
		terminated.parameters[1] = 903;
		terminated.parameters[3] = 10 << 8;
		terminated.parameters[5] = 7;
		terminated.string = lcf::DBString(std::string("|\0ignored", 9));
		Game_Interpreter interpreter;
		interpreter.Push({numbers, expanded, terminated}, 1);
		REQUIRE(interpreter.ExecuteCommand(numbers));
		REQUIRE(interpreter.ExecuteCommand(expanded));
		REQUIRE(interpreter.ExecuteCommand(terminated));
		CHECK(Main_Data::game_strings->Get(901) == "1f|fffffffb");
		CHECK(Main_Data::game_strings->Get(902) == "\\V[1]-5-5");
		CHECK(Main_Data::game_strings->Get(903) == "A|B");
		lcf::Data::system.maniac_text_syntax = previous_syntax;
		Player::escape_char = previous_escape;
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_strings.reset();
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 string sources expand before UTF16 padding and format signed numbers") {
	lcf::Data::actors.resize(1);
	lcf::Data::actors[0].ID = 1;
	lcf::Data::actors[0].Setup(true);
	lcf::Data::actors[0].name = "STATIC\\V[1]";
	lcf::Data::actors[0].title = "TITLE\\V[1]";
	lcf::Data::commonevents.resize(1);
	lcf::Data::commonevents[0].ID = 1;
	lcf::Data::commonevents[0].name = "COMMON";
	lcf::Data::skills.resize(1);
	lcf::Data::skills[0].ID = 7;
	lcf::Data::skills[0].name = "SPARSE";
	lcf::Data::skills[0].description = "DESC";
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		const auto previous_escape = Player::escape_char;
		const auto previous_syntax = lcf::Data::system.maniac_text_syntax;
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Player::escape_char = '\\';
		lcf::Data::system.maniac_text_syntax = 0;
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
		REQUIRE(bank != nullptr);
		bank->strings = {lcf::DBString{}, lcf::DBString("\\SELFT[2]"), lcf::DBString("\xF0\x9F\x98\x80")};
		bank->numbers = {0, -5, 31, 2};
		bank->switches = {0, 0, 2};
		lcf::rpg::EventCommand text;
		text.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		text.parameters = {1 << 4, 901, 0, 1 << 18, -1, 4, 0, 0};
		auto decimal = text;
		decimal.parameters = {0, 902, 0, (1 << 8) | (1 << 18), -5, 4, 0, 0};
		auto hex = decimal;
		hex.parameters = {0, 903, 0, (1 << 8) | (1 << 17), 31, 4, 0, 0};
		auto indirect_switch = text;
		indirect_switch.parameters = {2 << 4, 904, 0, (2 << 8) | (1 << 18), -3, -5, 0, 0};
		auto direct_switch = indirect_switch;
		direct_switch.parameters = {0, 905, 0, 2 << 8, -1, 0, 0, 0};
		Main_Data::game_actors->GetActor(1)->SetName("DYN\\V[1]");
		Main_Data::game_actors->GetActor(1)->SetTitle("DT\\V[1]");
		Main_Data::game_variables->Set(1, 7);
		auto dynamic_name = text;
		dynamic_name.parameters = {0, 906, 0, 3 << 8, 0, 1, 4, 0};
		auto dynamic_title = text;
		dynamic_title.parameters = {0, 907, 0, 4 << 8, 0, 1, 4, 0};
		auto static_name = text;
		static_name.parameters = {0, 908, 0, (3 << 8) | (1 << 18), 0, 1, 0, 0};
		auto static_title = text;
		static_title.parameters = {0, 909, 0, (4 << 8) | (1 << 18), 0, 1, 0, 0};
		auto common_name = text;
		common_name.parameters = {0, 910, 0, 3 << 8, 13, 1, 0, 0};
		auto skill_name = text;
		skill_name.parameters = {0, 911, 0, 3 << 8, 1, 7, 0, 0};
		auto skill_desc = text;
		skill_desc.parameters = {0, 912, 0, 4 << 8, 1, 7, 0, 0};
		Game_Interpreter interpreter;
		interpreter.Push({text, decimal, hex, indirect_switch, direct_switch,
			dynamic_name, dynamic_title, static_name, static_title, common_name, skill_name, skill_desc}, 1);
		REQUIRE(interpreter.ExecuteCommand(text));
		REQUIRE(interpreter.ExecuteCommand(decimal));
		REQUIRE(interpreter.ExecuteCommand(hex));
		REQUIRE(interpreter.ExecuteCommand(indirect_switch));
		REQUIRE(interpreter.ExecuteCommand(direct_switch));
		REQUIRE(interpreter.ExecuteCommand(dynamic_name));
		REQUIRE(interpreter.ExecuteCommand(dynamic_title));
		REQUIRE(interpreter.ExecuteCommand(static_name));
		REQUIRE(interpreter.ExecuteCommand(static_title));
		REQUIRE(interpreter.ExecuteCommand(common_name));
		REQUIRE(interpreter.ExecuteCommand(skill_name));
		REQUIRE(interpreter.ExecuteCommand(skill_desc));
		CHECK(Main_Data::game_strings->Get(901) == "  \xF0\x9F\x98\x80");
		CHECK(Main_Data::game_strings->Get(902) == "-005");
		CHECK(Main_Data::game_strings->Get(903) == "001f");
		CHECK(Main_Data::game_strings->Get(904) == "ON   ");
		CHECK(Main_Data::game_strings->Get(905) == "OFF");
		CHECK(Main_Data::game_strings->Get(906) == "DYN7");
		CHECK(Main_Data::game_strings->Get(907) == "DT7");
		CHECK(Main_Data::game_strings->Get(908) == "STATIC\\V[1]");
		CHECK(Main_Data::game_strings->Get(909) == "TITLE\\V[1]");
		CHECK(Main_Data::game_strings->Get(910) == "COMMON");
		CHECK(Main_Data::game_strings->Get(911) == "SPARSE");
		CHECK(Main_Data::game_strings->Get(912) == "DESC");
		Main_Data::game_strings->SetManiacRawSlot(40, "", true);
		Main_Data::game_strings->SetManiacRawSlot(41, "", false);
		auto empty_name = text;
		empty_name.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ChangeHeroName);
		empty_name.parameters = {1, 16, 40};
		auto null_title = empty_name;
		null_title.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ChangeHeroTitle);
		null_title.parameters[2] = 41;
		auto copy_name = text;
		copy_name.parameters = {0, -20, 0, 3 << 8, 0, 1, 1, 0};
		auto copy_title = text;
		copy_title.parameters = {0, -21, 0, 4 << 8, 0, 1, 1, 0};
		REQUIRE(interpreter.ExecuteCommand(empty_name));
		REQUIRE(interpreter.ExecuteCommand(null_title));
		REQUIRE(interpreter.ExecuteCommand(copy_name));
		REQUIRE(interpreter.ExecuteCommand(copy_title));
		CHECK(Main_Data::game_actors->GetActor(1)->IsManiacNamePresent());
		CHECK_FALSE(Main_Data::game_actors->GetActor(1)->IsManiacTitlePresent());
		CHECK(interpreter.IsManiacStringPresent(-20));
		CHECK_FALSE(interpreter.IsManiacStringPresent(-21));
		lcf::Data::system.maniac_text_syntax = previous_syntax;
		Player::escape_char = previous_escape;
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_strings.reset();
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 contextual expansion retains the native SELFS cursor") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		const auto previous_escape = Player::escape_char;
		const auto previous_syntax = lcf::Data::system.maniac_text_syntax;
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Player::escape_char = '\\';
		lcf::Data::system.maniac_text_syntax = 0;
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
		REQUIRE(bank != nullptr);
		bank->numbers = {0, 42};
		bank->switches = {0, 0, 1};
		bank->strings = {lcf::DBString{}, lcf::DBString("self")};
		Main_Data::game_switches->Set(2, true);
		lcf::rpg::EventCommand text;
		text.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		text.parameters = {0, 901, 0, 1 << 18, 0, 0, 0, 0};
		text.string = lcf::DBString("\\SELFS[2]X|\\S[-2]X|\\SELFV[1]|\\SELFT[1]|\\SELFS\xF0\x9F\x98\x80" "2|\\SELFSa\xF0\x9F\x98\x80" "2");
		Game_Interpreter interpreter;
		interpreter.Push({text}, 1);
		REQUIRE(interpreter.ExecuteCommand(text));
		CHECK(Main_Data::game_strings->Get(901) == "OFF]X|ONX|42|self|ON|OFF\xED\xB8\x80" "2");
		lcf::Data::system.maniac_text_syntax = previous_syntax;
		Player::escape_char = previous_escape;
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_strings.reset();
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 string assignments and append route signed targets") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
		REQUIRE(bank != nullptr);
		bank->strings = {lcf::DBString{}, lcf::DBString{}, lcf::DBString("A"),
			lcf::DBString("B"), lcf::DBString{}, lcf::DBString{}};
		bank->numbers = {0, 5, 3};
		Main_Data::game_strings->SetManiacRawSlot(4, "G", true);
		lcf::rpg::EventCommand append;
		append.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		append.parameters = {1 | (1 << 4), -2, -3, 1, -2, 0, 0, 0};
		auto range = append;
		range.parameters = {15, -1, -2, 0, 0, 0, 0, 0};
		range.string = lcf::DBString("C");
		auto indirect = range;
		indirect.parameters = {2, -2, 0, 1, 0, 0, 0, 0};
		indirect.string = lcf::DBString("D");
		auto mixed = range;
		mixed.parameters = {1, -4, 4, 0, 0, 0, 0, 0};
		mixed.string = lcf::DBString("M");
		auto clear = range;
		clear.parameters = {0, -2, 0, 5 << 8, 0, 0, 0, 0};
		auto zero = range;
		zero.parameters = {0, 0, 0, 0, 0, 0, 0, 0};
		zero.string = lcf::DBString("Z");
		Game_Interpreter interpreter;
		interpreter.Push({append, range, indirect, mixed, clear, zero}, 1);
		REQUIRE(interpreter.ExecuteCommand(append));
		CHECK(bank->strings[2] == "AA");
		CHECK(bank->strings[3] == "BA");
		REQUIRE(interpreter.ExecuteCommand(range));
		REQUIRE(interpreter.ExecuteCommand(indirect));
		REQUIRE(interpreter.ExecuteCommand(mixed));
		REQUIRE(interpreter.ExecuteCommand(clear));
		REQUIRE(interpreter.ExecuteCommand(zero));
		CHECK(bank->strings[2].empty());
		CHECK_FALSE(Game_ManiacSelfVariables::IsStringPresent(bank, 2));
		CHECK(bank->strings[3] == "CD");
		CHECK(bank->strings[4] == "M");
		CHECK(bank->strings[5] == "C");
		CHECK(Main_Data::game_strings->Get(4) == "G");
		CHECK(Main_Data::game_strings->GetManiacRawSlot(0) == "Z");
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_strings.reset();
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 string numeric consumers route SELF arrays and stored UTF16 length") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
		REQUIRE(bank != nullptr);
		bank->numbers = {0, 31, 3, 4};
		bank->strings.resize(6);
		bank->strings[1] = lcf::DBString("  -17tail");
		bank->strings[2] = lcf::DBString("0x20");
		bank->strings[3] = lcf::DBString(std::string("\xF0\x9F\x98\x80\0Z", 6));
		bank->strings[5] = lcf::DBString("\\SELFV[1]junk");
		const auto previous_escape = Player::escape_char;
		Player::escape_char = '\\';
		lcf::rpg::EventCommand decimal;
		decimal.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		decimal.parameters = {1 | (15 << 4), -2, -1, 2, 5, 0, 0, 0};
		auto hexadecimal = decimal;
		hexadecimal.parameters = {1, -1, -2, 2 | (1 << 17), -5, 0, 0, 0};
		auto length = decimal;
		length.parameters = {15, -2, -3, 3, -7, 0, 0, 0};
		auto expanded = decimal;
		expanded.parameters = {0 | (15 << 4), -5, 0, 2 | (1 << 17) | (1 << 18), -9, 0, 0, 0};
		auto unknown = decimal;
		unknown.parameters = {0, 20, 0, 255, 0, 0, 0, 0};
		Game_Interpreter interpreter;
		interpreter.Push({decimal, hexadecimal, length, expanded, unknown}, 1);
		REQUIRE(interpreter.ExecuteCommand(decimal));
		CHECK(Main_Data::game_variables->Get(5) == -17);
		CHECK(Main_Data::game_variables->Get(6) == 0);
		REQUIRE(interpreter.ExecuteCommand(hexadecimal));
		CHECK(interpreter.ReadVariable(-5) == -23);
		CHECK(interpreter.ReadVariable(-6) == 32);
		REQUIRE(interpreter.ExecuteCommand(length));
		CHECK(interpreter.ReadVariable(-7) == 4);
		CHECK(interpreter.ReadVariable(-8) == 0);
		REQUIRE(interpreter.ExecuteCommand(expanded));
		CHECK(interpreter.ReadVariable(-9) == 31);
		REQUIRE(interpreter.ExecuteCommand(unknown));
		CHECK(Main_Data::game_strings->GetManiacVectorSize() == 21);
		CHECK(Main_Data::game_strings->GetData().empty());
		Player::escape_char = previous_escape;
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_strings.reset();
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 text construction uses UTF16 slices and expands inputs before edits") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		const auto previous_escape = Player::escape_char;
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Player::escape_char = '\\';
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
		REQUIRE(bank != nullptr);
		bank->numbers = {0, 7, 8, 1, 3};
		bank->strings = {lcf::DBString{}, lcf::DBString("\\SELFV[1]"), lcf::DBString("X"), lcf::DBString{},
			lcf::DBString("\\V[1]"), lcf::DBString{}, lcf::DBString("7"), lcf::DBString("\\SELFT[4]"),
			lcf::DBString(std::string("\xF0\x9F\x98\x80\0Z", 6))};
		Main_Data::game_variables->Set(1, 9);
		lcf::rpg::EventCommand concat;
		concat.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		concat.parameters = {0, 1, 0, 6 << 8, 1, 1, 2, 0};
		concat.string = lcf::DBString("A\xE3\x81\x82" "BC");
		auto insert = concat;
		insert.parameters = {(1 << 4) | (15 << 8) | (1 << 12), 2, 0, (7 << 8) | (1 << 18), -1, -4, -2, 0};
		auto replace = concat;
		replace.parameters = {(1 << 4) | (1 << 8) | (1 << 12), 3, 0, (8 << 8) | (1 << 18), -1, -6, -7, 0};
		auto substring = concat;
		substring.parameters = {15 << 4, 4, 0, 9 << 8, -2, 1, 1, 0};
		auto erase = concat;
		erase.parameters = {15 << 4, 5, 0, 13 << 8, -2, 0, 1, 0};
		auto literal = concat;
		literal.parameters = {0, 6, 0, 13 << 8, 1, 0, 0, 0};
		literal.string = lcf::DBString("ABC");
		auto whole = concat;
		whole.parameters = {15 << 4, 7, 0, 0, -2, 0, 0, 0};
		Game_Interpreter interpreter;
		interpreter.Push({concat, insert, replace, substring, erase, literal, whole}, 1);
		for (const auto& command : {concat, insert, replace, substring, erase, literal, whole}) REQUIRE(interpreter.ExecuteCommand(command));
		CHECK(Main_Data::game_strings->Get(1) == "A\xE3\x81\x82");
		CHECK(Main_Data::game_strings->Get(2) == "7X");
		CHECK(Main_Data::game_strings->Get(3) == "\\V[1]");
		CHECK(Utils::DecodeUTF16(Main_Data::game_strings->Get(4), true) == std::u16string({char16_t(0xDE00)}));
		CHECK(Utils::DecodeUTF16(Main_Data::game_strings->Get(5), true) == std::u16string({char16_t(0xDE00), char16_t(0), u'Z'}));
		CHECK(Main_Data::game_strings->Get(6) == "A");
		CHECK(Main_Data::game_strings->Get(7) == std::string("\xF0\x9F\x98\x80\0Z", 6));
		Player::escape_char = previous_escape;
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_strings.reset();
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 regex uses UTF16 suffixes signed outputs captures and caught errors") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		const auto previous_escape = Player::escape_char;
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Player::escape_char = '\\';
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
		REQUIRE(bank != nullptr);
		bank->numbers = {0, 7};
		bank->strings = {lcf::DBString{}, lcf::DBString("\xF0\x9F\x98\x80" "a"), lcf::DBString("b"),
			lcf::DBString("("), lcf::DBString("X"), lcf::DBString("\\SELFV[1]"), lcf::DBString("7"),
			lcf::DBString("\\SELFT[8]"), lcf::DBString("\\V[1]")};
		Main_Data::game_variables->Set(1, 9);
		for (int id : {12, 20, 21}) Main_Data::game_strings->SetManiacRawSlot(id, "keep");
		Main_Data::game_strings->SetManiacRawSlot(30, "a");
		lcf::rpg::EventCommand search;
		search.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		search.parameters = {1, -1, -2, 9, 0, -4, 1, 0};
		search.string = lcf::DBString("^.");
		auto capture = search;
		capture.parameters = {1, -1, -2, 10, 0, -6, 0, 10};
		capture.string = lcf::DBString("(.)a");
		auto replace = search;
		replace.parameters = {(1 << 4) | (1 << 12), 22, 0, 14 << 8, -1, 1, -4, 0};
		replace.string = lcf::DBString(".");
		auto expanded = replace;
		expanded.parameters = {(1 << 4) | (1 << 8) | (1 << 12), 23, 0, (14 << 8) | (1 << 18), -5, -6, -7, 0};
		auto bad_replace = replace;
		bad_replace.parameters = {(1 << 4) | (1 << 8) | (1 << 12), 20, 0, 14 << 8, -1, -3, -4, 0};
		auto bad_capture = capture;
		bad_capture.parameters = {0, 30, 0, 10, 0, -9, 0, 21};
		bad_capture.string = lcf::DBString("(");
		Game_Interpreter interpreter;
		interpreter.Push({search, capture, replace, expanded, bad_replace, bad_capture}, 1);
		REQUIRE(interpreter.ExecuteCommand(search));
		REQUIRE(interpreter.ExecuteCommand(capture));
		REQUIRE(interpreter.ExecuteCommand(replace));
		REQUIRE(interpreter.ExecuteCommand(expanded));
		CHECK_NOTHROW(interpreter.ExecuteCommand(bad_replace));
		CHECK_NOTHROW(interpreter.ExecuteCommand(bad_capture));
		CHECK(interpreter.ReadVariable(-4) == 1);
		CHECK(interpreter.ReadVariable(-5) == -1);
		CHECK(interpreter.ReadVariable(-6) == 1);
		CHECK(interpreter.ReadVariable(-7) == -1);
		CHECK(interpreter.ReadVariable(-9) == -1);
		CHECK(Utils::DecodeUTF16(Main_Data::game_strings->Get(10), true) == std::u16string({char16_t(0xDE00), u'a'}));
		CHECK(Utils::DecodeUTF16(Main_Data::game_strings->Get(11), true) == std::u16string({char16_t(0xDE00)}));
		CHECK_FALSE(Main_Data::game_strings->GetData().count(12));
		CHECK_FALSE(Main_Data::game_strings->GetData().count(20));
		CHECK(Main_Data::game_strings->Get(21) == "keep"); // regex_error adds no ordinary-miss capture.
		CHECK(Main_Data::game_strings->Get(22) == "XXX");
		CHECK(Main_Data::game_strings->Get(23) == "\\V[1]");
		Player::escape_char = previous_escape;
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_strings.reset();
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac PopLine preserves CR before LF and clears aliased source") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_strings->SetManiacRawSlot(1, "A\r\nB");
		Main_Data::game_strings->PopLine(Game_Strings::Str_Params(1), 0, 2);
		CHECK(Main_Data::game_strings->Get(1) == "B");
		CHECK(Main_Data::game_strings->Get(2) == "A\r");
		Main_Data::game_strings->PopLine(Game_Strings::Str_Params(1), 0, 3);
		CHECK(Main_Data::game_strings->Get(1).empty());
		CHECK(Main_Data::game_strings->Get(3) == "B");
		Main_Data::game_strings->SetManiacRawSlot(4, "X");
		Main_Data::game_strings->PopLine(Game_Strings::Str_Params(4), 0, 4);
		CHECK(Main_Data::game_strings->Get(4).empty());
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 PopLine signed overlap writes sequentially and expands only prefix") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		const auto previous_escape = Player::escape_char;
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Player::escape_char = '\\';
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
		REQUIRE(bank != nullptr);
		bank->numbers = {0, 7, 2};
		bank->strings = {lcf::DBString{}, lcf::DBString("A\r\nB"), lcf::DBString("D\nE")};
		Main_Data::game_strings->SetManiacRawSlot(5, "\\SELFV[1]\r\nTAIL");
		Main_Data::game_strings->SetManiacRawSlot(7, "M\nN");
		lcf::rpg::EventCommand overlap;
		overlap.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		overlap.parameters = {1 | (1 << 4), -1, -2, 8, -2, 0, 0};
		auto expanded = overlap;
		expanded.parameters = {0, 5, 0, 8 | (1 << 18), 0, 0, 0};
		auto alias = overlap;
		alias.parameters = {0, -3, 0, 8, -3, 0, 0};
		auto alias_lf = overlap;
		alias_lf.parameters = {0, 7, 0, 8, 7, 0, 0};
		Game_Interpreter interpreter;
		interpreter.Push({overlap, expanded, alias, alias_lf}, 1);
		REQUIRE(interpreter.ExecuteCommand(overlap));
		CHECK(interpreter.ReadString(-1) == "B");
		CHECK(interpreter.ReadString(-2).empty());
		CHECK(interpreter.ReadString(-3) == "A\r");
		REQUIRE(interpreter.ExecuteCommand(expanded));
		CHECK(Main_Data::game_strings->GetManiacRawSlot(0) == "7\r");
		CHECK(Main_Data::game_strings->Get(5) == "TAIL");
		REQUIRE(interpreter.ExecuteCommand(alias));
		CHECK(interpreter.ReadString(-3).empty());
		REQUIRE(interpreter.ExecuteCommand(alias_lf));
		CHECK(Main_Data::game_strings->Get(7) == "N");
		Player::escape_char = previous_escape;
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_strings.reset();
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 Instr uses UTF16 signed numeric outputs NULL priority and raw zero input") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		const auto previous_escape = Player::escape_char;
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Player::escape_char = '\\';
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
		REQUIRE(bank != nullptr);
		bank->numbers = {0, 20, 0, 7};
		bank->numbers.resize(14, 99); // Distinguish a written zero from an ignored command.
		bank->strings = {lcf::DBString{}, lcf::DBString("\xF0\x9F\x98\x80" "A7Z"), lcf::DBString("b")};
		Main_Data::game_strings->SetManiacRawSlot(0, "Z");
		lcf::rpg::EventCommand direct;
		direct.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		direct.parameters = {1, -1, -2, 4, 0, -8, 0, 0};
		direct.string = lcf::DBString("A");
		auto indirect = direct;
		indirect.parameters = {1 | (1 << 8), -1, -2, 4 | (1 << 18), 0, -1, 0, 0};
		indirect.string = lcf::DBString("\\SELFV[3]");
		auto null_needle = direct;
		null_needle.parameters = {1 | (1 << 4), -1, -2, 4, -5, -12, -1, 0};
		auto zero = direct;
		zero.parameters = {0, 0, 0, 4, 0, -10, 0, 0};
		zero.string = lcf::DBString("Z");
		Game_Interpreter interpreter;
		interpreter.Push({direct, indirect, null_needle, zero}, 1);
		for (const auto& command : {direct, indirect, null_needle, zero}) REQUIRE(interpreter.ExecuteCommand(command));
		CHECK(interpreter.ReadVariable(-8) == 2);
		CHECK(interpreter.ReadVariable(-9) == -1);
		CHECK(Main_Data::game_variables->Get(20) == 3);
		CHECK(Main_Data::game_variables->Get(21) == -1);
		CHECK(interpreter.ReadVariable(-12) == 0);
		CHECK(interpreter.ReadVariable(-13) == 0);
		CHECK(interpreter.ReadVariable(-10) == 0);
		Player::escape_char = previous_escape;
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_strings.reset();
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 direct string array consumers read physical zero") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		Main_Data::game_strings->SetManiacRawSlot(0, "12X");
		lcf::rpg::EventCommand number;
		number.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		number.parameters = {0, 0, 0, 2, -1, 0, 0};
		auto length = number;
		length.parameters = {0, 0, 0, 3, -2, 0, 0};
		auto search = number;
		search.parameters = {0, 0, 0, 9, 0, -3, 0, 0};
		search.string = lcf::DBString("X");
		auto capture = search;
		capture.parameters = {0, 0, 0, 10, 0, -4, 0, -5};
		capture.string = lcf::DBString("(X)");
		Game_Interpreter interpreter;
		interpreter.Push({number, length, search, capture}, 1);
		for (const auto& command : {number, length, search, capture}) REQUIRE(interpreter.ExecuteCommand(command));
		CHECK(interpreter.ReadVariable(-1) == 12);
		CHECK(interpreter.ReadVariable(-2) == 3);
		CHECK(interpreter.ReadVariable(-3) == 2);
		CHECK(interpreter.ReadVariable(-4) == 2);
		CHECK(interpreter.ReadString(-5) == "X");
		CHECK(interpreter.ReadString(-6) == "X");
		CHECK(Main_Data::game_strings->GetManiacRawSlot(0) == "12X");
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_strings.reset();
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 regex replacement keeps zero groups literal across standard libraries") {
	CHECK(Game_Strings::ManiacRegExReplace("zaby", "(a)(b)", "$0|$00|$01|$1|$2|$12|$&|$$|$`|$'|$x|$", std::regex_constants::match_default)
		== "z$0|$00|a|a|b||ab|$|z|y|$x|$y");
	CHECK(Game_Strings::ManiacRegExReplace("ab", "(a)(b)", "$$0|$$00|$$$0|$000|$01", std::regex_constants::match_default)
		== "$0|$00|$$0|$000|a");
}

TEST_CASE("Maniac 250921 Split retains UTF16 units signed counts and sequential overlap") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
		REQUIRE(bank != nullptr);
		bank->numbers = {0, 8, 0, 0, 99, 99, 99};
		bank->strings = {lcf::DBString{}, lcf::DBString("a,,b,"), lcf::DBString("c,d"), lcf::DBString("\xF0\x9F\x98\x80" "Z")};
		Main_Data::game_strings->SetManiacRawSlot(20, "u,v");
		Main_Data::game_strings->SetManiacRawSlot(21, "x,y");
		Main_Data::game_strings->SetManiacRawSlot(30, "keep");
		Main_Data::game_strings->SetManiacRawSlot(31, "keep");
		Main_Data::game_strings->SetManiacRawSlot(40, "", true);
		lcf::rpg::EventCommand split;
		split.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		split.parameters = {1 | (1 << 8), -1, -2, 5, 0, -1, -4, 0};
		split.string = lcf::DBString(",");
		auto units = split;
		units.parameters = {0, -3, 0, 5, 0, -14, 0, 0};
		units.string = lcf::DBString{};
		auto overlap = split;
		overlap.parameters = {1, 20, 21, 5, 0, 21, -10, 0};
		auto missing = split;
		missing.parameters = {0, -7, 0, 5, 0, 30, -6, 0};
		auto missing_empty_delimiter = split;
		missing_empty_delimiter.parameters = {1 << 4, -7, 0, 5, 40, 31, -7, 0};
		Game_Interpreter interpreter;
		interpreter.Push({split, units, overlap, missing, missing_empty_delimiter}, 1);
		for (const auto& command : {split, units, overlap, missing, missing_empty_delimiter}) REQUIRE(interpreter.ExecuteCommand(command));
		CHECK(interpreter.ReadVariable(-4) == 4);
		CHECK(interpreter.ReadVariable(-5) == 2);
		CHECK(interpreter.ReadString(-8) == "a");
		CHECK(interpreter.ReadString(-9).empty());
		CHECK(interpreter.ReadString(-10) == "b");
		CHECK(interpreter.ReadString(-11).empty());
		CHECK(interpreter.ReadString(-4) == "c");
		CHECK(interpreter.ReadString(-5) == "d");
		CHECK(Utils::DecodeUTF16(interpreter.ReadString(-14), true) == std::u16string(1, char16_t(0xD83D)));
		CHECK(Utils::DecodeUTF16(interpreter.ReadString(-15), true) == std::u16string(1, char16_t(0xDE00)));
		CHECK(interpreter.ReadString(-16) == "Z");
		CHECK(Main_Data::game_strings->Get(21) == "u");
		CHECK(Main_Data::game_strings->Get(22) == "v");
		CHECK(Main_Data::game_strings->Get(23) == "u");
		CHECK(interpreter.ReadVariable(-10) == 2);
		CHECK(interpreter.ReadVariable(-11) == 1);
		// A nonnull delimiter emits the trailing NULL part even for a NULL input.
		CHECK(interpreter.ReadVariable(-6) == 1);
		CHECK(interpreter.ReadVariable(-7) == 1);
		CHECK(Main_Data::game_strings->GetData().count(30) == 0);
		CHECK(Main_Data::game_strings->GetData().count(31) == 0);
		CHECK(Main_Data::game_strings->GetData().count(40) == 1);
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_strings.reset();
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 UTF8 file range keeps body literal resolves encoding and reads native text") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto original_fs = FileFinder::Save();
		const auto folder = std::filesystem::canonical(std::filesystem::current_path()) / "build" / "compat-3020-file-utf8-20261008" / "unit-files-normal-dir";
		std::filesystem::create_directories(folder);
		std::filesystem::remove(folder / "Text" / "file77.txt");
		std::filesystem::remove(folder / "Text" / "bom.txt");
		std::filesystem::remove(folder / "Text" / "raw77.txt");
		// Only remove an empty owned directory. Old diagnostic outputs are kept.
		if (std::filesystem::exists(folder / "Text") && std::filesystem::is_empty(folder / "Text")) std::filesystem::remove(folder / "Text");
		const auto fixture_fs = FileFinder::Root().Create(folder.generic_u8string());
		REQUIRE(static_cast<bool>(fixture_fs));
		FileFinder::SetSaveFilesystem(fixture_fs);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		const auto previous_escape = Player::escape_char;
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Player::escape_char = '\\';
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
		REQUIRE(bank != nullptr);
		bank->numbers = {0, 77, 0, 0, -5, 1};
		bank->strings = {lcf::DBString{}, lcf::DBString("A\\V[1]"), lcf::DBString("|B")};
		lcf::rpg::EventCommand write;
		write.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		write.parameters = {1 | (15 << 8), -1, -2, 7 | (1 << 18), 0, -4, 0, 0};
		write.string = lcf::DBString("file\\SELFV[1]");
		auto read = write;
		read.parameters = {1 << 8, 20, 0, 12 << 8, 0, -5, 0, 0};
		read.string = lcf::DBString("bom");
		Game_Interpreter interpreter;
		interpreter.Push({write, read}, 1);
		REQUIRE(interpreter.ExecuteCommand(write));
		std::ifstream written(folder / "Text" / "file77.txt", std::ios::binary);
		CHECK(written.is_open());
		if (written) CHECK(std::string(std::istreambuf_iterator<char>(written), {}) == "A\\V[1]|B");
		{
			std::ofstream output(folder / "Text" / "bom.txt", std::ios::binary);
			REQUIRE(output.is_open());
			output << "\xEF\xBB\xBF" "PACK\r\nAGE\rX";
		}
		REQUIRE(interpreter.ExecuteCommand(read));
		CHECK(Main_Data::game_strings->Get(20) == "\xEF\xBB\xBF" "PACK\nAGE\rX");
		{
			std::ofstream output(folder / "Text" / "ansi.txt", std::ios::binary);
			REQUIRE(output.is_open());
			const char bytes[] = "\x82\xA0\r\nB\rX\x1A" "TAIL\0NUL";
			output.write(bytes, sizeof(bytes) - 1);
		}
		// The external write changes the Text listing, not the save root listing.
		FileFinder::Save().Subtree("Text").ClearCache();
		const auto previous_encoding = Player::encoding;
		Player::encoding = "CP932";
		bool do_yield = false;
		CHECK(Game_Strings::FromFile("ansi", 0, do_yield) == "\xE3\x81\x82\nB\rX");
		CHECK_FALSE(do_yield);
		const std::string ansi_output_text("\xE3\x81\x82\0B", 5);
		// Keep this file-format fixture outside other cases' shared output window.
		Game_Clock::SleepFor(std::chrono::milliseconds(1001));
		REQUIRE(Game_Strings::ToFile(std::string_view(ansi_output_text), "ansi0", 0));
		std::ifstream ansi_output(folder / "Text" / "ansi0.txt", std::ios::binary);
		REQUIRE(ansi_output.is_open());
		CHECK(std::string(std::istreambuf_iterator<char>(ansi_output), {}) == std::string("\x82\xA0\0\0\0\0", 6));
		CHECK(Game_Strings::FromFile("ansi0", 0, do_yield) == "\xE3\x81\x82");
		CHECK_FALSE(do_yield);
		Player::encoding = previous_encoding;
		CHECK(Game_Strings::FromFile("bom", 3, do_yield).empty());
		CHECK_FALSE(do_yield);
		const std::u16string raw_units{u'A', 0, u'B', 0xD83D, 0xDE00, u'\n'};
		const auto raw_text = Utils::EncodeUTF(raw_units, true);
		REQUIRE(Game_Strings::ToFile(std::string_view(raw_text), "raw77", 2));
		std::ifstream raw_output(folder / "Text" / "raw77.txt", std::ios::binary);
		REQUIRE(raw_output.is_open());
		CHECK(std::string(std::istreambuf_iterator<char>(raw_output), {}) == std::string("A\0\0\0B\0=\xD8\0\xDE\n\0", 12));
		Player::escape_char = previous_escape;
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
		FileFinder::SetSaveFilesystem(original_fs);
		Main_Data::game_strings.reset();
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 message window keeps restored face configuration") {
	Bitmap::SetFormat(format_R8G8B8A8_a().format());
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		lcf::rpg::SaveSystem saved;
		saved.face_name = "restored";
		saved.face_id = 2;
		saved.maniac_face_start = 2;
		saved.maniac_face_end = 3;
		saved.maniac_face_animation_flags = 10;
		Main_Data::game_system->SetupFromSave(std::move(saved));
		{
			Window_Message window(0, 160, 320, 80);
			CHECK(Main_Data::game_system->GetMessageFaceName() == "restored");
			CHECK(Main_Data::game_system->GetSaveData().maniac_face_start == 2);
		}
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 battle no-ops reach adjacent markers in one update") {
	struct NoopSource : Input::Source {
		using Input::Source::Source;
		void Update() override {}
		void UpdateSystem() override {}
	};
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		auto previous_source = std::move(Input::source);
		Input::source = std::make_unique<NoopSource>(Game_ConfigInput{}, Input::DirectionMappingArray{});
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		std::vector<lcf::rpg::EventCommand> commands;
		for (int index = 0; index < 9; ++index) {
			lcf::rpg::EventCommand noop;
			noop.code = index % 3 == 0 ? 3024 : index % 3 == 1 ? 3034 : 3035;
			if (index / 3 == 1) noop.parameters = {7};
			if (index / 3 == 2) {
				noop.parameters = {7, 99, -3};
				noop.string = lcf::DBString("DO_NOT_EXECUTE");
			}
			commands.push_back(noop);
			lcf::rpg::EventCommand marker;
			marker.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars);
			marker.parameters = {0, index + 1, 0, 0, 0, 11 * (index + 1), 0};
			commands.push_back(marker);
		}
		lcf::rpg::EventCommand end;
		end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::END);
		commands.push_back(end);
		Game_Interpreter_Battle battle;
		battle.Push(commands, 0);
		battle.Update();
		for (int index = 0; index < 9; ++index) {
			CHECK(Main_Data::game_variables->Get(index + 1) == 11 * (index + 1));
		}
		CHECK(!battle.IsRunning());
		Input::source = std::move(previous_source);
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 battle End Command polls input twice after frame cleanup") {
	struct CountingSource : Input::Source {
		using Input::Source::Source;
		int updates = 0;
		void Update() override { ++updates; }
		void UpdateSystem() override {}
	};
	struct RestoreSource {
		std::unique_ptr<Input::Source> previous = std::move(Input::source);
		~RestoreSource() { Input::source = std::move(previous); }
	};

	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		RestoreSource restore;
		Input::source = std::make_unique<CountingSource>(Game_ConfigInput{}, Input::DirectionMappingArray{});
		auto* source = static_cast<CountingSource*>(Input::source.get());
		lcf::rpg::EventCommand end;
		end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::END);
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Game_Interpreter_Battle battle;
		battle.Push(std::vector<lcf::rpg::EventCommand>{end}, 0);
		battle.Update();
		CHECK(!battle.IsRunning());
		CHECK(source->updates == 2);

		Game_Interpreter map(true);
		map.Push(std::vector<lcf::rpg::EventCommand>{end}, 0);
		map.Update();
		CHECK(!map.IsRunning());
		CHECK(source->updates == 2);

		Player::game_config.maniac_version.Set(241028);
		Game_Interpreter_Battle earlier_battle;
		earlier_battle.Push(std::vector<lcf::rpg::EventCommand>{end}, 0);
		earlier_battle.Update();
		CHECK(!earlier_battle.IsRunning());
		CHECK(source->updates == 2);
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 battle typed return delivers and clears before both input polls") {
	struct CountingSource : Input::Source {
		using Input::Source::Source;
		Game_Interpreter_Battle* owner = nullptr;
		int receiver = 2;
		int updates = 0;
		int compatible_polls = 0;
		void Update() override {
			++updates;
			const auto* bank = Main_Data::game_maniac_self_variables->Find(1, 0, 1);
			const bool delivered = receiver == 2 ? Main_Data::game_variables->Get(601) == 91
				: receiver == 5 ? Main_Data::game_strings->Get(601) == "Inner"
				: Main_Data::game_switches->Get(601) == 1;
			if (delivered && bank && bank->numbers.empty() && bank->strings.empty()
				&& bank->switches.empty() && bank->empty_string_slots.empty()
				&& owner->GetState().stack.size() == 1) ++compatible_polls;
		}
		void UpdateSystem() override {}
	};
	struct RestoreSource {
		std::unique_ptr<Input::Source> previous = std::move(Input::source);
		~RestoreSource() { Input::source = std::move(previous); }
	};
	struct ReturnBattle : Game_Interpreter_Battle { using Game_Interpreter::GetFrame; };
	struct RestoreBanks {
		std::unique_ptr<Game_ManiacSelfVariables> previous = std::move(Main_Data::game_maniac_self_variables);
		std::unique_ptr<Game_Strings> previous_strings = std::move(Main_Data::game_strings);
		RestoreBanks() {
			Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
			Main_Data::game_strings = std::make_unique<Game_Strings>();
		}
		~RestoreBanks() {
			Main_Data::game_maniac_self_variables = std::move(previous);
			Main_Data::game_strings = std::move(previous_strings);
		}
	};
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		RestoreBanks restore_banks;
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		int receiver = 2, source_type = 2;
		SUBCASE("numeric") {}
		SUBCASE("string") { receiver = 5; source_type = 5; }
		SUBCASE("switch") { receiver = 8; source_type = 8; }
		auto* bank = Main_Data::game_maniac_self_variables->Ensure(1, 0, 1);
		REQUIRE(bank != nullptr);
		bank->numbers = {0, 91};
		bank->strings = {lcf::DBString{}, lcf::DBString("Inner")};
		bank->switches = {0, 3};
		lcf::rpg::EventCommand wait, finish;
		wait.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
		wait.parameters = {100};
		finish.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::EndEventProcessing);
		finish.parameters = {7, source_type, -1};
		ReturnBattle battle;
		battle.Push({wait}, 0);
		battle.Push({finish}, 0);
		auto& frame = battle.GetFrame();
		frame.easyrpg_self_scope = 1;
		frame.easyrpg_self_event_id = 1;
		frame.easyrpg_maniac_return_type = receiver;
		frame.easyrpg_maniac_return_address = 601;
		RestoreSource restore;
		Input::source = std::make_unique<CountingSource>(Game_ConfigInput{}, Input::DirectionMappingArray{});
		auto* source = static_cast<CountingSource*>(Input::source.get());
		source->owner = &battle;
		source->receiver = receiver;
		battle.Update();
		CHECK(battle.IsRunning());
		CHECK(source->updates == 2);
		CHECK(source->compatible_polls == 2);
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 End Nest and opcode 40 do not consume the outer command budget") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		lcf::rpg::EventCommand end;
		end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::END);
		SUBCASE("End Nest") {}
		SUBCASE("Opcode 40") { end.code = 40; }
		end.parameters = {7, -3, 99};
		end.string = lcf::DBString("UNUSED_END_NEST_TEXT");
		std::vector<lcf::rpg::EventCommand> commands(10050, end);
		lcf::rpg::EventCommand marker;
		marker.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars);
		marker.parameters = {0, 1, 0, 0, 0, 99, 0};
		commands.push_back(marker);
		Game_Interpreter interpreter(true);
		interpreter.Push(commands, 1);
		interpreter.Update();
		CHECK(Main_Data::game_variables->Get(1) == 99);
		CHECK(!interpreter.IsRunning());
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 parallel list boundary rewinds one retained frame") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		lcf::rpg::EventCommand increment;
		increment.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars);
		increment.parameters = {0, 1, 0, 1, 0, 1, 0};
		lcf::rpg::EventCommand end;
		end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::END);
		Game_Interpreter parallel(false);
		parallel.Push({increment, end}, 1);
		for (int round = 1; round <= 3; ++round) {
			parallel.Update();
			CHECK(Main_Data::game_variables->Get(1) == round);
			REQUIRE(parallel.GetState().stack.size() == 1);
			CHECK(parallel.GetState().stack.front().current_command == 0);
			CHECK(parallel.GetState().stack.front().event_id == 1);
		}
		Player::game_config.patch_maniac.Set(previous_patch);
		Player::game_config.maniac_version.Set(previous_version);
	}
	Graphics::Quit();
}

TEST_CASE("Maniac original self bank resumes the next variable command in Player") {
	const char* save_path = std::getenv("MANIAC_READ_ORIGINAL_RESUME_SAVE");
	if (!save_path || !*save_path) return;
	auto save = lcf::LSD_Reader::Load(save_path);
	REQUIRE(save != nullptr);
	REQUIRE(ManiacOriginalSave::ImportObservedBanks(*save));
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		Main_Data::game_maniac_self_variables->SetSaveData(std::move(save->easyrpg_data.maniac_self_variable_banks));
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		Player::game_config.patch_maniac.Set(1);
		Main_Data::game_variables->SetData(std::move(save->system.variables));
		Game_Interpreter_Map resumed;
		resumed.SetState(save->foreground_event_execstate);
		REQUIRE(resumed.GetState().stack.size() == 1);
		CHECK(resumed.GetState().stack[0].current_command == 5);
		CHECK(Main_Data::game_variables->Get(3) == 42);
		CHECK(Main_Data::game_variables->Get(4) == 0);
		resumed.Update();
		CHECK(Main_Data::game_variables->Get(4) == 42);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac save defers signed result destination with caller identity") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	lcf::rpg::EventCommand save;
	save.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save);
	constexpr int result_id = -1000001; // Real historical silent-capacity boundary.
	save.parameters = {0, 1, 1, result_id};
	Game_Interpreter interpreter;
	interpreter.Push({save}, 3);
	REQUIRE(interpreter.ExecuteCommand(save));
	const auto operation = interpreter.GetAsyncOp();
	CHECK(operation.GetType() == AsyncOp::eSave);
	CHECK(operation.GetSaveSlot() == 1);
	CHECK(operation.GetSaveResultVar() == result_id);
	CHECK(operation.GetSaveSelfScope() == 2);
	CHECK(operation.GetSaveSelfMapId() == 1);
	CHECK(operation.GetSaveSelfEventId() == 3);
	CHECK(Main_Data::game_maniac_self_variables->Find(2, 1, 3) == nullptr);
	lcf::rpg::Save snapshot;
	snapshot.easyrpg_data.maniac_self_variable_banks = Main_Data::game_maniac_self_variables->GetSaveData();
	std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
	REQUIRE(lcf::LSD_Reader::Save(stream, snapshot, lcf::EngineVersion::e2k3));
	stream.seekg(0);
	auto loaded = lcf::LSD_Reader::Load(stream);
	REQUIRE(loaded != nullptr);
	CHECK(loaded->easyrpg_data.maniac_self_variable_banks.empty());
	CHECK(ApplyManiacSaveResult(operation, true, Main_Data::game_maniac_self_variables.get()));
	const auto* result_bank = Main_Data::game_maniac_self_variables->Find(2, 1, 3);
	REQUIRE(result_bank != nullptr);
	CHECK(Game_ManiacSelfVariables::ReadNumber(result_bank, result_id) == 1);
	CHECK(ApplyManiacSaveResult(operation, false, Main_Data::game_maniac_self_variables.get()));
	CHECK(Game_ManiacSelfVariables::ReadNumber(result_bank, result_id) == 0);
	save.parameters = {0, 1, 0}; // Legal short record pads the missing result address.
	Game_Interpreter short_record;
	short_record.Push({save}, 3);
	REQUIRE(short_record.ExecuteCommand(save));
	CHECK(short_record.GetAsyncOp().GetSaveResultVar() == 0);
	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 save resolves nonzero slot modes and invalid-slot results") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Main_Data::game_variables->Set(2, 7);
		Main_Data::game_variables->Set(7, 9); // Distinguishes one lookup from indirect-of-indirect.
		Main_Data::game_variables->Set(3, 99);
		auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 3);
		REQUIRE(bank != nullptr);
		bank->numbers.resize(7);
		bank->numbers[1] = 8;
		bank->numbers[6] = 99;
		lcf::rpg::EventCommand save;
		save.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Save);
		Game_Interpreter interpreter;
		interpreter.Push({save}, 3);
		save.parameters = {2, 2, 0, 0};
		REQUIRE(interpreter.ExecuteCommand(save));
		CHECK(interpreter.GetAsyncOp().GetSaveSlot() == 7);
		save.parameters = {1, -1, 0, 0};
		REQUIRE(interpreter.ExecuteCommand(save));
		CHECK(interpreter.GetAsyncOp().GetSaveSlot() == 8);
		Game_Interpreter skipped;
		skipped.Push({save}, 3);
		save.parameters = {0, 0, 1, 3};
		REQUIRE(skipped.ExecuteCommand(save));
		CHECK(!skipped.IsAsyncPending());
		CHECK(Main_Data::game_variables->Get(3) == 0);
		save.parameters = {0, 0, 1, -6};
		REQUIRE(skipped.ExecuteCommand(save));
		CHECK(!skipped.IsAsyncPending());
		CHECK(Game_ManiacSelfVariables::ReadNumber(bank, -6) == 0);
		Main_Data::game_variables->Set(3, 99);
		save.parameters = {0, 0, 0, 3};
		REQUIRE(skipped.ExecuteCommand(save));
		CHECK(Main_Data::game_variables->Get(3) == 99);
		save.parameters = {};
		REQUIRE(skipped.ExecuteCommand(save));
		CHECK(!skipped.IsAsyncPending());
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 load uses one lookup for every nonzero slot mode") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto original_fs = FileFinder::Save();
		const auto folder = std::filesystem::temp_directory_path() /
			("easyrpg-maniac-load-" + std::to_string(reinterpret_cast<uintptr_t>(&game)));
		std::filesystem::create_directory(folder);
		lcf::rpg::Save data;
		for (const auto* name : {"Save07.lsd", "Save09.lsd"}) {
			std::ofstream output(folder / name, std::ios::binary);
			REQUIRE(output.is_open());
			REQUIRE(lcf::LSD_Reader::Save(output, data, lcf::EngineVersion::e2k3));
		}
		const auto fixture_fs = FileFinder::Root().Create(folder.generic_u8string());
		REQUIRE(static_cast<bool>(fixture_fs));
		REQUIRE(fixture_fs.IsFeatureSupported(Filesystem::Feature::Write));
		FileFinder::SetSaveFilesystem(fixture_fs);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Main_Data::game_variables->Set(2, 7);
		Main_Data::game_variables->Set(7, 9);
		lcf::rpg::EventCommand load;
		load.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Load);
		load.parameters = {2, 2, 0};
		Game_Interpreter interpreter;
		interpreter.Push({load}, 3);
		REQUIRE(interpreter.ExecuteCommand(load));
		CHECK(interpreter.GetAsyncOp().GetType() == AsyncOp::eLoad);
		CHECK(interpreter.GetAsyncOp().GetSaveSlot() == 7);
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
		FileFinder::SetSaveFilesystem(original_fs);
		std::filesystem::remove(folder / "Save07.lsd");
		std::filesystem::remove(folder / "Save09.lsd");
		std::filesystem::remove(folder);
	}
	Graphics::Quit();
}

TEST_CASE("Maniac nested call resumes its return after an LSD wait checkpoint") {
	lcf::Data::commonevents.clear();
	lcf::rpg::CommonEvent common;
	common.ID = 1;
	common.name = lcf::DBString("Wait return");
	lcf::rpg::EventCommand wait;
	wait.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Wait);
	wait.parameters = {1};
	lcf::rpg::EventCommand deliver;
	deliver.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::EndEventProcessing);
	deliver.parameters = {0, 2, -1};
	lcf::rpg::EventCommand end;
	end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::END);
	common.event_commands = {wait, deliver, end};
	lcf::Data::commonevents.push_back(common);
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		Player::game_config.patch_maniac.Set(1);
		lcf::rpg::EventCommand call;
		call.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
		call.parameters = {0, 1, 0, 0x20, 0, -3, 1, 1, 77};
		lcf::rpg::EventCommand read;
		read.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars);
		read.parameters = {0, 14, 0, 0, 1, -3, 0};
		Game_Interpreter_Map running;
		running.Push({call, read, end}, 1);
		running.Update();
		REQUIRE(running.GetState().stack.size() == 2);
		REQUIRE(running.GetState().wait_time > 0);
		CHECK(running.GetState().stack[0].current_command == 1);
		CHECK(running.GetState().stack[1].current_command == 1);
		CHECK(running.GetState().stack[1].easyrpg_maniac_return_type == 2);
		CHECK(running.GetState().stack[1].easyrpg_maniac_return_address == -3);
		CHECK(Main_Data::game_variables->Get(14) == 0);
		lcf::rpg::Save save;
		save.system.variables = Main_Data::game_variables->GetData();
		save.foreground_event_execstate = running.GetSaveState();
		save.easyrpg_data.maniac_self_variable_banks =
			Main_Data::game_maniac_self_variables->GetSerializableSaveData();
		std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
		REQUIRE(lcf::LSD_Reader::Save(stream, save, lcf::EngineVersion::e2k3));
		stream.seekg(0);
		auto loaded = lcf::LSD_Reader::Load(stream);
		REQUIRE(loaded != nullptr);
		Main_Data::game_variables->SetData(std::move(loaded->system.variables));
		Main_Data::game_maniac_self_variables->SetSaveData(
			std::move(loaded->easyrpg_data.maniac_self_variable_banks));
		Game_Interpreter_Map resumed;
		resumed.SetState(loaded->foreground_event_execstate);
		CHECK(resumed.GetState().wait_time == running.GetState().wait_time);
		for (int frame = 0; frame < 25 && Main_Data::game_variables->Get(14) == 0; ++frame) resumed.Update();
		CHECK(Main_Data::game_variables->Get(14) == 77);
		CHECK(resumed.GetState().stack.size() == 1);
		CHECK(resumed.GetState().stack[0].current_command == 0);
		const auto* caller = Main_Data::game_maniac_self_variables->Find(2, 1, 1);
		const auto* callee = Main_Data::game_maniac_self_variables->Find(1, 0, 1);
		REQUIRE(caller != nullptr);
		REQUIRE(callee != nullptr);
		CHECK(Game_ManiacSelfVariables::ReadNumber(caller, -3) == 77);
		CHECK(Game_ManiacSelfVariables::ReadNumber(callee, -1) == 77);
		Player::game_config.patch_maniac.Set(previous_patch);
		Main_Data::game_maniac_self_variables.reset();
	}
	Graphics::Quit();
	lcf::Data::commonevents.clear();
}

TEST_CASE("Maniac self banks retain identity and survive an LSD round trip") {
	Game_ManiacSelfVariables state;
	auto* first = state.Ensure(2, 3, 7);
	REQUIRE(first != nullptr);
	first->numbers = {0, 42};
	first->switches = {0, 2};
	first->strings = {lcf::DBString(""), lcf::DBString("value")};
	auto* other_map = state.Ensure(2, 4, 7);
	REQUIRE(other_map != nullptr);
	other_map->numbers = {0, 99};
	auto* common = state.Ensure(1, 0, 7);
	REQUIRE(common != nullptr);
	common->numbers = {0, 23};
	CHECK(state.Find(2, 3, 7) != state.Find(2, 4, 7));
	CHECK(Game_ManiacSelfVariables::ReadNumber(state.Find(2, 3, 7), -1) == 42);
	CHECK(Game_ManiacSelfVariables::ReadNumber(state.Find(2, 4, 7), -1) == 99);
	CHECK(Game_ManiacSelfVariables::ReadSwitch(state.Find(2, 3, 7), -1) == 2);
	CHECK(Game_ManiacSelfVariables::ReadNumber(state.Find(1, 0, 7), -1) == 23);
	CHECK(state.Find(2, 5, 7) == nullptr);

	lcf::rpg::Save save;
	save.easyrpg_data.maniac_self_variable_banks = state.GetSaveData();
	save.easyrpg_data.maniac_random_seed = 0xfeed1234u;
	save.easyrpg_data.maniac_global_switch_raw = {0, 1, 25, 2, 40, 255};
	save.easyrpg_data.maniac_global_variable_zero = -12345678;
	save.easyrpg_data.maniac_global_string_zero = lcf::DBString("zero");
	save.easyrpg_data.maniac_global_string_zero_present = 1;
	save.foreground_event_execstate.stack.emplace_back();
	auto& frame = save.foreground_event_execstate.stack.back();
	frame.easyrpg_self_scope = 2;
	frame.easyrpg_self_map_id = 3;
	frame.easyrpg_self_event_id = 7;
	frame.easyrpg_maniac_return_type = 2;
	frame.easyrpg_maniac_return_address = -9;
	lcf::rpg::EventCommand cached_command;
	cached_command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Selfvar);
	cached_command.easyrpg_maniac_common_name_cache = 7;
	frame.commands.push_back(cached_command);
	std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
	REQUIRE(lcf::LSD_Reader::Save(stream, save, lcf::EngineVersion::e2k3));
	stream.seekg(0);
	auto loaded = lcf::LSD_Reader::Load(stream);
	REQUIRE(loaded != nullptr);
	CHECK(loaded->easyrpg_data.maniac_self_variable_banks == state.GetSaveData());
	CHECK(loaded->easyrpg_data.maniac_random_seed == 0xfeed1234u);
	CHECK(loaded->easyrpg_data.maniac_global_switch_raw == save.easyrpg_data.maniac_global_switch_raw);
	CHECK(loaded->easyrpg_data.maniac_global_variable_zero == -12345678);
	CHECK(loaded->easyrpg_data.maniac_global_string_zero == save.easyrpg_data.maniac_global_string_zero);
	CHECK(loaded->easyrpg_data.maniac_global_string_zero_present == 1);
	REQUIRE(loaded->foreground_event_execstate.stack.size() == 1);
	CHECK(loaded->foreground_event_execstate.stack[0].easyrpg_self_scope == 2);
	CHECK(loaded->foreground_event_execstate.stack[0].easyrpg_self_map_id == 3);
	CHECK(loaded->foreground_event_execstate.stack[0].easyrpg_self_event_id == 7);
	CHECK(loaded->foreground_event_execstate.stack[0].easyrpg_maniac_return_type == 2);
	CHECK(loaded->foreground_event_execstate.stack[0].easyrpg_maniac_return_address == -9);
	REQUIRE(loaded->foreground_event_execstate.stack[0].commands.size() == 1);
	CHECK(loaded->foreground_event_execstate.stack[0].commands[0].easyrpg_maniac_common_name_cache == 0);
}

TEST_CASE("Maniac global switches retain raw byte values across 3036 operations") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Selfvar);
	command.parameters = {0x1000, 0, 0, 25, 0, 0, 258, 0x20, 1, 0};
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_switches->Get(25));
	CHECK(Main_Data::game_switches->GetInt(25) == 2);
	const std::vector<int32_t> expected_raw_switches = {25, 2};
	CHECK(Main_Data::game_switches->GetManiacRawSaveData() == expected_raw_switches);
	const auto saved_boolean_switches = Main_Data::game_switches->GetData();
	Main_Data::game_switches->SetData(saved_boolean_switches);
	CHECK(Main_Data::game_switches->GetInt(25) == 1);
	Main_Data::game_switches->SetManiacRawSaveData(expected_raw_switches);
	CHECK(Main_Data::game_switches->GetInt(25) == 2);

	auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
	REQUIRE(bank != nullptr);
	bank->switches = {0, 3};
	command.parameters = {0x10031000, 0, 0, 30, 0, 0, 1, 0x120, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_switches->GetInt(30) == 3);
	command.parameters = {0x10001003, 0, 0, 2, 0, 0, 30, 0x120, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(bank->switches[2] == 3);
	CHECK(Main_Data::game_switches->GetInt(25) == 2);

	command.parameters = {0x1000, 0, 0, 25, 0, 0, 0, 0x20, 1, 1};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_switches->GetInt(25) == 0);
	CHECK(Main_Data::game_switches->GetInt(30) == 3);
	Main_Data::game_switches->Set(30, true);
	CHECK(Main_Data::game_switches->GetInt(30) == 1);
	Main_Data::game_switches->SetManiacRaw(40, 5);
	Main_Data::game_switches->SetManiacRaw(41, 6);
	Main_Data::game_switches->SetRange(40, 40, false);
	CHECK(Main_Data::game_switches->GetInt(40) == 0);
	CHECK(Main_Data::game_switches->GetInt(41) == 6);
	Main_Data::game_switches->FlipRange(41, 41);
	CHECK(Main_Data::game_switches->GetInt(41) == 0);
	command.parameters = {0x1000, 0, 0, 0, 0, 0, 261, 0x20, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_switches->GetManiacRawSlot(0) == 5);
	CHECK(interpreter.ReadSwitch(0) == 0); // Native ordinary reads ignore the physical byte in slot zero.
	CHECK_FALSE(Main_Data::game_switches->Get(0)); // Ordinary switch reads still treat zero as invalid.
	bank->switches[0] = 9;
	command.parameters = {0x10031000, 0, 0, 0, 0, 0, 0, 0x120, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_switches->GetManiacRawSlot(0) == 9);
	command.parameters = {0x10001003, 0, 0, 0, 0, 0, 0, 0x120, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(bank->switches[0] == 9);
	const auto raw_with_zero = Main_Data::game_switches->GetManiacRawSaveData();
	const auto boolean_with_zero = Main_Data::game_switches->GetData();
	Main_Data::game_switches->SetData(boolean_with_zero);
	CHECK(Main_Data::game_switches->GetManiacRawSlot(0) == 0);
	Main_Data::game_switches->SetManiacRawSaveData(raw_with_zero);
	CHECK(Main_Data::game_switches->GetManiacRawSlot(0) == 9);
	command.parameters = {0x1000, 0, 0, 0, 0, 0, 0, 0x20, 1, 1};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_switches->GetManiacRawSlot(0) == 0);

	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac global numeric slot zero survives 3036 copies and local clears") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Selfvar);
	command.parameters = {0x1000, 0, 0, 0, 0, 0, 12345678, 0, 1, 0};
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->GetManiacRawSlot(0) == 12345678);
	CHECK(Main_Data::game_variables->Get(0) == 0); // Ordinary numeric reads ignore physical slot zero.
	auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
	REQUIRE(bank != nullptr);
	bank->numbers = {9, 11};
	command.parameters = {0x10031000, 0, 0, 0, 0, 0, 0, 0x100, 2, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->GetManiacRawSlot(0) == 9);
	CHECK(Main_Data::game_variables->Get(1) == 11);
	command.parameters = {0x10001003, 0, 0, 0, 0, 0, 0, 0x100, 2, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(bank->numbers[0] == 9);
	CHECK(bank->numbers[1] == 11);
	command.parameters = {0x1000, 0, 0, 1, 0, 0, 0, 0, 1, 1};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->GetManiacRawSlot(0) == 9);
	CHECK(Main_Data::game_variables->Get(1) == 0);
	command.parameters[3] = 0;
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->GetManiacRawSlot(0) == 0);
	*Main_Data::game_variables->GetManiacWritable(0) = 27;
	const auto ordinary = Main_Data::game_variables->GetData();
	Main_Data::game_variables->SetData(ordinary);
	CHECK(Main_Data::game_variables->GetManiacRawSlot(0) == 0);
	*Main_Data::game_variables->GetManiacWritable(0) = 27; // Player restores this after SetData.
	CHECK(Main_Data::game_variables->GetManiacRawSlot(0) == 27);
	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac numeric slot zero affects execution after LSD reload") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	*Main_Data::game_variables->GetManiacWritable(0) = 17;
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars);
	command.parameters = {0, 150, 0, 0, 21, 2, 0x00010823, 0x00000401};
	Game_Interpreter_Map running;
	running.Push({command}, 1);
	lcf::rpg::Save save;
	save.system.variables = Main_Data::game_variables->GetData();
	save.easyrpg_data.maniac_global_variable_zero = Main_Data::game_variables->GetManiacRawSlot(0);
	save.foreground_event_execstate = running.GetSaveState();
	std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
	REQUIRE(lcf::LSD_Reader::Save(stream, save, lcf::EngineVersion::e2k3));
	stream.seekg(0);
	auto loaded = lcf::LSD_Reader::Load(stream);
	REQUIRE(loaded != nullptr);
	Main_Data::game_variables->SetData(std::move(loaded->system.variables));
	CHECK(Main_Data::game_variables->GetManiacRawSlot(0) == 0);
	*Main_Data::game_variables->GetManiacWritable(0) = loaded->easyrpg_data.maniac_global_variable_zero;
	Game_Interpreter_Map resumed;
	resumed.SetState(loaded->foreground_event_execstate);
	REQUIRE(resumed.GetState().stack.size() == 1);
	REQUIRE(resumed.Game_Interpreter::ExecuteCommand());
	CHECK(Main_Data::game_variables->GetManiacRawSlot(0) == 21);
	CHECK(Main_Data::game_variables->Get(150) == 21);
	Player::game_config.patch_maniac.Set(previous_patch);
	Graphics::Quit();
}

TEST_CASE("Maniac global string slot zero survives scalar and array operations") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	Main_Data::game_strings = std::make_unique<Game_Strings>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Selfvar);
	command.parameters = {0x1000, 0, 0, 0, 0, 0, 5, 0x10, 1, 0};
	command.string = lcf::DBString("hello");
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
	REQUIRE(bank != nullptr);
	bank->strings = {lcf::DBString("from"), lcf::DBString("next")};
	Main_Data::game_strings->SetManiacRawSlot(2, "temporary");
	Main_Data::game_strings->SetManiacRawSlot(2, "");
	CHECK(Main_Data::game_strings->GetData().count(2) == 1);
	command.parameters = {0x10031000, 0, 0, 1, 0, 0, 0, 0x110, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_strings->Get(1) == "from");
	CHECK(Main_Data::game_strings->GetData().count(2) == 1); // Unchanged allocated empty slot survives array copy.
	CHECK(Main_Data::game_strings->GetManiacZeroSaveData().empty()); // Copying slot 1 must not allocate slot 0.
	command.parameters[3] = 0;
	command.parameters[8] = 0;
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_strings->GetManiacZeroSaveData().empty()); // Count zero also leaves it absent.
	command.parameters = {0x10001000, 0, 0, 0, 0, 0, 0, 0x110, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_strings->GetManiacZeroSaveData().empty()); // Self-copy of a null slot stays null.
	command.parameters[6] = 2;
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_strings->GetManiacZeroSaveData().size() == 1); // Copy of allocated empty stays allocated.
	CHECK(Main_Data::game_strings->GetManiacRawSlot(0).empty());
	Main_Data::game_strings->SetData(Game_Strings::Strings_t{});
	Main_Data::game_strings->SetManiacRawSlot(2, "");
	command.parameters = {0x1000, 0, 0, 0, 0, 0, 5, 0x10, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_strings->GetManiacRawSlot(0) == "hello");
	command.parameters = {0x1000, 0, 0, 5, 0, 0, 0, 0x10, 0, 0};
	REQUIRE(interpreter.ExecuteCommand(command)); // Count zero grows a later slot without losing zero.
	CHECK(Main_Data::game_strings->GetManiacRawSlot(0) == "hello");
	CHECK(Main_Data::game_strings->GetData().count(2) == 1); // Nor lose an allocated empty positive slot.
	CHECK(Main_Data::game_strings->GetLcfData().size() == 5); // Physical length six excludes slot zero from the ordinary vector.
	bank->strings = {lcf::DBString("from"), lcf::DBString("next")};
	command.parameters = {0x10031000, 0, 0, 0, 0, 0, 0, 0x110, 2, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_strings->GetManiacRawSlot(0) == "from");
	CHECK(Main_Data::game_strings->Get(1) == "next");
	command.parameters = {0x10001003, 0, 0, 0, 0, 0, 0, 0x110, 2, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(bank->strings[0] == lcf::DBString("from"));
	CHECK(bank->strings[1] == lcf::DBString("next"));
	Main_Data::game_variables->Set(1, 0);
	command.parameters = {0x10001000, 0, 0, 1, 0, 0, 1, 0x10, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command)); // Global mode 1: number[1] -> string[0].
	CHECK(Main_Data::game_strings->Get(1).empty()); // Native ordinary string getter returns null for ID 0.
	CHECK(Main_Data::game_strings->GetManiacRawSlot(0) == "from");
	const auto saved_zero = Main_Data::game_strings->GetManiacZeroSaveData();
	Main_Data::game_strings->SetManiacRawSlot(3, "to-clear");
	CHECK(Main_Data::game_strings->GetData().count(3) == 1);
	command.parameters = {0x1000, 0, 0, 3, 0, 0, 0, 0x10, 1, 1};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_strings->GetData().count(3) == 0); // Native Clear releases string slots.
	Main_Data::game_strings->SetData(Game_Strings::Strings_t{});
	CHECK(Main_Data::game_strings->GetManiacZeroSaveData().empty());
	Main_Data::game_strings->SetManiacZeroSaveData(saved_zero);
	CHECK(Main_Data::game_strings->GetManiacRawSlot(0) == "from");
	command.parameters = {0x1000, 0, 0, 0, 0, 0, 0, 0x10, 1, 1};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_strings->GetManiacZeroSaveData().empty());
	Main_Data::game_strings->SetManiacRawSlot(0, {});
	CHECK(Main_Data::game_strings->GetManiacZeroSaveData().size() == 1); // Present empty differs from null.
	lcf::rpg::Save save;
	save.easyrpg_data.maniac_global_string_zero = lcf::DBString(Main_Data::game_strings->GetManiacRawSlot(0));
	save.easyrpg_data.maniac_global_string_zero_present = 1;
	std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
	REQUIRE(lcf::LSD_Reader::Save(stream, save, lcf::EngineVersion::e2k3));
	stream.seekg(0);
	auto loaded = lcf::LSD_Reader::Load(stream);
	REQUIRE(loaded != nullptr);
	CHECK(loaded->easyrpg_data.maniac_global_string_zero_present == 1);
	CHECK(loaded->easyrpg_data.maniac_global_string_zero.empty());
	Main_Data::game_strings->SetData(Game_Strings::Strings_t{});
	Main_Data::game_strings->SetManiacZeroSaveData({loaded->easyrpg_data.maniac_global_string_zero});
	CHECK(Main_Data::game_strings->GetManiacZeroSaveData().size() == 1);
	Main_Data::game_strings->SetManiacRawSlot(3, "allocated");
	Main_Data::game_strings->SetManiacRawSlot(3, "");
	Main_Data::game_strings->SetManiacVectorSize(8);
	save.system.maniac_strings = Main_Data::game_strings->GetLcfData();
	save.easyrpg_data.maniac_global_string_empty_slots = Main_Data::game_strings->GetManiacEmptySlotSaveData();
	save.easyrpg_data.maniac_global_string_vector_size = Main_Data::game_strings->GetManiacVectorSize();
	CHECK(save.easyrpg_data.maniac_global_string_empty_slots == std::vector<int32_t>{3});
	std::stringstream empty_stream(std::ios::in | std::ios::out | std::ios::binary);
	REQUIRE(lcf::LSD_Reader::Save(empty_stream, save, lcf::EngineVersion::e2k3));
	empty_stream.seekg(0);
	auto empty_loaded = lcf::LSD_Reader::Load(empty_stream);
	REQUIRE(empty_loaded != nullptr);
	Main_Data::game_strings->SetManiacRawSlot(99, "stale");
	Main_Data::game_strings->SetManiacRawSlot(0, "stale");
	Main_Data::game_strings->SetData(std::move(empty_loaded->system.maniac_strings));
	CHECK(Main_Data::game_strings->GetData().count(99) == 0); // Loading replaces the prior game's data.
	CHECK(Main_Data::game_strings->GetManiacZeroSaveData().empty());
	CHECK(Main_Data::game_strings->GetData().count(3) == 0); // Vector<DBString> drops empty entries.
	Main_Data::game_strings->SetManiacEmptySlotSaveData(empty_loaded->easyrpg_data.maniac_global_string_empty_slots);
	Main_Data::game_strings->SetManiacVectorSize(empty_loaded->easyrpg_data.maniac_global_string_vector_size);
	CHECK(Main_Data::game_strings->GetData().count(3) == 1);
	CHECK(Main_Data::game_strings->Get(3).empty());
	CHECK(Main_Data::game_strings->GetLcfData().size() == 8);
	Main_Data::game_strings->SetData(Game_Strings::Strings_t{});
	Main_Data::game_variables->Set(1, 0);
	command.parameters = {0x10001000, 0, 0, 12, 0, 0, 1, 0x10, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_strings->GetData().count(12) == 0);
	CHECK(Main_Data::game_strings->GetManiacVectorSize() == 13); // Native physical length 14 includes slot zero and the extra tail.
	CHECK(Main_Data::game_strings->GetLcfData().size() == 13);
	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Main_Data::game_strings.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac self string empty slots survive copies and LSD reload") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	Main_Data::game_strings = std::make_unique<Game_Strings>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Selfvar);
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
	REQUIRE(bank != nullptr);
	Main_Data::game_strings->SetManiacRawSlot(0, "");
	command.parameters = {0x10001003, 0, 0, 1, 0, 0, 0, 0x110, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Game_ManiacSelfVariables::IsStringPresent(bank, 1));
	CHECK(bank->strings[1].empty());
	command.parameters = {0x10031000, 0, 0, 2, 0, 0, 1, 0x110, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_strings->GetData().count(2) == 1);
	CHECK(Main_Data::game_strings->Get(2).empty());
	Main_Data::game_strings->SetManiacRawSlot(6, "");
	Main_Data::game_strings->SetManiacVectorSize(11);
	bank->strings.resize(7); // Slot 6 is null.
	CHECK(bank->strings[6].empty());
	CHECK_FALSE(Game_ManiacSelfVariables::IsStringPresent(bank, 6));
	command.parameters = {0x10031000, 0, 0, 6, 0, 0, 6, 0x110, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_strings->Get(6).empty());
	CHECK(Main_Data::game_strings->GetData().count(6) == 0); // Null source releases allocated empty destination.
	CHECK(Main_Data::game_strings->GetManiacVectorSize() == 11);
	Main_Data::game_strings->SetManiacRawSlot(2, "");
	command.parameters = {0x10001003, 0, 0, 3, 0, 0, 2, 0x110, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Game_ManiacSelfVariables::IsStringPresent(bank, 3));
	bank->strings.resize(4);
	bank->strings.resize(9); // Tail slots are all null and sparse LSD omits them.
	bank->strings[0] = lcf::DBString("");
	Game_ManiacSelfVariables::SetStringPresence(bank, 0, true);
	lcf::rpg::Save save;
	save.easyrpg_data.maniac_self_variable_banks = Main_Data::game_maniac_self_variables->GetSerializableSaveData();
	std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
	REQUIRE(lcf::LSD_Reader::Save(stream, save, lcf::EngineVersion::e2k3));
	stream.seekg(0);
	auto loaded = lcf::LSD_Reader::Load(stream);
	REQUIRE(loaded != nullptr);
	Main_Data::game_maniac_self_variables->SetSaveData(std::move(loaded->easyrpg_data.maniac_self_variable_banks));
	const auto* restored = Main_Data::game_maniac_self_variables->Find(2, 1, 1);
	REQUIRE(restored != nullptr);
	CHECK(Game_ManiacSelfVariables::IsStringPresent(restored, 0));
	CHECK(Game_ManiacSelfVariables::IsStringPresent(restored, 1));
	CHECK(Game_ManiacSelfVariables::IsStringPresent(restored, 3));
	CHECK(restored->strings.size() == 9);
	command.parameters = {0x10031003, 0, 0, 2, 0, 0, 1, 0x110, 2, 0};
	REQUIRE(interpreter.ExecuteCommand(command)); // Forward self-copy propagates the first empty string.
	restored = Main_Data::game_maniac_self_variables->Find(2, 1, 1);
	CHECK(Game_ManiacSelfVariables::IsStringPresent(restored, 2));
	CHECK(Game_ManiacSelfVariables::IsStringPresent(restored, 3));
	command.parameters = {0x1003, 0, 0, 4, 0, 0, 0, 0x10, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	restored = Main_Data::game_maniac_self_variables->Find(2, 1, 1);
	CHECK_FALSE(Game_ManiacSelfVariables::IsStringPresent(restored, 4)); // Zero-length literal returns null.
	command.parameters = {0x10031003, 0, 0, 5, 0, 0, 1, 0x10, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	restored = Main_Data::game_maniac_self_variables->Find(2, 1, 1);
	CHECK(Game_ManiacSelfVariables::IsStringPresent(restored, 5));
	command.parameters = {0x1003, 0, 0, 1, 0, 0, 0, 0x10, 1, 1};
	REQUIRE(interpreter.ExecuteCommand(command));
	restored = Main_Data::game_maniac_self_variables->Find(2, 1, 1);
	CHECK_FALSE(Game_ManiacSelfVariables::IsStringPresent(restored, 1));
	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Main_Data::game_strings.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac selfvar uses current and caller frame banks") {
	lcf::Data::commonevents.clear();
	lcf::Data::commonevents.emplace_back();
	lcf::Data::commonevents.back().ID = 1;
	lcf::Data::commonevents.back().name = lcf::DBString("名");
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	Main_Data::game_strings = std::make_unique<Game_Strings>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	CHECK(Player::IsPatchManiac());

	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Selfvar);
	command.parameters = {0x1003, 0, 0, 1, 0, 0, 42, 0, 1, 0};
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	CHECK(interpreter.GetState().stack.back().easyrpg_self_scope == 2);
	CHECK(interpreter.GetState().stack.back().easyrpg_self_map_id == 1);
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-1) == 42);
	CHECK(Main_Data::game_maniac_self_variables->Find(2, 1, 1) != nullptr);

	interpreter.Push({command}, 2);
	command.parameters[0] = 0x1004; // target caller frame
	command.parameters[3] = 2;
	command.parameters[6] = 77;
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-2) == 0);
	CHECK(Game_ManiacSelfVariables::ReadNumber(Main_Data::game_maniac_self_variables->Find(2, 1, 1), -2) == 77);
	command.parameters[9] = 1; // Clear the caller's slot without allocating another bank
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(Game_ManiacSelfVariables::ReadNumber(Main_Data::game_maniac_self_variables->Find(2, 1, 1), -2) == 0);
	CHECK(Main_Data::game_maniac_self_variables->Find(2, 1, 2) == nullptr);
	command.parameters = {0x1001, 9, 0, 1, 0, 0, 13, 0, 1, 1};
	CHECK(interpreter.ExecuteCommand(command)); // Clear cannot create common bank 9.
	CHECK(Main_Data::game_maniac_self_variables->Find(1, 0, 9) == nullptr);
	command.parameters[9] = 0;
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(Game_ManiacSelfVariables::ReadNumber(Main_Data::game_maniac_self_variables->Find(1, 0, 9), -1) == 13);
	command.parameters[9] = 2;
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_maniac_self_variables->Find(1, 0, 9) != nullptr);
	CHECK(Game_ManiacSelfVariables::ReadNumber(Main_Data::game_maniac_self_variables->Find(1, 0, 9), -1) == 0);

	command.parameters = {0x1003, 0, 0, 1, 0, 0, 2, 0, 1, 0};
	CHECK(interpreter.ExecuteCommand(command)); // Current event 2 slot 1 = 2.
	command.parameters = {0x1003, 0, 0, 2, 0, 0, 9, 1, -1, 0};
	CHECK(interpreter.ExecuteCommand(command)); // Count reads -1 once, retaining value 2.
	CHECK(Game_ManiacSelfVariables::ReadNumber(Main_Data::game_maniac_self_variables->Find(2, 1, 2), -2) == 9);
	CHECK(Game_ManiacSelfVariables::ReadNumber(Main_Data::game_maniac_self_variables->Find(2, 1, 2), -3) == 9);
	command.parameters = {0x2003, 0, 0, -1, 0, 0, 55, 0, 1, 0};
	CHECK(interpreter.ExecuteCommand(command)); // Negative destination reference propagates its sign.
	CHECK(Game_ManiacSelfVariables::ReadNumber(Main_Data::game_maniac_self_variables->Find(2, 1, 2), -2) == 55);
	CHECK(Game_Interpreter_Shared::ValueOrVariable(2, -1, interpreter) == 55);
	command.parameters = {0x1000, 0, 0, -2, 0, 0, 66, 0, 1, 0};
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(2) == 66);
	command.parameters = {0x10001, 15, 0, 1, 16, 0, 0, 0, 1, 0};
	CHECK(interpreter.ExecuteCommand(command)); // Invalid destination selector still resolves both environments.
	CHECK(Main_Data::game_maniac_self_variables->Find(1, 0, 15) != nullptr);
	CHECK(Main_Data::game_maniac_self_variables->Find(1, 0, 16) != nullptr);
	command.parameters = {0x1000, 2, 0, 1, 0, 0, 2, 0x10, 1, 0};
	command.string = lcf::DBString("汉字");
	CHECK(interpreter.ExecuteCommand(command)); // Two-byte budget consumes one UTF-16 unit.
	CHECK(Main_Data::game_strings->Get(1) == "汉");
	command.parameters[6] = 4;
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_strings->Get(1) == "汉字");
	command.parameters = {0x1000, 0, 0, 24, 0, 0, 2000000000, 0, 1, 0};
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(24) == 2000000000);
	Main_Data::game_strings->Asg(25, "clear me");
	Main_Data::game_switches->Set(25, true);
	const int variable_size_before_clear = Main_Data::game_variables->GetSize();
	command.parameters = {0x1000, 0, 0, 24, 0, 0, 0, 0x30, -1, 1};
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(24) == 0);
	CHECK(Main_Data::game_variables->GetSize() == variable_size_before_clear);
	CHECK(Main_Data::game_strings->Get(25).empty());
	CHECK(Main_Data::game_switches->Get(25) == false);
	command.parameters = {0x1031, 2, 0, 1, 0, 0, 23, 0, 1, 0};
	command.string = lcf::DBString("名");
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(Game_ManiacSelfVariables::ReadNumber(Main_Data::game_maniac_self_variables->Find(1, 0, 1), -1) == 23);
	for (int id = 1; id <= 4; ++id) Main_Data::game_variables->Set(id, id);
	command.parameters = {0x10001000, 0, 0, 2, 0, 0, 1, 0x100, 3, 0};
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(1) == 1);
	CHECK(Main_Data::game_variables->Get(2) == 1);
	CHECK(Main_Data::game_variables->Get(3) == 2);
	CHECK(Main_Data::game_variables->Get(4) == 3);
	Main_Data::game_strings->Asg(1, "a");
	Main_Data::game_strings->Asg(2, "b");
	Main_Data::game_strings->Asg(3, "c");
	Main_Data::game_strings->Asg(4, "d");
	command.parameters[7] = 0x110; // String array, forward overlap.
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_strings->Get(2) == "a");
	CHECK(Main_Data::game_strings->Get(3) == "a");
	CHECK(Main_Data::game_strings->Get(4) == "a");
	command.parameters = {0x10041003, 0, 0, 4, 0, 0, 1, 0x100, 1, 0};
	CHECK(interpreter.ExecuteCommand(command)); // Copy caller event 1 slot 1 to current event 2 slot 4.
	CHECK(Game_ManiacSelfVariables::ReadNumber(Main_Data::game_maniac_self_variables->Find(2, 1, 2), -4) == 42);
	command.parameters = {0x1003, 0, 0, 8, 0, 0, 4, 0, 0, 0};
	CHECK(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_maniac_self_variables->Find(2, 1, 2)->numbers.size() >= 9);
	CHECK(Game_ManiacSelfVariables::ReadNumber(Main_Data::game_maniac_self_variables->Find(2, 1, 2), -8) == 0);

	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Main_Data::game_strings.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac selfvar literal common name cache is shared within a command") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	lcf::Data::commonevents.clear();
	for (const auto& name : {"Alpha", "Beta"}) {
		lcf::Data::commonevents.emplace_back();
		auto& event = lcf::Data::commonevents.back();
		event.ID = static_cast<int32_t>(lcf::Data::commonevents.size());
		event.name = lcf::DBString(name);
	}
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	auto& state = *Main_Data::game_maniac_self_variables;
	state.Ensure(1, 0, 1)->numbers = {0, 42};
	state.Ensure(1, 0, 2)->numbers = {0, 99};
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Selfvar);
	command.parameters = {0x10311031, 5, 0, 2, 4, 0, 1, 0, 1, 0};
	command.string = lcf::DBString("AlphaBeta");
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Game_ManiacSelfVariables::ReadNumber(state.Find(1, 0, 1), -2) == 42);
	CHECK(Game_ManiacSelfVariables::ReadNumber(state.Find(1, 0, 2), -2) == 0);
	lcf::rpg::EventCommand second_command;
	second_command.code = command.code;
	second_command.parameters = {0x00311031, 5, 0, 3, 4, 0, 4, 0x10, 1, 0};
	second_command.string = lcf::DBString("AlphaBetaValue");
	REQUIRE(interpreter.ExecuteCommand(second_command));
	auto* target = state.Find(1, 0, 1);
	REQUIRE(target != nullptr);
	REQUIRE(target->strings.size() > 3);
	CHECK(static_cast<std::string_view>(target->strings[3]) == "Beta"); // Cached source name leaves the text cursor at byte five.
	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac selfvar literal common name cache persists across executions") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	lcf::Data::commonevents.clear();
	lcf::Data::commonevents.emplace_back();
	lcf::Data::commonevents.back().ID = 1;
	lcf::Data::commonevents.back().name = lcf::DBString("Alpha");
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Selfvar);
	command.parameters = {0x1031, 5, 0, 4, 0, 0, 4, 0x10, 1, 0};
	command.string = lcf::DBString("AlphaBeta");
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	REQUIRE(interpreter.ExecuteCommand());
	CHECK(interpreter.GetState().stack.front().commands[0].easyrpg_maniac_common_name_cache == 1);
	const auto* bank = Main_Data::game_maniac_self_variables->Find(1, 0, 1);
	REQUIRE(bank != nullptr);
	REQUIRE(bank->strings.size() > 4);
	CHECK(static_cast<std::string_view>(bank->strings[4]) == "Beta");
	REQUIRE(interpreter.ExecuteCommand());
	CHECK(static_cast<std::string_view>(bank->strings[4]) == "Alph");
	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac ordinary variable and switch controls share the current self bank") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars);
	command.parameters = {0, -1, 0, 0, 0, 10, 0};
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-1) == 10);
	command.parameters = {1, -1, -3, 1, 1, -1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-1) == 20);
	CHECK(interpreter.ReadVariable(-2) == 20);
	CHECK(interpreter.ReadVariable(-3) == 20);
	command.parameters = {0, -4, 0, 0, 0, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	command.parameters = {0, -5, 0, 0, 2, -4, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-5) == 20);
	command.parameters = {0, -6, 0, 0, 0, -1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	command.parameters = {0, -6, 0, 10, 0, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-6) == 0x7fffffff);

	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlSwitches);
	command.parameters = {1, -3, -1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadSwitch(-1) == 1);
	CHECK(interpreter.ReadSwitch(-2) == 1);
	CHECK(interpreter.ReadSwitch(-3) == 1);
	command.parameters = {1, -2, 3, 2}; // Mixed signs affect only the first switch.
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadSwitch(-1) == 1);
	CHECK(interpreter.ReadSwitch(-2) == 0);
	CHECK(interpreter.ReadSwitch(-3) == 1);
	command.parameters = {2, -4, 0, 0}; // Negative reference forces a self destination.
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadSwitch(-1) == 1);
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars);
	command.parameters = {0, -7, 0, 0, 10, -1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-7) == 1);
	command.parameters = {0, 20, 0, 0, 0, 2000000000, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(20) == 2000000000); // Raw assignment bypasses RPG bounds.
	command.parameters = {0, 20, 0, 10, 0, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(20) == 1000000000);
	command.parameters = {0, 21, 0, 0, 0, -1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	command.parameters = {0, 21, 0, 10, 0, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(21) == 2147483647);
	command.parameters = {1, 22, 23, 0, 0, 5, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(22) == 5);
	CHECK(Main_Data::game_variables->Get(23) == 5);
	Main_Data::game_variables->Set(40, 1);
	command.parameters = {1, 40, 42, 0, 22, 0x100, 0, 100, 40, 1};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(40) == 100);
	CHECK(Main_Data::game_variables->Get(41) == 10000);
	CHECK(Main_Data::game_variables->Get(42) == 10000);
	command.parameters = {0, 45, 0, 0, 22, 0, 0, 2000000000, 2, 1};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(45) == INT32_MIN);
	command.parameters = {0, -8, 0, 0, 23, 0, -1, 3};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-8) == 60); // -1,-2,-3 each contain 20.
	command.parameters = {0, -9, 0, 0, 24, 0, -1, 3};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-9) == 20);
	command.parameters = {0, -10, 0, 0, 25, 0, -1, 1};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-10) == INT32_MIN); // Native max skips its final element.
	command.parameters = {4, 6, 0, 0, 0, 7, 2, 0x011e0142, 32};
	REQUIRE(interpreter.ExecuteCommand(command)); // Range expression returns both endpoints.
	CHECK(Main_Data::game_variables->Get(30) == 7);
	CHECK(Main_Data::game_variables->Get(31) == 7);
	CHECK(Main_Data::game_variables->Get(32) == 7);
	command.parameters = {5, 6, 0, 0, 0, 8, 2, 0x010b0142, 12};
	REQUIRE(interpreter.ExecuteCommand(command)); // Other expression modes force self targets.
	CHECK(interpreter.ReadVariable(-11) == 8);
	CHECK(interpreter.ReadVariable(-12) == 8);
	Main_Data::game_variables->Set(30, 9);
	command.parameters = {1, 30, 32, 1, 21, 1, 0x001e0108};
	REQUIRE(interpreter.ExecuteCommand(command)); // Expression is evaluated for each destination.
	CHECK(Main_Data::game_variables->Get(30) == 18);
	CHECK(Main_Data::game_variables->Get(31) == 25);
	CHECK(Main_Data::game_variables->Get(32) == 25);
	command.parameters = {0, 33, 0, 0, 21, INT32_MAX, 0};
	REQUIRE(interpreter.ExecuteCommand(command)); // Oversized operand envelope remains within the record.
	CHECK(Main_Data::game_variables->Get(33) == 0);
	command.parameters = {0, 30, 0, 4, 0, 2, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(30) == 9); // Native operation 4 divides.
	command.parameters[3] = 5;
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(30) == 1); // Native operation 5 takes the remainder.
	command.parameters = {0, 34, 0, 0, 21, 2, static_cast<int32_t>(0xffff0308u), 0x0000ffff};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(34) == 20); // Expression token 8 reads the current self bank.
	command.parameters = {0, 35, 0, 0, 21, 1, 0x0001010b};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(35) == 20); // Token 11 forces a self address.
	command.parameters = {0, 36, 0, 0, 21, 1, 0x00000119};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(36) == 1); // Logical NOT of zero.
	command.parameters = {0, 37, 0, 0, 21, 1, 0x00010119};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(37) == 0);
	command.parameters = {0, 38, 0, 0, 0, 0, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	command.parameters = {0, 39, 0, 0, 21, 3, 0x01014030, 0x26010822, 0x07016301};
	REQUIRE(interpreter.ExecuteCommand(command)); // OR skips assignment and positions the following child.
	CHECK(Main_Data::game_variables->Get(39) == 8);
	CHECK(Main_Data::game_variables->Get(38) == 0);
	command.parameters = {0, 40, 0, 0, 21, 3, 0x00014130, 0x26010822, 0x07016301};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(40) == 7);
	CHECK(Main_Data::game_variables->Get(38) == 0);
	command.parameters = {0, 41, 0, 0, 21, 4, 0x01014830, 0x08220501, 0x63012601, 0x00000701};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(41) == 12);
	CHECK(Main_Data::game_variables->Get(38) == 0);
	command.parameters = {0, 42, 0, 0, 21, 2, static_cast<int32_t>(0xffff0339u), 0x0101ffff};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(42) == INT32_MAX); // Unsigned right shift of -1.
	command.parameters = {0, 43, 0, 0, 21, 2, 0x01050134, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(43) == 0); // Remainder divisor zero becomes one.
	command.parameters = {0, 44, 0, 0, 21, 2, static_cast<int32_t>(0x94000330u), 0x01017735};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(44) == Main_Data::game_variables->GetMaxValue());
	command.parameters = {0, 46, 0, 0, 21, 3, 0x00000333, 0x01188000, 0x00000001};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(46) == Main_Data::game_variables->GetMaxValue());
	command.parameters = {0, 48, 0, 0, 21, 4, 0x22021330, 0x012f0108, 0x01060109, 0x00000007};
	REQUIRE(interpreter.ExecuteCommand(command)); // List evaluates children for effects and returns zero.
	CHECK(Main_Data::game_variables->Get(47) == 9);
	CHECK(Main_Data::game_variables->Get(48) == 7);
	command.parameters = {0, 49, 0, 0, 21, 2, 0x04014230, 0x07010901};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(49) == 11); // Scalar consumer takes the range primary.
	command.parameters = {4, 6, 0, 0, 0, 55, 4, 0x42021343, 0x0b010a01, 0x010c0142, 0x0001010d};
	REQUIRE(interpreter.ExecuteCommand(command)); // Indexed list preserves the selected pair.
	CHECK(Main_Data::game_variables->Get(12) == 55);
	CHECK(Main_Data::game_variables->Get(13) == 55);
	command.parameters = {0, 51, 0, 0, 21, 3, 0x22000c4e, 0x01320108, 0x32010803};
	REQUIRE(interpreter.ExecuteCommand(command)); // Selector arity is fixed; marker zero does not suppress children.
	CHECK(Main_Data::game_variables->Get(50) == 3);
	CHECK(Main_Data::game_variables->Get(51) == 3); // Second child sees the first child assignment.
	command.parameters = {0, 52, 0, 0, 21, 3, 0x03010b22, 0x35940003, 0x00000077};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-3) == 2000000000); // Self scalar assignment keeps the raw value.
	CHECK(Main_Data::game_variables->Get(52) == 2000000000);
	command.parameters = {0, 53, 0, 0, 21, 2, 0x04010c22, 0x00000101};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadSwitch(-4) == 1);
	command.parameters = {0, 54, 0, 0, 21, 2, 0x01011022, 0x00000501};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-20) == 5); // Self-indirect lvalue resolves after the RHS.
	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac expression assignments preserve list copy and broadcast behavior") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars);
	command.parameters = {0, 150, 0, 0, 21, 0};
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	const auto evaluate = [&](std::initializer_list<uint8_t> bytes) {
		std::vector<uint8_t> padded(bytes);
		while (padded.size() % 4 != 0) padded.push_back(0);
		std::vector<int32_t> parameters = {0, 150, 0, 0, 21, static_cast<int32_t>(padded.size() / 4)};
		for (size_t index = 0; index < padded.size(); index += 4) {
			const uint32_t word = static_cast<uint32_t>(padded[index]) |
				(static_cast<uint32_t>(padded[index + 1]) << 8) |
				(static_cast<uint32_t>(padded[index + 2]) << 16) |
				(static_cast<uint32_t>(padded[index + 3]) << 24);
			parameters.push_back(static_cast<int32_t>(word));
		}
		command.parameters = lcf::DBArray<int32_t>(parameters.begin(), parameters.end());
		REQUIRE(interpreter.ExecuteCommand(command));
	};

	evaluate({34, 8, 66, 1, 60, 1, 62, 19, 3, 1, 7, 1, 8, 1, 9});
	CHECK(Main_Data::game_variables->Get(60) == 7);
	CHECK(Main_Data::game_variables->Get(61) == 8);
	CHECK(Main_Data::game_variables->Get(62) == 9);
	CHECK(Main_Data::game_variables->Get(150) == 9);
	evaluate({34, 8, 66, 1, 61, 1, 63, 8, 66, 1, 60, 1, 62});
	CHECK(Main_Data::game_variables->Get(61) == 7);
	CHECK(Main_Data::game_variables->Get(62) == 7);
	CHECK(Main_Data::game_variables->Get(63) == 7);
	evaluate({35, 11, 66, 1, 70, 1, 72, 1, 2});
	CHECK(Main_Data::game_variables->Get(70) == 6); // Numeric broadcast repeats the first global pointer.
	CHECK(interpreter.ReadVariable(-70) == 0);
	evaluate({34, 11, 1, 1, 1, 3});
	evaluate({35, 11, 66, 1, 70, 1, 72, 11, 1, 1});
	CHECK(interpreter.ReadVariable(-70) == 9); // RHS token 11 selects self for broadcast.
	CHECK(interpreter.ReadVariable(-71) == 0);
	evaluate({34, 9, 66, 1, 80, 1, 82, 1, 1});
	CHECK(Main_Data::game_switches->Get(80));
	CHECK(Main_Data::game_switches->Get(81));
	CHECK_FALSE(Main_Data::game_switches->Get(82)); // Broadcast omits the last switch.
	Main_Data::game_variables->Set(90, 0);
	Main_Data::game_variables->Set(91, 4);
	Main_Data::game_variables->Set(92, -2);
	evaluate({34, 9, 66, 1, 80, 1, 82, 9, 66, 1, 90, 1, 92});
	CHECK_FALSE(Main_Data::game_switches->Get(80));
	CHECK(Main_Data::game_switches->Get(81));
	CHECK(Main_Data::game_switches->Get(82)); // Optimized switch source reads numeric variables.
	evaluate({34, 8, 66, 1, 95, 1, 96, 19, 3, 1, 10, 1, 11,
		34, 8, 1, 97, 1, 5});
	CHECK(Main_Data::game_variables->Get(95) == 10);
	CHECK(Main_Data::game_variables->Get(96) == 11);
	CHECK(Main_Data::game_variables->Get(97) == 5); // Extra list elements still execute.
	CHECK(Main_Data::game_variables->Get(150) == 11);
	evaluate({34, 12, 66, 1, 85, 1, 87, 19, 3, 1, 0, 1, 1, 1, 2});
	CHECK(interpreter.ReadSwitch(-85) == 0);
	CHECK(interpreter.ReadSwitch(-86) == 1);
	CHECK(interpreter.ReadSwitch(-87) == 1);
	auto* self_bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
	REQUIRE(self_bank != nullptr);
	self_bank->switches.resize(89);
	self_bank->switches[88] = 7;
	evaluate({42, 12, 1, 88, 1, 7});
	CHECK(interpreter.ReadSwitch(-88) == 0); // Switch XOR compares raw byte to whole RHS.
	Main_Data::game_switches->SetManiacRaw(0, 7);
	evaluate({34, 9, 1, 0, 1, 1});
	CHECK(Main_Data::game_switches->GetManiacRawSlot(0) == 7); // Direct assignment ignores zero.
	evaluate({42, 9, 1, 0, 1, 7});
	CHECK(Main_Data::game_switches->GetManiacRawSlot(0) == 0); // Compound assignment can write it.
	Main_Data::game_switches->SetManiacRaw(0, 7);
	evaluate({42, 9, 1, 0, 2, 7, 1});
	CHECK(Main_Data::game_switches->GetManiacRawSlot(0) == 1); // XOR compares against whole int32 263.
	Main_Data::game_switches->SetManiacRaw(0, 7);
	evaluate({41, 9, 1, 0, 2, 0, 1});
	CHECK(Main_Data::game_switches->GetManiacRawSlot(0) == 0); // AND uses low byte of 256.
	Main_Data::game_variables->Set(1, 101);
	evaluate({34, 16, 24, 1, 1, 1, 5});
	CHECK(Main_Data::game_variables->Get(101) == 5); // Negative raw address becomes global.
	evaluate({34, 11, 1, 1, 1, 102});
	evaluate({34, 16, 1, 1, 1, 6});
	CHECK(interpreter.ReadVariable(-102) == 6); // Positive raw address resolves through self.
	evaluate({78, 19, 4, 1, 0, 1, 100, 1, 1, 1, 2});
	CHECK(Main_Data::game_variables->Get(150) == 50);
	Main_Data::game_variables->Set(120, 1);
	Main_Data::game_variables->Set(121, 5);
	Main_Data::game_variables->Set(122, -2);
	evaluate({78, 20, 0, 1, 120, 1, 3});
	CHECK(Main_Data::game_variables->Get(150) == 4); // Encoded arity does not change fixed evaluation.
	evaluate({78, 21, 2, 1, 120, 1, 3});
	CHECK(Main_Data::game_variables->Get(150) == -2);
	evaluate({78, 22, 2, 1, 120, 1, 3});
	CHECK(Main_Data::game_variables->Get(150) == 5);
	evaluate({78, 20, 2, 24, 1, 1, 1, 2});
	CHECK(Main_Data::game_variables->Get(150) == 102); // Self numeric sequence starts at -1.
	evaluate({78, 20, 2, 1, 130, 1, 0});
	CHECK(Main_Data::game_variables->Get(150) == 0);
	CHECK(Main_Data::game_variables->GetSize() >= 131); // Preparation runs even with a zero sum count.
	evaluate({34, 10, 34, 8, 1, 140, 1, 7, 34, 8, 1, 141, 1, 8});
	CHECK(Main_Data::game_variables->Get(140) == 7);
	CHECK(Main_Data::game_variables->Get(141) == 8);
	CHECK(Main_Data::game_variables->Get(150) == 0); // Invalid selector still evaluates two children.
	evaluate({34, 8, 24, 1, 142, 1, 9});
	CHECK(Main_Data::game_variables->Get(142) == 9); // Direct global lvalue takes abs32.
	evaluate({34, 8, 1, 0, 1, 13});
	CHECK(Main_Data::game_variables->GetManiacRawSlot(0) == 13);
	CHECK(Main_Data::game_variables->Get(0) == 0);
	evaluate({35, 8, 1, 0, 1, 4});
	CHECK(Main_Data::game_variables->GetManiacRawSlot(0) == 17); // Compound lvalue reads the raw slot.
	Main_Data::game_variables->Set(146, 12);
	Main_Data::game_variables->Set(147, 13);
	evaluate({35, 8, 66, 1, 145, 1, 147, 19, 1, 1, 2});
	CHECK(Main_Data::game_variables->Get(145) == 2);
	CHECK(Main_Data::game_variables->Get(146) == 12);
	CHECK(Main_Data::game_variables->Get(147) == 13); // Zero fill applies += 0, not clearing.
	const std::array<int32_t, 1> expression_words = {0x00000701};
	const auto arguments = ManiacPatch::ParseExpressions(MakeSpan(expression_words), interpreter);
	CHECK(arguments == std::vector<int32_t>{7, 0, 0}); // 3019 mode 4 consumes padding tokens.

	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac variable arrays use global and self storage") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlVarArray);
	command.parameters = {5, 0, -10, 3, 7};
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-10) == 7);
	CHECK(interpreter.ReadVariable(-11) == 8);
	CHECK(interpreter.ReadVariable(-12) == 9);
	command.parameters = {0, 0, -10, 3, 30};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(30) == 7);
	CHECK(Main_Data::game_variables->Get(31) == 8);
	CHECK(Main_Data::game_variables->Get(32) == 9);
	command.parameters = {22, 0, -11, 2, 30};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-11) == 7);
	CHECK(interpreter.ReadVariable(-12) == 8);
	command.parameters = {1, 0, -10, 2, -11};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-10) == 7);
	CHECK(interpreter.ReadVariable(-11) == 8);
	CHECK(interpreter.ReadVariable(-12) == 7);
	command.parameters = {6, 0, -10, 2, 30};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-10) == 14);
	CHECK(interpreter.ReadVariable(-11) == 16);
	command.parameters = {21, 0, -10, 3, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-10) == 7);
	CHECK(interpreter.ReadVariable(-12) == 14);
	Main_Data::game_variables->Set(40, 41);
	Main_Data::game_variables->Set(41, 99);
	command.parameters = {16, 0, 40, 1, 50};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(50) == 99);
	command.parameters = {20, 0, 60, 2, 2000000000};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(60) == 2000000000);
	CHECK(Main_Data::game_variables->Get(61) == 2000000000);
	command.parameters = {15, 0, 60, 1, 30};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(60) == 15625000); // Logical right shift by low five bits.
	Main_Data::game_variables->Set(70, 3);
	Main_Data::game_variables->Set(71, 1);
	Main_Data::game_variables->Set(72, 2);
	Main_Data::game_variables->Set(80, 30);
	Main_Data::game_variables->Set(81, 10);
	Main_Data::game_variables->Set(82, 20);
	command.parameters = {17, 0, 70, 3, 80};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(70) == 1);
	CHECK(Main_Data::game_variables->Get(71) == 2);
	CHECK(Main_Data::game_variables->Get(72) == 3);
	CHECK(Main_Data::game_variables->Get(80) == 10);
	CHECK(Main_Data::game_variables->Get(81) == 20);
	CHECK(Main_Data::game_variables->Get(82) == 30);
	command.parameters[0] = 18;
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(70) == 3);
	CHECK(Main_Data::game_variables->Get(80) == 30);
	command.parameters = {17, 0, 90, 2, 90};
	Main_Data::game_variables->Set(90, 2);
	Main_Data::game_variables->Set(91, 1);
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(90) == 2); // Identical paired arrays undo each swap.
	CHECK(Main_Data::game_variables->Get(91) == 1);
	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac variable arrays resolve modes and preserve allocation order") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlVarArray);
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	command.parameters = {20, 0, 90, 2, 11};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(90) == 11);
	CHECK(Main_Data::game_maniac_self_variables->Find(2, 1, 1) == nullptr);
	command.parameters = {0, 0, 150, 2, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->GetData().size() >= 153); // Copy prepares A2 plus the native extra slot before checking zero A4.
	CHECK(Main_Data::game_maniac_self_variables->Find(2, 1, 1) == nullptr);
	const auto allocated = Main_Data::game_variables->GetData().size();
	command.parameters = {6, 0, 153, 2, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->GetData().size() == allocated); // Arithmetic rejects zero A4 first.
	command.parameters = {5, 0, 160, 2, INT32_MAX};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(160) == INT32_MAX);
	CHECK(Main_Data::game_variables->Get(161) == INT32_MIN);
	auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
	REQUIRE(bank != nullptr);
	bank->numbers.resize(9);
	bank->numbers[5] = 120;
	bank->numbers[6] = 8;
	bank->numbers[8] = 211;
	command.parameters = {5, 0, -120, 2, 31};
	REQUIRE(interpreter.ExecuteCommand(command));
	Main_Data::game_variables->Set(121, 122);
	Main_Data::game_variables->Set(122, 2);
	command.parameters = {0, 0x221, -5, 121, -6};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-211) == 31);
	CHECK(interpreter.ReadVariable(-212) == 32);
	bank->numbers.resize(232);
	bank->numbers[40] = 5;
	bank->numbers[230] = -40;
	bank->numbers[231] = 40;
	Main_Data::game_variables->Set(40, 8);
	command.parameters = {16, 0, -230, 2, 240};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(240) == 8);
	CHECK(Main_Data::game_variables->Get(241) == 5);
	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac variable arrays retain the real SELF capacity boundary") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlVarArray);
	command.parameters = {20, 0, -1000001, 2, 123};
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-1000001) == 123);
	CHECK(interpreter.ReadVariable(-1000002) == 123);
	command.parameters = {0, 0, -1000001, 2, 20};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(20) == 123);
	CHECK(Main_Data::game_variables->Get(21) == 123);
	const auto previous_version = Player::game_config.maniac_version.Get();
	Player::game_config.maniac_version.Set(250921);
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars);
	command.parameters = {0, 22, 0, 0, 23, 0, -1000001, 2};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(22) == 246);
	Player::game_config.maniac_version.Set(previous_version);
	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac variable arrays consume the shared native random stream") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	const uint32_t previous_seed = Main_Data::maniac_random_seed;
	Player::game_config.patch_maniac.Set(1);
	Main_Data::maniac_random_seed = Main_Data::maniac_initial_random_seed;
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlVarArray);
	command.parameters = {4, 0, 100, 4, 0};
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	for (int i = 0; i < 4; ++i) Main_Data::game_variables->Set(100 + i, 10 * (i + 1));
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(100) == 30);
	CHECK(Main_Data::game_variables->Get(101) == 20);
	CHECK(Main_Data::game_variables->Get(102) == 10);
	CHECK(Main_Data::game_variables->Get(103) == 40);
	CHECK(Main_Data::maniac_random_seed == 0x841644a9u); // Final singleton still consumes a step.
	Main_Data::maniac_random_seed = Main_Data::maniac_initial_random_seed;
	for (int i = 0; i < 4; ++i) {
		Main_Data::game_variables->Set(100 + i, i + 1);
		Main_Data::game_variables->Set(110 + i, 10 * (i + 1));
	}
	command.parameters = {19, 0, 100, 4, 110};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(100) == 3);
	CHECK(Main_Data::game_variables->Get(102) == 1);
	CHECK(Main_Data::game_variables->Get(110) == 30);
	CHECK(Main_Data::game_variables->Get(112) == 10);
	CHECK(Main_Data::maniac_random_seed == 0x841644a9u);
	Main_Data::maniac_random_seed = Main_Data::maniac_initial_random_seed;
	CHECK(Main_Data::ManiacRandomInclusive(5, 2) == 5);
	CHECK(Main_Data::maniac_random_seed == 0xc218a2d2u);
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars);
	command.parameters = {0, 120, 0, 0, 3, 2, 5, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(120) == 2); // 10220 shares the next LCG step.
	CHECK(Main_Data::maniac_random_seed == 0x24ff761bu);
	const std::array<int32_t, 2> expression_words = {0x0100004e, 0x00050102};
	CHECK(ManiacPatch::ParseExpression(MakeSpan(expression_words), interpreter) == 5);
	CHECK(Main_Data::maniac_random_seed == 0xfabb3a88u);
	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::maniac_random_seed = previous_seed;
	Graphics::Quit();
}

TEST_CASE("Maniac variable arrays refresh event pages only on native refresh branches") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	Main_Data::game_strings = std::make_unique<Game_Strings>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	auto* event = MockGame::GetEvent(1);
	REQUIRE(event != nullptr);
	auto* page = const_cast<lcf::rpg::EventPage*>(event->GetPage(1));
	REQUIRE(page != nullptr);
	page->condition.flags.variable = true;
	page->condition.variable_id = 90;
	page->condition.variable_value = 1;
	Game_Map::Refresh();
	CHECK(event->GetActivePage() == nullptr);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlVarArray);
	command.parameters = {20, 0, 90, 1, 1};
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(event->GetActivePage() == page); // Fill refreshes inside the command.
	page->condition.variable_value = 2;
	Main_Data::game_variables->Set(91, 2);
	Game_Map::Refresh();
	CHECK(event->GetActivePage() == nullptr);
	command.parameters = {21, 0, 90, 2, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(90) == 2);
	CHECK(event->GetActivePage() == nullptr); // Reverse omits the native refresh.
	Game_Map::Refresh();
	CHECK(event->GetActivePage() == page);
	Main_Data::game_variables->Set(90, 0);
	Main_Data::game_variables->Set(92, 93);
	Main_Data::game_variables->Set(93, 2);
	Game_Map::Refresh();
	CHECK(event->GetActivePage() == nullptr);
	command.parameters = {16, 0, 92, 1, 90};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(90) == 2);
	CHECK(event->GetActivePage() == nullptr); // Dereference also omits it.
	Game_Map::Refresh();
	CHECK(event->GetActivePage() == page);
	*Main_Data::game_variables->GetManiacWritable(90) = 0;
	command.parameters = {6, 0, 90, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(event->GetActivePage() == page); // Guarded no-op does not refresh.
	page->condition.variable_value = 3;
	Game_Map::Refresh();
	CHECK(event->GetActivePage() == nullptr);
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars);
	command.parameters = {0, 90, 0, 0, 0, 3, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(event->GetActivePage() == page); // 10220 also refreshes in the command.
	page->condition.flags.variable = false;
	page->condition.flags.switch_a = true;
	page->condition.switch_a_id = 25;
	Game_Map::Refresh();
	CHECK(event->GetActivePage() == nullptr);
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlSwitches);
	command.parameters = {0, 25, 0, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(event->GetActivePage() == page); // 10210 uses the same immediate map refresh.
	Main_Data::game_switches->Set(25, false);
	Game_Map::Refresh();
	CHECK(event->GetActivePage() == nullptr);
	Main_Data::game_switches->Set(25, true);
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Selfvar);
	command.parameters = {0, 0, 0, 0, 0, 0, 0, 0, 0, 99};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(event->GetActivePage() == page); // Unknown 3036 operation still refreshes.
	Main_Data::game_switches->Set(25, false);
	Game_Map::Refresh();
	Main_Data::game_switches->Set(25, true);
	command.parameters[9] = 1;
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(event->GetActivePage() == page); // Invalid Clear target selector still refreshes.
	page->condition.flags.switch_a = false;
	page->condition.flags.variable = true;
	page->condition.variable_id = 95;
	page->condition.variable_value = 1;
	Game_Map::Refresh();
	CHECK(event->GetActivePage() == nullptr);
	command.parameters = {0x1000, 0, 0, 95, 0, 0, 1, 0, 1, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(95) == 1);
	CHECK(event->GetActivePage() == page); // Scalar 3036 Set refreshes after writing.
	command.parameters = {0x1000, 0, 0, 95, 0, 0, 0, 0, 1, 1};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(95) == 0);
	CHECK(event->GetActivePage() == nullptr); // Clear refreshes after writing.
	Main_Data::game_variables->Set(95, 1);
	Game_Map::Refresh();
	CHECK(event->GetActivePage() == page);
	command.parameters = {0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(95) == 0);
	CHECK(event->GetActivePage() == nullptr); // Init refreshes after freeing global values.
	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Main_Data::game_strings.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac CallCommand redispatches within its caller frame") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_CallCommand);
	command.indent = 2;
	command.parameters = {0x30, 10220, 100, 7, 0, 0, -160, 0, 0, 0, 42, 0};
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.GetState().stack.size() == 1);
	CHECK(interpreter.ReadVariable(-160) == 42);
	command.parameters = {0x40, 3019, 0, 0, 0, static_cast<int32_t>(0xa1010822u), 0x00000901};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.GetState().stack.size() == 1);
	CHECK(Main_Data::game_variables->Get(161) == 0); // Generated 3019 skips argument evaluation.
	const int32_t generated[] = {0, -162, 0, 0, 0, 61};
	for (int i = 0; i < 6; ++i) Main_Data::game_variables->Set(200 + i, generated[i]);
	command.parameters = {0, 10220, 200, 6, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-162) == 61);
	CHECK(Main_Data::game_variables->GetData().size() >= 207); // Native source preparation keeps one extra slot.
	Main_Data::game_variables->Set(210, 200);
	Main_Data::game_variables->Set(211, 6);
	command.parameters = {0x110, 10220, 210, 211, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.GetState().stack.size() == 1);
	Main_Data::game_variables->Set(212, 210);
	command.parameters = {0x20, 10220, 212, 6, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	auto* bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
	REQUIRE(bank != nullptr);
	bank->numbers.resize(206);
	for (int i = 0; i < 6; ++i) bank->numbers[200 + i] = generated[i];
	bank->numbers[205] = 62;
	command.parameters = {0, 10220, -200, 6, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-162) == 62);
	const auto previous_version = Player::game_config.maniac_version.Get();
	Player::game_config.maniac_version.Set(250921);
	bank->numbers.resize(1000007);
	for (int i = 0; i < 6; ++i) bank->numbers[1000001 + i] = generated[i];
	bank->numbers[1000006] = 63;
	command.parameters = {0, 10220, -1000001, 6, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-162) == 63);
	CHECK(bank->numbers.size() == 1000008); // Real source preparation, including its zero tail.
	Player::game_config.maniac_version.Set(previous_version);
	Player::game_config.maniac_version.Set(250921);
	// Native inline values/mode words absent from the record default to zero.
	// A small valid input still produces the real count beyond the removed cutoff.
	command.parameters = {0x30, 10220, 12, 1000001, 0, 0, -164, 0, 0, 0, 64, 0, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-164) == 64);
	CHECK(interpreter.GetState().stack.size() == 1);
	Player::game_config.maniac_version.Set(previous_version);
	Main_Data::game_variables->Set(220, 88);
	Main_Data::game_variables->Set(221, 220);
	command.parameters = {0x30, 10220, 11, 6, 0, 0, -163, 0, 0, 0, 220, 0, 0x10};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-163) == 88);
	command.parameters[10] = 221;
	command.parameters[12] = 0x20;
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-163) == 88); // Inline mode 2 follows two numeric references.
	command.parameters = {0x40, 10110, 0, 0, 0, static_cast<int32_t>(0xaa010823u), 0x00000101};
	CHECK_FALSE(interpreter.ExecuteCommand(command)); // No message window: busy generated command retries.
	CHECK(interpreter.GetState().stack.size() == 1);
	CHECK(Main_Data::game_variables->Get(170) == 1);
	CHECK_FALSE(interpreter.ExecuteCommand(command));
	CHECK(Main_Data::game_variables->Get(170) == 2); // Expression effects recur on retry.
	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac selfvar Clear and Init resolve map selector mode five") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	auto& state = *Main_Data::game_maniac_self_variables;
	state.Ensure(2, 1, 2)->numbers = {0, 22};
	state.Ensure(2, 1, 3)->numbers = {0, 33};
	state.Ensure(2, 2, 2)->numbers = {0, 44};
	Main_Data::game_variables->Set(30, 2);
	Main_Data::game_variables->Set(31, 2);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_Selfvar);
	command.parameters = {0x1052, 30, 1, 1, 0, 0, 0, 0, 1, 1};
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(Game_ManiacSelfVariables::ReadNumber(state.Find(2, 1, 2), -1) == 0);
	CHECK(Game_ManiacSelfVariables::ReadNumber(state.Find(2, 1, 3), -1) == 33);
	CHECK(Game_ManiacSelfVariables::ReadNumber(state.Find(2, 2, 2), -1) == 44);
	command.parameters = {0x502, 2, 31, 0, 0, 0, 0, 0, 1, 2};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(state.Find(2, 2, 2)->numbers.empty());
	CHECK(Game_ManiacSelfVariables::ReadNumber(state.Find(2, 1, 3), -1) == 33);
	command.parameters = {0x3002, 9, 1, 1, 0, 0, 0, 0, 1, 1};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(state.Find(2, 1, 9) == nullptr); // Invalid Clear ID mode cannot allocate.
	state.Ensure(2, 1, 4)->numbers = {0, 77};
	command.parameters = {0x102, 3, 1, 0, 0, 0, 0, 0, 1, 2};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(state.Find(2, 1, 3)->numbers.empty());
	CHECK(Game_ManiacSelfVariables::ReadNumber(state.Find(2, 1, 4), -1) == 77);
	state.Ensure(2, 1, 1)->numbers = {0, 2};
	state.Ensure(2, 1, 2)->numbers = {0, 88};
	state.Ensure(2, 2, 2)->numbers = {0, 99};
	command.parameters = {0x502, 2, -1, 0, 0, 0, 0, 0, 1, 2};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(state.Find(2, 1, 2)->numbers.empty());
	CHECK(state.Find(2, 2, 2)->numbers.empty());
	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac CallEvent resolves common names and ID offsets") {
	lcf::Data::commonevents.clear();
	for (int id = 1; id <= 2; ++id) {
		lcf::Data::commonevents.emplace_back();
		auto& event = lcf::Data::commonevents.back();
		event.ID = id;
		event.name = lcf::DBString(id == 1 ? "汉" : "目标");
		lcf::rpg::EventCommand end;
		end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::END);
		event.event_commands.push_back(end);
	}
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	Main_Data::game_strings = std::make_unique<Game_Strings>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
	command.parameters = {5, 2, 0, 0, 1, 0, 0};
	command.string = lcf::DBString("汉");
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	REQUIRE(interpreter.ExecuteCommand(command));
	REQUIRE(interpreter.GetState().stack.size() == 2);
	CHECK(interpreter.GetState().stack.back().easyrpg_self_scope == 1);
	CHECK(interpreter.GetState().stack.back().easyrpg_self_event_id == 2);
	CHECK(command.easyrpg_maniac_common_name_cache == 1); // Cache the pre-offset ID.
	command.string = lcf::DBString("无"); // Cache hit still calls the text slicer.
	REQUIRE(interpreter.ExecuteCommand(command));
	REQUIRE(interpreter.GetState().stack.size() == 3);
	CHECK(interpreter.GetState().stack.back().easyrpg_self_event_id == 2);
	lcf::rpg::EventCommand short_command;
	short_command.code = command.code;
	short_command.parameters = {5, 2};
	short_command.string = lcf::DBString("汉");
	Game_Interpreter short_call;
	short_call.Push({short_command}, 1);
	REQUIRE(short_call.ExecuteCommand(short_command));
	CHECK(short_command.easyrpg_maniac_common_name_cache == 1);
	REQUIRE(short_call.GetState().stack.size() == 2);
	CHECK(short_call.GetState().stack.back().easyrpg_self_event_id == 1);
	Main_Data::game_strings->Asg(10, "汉");
	command.parameters = {6, 10, 0, 0, 0, 0, 0};
	Game_Interpreter by_string;
	by_string.Push({command}, 1);
	REQUIRE(by_string.ExecuteCommand(command));
	REQUIRE(by_string.GetState().stack.size() == 2);
	CHECK(by_string.GetState().stack.back().easyrpg_self_event_id == 1);
	Main_Data::game_variables->Set(11, 10);
	command.parameters = {7, 11, 0, 0, 0, 0, 0};
	Game_Interpreter by_string_reference;
	by_string_reference.Push({command}, 1);
	REQUIRE(by_string_reference.ExecuteCommand(command));
	REQUIRE(by_string_reference.GetState().stack.size() == 2);
	CHECK(by_string_reference.GetState().stack.back().easyrpg_self_event_id == 1);
	auto* caller_bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
	REQUIRE(caller_bank != nullptr);
	caller_bank->numbers = {0, 13};
	command.parameters = {5, 2, 0, 0, 0, 0, 4, 1, 11, 4, 1, 7, 258, 2, -1};
	command.string = lcf::DBString("汉Z");
	Game_Interpreter typed;
	typed.Push({command}, 1);
	REQUIRE(typed.ExecuteCommand(command));
	const auto* bank = Main_Data::game_maniac_self_variables->Find(1, 0, 1);
	REQUIRE(bank != nullptr);
	CHECK(bank->numbers.size() >= 3);
	CHECK(bank->numbers[1] == 11);
	CHECK(bank->numbers[2] == 13);
	CHECK(bank->strings.size() >= 2);
	CHECK(bank->strings[1] == lcf::DBString("Z"));
	CHECK(bank->switches.size() >= 2);
	CHECK(bank->switches[1] == 2);
	Main_Data::game_strings->SetManiacRawSlot(12, "");
	command.parameters = {0, 1, 0, 0, 0, 0, 1, 5, 12};
	Game_Interpreter empty_argument;
	empty_argument.Push({command}, 1);
	REQUIRE(empty_argument.ExecuteCommand(command));
	bank = Main_Data::game_maniac_self_variables->Find(1, 0, 1);
	CHECK(Game_ManiacSelfVariables::IsStringPresent(bank, 1));
	command.parameters = {0, 1, 0, 0, 0, 0, 1, 4, 0};
	REQUIRE(empty_argument.ExecuteCommand(command));
	bank = Main_Data::game_maniac_self_variables->Find(1, 0, 1);
	CHECK_FALSE(Game_ManiacSelfVariables::IsStringPresent(bank, 1));
	command.parameters = {0, 1, 0, 0, 0, 0, 2, 1, 44, 2, -1};
	Game_Interpreter recursive;
	recursive.Push(&Game_Map::GetCommonEvents()[0]);
	REQUIRE(recursive.ExecuteCommand(command));
	bank = Main_Data::game_maniac_self_variables->Find(1, 0, 1);
	REQUIRE(bank != nullptr);
	CHECK(bank->numbers[1] == 44);
	CHECK(bank->numbers[2] == 44); // The second source sees the first write.
	caller_bank = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
	caller_bank->numbers = {0, 13, 1};
	caller_bank->strings = {lcf::DBString(""), lcf::DBString("self string")};
	caller_bank->switches = {0, 7};
	command.parameters = {99, 2, 0, 0, 0, 0, 2, 6, -2, 9, -2};
	Game_Interpreter negative_reference;
	negative_reference.Push({command}, 1);
	REQUIRE(negative_reference.ExecuteCommand(command));
	bank = Main_Data::game_maniac_self_variables->Find(1, 0, 2);
	REQUIRE(bank != nullptr);
	CHECK(bank->strings[1] == lcf::DBString("self string"));
	CHECK(bank->switches[1] == 7);
	command.parameters = {0, 1, 0, 0x20, 0, -3};
	Game_Interpreter receiving;
	receiving.Push({command}, 1);
	REQUIRE(receiving.ExecuteCommand(command));
	REQUIRE(receiving.GetState().stack.size() == 2);
	CHECK(receiving.GetState().stack.back().easyrpg_maniac_return_type == 2);
	CHECK(receiving.GetState().stack.back().easyrpg_maniac_return_address == -3);
	command.parameters = {0, 1, 0};
	Game_Interpreter legacy;
	legacy.Push({command}, 1);
	REQUIRE(legacy.ExecuteCommand(command));
	CHECK(legacy.GetState().stack.back().easyrpg_maniac_return_type == 0);
	CHECK(legacy.GetState().stack.back().easyrpg_maniac_return_address == 0);
	auto* return_bank = Main_Data::game_maniac_self_variables->Ensure(1, 0, 1);
	REQUIRE(return_bank != nullptr);
	return_bank->numbers = {0, 8};
	return_bank->strings = {lcf::DBString(""), lcf::DBString("keep")};
	return_bank->switches = {0, 3};
	lcf::rpg::EventCommand finish;
	finish.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::EndEventProcessing);
	finish.parameters = {5}; // Clear number and switch storage, preserve strings.
	REQUIRE(legacy.ExecuteCommand(finish));
	CHECK(return_bank->numbers.empty());
	CHECK(return_bank->switches.empty());
	CHECK(return_bank->strings[1] == lcf::DBString("keep"));
	finish.parameters = {1, 1, 91};
	REQUIRE(receiving.ExecuteCommand(finish));
	CHECK(Game_ManiacSelfVariables::ReadNumber(Main_Data::game_maniac_self_variables->Find(2, 1, 1), -3) == 91);
	CHECK(Main_Data::game_maniac_self_variables->Find(1, 0, 1)->numbers.empty());
	command.parameters = {0, 1, 0, 0x30, 0, -2};
	Game_Interpreter indirect_return;
	indirect_return.Push({command}, 1);
	REQUIRE(indirect_return.ExecuteCommand(command));
	auto* caller_for_return = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
	caller_for_return->numbers.resize(3);
	caller_for_return->numbers[2] = 5;
	finish.parameters = {0, 1, 77};
	REQUIRE(indirect_return.ExecuteCommand(finish));
	CHECK(Game_ManiacSelfVariables::ReadNumber(Main_Data::game_maniac_self_variables->Find(2, 1, 1), -5) == 77);
	Main_Data::game_variables->Set(50, -4);
	command.parameters = {0, 1, 0, 0x30, 0, 50};
	Game_Interpreter positive_indirect;
	positive_indirect.Push({command}, 1);
	REQUIRE(positive_indirect.ExecuteCommand(command));
	finish.parameters = {0, 1, 55};
	REQUIRE(positive_indirect.ExecuteCommand(finish));
	CHECK(Game_ManiacSelfVariables::ReadNumber(Main_Data::game_maniac_self_variables->Find(1, 0, 1), -4) == 55);
	command.parameters = {0, 1, 0, 0x50, 0, -4};
	Game_Interpreter string_return;
	string_return.Push({command}, 1);
	REQUIRE(string_return.ExecuteCommand(command));
	finish.parameters = {0, 4, 0};
	finish.string = lcf::DBString("returned");
	REQUIRE(string_return.ExecuteCommand(finish));
	const auto* caller_after_string = Main_Data::game_maniac_self_variables->Find(2, 1, 1);
	REQUIRE(caller_after_string != nullptr);
	REQUIRE(caller_after_string->strings.size() >= 5);
	CHECK(caller_after_string->strings[4] == lcf::DBString("returned"));
	Main_Data::game_strings->SetManiacRawSlot(74, "");
	command.parameters = {0, 1, 0, 0x50, 0, -8};
	Game_Interpreter empty_string_return;
	empty_string_return.Push({command}, 1);
	REQUIRE(empty_string_return.ExecuteCommand(command));
	finish.parameters = {0, 5, 74};
	REQUIRE(empty_string_return.ExecuteCommand(finish));
	caller_after_string = Main_Data::game_maniac_self_variables->Find(2, 1, 1);
	CHECK(Game_ManiacSelfVariables::IsStringPresent(caller_after_string, 8));
	Main_Data::game_variables->Set(51, -6);
	command.parameters = {0, 1, 0, 0x60, 0, 51};
	Game_Interpreter indirect_string_return;
	indirect_string_return.Push({command}, 1);
	REQUIRE(indirect_string_return.ExecuteCommand(command));
	finish.parameters = {0, 1, -42};
	REQUIRE(indirect_string_return.ExecuteCommand(finish));
	const auto* callee_after_string = Main_Data::game_maniac_self_variables->Find(1, 0, 1);
	REQUIRE(callee_after_string != nullptr);
	REQUIRE(callee_after_string->strings.size() >= 7);
	CHECK(callee_after_string->strings[6] == lcf::DBString("-42"));
	command.parameters = {0, 1, 0, 0x50, 0, -7};
	Game_Interpreter switch_to_string;
	switch_to_string.Push({command}, 1);
	REQUIRE(switch_to_string.ExecuteCommand(command));
	auto* source_switch_bank = Main_Data::game_maniac_self_variables->Ensure(1, 0, 1);
	REQUIRE(source_switch_bank != nullptr);
	source_switch_bank->switches.resize(2);
	source_switch_bank->switches[1] = 7;
	finish.parameters = {0, 8, -1};
	REQUIRE(switch_to_string.ExecuteCommand(finish));
	const auto* caller_switch_string = Main_Data::game_maniac_self_variables->Find(2, 1, 1);
	REQUIRE(caller_switch_string != nullptr);
	REQUIRE(caller_switch_string->strings.size() >= 8);
	CHECK(caller_switch_string->strings[7] == lcf::DBString("1"));
	command.parameters = {0, 1, 0, 0x20, 0, 75};
	Game_Interpreter decimal_literal;
	decimal_literal.Push({command}, 1);
	REQUIRE(decimal_literal.ExecuteCommand(command));
	finish.parameters = {0, 4, 0};
	finish.string = lcf::DBString("  -123suffix");
	REQUIRE(decimal_literal.ExecuteCommand(finish));
	CHECK(Main_Data::game_variables->Get(75) == -123);
	Main_Data::game_strings->Asg(73, "+45suffix");
	command.parameters[5] = 76;
	Game_Interpreter decimal_string;
	decimal_string.Push({command}, 1);
	REQUIRE(decimal_string.ExecuteCommand(command));
	finish.parameters = {0, 5, 73};
	REQUIRE(decimal_string.ExecuteCommand(finish));
	CHECK(Main_Data::game_variables->Get(76) == 45);
	auto* source_bank = Main_Data::game_maniac_self_variables->Ensure(1, 0, 1);
	source_bank->numbers.resize(3);
	source_bank->numbers[2] = 1;
	source_bank->strings.resize(2);
	source_bank->strings[1] = lcf::DBString(" -56suffix");
	command.parameters[5] = 77;
	Game_Interpreter decimal_indirect_string;
	decimal_indirect_string.Push({command}, 1);
	REQUIRE(decimal_indirect_string.ExecuteCommand(command));
	finish.parameters = {0, 6, -2};
	REQUIRE(decimal_indirect_string.ExecuteCommand(finish));
	CHECK(Main_Data::game_variables->Get(77) == -56);
	auto* map_event = Game_Map::GetEvent(1);
	REQUIRE(map_event != nullptr);
	auto* page = const_cast<lcf::rpg::EventPage*>(map_event->GetPage(1));
	REQUIRE(page != nullptr);
	page->event_commands.push_back(lcf::rpg::EventCommand{});
	command.parameters = {1, 10005, 1};
	Game_Interpreter this_event_call;
	this_event_call.Push({command}, 1);
	REQUIRE(this_event_call.ExecuteCommand(command));
	REQUIRE(this_event_call.GetState().stack.size() == 2);
	CHECK(this_event_call.GetState().stack.back().easyrpg_self_scope == 2);
	CHECK(this_event_call.GetState().stack.back().easyrpg_self_event_id == 1);
	Main_Data::game_variables->Set(52, 10005);
	Main_Data::game_variables->Set(53, 1);
	command.parameters = {2, 52, 53};
	Game_Interpreter indirect_this_event;
	indirect_this_event.Push({command}, 1);
	REQUIRE(indirect_this_event.ExecuteCommand(command));
	CHECK(indirect_this_event.GetState().stack.size() == 1); // Selector 2 does not expand 10005.
	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Main_Data::game_strings.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac CallEvent literal budgets preserve native signed boundary ordering") {
	lcf::Data::commonevents.clear();
	lcf::Data::commonevents.emplace_back();
	auto& common = lcf::Data::commonevents.back();
	common.ID = 1;
	common.name = lcf::DBString("漢XY");
	lcf::rpg::EventCommand end;
	end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::END);
	common.event_commands.push_back(end);
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
	command.string = lcf::DBString("漢XY");
	for (int32_t budget : {INT32_MIN, INT32_MIN + 1, -1, 0, 1, 2, 3, INT32_MAX}) {
		command.parameters = {0, 1, 0, 0, 0, 0, 2, 4, budget, 4, 4};
		Game_Interpreter interpreter;
		interpreter.Push({command}, 1);
		REQUIRE(interpreter.ExecuteCommand(command));
		const auto* bank = Main_Data::game_maniac_self_variables->Find(1, 0, 1);
		REQUIRE(bank != nullptr);
		const std::string first = budget == INT32_MIN || budget == INT32_MAX ? "漢XY"
			: budget == 2 ? "漢" : budget == 3 ? "漢X" : "";
		const std::string second = first == "漢XY" ? "" : first == "漢X" ? "Y" : first == "漢" ? "XY" : "漢XY";
		CHECK(bank->strings[1] == lcf::DBString(first));
		CHECK(bank->strings[2] == lcf::DBString(second));
		CHECK(Game_ManiacSelfVariables::IsStringPresent(bank, 1) == !first.empty());
		CHECK(Game_ManiacSelfVariables::IsStringPresent(bank, 2) == !second.empty());
	}
	command.parameters = {5, INT32_MIN, 0, 0, 0, 0, 1, 4, 4};
	Game_Interpreter named;
	named.Push({command}, 1);
	REQUIRE(named.ExecuteCommand(command));
	CHECK(command.easyrpg_maniac_common_name_cache == 1);
	command.string = lcf::DBString("Wrong");
	REQUIRE(named.ExecuteCommand(command));
	const auto* bank = Main_Data::game_maniac_self_variables->Find(1, 0, 1);
	REQUIRE(bank != nullptr);
	CHECK(bank->strings[1] == lcf::DBString("Wron")); // Cached name rescans the prefix.
	Player::game_config.patch_maniac.Set(previous_patch);
}

TEST_CASE("Maniac generated calls share name cache and preserve surrogate slots in CP936 saves") {
	lcf::Data::commonevents.clear();
	for (int id = 1; id <= 2; ++id) {
		auto& common = lcf::Data::commonevents.emplace_back();
		common.ID = id;
		common.name = lcf::DBString(id == 1 ? "Dup" : "Other");
		lcf::rpg::EventCommand end;
		end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::END);
		common.event_commands.push_back(end);
	}
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	struct Restore {
		int patch = Player::game_config.patch_maniac.Get();
		int cache = ManiacPatch::GetGeneratedCommonNameCache();
		~Restore() {
			Player::game_config.patch_maniac.Set(patch);
			ManiacPatch::SetGeneratedCommonNameCache(cache);
		}
	} restore;
	Player::game_config.patch_maniac.Set(1);
	ManiacPatch::SetGeneratedCommonNameCache(0);
	const auto generate = [](std::vector<int32_t> values, const char* text) {
		lcf::rpg::EventCommand command;
		command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_CallCommand);
		std::vector<int32_t> fields{0x30, 12330, static_cast<int32_t>(5 + values.size()), static_cast<int32_t>(values.size()), 0};
		fields.insert(fields.end(), values.begin(), values.end());
		fields.resize(fields.size() + (values.size() + 3) / 4);
		command.parameters = lcf::DBArray<int32_t>(fields.begin(), fields.end());
		command.string = lcf::DBString(text);
		return command;
	};
	const std::string high = "\xed\xa0\xbd", low = "\xed\xb8\x80";
	auto first = generate({5,3,0,0,0,0,6,4,1,4,1,4,2,4,2,4,2,4,1}, "DupｱＡ😀Z");
	Game_Interpreter caller;
	caller.Push({first}, 1);
	REQUIRE(caller.ExecuteCommand(first));
	CHECK(ManiacPatch::GetGeneratedCommonNameCache() == 1);
	const auto* bank = Main_Data::game_maniac_self_variables->Find(1, 0, 1);
	REQUIRE(bank != nullptr);
	CHECK(bank->strings[1] == lcf::DBString("ｱ"));
	CHECK_FALSE(Game_ManiacSelfVariables::IsStringPresent(bank, 2));
	CHECK(bank->strings[3] == lcf::DBString("Ａ"));
	CHECK(bank->strings[4] == lcf::DBString(high));
	CHECK(bank->strings[5] == lcf::DBString(low));
	CHECK(Game_ManiacSelfVariables::IsStringPresent(bank, 4));
	CHECK(Game_ManiacSelfVariables::IsStringPresent(bank, 5));
	auto second = generate({5,5,0,0,0,0,1,4,1}, "OtherQ");
	Game_Interpreter another;
	another.Push({second}, 1);
	REQUIRE(another.ExecuteCommand(second));
	CHECK(another.GetState().stack.back().easyrpg_self_event_id == 1);
	CHECK(bank->strings[1] == lcf::DBString("Q"));
	CHECK(Main_Data::game_maniac_self_variables->Find(1, 0, 2) == nullptr);
	lcf::rpg::Save save;
	save.easyrpg_data.maniac_generated_common_name_cache = ManiacPatch::GetGeneratedCommonNameCache();
	save.easyrpg_data.maniac_self_variable_banks = Main_Data::game_maniac_self_variables->GetSerializableSaveData();
	const std::vector<lcf::DBString> globals{lcf::DBString(high), lcf::DBString("ｱ😀"), lcf::DBString(std::string("x\0y", 3))};
	save.easyrpg_data.maniac_global_string_raw_bytes = ManiacStringStorage::Encode(globals);
	std::stringstream file;
	REQUIRE(lcf::LSD_Reader::Save(file, save, lcf::EngineVersion::e2k3, "CP936"));
	file.seekg(0);
	auto loaded = lcf::LSD_Reader::Load(file, "CP936");
	REQUIRE(loaded != nullptr);
	CHECK(loaded->easyrpg_data.maniac_generated_common_name_cache == 1);
	Game_ManiacSelfVariables restored;
	REQUIRE(restored.SetSaveData(loaded->easyrpg_data.maniac_self_variable_banks));
	CHECK(restored.Find(1, 0, 1)->strings[4] == lcf::DBString(high));
	CHECK(restored.Find(1, 0, 1)->strings[5] == lcf::DBString(low));
	std::vector<lcf::DBString> decoded;
	REQUIRE(ManiacStringStorage::Decode(loaded->easyrpg_data.maniac_global_string_raw_bytes, decoded));
	CHECK(decoded == globals);
	auto malformed = loaded->easyrpg_data.maniac_self_variable_banks;
	malformed.front().string_raw_bytes.pop_back();
	CHECK_FALSE(restored.SetSaveData(malformed));
	CHECK(restored.Find(1, 0, 1)->strings[4] == lcf::DBString(high));
}

TEST_CASE("Maniac referenced names retain isolated UTF16 units before exact matching") {
	lcf::Data::commonevents.clear();
	const std::string name = std::string("\xed\xa0\xbd") + "Dup";
	for (int id : {1, 2}) {
		auto& common = lcf::Data::commonevents.emplace_back();
		common.ID = id;
		common.name = lcf::DBString(id == 1 ? "Dup" : name);
		lcf::rpg::EventCommand end;
		end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::END);
		common.event_commands.push_back(end);
	}
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_strings = std::make_unique<Game_Strings>();
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	struct RestorePatch {
		int value = Player::game_config.patch_maniac.Get();
		~RestorePatch() { Player::game_config.patch_maniac.Set(value); }
	} restore;
	Player::game_config.patch_maniac.Set(1);
	Main_Data::game_strings->SetManiacRawSlot(10, name);
	Main_Data::game_variables->Set(11, 10);
	for (int selector : {6, 7}) {
		lcf::rpg::EventCommand call;
		call.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
		call.parameters = {selector, selector == 6 ? 10 : 11};
		Game_Interpreter caller;
		caller.Push({call}, 1);
		REQUIRE(caller.ExecuteCommand(call));
		CHECK(caller.GetState().stack.back().easyrpg_self_event_id == 2);
	}
}

TEST_CASE("Maniac surrogate conversion remains opt in") {
	const std::u16string high{0xd83d}, low{0xde00};
	CHECK(Utils::EncodeUTF(high).empty());
	CHECK(Utils::EncodeUTF(low).empty());
	CHECK(Utils::DecodeUTF16("\xed\xa0\xbd").empty());
	CHECK(Utils::DecodeUTF16("\xed\xb8\x80").empty());
	CHECK(Utils::EncodeUTF(high, true) == "\xed\xa0\xbd");
	CHECK(Utils::EncodeUTF(low, true) == "\xed\xb8\x80");
	CHECK(Utils::DecodeUTF16("\xed\xa0\xbd", true) == high);
	CHECK(Utils::DecodeUTF16("\xed\xb8\x80", true) == low);
	CHECK(Utils::EncodeUTF(std::u16string{0xd83d,0xde00}, true) == "😀");

}

TEST_CASE("Maniac native bank encoding trims zero tails while private state keeps allocation") {
	Game_ManiacSelfVariables state;
	auto* bank = state.Ensure(1, 0, 1);
	REQUIRE(bank != nullptr);
	bank->numbers = {11, 77, 0};
	bank->switches = {3, 3, 0};
	bank->strings.resize(3);
	auto* zero = state.Ensure(1, 0, 2);
	REQUIRE(zero != nullptr);
	zero->numbers.resize(17);
	zero->switches.resize(3);
	zero->strings.resize(3);
	std::vector<uint8_t> native;
	REQUIRE(ManiacOriginalSave::EncodeObservedBanks(state.GetSaveData(), native));
	Game_ManiacSelfVariables::Banks decoded;
	REQUIRE(ManiacOriginalSave::Decode(native, decoded));
	REQUIRE(decoded.size() == 2);
	CHECK(decoded[0].numbers == std::vector<int32_t>{0, 77});
	CHECK(decoded[0].switches == std::vector<uint8_t>{0, 1});
	CHECK(decoded[0].strings.empty());
	CHECK(decoded[1].numbers.empty());
	CHECK(decoded[1].switches.empty());
	lcf::rpg::Save save;
	save.easyrpg_data.maniac_self_variable_banks = state.GetSerializableSaveData();
	save.maniac_original_bank_bytes = native;
	std::stringstream file;
	REQUIRE(lcf::LSD_Reader::Save(file, save, lcf::EngineVersion::e2k3, "CP936"));
	file.seekg(0);
	auto loaded = lcf::LSD_Reader::Load(file, "CP936");
	REQUIRE(loaded != nullptr);
	Game_ManiacSelfVariables restored;
	REQUIRE(restored.SetSaveData(loaded->easyrpg_data.maniac_self_variable_banks));
	CHECK(restored.Find(1, 0, 1)->numbers == std::vector<int32_t>{11, 77, 0});
	CHECK(restored.Find(1, 0, 1)->switches == std::vector<uint8_t>{3, 3, 0});
	CHECK(restored.Find(1, 0, 2)->numbers.size() == 17);
	CHECK(restored.Find(1, 0, 2)->switches.size() == 3);
	CHECK(restored.Find(1, 0, 2)->strings.size() == 3);
}

TEST_CASE("Maniac native global string payload follows zero markers and project encoding") {
	lcf::rpg::Save save;
	const std::string high = "\xed\xa0\xbd", low = "\xed\xb8\x80";
	const std::string mixed = std::string("😀") + high + low;
	save.system.maniac_strings = {lcf::DBString("ｱＡ😀"), lcf::DBString{}, lcf::DBString{},
		lcf::DBString(high), lcf::DBString(low), lcf::DBString(std::string("x\0y", 3)), lcf::DBString(mixed)};
	save.easyrpg_data.maniac_global_string_empty_slots = {2};
	save.easyrpg_data.maniac_global_string_vector_size = 9;
	save.actors.emplace_back();
	save.actors.back().ID = 1;
	save.actors.back().name = lcf::DBString("名称");
	REQUIRE(ManiacOriginalSave::EncodeObservedGlobals(save));
	const auto payload = save.system.maniac_string_payload;
	std::stringstream file;
	REQUIRE(lcf::LSD_Reader::Save(file, save, lcf::EngineVersion::e2k3, "CP936"));
	file.seekg(0);
	auto loaded = lcf::LSD_Reader::Load(file, "CP936");
	REQUIRE(loaded != nullptr);
	CHECK(loaded->system.maniac_strings_utf16);
	CHECK(loaded->system.maniac_string_payload == payload);
	CHECK(loaded->actors.front().name == lcf::DBString("名称"));
	REQUIRE(ManiacOriginalSave::ImportObservedGlobals(*loaded));
	REQUIRE(loaded->system.maniac_strings.size() == 6);
	CHECK(loaded->easyrpg_data.maniac_global_string_empty_slots.empty());
	CHECK(loaded->easyrpg_data.maniac_global_string_vector_size == 9);
	CHECK(loaded->system.maniac_strings[0] == lcf::DBString("ｱＡ😀"));
	CHECK(loaded->system.maniac_strings[1].empty());
	CHECK(loaded->system.maniac_strings[2] == lcf::DBString(high));
	CHECK(loaded->system.maniac_strings[3] == lcf::DBString(low));
	CHECK(loaded->system.maniac_strings[4] == lcf::DBString(std::string("x\0y", 3)));
	CHECK(loaded->system.maniac_strings[5] == lcf::DBString("😀😀"));
	REQUIRE(ManiacOriginalSave::EncodeObservedGlobals(*loaded));
	auto canonical_payload = payload;
	canonical_payload.erase(canonical_payload.begin() + 9); // Zero marker after four UTF16 units disappears.
	CHECK(loaded->system.maniac_string_payload == canonical_payload);
	const auto strings = loaded->system.maniac_strings;
	loaded->system.maniac_string_payload.pop_back();
	CHECK_FALSE(ManiacOriginalSave::ImportObservedGlobals(*loaded));
	CHECK(loaded->system.maniac_strings == strings);
	save.system.maniac_strings[0] = lcf::DBString("\x80");
	CHECK_FALSE(ManiacOriginalSave::EncodeObservedGlobals(save));
	CHECK(save.system.maniac_string_payload == payload);

}

TEST_CASE("Maniac global scalar save keeps native switch bytes and complete Player zero tails") {
	lcf::rpg::Save save;
	save.system.variables = {0, INT32_MIN, INT32_MAX, 0, 0};
	save.system.switches = {true, false, true, false, false};
	save.easyrpg_data.maniac_global_switch_raw = {0, 3, 1, 7, 3, 255};
	REQUIRE(ManiacOriginalSave::EncodeObservedGlobalScalars(save));
	CHECK(save.system.variables == std::vector<int32_t>{0, INT32_MIN, INT32_MAX});
	CHECK(save.system.maniac_switch_payload == std::vector<uint8_t>{7, 0, 255});
	CHECK(save.easyrpg_data.maniac_global_variables.size() == 5);
	CHECK(save.easyrpg_data.maniac_global_switches.size() == 5);
	std::stringstream file;
	REQUIRE(lcf::LSD_Reader::Save(file, save, lcf::EngineVersion::e2k3, "CP936"));
	file.seekg(0);
	auto loaded = lcf::LSD_Reader::Load(file, "CP936");
	REQUIRE(loaded != nullptr);
	CHECK(loaded->system.maniac_switch_payload == std::vector<uint8_t>{7, 0, 255});
	CHECK_FALSE(loaded->system.maniac_global_scalars_active);
	REQUIRE(ManiacOriginalSave::ImportObservedGlobalScalars(*loaded));
	CHECK(loaded->system.variables == std::vector<int32_t>{0, INT32_MIN, INT32_MAX, 0, 0});
	Game_Switches switches;
	switches.SetData(loaded->system.switches);
	switches.SetManiacRawSaveData(loaded->easyrpg_data.maniac_global_switch_raw);
	CHECK(switches.GetSize() == 5);
	CHECK(switches.GetManiacRawSlot(0) == 3);
	CHECK(switches.GetManiacRawSlot(1) == 7);
	CHECK(switches.GetManiacRawSlot(3) == 255);
	CHECK(switches.GetManiacRawSlot(5) == 0);

	// Native saves have the same canonical arrays and no Player private data.
	save.easyrpg_data = {};
	file = std::stringstream{};
	REQUIRE(lcf::LSD_Reader::Save(file, save, lcf::EngineVersion::e2k3, "CP936"));
	file.seekg(0);
	loaded = lcf::LSD_Reader::Load(file, "CP936");
	REQUIRE(loaded != nullptr);
	REQUIRE(ManiacOriginalSave::ImportObservedGlobalScalars(*loaded));
	CHECK(loaded->system.variables.size() == 3);
	CHECK(loaded->easyrpg_data.maniac_global_switch_raw == std::vector<int32_t>{1, 7, 3, 255});

	// A normal save still writes boolean bytes even after a raw payload was read.
	file = std::stringstream{};
	REQUIRE(lcf::LSD_Reader::Save(file, *loaded, lcf::EngineVersion::e2k3, "CP936"));
	file.seekg(0);
	loaded = lcf::LSD_Reader::Load(file, "CP936");
	REQUIRE(loaded != nullptr);
	CHECK(loaded->system.maniac_switch_payload == std::vector<uint8_t>{1, 0, 1});
}

TEST_CASE("Maniac global scalar save restores zero tails and rejects contradictory private state") {
	lcf::rpg::Save save;
	save.system.variables.assign(17, 0);
	save.system.switches.assign(17, false);
	REQUIRE(ManiacOriginalSave::EncodeObservedGlobalScalars(save));
	CHECK(save.system.variables.empty());
	CHECK(save.system.switches.empty());
	std::stringstream file;
	REQUIRE(lcf::LSD_Reader::Save(file, save, lcf::EngineVersion::e2k3, "CP936"));
	file.seekg(0);
	auto loaded = lcf::LSD_Reader::Load(file, "CP936");
	REQUIRE(loaded != nullptr);
	REQUIRE(ManiacOriginalSave::ImportObservedGlobalScalars(*loaded));
	CHECK(loaded->system.variables.size() == 17);
	CHECK(loaded->system.switches.size() == 17);
	CHECK(loaded->system.variables.back() == 0);
	CHECK_FALSE(loaded->system.switches.back());
	loaded->system.variables[0] = 12;
	CHECK_FALSE(ManiacOriginalSave::ImportObservedGlobalScalars(*loaded));
	CHECK(loaded->system.variables[0] == 12);

	save = {};
	save.system.variables = {7, 0};
	save.system.switches = {true};
	save.easyrpg_data.maniac_global_switch_raw = {2, 3};
	CHECK_FALSE(ManiacOriginalSave::EncodeObservedGlobalScalars(save));
	CHECK(save.system.variables == std::vector<int32_t>{7, 0});
	CHECK(save.easyrpg_data.maniac_global_variables.empty());
}

TEST_CASE("Maniac legacy byte strings honor the final save codepage") {
	lcf::rpg::Save save;
	save.easyrpg_data.codepage = 65001;
	save.system.maniac_strings = {lcf::DBString("中文"), lcf::DBString{}, lcf::DBString("😀")};
	std::stringstream file;
	REQUIRE(lcf::LSD_Reader::Save(file, save, lcf::EngineVersion::e2k3, "CP936"));
	file.seekg(0);
	auto loaded = lcf::LSD_Reader::Load(file, "CP936");
	REQUIRE(loaded != nullptr);
	CHECK_FALSE(loaded->system.maniac_strings_utf16);
	CHECK(loaded->system.maniac_strings == save.system.maniac_strings);
}

TEST_CASE("Maniac 250921 calls create fresh fields and retain empty frames") {
	lcf::Data::commonevents.clear();
	lcf::Data::commonevents.emplace_back();
	lcf::Data::commonevents.back().ID = 1;
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	const auto previous_version = Player::game_config.maniac_version.Get();
	Player::game_config.patch_maniac.Set(1);
	Player::game_config.maniac_version.Set(250921);
	lcf::rpg::EventCommand call;
	call.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
	call.parameters = {0, 1};
	Game_Interpreter_Map caller;
	caller.Push({call}, 1);
	auto seeded = caller.GetState();
	seeded.stack.back().easyrpg_maniac_return_type = 6;
	seeded.stack.back().easyrpg_maniac_return_address = -55;
	seeded.stack.back().subcommand_path = {1, 2, 3};
	caller.SetState(seeded);
	REQUIRE(caller.ExecuteCommand(call));
	REQUIRE(caller.GetState().stack.size() == 2);
	const auto& common = caller.GetState().stack.back();
	CHECK(common.ID == 2);
	CHECK(common.commands.empty());
	CHECK(common.current_command == 0);
	CHECK(common.event_id == 0);
	CHECK(common.maniac_event_info == 0x25);
	CHECK(common.maniac_event_id == 1);
	CHECK(common.maniac_event_page_id == 0);
	CHECK_FALSE(common.triggered_by_decision_key);
	CHECK(common.subcommand_path.empty());
	CHECK(common.easyrpg_maniac_return_type == 0);
	CHECK(common.easyrpg_maniac_return_address == 0);
	CHECK(Main_Data::game_maniac_self_variables->Find(1, 0, 1) == nullptr);
	call.parameters = {0, 1, 0, 0, 0, 0, 1000001};
	Game_Interpreter large;
	large.Push({call}, 1);
	REQUIRE(large.ExecuteCommand(call));
	REQUIRE(Main_Data::game_maniac_self_variables->Find(1, 0, 1) != nullptr);
	CHECK(Main_Data::game_maniac_self_variables->Find(1, 0, 1)->numbers.size() == 1000002);
	call.parameters = {1, 1, 1};
	Game_Interpreter map_caller;
	map_caller.Push({call}, 1);
	REQUIRE(map_caller.ExecuteCommand(call));
	REQUIRE(map_caller.GetState().stack.size() == 2);
	const auto& map = map_caller.GetState().stack.back();
	CHECK(map.maniac_event_info == 0x15);
	CHECK(map.maniac_event_id == 1);
	CHECK(map.maniac_event_page_id == 1);
	CHECK(map.easyrpg_maniac_return_type == 0);
	CHECK(map.easyrpg_maniac_return_address == 0);
	Player::game_config.patch_maniac.Set(previous_patch);
	Player::game_config.maniac_version.Set(previous_version);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
	lcf::Data::commonevents.clear();
}

TEST_CASE("Maniac 250921 serialized command Text terminates at NUL but generated Text keeps stored bytes") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		lcf::rpg::EventCommand literal;
		literal.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		literal.parameters = {0, 500, 500, 0, 0, 0, 0, 0};
		literal.string = lcf::DBString(std::string("12\0ignored", 10));
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Game_Interpreter native;
		native.Push({literal}, 0);
		CHECK(native.GetState().stack.front().commands.front().string == "12");
		CHECK(literal.string.size() == 10);
		lcf::rpg::SaveEventExecState save;
		save.stack.emplace_back().commands = {literal};
		Game_Interpreter_Map restored(false);
		restored.SetState(save);
		CHECK(restored.GetState().stack.front().commands.front().string == "12");
		CHECK(save.stack.front().commands.front().string.size() == 10);
		Player::game_config.patch_maniac.Set(0);
		Game_Interpreter ordinary;
		ordinary.Push({literal}, 0);
		CHECK(ordinary.GetState().stack.front().commands.front().string.size() == 10);
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(241028);
		Game_Interpreter other_version;
		other_version.Push({literal}, 0);
		CHECK(other_version.GetState().stack.front().commands.front().string.size() == 10);
		Player::game_config.maniac_version.Set(250921);
		Main_Data::game_strings->SetManiacRawSlot(500, std::string("A\0B", 3), true);
		lcf::rpg::EventCommand generated;
		generated.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_CallCommand);
		generated.parameters = {0x1030, 3020, 13, 8, 500, 0, 601, 0, 0, 0, 0, 0, 0, 0, 0};
		CHECK(native.ExecuteCommand(generated));
		CHECK(Main_Data::game_strings->Get(601) == std::string("A\0B", 3));
		Main_Data::game_strings.reset();
		Main_Data::game_maniac_self_variables.reset();
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 zero-length string producers remove existing global slots") {
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		const auto previous_patch = Player::game_config.patch_maniac.Get();
		const auto previous_version = Player::game_config.maniac_version.Get();
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		Main_Data::game_strings = std::make_unique<Game_Strings>();
		Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		Main_Data::game_strings->SetManiacRawSlot(101, "abc", true);
		Main_Data::game_strings->SetManiacRawSlot(102, "^abc$", true);
		Main_Data::game_strings->SetManiacRawSlot(700, "Tail", true);
		Game_Interpreter interpreter;
		lcf::rpg::EventCommand end;
		end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::END);
		interpreter.Push({end}, 0);
		const std::vector<std::vector<int32_t>> parameters = {
			{0, 500, 500, 5<<8, 0, 0, 0, 0}, {0, 500, 500, 11<<8, 0, 0, 0, 0},
			{0, 500, 500, 255<<8, 0, 0, 0, 0}, {0x10, 500, 500, 9<<8, 101, 0, 0, 0},
			{0x10, 500, 500, 9<<8, 101, 3, 1, 0}, {0x10, 500, 500, 13<<8, 101, 0, 3, 0},
			{0, 500, 500, 6<<8, 0, 0, 0, 0}, {0, 500, 500, 10<<8, 0, 610, 0, 0},
			{0, 500, 500, 10<<8, 0, 610, 1, 0}, {0x1110, 500, 500, 14<<8, 101, 102, 103, 0}
		};
		lcf::rpg::EventCommand command;
		command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
		for (const auto& args : parameters) {
			Main_Data::game_strings->SetManiacRawSlot(500, "sentinel", true);
			command.parameters = lcf::DBArray<int32_t>(args.begin(), args.end());
			CHECK(interpreter.ExecuteCommand(command));
			CHECK(Main_Data::game_strings->GetData().count(500) == 0);
			CHECK(Main_Data::game_strings->Get(700) == "Tail");
		}
		Main_Data::game_strings->SetManiacRawSlot(111, {}, true);
		command.parameters = {0x10, 500, 500, 0, 111, 0, 0, 0};
		CHECK(interpreter.ExecuteCommand(command));
		CHECK(Main_Data::game_strings->GetData().count(500) == 1);
		Main_Data::game_strings.reset();
		Main_Data::game_maniac_self_variables.reset();
		Player::game_config.maniac_version.Set(previous_version);
		Player::game_config.patch_maniac.Set(previous_patch);
	}
	Graphics::Quit();
}

TEST_CASE("Maniac return numeric parsing preserves surrogate units and empty literals remove string slots") {
	lcf::Data::commonevents.clear();
	lcf::Data::commonevents.emplace_back();
	lcf::Data::commonevents.back().ID = 1;
	lcf::rpg::EventCommand end;
	end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::END);
	lcf::Data::commonevents.back().event_commands.push_back(end);
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_strings = std::make_unique<Game_Strings>();
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	const auto previous_version = Player::game_config.maniac_version.Get();
	Player::game_config.patch_maniac.Set(1);
	Player::game_config.maniac_version.Set(250921);
	lcf::rpg::EventCommand call;
	call.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
	call.parameters = {0, 1, 0, 0x20, 0, 601};
	for (const int source_type : {4, 5, 6}) {
		Game_Interpreter interpreter;
		interpreter.Push({call}, 1);
		REQUIRE(interpreter.ExecuteCommand(call));
		auto* bank = Main_Data::game_maniac_self_variables->Ensure(1, 0, 1);
		bank->numbers = {0, 1};
		bank->strings = {lcf::DBString{}, lcf::DBString("\xed\xb8\x80" "7")};
		Main_Data::game_variables->Set(601, -901);
		lcf::rpg::EventCommand finish;
		finish.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::EndEventProcessing);
		finish.parameters = {0, source_type, -1};
		finish.string = lcf::DBString("\xed\xb8\x80" "7");
		REQUIRE(interpreter.ExecuteCommand(finish));
		CHECK(Main_Data::game_variables->Get(601) == 0);
	}
	Main_Data::game_strings->SetManiacRawSlot(500, "old");
	Main_Data::game_strings->SetManiacRawSlot(601, "later");
	lcf::rpg::EventCommand assign;
	assign.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::Maniac_ControlStrings);
	assign.parameters = {0, 500, 500, 0, 0, 0, 0, 0};
	Game_Interpreter literal;
	literal.Push({assign}, 1);
	REQUIRE(literal.ExecuteCommand());
	CHECK(Main_Data::game_strings->GetData().count(500) == 0);
	CHECK(Main_Data::game_strings->Get(601) == "later");
	Main_Data::game_strings->SetManiacRawSlot(500, "", true);
	CHECK(Main_Data::game_strings->GetData().count(500) == 1);
	Main_Data::game_strings->SetManiacRawSlot(501, "old");
	assign.parameters[0] = 1;
	assign.parameters[2] = 501;
	Game_Interpreter range;
	range.Push({assign}, 1);
	REQUIRE(range.ExecuteCommand());
	CHECK(Main_Data::game_strings->GetData().count(500) == 0);
	CHECK(Main_Data::game_strings->GetData().count(501) == 0);
	Player::game_config.patch_maniac.Set(previous_patch);
	Player::game_config.maniac_version.Set(previous_version);
	Main_Data::game_maniac_self_variables.reset();
	Main_Data::game_strings.reset();
	Graphics::Quit();
	lcf::Data::commonevents.clear();
}

TEST_CASE("Maniac variable division and remainder use signed64 arithmetic and live bounds") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	const auto previous_version = Player::game_config.maniac_version.Get();
	Player::game_config.patch_maniac.Set(1);
	Player::game_config.maniac_version.Set(250921);
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ControlVars);
	Game_Interpreter interpreter;
	interpreter.Push({command}, 1);
	struct Sample { int32_t dividend, divisor, quotient, remainder; };
	const Sample samples[] = {
		{11, 3, 3, 2}, {-11, 3, -3, -2}, {11, -3, -3, 2}, {-11, -3, 3, -2},
		{11, 0, 11, 0}, {INT32_MIN, -1, 9999999, 0},
		{INT32_MIN, 1, -9999999, 0}, {INT32_MAX, 1, 9999999, 0}
	};
	for (const auto& sample : samples) {
		for (const int target : {51, -1}) {
			for (const int operation : {4, 5}) {
				command.parameters = {0, target, 0, 0, 0, sample.dividend};
				REQUIRE(interpreter.ExecuteCommand(command));
				command.parameters = {0, target, 0, operation, 0, sample.divisor};
				REQUIRE(interpreter.ExecuteCommand(command));
				CHECK(interpreter.ReadVariable(target) == (operation == 4 ? sample.quotient : sample.remainder));
			}
		}
	}
	for (const auto pair : {std::pair<int, int>{-1, 16}, {-2, 6}, {-3, 9}}) {
		command.parameters = {0, pair.first, 0, 0, 0, pair.second};
		REQUIRE(interpreter.ExecuteCommand(command));
	}
	command.parameters = {1, -1, -3, 4, 1, -1};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-1) == 1);
	CHECK(interpreter.ReadVariable(-2) == 6);
	CHECK(interpreter.ReadVariable(-3) == 9);
	auto previous_variables = std::move(Main_Data::game_variables);
	Main_Data::game_variables = std::make_unique<Game_Variables>(-7, 9);
	command.parameters = {0, 51, 0, 0, 0, 11};
	REQUIRE(interpreter.ExecuteCommand(command));
	command.parameters = {0, 51, 0, 4, 0, 0};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(51) == 9);
	command.parameters = {0, -1, 0, 0, 0, -11};
	REQUIRE(interpreter.ExecuteCommand(command));
	command.parameters = {0, -1, 0, 5, 0, 20};
	REQUIRE(interpreter.ExecuteCommand(command));
	CHECK(interpreter.ReadVariable(-1) == -7);
	Main_Data::game_variables = std::move(previous_variables);
	Player::game_config.patch_maniac.Set(previous_patch);
	Player::game_config.maniac_version.Set(previous_version);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("Maniac 250921 calls permit depth 8192 to append one more frame") {
	lcf::Data::commonevents.clear();
	lcf::Data::commonevents.emplace_back();
	lcf::Data::commonevents.back().ID = 1;
	Graphics::Init();
	MockGame game(MockMap::eNone);
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	const auto previous_version = Player::game_config.maniac_version.Get();
	Player::game_config.patch_maniac.Set(1);
	Player::game_config.maniac_version.Set(250921);
	lcf::rpg::SaveEventExecState seeded;
	seeded.stack.resize(8192);
	for (size_t i = 0; i < seeded.stack.size(); ++i) seeded.stack[i].ID = static_cast<int>(i + 1);
	Game_Interpreter_Map caller;
	caller.SetState(std::move(seeded));
	lcf::rpg::EventCommand call;
	call.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
	call.parameters = {0, 1};
	REQUIRE(caller.ExecuteCommand(call));
	CHECK(caller.GetState().stack.size() == 8193);
	CHECK(caller.GetState().stack.back().ID == 8193);
	Player::game_config.patch_maniac.Set(0);
	Game_Interpreter legacy;
	legacy.Push({}, 1);
	CHECK(legacy.GetState().stack.empty());
	Player::game_config.patch_maniac.Set(previous_patch);
	Player::game_config.maniac_version.Set(previous_version);
	Graphics::Quit();
	lcf::Data::commonevents.clear();
}

TEST_CASE("Maniac called map event frame retains trigger context") {
	Graphics::Init();
	MockGame game(MockMap::eNone);
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	auto* event = MockGame::GetEvent(1);
	REQUIRE(event != nullptr);
	auto* page = const_cast<lcf::rpg::EventPage*>(event->GetPage(1));
	REQUIRE(page != nullptr);
	lcf::rpg::EventCommand end;
	end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::END);
	page->event_commands.push_back(end);
	lcf::rpg::EventCommand call;
	call.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
	call.parameters = {1, 1, 1};
	Game_Interpreter interpreter;
	interpreter.Push({call}, 1);
	REQUIRE(interpreter.ExecuteCommand(call));
	REQUIRE(interpreter.GetState().stack.size() == 2);
	const auto& frame = interpreter.GetState().stack.back();
	CHECK(frame.maniac_event_info == (0x10 | static_cast<int>(page->trigger)));
	CHECK(frame.maniac_event_id == 1);
	CHECK(frame.maniac_event_page_id == 1);
	Player::game_config.patch_maniac.Set(previous_patch);
	Graphics::Quit();
}

TEST_CASE("Maniac explicit return refreshes map pages during delivery") {
	lcf::Data::commonevents.clear();
	lcf::Data::commonevents.emplace_back();
	auto& common = lcf::Data::commonevents.back();
	common.ID = 1;
	lcf::rpg::EventCommand end;
	end.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::END);
	common.event_commands.push_back(end);
	Graphics::Init();
	MockGame game(MockMap::eNone);
	Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	Player::game_config.patch_maniac.Set(1);
	auto* event = MockGame::GetEvent(1);
	REQUIRE(event != nullptr);
	auto* page = const_cast<lcf::rpg::EventPage*>(event->GetPage(1));
	REQUIRE(page != nullptr);
	page->condition.flags.variable = true;
	page->condition.variable_id = 90;
	page->condition.variable_value = 1;
	Game_Map::Refresh();
	CHECK(event->GetActivePage() == nullptr);
	lcf::rpg::EventCommand call;
	call.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
	call.parameters = {0, 1, 0, 0x20, 0, 90};
	Game_Interpreter interpreter;
	interpreter.Push({call}, 1);
	REQUIRE(interpreter.ExecuteCommand(call));
	REQUIRE(interpreter.GetState().stack.size() == 2);
	lcf::rpg::EventCommand finish;
	finish.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::EndEventProcessing);
	finish.parameters = {0, 1, 1};
	REQUIRE(interpreter.ExecuteCommand(finish));
	CHECK(Main_Data::game_variables->Get(90) == 1);
	CHECK(event->GetActivePage() == page);
	page->condition.variable_value = 2;
	Game_Map::Refresh();
	CHECK(event->GetActivePage() == nullptr);
	call.parameters[3] = 0x40; // Unknown receiver still enters the refresh branch.
	REQUIRE(interpreter.ExecuteCommand(call));
	Main_Data::game_variables->Set(90, 2);
	REQUIRE(interpreter.ExecuteCommand(finish));
	CHECK(event->GetActivePage() == page);
	Player::game_config.patch_maniac.Set(previous_patch);
	Main_Data::game_maniac_self_variables.reset();
	Graphics::Quit();
}

TEST_CASE("ChangePartyMember ignores actor zero without warning") {
	lcf::Data::actors.clear();
	lcf::Data::actors.resize(1);
	lcf::Data::actors[0].initial_level = 1;
	lcf::Data::actors[0].Setup(true);

	Graphics::Init();
	MockGame game(MockMap::eNone);

	Main_Data::game_party->AddActor(1);
	REQUIRE(Main_Data::game_party->GetBattlerCount() == 1);

	std::vector<CapturedLog> logs;
	const auto old_level = Output::GetLogLevel();
	Output::SetLogLevel(LogLevel::Warning);
	Output::SetLogCallback(CaptureLog, &logs);

	lcf::rpg::EventCommand cmd;
	cmd.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::ChangePartyMembers);
	cmd.parameters = {1, 0, 0};

	Game_Interpreter interpreter;
	CHECK(interpreter.ExecuteCommand(cmd));
	CHECK(Main_Data::game_party->GetBattlerCount() == 1);

	Output::SetLogCallback(nullptr);
	Output::SetLogLevel(old_level);
	Graphics::Quit();

	CHECK(logs.empty());
}

TEST_CASE("Maniac return destinations survive native and private scene saves") {
	Graphics::Init();
	MockGame game(MockMap::ePassBlock20x15);
	auto previous_targets = std::move(Main_Data::game_targets);
	auto previous_windows = std::move(Main_Data::game_windows);
	auto previous_dynrpg = std::move(Main_Data::game_dynrpg);
	auto previous_strings = std::move(Main_Data::game_strings);
	Main_Data::game_targets = std::make_unique<Game_Targets>();
	Main_Data::game_windows = std::make_unique<Game_Windows>();
	Main_Data::game_dynrpg = std::make_unique<Game_DynRpg>();
	Main_Data::game_strings = std::make_unique<Game_Strings>();
	const auto previous_patch = Player::game_config.patch_maniac.Get();
	const auto previous_version = Player::game_config.maniac_version.Get();
	Player::game_config.patch_maniac.Set(1);
	Player::game_config.maniac_version.Set(250921);
	lcf::Data::commonevents.clear();
	lcf::Data::commonevents.emplace_back();
	lcf::Data::commonevents.back().ID = 1;
	for (int destination = 0; destination < 5; ++destination) {
		for (int kind = 0; kind < 3; ++kind) {
			// All routing combinations use small data; one large slot per type/scope catches the old cap.
			const int slot = destination == kind * 2 ? 1000001 : 17;
			CAPTURE(destination);
			CAPTURE(kind);
			Main_Data::game_variables->SetData({});
			Main_Data::game_switches->SetData({});
			Main_Data::game_strings->SetData(Game_Strings::Strings_t{});
			Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
			const int direct = kind == 0 ? 2 : kind == 1 ? 5 : 8;
			const bool indirect = destination == 1 || destination == 2 || destination == 4;
			const int address = destination == 3 ? -slot : destination == 4 ? -2 : indirect ? 110 : slot;
			Main_Data::game_variables->Set(110, destination == 2 ? -slot : slot);
			lcf::rpg::EventCommand call;
			call.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::CallEvent);
			call.parameters = {0, 1, 0, (direct + int(indirect)) << 4, 0, address};
			Game_Interpreter interpreter;
			interpreter.Push({call}, 1);
			REQUIRE(interpreter.ExecuteCommand(call));
			if (destination == 4) {
				auto* caller = Main_Data::game_maniac_self_variables->Ensure(2, 1, 1);
				REQUIRE(caller != nullptr);
				caller->numbers = {0, 0, slot};
			}
			lcf::rpg::EventCommand finish;
			finish.code = static_cast<int32_t>(lcf::rpg::EventCommand::Code::EndEventProcessing);
			finish.parameters = {0, kind == 1 ? 4 : 1, -731};
			finish.string = lcf::DBString("LargeSlot");
			REQUIRE(interpreter.ExecuteCommand(finish));
			std::stringstream file;
			REQUIRE(Scene_Save::Save(file, 1, false));
			file.seekg(0);
			auto loaded = lcf::LSD_Reader::Load(file, "CP936");
			REQUIRE(loaded != nullptr);
			REQUIRE(ManiacOriginalSave::ImportObservedGlobalScalars(*loaded));
			REQUIRE(ManiacOriginalSave::ImportObservedGlobals(*loaded));
			if (destination < 2) {
				if (kind == 0) {
					REQUIRE(loaded->system.variables.size() >= slot);
					CHECK(loaded->system.variables[slot - 1] == -731);
				} else if (kind == 1) {
					REQUIRE(loaded->system.maniac_strings.size() >= slot);
					CHECK(loaded->system.maniac_strings[slot - 1] == lcf::DBString("LargeSlot"));
				} else {
					REQUIRE(loaded->system.switches.size() >= slot);
					CHECK(loaded->system.switches[slot - 1]);
				}
			} else {
				const auto check = [&](const Game_ManiacSelfVariables& state) {
					const auto* bank = state.Find(destination == 2 ? 1 : 2, destination == 2 ? 0 : 1, 1);
					REQUIRE(bank != nullptr);
					if (kind == 0) {
						REQUIRE(bank->numbers.size() > slot);
						CHECK(bank->numbers[slot] == -731);
					} else if (kind == 1) {
						REQUIRE(bank->strings.size() > slot);
						CHECK(bank->strings[slot] == lcf::DBString("LargeSlot"));
						CHECK(Game_ManiacSelfVariables::IsStringPresent(bank, slot));
					} else {
						REQUIRE(bank->switches.size() > slot);
						CHECK(bank->switches[slot] == 1);
					}
				};
				Game_ManiacSelfVariables private_state;
				REQUIRE(private_state.SetSaveData(loaded->easyrpg_data.maniac_self_variable_banks));
				check(private_state);
				Game_ManiacSelfVariables native_state;
				REQUIRE(ManiacOriginalSave::Decode(loaded->maniac_original_bank_bytes, native_state.MutableBanks()));
				check(native_state);
			}
		}
	}
	Player::game_config.patch_maniac.Set(previous_patch);
	Player::game_config.maniac_version.Set(previous_version);
	Main_Data::game_maniac_self_variables.reset();
	Main_Data::game_targets = std::move(previous_targets);
	Main_Data::game_windows = std::move(previous_windows);
	Main_Data::game_dynrpg = std::move(previous_dynrpg);
	Main_Data::game_strings = std::move(previous_strings);
	Graphics::Quit();
}

TEST_CASE("Maniac large string private identity and long native NULL runs restore") {
	constexpr size_t slot = 1000002;
	Game_ManiacSelfVariables state;
	auto* bank = state.Ensure(1, 0, 1);
	REQUIRE(bank != nullptr);
	bank->strings.resize(slot + 1);
	bank->strings[slot] = lcf::DBString("Tail");
	Game_ManiacSelfVariables::SetStringPresence(bank, slot - 1, true);
	const auto raw = ManiacStringStorage::Encode(bank->strings);
	std::vector<lcf::DBString> strings;
	REQUIRE(ManiacStringStorage::Decode(raw, strings));
	CHECK(strings.size() == slot + 1);
	CHECK(strings.back() == lcf::DBString("Tail"));
	Game_ManiacSelfVariables restored;
	REQUIRE(restored.SetSaveData(state.GetSerializableSaveData()));
	CHECK(Game_ManiacSelfVariables::IsStringPresent(restored.Find(1, 0, 1), slot - 1));
	// Allocated-empty is Player-only state; use a NULL run for native import.
	bank->empty_string_slots.clear();
	bank->switches.resize(slot + 1);
	bank->switches.back() = 1;
	std::vector<uint8_t> native;
	REQUIRE(ManiacOriginalSave::EncodeObservedBanks(state.GetSaveData(), native));
	Game_ManiacSelfVariables decoded;
	REQUIRE(ManiacOriginalSave::Decode(native, decoded.MutableBanks()));
	REQUIRE(decoded.Find(1, 0, 1) != nullptr);
	CHECK(decoded.Find(1, 0, 1)->strings.size() == slot + 1);
	CHECK(decoded.Find(1, 0, 1)->strings.back() == lcf::DBString("Tail"));
	CHECK(decoded.Find(1, 0, 1)->switches.size() == slot + 1);
	CHECK(decoded.Find(1, 0, 1)->switches.back() == 1);
	Game_Strings globals;
	globals.SetManiacVectorSize(slot);
	globals.SetManiacEmptySlotSaveData({static_cast<int32_t>(slot)});
	CHECK(globals.GetManiacVectorSize() == slot);
	CHECK(globals.GetData().count(slot) == 1);
}

TEST_SUITE_END();
