# Native Windows bring-up for the Pico -> Bitwig bridge. No WSL, no usbipd,
# no admin shell.
#
#   Pico USB (WinUSB/libusbK driver, installed once via Zadig)
#     -> pico-udp-midi-bridge.exe (this window)
#     -> UDP 127.0.0.1:$UdpPort -> udp_midi_receiver.py -> loopMIDI "Pico In" -> Bitwig
#   Bitwig -> loopMIDI "Pico Out" -> receiver sink -> UDP 127.0.0.1:$LedPort -> exe -> LEDs
#
# One-time setup (see docs/reference/pico-native-windows.md):
#   - Zadig: WinUSB (or libusbK) driver on BOTH Pico USB identities
#     (pre-firmware 2139:0001 and post-firmware 2139:0101)
#   - usbipd unbind the Pico if it was bound for the WSL flow
#   - VC++ 2008 SP1 x86 redistributable (pico_decoder_1_0_0.dll needs MSVCR90)
#   - pico_decoder_1_0_0.dll next to pico-udp-midi-bridge.exe
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File pico-native.ps1 `
#       -BitwigInputPort "Pico In" -BitwigOutputPort "Pico Out"

param(
    [string]$BitwigInputPort = "",
    [string]$BitwigOutputPort = "",
    [int]$UdpPort = 5005,
    [int]$LedPort = 5006,
    [string]$BridgeExe = "",
    [string]$ReceiverPath = "",
    [string]$Mode = "stable",
    [string]$DebugScope = "all",
    [switch]$DebugBridge,
    [switch]$DebugReceiver,
    [switch]$SkipReceiver,
    [switch]$SkipSink,
    [switch]$SkipLedForward
)

$ErrorActionPreference = "Stop"

function Resolve-RepoRoot {
    $scriptDir = $PSScriptRoot
    return (Resolve-Path (Join-Path $scriptDir "..")).ProviderPath
}

function Add-CandidatePath([ref]$List, [string]$BasePath, [string]$ChildPath) {
    if ($BasePath) {
        $List.Value += (Join-Path $BasePath $ChildPath)
    }
}

function Resolve-FirstExisting([string[]]$Candidates, [string]$Label) {
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return (Resolve-Path $candidate).ProviderPath
        }
    }
    throw "Could not find $Label. Checked: $($Candidates -join ', ')"
}

function Resolve-PythonCommand {
    foreach ($name in @("py", "python")) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) {
            return $cmd.Name
        }
    }
    throw "Could not find a Python launcher. Install Python for Windows and ensure 'py' or 'python' is on PATH."
}

function Get-ManagedProcesses([string[]]$Patterns) {
    $procs = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue
    foreach ($proc in $procs) {
        $cmd = $proc.CommandLine
        if (-not $cmd) {
            continue
        }
        foreach ($pattern in $Patterns) {
            if ($cmd -like "*$pattern*") {
                $proc
                break
            }
        }
    }
}

function Stop-StaleReceiverProcesses() {
    $patterns = @("udp_midi_receiver.py")
    $procs = @(Get-ManagedProcesses -Patterns $patterns)
    if ($procs.Count -eq 0) {
        return
    }

    Write-Host "Stopping stale Windows MIDI receiver process(es)"
    foreach ($proc in $procs) {
        Stop-Process -Id $proc.ProcessId -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 250
}

$scriptDir = (Resolve-Path $PSScriptRoot).ProviderPath
$repoRoot = ""
try { $repoRoot = Resolve-RepoRoot } catch { }

if (-not $BridgeExe) {
    $exeCandidates = @()
    Add-CandidatePath ([ref]$exeCandidates) $scriptDir "pico-udp-midi-bridge.exe"
    Add-CandidatePath ([ref]$exeCandidates) $repoRoot "build-win-native\release\bin\pico-udp-midi-bridge.exe"
    $BridgeExe = Resolve-FirstExisting $exeCandidates "pico-udp-midi-bridge.exe"
}

$exeDir = Split-Path -Parent $BridgeExe
if (-not (Test-Path (Join-Path $exeDir "pico_decoder_1_0_0.dll"))) {
    throw "pico_decoder_1_0_0.dll not found next to $BridgeExe (copy it from resources\picodecoder\windows\x86\)"
}

if (-not $ReceiverPath) {
    $receiverCandidates = @()
    Add-CandidatePath ([ref]$receiverCandidates) $scriptDir "udp_midi_receiver.py"
    Add-CandidatePath ([ref]$receiverCandidates) $repoRoot "tools\udp_midi_receiver.py"
    $ReceiverPath = Resolve-FirstExisting $receiverCandidates "udp_midi_receiver.py"
}

if (-not $BitwigInputPort) {
    throw "Pass -BitwigInputPort (the loopMIDI port Bitwig reads, e.g. 'Pico In')."
}
if ((-not $SkipSink) -and (-not $BitwigOutputPort)) {
    throw "Pass -BitwigOutputPort (the loopMIDI port Bitwig writes, e.g. 'Pico Out'), or -SkipSink."
}

if (-not $SkipReceiver) {
    Stop-StaleReceiverProcesses
    $pythonCmd = Resolve-PythonCommand
    $quotedReceiver = $ReceiverPath.Replace("'", "''")
    $quotedPortName = $BitwigInputPort.Replace("'", "''")
    $command = "$pythonCmd '$quotedReceiver' --out '$quotedPortName' --port $UdpPort"
    if (-not $SkipSink) {
        $quotedSinkPortName = $BitwigOutputPort.Replace("'", "''")
        $command = "$command --sink-in '$quotedSinkPortName'"
        if (-not $SkipLedForward) {
            $command = "$command --forward-host 127.0.0.1 --forward-port $LedPort"
        }
    }
    if ($DebugReceiver) {
        $command = "$command --debug"
    }
    Write-Host "Starting UDP MIDI receiver for '$BitwigInputPort' on UDP $UdpPort"
    Start-Process -FilePath "powershell.exe" -ArgumentList @("-NoExit", "-Command", $command) | Out-Null
}

$bridgeLedPort = $LedPort
if ($SkipSink -or $SkipLedForward) {
    $bridgeLedPort = 0
}
$debugFlag = 0
if ($DebugBridge) {
    $debugFlag = 1
}

Write-Host "Starting native bridge: $BridgeExe 127.0.0.1 $UdpPort $debugFlag $Mode $DebugScope $bridgeLedPort"
Write-Host "(Ctrl+C stops it; the receiver window stays up for reuse)"
& $BridgeExe 127.0.0.1 $UdpPort $debugFlag $Mode $DebugScope $bridgeLedPort
