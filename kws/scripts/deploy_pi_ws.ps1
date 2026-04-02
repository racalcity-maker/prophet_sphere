param(
    [Parameter(Mandatory = $false)]
    [string]$PiHost = "192.168.43.242",

    [Parameter(Mandatory = $false)]
    [string]$PiUser = "prophet_master",

    [Parameter(Mandatory = $false)]
    [string]$RemoteDir = "~/orb_ws",

    [Parameter(Mandatory = $false)]
    [string[]]$IncludePaths = @(
        "kws/pi_ws",
        "docs/texts",
        "kws/scripts/pi_mic_ws_server.py",
        "kws/scripts/pi_oracle_llm_smoke.py",
        "kws/scripts/pi_tts_ab_test.py",
        "kws/scripts/pi_install_piper.sh"
    ),

    [Parameter(Mandatory = $false)]
    [switch]$InstallDeps
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Resolve-Path (Join-Path $ScriptDir "..\..")
$ArchivePath = Join-Path $env:TEMP ("orb_pi_ws_sync_{0}.tar.gz" -f (Get-Date -Format "yyyyMMdd_HHmmss"))

Write-Host "Repo root: $RepoRoot"
Write-Host "Preparing archive from include paths:"
foreach ($p in $IncludePaths) {
    Write-Host "  - $p"
    $localPath = Join-Path $RepoRoot $p
    if (-not (Test-Path $localPath)) {
        throw "Include path not found: $p ($localPath)"
    }
}

if (Test-Path $ArchivePath) {
    Remove-Item $ArchivePath -Force
}

& tar -czf $ArchivePath -C $RepoRoot @IncludePaths

$LocalHash = (Get-FileHash -Algorithm SHA256 $ArchivePath).Hash.ToLower()
Write-Host "Archive created: $ArchivePath"
Write-Host "Local SHA256: $LocalHash"

$Remote = "$PiUser@$PiHost"
$RemoteArchive = "/tmp/orb_pi_ws_sync.tar.gz"

Write-Host "Uploading archive to $Remote ..."
& scp $ArchivePath "${Remote}:$RemoteArchive"
if ($LASTEXITCODE -ne 0) {
    throw "scp failed with exit code $LASTEXITCODE"
}

Write-Host "Running remote verify/extract/deps in a single SSH session ..."
$InstallFlag = if ($InstallDeps) { "1" } else { "0" }
$ResolvedRemoteDir = $RemoteDir
if ($ResolvedRemoteDir -eq "~") {
    $ResolvedRemoteDir = '$HOME'
} elseif ($ResolvedRemoteDir.StartsWith("~/")) {
    $ResolvedRemoteDir = '$HOME/' + $ResolvedRemoteDir.Substring(2)
}

$RemoteCmd = @(
    "set -euo pipefail",
    "remote_archive='$RemoteArchive'",
    "remote_dir=""$ResolvedRemoteDir""",
    "expected_sha='$LocalHash'",
    "install_deps='$InstallFlag'",
    "remote_sha=`$(sha256sum ""`$remote_archive"" | cut -d' ' -f1 | tr '[:upper:]' '[:lower:]')",
    "[ ""`$remote_sha"" = ""`$expected_sha"" ] || { echo ""Checksum mismatch: remote=`$remote_sha expected=`$expected_sha"" >&2; exit 3; }",
    "echo ""Checksum OK: `$remote_sha""",
    "mkdir -p ""`$remote_dir""",
    "tar -xzf ""`$remote_archive"" -C ""`$remote_dir""",
    "rm -f ""`$remote_archive""",
    "if [ ""`$install_deps"" = ""1"" ]; then python3 -m venv ""`$remote_dir/.venv""; ""`$remote_dir/.venv/bin/python"" -m pip install --upgrade pip; ""`$remote_dir/.venv/bin/pip"" install -r ""`$remote_dir/kws/pi_ws/requirements.txt""; fi"
) -join "; "

& ssh $Remote $RemoteCmd
if ($LASTEXITCODE -ne 0) {
    throw "remote ssh step failed with exit code $LASTEXITCODE"
}

Remove-Item $ArchivePath -Force
Write-Host "Done."
Write-Host ""
Write-Host "Run on Pi:"
Write-Host "  cd $RemoteDir"
Write-Host "  . .venv/bin/activate"
Write-Host "  chmod +x kws/scripts/pi_install_piper.sh && kws/scripts/pi_install_piper.sh"
Write-Host '  LD_LIBRARY_PATH=~/orb_ws/piper:~/orb_ws/piper/lib:$LD_LIBRARY_PATH python kws/scripts/pi_mic_ws_server.py --host 0.0.0.0 --port 8765 --vosk-model ~/orb_ws/models/vosk-model-small-ru-0.22 --tts-backend silero --silero-language ru --silero-speaker-model v4_ru --silero-speaker xenia --tts-fx-preset mystic --tts-tail-fade-ms 20 --tts-tail-silence-ms 120'
