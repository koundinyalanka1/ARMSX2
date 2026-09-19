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
// The SBUS tests also check the RELOAD half of the pair: a real write to
// HW_ICFG reaches hwIntcIrq(INTC_SBUS) -> cpuTestINTCInts, which clears the IOP's
// remaining budget while the EE event test is active. Recreate that scheduling
// state around the standalone harness; no replacement handler or JIT hook is
// needed. PSX_INT -> cpuSetNextEventDelta only schedules an EE event and does
// not itself shorten iopCycleEE.
//
// Every run here is budget-bounded rather than program-bounded — the harness
// hands ExecuteBlock a fixed EE-cycle budget and the JIT runs until iopCycleEE
// is exhausted — so "how many IOP cycles did we get through" is a direct
// readout of whether the accounting stayed intact across the seam.

#include "harness/JitTestHarness.h"

#include "Hw.h"
#include "IopHw.h"
#include "Memory.h"
#include "R5900.h"
#include "Dmac.h"

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

// JitTestHarness restores the IOP register file between runs, but these tests
// also touch EE interrupt state and IOP hardware. Keep those changes local.
class ScopedSbusInterruptState
{
public:
	ScopedSbusInterruptState(bool event_test_active, bool masked, bool interrupts_enabled)
		: saved_cpu_regs_(cpuRegs)
		, saved_event_test_active_(eeEventTestIsActive)
		, saved_intc_stat_(psHu32(INTC_STAT))
		, saved_intc_mask_(psHu32(INTC_MASK))
		, saved_icfg_(psxHu32(HW_ICFG))
	{
		cpuRegs.CP0.n.Status.val = interrupts_enabled ? 0x10401u : 0u; // EIE, INTC, IE
		cpuRegs.cycle = 1000;
		cpuRegs.nextEventCycle = cpuRegs.cycle + 100;
		eeEventTestIsActive = event_test_active;
		psHu32(INTC_STAT) = 0;
		psHu32(INTC_MASK) = masked ? 0u : (1u << INTC_SBUS);
		psxHu32(HW_ICFG) = 0; // PS2's 1:8 clock ratio, no PS1-mode transition
	}

	~ScopedSbusInterruptState()
	{
		cpuRegs = saved_cpu_regs_;
		eeEventTestIsActive = saved_event_test_active_;
		psHu32(INTC_STAT) = saved_intc_stat_;
		psHu32(INTC_MASK) = saved_intc_mask_;
		psxHu32(HW_ICFG) = saved_icfg_;
	}

private:
	cpuRegisters saved_cpu_regs_;
	bool saved_event_test_active_;
	u32 saved_intc_stat_;
	u32 saved_intc_mask_;
	u32 saved_icfg_;
};

void RunSbusWrite(bool event_test_active, bool masked, bool interrupts_enabled, bool delay_slot)
{
	for (const bool halfword : {false, true})
	{
		SCOPED_TRACE(halfword ? "SH" : "SW");
		ScopedSbusInterruptState interrupt_state(event_test_active, masked, interrupts_enabled);
		JitTestHarness h;
		h.SetGpr(reg::a0, HW_ICFG);
		h.SetGpr(reg::v1, 1u << 1);
		// Avoid unrelated IOP events. The production SBUS handler affects EE
		// INTC only; no EE execution is needed to observe its budget change.
		psxRegs.iopNextEventCycle = 0x100000;
		const u32 store = halfword ? SH(reg::v1, 0, reg::a0) : SW(reg::v1, 0, reg::a0);
		if (delay_slot)
		{
			h.LoadProgramNoTerm({
				ADDIU(reg::v0, reg::zero, 0x11), BEQ(reg::zero, reg::zero, 1), NOP,
				ADDIU(reg::v0, reg::v0, 1), JR(reg::ra), store,
			});
		}
		else
		{
			h.LoadProgramNoTerm({
				ADDIU(reg::v0, reg::zero, 0x11), BEQ(reg::zero, reg::zero, 1), NOP,
				store, ADDIU(reg::v0, reg::v0, 1), JR(reg::ra), NOP,
			});
		}
		h.Run();

		EXPECT_EQ(h.GetGprJit(reg::v0), 0x12u);
		EXPECT_EQ(psHu32(INTC_STAT), 1u << INTC_SBUS);
		EXPECT_EQ(psxHu32(HW_ICFG), 1u << 1);
		const auto& jit = h.JitSnapshot().regs;
		const auto& interp = h.InterpSnapshot().regs;
		if (event_test_active && !masked && interrupts_enabled)
		{
			// The first three instructions consumed 24 EE cycles. The helper
			// returns the remaining budget through iopBreak, and execution
			// stops at this block's branch rather than entering the parking
			// loop. A stale resident budget would keep executing that loop.
			const u64 instruction_count = delay_slot ? 6u : 7u;
			EXPECT_EQ(jit.cycle, instruction_count);
			EXPECT_EQ(jit.cycle, interp.cycle);
			EXPECT_GT(jit.iopBreak, 0);
			EXPECT_EQ(jit.iopBreak, interp.iopBreak);
			EXPECT_EQ(jit.iopCycleEE, -static_cast<s32>((instruction_count - 3) * 8));
			EXPECT_EQ(jit.iopCycleEE, interp.iopCycleEE);
			EXPECT_EQ(cpuRegs.nextEventCycle, cpuRegs.cycle + 4);
		}
		else
		{
			// Pending SBUS alone must not steal a timeslice when the EE event
			// test is inactive, the line is masked, or CP0 disables interrupts.
			EXPECT_EQ(jit.iopBreak, 0);
			EXPECT_EQ(interp.iopBreak, 0);
			ExpectCycleAccountingIntact(h);
			if (masked || !interrupts_enabled)
				EXPECT_EQ(cpuRegs.nextEventCycle, cpuRegs.cycle + 100);
			else
				EXPECT_EQ(cpuRegs.nextEventCycle, cpuRegs.cycle + 4);
		}
	}
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

TEST(IopCycleResidency, ReloadsBudgetShortenedBySbusStore)
{
	RunSbusWrite(true, false, true, false);
}

TEST(IopCycleResidency, ReloadsBudgetShortenedByDelaySlotSbusStore)
{
	RunSbusWrite(true, false, true, true);
}

TEST(IopCycleResidency, SbusOutsideEeEventTestPreservesBudget)
{
	RunSbusWrite(false, false, true, false);
}

TEST(IopCycleResidency, MaskedSbusPreservesBudget)
{
	RunSbusWrite(true, true, true, false);
}

TEST(IopCycleResidency, SbusWithDisabledEeInterruptsPreservesBudget)
{
	RunSbusWrite(true, false, false, false);
}
