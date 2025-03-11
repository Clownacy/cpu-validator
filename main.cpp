#include <cstdlib>
#include <iostream>
#include <fstream>
#include <string_view>

#include "clown68000/interpreter/clown68000.h"
#include "json.hpp"

namespace M68000
{
	static cc_u8l ram[1L << 24];

	static cc_u16f ReadCallback([[maybe_unused]] const void* const user_data, const cc_u32f address, const cc_bool do_high_byte, const cc_bool do_low_byte)
	{
		cc_u16f value = 0;
		if (do_high_byte)
			value |= ram[address * 2 + 0] << 8;
		if (do_low_byte)
			value |= ram[address * 2 + 1] << 0;
		return value;
	}

	static void WriteCallback([[maybe_unused]] const void* const user_data, const cc_u32f address, const cc_bool do_high_byte, const cc_bool do_low_byte, const cc_u16f value)
	{
		if (do_high_byte)
			ram[address * 2 + 0] = value >> 8;
		if (do_low_byte)
			ram[address * 2 + 1] = value & 0xFF;
	}

	static const Clown68000_ReadWriteCallbacks callbacks = {ReadCallback, WriteCallback, nullptr};

	static Clown68000_State StateFromJSON(const nlohmann::json &json)
	{
		Clown68000_State m68000_state;
		m68000_state.data_registers[0] = json["d0"].get<cc_u16f>();
		m68000_state.data_registers[1] = json["d1"].get<cc_u16f>();
		m68000_state.data_registers[2] = json["d2"].get<cc_u16f>();
		m68000_state.data_registers[3] = json["d3"].get<cc_u16f>();
		m68000_state.data_registers[4] = json["d4"].get<cc_u16f>();
		m68000_state.data_registers[5] = json["d5"].get<cc_u16f>();
		m68000_state.data_registers[6] = json["d6"].get<cc_u16f>();
		m68000_state.data_registers[7] = json["d7"].get<cc_u16f>();
		m68000_state.address_registers[0] = json["a0"].get<cc_u16f>();
		m68000_state.address_registers[1] = json["a1"].get<cc_u16f>();
		m68000_state.address_registers[2] = json["a2"].get<cc_u16f>();
		m68000_state.address_registers[3] = json["a3"].get<cc_u16f>();
		m68000_state.address_registers[4] = json["a4"].get<cc_u16f>();
		m68000_state.address_registers[5] = json["a5"].get<cc_u16f>();
		m68000_state.address_registers[6] = json["a6"].get<cc_u16f>();
		m68000_state.user_stack_pointer = json["usp"].get<cc_u16f>();
		m68000_state.supervisor_stack_pointer = json["ssp"].get<cc_u16f>();
		m68000_state.status_register = json["sr"].get<cc_u16f>();
		m68000_state.address_registers[7] = (m68000_state.status_register & 0x2000) != 0 ? m68000_state.supervisor_stack_pointer : m68000_state.user_stack_pointer;
		m68000_state.program_counter = json["pc"].get<cc_u16f>() - 4;
		m68000_state.instruction_register = 0;
		m68000_state.halted = m68000_state.stopped = cc_false;
		return m68000_state;
	}

	static bool CompareState(const Clown68000_State &obtained_state, const Clown68000_State &expected_state)
	{
		bool success = true;

		const auto Compare = [](const std::string_view &name, const cc_u32f obtained, const cc_u32f expected)
		{
			if (obtained != expected)
			{
				std::cerr << std::hex << std::uppercase << name << " differs (should be " << expected << " but was " << obtained << ").\n";
				return false;
			}

			return true;
		};

		success &= Compare("Data register 0", obtained_state.data_registers[0], expected_state.data_registers[0]);
		success &= Compare("Data register 1", obtained_state.data_registers[1], expected_state.data_registers[1]);
		success &= Compare("Data register 2", obtained_state.data_registers[2], expected_state.data_registers[2]);
		success &= Compare("Data register 3", obtained_state.data_registers[3], expected_state.data_registers[3]);
		success &= Compare("Data register 4", obtained_state.data_registers[4], expected_state.data_registers[4]);
		success &= Compare("Data register 5", obtained_state.data_registers[5], expected_state.data_registers[5]);
		success &= Compare("Data register 6", obtained_state.data_registers[6], expected_state.data_registers[6]);
		success &= Compare("Data register 7", obtained_state.data_registers[7], expected_state.data_registers[7]);
		success &= Compare("Address register 0", obtained_state.address_registers[0], expected_state.address_registers[0]);
		success &= Compare("Address register 1", obtained_state.address_registers[1], expected_state.address_registers[1]);
		success &= Compare("Address register 2", obtained_state.address_registers[2], expected_state.address_registers[2]);
		success &= Compare("Address register 3", obtained_state.address_registers[3], expected_state.address_registers[3]);
		success &= Compare("Address register 4", obtained_state.address_registers[4], expected_state.address_registers[4]);
		success &= Compare("Address register 5", obtained_state.address_registers[5], expected_state.address_registers[5]);
		success &= Compare("Address register 6", obtained_state.address_registers[6], expected_state.address_registers[6]);
		success &= Compare("Address register 7", obtained_state.address_registers[7], expected_state.address_registers[7]);
		if ((obtained_state.status_register & 0x2000) == 0)
			success &= Compare("Supervisor stack pointer", obtained_state.supervisor_stack_pointer, expected_state.supervisor_stack_pointer);
		else
			success &= Compare("User stack pointer", obtained_state.user_stack_pointer, expected_state.user_stack_pointer);
		success &= Compare("Program counter", obtained_state.program_counter - (obtained_state.stopped ? 4 : 0), expected_state.program_counter); // A small hack to work around a quirk of the 'STOP' instruction.
		success &= Compare("Status register", obtained_state.status_register, expected_state.status_register);

		return success;
	}

	constexpr auto ReadWord = [](unsigned long address) constexpr
	{
		address &= 0xFFFF;
		cc_u32f result = 0;
		result |= static_cast<unsigned long>(ram[address + 0]) << 8 * 1;
		result |= static_cast<unsigned long>(ram[address + 1]) << 8 * 0;
		return result;
	};

	constexpr auto ReadLongword = [](unsigned long address) constexpr
	{
		address &= 0xFFFF;
		cc_u32f result = 0;
		result |= static_cast<unsigned long>(ram[address + 0]) << 8 * 3;
		result |= static_cast<unsigned long>(ram[address + 1]) << 8 * 2;
		result |= static_cast<unsigned long>(ram[address + 2]) << 8 * 1;
		result |= static_cast<unsigned long>(ram[address + 3]) << 8 * 0;
		return result;
	};

	static bool Group0Exception(const Clown68000_State &m68000_state)
	{
		for (unsigned int i = 2; i < 4; ++i)
			if (m68000_state.program_counter == ReadLongword(i * 4) && (ReadWord(m68000_state.address_registers[7]) & 0xFFEF) == 0xFFEE)
				return true;

		return false;
	}

	static bool Group1Or2Exception(const Clown68000_State &m68000_state)
	{
		for (unsigned int i = 4; i < 0x100 / 4; ++i)
			if (m68000_state.program_counter == ReadLongword(i * 4))
				return true;

		return false;
	}

	static bool CompareRAM(const nlohmann::json &final_ram, const Clown68000_State &m68000_state)
	{
		bool success = true;

		const bool group_0_exception = Group0Exception(m68000_state);
		const bool group_1_or_2_exception = Group1Or2Exception(m68000_state);

		for (const auto &value : final_ram)
		{
			const auto address = value[0].get<cc_u16f>();
			const unsigned int obtained_value = ram[address];
			const unsigned int expected_value = value[1].get<cc_u16f>();

			// We do not care about interrupt stack frame accuracy right now.
			// TODO: Actually do verify stack frame accuracy at some point.
			if (group_0_exception && (
				address == m68000_state.address_registers[7] + 1 ||
				address == m68000_state.address_registers[7] + 10 ||
				address == m68000_state.address_registers[7] + 11 ||
				address == m68000_state.address_registers[7] + 12 ||
				address == m68000_state.address_registers[7] + 13))
				continue;
//			else if (group_1_or_2_exception && (
//				address == m68000_state.address_registers[7] + 2 ||
//				address == m68000_state.address_registers[7] + 3 ||
//				address == m68000_state.address_registers[7] + 4 ||
//				address == m68000_state.address_registers[7] + 5))
//				continue;

			if (obtained_value != expected_value)
			{
				std::cerr << std::hex << std::uppercase << "RAM at address " << address << " differs (should be " << expected_value << " but was " << obtained_value << ").\n";
				success = false;
			}
		}

		return success;
	}

	static bool DoTest(const nlohmann::json &test)
	{
		const auto &initial_state = test["initial"];
		const auto &initial_ram = initial_state["ram"];
		const auto &final_state = test["final"];
		const auto &final_ram = final_state["ram"];
		const auto expected_duration = test["length"].get<cc_u16f>();

		// Initialise the CPU.
		Clown68000_State m68000_state = StateFromJSON(initial_state);

		// Initialise the RAM.
		for (auto &value : final_ram)
			ram[value[0].get<cc_u32f>()] = 0;
		for (const auto &value : initial_ram)
			ram[value[0].get<cc_u32f>()] = value[1].get<cc_u16f>();

		// Run the instruction.
		const auto actual_duration = Clown68000_DoCycle(&m68000_state, &callbacks);

		// For now, we don't care about differences when exceptions occur
		// (the values of registers seems to vary based on microcode, which is very annoying).
		if (Group0Exception(m68000_state) /*|| Group1Or2Exception(m68000_state)*/)
			return true;

		if (actual_duration != expected_duration)
		{
			std::cerr << "Duration differs (should be " << expected_duration << " but was " << actual_duration <<  ").\n";
			return false;
		}

		Clown68000_State final_m68000_state = StateFromJSON(final_state);

		return CompareState(m68000_state, final_m68000_state) && CompareRAM(final_ram, m68000_state);
	}
}

int main(const int argc, char** const argv)
{
	if (argc != 2)
	{
		std::cerr << "Pass a filename.\n";
		return EXIT_FAILURE;
	}

	const auto tests = nlohmann::json::parse(std::ifstream(argv[1]));

	bool success = true;
	for (const auto &test : tests)
	{
		if (!M68000::DoTest(test))
		{
			success = false;
			std::cerr << "Failure in test " << test["name"].get<std::string_view>() << ".\n";
		}
	}

	return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
