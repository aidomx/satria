# install.ps1 — pasang satria untuk Windows.
#
# Pemakaian:
#   irm https://raw.githubusercontent.com/aidomx/satria/main/install.ps1 | iex
#   .\install.ps1 --dev
#   .\install.ps1 --dev --version v0.2.0
#   .\install.ps1 --version v0.2.0
#
# Mode default mengunduh binary satria Windows dari GitHub Releases.
# Mode --dev melakukan bootstrap build dari source dengan compiler C lokal.
#
# Sumber versi (urutan prioritas):
#   1. --version <tag>   (argumen eksplisit)
#   2. $env:SATRIA_VERSION (di-set CI dari git tag, mis. v0.2.0)
#   3. .satria-version     (file di source tree)
#   4. fallback hardcoded
#
# Compiler yang didukung:
#   clang
#   gcc
#   cl (MSVC Developer Command Prompt)

$ErrorActionPreference = "Stop"

$GitHubUser = "aidomx"
$RepoName   = "satria"
$Branch     = "main"
$Version    = "v0.2.0"   # fallback terakhir

# ---------------------------------------------------------------------------
# Helper umum
# ---------------------------------------------------------------------------

function Read-Version([string]$Root) {
    if ($env:SATRIA_VERSION) {
        $v = $env:SATRIA_VERSION.Trim()
        if ($v) { return $v }
    }
    $file = Join-Path $Root ".satria-version"
    if (Test-Path $file) {
        $v = (Get-Content $file -TotalCount 1).Trim()
        if ($v) { return $v }
    }
    return $null
}

function Find-Compiler {
    if ($env:CC) {
        if (Get-Command $env:CC -ErrorAction SilentlyContinue) {
            return $env:CC
        }
        throw "compiler C '$env:CC' (dari CC) tidak ditemukan di PATH. " +
              "Jika ini job MSVC, jalankan ilammy/msvc-dev-cmd sebelum step ini."
    }
    foreach ($name in @("clang", "gcc", "cl")) {
        if (Get-Command $name -ErrorAction SilentlyContinue) { return $name }
    }
    return $null
}

function Get-HumanSize([long]$bytes) {
    if ($bytes -ge 1MB) { return "{0:F1}M" -f ($bytes / 1MB) }
    if ($bytes -ge 1KB) { return "{0:F1}K" -f ($bytes / 1KB) }
    return "${bytes}B"
}

function Write-CompileProgress {
    param([int]$Current, [int]$Total)
    $percent = [int](($Current * 100) / $Total)
    $width = 40 # Lebar tetap agar konsisten di semua terminal
    $filled = [int](($percent * $width) / 100)
    $empty = $width - $filled
    $bar = "_" * $filled
    $rest = "." * $empty
    # `r (carriage return) menimpa baris sebelumnya, persis seperti di bash
    Write-Host -NoNewline "`r> ${bar}${rest} ${percent}%"
}

function Invoke-NativeCapture([string]$Exe, [string[]]$CmdArgs) {
    # stderr compiler/linker digabung ke output. ErrorActionPreference dilonggarkan
    # hanya selama pemanggilan, agar warning (stderr) tidak dianggap error terminasi
    # oleh $ErrorActionPreference = "Stop". Output hanya ditampilkan bila gagal.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $out  = & $Exe @CmdArgs 2>&1
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $prev
    }
    return [pscustomobject]@{ Code = $code; Output = $out }
}

function Install-Binary([string]$Binary) {
    $InstallDir  = Join-Path $env:LOCALAPPDATA "satria"
    $InstallPath = Join-Path $InstallDir "satria.exe"

    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Copy-Item -Force $Binary $InstallPath

    $UserPath  = [Environment]::GetEnvironmentVariable("Path", "User")
    $PathParts = @()
    if ($UserPath) { $PathParts = $UserPath -split ";" | Where-Object { $_ } }

    if ($PathParts -notcontains $InstallDir) {
        $NewPath = (($PathParts + $InstallDir) -join ";")
        [Environment]::SetEnvironmentVariable("Path", $NewPath, "User")
    }

    Write-Host "`n> Installed  : $InstallPath"

    if (Test-Path $InstallPath) {
        & $InstallPath version 2>$null
    }
    Write-Host "> Note       : buka terminal baru agar PATH diperbarui."
}

# ---------------------------------------------------------------------------
# Parsing argumen
# ---------------------------------------------------------------------------

$Dev        = $false
$VersionTag = $null

for ($i = 0; $i -lt $args.Count; $i++) {
    switch ($args[$i]) {
        "--dev" {
            $Dev = $true
        }
        "--version" {
            if (($i + 1) -lt $args.Count) {
                $VersionTag = $args[$i + 1]
                $i++
            } else {
                throw "--version memerlukan argumen tag (mis. --version v0.2.0)"
            }
        }
    }
}

$ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }

$LocalSource = (
    (Test-Path (Join-Path $ScriptDir "src\main.c")) -and
    (Test-Path (Join-Path $ScriptDir "include\satria.h"))
)

# ---------------------------------------------------------------------------
# Mode --dev: bootstrap build dari source (DISELARASKAN DENGAN install.sh)
# ---------------------------------------------------------------------------

if ($Dev) {
    $TempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("satria-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $TempDir | Out-Null

    try {
        if ($LocalSource) {
            $SrcDir = $ScriptDir
        } else {
            $SourceUrl = "https://github.com/$GitHubUser/$RepoName/archive/refs/heads/$Branch.zip"
            $Archive   = Join-Path $TempDir "satria.zip"

            Write-Host "> Downloading source: $SourceUrl"
            Invoke-WebRequest -Uri $SourceUrl -OutFile $Archive

            Expand-Archive -Path $Archive -DestinationPath $TempDir -Force
            $SrcDir = Get-ChildItem $TempDir -Directory |
                Where-Object { $_.Name -ne "build" } |
                Select-Object -First 1 -ExpandProperty FullName
        }

        if (-not $SrcDir -or -not (Test-Path (Join-Path $SrcDir "src\main.c"))) {
            throw "source tree tidak valid (src\main.c tidak ditemukan)"
        }

        if ($VersionTag) {
            $Version = $VersionTag
        } else {
            $found = Read-Version $SrcDir
            if ($found) { $Version = $found }
        }

        $Compiler = Find-Compiler
        if (-not $Compiler) {
            throw "compiler C tidak ditemukan; pasang clang, gcc, atau gunakan MSVC Developer Command Prompt"
        }

        Write-Host "> Bootstrap  : satria $Version"
        Write-Host "> Compiler   : $Compiler"

        $Sources = Get-ChildItem (Join-Path $SrcDir "src") -Filter "*.c" -Recurse
        $Total = $Sources.Count
        if ($Total -eq 0) {
            throw "tidak ada file sumber (.c) ditemukan"
        }

        Write-Host "> Compile    : $Total file(s)"

        $ObjDir = Join-Path $TempDir "obj"
        New-Item -ItemType Directory -Force -Path $ObjDir | Out-Null

        $IncludeDir = Join-Path $SrcDir "include"
        $SrcInner   = Join-Path $SrcDir "src"
        $Done       = 0

        foreach ($Source in $Sources) {
            $Relative    = $Source.FullName.Substring($SrcInner.Length).TrimStart("\")
            $ObjRelative = [System.IO.Path]::ChangeExtension($Relative, ".o")
            $Obj         = Join-Path $ObjDir $ObjRelative
            $ObjParent   = Split-Path $Obj -Parent

            New-Item -ItemType Directory -Force -Path $ObjParent | Out-Null

            $global:LASTEXITCODE = 0
            if ($Compiler -eq "cl") {
                $ClArgs = @(
                    "/nologo", "/std:c11", "/O2", "/W4",
                    "/DSATRIA_VERSION_EMBEDDED=`"$Version`"",
                    "/I$IncludeDir",
                    "/I$SrcInner",
                    "/c", $Source.FullName,
                    "/Fo$Obj"
                )
                $cr = Invoke-NativeCapture "cl" $ClArgs
            } else {
                $CcArgs = @(
                    "-std=gnu11", "-O2", "-Wall", "-Wextra",
                    "-DSATRIA_VERSION_EMBEDDED=`"$Version`"",
                    "-I$IncludeDir",
                    "-I$SrcInner",
                    "-c", $Source.FullName,
                    "-o", $Obj
                )
                $cr = Invoke-NativeCapture $Compiler $CcArgs
            }

            if ($cr.Code -ne 0) {
                Write-Host ""
                $cr.Output | ForEach-Object { Write-Host "$_" }
                throw "gagal mengompilasi $($Source.Name)"
            }

            $Done++
            Write-CompileProgress -Current $Done -Total $Total
        }
        
        # Cetak newline setelah progress bar 100% selesai
        Write-Host ""

        # Output langsung ke bin/ (bukan build/bin/)
        $Output = Join-Path $SrcDir "bin\satria.exe"
        New-Item -ItemType Directory -Force -Path (Split-Path $Output) | Out-Null

        Write-Host "> Linking    : bin\satria.exe"
        
        $Objects = Get-ChildItem $ObjDir -Filter "*.o" -Recurse | Select-Object -ExpandProperty FullName

        $global:LASTEXITCODE = 0
        if ($Compiler -eq "cl") {
            $LinkArgs = @($Objects) + @("/nologo", "/SUBSYSTEM:CONSOLE", "/OUT:$Output", "shell32.lib")
            $lr = Invoke-NativeCapture "link" $LinkArgs
        } else {
            $LinkArgs = @($Objects) + @("-o", $Output, "-lshell32")
            $lr = Invoke-NativeCapture $Compiler $LinkArgs
        }

        if ($lr.Code -ne 0) {
            $lr.Output | ForEach-Object { Write-Host "$_" }
            throw "gagal melakukan linking satria"
        }

        $Size = (Get-Item $Output).Length
        $SizeStr = Get-HumanSize $Size
        
        # Update pesan linking dengan ukuran (meniru install.sh)
        # Kita timpa baris "Linking" sebelumnya dengan yang ada ukurannya
        Write-Host "`r> Linking    : bin\satria.exe ($SizeStr)   "

        Write-Host "> Built & Saved: $Output"
        
        # Jalankan versi untuk memastikan binary valid
        & $Output version 2>$null

        # Install ke user PATH juga (opsional tapi disarankan)
        Install-Binary $Output

        Write-Host "> Satria is installed, for show help run : bin\satria.exe help"
    }
    finally {
        Remove-Item -Recurse -Force $TempDir -ErrorAction SilentlyContinue
    }

    exit 0
}

# ---------------------------------------------------------------------------
# Mode default: unduh binary dari GitHub Releases
# ---------------------------------------------------------------------------

function Get-ReleaseInfo([string]$Tag) {
    $api = if ($Tag) {
        "https://api.github.com/repos/$GitHubUser/$RepoName/releases/tags/$Tag"
    } else {
        "https://api.github.com/repos/$GitHubUser/$RepoName/releases/latest"
    }
    $headers = @{
        "User-Agent" = "satria-installer"
        "Accept"     = "td.github+json"
    }
    try {
        return Invoke-RestMethod -Uri $api -Headers $headers
    } catch {
        throw "gagal mengambil release dari GitHub API: $($_.Exception.Message)"
    }
}

function Get-WindowsAssetUrl([string]$Tag) {
    $release = Get-ReleaseInfo $Tag
    $asset = $release.assets |
        Where-Object { $_.name -eq "satria-windows-x64.exe" } |
        Select-Object -First 1

    if (-not $asset) {
        $names = ($release.assets | ForEach-Object { $_.name }) -join ", "
        throw "asset 'satria-windows-x64.exe' tidak ada. Tersedia: $names"
    }

    Write-Host "> Release    : $($release.tag_name)"
    return $asset.browser_download_url
}

if (-not $VersionTag -and $env:SATRIA_VERSION) {
    $VersionTag = $env:SATRIA_VERSION.Trim()
}

$TempFile = Join-Path ([System.IO.Path]::GetTempPath()) ("satria-" + [guid]::NewGuid().ToString("N") + ".exe")

try {
    $url = Get-WindowsAssetUrl $VersionTag
    Write-Host "> Downloading: $url"
    Invoke-WebRequest -Uri $url -OutFile $TempFile
    Install-Binary $TempFile
}
finally {
    Remove-Item -Force $TempFile -ErrorAction SilentlyContinue
}
