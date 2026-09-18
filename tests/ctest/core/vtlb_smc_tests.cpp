// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Config.h"
#include "Memory.h"
#include "R5900.h"
#include "vtlb.h"

#include <gtest/gtest.h>

namespace
{
	u32 clear_address;
	u32 clear_words;
	u32 clear_calls;

	class VtlbSmc : public testing::Test
	{
	protected:
		void SetUp() override
		{
			page_size = static_cast<u32>(HostSys::GetRuntimePageSize());
			ASSERT_TRUE(HostSys::IsRuntimePageSizeCompatible(page_size));
			saved_fastmem = EmuConfig.Cpu.Recompiler.EnableFastmem;
			EmuConfig.Cpu.Recompiler.EnableFastmem = true;
			ASSERT_TRUE(SysMemory::Allocate());
			vtlb_Init();
			vtlb_MapBlock(eeMem->Main, 0, Ps2MemSize::ExposedRam);
			mmap_ResetBlockTracking();
			saved_cpu = Cpu;
			cpu = intCpu;
			cpu.Clear = [](u32 address, u32 words) {
				clear_address = address;
				clear_words = words;
				clear_calls++;
			};
			Cpu = &cpu;
			clear_calls = 0;
		}

		void TearDown() override
		{
			if (SysMemory::IsAllocated())
			{
				mmap_ResetBlockTracking();
				vtlb_Shutdown();
				SysMemory::Release();
			}
			Cpu = saved_cpu;
			EmuConfig.Cpu.Recompiler.EnableFastmem = saved_fastmem;
		}

		u32 page_size = 0;
		bool saved_fastmem = false;
		R5900cpu* saved_cpu = nullptr;
		R5900cpu cpu{};
	};
} // namespace

TEST_F(VtlbSmc, FaultInvalidatesOnlyOneRuntimePageAndCanReprotect)
{
	for (u32 offset = 0; offset < 2 * __pagesize; offset += page_size)
		mmap_MarkCountedRamPage(offset);

	const u32 fault_offset = page_size;
	using Result = PageFaultHandler::HandlerResult;
	EXPECT_EQ(PageFaultHandler::HandlePageFault(nullptr, eeMem->Main + fault_offset + 4, true), Result::ContinueExecution);
	EXPECT_EQ(clear_calls, 1u);
	EXPECT_EQ(clear_address, fault_offset);
	EXPECT_EQ(clear_words * sizeof(u32), page_size);
	for (u32 offset = 0; offset < 2 * __pagesize; offset += page_size)
		EXPECT_EQ(mmap_GetRamPageInfo(offset), offset == fault_offset ? ProtMode_Manual : ProtMode_Write);

	// Already-manual faults are not SMC; they must not hit the invariant again.
	EXPECT_EQ(PageFaultHandler::HandlePageFault(nullptr, eeMem->Main + fault_offset, true), Result::ExecuteNextHandler);
	EXPECT_EQ(clear_calls, 1u);
	mmap_MarkCountedRamPage(fault_offset);
	EXPECT_EQ(mmap_GetRamPageInfo(fault_offset), ProtMode_Write);
	*reinterpret_cast<volatile u32*>(eeMem->Main + fault_offset) = 0x12345678;
	EXPECT_EQ(clear_calls, 2u);
	EXPECT_EQ(mmap_GetRamPageInfo(fault_offset), ProtMode_Manual);
}

TEST_F(VtlbSmc, AliasesPreserveMixedPermissionsAfterMappingAndRemapping)
{
	ASSERT_NE(vtlb_private::vtlbdata.fastmem_base, 0u);
	constexpr u32 alias = 0x80000000;
	constexpr u32 second_alias = 0xa0000000;
	constexpr u32 span = 2 * __pagesize;
	vtlb_VMap(alias, 0, span);
	// Keep the final runtime page writable when completing each mapping. The
	// earlier protected sub-pages must not inherit that last page's mode.
	mmap_MarkCountedRamPage(0);
	mmap_MarkCountedRamPage(__pagesize);
	vtlb_VMap(second_alias, 0, span);

	auto store = [](u32 address) {
		*reinterpret_cast<volatile u32*>(vtlb_private::vtlbdata.fastmem_base + address) = 0xdeadbeef;
	};
	store(second_alias);
	EXPECT_EQ(clear_calls, 1u);
	EXPECT_EQ(clear_address, 0u);
	EXPECT_EQ(clear_words * sizeof(u32), page_size);
	store(alias); // Every alias of the now-manual runtime page is writable.
	EXPECT_EQ(clear_calls, 1u);
	EXPECT_EQ(mmap_GetRamPageInfo(__pagesize), ProtMode_Write);

	// Removing a guest 4K page unmaps the compile-time mapping. Re-coalescing
	// must restore all runtime permissions, including protected siblings.
	vtlb_VMapUnmap(second_alias + __pagesize + vtlb_private::VTLB_PAGE_SIZE, vtlb_private::VTLB_PAGE_SIZE);
	vtlb_VMap(second_alias + __pagesize + vtlb_private::VTLB_PAGE_SIZE, __pagesize + vtlb_private::VTLB_PAGE_SIZE, vtlb_private::VTLB_PAGE_SIZE);
	store(second_alias + __pagesize);
	EXPECT_EQ(clear_calls, 2u);
	EXPECT_EQ(clear_address, __pagesize);
	EXPECT_EQ(clear_words * sizeof(u32), page_size);

	mmap_ResetBlockTracking();
	for (u32 offset = 0; offset < span; offset += page_size)
	{
		store(alias + offset);
		store(second_alias + offset);
		EXPECT_EQ(mmap_GetRamPageInfo(offset), ProtMode_None);
	}
	EXPECT_EQ(clear_calls, 2u);
}

TEST_F(VtlbSmc, ReplacingMappingRemovesTheOldProtectionAlias)
{
	ASSERT_NE(vtlb_private::vtlbdata.fastmem_base, 0u);
	constexpr u32 alias = 0x80000000;
	vtlb_VMap(alias, 0, __pagesize);
	vtlb_VMap(alias, __pagesize, __pagesize);
	mmap_MarkCountedRamPage(0);
	// Protecting the old physical page must neither issue redundant mprotects
	// on this alias nor accidentally make its replacement read-only.
	*reinterpret_cast<volatile u32*>(vtlb_private::vtlbdata.fastmem_base + alias) = 0x12345678;
	EXPECT_EQ(clear_calls, 0u);
	EXPECT_EQ(mmap_GetRamPageInfo(0), ProtMode_Write);
	EXPECT_EQ(*reinterpret_cast<u32*>(eeMem->Main + __pagesize), 0x12345678u);
}

TEST_F(VtlbSmc, EveryRuntimeSubpageProtectsAllAliasesIndependently)
{
	ASSERT_NE(vtlb_private::vtlbdata.fastmem_base, 0u);
	constexpr u32 alias = 0x80000000;
	constexpr u32 second_alias = 0xa0000000;
	vtlb_VMap(alias, 0, __pagesize);
	vtlb_VMap(second_alias, 0, __pagesize);
	for (u32 offset = 0; offset < __pagesize; offset += page_size)
		mmap_MarkCountedRamPage(offset);

	for (u32 offset = 0; offset < __pagesize; offset += page_size)
	{
		SCOPED_TRACE(offset);
		*reinterpret_cast<volatile u32*>(vtlb_private::vtlbdata.fastmem_base + alias + offset) = offset;
		EXPECT_EQ(clear_calls, offset / page_size + 1);
		EXPECT_EQ(clear_address, offset);
		EXPECT_EQ(clear_words * sizeof(u32), page_size);
		*reinterpret_cast<volatile u32*>(vtlb_private::vtlbdata.fastmem_base + second_alias + offset) = offset;
		EXPECT_EQ(clear_calls, offset / page_size + 1);
		for (u32 next = offset + page_size; next < __pagesize; next += page_size)
			EXPECT_EQ(mmap_GetRamPageInfo(next), ProtMode_Write);
	}
}
