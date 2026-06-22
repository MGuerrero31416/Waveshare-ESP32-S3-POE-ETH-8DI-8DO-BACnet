$py='C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe'
$idf_path='C:\Espressif\frameworks\esp-idf-v5.5.1'
$activate = Join-Path $idf_path 'tools\activate.py'
Write-Host "Using python: $py"
Write-Host "Running activate: $activate"
$exports = & $py $activate --export
$exports | Out-File -FilePath idf_export.ps1 -Encoding ASCII
Write-Host "Sourcing exported environment"
. .\idf_export.ps1
$idf_script = Join-Path $idf_path 'tools\idf.py'
Write-Host "Running: $py $idf_script fullclean"
& $py $idf_script fullclean
Write-Host "Running: $py $idf_script build"
& $py $idf_script build 2>&1 | Tee-Object -FilePath build.log
Write-Host "Build finished, log at build.log"
