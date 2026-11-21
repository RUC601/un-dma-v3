param([Parameter(Mandatory=$true)][string]$PackageDirectory)
$ErrorActionPreference='Stop'
$manifest=Get-Content (Join-Path $PackageDirectory 'latest.json') -Raw | ConvertFrom-Json
$zip=Join-Path $PackageDirectory ('Unleashed-V3-'+$manifest.version+'-windows-x86_64.zip')
foreach ($asset in @($zip,($zip+'.sig'),(Join-Path $PackageDirectory 'latest.json'))) { if (-not (Test-Path -LiteralPath $asset)) { throw ('Missing release asset: '+$asset) } }
& gh release create ('v'+$manifest.version) --repo RUC601/un-dma-v3 --draft --title ('Unleashed V3 '+$manifest.version) --notes 'Portable Windows release.' $zip ($zip+'.sig') (Join-Path $PackageDirectory 'latest.json')
if ($LASTEXITCODE) { throw 'Creating the draft release failed' }
