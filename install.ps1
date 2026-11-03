# install.ps1  pasang satria untuk Windows.
#
# Pemakaian:
#   irm https://raw.githubusercontent.com/aidomx/satria/main/install.ps1 | iex
#   .\install.ps1 --dev
#   .\install.ps1 --dev --version v0.2.0
#   .\install.ps1 --dev -o build.log          (keluaran build ikut ditulis ke log)
#   .\install.ps1 --version v0.2.0
#
# Mode default mengunduh binary satria Windows dari GitHub Releases.
# Mode --dev melakukan bootstrap build dari source dengan compiler C lokal.
#
# Sumber versi (urutan prioritas, sama dengan install.sh):
#   1. --version <tag>      (argumen eksplisit)
#   2. $env:SATRIA_VERSION  (di-set CI dari git tag, mis. v0.2.0)
#   3. .satria-version      (file di source tree; hanya untuk --dev)
#   4. fallback hardcoded
#
# Compiler yang didukung (urutan deteksi jika $env:CC tidak di-set):
#   clang, gcc, cl (MSVC Developer Command Prompt)

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
    $width = 40
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

function Quote-Arg([string]$Arg) {
    # Bungkus argumen yang mengandung spasi atau tanda kutip; tanda kutip di dalamnya di-escape.
    if ($Arg -match '[\s"]') {
        return '"' + ($Arg -replace '"', '\"') + '"'
    }
    return $Arg
}

# Kompilasi paralel dengan pool Start-Process (kompatibel Windows PowerShell 5.1).
# Setiap job: [pscustomobject]@{ Name = ...; Args = [string[]] ; Obj = ... }
# Keluaran tiap proses ditulis ke file sementara; hanya ditampilkan bila gagal.
# Mengembalikan jumlah job yang gagal (semua job tetap dijalankan, seperti xargs -P).
function Invoke-CompilePool {
    param(
        [string]$Compiler,
        [object[]]$Jobs,
        [int]$Parallel,
        [string]$TempDir
    )
    $exePath = (Get-Command $Compiler -ErrorAction Stop).Source
    $queue   = New-Object System.Collections.Queue
    foreach ($j in $Jobs) { $queue.Enqueue($j) }
    $running = New-Object System.Collections.ArrayList
    $total   = $Jobs.Count
    $done    = 0
    $failed  = 0
    $seq     = 0

    Write-CompileProgress -Current 0 -Total $total

    while ($queue.Count -gt 0 -or $running.Count -gt 0) {
        while ($queue.Count -gt 0 -and $running.Count -lt $Parallel) {
            $job     = $queue.Dequeue()
            $seq++
            $outFile = Join-Path $TempDir ("out-$seq.txt")
            $errFile = Join-Path $TempDir ("err-$seq.txt")
            $argStr  = (@($job.Args) | ForEach-Object { Quote-Arg $_ }) -join " "
            $proc = Start-Process -FilePath $exePath -ArgumentList $argStr -NoNewWindow -PassThru `
                -RedirectStandardOutput $outFile -RedirectStandardError $errFile
            [void]$running.Add([pscustomobject]@{ Proc = $proc; Job = $job; Out = $outFile; Err = $errFile })
        }

        Start-Sleep -Milliseconds 50

        for ($k = $running.Count - 1; $k -ge 0; $k--) {
            $r = $running[$k]
            if ($r.Proc.HasExited) {
                $running.RemoveAt($k)
                $done++
                if ($r.Proc.ExitCode -ne 0) {
                    $failed++
                    Write-Host ""
                    Write-Host "gagal mengompilasi $($r.Job.Name)"
                    foreach ($f in @($r.Out, $r.Err)) {
                        if (Test-Path $f) { Get-Content $f | ForEach-Object { Write-Host "$_" } }
                    }
                }
                Write-CompileProgress -Current $done -Total $total
            }
        }
    }

    # Newline setelah progress bar 100%
    Write-Host ""
    return $failed
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

function Get-ReleaseInfo([string]$Tag) {
    $api = if ($Tag) {
        "https://api.github.com/repos/$GitHubUser/$RepoName/releases/tags/$Tag"
    } else {
        "https://api.github.com/repos/$GitHubUser/$RepoName/releases/latest"
    }
    $headers = @{
        "User-Agent" = "satria-installer"
        "Accept"     = "application/vnd.github+json"
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

# ---------------------------------------------------------------------------
# Parsing argumen
# ---------------------------------------------------------------------------

$Dev        = $false
$VersionTag = $null
$LogFile    = $null

for ($i = 0; $i -lt $args.Count; $i++) {
    switch ($args[$i]) {
        "--dev" {
            $Dev = $true
        }
        { $_ -eq "-o" -or $_ -eq "--output" } {
            if (($i + 1) -lt $args.Count) {
                $LogFile = $args[$i + 1]
                $i++
            } else {
                throw "-o memerlukan argumen file log (mis. -o build.log)"
            }
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

# Prioritas 2: env SATRIA_VERSION (berlaku untuk --dev dan mode default)
if (-not $VersionTag -and $env:SATRIA_VERSION) {
    $VersionTag = $env:SATRIA_VERSION.Trim()
}

if ($LogFile) {
    $LogFile = [System.IO.Path]::GetFullPath($LogFile)
    Start-Transcript -Path $LogFile -Force | Out-Null
}

$ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }

$LocalSource = (
    (Test-Path (Join-Path $ScriptDir "src\main.c")) -and
    (Test-Path (Join-Path $ScriptDir "include\satria.h"))
)

# ---------------------------------------------------------------------------
# Mode --dev: bootstrap build dari source (diselaraskan dengan install.sh)
# ---------------------------------------------------------------------------

if ($Dev) {
    $TempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("satria-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $TempDir | Out-Null

    try {
        if ($LocalSource) {
            $SrcDir = $ScriptDir
        } else {
            # Tanpa --version / SATRIA_VERSION: pakai tag release terbaru (seperti install.sh).
            # Bila API GitHub gagal, jatuh ke branch.
            $Tag = $VersionTag
            if (-not $Tag) {
                try { $Tag = (Get-ReleaseInfo $null).tag_name } catch { $Tag = $null }
            }
            if ($Tag) {
                $VersionTag = $Tag
                $SourceUrl  = "https://github.com/$GitHubUser/$RepoName/archive/refs/tags/$Tag.zip"
            } else {
                $SourceUrl  = "https://github.com/$GitHubUser/$RepoName/archive/refs/heads/$Branch.zip"
            }
            $Archive = Join-Path $TempDir "satria.zip"

            Write-Host "> Downloading source: $SourceUrl"
            Invoke-WebRequest -UseBasicParsing -Uri $SourceUrl -OutFile $Archive

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

        $JobsN = [Environment]::ProcessorCount
        if ($JobsN -lt 1) { $JobsN = 2 }

        Write-Host "> Bootstrap  : satria $Version"
        Write-Host "> Compiler   : $Compiler"
        Write-Host "> Parallel   : $JobsN jobs"

        $SrcInner   = Join-Path $SrcDir "src"
        $IncludeDir = Join-Path $SrcDir "include"
        $Sources    = @(Get-ChildItem $SrcInner -Filter "*.c" -Recurse -File | Sort-Object FullName)
        $Total      = $Sources.Count
        if ($Total -eq 0) {
            throw "tidak ada file sumber (.c) ditemukan di $SrcInner"
        }

        Write-Host "> Compile    : $Total file(s)"

        $ObjDir = Join-Path $TempDir "obj"
        New-Item -ItemType Directory -Force -Path $ObjDir | Out-Null

        # MSVC memakai .obj (link.exe), gcc/clang memakai .o
        $ObjExt = if ($Compiler -eq "cl") { ".obj" } else { ".o" }

        $Jobs = @(foreach ($Source in $Sources) {
            $Relative    = $Source.FullName.Substring($SrcInner.Length).TrimStart("\")
            $ObjRelative = [System.IO.Path]::ChangeExtension($Relative, $ObjExt)
            $Obj         = Join-Path $ObjDir $ObjRelative
            New-Item -ItemType Directory -Force -Path (Split-Path $Obj -Parent) | Out-Null

            if ($Compiler -eq "cl") {
                $JobArgs = @(
                    "/nologo", "/std:c11", "/O2", "/W4",
                    "/DSATRIA_VERSION_EMBEDDED=`"$Version`"",
                    "/I$IncludeDir",
                    "/I$SrcInner",
                    "/c", $Source.FullName,
                    "/Fo$Obj"
                )
            } else {
                $JobArgs = @(
                    "-std=gnu11", "-O2", "-Wall", "-Wextra",
                    "-DSATRIA_VERSION_EMBEDDED=`"$Version`"",
                    "-I$IncludeDir",
                    "-I$SrcInner",
                    "-c", $Source.FullName,
                    "-o", $Obj
                )
            }
            [pscustomobject]@{ Name = $Source.Name; Args = $JobArgs; Obj = $Obj }
        })

        $Failed = Invoke-CompilePool -Compiler $Compiler -Jobs $Jobs -Parallel $JobsN -TempDir $TempDir
        if ($Failed -gt 0) {
            throw "kompilasi gagal ($Failed file)"
        }

        # Output langsung ke bin/ (sama dengan install.sh)
        $Output = Join-Path $SrcDir "bin\satria.exe"
        New-Item -ItemType Directory -Force -Path (Split-Path $Output) | Out-Null

        $Objects = @(Get-ChildItem $ObjDir -Filter "*$ObjExt" -Recurse -File |
            Sort-Object FullName | Select-Object -ExpandProperty FullName)

        if ($Compiler -eq "cl") {
            $LinkArgs = $Objects + @("/nologo", "/SUBSYSTEM:CONSOLE", "/OUT:$Output",
                                     "shell32.lib", "kernel32.lib", "ws2_32.lib")
            $lr = Invoke-NativeCapture "link" $LinkArgs
        } else {
            $LinkArgs = $Objects + @("-o", $Output, "-lshell32", "-lkernel32", "-lws2_32")
            $lr = Invoke-NativeCapture $Compiler $LinkArgs
        }

        if ($lr.Code -ne 0) {
            $lr.Output | ForEach-Object { Write-Host "$_" }
            throw "gagal melakukan linking satria"
        }

        $SizeStr = Get-HumanSize (Get-Item $Output).Length
        Write-Host "> Linking    : bin\satria.exe ($SizeStr)"

        # Verifikasi binary berjalan; keluaran tidak ditampilkan (sama dengan install.sh)
        & $Output version *> $null

        if ($LocalSource) {
            # Pohon source lokal: hanya memberi tahu lokasi binary, tidak memasang.
            Write-Host "> Satria is installed, for show help run : bin\satria.exe help"
        } else {
            # Source diunduh: pasang ke user PATH (sama dengan install.sh).
            Install-Binary $Output
            Write-Host "> Satria is installed, for show help run : satria"
        }
    }
    finally {
        Remove-Item -Recurse -Force $TempDir -ErrorAction SilentlyContinue
        if ($LogFile) { try { Stop-Transcript | Out-Null } catch { } }
    }

    exit 0
}

# ---------------------------------------------------------------------------
# Mode default: unduh binary dari GitHub Releases
# ---------------------------------------------------------------------------

$TempFile = Join-Path ([System.IO.Path]::GetTempPath()) ("satria-" + [guid]::NewGuid().ToString("N") + ".exe")

try {
    $url = Get-WindowsAssetUrl $VersionTag
    Write-Host "> Downloading: $url"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $TempFile
    Install-Binary $TempFile
}
finally {
    Remove-Item -Force $TempFile -ErrorAction SilentlyContinue
    if ($LogFile) { try { Stop-Transcript | Out-Null } catch { } }
}
