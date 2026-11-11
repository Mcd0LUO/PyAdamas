# py23.ps1 - Windows entry point for the py23 compiler.
#
# py23 itself targets Linux (System V x86-64 ABI + ELF) and runs inside the
# py23-dev container in WSL2. This script forwards Windows invocations into it:
#
#   * .py sources are copied from Windows into /tmp/py23-stage in the container
#   * source arguments are rewritten to their in-container paths
#   * `build -o` output is copied back to Windows
#   * the exit code is propagated
#
# Limitation: relative paths and data files used by the program are resolved
# INSIDE the container (cwd = /tmp/py23-stage), not on Windows. Files the
# program needs must be copied in as well.
#
# NOTE: keep this file pure ASCII. Windows PowerShell 5.1 reads .ps1 files
# without a UTF-8 BOM as ANSI/GBK, so non-ASCII text would be mis-decoded.

$ErrorActionPreference = 'Stop'

$Distro    = if ($env:PY23_WSL_DISTRO) { $env:PY23_WSL_DISTRO } else { 'Debian' }
$Container = if ($env:PY23_CONTAINER)   { $env:PY23_CONTAINER }   else { 'py23-dev' }
$Stage     = '/tmp/py23-stage'
# the repo is mounted at /w inside the container; the host path does not exist there
$Py23InCtr = '/w/build/dev/py23'

function Quote-Bash([string]$s) { "'" + ($s -replace "'", "'\''") + "'" }

function Wsl-Bash([string]$cmd) {
    # no return value: the exit code is picked up from $LASTEXITCODE by the caller
    & wsl.exe -d $Distro -- bash -lc $cmd
}

if ($args.Count -eq 0) {
    Write-Host "usage: py23 <check|run|build|tokens|ast|types|ir|asm> <file.py> [args...]"
    exit 2
}

# start the container if it is not running
$state = (wsl.exe -d $Distro -- bash -lc "docker inspect -f '{{.State.Running}}' $Container 2>/dev/null").Trim()
if ($state -ne 'true') {
    Write-Host "[py23] container $Container is not running, starting it ..."
    Wsl-Bash "docker start $Container" | Out-Null
}
Wsl-Bash "docker exec $Container mkdir -p $Stage" | Out-Null

# rewrite arguments: .py files get copied in, -o output path is redirected
$newArgs = @()
$outWin  = $null
$outCont = $null
$i = 0
while ($i -lt $args.Count) {
    $a = [string]$args[$i]

    if ($a -eq '-o' -and ($i + 1) -lt $args.Count) {
        $outWin = [string]$args[$i + 1]
        # docker cp runs with the WSL cwd, so the destination must be absolute
        if (-not [IO.Path]::IsPathRooted($outWin)) {
            $outWin = [IO.Path]::GetFullPath((Join-Path (Get-Location).Path $outWin))
        }
        $outCont = "$Stage/out_$([IO.Path]::GetFileName($outWin))"
        $newArgs += '-o'
        $newArgs += $outCont
        $i += 2
        continue
    }

    if ((Test-Path -LiteralPath $a -PathType Leaf) -and ($a -match '\.(py|py23i)$')) {
        $full = (Resolve-Path -LiteralPath $a).Path
        # wsl.exe eats backslashes in its arguments; feed it forward slashes
        $wslPath = (wsl.exe -d $Distro -- wslpath -a ($full -replace '\\', '/')).Trim()
        $name = "s$($newArgs.Count)_$([IO.Path]::GetFileName($full))"
        $dst = "${Container}:${Stage}/${name}"
        Wsl-Bash "docker cp $(Quote-Bash $wslPath) $(Quote-Bash $dst)" | Out-Null
        $newArgs += "${Stage}/${name}"
    } else {
        $newArgs += $a
    }
    $i++
}

$quoted = ($newArgs | ForEach-Object { Quote-Bash ([string]$_) }) -join ' '
$cmd = "docker exec -w $Stage $Container $Py23InCtr $quoted"

Wsl-Bash $cmd
$code = $LASTEXITCODE

# copy the build artifact back to Windows
if ($outWin -and $outCont) {
    & wsl.exe -d $Distro -- bash -lc "docker exec $Container test -f $outCont" | Out-Null
    if ($LASTEXITCODE -eq 0) {
        $wslDir  = (wsl.exe -d $Distro -- bash -lc "docker exec $Container dirname $outCont").Trim()
        $wslBase = (wsl.exe -d $Distro -- bash -lc "docker exec $Container basename $outCont").Trim()
        $src = "${Container}:${wslDir}/${wslBase}"
        # docker cp parses a "D:\..." destination as container:path too, so land in
        # WSL first and then copy through the normal /mnt/d filesystem
        $wslTmp = "/home/lenovo/.py23-out-$wslBase"
        $wslDest = (wsl.exe -d $Distro -- wslpath -a ($outWin -replace '\\', '/')).Trim()
        Wsl-Bash "docker cp $(Quote-Bash $src) $(Quote-Bash $wslTmp)" | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[py23] ERROR: could not copy the artifact out of the container"
        } else {
            Wsl-Bash "cp $(Quote-Bash $wslTmp) $(Quote-Bash $wslDest)" | Out-Null
            if ($LASTEXITCODE -ne 0) {
                Write-Host "[py23] ERROR: could not copy the artifact to $outWin"
            } else {
                Write-Host "[py23] artifact copied back to: $outWin"
            }
        }
    } else {
        Write-Host "[py23] ERROR: the container did not produce $outCont"
    }
}

exit $code
