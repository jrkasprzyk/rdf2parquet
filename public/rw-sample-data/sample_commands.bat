@echo off
rem Sample commands demonstrating rdf2parquet against the bundled RDF files.
rem Run from anywhere:
rem     public\rw-sample-data\sample_commands.bat
rem
rem All output goes to %TEMP%\rdf2parquet-samples so this folder stays clean.

setlocal

set "RDF=%~dp0"
set "REPO=%~dp0..\.."
set "OUT=%TEMP%\rdf2parquet-samples"

rem Prefer a local build; fall back to whatever is on PATH.
set "EXE=%REPO%\build\windows\rdf2parquet.exe"
if not exist "%EXE%" (
    where rdf2parquet >nul 2>&1
    if errorlevel 1 (
        echo ERROR: rdf2parquet not found.
        echo Looked for "%EXE%" and for rdf2parquet on PATH.
        echo.
        echo Build it first, from a Developer PowerShell for VS 2022:
        echo     cmake --preset windows
        echo     cmake --build --preset windows
        exit /b 1
    )
    set "EXE=rdf2parquet"
)

if not exist "%OUT%" mkdir "%OUT%"

echo ============================================================
echo 0. Version
echo ============================================================
"%EXE%" --version || exit /b 1

echo.
echo ============================================================
echo 1. Convert sample_traces.rdf to the default long layout
echo ============================================================
"%EXE%" convert "%RDF%sample_traces.rdf" "%OUT%\traces.parquet" || exit /b 1
echo   wrote %OUT%\traces.parquet

echo.
echo ============================================================
echo 2. Info: schema, row count, row groups, key-value metadata
echo ============================================================
"%EXE%" info "%OUT%\traces.parquet" || exit /b 1

echo.
echo ============================================================
echo 3. Head: first 5 rows, tab-separated
echo ============================================================
"%EXE%" head "%OUT%\traces.parquet" -n 5 || exit /b 1

echo.
echo ============================================================
echo 4. To-CSV: the whole file as RFC-4180 CSV
echo ============================================================
"%EXE%" to-csv "%OUT%\traces.parquet" "%OUT%\traces.csv" || exit /b 1
echo   wrote %OUT%\traces.csv

echo.
echo ============================================================
echo 5. Wide layout: one file per series slot, plus scalars.parquet
echo    (rdf2parquet converts every slot at once - there is no
echo     per-slot selection flag)
echo ============================================================
"%EXE%" convert "%RDF%sample_subset.rdf" "%OUT%\subset_wide" --wide || exit /b 1
dir /b "%OUT%\subset_wide"

if not exist "%RDF%res.rdf" goto :done

echo.
echo ============================================================
echo 6. Real-scale sample: res.rdf
echo    400 traces x 60 monthly timesteps x 105 series slots
echo ============================================================
"%EXE%" convert "%RDF%res.rdf" "%OUT%\res.parquet" || exit /b 1
for %%A in ("%RDF%res.rdf")      do echo   source .rdf : %%~zA bytes
for %%A in ("%OUT%\res.parquet") do echo   parquet     : %%~zA bytes  (default zstd)

:done
echo.
echo Done. Output is in %OUT%
endlocal
