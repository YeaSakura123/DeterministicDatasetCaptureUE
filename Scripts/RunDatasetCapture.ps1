[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Map,

    [Parameter(Mandatory = $true)]
    [string]$Job,

    [string]$Project = (Join-Path $PSScriptRoot '..\..\..\UnrealCodeDemo.uproject'),

    [string]$Editor = '',

    [switch]$UseMemoryDDC,

    [switch]$UseWorkspaceLocalDDC
)

$resolvedProject = (Resolve-Path -LiteralPath $Project -ErrorAction Stop).Path
$resolvedJob = (Resolve-Path -LiteralPath $Job -ErrorAction Stop).Path
$jobDescriptor = Get-Content -Raw -LiteralPath $resolvedJob | ConvertFrom-Json

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
    '-log'
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
& $resolvedEditor @arguments
$editorExitCode = $LASTEXITCODE

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
if ($manifestInfo.LastWriteTimeUtc -lt $captureStartedUtc.AddSeconds(-2)) {
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
if ($editorExitCode -ne 0) {
    Write-Error "Dataset manifest completed, but Unreal Editor returned exit code $editorExitCode."
    exit $editorExitCode
}

Write-Host "Dataset capture completed: $capturedSamples sample(s); manifest=$manifestPath"
exit 0
