// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "libretro.h"

#include "pcsx2/MemoryTypes.h"

// The PS2 address space as libretro describes it, for the frontend's
// RetroAchievements client to read through.
//
// This lives in a header of its own, apart from the core that publishes it,
// so the geometry can be checked against rcheevos' own PS2 console map
// without a frontend and without a booted VM - see
// tests/ctest/core/libretro_memory_map_tests.cpp. The numbers here have to
// agree with that map exactly, and nothing about building the core would say
// so if they stopped.
namespace LibretroMemoryMap
{
	/// Main RAM, then the scratchpad.
	inline constexpr unsigned kDescriptorCount = 2;

	/// Where the scratchpad answers on the EE bus. rcheevos' third PS2 region
	/// is mapped here; in the host struct it sits 128MB into the allocation,
	/// past the whole devkit-sized Main array, which is why a frontend cannot
	/// find it from the system-RAM block alone.
	inline constexpr u32 kScratchpadAddress = 0x70000000;

	/// Describes `main_ram` and `scratchpad` into `out`, which must have room
	/// for kDescriptorCount entries.
	///
	/// Main RAM is reported as the 32MB the console has and achievement sets
	/// are written against, not the 128MB the Main array reserves for devkit
	/// mode: the bytes past 32MB are not part of the address space a set can
	/// name.
	///
	/// Both descriptors leave `select` zero, which libretro reads as "start
	/// and len are the whole mapping" and only allows for a power-of-two len.
	inline void Build(u8* main_ram, u8* scratchpad, retro_memory_descriptor* out)
	{
		static_assert((Ps2MemSize::MainRam & (Ps2MemSize::MainRam - 1)) == 0);
		static_assert((Ps2MemSize::Scratch & (Ps2MemSize::Scratch - 1)) == 0);

		out[0] = {};
		out[0].flags = RETRO_MEMDESC_SYSTEM_RAM;
		out[0].ptr = main_ram;
		out[0].start = 0x00000000;
		out[0].len = Ps2MemSize::MainRam;
		out[0].addrspace = "main";

		out[1] = {};
		out[1].flags = RETRO_MEMDESC_SYSTEM_RAM;
		out[1].ptr = scratchpad;
		out[1].start = kScratchpadAddress;
		out[1].len = Ps2MemSize::Scratch;
		out[1].addrspace = "scratchpad";
	}
} // namespace LibretroMemoryMap
