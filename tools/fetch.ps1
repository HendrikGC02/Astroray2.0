# SPDX-License-Identifier: Apache-2.0
# tools/fetch.ps1 deps
# Fills the git-ignored cache third_party/_cache/<name>-<version>/ for every fetched-* entry of
# third_party/deps.toml, verifies SHA-256 (a cached copy that fails is deleted and downloaded
# again once; a fresh download that fails is deleted and fails) and extracts zips.
# Ref: 0001 D9.2, 0009 d6. Last line: "PASS deps (n downloaded, m cached)" or "FAIL deps: ...".
param([Parameter(Mandatory, Position = 0)][string]$Verb, [string]$Name)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'lib/python.ps1')
if ($Verb -notin @('deps', 'assets', 'refs')) {
  Write-Host "FAIL fetch: unknown verb '$Verb' (only: deps, assets, refs)"
  exit 2
}

function Fail([string]$why) {
  Write-Host "FAIL deps: $why"
  exit 1
}
function Sha([string]$p) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash.ToLowerInvariant() }

if ($Verb -eq 'assets') {
  # Ref: 0009 d21. Names only, never a crawl; a mismatch or a missing licence fails.
  function FailA([string]$why) {
    Write-Host "FAIL assets: $why"
    exit 1
  }
  try {
    $py = Get-ArPython
    $q = "import tomllib,json;print(json.dumps(tomllib.load(open('assets/manifest.toml','rb'))['asset']))"
    $assets = @((& $py -I -c $q) | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0) { FailA 'cannot parse assets/manifest.toml' }
    if ($Name) {
      $assets = @($assets | Where-Object { $_.name -eq $Name })
      if ($assets.Count -eq 0) { FailA "no asset named '$Name' in assets/manifest.toml" }
    }
    $got = 0
    $cached = 0
    foreach ($a in $assets) {
      if (-not $a.spdx -or -not "$($a.spdx)".Trim()) { FailA "$($a.name): missing spdx" }
      if (-not $a.sha256 -or -not $a.url) { FailA "$($a.name): missing url or sha256" }
      $dir = Join-Path $root "assets/_cache/$($a.name)"
      $file = Join-Path $dir ([IO.Path]::GetFileName(([Uri]$a.url).AbsolutePath))
      if ((Test-Path $file) -and ((Sha $file) -eq $a.sha256)) { $cached++; continue }
      if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
      New-Item -ItemType Directory -Force $dir | Out-Null
      Invoke-WebRequest -Uri $a.url -OutFile $file -UseBasicParsing
      if ((Sha $file) -ne $a.sha256) {
        Remove-Item -Recurse -Force $dir
        FailA "$($a.name): SHA-256 mismatch for $($a.url)"
      }
      $got++
      Write-Host "fetched $($a.name)"
    }
    Write-Host "PASS assets ($got downloaded, $cached cached)"
    exit 0
  } catch {
    FailA $_.Exception.Message
  }
}

if ($Verb -eq 'refs') {
  # Ref: 0009 d6. Reference sources, read-only, cited and never built; one copy for all worktrees.
  function FailR([string]$why) {
    Write-Host "FAIL refs: $why"
    exit 1
  }
  function Remove-Tree([string]$p) {
    if (-not (Test-Path -LiteralPath $p)) { return }
    Get-ChildItem -LiteralPath $p -Recurse -Force -File | ForEach-Object { $_.IsReadOnly = $false }
    Remove-Item -LiteralPath $p -Recurse -Force
  }
  try {
    $py = Get-ArPython
    $q = "import tomllib,json;print(json.dumps(tomllib.load(open('third_party/refs.toml','rb'))['ref']))"
    $refs = @((& $py -I -c $q) | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0) { FailR 'cannot parse third_party/refs.toml' }
    if ($Name) {
      $refs = @($refs | Where-Object { $_.name -eq $Name })
      if ($refs.Count -eq 0) { FailR "no reference named '$Name' in third_party/refs.toml" }
    }
    $base = if ($env:ASTRORAY_DEPS_CACHE) { $env:ASTRORAY_DEPS_CACHE } else { Join-Path $root 'third_party/_cache' }
    $base = Join-Path $base 'refs'
    New-Item -ItemType Directory -Force $base | Out-Null
    $got = 0
    $cached = 0
    foreach ($r in $refs) {
      foreach ($k in 'name', 'url', 'commit', 'sha256', 'spdx', 'purpose') {
        if (-not $r.$k -or -not "$($r.$k)".Trim()) { FailR "$($r.name): missing $k" }
      }
      $dir = Join-Path $base "$($r.name)-$($r.sha256.Substring(0, 12))"
      $done = Join-Path $dir 'extracted.ok'
      if ((Test-Path $done) -and ((Get-Content -LiteralPath $done -Raw).Trim() -eq $r.sha256)) { $cached++; continue }
      Remove-Tree $dir
      $zip = Join-Path $base "$($r.name).download.zip"
      $tmp = Join-Path $base "$($r.name).extracting"
      Remove-Tree $tmp
      Invoke-WebRequest -Uri $r.url -OutFile $zip -UseBasicParsing
      if ((Sha $zip) -ne $r.sha256) {
        Remove-Item -Force $zip
        FailR "$($r.name): SHA-256 mismatch for $($r.url); GitHub archives are not byte-stable: re-pin"
      }
      Expand-Archive -LiteralPath $zip -DestinationPath $tmp -Force
      Remove-Item -Force $zip
      $top = @(Get-ChildItem -LiteralPath $tmp -Force)
      if ($top.Count -ne 1 -or -not $top[0].PSIsContainer) { Remove-Tree $tmp; FailR "$($r.name): archive has no single top folder" }
      Move-Item -LiteralPath $top[0].FullName -Destination $dir
      Remove-Tree $tmp
      $past = [datetime]::new(2000, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)
      Get-ChildItem -LiteralPath $dir -Recurse -Force -File | ForEach-Object { $_.LastWriteTimeUtc = $past; $_.IsReadOnly = $true }
      Set-Content -Path $done -Value $r.sha256
      (Get-Item -LiteralPath $done).IsReadOnly = $true
      $got++
      Write-Host "fetched $($r.name) $($r.commit.Substring(0, 12))"
    }
    Write-Host "PASS refs ($got downloaded, $cached cached)"
    exit 0
  } catch {
    FailR $_.Exception.Message
  }
}

try {
  $py = Get-ArPython
  $q = "import tomllib,json;print(json.dumps(tomllib.load(open('third_party/deps.toml','rb'))['dep']))"
  $deps = @((& $py -I -c $q) | ConvertFrom-Json)
  if ($LASTEXITCODE -ne 0) { Fail 'cannot parse third_party/deps.toml' }
  $got = 0
  $cached = 0
  # Same override as cmake/deps.cmake: one shared cache for every worktree.
  $depsCache = if ($env:ASTRORAY_DEPS_CACHE) { $env:ASTRORAY_DEPS_CACHE } else { Join-Path $root 'third_party/_cache' }
  foreach ($d in $deps) {
    if ($d.kind -notin @('fetched-binary', 'fetched-not-redistributable')) { continue }
    if (-not $d.spdx) { Fail "$($d.name): missing spdx" }
    if (-not $d.sha256 -or -not $d.url) { Fail "$($d.name): missing url or sha256" }
    $dir = Join-Path $depsCache "$($d.name)-$($d.version)"
    $file = Join-Path $dir ([IO.Path]::GetFileName(([Uri]$d.url).AbsolutePath))
    $done = Join-Path $dir 'extracted.ok'
    if ((Test-Path $file) -and (Test-Path $done) -and ((Sha $file) -eq $d.sha256)) {
      $cached++
      continue
    }
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force $dir | Out-Null
    Invoke-WebRequest -Uri $d.url -OutFile $file -UseBasicParsing
    if ((Sha $file) -ne $d.sha256) {
      Remove-Item -Recurse -Force $dir
      Fail "$($d.name) $($d.version): SHA-256 mismatch for $($d.url)"
    }
    if ($file -match '\.zip$') {
      $ex = Join-Path $dir 'extracted'
      Expand-Archive -LiteralPath $file -DestinationPath $ex -Force
      # Zip entries carry local times without a zone; a file stamped ahead of this
      # machine's clock makes ninja rebuild its dependants forever. Pin a past time.
      $past = [datetime]::new(2000, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)
      Get-ChildItem -LiteralPath $ex -Recurse -File | ForEach-Object { $_.LastWriteTimeUtc = $past }
    }
    Set-Content -Path $done -Value $d.sha256
    $got++
    Write-Host "fetched $($d.name) $($d.version)"
  }
  Write-Host "PASS deps ($got downloaded, $cached cached)"
} catch {
  Fail $_.Exception.Message
}
