[CmdletBinding()]
param(
    [string]$Map = '/Game/FirstPerson/Lvl_FirstPerson',

    [string]$Project = (Join-Path $PSScriptRoot '..\..\..\UnrealCodeDemo.uproject'),

    [string]$Editor = '',

    [string]$Python = 'python'
)

$ErrorActionPreference = 'Stop'
$resolvedProject = (Resolve-Path -LiteralPath $Project).Path
$projectRoot = Split-Path -Parent $resolvedProject
$highJob = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\Config\job.capture-order-high-validation.json')).Path
$lowJob = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\Config\job.capture-order-low-validation.json')).Path
$validator = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'ValidateDataset.py')).Path

if ([string]::IsNullOrWhiteSpace($Editor)) {
    $projectDescriptor = Get-Content -Raw -LiteralPath $resolvedProject | ConvertFrom-Json
    $engineAssociation = $projectDescriptor.EngineAssociation
    if ([string]::IsNullOrWhiteSpace($engineAssociation)) {
        throw 'The project has no EngineAssociation. Pass -Editor explicitly.'
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

$resolvedEditor = (Resolve-Path -LiteralPath $Editor).Path

function Invoke-CaptureOrderJob {
    param([Parameter(Mandatory = $true)][string]$JobPath)

    $jobDescriptor = Get-Content -Raw -LiteralPath $JobPath | ConvertFrom-Json
    $arguments = @(
        $resolvedProject
        $Map
        '-game'
        '-RenderOffscreen'
        '-unattended'
        '-NoSound'
        "-SRDatasetJob=$JobPath"
        '-SRDatasetAutoQuit'
        '-log'
        "-ResX=$($jobDescriptor.hRResolution.x)"
        "-ResY=$($jobDescriptor.hRResolution.y)"
        '-ForceRes'
    )
    Write-Host "Capturing $($jobDescriptor.auxiliaryCaptureOrder): $JobPath"
    & $resolvedEditor @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Unreal capture failed with exit code $LASTEXITCODE for $JobPath"
    }
}

Invoke-CaptureOrderJob -JobPath $highJob
Invoke-CaptureOrderJob -JobPath $lowJob

$highRoot = Join-Path $projectRoot 'Saved\SRDataset\capture_order_high_validation'
$lowRoot = Join-Path $projectRoot 'Saved\SRDataset\capture_order_low_validation'

& $Python $validator $highRoot
if ($LASTEXITCODE -ne 0) {
    throw 'High-resolution-first dataset failed standalone validation.'
}

& $Python $validator $lowRoot `
    --compare $highRoot `
    --compare-mode capture-order `
    --report (Join-Path $lowRoot 'capture_order_validation_report.json')
if ($LASTEXITCODE -ne 0) {
    throw 'Capture-order invariance validation failed.'
}

Write-Host "Capture-order invariance passed: $lowRoot\capture_order_validation_report.json"
