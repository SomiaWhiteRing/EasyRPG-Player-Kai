#include "doctest.h"
#include "game_commonevent.h"
#include "game_interpreter.h"
#include "graphics.h"
#include "mock_game.h"
#include "output.h"
#include "player.h"
#include <lcf/ldb/reader.h>
#include <lcf/lmu/reader.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
using Code = lcf::rpg::EventCommand::Code;

lcf::rpg::EventCommand Command(Code code, std::initializer_list<int32_t> args = {}) {
	lcf::rpg::EventCommand command;
	command.code = static_cast<int32_t>(code);
	command.parameters = args;
	return command;
}
}

TEST_CASE("Maniac 1005 boundary fixture serialization") {
	const char* directory = std::getenv("MANIAC_COMMON1005_FIXTURE_DIR");
	const char* selected = std::getenv("MANIAC_COMMON1005_CASE");
	if (!directory || !selected) return;
	const std::filesystem::path path(directory);
	const std::string variant(selected);
	auto database = lcf::LDB_Reader::Load((path / "RPG_RT.ldb").string());
	auto map = lcf::LMU_Reader::Load((path / "Map0001.lmu").string());
	REQUIRE(database != nullptr);
	REQUIRE(map != nullptr);
	database->commonevents.clear();
	auto& common = database->commonevents.emplace_back();
	common.ID = 1;
	common.name = lcf::DBString("1005 boundary");
	auto call = Command(Code::CallCommonEvent, {1});
	if (variant == "missing") call.parameters = {};
	else if (variant == "zero") call.parameters = {0};
	else if (variant == "negative") call.parameters = {-1};
	else if (variant == "out-of-range") call.parameters = {2};
	else if (variant == "int-max") call.parameters = {2147483647};
	else REQUIRE((variant == "empty" || variant == "depth-pass" || variant == "depth-error"));
	if (variant == "depth-pass" || variant == "depth-error") {
		auto branch = Command(Code::ConditionalBranch, {1, 1, 0, variant == "depth-pass" ? 8192 : 8193, 4, 0});
		auto recursive = call;
		recursive.indent = 1;
		common.event_commands = {Command(Code::ControlVars, {0, 1, 0, 1, 0, 1, 0}), branch,
			recursive, Command(Code::EndBranch), Command(Code::END)};
	}
	map->events.front().pages.front().event_commands = {
		Command(Code::ControlVars, {0, 1, 0, 0, 0, 0, 0}), call,
		Command(Code::ControlVars, {0, 2, 0, 0, 0, 99, 0}),
		Command(Code::Maniac_Save, {0, 1, 0, 0}), Command(Code::Wait, {9999}), Command(Code::END)};
	REQUIRE(lcf::LDB_Reader::Save((path / "RPG_RT.ldb").string(), *database));
	REQUIRE(lcf::LMU_Reader::Save((path / "Map0001.lmu").string(), *map, lcf::EngineVersion::e2k3));
	auto reread = lcf::LDB_Reader::Load((path / "RPG_RT.ldb").string());
	REQUIRE(reread != nullptr);
	CHECK(reread->commonevents.size() == 1);
	CHECK(reread->commonevents.front().event_commands == common.event_commands);
}

TEST_CASE("Maniac 3019 generated 1005 fixture serialization") {
	const char* directory = std::getenv("MANIAC_COMMON1005_GENERATED_DIR");
	if (!directory) return;
	const std::filesystem::path path(directory);
	auto database = lcf::LDB_Reader::Load((path / "RPG_RT.ldb").string());
	auto map = lcf::LMU_Reader::Load((path / "Map0001.lmu").string());
	REQUIRE(database != nullptr);
	REQUIRE(map != nullptr);
	int converted = 0;
	const auto convert = [&](std::vector<lcf::rpg::EventCommand>& commands) {
		for (auto& command : commands) {
			if (command.code != static_cast<int32_t>(Code::CallCommonEvent)) continue;
			std::vector<int32_t> parameters{0x30, static_cast<int32_t>(Code::CallCommonEvent), 4,
				static_cast<int32_t>(std::min<size_t>(2, command.parameters.size())), 0};
			for (size_t index = 0; index < 2 && index < command.parameters.size(); ++index) {
				parameters.push_back(command.parameters[index]);
			}
			command.code = static_cast<int32_t>(Code::Maniac_CallCommand);
			command.parameters = lcf::DBArray<int32_t>(parameters.begin(), parameters.end());
			command.string = lcf::DBString("3019_GENERATED_1005_TEXT");
			++converted;
		}
	};
	for (auto& common : database->commonevents) convert(common.event_commands);
	for (auto& event : map->events) {
		for (auto& page : event.pages) convert(page.event_commands);
	}
	for (auto& troop : database->troops) {
		for (auto& page : troop.pages) convert(page.event_commands);
	}
	CHECK((converted == 63 || converted == 124));
	REQUIRE(lcf::LDB_Reader::Save((path / "RPG_RT.ldb").string(), *database));
	REQUIRE(lcf::LMU_Reader::Save((path / "Map0001.lmu").string(), *map, lcf::EngineVersion::e2k3));
	auto reread = lcf::LDB_Reader::Load((path / "RPG_RT.ldb").string());
	auto reread_map = lcf::LMU_Reader::Load((path / "Map0001.lmu").string());
	REQUIRE(reread != nullptr);
	REQUIRE(reread_map != nullptr);
	int serialized = 0;
	const auto check_commands = [&](const std::vector<lcf::rpg::EventCommand>& commands) {
		for (const auto& command : commands) {
			CHECK(command.code != static_cast<int32_t>(Code::CallCommonEvent));
			if (command.code == static_cast<int32_t>(Code::Maniac_CallCommand)) {
				++serialized;
				CHECK(command.string == lcf::DBString("3019_GENERATED_1005_TEXT"));
			}
		}
	};
	for (const auto& common : reread->commonevents) {
		check_commands(common.event_commands);
	}
	for (const auto& event : reread_map->events) {
		for (const auto& page : event.pages) check_commands(page.event_commands);
	}
	for (const auto& troop : reread->troops) {
		for (const auto& page : troop.pages) check_commands(page.event_commands);
	}
	CHECK(serialized == converted);
}

TEST_CASE("Maniac 1005 frame isolation fixture serialization") {
	const char* directory = std::getenv("MANIAC_COMMON1005_FRAME_DIR");
	const char* context = std::getenv("MANIAC_COMMON1005_FRAME_CONTEXT");
	if (!directory || !context) return;
	const std::filesystem::path path(directory);
	auto database = lcf::LDB_Reader::Load((path / "RPG_RT.ldb").string());
	auto map = lcf::LMU_Reader::Load((path / "Map0001.lmu").string());
	REQUIRE(database != nullptr);
	REQUIRE(map != nullptr);
	const auto set = [](int destination, int value) {
		return Command(Code::ControlVars, {0, destination, 0, 0, 0, value, 0});
	};
	const auto copy = [](int destination, int source) {
		return Command(Code::ControlVars, {0, destination, 0, 0, 1, source, 0});
	};
	database->commonevents.clear();
	for (int id = 1; id <= 3; ++id) {
		auto& common = database->commonevents.emplace_back();
		common.ID = id;
		common.name = lcf::DBString("1005 frame " + std::to_string(id));
	}
	database->commonevents[0].event_commands = {
		copy(1, -1), set(-1, 31), copy(2, -1), Command(Code::CallCommonEvent, {2}),
		copy(13, 20), Command(Code::EndEventProcessing, {0, 1, 111}), Command(Code::END)};
	database->commonevents[1].event_commands = {
		copy(3, -1), set(-1, 42), copy(4, -1),
		Command(Code::EndEventProcessing, {0, 1, 222}), Command(Code::END)};
	database->commonevents[2].event_commands = {
		copy(5, -1), Command(Code::CallCommonEvent, {1}), copy(6, -1), copy(7, 20),
		Command(Code::EndEventProcessing, {0, 1, 333}), Command(Code::END)};
	std::vector<lcf::rpg::EventCommand> commands{
		set(-1, 90), set(20, 123),
		Command(Code::CallEvent, {0, 3, 0, 0x20, 0, 20, 3, 1, 77, 4, 1, 7, 1}),
		copy(8, 20), copy(9, -1), copy(10, 1), copy(11, 3), copy(12, 13),
		Command(Code::CallCommonEvent, {1}), copy(14, -1),
		Command(Code::Maniac_Save, {0, 1, 0, 0}), Command(Code::Wait, {9999}), Command(Code::END)};
	commands[2].string = lcf::DBString("P");
	if (std::string(context) == "nested" || std::string(context) == "battle") {
		auto& wrapper = database->commonevents.emplace_back();
		wrapper.ID = 4;
		wrapper.name = lcf::DBString("1005 root wrapper");
		wrapper.event_commands = commands;
		commands = {Command(Code::CallCommonEvent, {4}), Command(Code::Wait, {9999}), Command(Code::END)};
	}
	if (std::string(context) == "battle") {
		REQUIRE(!database->troops.empty());
		REQUIRE(!database->troops.front().pages.empty());
		database->troops.front().pages.front().event_commands = commands;
	} else {
		auto& page = map->events.front().pages.front();
		page.trigger = std::string(context) == "parallel" ? 4 : 3;
		page.event_commands = commands;
	}
	REQUIRE(lcf::LDB_Reader::Save((path / "RPG_RT.ldb").string(), *database));
	REQUIRE(lcf::LMU_Reader::Save((path / "Map0001.lmu").string(), *map, lcf::EngineVersion::e2k3));
	auto reread = lcf::LDB_Reader::Load((path / "RPG_RT.ldb").string());
	auto reread_map = lcf::LMU_Reader::Load((path / "Map0001.lmu").string());
	REQUIRE(reread != nullptr);
	REQUIRE(reread_map != nullptr);
	CHECK(reread->commonevents == database->commonevents);
	CHECK(reread_map->events == map->events);
	CHECK(reread->troops == database->troops);
}

TEST_CASE("Maniac 1005 creates fresh fields without copying a dirty caller") {
	struct Interpreter : Game_Interpreter {
		void DirtyCaller() {
			auto& frame = _state.stack.back();
			frame.current_command = 17;
			frame.triggered_by_decision_key = true;
			frame.subcommand_path = {1, 2, 3};
			frame.maniac_event_info = 0x13;
			frame.maniac_event_page_id = 9;
			frame.maniac_loop_info_size = 1;
			frame.maniac_loop_info = {31, 41};
			frame.easyrpg_maniac_return_type = 2;
			frame.easyrpg_maniac_return_address = 20;
		}
	};
	struct Restore {
		int patch = Player::game_config.patch_maniac.Get();
		int version = Player::game_config.maniac_version.Get();
		~Restore() {
			Player::game_config.patch_maniac.Set(patch);
			Player::game_config.maniac_version.Set(version);
		}
	};
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		Restore restore;
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		lcf::Data::commonevents.clear();
		Game_Map::GetCommonEvents().clear();
		for (int id = 1; id <= 2; ++id) {
			auto& common = lcf::Data::commonevents.emplace_back();
			common.ID = id;
			common.event_commands = {Command(Code::Wait, {10}), Command(Code::END)};
			Game_Map::GetCommonEvents().emplace_back(id);
		}
		Interpreter interpreter;
		interpreter.Push({Command(Code::Wait, {9999})}, 5, true, 9);
		interpreter.DirtyCaller();
		const auto caller = interpreter.GetState().stack.back();
		for (int id = 1; id <= 2; ++id) {
			REQUIRE(interpreter.ExecuteCommand(Command(Code::CallCommonEvent, {id})));
			const auto& frames = interpreter.GetState().stack;
			REQUIRE(frames.size() == static_cast<size_t>(id + 1));
			lcf::rpg::SaveEventExecFrame expected;
			expected.ID = id + 1;
			expected.commands = lcf::Data::commonevents[id - 1].event_commands;
			expected.maniac_event_id = id;
			expected.maniac_event_info = 0x25;
			expected.easyrpg_self_scope = 1;
			expected.easyrpg_self_event_id = id;
			CHECK(frames.back() == expected);
			CHECK(frames.back().ID == expected.ID);
			CHECK(frames.front() == caller);
		}
	}
	Graphics::Quit();
}

TEST_CASE("Maniac 3019 non-inline 1005 fixture serialization") {
	const char* directory = std::getenv("MANIAC_COMMON1005_MODES_DIR");
	const char* context = std::getenv("MANIAC_COMMON1005_MODES_CONTEXT");
	if (!directory || !context) return;
	const std::filesystem::path path(directory);
	const std::string mode(context);
	auto database = lcf::LDB_Reader::Load((path / "RPG_RT.ldb").string());
	auto map = lcf::LMU_Reader::Load((path / "Map0001.lmu").string());
	REQUIRE(database != nullptr);
	REQUIRE(map != nullptr);
	const auto set = [](int destination, int value) {
		return Command(Code::ControlVars, {0, destination, 0, 0, 0, value, 0});
	};
	const auto copy = [](int destination, int source) {
		return Command(Code::ControlVars, {0, destination, 0, 0, 1, source, 0});
	};
	const auto add = [](int destination) {
		return Command(Code::ControlVars, {0, destination, 0, 1, 0, 1, 0});
	};
	const auto produce = [](std::initializer_list<int32_t> args, const char* text) {
		auto result = Command(Code::Maniac_CallCommand, args);
		result.string = lcf::DBString(text);
		return result;
	};
	database->commonevents.clear();
	for (int id = 1; id <= (mode == "nested" ? 4 : 3); ++id) {
		auto& common = database->commonevents.emplace_back();
		common.ID = id;
		common.name = lcf::DBString("3019 source " + std::to_string(id));
	}
	database->commonevents[0].event_commands = {add(1), Command(Code::END)};
	database->commonevents[1].event_commands = {add(2), Command(Code::END)};
	auto string = Command(Code::Maniac_ControlStrings, {0, 10, 0, 0, 0, 0, 0, 0});
	string.string = lcf::DBString("DYNAMIC_TEXT");
	std::vector<lcf::rpg::EventCommand> sequence{
		set(1, 0), set(2, 0), set(220, 1005), set(221, 220), set(210, 200),
		set(211, 210), set(212, 1), set(213, 212), set(230, 10), set(240, 2), string};
	const auto append = [&](lcf::rpg::EventCommand command, int index) {
		sequence.push_back(std::move(command));
		sequence.push_back(copy(100 + index, 1));
		sequence.push_back(copy(110 + index, 2));
	};
	sequence.push_back(set(200, 1));
	append(produce({0, 1005, 200, 1, 0}, "RAW_TEXT"), 0);
	sequence.push_back(set(200, 2));
	append(produce({0x1111, 220, 210, 212, 10}, "IGNORED_VAR_TEXT"), 1);
	sequence.push_back(set(200, 1));
	append(produce({0x2222, 221, 211, 213, 230}, "IGNORED_INDIRECT_TEXT"), 2);
	sequence.push_back(set(-200, 2));
	append(produce({0, 1005, -200, 1, 0}, "SELF_TEXT"), 3);
	append(produce({0x1040, 1005, 0, 0, 10, 0x00000101}, "IGNORED_EXPR_TEXT"), 4);
	append(produce({0x40, 1005, 0, 0, 0, 0x00f00108}, "EXPR_VAR_TEXT"), 5);
	append(produce({0x40, 1005, 0, 0, 0, static_cast<int32_t>(0xfb010822u), 0x00000101},
		"EXPR_ASSIGN_TEXT"), 6);
	sequence.push_back(Command(Code::END));
	database->commonevents[2].event_commands = sequence;
	if (mode == "nested") {
		database->commonevents[3].event_commands = {
			Command(Code::CallCommonEvent, {3}), set(5, 55), Command(Code::END)};
	}
	std::vector<lcf::rpg::EventCommand> parent{
		Command(Code::CallCommonEvent, {mode == "nested" ? 4 : 3}), set(4, 99),
		Command(Code::Maniac_Save, {0, 1, 0, 0}), Command(Code::Wait, {9999}), Command(Code::END)};
	if (mode == "battle") {
		REQUIRE(!database->troops.empty());
		REQUIRE(!database->troops.front().pages.empty());
		database->troops.front().pages.front().event_commands = parent;
	} else {
		auto& page = map->events.front().pages.front();
		page.trigger = mode == "parallel" ? 4 : 3;
		page.event_commands = parent;
	}
	REQUIRE(lcf::LDB_Reader::Save((path / "RPG_RT.ldb").string(), *database));
	REQUIRE(lcf::LMU_Reader::Save((path / "Map0001.lmu").string(), *map, lcf::EngineVersion::e2k3));
	auto reread = lcf::LDB_Reader::Load((path / "RPG_RT.ldb").string());
	auto reread_map = lcf::LMU_Reader::Load((path / "Map0001.lmu").string());
	REQUIRE(reread != nullptr);
	REQUIRE(reread_map != nullptr);
	CHECK(reread->commonevents == database->commonevents);
	CHECK(reread_map->events == map->events);
	CHECK(reread->troops == database->troops);
	int generated = 0;
	for (const auto& command : reread->commonevents[2].event_commands) {
		if (command.code == static_cast<int32_t>(Code::Maniac_CallCommand)) ++generated;
		CHECK(command.code != static_cast<int32_t>(Code::CallCommonEvent));
	}
	CHECK(generated == 7);
}

#ifndef DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
TEST_CASE("Maniac 250921 common call error and depth boundaries") {
	struct Error { std::string message; };
	struct Interpreter : Game_Interpreter {
		Interpreter() : Game_Interpreter(true) {}
		void SetDepth(size_t depth) { _state.stack.resize(depth); }
	};
	struct Restore {
		int patch = Player::game_config.patch_maniac.Get();
		int version = Player::game_config.maniac_version.Get();
		~Restore() {
			Output::SetLogCallback(nullptr);
			Player::game_config.patch_maniac.Set(patch);
			Player::game_config.maniac_version.Set(version);
		}
	};
	Graphics::Init();
	{
		MockGame game(MockMap::eNone);
		Restore restore;
		Player::game_config.patch_maniac.Set(1);
		Player::game_config.maniac_version.Set(250921);
		lcf::Data::commonevents.clear();
		Game_Map::GetCommonEvents().clear();
		lcf::Data::commonevents.emplace_back();
		lcf::Data::commonevents.back().ID = 1;
		Game_Map::GetCommonEvents().emplace_back(1);
		Output::SetLogCallback([](LogLevel level, const std::string& message, LogCallbackUserData) {
			if (level == LogLevel::Error) throw Error{message};
		});
		std::ofstream evidence;
		if (const char* path = std::getenv("MANIAC_COMMON1005_PLAYER_EVIDENCE")) {
			evidence.open(path);
			evidence << "case\tdepth_before\tdepth_after\tcommon_id\tkind\terror\n";
		}
		for (const auto& entry : std::vector<std::pair<std::string, std::vector<int32_t>>>{
			{"missing", {}}, {"zero", {0}}, {"negative", {-1}}, {"out-of-range", {2}}, {"int-max", {2147483647}}}) {
			Interpreter interpreter;
			interpreter.SetDepth(1);
			auto call = Command(Code::CallCommonEvent);
			call.parameters = lcf::DBArray<int32_t>(entry.second.begin(), entry.second.end());
			std::string error;
			try { interpreter.ExecuteCommand(call); } catch (const Error& raised) { error = raised.message; }
			CHECK(error.find("invalid common event") != std::string::npos);
			REQUIRE(interpreter.GetState().stack.size() == 2);
			const auto& frame = interpreter.GetState().stack.back();
			CHECK(frame.maniac_event_id == (entry.second.empty() ? 0 : entry.second[0]));
			CHECK(frame.maniac_event_info == 0x25);
			CHECK(frame.commands.empty());
			if (evidence) evidence << entry.first << "\t1\t2\t" << frame.maniac_event_id << "\t37\t" << error << '\n';
			interpreter.Clear();
			CHECK(!interpreter.IsRunning());
		}
		Interpreter interpreter;
		interpreter.SetDepth(8192);
		CHECK(interpreter.ExecuteCommand(Command(Code::CallCommonEvent, {1})));
		REQUIRE(interpreter.GetState().stack.size() == 8193);
		CHECK(interpreter.GetState().stack.back().commands.empty());
		if (evidence) evidence << "depth-pass\t8192\t8193\t1\t37\t-\n";
		std::string error;
		try { interpreter.ExecuteCommand(Command(Code::CallCommonEvent, {2})); } catch (const Error& raised) { error = raised.message; }
		CHECK(error.find("8192") != std::string::npos);
		REQUIRE(interpreter.GetState().stack.size() == 8193);
		CHECK(interpreter.GetState().stack.back().maniac_event_id == 1);
		if (evidence) evidence << "depth-error\t8193\t8193\t1\t37\t" << error << '\n';
		interpreter.Clear();
		CHECK(!interpreter.IsRunning());
		interpreter.Push({Command(Code::CallCommonEvent, {1}),
			Command(Code::ControlVars, {0, 2, 0, 0, 0, 99, 0}), Command(Code::END)}, 0);
		interpreter.Update();
		CHECK(Main_Data::game_variables->Get(2) == 99);
		CHECK(!interpreter.IsRunning());
		if (evidence) evidence << "empty\t1\t0\t1\t37\t-\n";
	}
	Graphics::Quit();
}
#endif
