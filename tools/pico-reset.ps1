param(
    [switch]$ShutdownWsl,
    [switch]$RestartLoopMidi
)

$ErrorActionPreference = "Stop"

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

function Stop-ManagedProcesses([string[]]$Patterns, [string]$Label) {
    $procs = @(Get-ManagedProcesses -Patterns $Patterns)
    if ($procs.Count -eq 0) {
        Write-Host "No $Label processes found."
        return
    }

    Write-Host "Stopping $Label process(es)..."
    foreach ($proc in $procs) {
        Stop-Process -Id $proc.ProcessId -Force -ErrorAction SilentlyContinue
    }
}

function Restart-LoopMidiIfRunning() {
    $proc = Get-Process -Name "loopMIDI" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $proc) {
        Write-Host "loopMIDI is not running."
        return
    }

    $path = $proc.Path
    if (-not $path) {
        Write-Host "loopMIDI is running, but its executable path could not be resolved."
        return
    }

    Write-Host "Restarting loopMIDI..."
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
    Start-Process -FilePath $path | Out-Null
}

Stop-ManagedProcesses -Patterns @("udp_midi_receiver.py") -Label "Windows MIDI receiver"
Stop-ManagedProcesses -Patterns @("pico_loader.py") -Label "Pico loader"

if ($ShutdownWsl) {
    Write-Host "Shutting down WSL..."
    & wsl --shutdown
}

if ($RestartLoopMidi) {
    Restart-LoopMidiIfRunning
}

Write-Host "Pico Windows-side reset complete."
