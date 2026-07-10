param(
    [string]$BitwigInputPort = "",
    [string]$BitwigOutputPort = "",
    [int]$UdpPort = 5005,
    [string]$LoaderPath = "",
    [string]$ReceiverPath = "",
    [string]$FirmwarePath = "",
    [string]$BusId = "",
    [switch]$DebugReceiver,
    [switch]$SkipFirmware,
    [switch]$SkipAttach,
    [switch]$SkipReceiver,
    [switch]$SkipSink
)

$ErrorActionPreference = "Stop"

function Resolve-RepoRoot {
    $scriptDir = $PSScriptRoot
    return (Resolve-Path (Join-Path $scriptDir "..")).ProviderPath
}

function Resolve-ScriptDir {
    return (Resolve-Path $PSScriptRoot).ProviderPath
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

function Find-PicoBusId {
    $usbipd = Get-Command usbipd -ErrorAction SilentlyContinue
    if (-not $usbipd) {
        throw "usbipd is not installed or not on PATH."
    }

    $lines = & usbipd list
    foreach ($line in $lines) {
        if ($line -match '^\s*([0-9-]+)\s+.*(2139:0101|BECA:0101|Eigenharp|Pico)') {
            return $Matches[1]
        }
    }
    return $null
}

function Get-PicoUsbState([string]$Loader, [string]$PythonCmd) {
    $output = & $PythonCmd $Loader --list 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to query Pico USB state via pico_loader.py --list"
    }

    $text = ($output | Out-String)
    return [pscustomobject]@{
        HasPreload  = ($text -match "VID_2139&PID_0001") -or ($text -match "VID_04B4&PID_6473")
        HasPostload = ($text -match "VID_2139&PID_0101") -or ($text -match "VID_BECA&PID_0101")
        RawOutput   = $text
    }
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

function Invoke-UsbipdCommand([string]$Arguments) {
    $quoted = "usbipd $Arguments 2>&1"
    $output = & cmd /c $quoted
    return [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Output   = ($output | Out-String)
    }
}

function Start-Receiver([string]$Receiver, [string]$PortName, [int]$PortNumber, [string]$SinkPortName, [bool]$UseSink, [bool]$UseDebug) {
    $quotedReceiver = $Receiver.Replace("'", "''")
    $quotedPortName = $PortName.Replace("'", "''")
    $pythonCmd = Resolve-PythonCommand
    $command = "$pythonCmd '$quotedReceiver' --out '$quotedPortName' --port $PortNumber"
    if ($UseSink) {
        $quotedSinkPortName = $SinkPortName.Replace("'", "''")
        $command = "$command --sink-in '$quotedSinkPortName'"
    }
    if ($UseDebug) {
        $command = "$command --debug"
    }
    Start-Process -FilePath "powershell.exe" -ArgumentList @("-NoExit", "-Command", $command) | Out-Null
}

$repoRoot = Resolve-RepoRoot
$scriptDir = Resolve-ScriptDir
$pythonCmd = Resolve-PythonCommand

if (-not $BitwigInputPort) {
    throw "BitwigInputPort is required. Example: -BitwigInputPort 'Pico In' [-BitwigOutputPort 'Pico Out']"
}

if (-not $BitwigOutputPort) {
    $BitwigOutputPort = "$BitwigInputPort Out"
}

if (-not $LoaderPath) {
    $LoaderPath = Resolve-FirstExisting @(
        (Join-Path $scriptDir "pico_loader.py"),
        (Join-Path $repoRoot "tools\pico_loader.py")
    ) "pico_loader.py"
} else {
    $LoaderPath = Resolve-FirstExisting @($LoaderPath) "pico_loader.py"
}

if (-not $ReceiverPath) {
    $ReceiverPath = Resolve-FirstExisting @(
        (Join-Path $scriptDir "udp_midi_receiver.py"),
        (Join-Path $repoRoot "tools\udp_midi_receiver.py")
    ) "udp_midi_receiver.py"
} else {
    $ReceiverPath = Resolve-FirstExisting @($ReceiverPath) "udp_midi_receiver.py"
}

if (-not $FirmwarePath) {
    $firmwareCandidates = @()
    Add-CandidatePath ([ref]$firmwareCandidates) $scriptDir "pico.ihx"
    Add-CandidatePath ([ref]$firmwareCandidates) $repoRoot "eigenapi\resources\firmware\ihx\pico.ihx"
    $repoParent = ""
    if ($repoRoot) {
        $repoParent = Split-Path -Parent $repoRoot
    }
    Add-CandidatePath ([ref]$firmwareCandidates) $repoParent "EigenD\resources\pico.ihx"
    $FirmwarePath = Resolve-FirstExisting $firmwareCandidates "pico.ihx"
} else {
    $FirmwarePath = Resolve-FirstExisting @($FirmwarePath) "pico.ihx"
}

if (-not $SkipFirmware) {
    $usbState = Get-PicoUsbState -Loader $LoaderPath -PythonCmd $pythonCmd
    if ($usbState.HasPreload) {
        Write-Host "Pre-load Pico detected. Loading firmware with $LoaderPath"
        & $pythonCmd $LoaderPath --firmware $FirmwarePath
        if ($LASTEXITCODE -ne 0) {
            throw "Firmware load failed."
        }
    } elseif ($usbState.HasPostload) {
        Write-Host "Post-load Pico already present. Skipping firmware load."
    } else {
        $existingBusId = $BusId
        if (-not $existingBusId) {
            $existingBusId = Find-PicoBusId
        }
        if ($existingBusId) {
            Write-Host "No Windows-side Pico USB descriptor detected, but usbipd can already see Pico bus ID $existingBusId. Skipping firmware load."
            $BusId = $existingBusId
        } else {
            Write-Host "No known Pico USB state detected. Attempting firmware load anyway."
            & $pythonCmd $LoaderPath --firmware $FirmwarePath
            if ($LASTEXITCODE -ne 0) {
                throw "Firmware load failed."
            }
        }
    }
}

if (-not $SkipAttach) {
    if (-not $BusId) {
        $BusId = Find-PicoBusId
    }
    if (-not $BusId) {
        throw "Could not determine Pico usbipd bus ID. Run 'usbipd list' and pass -BusId manually."
    }

    Write-Host "Attaching Pico bus ID $BusId to WSL"
    $bindResult = Invoke-UsbipdCommand "bind --busid $BusId"
    if ($bindResult.ExitCode -ne 0) {
        throw "usbipd bind failed. If bind needs elevation, rerun this PowerShell as Administrator."
    }

    $attachResult = Invoke-UsbipdCommand "attach --wsl --busid $BusId"
    if ($attachResult.ExitCode -ne 0) {
        $attachText = $attachResult.Output
        if ($attachText -match "already attached to a client") {
            Write-Host "Pico bus ID $BusId is already attached to WSL. Continuing."
        } else {
            throw "usbipd attach failed. If bind/attach needs elevation, rerun this PowerShell as Administrator."
        }
    }
}

if (-not $SkipReceiver) {
    Stop-StaleReceiverProcesses
    Write-Host "Starting UDP MIDI receiver for Bitwig input port '$BitwigInputPort' on UDP $UdpPort"
    if (-not $SkipSink) {
        Write-Host "Also opening MIDI sink on Bitwig output port '$BitwigOutputPort'"
    }
    Start-Receiver -Receiver $ReceiverPath -PortName $BitwigInputPort -PortNumber $UdpPort -SinkPortName $BitwigOutputPort -UseSink (-not $SkipSink) -UseDebug $DebugReceiver
}

Write-Host ""
Write-Host "Windows side is ready."
Write-Host "Bitwig input port:  $BitwigInputPort"
Write-Host "Bitwig output port: $BitwigOutputPort"
Write-Host "Next: in WSL run ./tools/pico-online-wsl.sh"
