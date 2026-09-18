// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <algorithm>
#include <vector>

// Turning a scattered set of fastmem page offsets into the fewest mprotect calls that cover
// them.
//
// vtlb protects fastmem a page at a time, once per alias the guest has a page mapped through.
// For one page that is one syscall and there is nothing to do about it, but a whole-RAM pass -
// mmap_ResetBlockTracking, which runs before every save-state load and on every VM reset -
// walks 32MB and pays a syscall for each of its 8192 pages, times every alias. Those aliases
// are very nearly contiguous in the fastmem area, so nearly all of that is one range being
// re-entered a page at a time.
//
// Merging them changes nothing about which bytes end up protected: a run only ever extends
// across offsets that touch, so it spans exactly the pages that were going to be protected
// individually and no others. It only stops asking the kernel the same question 8192 times.
namespace vtlbPageRuns
{
	struct Run
	{
		u64 start;
		u64 size;
	};

	/// Sorts `offsets` in place, drops duplicate aliases, and returns the fewest runs of
	/// `page_size`-sized pages that cover them. `offsets` must all be `page_size`-aligned.
	inline std::vector<Run> Build(std::vector<u32>& offsets, u64 page_size)
	{
		std::vector<Run> runs;
		if (offsets.empty() || page_size == 0)
			return runs;

		std::sort(offsets.begin(), offsets.end());
		offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());

		Run current{offsets[0], page_size};
		for (size_t i = 1; i < offsets.size(); i++)
		{
			// Extend only across a page boundary that actually touches, so a gap in the
			// mapping never ends up inside a run - those pages may not be mapped at all,
			// and one mprotect across a hole fails for the whole range.
			if (static_cast<u64>(offsets[i]) == current.start + current.size)
			{
				current.size += page_size;
				continue;
			}

			runs.push_back(current);
			current = Run{offsets[i], page_size};
		}
		runs.push_back(current);

		return runs;
	}
} // namespace vtlbPageRuns
