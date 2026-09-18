// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// vtlb_UpdateFastmemProtection used to spend one mprotect per page per alias. A whole-RAM
// sweep - mmap_ResetBlockTracking, before every save-state load and on every VM reset - is
// 32MB, so that was 8192 syscalls times however many aliases the guest had the page mapped
// through. Merging the contiguous ones collapses that to a handful.
//
// The merge is only safe because it protects exactly the same bytes. That is the property
// worth pinning: a run may never swallow an offset that was not going to be protected anyway,
// because the pages in a gap can be unmapped entirely and one mprotect across a hole fails for
// the whole range - which would silently leave real pages unprotected and turn SMC detection
// off for them.

#include "vtlbPageRuns.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <vector>

namespace
{
	constexpr u64 kPage = 4096;

	// Every byte a run set covers, the way mprotect would apply it.
	std::set<u64> CoveredBytes(const std::vector<vtlbPageRuns::Run>& runs)
	{
		std::set<u64> covered;
		for (const vtlbPageRuns::Run& run : runs)
		{
			for (u64 page = run.start; page < run.start + run.size; page += kPage)
				covered.insert(page);
		}
		return covered;
	}

	std::set<u64> ExpectedPages(const std::vector<u32>& offsets)
	{
		return std::set<u64>(offsets.begin(), offsets.end());
	}
} // namespace

TEST(VtlbPageRuns, MergesAContiguousSweepIntoOneCall)
{
	// What a 32MB reset looks like when the guest's mapping is contiguous: the case the
	// whole change exists for.
	std::vector<u32> offsets;
	for (u32 i = 0; i < 8192; i++)
		offsets.push_back(static_cast<u32>(i * kPage));

	const std::vector<vtlbPageRuns::Run> runs = vtlbPageRuns::Build(offsets, kPage);

	ASSERT_EQ(runs.size(), 1u);
	EXPECT_EQ(runs[0].start, 0u);
	EXPECT_EQ(runs[0].size, 8192 * kPage);
}

TEST(VtlbPageRuns, DoesNotBridgeAGap)
{
	// The one thing that must never happen: the hole between the two runs may be unmapped,
	// and a single mprotect spanning it would fail for the whole range.
	std::vector<u32> offsets = {0, 4096, 8192, /* gap */ 65536, 69632};

	const std::vector<vtlbPageRuns::Run> runs = vtlbPageRuns::Build(offsets, kPage);

	ASSERT_EQ(runs.size(), 2u);
	EXPECT_EQ(runs[0].start, 0u);
	EXPECT_EQ(runs[0].size, 3 * kPage);
	EXPECT_EQ(runs[1].start, 65536u);
	EXPECT_EQ(runs[1].size, 2 * kPage);
}

TEST(VtlbPageRuns, CoversExactlyThePagesItWasGiven)
{
	// The equivalence that makes the merge safe, over a deliberately awkward layout:
	// unsorted, duplicated (the same page reached through several aliases), and holey.
	std::vector<u32> offsets = {
		40960, 0, 8192, 4096, 40960, 16384, 65536, 0, 69632, 45056, 12288};
	const std::set<u64> expected = ExpectedPages(offsets);

	const std::vector<vtlbPageRuns::Run> runs = vtlbPageRuns::Build(offsets, kPage);

	EXPECT_EQ(CoveredBytes(runs), expected);
}

TEST(VtlbPageRuns, CoversExactlyThePagesItWasGivenAcrossManyLayouts)
{
	// Same equivalence, swept over a range of densities so a sparse map and a nearly solid
	// one are both covered. Deterministic, so a failure is reproducible.
	u32 seed = 12345;
	const auto next = [&seed]() { return (seed = seed * 1103515245u + 12345u) >> 16; };

	for (u32 density = 1; density <= 16; density++)
	{
		std::vector<u32> offsets;
		for (u32 page = 0; page < 512; page++)
		{
			if ((next() % 16u) < density)
				offsets.push_back(static_cast<u32>(page * kPage));
		}
		if (offsets.empty())
			continue;

		SCOPED_TRACE(density);
		const std::set<u64> expected = ExpectedPages(offsets);
		const std::vector<vtlbPageRuns::Run> runs = vtlbPageRuns::Build(offsets, kPage);

		EXPECT_EQ(CoveredBytes(runs), expected);

		// Merging has to be worth doing: never more calls than the per-page loop made.
		EXPECT_LE(runs.size(), expected.size());
	}
}

TEST(VtlbPageRuns, MergesAtTheHostPageSizeOn16KHosts)
{
	// On a host whose pages are larger than vtlb's, only host-aligned offsets are protected
	// and each call covers a whole host page. Contiguity has to be judged at that size.
	constexpr u64 kHostPage = 16384;
	std::vector<u32> offsets = {0, 16384, 32768, /* gap */ 131072};

	const std::vector<vtlbPageRuns::Run> runs = vtlbPageRuns::Build(offsets, kHostPage);

	ASSERT_EQ(runs.size(), 2u);
	EXPECT_EQ(runs[0].start, 0u);
	EXPECT_EQ(runs[0].size, 3 * kHostPage);
	EXPECT_EQ(runs[1].start, 131072u);
	EXPECT_EQ(runs[1].size, kHostPage);
}

TEST(VtlbPageRuns, HandlesNothingToDo)
{
	std::vector<u32> empty;
	EXPECT_TRUE(vtlbPageRuns::Build(empty, kPage).empty());

	// A zero page size would loop forever downstream; refuse it rather than emit a run.
	std::vector<u32> one = {4096};
	EXPECT_TRUE(vtlbPageRuns::Build(one, 0).empty());
}

TEST(VtlbPageRuns, CollapsesAliasesOfTheSamePage)
{
	// The same page reached through KUSEG, KSEG0 and KSEG1 arrives three times; it should
	// still be protected once.
	std::vector<u32> offsets = {8192, 8192, 8192};

	const std::vector<vtlbPageRuns::Run> runs = vtlbPageRuns::Build(offsets, kPage);

	ASSERT_EQ(runs.size(), 1u);
	EXPECT_EQ(runs[0].start, 8192u);
	EXPECT_EQ(runs[0].size, kPage);
}
