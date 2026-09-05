[CmdletBinding()]
param(
    [string]$Map = '',

    [Parameter(Mandatory = $true)]
    [string]$Job,

    [string]$Project = '',

    [string]$Editor = '',

    [string]$LogPath = '',

    [switch]$UseMemoryDDC,

    [switch]$UseWorkspaceLocalDDC
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Project)) {
    $candidateProjects = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot '..\..\..') -Filter '*.uproject' -File)
    if ($candidateProjects.Count -ne 1) { throw 'Pass -Project: exactly one host .uproject could not be discovered.' }
    $Project = $candidateProjects[0].FullName
}
$resolvedProject = (Resolve-Path -LiteralPath $Project -ErrorAction Stop).Path
$resolvedJob = (Resolve-Path -LiteralPath $Job -ErrorAction Stop).Path
$jobDescriptor = Get-Content -Raw -LiteralPath $resolvedJob | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($Map)) { $Map = [string]$jobDescriptor.expectedMap }
if ([string]::IsNullOrWhiteSpace($Map) -or -not $Map.StartsWith('/')) { throw 'Pass -Map or set the job expectedMap to an absolute Unreal asset path.' }
if ($jobDescriptor.expectedMap -and $Map -ne $jobDescriptor.expectedMap) { throw '-Map must match the job expectedMap.' }

if ([string]::IsNullOrWhiteSpace($Editor)) {
    $projectDescriptor = Get-Content -Raw -LiteralPath $resolvedProject | ConvertFrom-Json
    $engineAssociation = $projectDescriptor.EngineAssociation
    if ([string]::IsNullOrWhiteSpace($engineAssociation)) {
        throw "The project has no EngineAssociation. Pass -Editor explicitly."
    }

    $engineRegistryKeys = @(
        "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$engineAssociation"
        "HKLM:\SOFTWARE\WOW6432Node\EpicGames\Unreal Engine\$engineAssociation"
    )
    foreach ($registryKey in $engineRegistryKeys) {
        $engineRoot = (Get-ItemProperty -LiteralPath $registryKey -ErrorAction SilentlyContinue).InstalledDirectory
        if (-not [string]::IsNullOrWhiteSpace($engineRoot)) {
            $candidateEditor = Join-Path $engineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
            if (Test-Path -LiteralPath $candidateEditor) {
                $Editor = $candidateEditor
                break
            }
        }
    }

    if ([string]::IsNullOrWhiteSpace($Editor)) {
        throw "Could not locate Unreal Engine $engineAssociation. Pass -Editor explicitly."
    }
}

$resolvedEditor = (Resolve-Path -LiteralPath $Editor -ErrorAction Stop).Path
$projectRoot = Split-Path -Parent $resolvedProject
if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $LogPath = Join-Path $projectRoot ("Saved\SRDatasetLogs\{0}-{1}.log" -f [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'), [Guid]::NewGuid().ToString('N'))
}
$LogPath = [IO.Path]::GetFullPath($LogPath)
if (Test-Path -LiteralPath $LogPath) { throw "LogPath already exists; choose a new run log: $LogPath" }
New-Item -ItemType Directory -Path (Split-Path -Parent $LogPath) -Force | Out-Null
$shaderWorkingDirectory = Join-Path $projectRoot 'Saved\ShaderWorkingDirectory'
New-Item -ItemType Directory -Path $shaderWorkingDirectory -Force | Out-Null
if ($UseMemoryDDC -and $UseWorkspaceLocalDDC) {
    throw 'UseMemoryDDC and UseWorkspaceLocalDDC are mutually exclusive.'
}

$arguments = @(
    $resolvedProject
    $Map
    '-game'
    '-RenderOffscreen'
    '-unattended'
    '-NoSound'
    "-ShaderWorkingDir=$shaderWorkingDirectory"
    "-SRDatasetJob=$resolvedJob"
    '-SRDatasetAutoQuit'
    "-abslog=$LogPath"
)

if ($UseMemoryDDC) {
    $arguments += '-DDC-ForceMemoryCache'
}
elseif ($UseWorkspaceLocalDDC) {
    $workspaceDDC = Join-Path $projectRoot 'Saved\DerivedDataCache'
    New-Item -ItemType Directory -Path $workspaceDDC -Force | Out-Null
    Set-Item -Path 'Env:UE-LocalDataCachePath' -Value $workspaceDDC
    $arguments += '-ddc=InstalledNoZenLocalFallback'
}

if ($jobDescriptor.bCaptureMainViewTemporalDiagnostics -eq $true) {
    $arguments += "-ResX=$($jobDescriptor.hRResolution.x)"
    $arguments += "-ResY=$($jobDescriptor.hRResolution.y)"
    $arguments += '-ForceRes'
}

Write-Host "Starting deterministic dataset capture: $resolvedJob"
$captureStartedUtc = [DateTime]::UtcNow
# Start-Process joins ArgumentList into one Windows command line. Quote each
# value using CommandLineToArgvW rules, including paths containing spaces.
$quotedArguments = foreach ($argument in $arguments) {
    '"' + [regex]::Replace([regex]::Replace($argument, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}
$captureProcess = Start-Process -FilePath $resolvedEditor -ArgumentList ($quotedArguments -join ' ') -WindowStyle Hidden -PassThru
$captureProcess.WaitForExit()
$editorExitCode = $captureProcess.ExitCode
if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) { throw "Unreal produced no run log (exit $editorExitCode): $LogPath" }
$captureLog = Get-Content -Raw -LiteralPath $LogPath
if ($captureLog -match 'LogSRDataset:\s*Error:|Fatal error:') {
    throw "Capture failed even if the Unreal process returned zero. Inspect $LogPath"
}

$configuredOutput = [string]$jobDescriptor.outputDirectory
if ([string]::IsNullOrWhiteSpace($configuredOutput)) {
    Write-Error 'The capture job has no outputDirectory.'
    exit 1
}
$resolvedOutput = if ([IO.Path]::IsPathRooted($configuredOutput)) {
    [IO.Path]::GetFullPath($configuredOutput)
}
else {
    [IO.Path]::GetFullPath((Join-Path $projectRoot $configuredOutput))
}
$manifestPath = Join-Path $resolvedOutput 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    Write-Error "Dataset capture produced no manifest: $manifestPath (editor exit code $editorExitCode)"
    exit 1
}
$manifestInfo = Get-Item -LiteralPath $manifestPath
if ($manifestInfo.LastWriteTimeUtc -lt $captureStartedUtc.AddSeconds(-2) -and
    -not ($jobDescriptor.bResume -eq $true -and $captureLog -match 'Reused \d+ verified spatial samples with their original manifest unchanged')) {
    Write-Error "Dataset manifest was not updated by this run: $manifestPath"
    exit 1
}
try {
    $captureManifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
}
catch {
    Write-Error "Dataset manifest is unreadable: $manifestPath`n$($_.Exception.Message)"
    exit 1
}

$captureState = [string]$captureManifest.state
$capturedSamples = [int]$captureManifest.capturedSamples
if ($captureState -ne 'Completed') {
    $captureError = [string]$captureManifest.error
    Write-Error "Dataset capture state is '$captureState' after $capturedSamples sample(s): $captureError"
    exit 1
}
if ($capturedSamples -le 0 -or @($captureManifest.frames).Count -ne $capturedSamples -or $captureManifest.pendingSamples -gt 0) {
    throw "Completed manifest has an inconsistent published frame count: $manifestPath"
}
if ($editorExitCode -ne 0) {
    Write-Error "Dataset manifest completed, but Unreal Editor returned exit code $editorExitCode."
    exit $editorExitCode
}

Write-Host "Dataset capture completed: $capturedSamples sample(s); manifest=$manifestPath; log=$LogPath"
exit 0
