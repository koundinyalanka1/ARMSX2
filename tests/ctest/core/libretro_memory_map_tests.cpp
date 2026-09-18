// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The libretro core hands the frontend's RetroAchievements client a memory map, and rcheevos
// reads it through a console map of its own: three PS2 regions, each naming the real address
// its bytes live at. Nothing links the two at build time. If the map here and the map there
// stop agreeing - a region moves, main RAM is reported at the devkit size, the scratchpad
// address is mistyped - the core still builds, still runs, and achievements quietly read the
// wrong bytes or none at all.
//
// So resolve rcheevos' own PS2 regions through the descriptors the core publishes, the way a
// frontend does, and require that every byte of all three lands where it should.

#include "MemoryMap.h"

#include "rc_consoles.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>

namespace
{
	// Stand-ins for the VM's allocation. Only the addresses matter here, never the contents,
	// but they are real storage so a resolved pointer can be compared against a known base.
	u8 s_main_ram[Ps2MemSize::MainRam];
	u8 s_scratchpad[Ps2MemSize::Scratch];

	struct Resolved
	{
		const u8* ptr;
		u32 run; // Contiguous bytes available from ptr in the descriptor that claimed it.
	};

	// What a frontend does with a descriptor table: find the one that claims a real address
	// and turn it into a host pointer. select == 0 descriptors are a plain range check, which
	// is all the core publishes.
	std::optional<Resolved> Resolve(const retro_memory_descriptor* descriptors, u32 real_address)
	{
		for (unsigned i = 0; i < LibretroMemoryMap::kDescriptorCount; i++)
		{
			const retro_memory_descriptor& d = descriptors[i];
			if (d.select != 0)
				continue; // Not a shape this test (or the core) produces.

			if (real_address < d.start || real_address >= d.start + d.len)
				continue;

			const size_t offset = real_address - d.start;
			return Resolved{static_cast<const u8*>(d.ptr) + d.offset + offset,
				static_cast<u32>(d.len - offset)};
		}
		return std::nullopt;
	}
} // namespace

TEST(LibretroMemoryMap, CoversEveryRcheevosPS2Region)
{
	retro_memory_descriptor descriptors[LibretroMemoryMap::kDescriptorCount];
	LibretroMemoryMap::Build(s_main_ram, s_scratchpad, descriptors);

	const rc_memory_regions_t* const regions = rc_console_memory_regions(RC_CONSOLE_PLAYSTATION_2);
	ASSERT_NE(regions, nullptr);
	ASSERT_GT(regions->num_regions, 0u);

	for (uint32_t i = 0; i < regions->num_regions; i++)
	{
		const rc_memory_region_t& region = regions->region[i];
		SCOPED_TRACE(region.description);

		// Only the RAM regions are the core's to serve; rcheevos marks anything else
		// (padding, unused space) with a type this map does not claim to cover.
		if (region.type != RC_MEMORY_TYPE_SYSTEM_RAM)
			continue;

		u32 remaining = region.end_address - region.start_address + 1;
		u32 real_address = region.real_address;
		while (remaining > 0)
		{
			const std::optional<Resolved> resolved = Resolve(descriptors, real_address);
			ASSERT_TRUE(resolved.has_value())
				<< "no descriptor claims real address " << std::hex << real_address;

			const u32 taken = std::min(remaining, resolved->run);
			remaining -= taken;
			real_address += taken;
		}
	}
}

TEST(LibretroMemoryMap, ReportsTheConsolesRamNotTheDevkitCeiling)
{
	retro_memory_descriptor descriptors[LibretroMemoryMap::kDescriptorCount];
	LibretroMemoryMap::Build(s_main_ram, s_scratchpad, descriptors);

	// rcheevos' first two PS2 regions run to 0x01FFFFFF and no further, so a core that
	// reported the 128MB Main array would be describing 96MB no set can reference - and
	// would leave the scratchpad descriptor overlapping it.
	const rc_memory_regions_t* const regions = rc_console_memory_regions(RC_CONSOLE_PLAYSTATION_2);
	ASSERT_NE(regions, nullptr);

	u32 highest_ram_address = 0;
	for (uint32_t i = 0; i < regions->num_regions; i++)
	{
		const rc_memory_region_t& region = regions->region[i];
		if (region.type == RC_MEMORY_TYPE_SYSTEM_RAM && region.real_address < LibretroMemoryMap::kScratchpadAddress)
			highest_ram_address = std::max(highest_ram_address, region.real_address + (region.end_address - region.start_address));
	}

	EXPECT_EQ(descriptors[0].start, 0u);
	EXPECT_EQ(descriptors[0].len, Ps2MemSize::MainRam);
	EXPECT_EQ(descriptors[0].len, highest_ram_address + 1);
	EXPECT_LT(Ps2MemSize::MainRam, Ps2MemSize::TotalRam);
}

TEST(LibretroMemoryMap, PlacesTheScratchpadWhereRcheevosLooksForIt)
{
	retro_memory_descriptor descriptors[LibretroMemoryMap::kDescriptorCount];
	LibretroMemoryMap::Build(s_main_ram, s_scratchpad, descriptors);

	const rc_memory_regions_t* const regions = rc_console_memory_regions(RC_CONSOLE_PLAYSTATION_2);
	ASSERT_NE(regions, nullptr);

	// The scratchpad is the one PS2 region a frontend cannot reach through the coarse
	// RETRO_MEMORY_SYSTEM_RAM block: in the host struct it sits past the whole 128MB Main
	// array, not at the 32MB mark where walking the console map would put it.
	const rc_memory_region_t* scratchpad = nullptr;
	for (uint32_t i = 0; i < regions->num_regions; i++)
	{
		if (regions->region[i].real_address == LibretroMemoryMap::kScratchpadAddress)
			scratchpad = &regions->region[i];
	}
	ASSERT_NE(scratchpad, nullptr) << "rcheevos no longer maps the PS2 scratchpad at 0x70000000";

	EXPECT_EQ(descriptors[1].start, LibretroMemoryMap::kScratchpadAddress);
	EXPECT_EQ(descriptors[1].len, scratchpad->end_address - scratchpad->start_address + 1);
	EXPECT_EQ(descriptors[1].len, Ps2MemSize::Scratch);
	EXPECT_EQ(descriptors[1].ptr, s_scratchpad);
}

TEST(LibretroMemoryMap, LeavesLenAPowerOfTwoForSelectlessDescriptors)
{
	retro_memory_descriptor descriptors[LibretroMemoryMap::kDescriptorCount];
	LibretroMemoryMap::Build(s_main_ram, s_scratchpad, descriptors);

	// libretro only accepts select == 0 ("start and len are the whole mapping") when len is
	// a power of two; a frontend is free to reject or mis-round anything else.
	for (unsigned i = 0; i < LibretroMemoryMap::kDescriptorCount; i++)
	{
		SCOPED_TRACE(descriptors[i].addrspace);
		EXPECT_EQ(descriptors[i].select, 0u);
		EXPECT_NE(descriptors[i].len, 0u);
		EXPECT_EQ(descriptors[i].len & (descriptors[i].len - 1), 0u);
		EXPECT_EQ(descriptors[i].flags & RETRO_MEMDESC_SYSTEM_RAM, uint64_t{RETRO_MEMDESC_SYSTEM_RAM});
		EXPECT_NE(descriptors[i].ptr, nullptr);
	}
}
