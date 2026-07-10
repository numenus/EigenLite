param(
    [string]$BitwigInputPort = "",
    [string]$BitwigOutputPort = "",
    [int]$UdpPort = 5005,
    [int]$LedPort = 5006,
    [string]$LoaderPath = "",
    [string]$ReceiverPath = "",
    [string]$FirmwarePath = "",
    [string]$BusId = "",
    [string]$WslDistro = "",
    [switch]$DebugReceiver,
    [switch]$SkipFirmware,
    [switch]$SkipAttach,
    [switch]$SkipReceiver,
    [switch]$SkipSink,
    [switch]$SkipLedForward,
    [switch]$Watch,
    [int]$WatchIntervalSeconds = 5,
    [switch]$StartWsl,
    [string]$WslArgs = ""
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

function Get-WslIp([string]$Distro) {
    $hostnameArgs = @()
    if ($Distro) {
        $hostnameArgs += @("-d", $Distro)
    }
    $hostnameArgs += @("hostname", "-I")
    try {
        $output = & wsl @hostnameArgs 2>&1
    } catch {
        return $null
    }
    if ($LASTEXITCODE -ne 0 -or -not $output) {
        return $null
    }
    $first = ($output | Out-String).Trim().Split(" ")[0]
    if ($first) {
        return $first
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

function Start-Receiver([string]$Receiver, [string]$PortName, [int]$PortNumber, [string]$SinkPortName, [bool]$UseSink, [bool]$UseDebug, [string]$ForwardHost, [int]$ForwardPort) {
    $quotedReceiver = $Receiver.Replace("'", "''")
    $quotedPortName = $PortName.Replace("'", "''")
    $pythonCmd = Resolve-PythonCommand
    $command = "$pythonCmd '$quotedReceiver' --out '$quotedPortName' --port $PortNumber"
    if ($UseSink) {
        $quotedSinkPortName = $SinkPortName.Replace("'", "''")
        $command = "$command --sink-in '$quotedSinkPortName'"
        if ($ForwardHost) {
            $command = "$command --forward-host $ForwardHost --forward-port $ForwardPort"
        }
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

function Invoke-PicoArm([bool]$SkipFirmwareStep, [bool]$SkipAttachStep, [string]$Loader, [string]$PythonCmd, [string]$Firmware, [string]$InitialBusId, [bool]$Quiet) {
    # Redoes firmware-load + usbipd bind/attach. Returns the resolved bus ID on
    # success, $null on failure. In -Quiet mode, failures are logged instead of
    # thrown, so a watch loop can keep retrying after a real unplug/replug
    # instead of killing the whole script.
    $busId = $InitialBusId

    function Fail([string]$Message) {
        if ($Quiet) {
            Write-Host "WARNING: $Message"
            return $false
        }
        throw $Message
    }

    if (-not $SkipFirmwareStep) {
        $usbState = Get-PicoUsbState -Loader $Loader -PythonCmd $PythonCmd
        if ($usbState.HasPreload) {
            Write-Host "Pre-load Pico detected. Loading firmware with $Loader"
            & $PythonCmd $Loader --firmware $Firmware
            if ($LASTEXITCODE -ne 0) {
                if (-not (Fail "Firmware load failed.")) { return $null }
            }
        } elseif ($usbState.HasPostload) {
            Write-Host "Post-load Pico already present. Skipping firmware load."
        } else {
            $existingBusId = $busId
            if (-not $existingBusId) {
                $existingBusId = Find-PicoBusId
            }
            if ($existingBusId) {
                Write-Host "No Windows-side Pico USB descriptor detected, but usbipd can already see Pico bus ID $existingBusId. Skipping firmware load."
                $busId = $existingBusId
            } else {
                Write-Host "No known Pico USB state detected. Attempting firmware load anyway."
                & $PythonCmd $Loader --firmware $Firmware
                if ($LASTEXITCODE -ne 0) {
                    if (-not (Fail "Firmware load failed.")) { return $null }
                }
            }
        }
    }

    if (-not $SkipAttachStep) {
        if (-not $busId) {
            $busId = Find-PicoBusId
        }
        if (-not $busId) {
            if (-not (Fail "Could not determine Pico usbipd bus ID. Run 'usbipd list' and pass -BusId manually.")) { return $null }
        }

        Write-Host "Attaching Pico bus ID $busId to WSL"
        $bindResult = Invoke-UsbipdCommand "bind --busid $busId"
        if ($bindResult.ExitCode -ne 0) {
            if (-not (Fail "usbipd bind failed. If bind needs elevation, rerun this PowerShell as Administrator.")) { return $null }
        }

        $attachResult = Invoke-UsbipdCommand "attach --wsl --busid $busId"
        if ($attachResult.ExitCode -ne 0) {
            $attachText = $attachResult.Output
            if ($attachText -match "already attached to a client") {
                Write-Host "Pico bus ID $busId is already attached to WSL. Continuing."
            } elseif (-not (Fail "usbipd attach failed. If bind/attach needs elevation, rerun this PowerShell as Administrator.")) {
                return $null
            }
        }
    }

    return $busId
}

$BusId = Invoke-PicoArm -SkipFirmwareStep $SkipFirmware -SkipAttachStep $SkipAttach -Loader $LoaderPath -PythonCmd $pythonCmd -Firmware $FirmwarePath -InitialBusId $BusId -Quiet $false
if ((-not $BusId) -and (-not $SkipAttach)) {
    throw "Could not arm the Pico (firmware load / usbipd attach failed)."
}

if (-not $SkipReceiver) {
    Stop-StaleReceiverProcesses
    Write-Host "Starting UDP MIDI receiver for Bitwig input port '$BitwigInputPort' on UDP $UdpPort"

    $wslIp = ""
    if ((-not $SkipSink) -and (-not $SkipLedForward)) {
        $wslIp = Get-WslIp -Distro $WslDistro
        if ($wslIp) {
            Write-Host "Also opening MIDI sink on Bitwig output port '$BitwigOutputPort', forwarding LED control to WSL $wslIp`:$LedPort"
        } else {
            Write-Host "Also opening MIDI sink on Bitwig output port '$BitwigOutputPort' (could not resolve WSL IP; LED control forwarding disabled -- pass -WslDistro or check 'wsl hostname -I')"
        }
    } elseif (-not $SkipSink) {
        Write-Host "Also opening MIDI sink on Bitwig output port '$BitwigOutputPort' (LED control forwarding skipped, -SkipLedForward)"
    }

    Start-Receiver -Receiver $ReceiverPath -PortName $BitwigInputPort -PortNumber $UdpPort -SinkPortName $BitwigOutputPort -UseSink (-not $SkipSink) -UseDebug $DebugReceiver -ForwardHost $wslIp -ForwardPort $LedPort
}

Write-Host ""
Write-Host "Windows side is ready."
Write-Host "Bitwig input port:  $BitwigInputPort"
Write-Host "Bitwig output port: $BitwigOutputPort"

if ($StartWsl) {
    $distroArg = ""
    if ($WslDistro) {
        $distroArg = "-d $WslDistro "
    }
    $wslCommand = "wsl.exe $($distroArg)-- /home/hotpo/repos/EigenLite/tools/pico-online-wsl.sh $WslArgs"
    Write-Host "Starting WSL bridge: $wslCommand"
    Start-Process -FilePath "powershell.exe" -ArgumentList @("-NoExit", "-Command", $wslCommand) | Out-Null
} else {
    Write-Host "Next: in WSL run ./tools/pico-online-wsl.sh"
}

if ($Watch) {
    Write-Host ""
    Write-Host "Watching for Pico reconnects every $WatchIntervalSeconds s (Ctrl+C to stop)..."
    while ($true) {
        Start-Sleep -Seconds $WatchIntervalSeconds
        try {
            $state = Get-PicoUsbState -Loader $LoaderPath -PythonCmd $pythonCmd
        } catch {
            continue
        }
        if ($state.HasPreload -and -not $state.HasPostload) {
            Write-Host "Pico reconnect detected (pre-load state) -- re-arming."
            $newBusId = Invoke-PicoArm -SkipFirmwareStep $false -SkipAttachStep $SkipAttach -Loader $LoaderPath -PythonCmd $pythonCmd -Firmware $FirmwarePath -InitialBusId "" -Quiet $true
            if ($newBusId) {
                Write-Host "Pico re-armed (bus ID $newBusId). The already-running WSL bridge should reconnect via EigenLite's own discovery thread -- no restart needed on either side."
            } else {
                Write-Host "Re-arm attempt failed; will retry on the next watch tick."
            }
        }
    }
}
