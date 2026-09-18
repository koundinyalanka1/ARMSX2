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

#include <gtest/gtest.h>

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
