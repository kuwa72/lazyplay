param(
    [ValidateRange(1, 86400)][int]$Seconds = 30,
    [ValidateRange(1, 60)][int]$IntervalSeconds = 1,
    [int]$ProcessId = 0,
    [ValidateSet('idle', 'mirroring', 'unknown')][string]$Workload = 'unknown'
)

$ErrorActionPreference = 'Stop'
if ($IntervalSeconds -gt $Seconds) { throw 'IntervalSeconds must not exceed Seconds.' }
$processes = @(Get-Process -Name lazyplay -ErrorAction SilentlyContinue)
if ($ProcessId) { $processes = @($processes | Where-Object Id -eq $ProcessId) }
if ($processes.Count -ne 1) { throw 'Start lazyplay first; use -ProcessId if multiple instances are running.' }
$process = $processes[0]
$system = Get-CimInstance Win32_ComputerSystem
$os = Get-CimInstance Win32_OperatingSystem
$cpu = @(Get-CimInstance Win32_Processor | ForEach-Object { $_.Name.Trim() })
$gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion)
$binaryHash = (Get-FileHash -LiteralPath $process.Path -Algorithm SHA256).Hash
$logicalProcessors = [int]$system.NumberOfLogicalProcessors
$startedAt = [DateTimeOffset]::Now.ToString('o')
$process.Refresh()
$previousCpu = $process.TotalProcessorTime.TotalSeconds
$clock = [Diagnostics.Stopwatch]::StartNew()
$previousTime = 0.0
$samples = @()
while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
    $remaining = $Seconds - $clock.Elapsed.TotalSeconds
    Start-Sleep -Milliseconds ([int][Math]::Max(1, [Math]::Min($IntervalSeconds * 1000, $remaining * 1000)))
    $process.Refresh()
    if ($process.HasExited) { throw 'lazyplay exited during measurement.' }
    $elapsed = $clock.Elapsed.TotalSeconds
    $currentCpu = $process.TotalProcessorTime.TotalSeconds
    $duration = $elapsed - $previousTime
    $samples += [pscustomobject]@{
        elapsedSeconds = $elapsed
        intervalSeconds = $duration
        cpuSeconds = $currentCpu - $previousCpu
        cpuPercent = 100.0 * ($currentCpu - $previousCpu) / $duration / $logicalProcessors
        workingSetMiB = $process.WorkingSet64 / 1MB
        privateBytesMiB = $process.PrivateMemorySize64 / 1MB
    }
    $previousCpu = $currentCpu
    $previousTime = $elapsed
}
$clock.Stop()
$cpuStats = $samples | Measure-Object cpuPercent -Minimum -Maximum
$workingSetStats = $samples | Measure-Object workingSetMiB -Average -Maximum
$privateStats = $samples | Measure-Object privateBytesMiB -Average -Maximum
[pscustomobject]@{
    startedAt = $startedAt
    workload = $Workload
    processId = $process.Id
    binarySha256 = $binaryHash
    durationSeconds = [Math]::Round($previousTime, 3)
    sampleCount = $samples.Count
    environment = [pscustomobject]@{
        manufacturer = $system.Manufacturer
        model = $system.Model
        windows = $os.Caption
        windowsBuild = $os.BuildNumber
        cpu = $cpu
        logicalProcessors = $logicalProcessors
        memoryGiB = [Math]::Round($system.TotalPhysicalMemory / 1GB, 1)
        gpu = $gpu
    }
    summary = [pscustomobject]@{
        cpuAveragePercent = [Math]::Round(100.0 * ($samples | Measure-Object cpuSeconds -Sum).Sum / $previousTime / $logicalProcessors, 3)
        cpuMinimumPercent = [Math]::Round($cpuStats.Minimum, 3)
        cpuMaximumPercent = [Math]::Round($cpuStats.Maximum, 3)
        workingSetAverageMiB = [Math]::Round($workingSetStats.Average, 2)
        workingSetMaximumMiB = [Math]::Round($workingSetStats.Maximum, 2)
        privateBytesAverageMiB = [Math]::Round($privateStats.Average, 2)
        privateBytesMaximumMiB = [Math]::Round($privateStats.Maximum, 2)
    }
    samples = $samples
} | ConvertTo-Json -Depth 5
