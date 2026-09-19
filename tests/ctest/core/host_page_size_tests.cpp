// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// On arm64 the page size is baked in at compile time, and PerformEarlyHardwareChecks refuses
// to boot when the running kernel's does not fit it. That check used to demand equality, which
// made one binary per page size the only option - and left every 16K-page device (Android 15
// and up, Apple Silicon) with no build that would start.
//
// Equality was stricter than the hardware needs. What matters is only that the kernel's page
// divides the one the build was compiled for: every mapping the emulator makes is a whole
// multiple of __pagesize and aligned to one, so a larger compiled page stays legal on a
// smaller kernel page. The reverse cannot work at all.
//
// These pin that asymmetry, because getting it backwards is not a crash at the check - it is
// an mprotect that silently fails somewhere inside fastmem much later.

#include "common/HostSys.h"
#include "vtlbProtection.h"
#include "vtlbFastmem.h"

#include <gtest/gtest.h>

TEST(HostPageSize, RuntimeFastmemMapsIndependent4KPagesIn16KBuild)
{
	// A partial or noncontiguous group cannot be mapped as a single 16K
	// kernel page. On a 4K kernel each guest page is independently eligible.
	const u32 offsets[] = {0xffffffffu, 0x5000, 0xb000, 0xffffffffu};
	EXPECT_TRUE(vtlbFastmem::IsCoalesced(offsets, 1, 0x1000, 0x1000));
	EXPECT_TRUE(vtlbFastmem::IsCoalesced(offsets, 2, 0x1000, 0x1000));
	EXPECT_FALSE(vtlbFastmem::IsCoalesced(offsets, 0, 0x1000, 0x1000));
	EXPECT_FALSE(vtlbFastmem::IsCoalesced(offsets, 1, 0x4000, 0x1000));
	const u32 contiguous[] = {0x8000, 0x9000, 0xa000, 0xb000};
	EXPECT_TRUE(vtlbFastmem::IsCoalesced(contiguous, 2, 0x4000, 0x1000));
	const u32 unaligned[] = {0x9000, 0xa000, 0xb000, 0xc000};
	EXPECT_FALSE(vtlbFastmem::IsCoalesced(unaligned, 2, 0x4000, 0x1000));
}

TEST(HostPageSize, AcceptsTheKernelPageThisBuildWasCompiledFor)
{
	EXPECT_TRUE(HostSys::IsRuntimePageSizeCompatible(__pagesize));
}

TEST(HostPageSize, AcceptsAKernelPageThatDividesTheCompiledOne)
{
	// The direction that makes one arm64 binary serve every device: compiled large, running
	// on a kernel whose pages are smaller.
	for (size_t runtime = 1024; runtime <= __pagesize; runtime *= 2)
	{
		SCOPED_TRACE(runtime);
		EXPECT_EQ(HostSys::IsRuntimePageSizeCompatible(runtime), (__pagesize % runtime) == 0);
	}
}

TEST(HostPageSize, RejectsAKernelPageLargerThanTheCompiledOne)
{
	// A build cannot protect a sub-range of the kernel's page, so this has to stay fatal.
	for (size_t runtime = static_cast<size_t>(__pagesize) * 2; runtime <= 1024 * 1024; runtime *= 2)
	{
		SCOPED_TRACE(runtime);
		EXPECT_FALSE(HostSys::IsRuntimePageSizeCompatible(runtime));
	}
}

TEST(HostPageSize, RejectsAPageSizeThatDoesNotDivideTheCompiledOne)
{
	// Not a real kernel's page size, but the predicate is divisibility, not magnitude - a
	// smaller-but-indivisible value must not slip through on size alone.
	EXPECT_FALSE(HostSys::IsRuntimePageSizeCompatible(3072));
}

TEST(HostPageSize, RejectsAFailedQuery)
{
	// GetRuntimePageSize answers 0 when it could not read one. Mapping blind is worse than
	// refusing.
	EXPECT_FALSE(HostSys::IsRuntimePageSizeCompatible(0));
}

TEST(HostPageSize, AgreesWithTheRunningKernelOnThisHost)
{
	// The check the emulator actually performs at startup, run against the real host: the
	// test binary is built with the same __pagesize as the emulator, so if this fails here
	// the shipped build would refuse to boot on this machine too.
	const size_t runtime = HostSys::GetRuntimePageSize();
	ASSERT_GT(runtime, 0u);
	EXPECT_TRUE(HostSys::IsRuntimePageSizeCompatible(runtime))
		<< "compiled for " << __pagesize << ", kernel reports " << runtime;
}

TEST(HostPageSize, LargeBuildTracksFourIndependentSmallKernelPages)
{
	const auto granularity = vtlbProtection::GetGranularity(4096, 16384);
	ASSERT_TRUE(granularity);
	EXPECT_EQ(granularity->size, 4096u);
	EXPECT_EQ(granularity->shift, 12u);
	for (u32 page = 0; page < 4; page++)
	{
		SCOPED_TRACE(page);
		EXPECT_EQ(granularity->Index(page * 4096 + 4095), page);
		EXPECT_EQ(granularity->Align(page * 4096 + 4095), page * 4096);
		EXPECT_TRUE(granularity->IsAligned(page * 4096));
	}
	// The last byte of RAM must fit the table sized for the finest granularity.
	constexpr u32 ram_size = 32 * 1024 * 1024;
	EXPECT_EQ(granularity->Index(ram_size - 1), (ram_size >> vtlbProtection::MIN_PAGE_SHIFT) - 1);
}

TEST(HostPageSize, MatchingKernelRetainsItsProtectionGranularity)
{
	for (const u32 size : {4096u, 16384u})
	{
		const auto granularity = vtlbProtection::GetGranularity(size, size);
		ASSERT_TRUE(granularity);
		EXPECT_EQ(granularity->size, size);
		EXPECT_EQ(granularity->Index(size - 1), 0u);
		EXPECT_EQ(granularity->Index(size), 1u);
		EXPECT_EQ(granularity->Align(size + 4095), size);
	}
}

TEST(HostPageSize, ProtectionRefusesUnknownOrUnsupportedKernelPages)
{
	for (const size_t runtime : {0u, 1024u, 3072u, 8193u, 32768u})
		EXPECT_FALSE(vtlbProtection::GetGranularity(runtime, 16384));
	EXPECT_FALSE(vtlbProtection::GetGranularity(16384, 4096));
}
