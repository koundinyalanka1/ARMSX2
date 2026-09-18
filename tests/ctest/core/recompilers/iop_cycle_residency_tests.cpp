// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The IOP's two cycle counters live in callee-saved registers for the whole JIT
// session — RPSXCYCLE mirrors psxRegs.cycle, RPSXEECYCLE mirrors
// psxRegs.iopCycleEE (see the contract in arm64/iR3000A-arm64.h). Block tails
// update and test them without touching memory; every seam that reaches C
// publishes them first and reads them back after.
//
// The harness's ordinary JIT-vs-interp diff deliberately ignores both fields
// (StateSnapshot.h), so nothing else in the suite would notice if they were
// mistracked. These do three things nothing else does:
//
//   * Check the counters are PUBLISHED at all. A missing flush on the exit path
//     leaves psxRegs.cycle at its pre-run value and the VM's whole timeslice
//     accounting stops advancing.
//   * Check they are published ACCURATELY, by pinning the JIT's count against
//     the interpreter's for the same program. A flush/reload pair that is
//     missing its reload leaves a stale register that then overwrites whatever C
//     just wrote, and the two counts drift apart.
//   * Cover each distinct C seam separately: the MMIO paths inside the two
//     fast-path stubs, the SMC-clear path inside the store stub, and an
//     interpreter fallback (the GTE ops are the IOP's only REC_FUNC users).
//
// One gap worth naming, because a mutation test finds it: deleting the RELOAD
// half of the stubs' pair does not fail anything here. It is still required.
// An IOP MMIO write can reach PSX_INT -> cpuSetNextEventDelta -> cpuTestINTCInts
// (R5900.cpp), which sets psxRegs.iopCycleEE = 0 to cut the IOP's timeslice
// short — but only while eeEventTestIsActive, i.e. when the EE is driving the
// IOP through cpuEventTest. This harness runs the IOP standalone, so that flag
// is never set and the value C writes always matches the one already in the
// register. Reproducing it needs an EE-driven run, which is a different harness.
//
// Every run here is budget-bounded rather than program-bounded — the harness
// hands ExecuteBlock a fixed EE-cycle budget and the JIT runs until iopCycleEE
// is exhausted — so "how many IOP cycles did we get through" is a direct
// readout of whether the accounting stayed intact across the seam.

#include "harness/JitTestHarness.h"

#include <gtest/gtest.h>

#include <cstdlib>

using namespace recompiler_tests;
using namespace mips;

namespace {
constexpr u32 kPhysBase = RecompilerTestEnvironment::kScratchAddr;
// Helper-path addresses. kHwReadAddr is INTC I_CTRL, which is safe to READ once
// (it reads as zero here and clears on read). Stores go to kHwInertAddr instead:
// (mem & 0xf000) == 0x2000 lands in iopMemWrite32's default branch, a plain
// psxHu32 write into the zero-filled iopHw array, so nothing this file does
// leaves live hardware state behind for the rest of the suite.
constexpr u32 kHwReadAddr = 0x1F801078u;
constexpr u32 kHwInertAddr = 0x1F802000u;

// COP2 (GTE) move-from, the cheapest instruction that routes through
// REC_GTE_FUNC's _psxFlushCall + C call.
constexpr u32 MFC2(u32 rt, u32 rd) { return RType(0x12, 0, rt, rd, 0, 0); }

// The interpreter reaches its budget check only on a taken branch, so it can
// overshoot the JIT by one block's worth of cycles. Anything beyond that is
// accounting drift, not scheduling granularity.
void ExpectCycleAccountingIntact(JitTestHarness& h)
{
	const u64 jit = h.JitSnapshot().regs.cycle;
	const u64 interp = h.InterpSnapshot().regs.cycle;
	EXPECT_GT(jit, 0u) << "psxRegs.cycle was never published out of RPSXCYCLE";
	EXPECT_LE(std::llabs(static_cast<long long>(jit) - static_cast<long long>(interp)), 8)
		<< "JIT cycle " << jit << " vs interp " << interp;
	EXPECT_LE(h.JitSnapshot().regs.iopCycleEE, 0)
		<< "the EE timeslice was not consumed, or iopCycleEE was never published "
		   "out of RPSXEECYCLE";
}
} // namespace

TEST(IopCycleResidency, StraightLineBlockPublishesCounters)
{
	JitTestHarness h;
	h.SetGpr(reg::a0, kPhysBase);
	h.WriteU32(kPhysBase, 7);
	h.LoadProgram({LW(reg::v0, 0, reg::a0), ADDIU(reg::v1, reg::v0, 1)});
	h.Run();
	ExpectCycleAccountingIntact(h);
}

TEST(IopCycleResidency, BranchChainPublishesCounters)
{
	// Several block tails in a row — each one updates both counters purely in
	// registers, so this is the shape the change is actually for.
	JitTestHarness h;
	h.SetGpr(reg::a0, 3);
	h.LoadProgramNoTerm({
		BNE(reg::a0, reg::zero, 2),
		NOP,
		ADDIU(reg::v0, reg::zero, 1),
		NOP,
		ADDIU(reg::v1, reg::zero, 2),
		JR(reg::ra),
		NOP,
	});
	h.Run();
	ExpectCycleAccountingIntact(h);
}

TEST(IopCycleResidency, SurvivesLoadStubHelperPath)
{
	// Load stub → iopMemRead32. The stub's save/restore carries the cycle
	// flush/reload, because an MMIO handler can reach iopBranchTest.
	JitTestHarness h;
	h.SetGpr(reg::a0, kHwReadAddr);
	h.LoadProgram({LW(reg::v0, 0, reg::a0)});
	h.Run();
	ExpectCycleAccountingIntact(h);
}

TEST(IopCycleResidency, SurvivesStoreStubHelperPath)
{
	JitTestHarness h;
	h.SetGpr(reg::a0, kHwInertAddr);
	h.SetGpr(reg::v1, 0x55u);
	h.LoadProgram({SW(reg::v1, 0, reg::a0)});
	h.Run();
	ExpectCycleAccountingIntact(h);
}

TEST(IopCycleResidency, SurvivesStoreStubSmcClearPath)
{
	// Third C path inside the store stub: RAM store into a granule that holds
	// compiled code, so iopStoreClearHit runs.
	JitTestHarness h;
	h.SetGpr(reg::a0, RecompilerTestEnvironment::kProgramPc + 0xF0);
	h.SetGpr(reg::v1, 0x1u);
	h.LoadProgram({SW(reg::v1, 0, reg::a0)});
	h.Run();
	ExpectCycleAccountingIntact(h);
}

TEST(IopCycleResidency, SurvivesInterpreterFallback)
{
	JitTestHarness h;
	h.LoadProgram({MFC2(reg::v0, 0)});
	h.Run();
	ExpectCycleAccountingIntact(h);
}
