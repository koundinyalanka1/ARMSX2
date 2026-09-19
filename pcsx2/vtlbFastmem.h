// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "common/Assertions.h"

#include <span>

namespace vtlbFastmem
{
	// A kernel mapping can cover several guest TLB pages only when their file
	// offsets are contiguous and the first is aligned to that mapping's size.
	// mapping_size is independent of the build's reservation alignment and of
	// guest_page_size. Callers validate the kernel size before constructing maps.
	inline bool IsCoalesced(std::span<const u32> offsets, u32 page, u32 mapping_size, u32 guest_page_size)
	{
		const u32 count = mapping_size / guest_page_size;
		pxAssert(count != 0 && (count & (count - 1)) == 0);
		const u32 base = page & ~(count - 1);
		if (static_cast<size_t>(base) + count > offsets.size())
			return false;

		const u32 base_offset = offsets[base];
		if ((base_offset & (mapping_size - 1)) != 0)
			return false;

		for (u32 i = 0; i < count; i++)
		{
			if (offsets[base + i] != base_offset + i * guest_page_size)
				return false;
		}
		return true;
	}
} // namespace vtlbFastmem
