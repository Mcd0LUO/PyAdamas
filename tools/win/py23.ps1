#!/usr/bin/env pwsh
# py23.ps1 - Windows entry point for the py23 compiler.
#
# Can be started from cmd/PowerShell (via py23.cmd), or directly from a bash
# shell (Git Bash) as `./py23.ps1 run x.py` - the shebang above makes bash
# hand the file to pwsh.
#
# py23 itself targets Linux (System V x86-64 ABI + ELF) and lives in WSL2.
# This script runs it in a throwaway container that mounts:
#
#   * the WSL repo        -> /w     (compiler binary + module cache)
#   * the script's folder -> /work  (the program's own working directory)
#
# Because the program's folder is mounted, relative paths, data files and
# argv[0]-style paths all behave the way they do on Windows. Nothing is
# copied in or out; `build -o` writes the artifact straight into the
# Windows folder.
#
#   py23 run x.py
#   py23 check x.py
#   py23 build x.py -o out
#
# Limitation: the produced binary is a Linux ELF - it cannot execute on
# Windows, it has to run in WSL / the container.
#
# Env overrides: PY23_WSL_DISTRO (Debian), PY23_IMAGE (py23-env:gcc14),
#                PY23_REPO (/home/lenovo/cpp/cpy23)
#
# NOTE: keep this file pure ASCII. Windows PowerShell 5.1 reads .ps1 files
# without a UTF-8 BOM as ANSI/GBK, so non-ASCII text would be mis-decoded.

$ErrorActionPreference = 'Stop'

# When pwsh was started from Git Bash, MSYS would rewrite POSIX-looking
# arguments of child processes into Windows paths. The container paths below
# (/w, /work) must survive untouched.
$env:MSYS_NO_PATHCONV = '1'
# wsl.exe writes UTF-16 by default, which lands as garbage in a UTF-8 pipe
$env:WSL_UTF8 = '1'

$Distro  = if ($env:PY23_WSL_DISTRO) { $env:PY23_WSL_DISTRO } else { 'Debian' }
$Image   = if ($env:PY23_IMAGE)       { $env:PY23_IMAGE }       else { 'py23-env:gcc14' }
$RepoWsl = if ($env:PY23_REPO)        { $env:PY23_REPO }        else { '/home/lenovo/cpp/cpy23' }
$RepoCtr = '/w'
$WorkCtr = '/work'

function Quote-Bash([string]$s) { "'" + ($s -replace "'", "'\''") + "'" }

function Wsl-Bash([string]$cmd) {
    # no return value: the exit code is picked up from $LASTEXITCODE by the caller
    & wsl.exe -d $Distro -- bash -lc $cmd
}

if ($args.Count -eq 0) {
    Write-Host "usage: py23 <check|run|build|tokens|ast|types|ir|asm> <file.py> [args...]"
    exit 2
}

# locate the script argument
$scriptIdx = -1
for ($k = 0; $k -lt $args.Count; $k++) {
    $a = [string]$args[$k]
    if ($a -match '\.(py|py23i)$' -and (Test-Path -LiteralPath $a -PathType Leaf)) {
        $scriptIdx = $k
        break
    }
}
if ($scriptIdx -lt 0) {
    Write-Host "[py23] no .py file argument found"
    exit 2
}

# script path -> WSL path -> (folder, basename)
$absScript = (Resolve-Path -LiteralPath ([string]$args[$scriptIdx])).Path
$wslScript = (wsl.exe -d $Distro -- wslpath -a ($absScript -replace '\\', '/')).Trim()
$slash = $wslScript.LastIndexOf('/')
if ($slash -lt 0) { Write-Host "[py23] cannot resolve the script path: $wslScript"; exit 2 }
$wslDir  = $wslScript.Substring(0, $slash)
$wslBase = $wslScript.Substring($slash + 1)

# the image has to exist locally (Docker Hub is unreachable from here)
& wsl.exe -d $Distro -- bash -lc "docker image inspect $Image" | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "[py23] docker image '$Image' not found (build it first: docker build -t $Image .)"
    exit 1
}

# rebuild the argument list with the script pointing into the mount
$newArgs = @()
for ($k = 0; $k -lt $args.Count; $k++) {
    if ($k -eq $scriptIdx) {
        $newArgs += "$WorkCtr/$wslBase"
    } else {
        $newArgs += [string]$args[$k]
    }
}
$quoted = ($newArgs | ForEach-Object { Quote-Bash ([string]$_) }) -join ' '

$cmd = "docker run --rm --user 1000:1000 -e HOME=/tmp " +
       "-v $(Quote-Bash $RepoWsl):$RepoCtr " +
       "-v $(Quote-Bash $wslDir):$WorkCtr " +
       "-w $WorkCtr " +
       "$Image $RepoCtr/build/dev/py23 $quoted"

Wsl-Bash $cmd
exit $LASTEXITCODE
