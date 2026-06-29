@echo off
REM === ESP-IDF Build Script for ecg_monitor ===
REM Paths for ESP-IDF v5.5.4 on this machine

set IDF_PATH=D:\esp\env\Espressif\frameworks\esp-idf-v5.5.4
set IDF_PYTHON_ENV_PATH=D:\esp\env\Espressif\python_env\idf5.5_py3.11_env
set PYTHON=%IDF_PYTHON_ENV_PATH%\Scripts\python.exe

set PATH=D:\esp\env\Espressif\tools\cmake\3.30.2\bin
set PATH=%PATH%;D:\esp\env\Espressif\tools\ninja\1.12.1
set PATH=%PATH%;D:\esp\env\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin
set PATH=%PATH%;D:\esp\env\Espressif\tools\idf-python\3.11.2
set PATH=%PATH%;%IDF_PYTHON_ENV_PATH%\Scripts
set PATH=%PATH%;%SystemRoot%\system32;%SystemRoot%

echo [INFO] IDF_PATH = %IDF_PATH%
echo [INFO] PYTHON   = %PYTHON%
cmake --version
ninja --version

cd /d d:\esp\code\ecg_monitor\ecg_monitor
%PYTHON% %IDF_PATH%\tools\idf.py %*
