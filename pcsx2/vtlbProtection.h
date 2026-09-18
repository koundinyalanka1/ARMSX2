// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/HostSys.h"

#include <bit>
#include <optional>

namespace vtlbProtection
{
	// SMC tracks host protection units, independently of both the 4K guest TLB
	// and the compile-time mapping/reservation alignment.
	static constexpr u32 MIN_PAGE_SHIFT = 12;
	static constexpr u32 MIN_PAGE_SIZE = 1u << MIN_PAGE_SHIFT;

	struct Granularity
	{
		u32 size;
		u32 shift;

		constexpr u32 Align(u32 offset) const { return offset & ~(size - 1); }
		constexpr bool IsAligned(u32 offset) const { return Align(offset) == offset; }
		constexpr u32 Index(uptr offset) const { return static_cast<u32>(offset >> shift); }
	};

	// The explicit build size also lets tests exercise both kernel sizes on any host.
	constexpr std::optional<Granularity> GetGranularity(size_t runtime_size, size_t build_size = __pagesize)
	{
		if (runtime_size < MIN_PAGE_SIZE || !std::has_single_bit(runtime_size) ||
			build_size == 0 || (build_size % runtime_size) != 0)
		{
			return std::nullopt;
		}

		return Granularity{static_cast<u32>(runtime_size), static_cast<u32>(std::countr_zero(runtime_size))};
	}
} // namespace vtlbProtection
