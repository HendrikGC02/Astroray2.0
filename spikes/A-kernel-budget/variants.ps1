# Variant definitions shared by gen_ninja.ps1 and the measurement scripts.
# n = name, w = spectral width, lb = __launch_bounds__ minBlocks (0 = none; 256 threads), ni = lobes __noinline__, ptr = material by pointer
$variants = @(
  @{n='base';      w=4; lb=0; ni=0; ptr=0},
  @{n='lb';        w=4; lb=2; ni=0; ptr=0},   # 256 x 2 blocks => 128-register cap
  @{n='lb4';       w=4; lb=4; ni=0; ptr=0},   # diagnostic: 256 x 4 blocks => 64-register cap
  @{n='noinl';     w=4; lb=0; ni=1; ptr=0},
  @{n='noinl_lb';  w=4; lb=2; ni=1; ptr=0},
  @{n='ptr';       w=4; lb=0; ni=0; ptr=1},
  @{n='noinl_ptr'; w=4; lb=0; ni=1; ptr=1},
  @{n='w8';        w=8; lb=0; ni=0; ptr=0},
  @{n='w8_lb';     w=8; lb=2; ni=0; ptr=0},
  @{n='w8_noinl';  w=8; lb=0; ni=1; ptr=0}
)
$paramVariants = @(@{n='base'; w=4}, @{n='w8'; w=8})
$fvmVariants = @(@{n='fvm'; w=4}, @{n='w8_fvm'; w=8})
function Flags($v) { "-DKL_VARIANT=$($v.n) -DKL_WIDTH=$($v.w) -DKL_LB=$([int]($v.lb -gt 0)) -DKL_LB_MIN=$($v.lb) -DKL_NOINLINE_LOBES=$($v.ni) -DKL_MAT_BYPTR=$($v.ptr)" }
# All kernel translation units: @{obj=..., src=..., flags=...}
function Get-Units {
  $u = @()
  foreach ($v in $variants) { foreach ($k in 'fused','bsdf','nee') { $u += @{ obj="out/obj/$($v.n)_$k.obj"; src="src/k_$k.cu"; flags=(Flags $v); name="$($v.n)_$k" } } }
  foreach ($kind in @(@{list=$paramVariants; k='param'}, @{list=$fvmVariants; k='fvm'})) {
    foreach ($v in $kind.list) { $vv = @{n=$v.n; w=$v.w; lb=0; ni=0; ptr=0}; $u += @{ obj="out/obj/$($v.n)_$($kind.k).obj"; src="src/k_$($kind.k).cu"; flags=(Flags $vv); name="$($v.n)_$($kind.k)" } }
  }
  return $u
}
