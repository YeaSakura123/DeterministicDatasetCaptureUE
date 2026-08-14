[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Map,

    [Parameter(Mandatory = $true)]
    [string]$Job,

    [string]$Project = (Join-Path $PSScriptRoot '..\..\..\UnrealCodeDemo.uproject'),

    [string]$Editor = ''
)

$resolvedProject = (Resolve-Path -LiteralPath $Project -ErrorAction Stop).Path
$resolvedJob = (Resolve-Path -LiteralPath $Job -ErrorAction Stop).Path

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

$arguments = @(
    $resolvedProject
    $Map
    '-game'
    '-RenderOffscreen'
    '-unattended'
    '-NoSound'
    "-SRDatasetJob=$resolvedJob"
    '-SRDatasetAutoQuit'
    '-log'
)

Write-Host "Starting deterministic dataset capture: $resolvedJob"
& $resolvedEditor @arguments
exit $LASTEXITCODE
