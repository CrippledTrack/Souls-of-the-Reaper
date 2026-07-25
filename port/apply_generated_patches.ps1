<#
.SYNOPSIS
  Re-applies the hand patches to port\generated\default\*.cpp that the ReXGlue
  codegen step cannot produce on its own. generated\ is rebuilt from scratch by
  `rexglue.exe codegen` (see README / port\diablo3_manifest.toml) every time it
  runs, which wipes any manual edit to that tree. Run this script once after
  every codegen run, before building.

.DESCRIPTION
  Two patches, both documented as "deuda tecnica" in CLAUDE.md:

  1. setjmp/longjmp fix (diablo3_recomp.108.cpp, sub_831583B0/sub_83158680):
     the recompiler mistranslates the guest setjmp/longjmp pair (truncated at
     an unsupported VMX instruction), which corrupts Lua's protected-call
     mechanism and crashes the GC. Replaced with host ppc_setjmp/ppc_longjmp
     plus manual save/restore of the guest callee-saved registers.

  2. Main-menu "B -> confirm -> Exit" fix (diablo3_recomp.19.cpp,
     sub_82632E00): redirects the game's "leave session, return to title
     screen" routine to close the game window instead, so confirming the
     native "return to title screen?" dialog from the main menu quits the app
     like a normal PC game's Exit option. The host-side hook this calls
     (D3RequestGameExit) lives in rexglue-sdk\src\kernel\xam\xam_input.cpp,
     which is a normal source file and does not need re-patching.

  Idempotent: safe to run multiple times, or on a tree that's already patched
  (each patch checks whether it was already applied and skips if so).

.EXAMPLE
  .\apply_generated_patches.ps1
#>
param(
    [string]$GeneratedDir = "$PSScriptRoot\generated\default"
)

$ErrorActionPreference = 'Stop'

function Apply-TextPatch {
    param(
        [Parameter(Mandatory)] [string]$File,
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$AlreadyAppliedMarker,
        [Parameter(Mandatory)] [string]$Find,
        [Parameter(Mandatory)] [string]$Replace
    )
    if (-not (Test-Path -LiteralPath $File)) {
        Write-Warning "$Name`: $File not found - run codegen first. Skipped."
        return
    }
    $text = Get-Content -LiteralPath $File -Raw
    if ($text.Contains($AlreadyAppliedMarker)) {
        Write-Host "$Name`: already applied, skipping." -ForegroundColor DarkGray
        return
    }
    $idx = $text.IndexOf($Find)
    if ($idx -lt 0) {
        Write-Warning ("{0}: anchor text not found in {1} - codegen output changed shape, " +
            "patch NOT applied. Re-check this script against the fresh file." -f $Name, (Split-Path -Leaf $File))
        return
    }
    $newText = $text.Substring(0, $idx) + $Replace + $text.Substring($idx + $Find.Length)
    Set-Content -LiteralPath $File -Value $newText -NoNewline -Encoding utf8
    Write-Host "$Name`: applied." -ForegroundColor Green
}

# Like Apply-TextPatch, but replaces everything from StartMarker up to (and
# NOT including) EndMarker - use this when the codegen output between the two
# markers is an entire function body that must be discarded wholesale (as
# opposed to Apply-TextPatch, which only swaps out the exact $Find text and
# leaves whatever follows it untouched - wrong when that "whatever follows"
# is the rest of a broken function we're replacing).
function Apply-RangeReplace {
    param(
        [Parameter(Mandatory)] [string]$File,
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$AlreadyAppliedMarker,
        [Parameter(Mandatory)] [string]$StartMarker,
        [Parameter(Mandatory)] [string]$EndMarker,
        [Parameter(Mandatory)] [string]$Replacement
    )
    if (-not (Test-Path -LiteralPath $File)) {
        Write-Warning "$Name`: $File not found - run codegen first. Skipped."
        return
    }
    $text = Get-Content -LiteralPath $File -Raw
    if ($text.Contains($AlreadyAppliedMarker)) {
        Write-Host "$Name`: already applied, skipping." -ForegroundColor DarkGray
        return
    }
    $startIdx = $text.IndexOf($StartMarker)
    if ($startIdx -lt 0) {
        Write-Warning ("{0}: start marker not found in {1} - codegen output changed shape, " +
            "patch NOT applied. Re-check this script against the fresh file." -f $Name, (Split-Path -Leaf $File))
        return
    }
    $endIdx = $text.IndexOf($EndMarker, $startIdx + $StartMarker.Length)
    if ($endIdx -lt 0) {
        Write-Warning "$Name`: end marker not found after start marker - patch NOT applied."
        return
    }
    $newText = $text.Substring(0, $startIdx) + $Replacement + $text.Substring($endIdx)
    Set-Content -LiteralPath $File -Value $newText -NoNewline -Encoding utf8
    Write-Host "$Name`: applied." -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Patch 1: setjmp/longjmp fix (diablo3_recomp.108.cpp)
# ---------------------------------------------------------------------------
$setjmpFile = Join-Path $GeneratedDir 'diablo3_recomp.108.cpp'

$setjmpHeaderAndBody = @'
// [D3 s9d] Correct setjmp/longjmp. The recompiler truncated the guest setjmp
// at an unsupported VMX instruction (halt_baddata @0x83158464): with
// trampoline target DAT_83443EF4 NULL the inline fallback (save regs +
// return 0) was DROPPED, so setjmp returned garbage (nonzero) -> EVERY
// luaD_rawrunprotected skipped its protected function -> f_luaopen/stack_init
// never ran -> half-built lua_State -> GC crash. Replace setjmp/longjmp with
// host ppc_setjmp/ppc_longjmp + manual save/restore of guest callee-saved
// registers (r1, r13..r31, lr, f14..f31), keyed by jmp_buf addr. NOTE: hand
// patch of generated/ output - lost on every codegen re-run, reapply with
// port/apply_generated_patches.ps1.
namespace {
struct D3JmpState {
	uint64_t r1;
	uint64_t r13,r14,r15,r16,r17,r18,r19,r20,r21,r22,r23,r24,r25,r26,r27,r28,r29,r30,r31;
	uint64_t lr;
	uint64_t f14,f15,f16,f17,f18,f19,f20,f21,f22,f23,f24,f25,f26,f27,f28,f29,f30,f31;
	uint32_t val;
};
inline std::unordered_map<uint32_t, D3JmpState>& d3_jmp_regs() {
	static thread_local std::unordered_map<uint32_t, D3JmpState> m;
	return m;
}
}

DEFINE_REX_FUNC(sub_831583B0) {
	const uint32_t buf = ctx.r3.u32;
	{
		D3JmpState& s = d3_jmp_regs()[buf];
		s.r1=ctx.r1.u64;
		s.r13=ctx.r13.u64; s.r14=ctx.r14.u64; s.r15=ctx.r15.u64; s.r16=ctx.r16.u64;
		s.r17=ctx.r17.u64; s.r18=ctx.r18.u64; s.r19=ctx.r19.u64; s.r20=ctx.r20.u64;
		s.r21=ctx.r21.u64; s.r22=ctx.r22.u64; s.r23=ctx.r23.u64; s.r24=ctx.r24.u64;
		s.r25=ctx.r25.u64; s.r26=ctx.r26.u64; s.r27=ctx.r27.u64; s.r28=ctx.r28.u64;
		s.r29=ctx.r29.u64; s.r30=ctx.r30.u64; s.r31=ctx.r31.u64; s.lr=ctx.lr;
		s.f14=ctx.f14.u64; s.f15=ctx.f15.u64; s.f16=ctx.f16.u64; s.f17=ctx.f17.u64;
		s.f18=ctx.f18.u64; s.f19=ctx.f19.u64; s.f20=ctx.f20.u64; s.f21=ctx.f21.u64;
		s.f22=ctx.f22.u64; s.f23=ctx.f23.u64; s.f24=ctx.f24.u64; s.f25=ctx.f25.u64;
		s.f26=ctx.f26.u64; s.f27=ctx.f27.u64; s.f28=ctx.f28.u64; s.f29=ctx.f29.u64;
		s.f30=ctx.f30.u64; s.f31=ctx.f31.u64;
	}
	int rv = 0;
	if (ppc_setjmp(buf) != 0) {
		D3JmpState& r = d3_jmp_regs()[buf];
		rv = (int)r.val;
		ctx.r1.u64=r.r1;
		ctx.r13.u64=r.r13; ctx.r14.u64=r.r14; ctx.r15.u64=r.r15; ctx.r16.u64=r.r16;
		ctx.r17.u64=r.r17; ctx.r18.u64=r.r18; ctx.r19.u64=r.r19; ctx.r20.u64=r.r20;
		ctx.r21.u64=r.r21; ctx.r22.u64=r.r22; ctx.r23.u64=r.r23; ctx.r24.u64=r.r24;
		ctx.r25.u64=r.r25; ctx.r26.u64=r.r26; ctx.r27.u64=r.r27; ctx.r28.u64=r.r28;
		ctx.r29.u64=r.r29; ctx.r30.u64=r.r30; ctx.r31.u64=r.r31; ctx.lr=r.lr;
		ctx.f14.u64=r.f14; ctx.f15.u64=r.f15; ctx.f16.u64=r.f16; ctx.f17.u64=r.f17;
		ctx.f18.u64=r.f18; ctx.f19.u64=r.f19; ctx.f20.u64=r.f20; ctx.f21.u64=r.f21;
		ctx.f22.u64=r.f22; ctx.f23.u64=r.f23; ctx.f24.u64=r.f24; ctx.f25.u64=r.f25;
		ctx.f26.u64=r.f26; ctx.f27.u64=r.f27; ctx.f28.u64=r.f28; ctx.f29.u64=r.f29;
		ctx.f30.u64=r.f30; ctx.f31.u64=r.f31;
	}
	ctx.r3.u64 = (uint32_t)rv;
}

'@

Apply-RangeReplace -File $setjmpFile -Name 'setjmp fix (sub_831583B0)' `
    -AlreadyAppliedMarker 'D3JmpState' `
    -StartMarker 'DEFINE_REX_FUNC(sub_831583B0) {' `
    -EndMarker 'DEFINE_REX_FUNC(sub_83158680) {' `
    -Replacement $setjmpHeaderAndBody

$longjmpPrefix = @'
DEFINE_REX_FUNC(sub_83158680) {
	// [D3 s9d] longjmp(jmp_buf=r3, val=r4): stash val, transfer to ppc_setjmp in
	// sub_831583B0 (which restores guest callee-saved regs). [[noreturn]].
	{ uint32_t b = ctx.r3.u32; int v = ctx.r4.s32; if (v == 0) v = 1; d3_jmp_regs()[b].val = (uint32_t)v; ppc_longjmp(b, v); }
'@

Apply-TextPatch -File $setjmpFile -Name 'longjmp fix (sub_83158680)' `
    -AlreadyAppliedMarker 'ppc_longjmp(b, v)' `
    -Find 'DEFINE_REX_FUNC(sub_83158680) {' `
    -Replace $longjmpPrefix

# ---------------------------------------------------------------------------
# Patch 2: main-menu "B -> confirm -> Exit" fix (diablo3_recomp.19.cpp)
# ---------------------------------------------------------------------------
$mainMenuFile = Join-Path $GeneratedDir 'diablo3_recomp.19.cpp'

$mainMenuReplace = @'
extern "C" void D3RequestGameExit();

// [D3 main-menu-exit] sub_82632E00 is the live "leave session, return to
// title screen" teardown routine, reached from the main-menu B -> confirm
// dialog -> A (Aceptar) flow (sub_8277D398 -> sub_82634E20 shows the confirm
// dialog -> its OnOK handler sub_82636F20 unconditionally calls this).
// Redirected to close the game like a normal PC "Exit" option instead of
// running its own teardown + reload-title-screen sequence. NOTE: hand patch
// of generated/ output - lost on every codegen re-run, reapply with
// port/apply_generated_patches.ps1.
DEFINE_REX_FUNC(sub_82632E00) {
	REX_FUNC_PROLOGUE();
	D3RequestGameExit();
	return;
	uint32_t ea{};
'@

Apply-TextPatch -File $mainMenuFile -Name 'main-menu exit fix (sub_82632E00)' `
    -AlreadyAppliedMarker 'D3RequestGameExit' `
    -Find "DEFINE_REX_FUNC(sub_82632E00) {`n`tREX_FUNC_PROLOGUE();`n`tuint32_t ea{};" `
    -Replace $mainMenuReplace

Write-Host "`nDone. Re-run 'port\build.ps1 -Config RelWithDebInfo' to rebuild." -ForegroundColor Cyan
