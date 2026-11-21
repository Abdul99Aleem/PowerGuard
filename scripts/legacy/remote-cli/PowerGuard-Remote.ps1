# POWERGUARD  /  TEAM SCHEDULIX
# --------------------------------------------------------------------------
# PowerGuard-Remote.ps1
# Original software (c) 2026 Abdul Aleem. MIT License.
# Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.

param(
    [string]$BoardIP = '10.78.56.80',
    [string]$User = 'root'
)
$ErrorActionPreference = 'Stop'
$remote = "$User@$BoardIP"
$localMenu = Join-Path $PSScriptRoot 'pg-remote-menu.sh'
if (-not (Test-Path -LiteralPath $localMenu)) { throw "Missing script: $localMenu" }
foreach ($cmd in @('ssh', 'scp')) {
    if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) {
        throw "$cmd is not installed. Enable Windows OpenSSH Client first."
    }
}
Write-Host "Connecting to PowerGuard QNX target $remote" -ForegroundColor Cyan
Write-Host 'You will be asked for the root password by SSH/SCP. It is not stored in this file.'
Write-Host 'First connection: check the host fingerprint with your team before accepting.'
Write-Host 'Uploading the remote CLI menu to /tmp (does not modify the product binaries)...'
& scp -o ConnectTimeout=12 -- $localMenu "${remote}:/tmp/pg-remote-menu.sh"
if ($LASTEXITCODE -ne 0) { throw "SCP failed (exit $LASTEXITCODE); nothing was run on the board." }
Write-Host 'Starting interactive remote menu. All inputs are sent to the QNX board.' -ForegroundColor Green
& ssh -tt -o ConnectTimeout=12 -- $remote 'sh /tmp/pg-remote-menu.sh'
if ($LASTEXITCODE -ne 0) { Write-Warning "SSH exited with code $LASTEXITCODE" }
