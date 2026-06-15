@echo off
REM Double-click launcher for check_memory.ps1
REM Runs PowerShell in STA mode (required for WinForms) and bypasses the
REM execution policy for this one script only.
powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0check_memory.ps1"
