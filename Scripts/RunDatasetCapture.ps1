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
& $resolvedEditor @arguments
exit $LASTEXITCODE
