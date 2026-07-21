@echo off
rem Sample commands demonstrating rdf.py against the bundled RDF files.
rem Run from the repo root:
rem     public\rw-sample-data\sample_commands.bat

setlocal enabledelayedexpansion

set REPO=%~dp0..\..
set RDF=%~dp0
set SCRIPTS=%REPO%\scripts\rdf.py

set TRACES=%RDF%sample_traces.rdf
set SUBSET=%RDF%sample_subset.rdf

echo ============================================================
echo 1. Info: inspect sample_traces.rdf
echo ============================================================
python "%SCRIPTS%" info "%TRACES%"

echo.
echo ============================================================
echo 2. Info: inspect sample_subset.rdf
echo ============================================================
python "%SCRIPTS%" info "%SUBSET%"

echo.
echo ============================================================
echo 3. Convert all slots in sample_traces.rdf (wide format)
echo    Scalar slots auto-generate a _labels.csv sidecar per output file.
echo ============================================================
for /f "usebackq delims=" %%S in (`python "%SCRIPTS%" slots "%TRACES%" --series-only`) do (
    set "SLOT=%%S"

    rem Build a safe filename: replace spaces and dots with underscores
    set "SAFE=%%S"
    set "SAFE=!SAFE: =_!"
    set "SAFE=!SAFE:.=_!"

    set "OUT=%RDF%output_!SAFE!_wide.csv"
    echo   slot : !SLOT!
    echo   out  : !OUT!
    python "%SCRIPTS%" convert "%TRACES%" --slot "!SLOT!" --output "!OUT!" --format wide
    echo.
)

echo ============================================================
echo 4. Convert all slots in sample_traces.rdf (stacked-header wide format)
echo    Output includes scalar label rows above wide data columns.
echo ============================================================
for /f "usebackq delims=" %%S in (`python "%SCRIPTS%" slots "%TRACES%" --series-only`) do (
    set "SLOT=%%S"

    set "SAFE=%%S"
    set "SAFE=!SAFE: =_!"
    set "SAFE=!SAFE:.=_!"

    set "OUT=%RDF%output_!SAFE!_stacked.csv"
    echo   slot : !SLOT!
    echo   out  : !OUT!
    python "%SCRIPTS%" convert "%TRACES%" --slot "!SLOT!" --output "!OUT!" --format stacked
    echo.
)

echo ============================================================
echo 5. Convert all slots in sample_traces.rdf (long format)
echo ============================================================
for /f "usebackq delims=" %%S in (`python "%SCRIPTS%" slots "%TRACES%" --series-only`) do (
    set "SLOT=%%S"

    set "SAFE=%%S"
    set "SAFE=!SAFE: =_!"
    set "SAFE=!SAFE:.=_!"

    set "OUT=%RDF%output_!SAFE!_long.csv"
    echo   slot : !SLOT!
    echo   out  : !OUT!
    python "%SCRIPTS%" convert "%TRACES%" --slot "!SLOT!" --output "!OUT!" --format long
    echo.
)

echo Done.

endlocal
