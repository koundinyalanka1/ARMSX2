// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Coverage expansion for the IOP load/store JIT path. Basic width /
// sign-extension coverage lives in iop_loadstore_tests.cpp; this file picks up
// what that file doesn't:
//
//   * RAM mirror aliasing (physical 0x00000000 / kseg0 0x80000000 / kseg1
//     0xa0000000 all land in the same 2MB iopMem->Main through the fast
//     path's 21-bit mask).
//   * Helper-path dispatch — any effective address with bit 28 set bypasses
//     the fast path and calls iopMemRead*/iopMemWrite* directly.
//   * Unaligned load/store (LWL/LWR/SWL/SWR) via the REC_FUNC interpreter
//     fallback — a different JIT code path from aligned loads/stores.
//   * `lw $0, ...` short-circuit (the _Rt_==0 branch in rpsxLW).

#include "harness/JitTestHarness.h"

#include "MemoryTypes.h"

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;

namespace {
constexpr u32 kPhysBase = RecompilerTestEnvironment::kScratchAddr;  // 0x00020000
constexpr u32 kKseg0Base = 0x80000000u | kPhysBase;                 // 0x80020000
constexpr u32 kKseg1Base = 0xA0000000u | kPhysBase;                 // 0xA0020000

// A bit-28-set address that routes through the helper path. Chosen in the
// IOP hardware register window (0x1F801xxx); iopHw is zero-filled by the
// test env, so reads from unassigned registers return whatever the helper
// dispatches to. No values are asserted — the implicit JIT-vs-interp diff
// in Run() locks the two paths to the same result.
constexpr u32 kHwHelperAddr = 0x1F801078u;

// A helper-path address with NO side effects, for the tests below that need to
// both write and read one. iopMemRead32/iopMemWrite32 switch on (mem & 0xf000)
// and send 0x2000 to the default branch, which is a plain psxHu32 access into
// the zero-filled iopHw array. kHwHelperAddr is deliberately not that: 0x1F801078
// is INTC I_CTRL, which clears on read, so a JIT pass and an interpreter pass
// over the same load cannot agree once it holds a nonzero value.
constexpr u32 kHwInertAddr = 0x1F802000u;

// Seeded at kPhysBase by the routing tests below. Any non-RAM address they use
// is picked to fold onto kPhysBase under the load fast path's mask, so "the
// stub served this from RAM" shows up as this exact value rather than as a
// coincidental zero.
constexpr u32 kRamSentinel = 0xA5A5C3C3u;
} // namespace

// ---------------------------------------------------------------------------
// RAM mirror aliasing — physical / kseg0 / kseg1 all address the same byte.
// ---------------------------------------------------------------------------

TEST(IopMemoryAccess, LwThroughKseg0Mirror)
{
	JitTestHarness h;
	h.WriteU32(kPhysBase, 0xABCD1234u);
	h.SetGpr(reg::a0, kKseg0Base);
	h.LoadProgram({LW(reg::v0, 0, reg::a0)});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0xABCD1234u);
}

TEST(IopMemoryAccess, LwThroughKseg1Mirror)
{
	JitTestHarness h;
	h.WriteU32(kPhysBase, 0xFEEDFACEu);
	h.SetGpr(reg::a0, kKseg1Base);
	h.LoadProgram({LW(reg::v0, 0, reg::a0)});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0xFEEDFACEu);
}

TEST(IopMemoryAccess, SwViaKseg0ReadableViaPhysical)
{
	JitTestHarness h;
	h.TrackMemWindow(kPhysBase, 4);
	h.SetGpr(reg::a0, kKseg0Base);
	h.SetGpr(reg::a1, 0xDEADBEEFu);
	h.LoadProgram({SW(reg::a1, 0, reg::a0)});
	h.Run();
	EXPECT_EQ(h.ReadU32(kPhysBase), 0xDEADBEEFu);
}

TEST(IopMemoryAccess, SwViaKseg1ReadableViaPhysical)
{
	JitTestHarness h;
	h.TrackMemWindow(kPhysBase, 4);
	h.SetGpr(reg::a0, kKseg1Base);
	h.SetGpr(reg::a1, 0x55AA55AAu);
	h.LoadProgram({SW(reg::a1, 0, reg::a0)});
	h.Run();
	EXPECT_EQ(h.ReadU32(kPhysBase), 0x55AA55AAu);
}

TEST(IopMemoryAccess, LbuThroughKseg1Mirror)
{
	// Byte load through the uncached-mirror address.
	JitTestHarness h;
	h.WriteU8(kPhysBase, 0x42u);
	h.SetGpr(reg::a0, kKseg1Base);
	h.LoadProgram({LBU(reg::v0, 0, reg::a0)});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0x42u);
}

TEST(IopMemoryAccess, LhuThroughKseg0Mirror)
{
	JitTestHarness h;
	h.WriteU16(kPhysBase, 0xBEEFu);
	h.SetGpr(reg::a0, kKseg0Base);
	h.LoadProgram({LHU(reg::v0, 0, reg::a0)});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0xBEEFu);
}

TEST(IopMemoryAccess, MirroredWritesAliasSamePhysicalWord)
{
	// Sequence that bounces through all three mirrors.
	//   SW  $a1, 0($a0)   ; a0 = phys, a1 = 0x11111111 → phys[0] = 0x11111111
	//   LW  $v0, 0($a2)   ; a2 = kseg0, should see 0x11111111
	//   SW  $a1, 0($a3)   ; a3 = kseg1, overwrite with 0x22222222
	//   LW  $v1, 0($a0)   ; phys, should see 0x22222222
	JitTestHarness h;
	h.TrackMemWindow(kPhysBase, 4);
	h.SetGpr(reg::a0, kPhysBase);
	h.SetGpr(reg::a1, 0x11111111u);
	h.SetGpr(reg::a2, kKseg0Base);
	h.SetGpr(reg::a3, kKseg1Base);
	h.LoadProgram({
		SW(reg::a1, 0, reg::a0),
		LW(reg::v0, 0, reg::a2),
		ORI(reg::a1, reg::zero, 0x2222u),        // a1 = 0x00002222
		LUI(reg::t0, 0x2222),                    // t0 = 0x22220000
		ADDU(reg::a1, reg::a1, reg::t0),         // a1 = 0x22222222
		SW(reg::a1, 0, reg::a3),
		LW(reg::v1, 0, reg::a0),
	});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0x11111111u);
	EXPECT_EQ(h.GetGprInterp(reg::v1), 0x22222222u);
	EXPECT_EQ(h.ReadU32(kPhysBase), 0x22222222u);
}

// ---------------------------------------------------------------------------
// Helper-path dispatch — bit 28 of the effective address routes to the
// iopMemRead*/iopMemWrite* C helpers instead of the RAM fast path.
// ---------------------------------------------------------------------------

TEST(IopMemoryAccess, LwViaHelperPathMatchesInterp)
{
	// Read from an IOP HW register window address. No concrete assertion —
	// the test relies on the harness's implicit JIT-vs-interp diff in Run() to lock
	// both paths to the same value. If the JIT's helper call emits a
	// different ABI or clobbers a reg the interp didn't, the diff surfaces.
	JitTestHarness h;
	h.SetGpr(reg::a0, kHwHelperAddr);
	h.LoadProgram({LW(reg::v0, 0, reg::a0)});
	h.Run();
	// Lock the observed value as spec (whatever interp returned).
	EXPECT_EQ(h.GetGprJit(reg::v0), h.GetGprInterp(reg::v0));
}

TEST(IopMemoryAccess, LbuViaHelperPathMatchesInterp)
{
	JitTestHarness h;
	h.SetGpr(reg::a0, kHwHelperAddr);
	h.LoadProgram({LBU(reg::v0, 0, reg::a0)});
	h.Run();
	EXPECT_EQ(h.GetGprJit(reg::v0), h.GetGprInterp(reg::v0));
}

// ---------------------------------------------------------------------------
// _Rt_==0 short-circuit in rpsxLoad. The fast path body early-exits
// without writing a result, but the helper path still runs so device
// side-effects fire.
// ---------------------------------------------------------------------------

TEST(IopMemoryAccess, LoadIntoZeroDoesNotDisturbR0)
{
	// r0 is hardwired zero and must stay that way even when named as the
	// destination of a load. Both JIT and interp uphold this; the diff
	// locks them together, and the direct check pins the architectural
	// guarantee.
	JitTestHarness h;
	h.WriteU32(kPhysBase, 0xAAAAAAAAu);
	h.SetGpr(reg::a0, kPhysBase);
	h.LoadProgram({LW(reg::zero, 0, reg::a0)});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::zero), 0u);
	EXPECT_EQ(h.GetGprJit(reg::zero), 0u);
}

// ---------------------------------------------------------------------------
// LWL/LWR/SWL/SWR — REC_FUNC interpreter fallback. These spill the opcode,
// flush live regs, and dispatch to the interpreter. Different JIT code path
// from aligned loads/stores; worth smoke-testing.
// ---------------------------------------------------------------------------

TEST(IopMemoryAccess, LwlLwrAssembleAlignedWord)
{
	// `lwl rt, 3(base); lwr rt, 0(base)` — the canonical unaligned-load
	// pattern, here used with naturally-aligned base = 0x20000. Expected
	// result: the word at 0x20000 (little-endian).
	JitTestHarness h;
	h.WriteU32(kPhysBase, 0x44332211u);
	h.SetGpr(reg::a0, kPhysBase);
	h.LoadProgram({
		LWL(reg::v0, 3, reg::a0),
		LWR(reg::v0, 0, reg::a0),
	});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0x44332211u);
}

TEST(IopMemoryAccess, LwlLwrAssembleUnalignedWord)
{
	// Load 4 bytes starting at an unaligned boundary (offset 1). Seed two
	// adjacent words so the unaligned read crosses the boundary. Lock
	// whatever the interpreter produces as spec.
	JitTestHarness h;
	h.WriteU32(kPhysBase + 0, 0x44332211u);
	h.WriteU32(kPhysBase + 4, 0x88776655u);
	h.SetGpr(reg::a0, kPhysBase);
	h.LoadProgram({
		// lwl rt, 4(a0)  -> loads high bytes from word at 0x20004
		// lwr rt, 1(a0)  -> loads low bytes starting at 0x20001
		LWL(reg::v0, 4, reg::a0),
		LWR(reg::v0, 1, reg::a0),
	});
	h.Run();
	// Don't pin a concrete value; surface only if JIT diverges from interp.
	EXPECT_EQ(h.GetGprJit(reg::v0), h.GetGprInterp(reg::v0));
}

TEST(IopMemoryAccess, SwlSwrWriteAlignedWord)
{
	// Mirror of the LWL/LWR round-trip for stores.
	JitTestHarness h;
	h.WriteU32(kPhysBase, 0u);
	h.TrackMemWindow(kPhysBase, 4);
	h.SetGpr(reg::a0, kPhysBase);
	h.SetGpr(reg::a1, 0x44332211u);
	h.LoadProgram({
		SWL(reg::a1, 3, reg::a0),
		SWR(reg::a1, 0, reg::a0),
	});
	h.Run();
	EXPECT_EQ(h.ReadU32(kPhysBase), 0x44332211u);
}

TEST(IopMemoryAccess, LwlAfterSwSeesFlushedData)
{
	// Sequence: pre-state word → SW overwrites it → LWL+LWR reads it back.
	// Exercises the flush between rec opcodes: the SW leaves its value in
	// memory (helper-dispatched), and the subsequent LWL — which falls back to
	// the interpreter after flushing all live guest registers to memory — must
	// see the updated word.
	JitTestHarness h;
	h.WriteU32(kPhysBase, 0xDEADDEADu);
	h.TrackMemWindow(kPhysBase, 4);
	h.SetGpr(reg::a0, kPhysBase);
	h.SetGpr(reg::a1, 0xCAFEF00Du);
	h.LoadProgram({
		SW(reg::a1, 0, reg::a0),
		LWL(reg::v0, 3, reg::a0),
		LWR(reg::v0, 0, reg::a0),
	});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0xCAFEF00Du);
}

// ---------------------------------------------------------------------------
// Flush semantics under pressure — a helper-dispatched store must flush
// all live guest regs so the post-helper state is coherent. Existing ALU
// tests don't stress this because none follow an SW with a read-from-
// register sequence long enough to see a bad flush.
// ---------------------------------------------------------------------------

TEST(IopMemoryAccess, ManyLiveRegsSurviveStoreFlush)
{
	// Load several regs, issue an SW (which calls _psxFlushCall), then
	// consume those regs afterward. A botched flush would show as one of
	// the post-SW reads returning a stale value.
	JitTestHarness h;
	h.SetGpr(reg::a0, kPhysBase);
	h.SetGpr(reg::t0, 0x11111111u);
	h.SetGpr(reg::t1, 0x22222222u);
	h.SetGpr(reg::t2, 0x33333333u);
	h.SetGpr(reg::t3, 0x44444444u);
	h.LoadProgram({
		SW(reg::t0, 0, reg::a0),                // flush happens here
		ADDU(reg::v0, reg::t1, reg::t2),        // uses t1+t2 post-flush
		ADDU(reg::v1, reg::t2, reg::t3),        // uses t2+t3 post-flush
	});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0x55555555u);
	EXPECT_EQ(h.GetGprInterp(reg::v1), 0x77777777u);
	EXPECT_EQ(h.ReadU32(kPhysBase), 0x11111111u);
}

// ---------------------------------------------------------------------------
// Load routing — which side of `(addr & 0x1f800000) == 0` an address lands on.
//
// The RAM load fast path (g_iopLoadStub, iR3000A-arm64.cpp) reads
// iopMem->Main[addr & (ExposedIopRam-1)] directly whenever that test passes,
// and tail-jumps to iopMemRead* when it doesn't. These pin the two things that
// makes sound: the mirror fold inside the window, and the fact that everything
// the read LUT maps but the write LUT does NOT — ROM and SIF, where a store
// stub's reasoning would not transfer — sits outside it. Every case here is
// also covered by Run()'s implicit JIT-vs-interp diff, so a mis-routed address
// fails even where the expected value is 0.
// ---------------------------------------------------------------------------

TEST(IopMemoryAccess, LwThroughRamWindowMirrorFoldsToSamePhysicalWord)
{
	// Pages 0x00-0x7f all map to Main through `(page & mask) << 16`, so with
	// 2MB exposed, page 0x22 is page 0x02's mirror *inside* the RAM window —
	// the fold the kseg mirror tests above never reach, because kseg only
	// changes bits the 21-bit mask discards anyway.
	const u32 mirror = kPhysBase + Ps2MemSize::ExposedIopRam;
	if (mirror >= 0x00800000u)
		GTEST_SKIP() << "extra IOP RAM exposed; no in-window mirror below page 0x80";

	JitTestHarness h;
	h.WriteU32(kPhysBase, 0x0FF1CE55u);
	h.SetGpr(reg::a0, mirror);
	h.LoadProgram({LW(reg::v0, 0, reg::a0)});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0x0FF1CE55u);
}

TEST(IopMemoryAccess, LwJustPastRamWindowTakesHelperPath)
{
	// Page 0x80 is the first page iopMemReset does not map, so iopMemRead32
	// finds a null RLUT entry and returns 0. The address is chosen to fold
	// onto kPhysBase under the fast path's mask, and kPhysBase is seeded with
	// a sentinel — so a mis-routed load returns the sentinel instead of 0 and
	// this fails, rather than passing on a coincidence of two zeroes.
	const u32 unmapped = 0x00800000u + kPhysBase;
	ASSERT_EQ(unmapped & (Ps2MemSize::ExposedIopRam - 1), kPhysBase)
		<< "test address no longer aliases the sentinel";

	JitTestHarness h;
	h.WriteU32(kPhysBase, kRamSentinel);
	h.SetGpr(reg::a0, unmapped);
	h.LoadProgram({LW(reg::v0, 0, reg::a0)});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0u);
	EXPECT_NE(h.GetGprJit(reg::v0), kRamSentinel) << "load was served from RAM";
}

TEST(IopMemoryAccess, LwFromRomWindowTakesHelperPath)
{
	// BIOS ROM: mapped in RLUT, absent from WLUT. The store stub never has to
	// reason about it; the load stub does, and the answer is that 0x1fc0xxxx
	// has bits 23-28 set, so it routes to C like any other non-RAM target.
	// Same sentinel-aliasing trick as above — the address folds onto kPhysBase,
	// so serving this from RAM is visible even with no BIOS image loaded.
	const u32 rom = 0x1FC00000u + kPhysBase;
	ASSERT_EQ(rom & (Ps2MemSize::ExposedIopRam - 1), kPhysBase)
		<< "test address no longer aliases the sentinel";

	JitTestHarness h;
	h.WriteU32(kPhysBase, kRamSentinel);
	h.SetGpr(reg::a0, rom);
	h.LoadProgram({LW(reg::v0, 0, reg::a0)});
	h.Run();
	EXPECT_EQ(h.GetGprJit(reg::v0), h.GetGprInterp(reg::v0));
	EXPECT_NE(h.GetGprJit(reg::v0), kRamSentinel) << "load was served from RAM";
}

TEST(IopMemoryAccess, LwFromSifWindowTakesHelperPath)
{
	// SIF at 0x1d00xxxx is RLUT-mapped too, and iopMemRead32 special-cases it
	// *inside* the LUT branch (SBUS_F2x0 registers). Reading it through the
	// RAM fast path would return the LUT's raw bytes instead.
	JitTestHarness h;
	h.SetGpr(reg::a0, 0x1D000040u); // SBUS_F240 mirror — C ORs in 0xF0000002
	h.LoadProgram({LW(reg::v0, 0, reg::a0)});
	h.Run();
	EXPECT_EQ(h.GetGprJit(reg::v0), h.GetGprInterp(reg::v0));
	EXPECT_EQ(h.GetGprInterp(reg::v0) & 0xF0000002u, 0xF0000002u);
}

TEST(IopMemoryAccess, ConstRamAddressTakesFastPath)
{
	// Rs const + RAM target: the address folds at compile time and the stub's
	// runtime routing test then sends it down the RAM path.
	JitTestHarness h;
	h.WriteU32(kPhysBase, 0x5EEDBEEFu);
	h.LoadProgram({
		LUI(reg::a0, static_cast<u16>(kPhysBase >> 16)),
		ORI(reg::a0, reg::a0, static_cast<u16>(kPhysBase & 0xFFFF)),
		LW(reg::v0, 0, reg::a0),
	});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0x5EEDBEEFu);
}

TEST(IopMemoryAccess, ConstHwAddressRoutesThroughStubToHelper)
{
	// Rs const + non-RAM target. There is no compile-time shortcut any more —
	// a direct C call would clobber the allocator pool the stub exists to
	// preserve — so this takes the stub's routing test like everything else.
	JitTestHarness h;
	h.LoadProgram({
		LUI(reg::a0, static_cast<u16>(kHwHelperAddr >> 16)),
		ORI(reg::a0, reg::a0, static_cast<u16>(kHwHelperAddr & 0xFFFF)),
		LW(reg::v0, 0, reg::a0),
	});
	h.Run();
	EXPECT_EQ(h.GetGprJit(reg::v0), h.GetGprInterp(reg::v0));
}

TEST(IopMemoryAccess, LbSignExtendsThroughFastPathMirror)
{
	// The stub always zero-extends (Ldrb) and leaves sign extension to the
	// site, exactly as it does for the C return value. Check both signs
	// through a kseg mirror so the fast path is definitely the one running.
	JitTestHarness h;
	h.WriteU32(kPhysBase, 0x00000080u); // byte 0 = 0x80, byte 1 = 0x00
	h.SetGpr(reg::a0, kKseg1Base);
	h.LoadProgram({
		LB(reg::v0, 0, reg::a0),
		LB(reg::v1, 1, reg::a0),
	});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0xFFFFFF80u);
	EXPECT_EQ(h.GetGprInterp(reg::v1), 0u);
}

TEST(IopMemoryAccess, LhSignExtendsThroughFastPathMirror)
{
	JitTestHarness h;
	h.WriteU32(kPhysBase, 0x00018000u); // half 0 = 0x8000, half 1 = 0x0001
	h.SetGpr(reg::a0, kKseg0Base);
	h.LoadProgram({
		LH(reg::v0, 0, reg::a0),
		LH(reg::v1, 2, reg::a0),
	});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0xFFFF8000u);
	EXPECT_EQ(h.GetGprInterp(reg::v1), 1u);
}

// ---------------------------------------------------------------------------
// Register residency across memory ops.
//
// Aligned loads and stores no longer emit _psxFlushCall(FLUSH_EVERYTHING); they
// marshal into the fast-path stubs' non-allocatable registers and BL. That makes
// the stubs responsible for the allocator pool: the fast path must not touch it,
// and every path inside a stub that reaches C must save and restore the
// caller-saved half of it (emitIopStubSaveVolatiles, iR3000A-arm64.cpp).
//
// On what these can and cannot prove: deleting a register pair from the stub's
// save set only fails a test if the C function behind the stub actually clobbers
// that pair. As built here iopMemRead32 touches only x0-x2 and x8-x10, so
// dropping the x4/x5 or x6/x7 saves passes — while clobbering those same
// registers on the stub's FAST path (which nothing saves) fails 12 tests, i.e.
// the programs below really do hold live values in all of them. The save set is
// sized by the AAPCS contract, not by what today's callees happen to touch: any
// of x0-x17 is fair game for a callee, and a different compiler, optimisation
// level or hardware-handler path will take a wider set.
//
// These fill the pool with live guest values, put one memory op in the middle,
// and then consume every value. There are three distinct C paths inside the
// stubs to cover — a load that misses RAM, a store that misses RAM, and a store
// that hits RAM but lands in a granule with compiled code (the SMC clear) — so
// there is one test per path plus the pure fast path. Run()'s JIT-vs-interp diff
// compares the whole register file, so a clobber fails even if the sum survives.
// ---------------------------------------------------------------------------

namespace {
// 18 guest regs — more than the 16-entry IOP pool, so the allocator is forced to
// spill as well as fill, and every pool member is occupied at the memory op.
const u32 kLiveRegs[18] = {
	reg::t0, reg::t1, reg::t2, reg::t3, reg::t4, reg::t5, reg::t6, reg::t7,
	reg::t8, reg::t9, reg::s0, reg::s1, reg::s2, reg::s3, reg::s4, reg::s5,
	reg::s6, reg::s7,
};
constexpr u32 kLiveCount = 18;
constexpr u32 LivePattern(u32 i) { return 0x02020202u * (i + 1); }

// Seeds kLiveCount words at kPhysBase and emits the loads that make them
// resident, repeating `middle` after EVERY load, then the ADDU chain that
// consumes them all into v0. Returns the sum the chain must produce.
//
// The repetition is what gives this teeth. One copy of `middle` placed after all
// the loads only exercises whichever pool slots happen to be occupied at that
// one point — a mutation that drops, say, x4/x5 from the stub's save set can
// slip through. Interleaving means the op under test runs at every occupancy
// level from 1 live value up to full-pool-plus-spilling, so every allocatable
// host register holds a dirty guest value across it at some point.
u32 BuildResidencyProgram(JitTestHarness& h, std::vector<u32> middle, std::vector<u32>& prog)
{
	u32 expected = 0;
	for (u32 i = 0; i < kLiveCount; ++i)
	{
		h.WriteU32(kPhysBase + i * 4, LivePattern(i));
		expected += LivePattern(i);
		prog.push_back(LW(kLiveRegs[i], static_cast<s16>(i * 4), reg::a0));
		for (const u32 insn : middle)
			prog.push_back(insn);
	}
	prog.push_back(ADDU(reg::v0, kLiveRegs[0], reg::zero));
	for (u32 i = 1; i < kLiveCount; ++i)
		prog.push_back(ADDU(reg::v0, reg::v0, kLiveRegs[i]));
	return expected;
}
} // namespace

TEST(IopMemoryAccess, LiveRegsSurviveRamLoadFastPath)
{
	JitTestHarness h;
	h.SetGpr(reg::a0, kPhysBase);
	h.SetGpr(reg::a1, kPhysBase + 0x80);
	h.WriteU32(kPhysBase + 0x80, 0x13571357u);

	std::vector<u32> prog;
	const u32 expected = BuildResidencyProgram(h, {LW(reg::v1, 0, reg::a1)}, prog);
	h.LoadProgramAt(RecompilerTestEnvironment::kProgramPc,
		prog.data(), prog.size(), /*append_jr_ra_term=*/true);
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), expected);
	EXPECT_EQ(h.GetGprInterp(reg::v1), 0x13571357u);
}

TEST(IopMemoryAccess, LiveRegsSurviveHelperPathLoad)
{
	// Load that misses RAM: the stub saves the pool, calls iopMemRead32, moves
	// the result out of w0, and restores. Getting that order wrong (restoring
	// before reading w0) loses the loaded value.
	JitTestHarness h;
	h.SetGpr(reg::a0, kPhysBase);
	h.SetGpr(reg::a1, kHwInertAddr);

	std::vector<u32> prog;
	const u32 expected = BuildResidencyProgram(h, {LW(reg::v1, 0, reg::a1)}, prog);
	h.LoadProgramAt(RecompilerTestEnvironment::kProgramPc,
		prog.data(), prog.size(), /*append_jr_ra_term=*/true);
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), expected);
	EXPECT_EQ(h.GetGprJit(reg::v1), h.GetGprInterp(reg::v1));
}

TEST(IopMemoryAccess, LiveRegsSurviveHelperPathStore)
{
	JitTestHarness h;
	h.SetGpr(reg::a0, kPhysBase);
	h.SetGpr(reg::a1, kHwInertAddr);
	h.SetGpr(reg::v1, 0x2468ACE0u);

	std::vector<u32> prog;
	const u32 expected = BuildResidencyProgram(h, {SW(reg::v1, 0, reg::a1)}, prog);
	h.LoadProgramAt(RecompilerTestEnvironment::kProgramPc,
		prog.data(), prog.size(), /*append_jr_ra_term=*/true);
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), expected);
}

TEST(IopMemoryAccess, LiveRegsSurviveStoreSmcClearPath)
{
	// A RAM store whose granule holds compiled code takes the stub's third C
	// path, iopStoreClearHit. Target a word past this program's own terminator
	// but inside its 256-byte coverage granule, so the probe fires without
	// rewriting an instruction that is still executing.
	JitTestHarness h;
	h.SetGpr(reg::a0, kPhysBase);
	h.SetGpr(reg::a1, RecompilerTestEnvironment::kProgramPc + 0xF0);
	h.SetGpr(reg::v1, 0x0BADF00Du);

	std::vector<u32> prog;
	const u32 expected = BuildResidencyProgram(h, {SW(reg::v1, 0, reg::a1)}, prog);
	h.LoadProgramAt(RecompilerTestEnvironment::kProgramPc,
		prog.data(), prog.size(), /*append_jr_ra_term=*/true);
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), expected);
	EXPECT_EQ(h.ReadU32(RecompilerTestEnvironment::kProgramPc + 0xF0), 0x0BADF00Du);
}

TEST(IopMemoryAccess, LoadIntoBaseRegisterReadsPreUpdateAddress)
{
	// Rs == Rt. The address is formed before the destination is written, so the
	// load must use the OLD base — the flush-era code needed an explicit
	// _psxDeleteReg for this; now it falls out of the ordering.
	JitTestHarness h;
	h.WriteU32(kPhysBase + 4, 0xFACEB00Cu);
	h.SetGpr(reg::a0, kPhysBase);
	h.LoadProgram({LW(reg::a0, 4, reg::a0)});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::a0), 0xFACEB00Cu);
}

TEST(IopMemoryAccess, StoreReadsValueFromResidentRegister)
{
	// The store value now comes from wherever Rt lives rather than from a
	// freshly flushed psxRegs slot. Compute it in-register immediately before
	// the SW so the only correct source is the host register.
	JitTestHarness h;
	h.TrackMemWindow(kPhysBase, 4);
	h.SetGpr(reg::a0, kPhysBase);
	h.SetGpr(reg::t0, 0x1000u);
	h.SetGpr(reg::t1, 0x0234u);
	h.LoadProgram({
		ADDU(reg::t2, reg::t0, reg::t1), // t2 = 0x1234, register-resident
		SW(reg::t2, 0, reg::a0),
	});
	h.Run();
	EXPECT_EQ(h.ReadU32(kPhysBase), 0x1234u);
}
