@echo off
REM === ESP-IDF Unit Test Build Script for ecg_adc ===
REM Chỉ include ecg_adc + ecg_common (tránh kéo vào ecg_network/ecg_display bị lỗi build)

set IDF_PATH=D:\esp\env\Espressif\frameworks\esp-idf-v5.5.4
set IDF_PYTHON_ENV_PATH=D:\esp\env\Espressif\python_env\idf5.5_py3.11_env
set PYTHON=%IDF_PYTHON_ENV_PATH%\Scripts\python.exe

set COMP=d:\esp\code\ecg_monitor\ecg_monitor\components

set PATH=D:\esp\env\Espressif\tools\cmake\3.30.2\bin
set PATH=%PATH%;D:\esp\env\Espressif\tools\ninja\1.12.1
set PATH=%PATH%;D:\esp\env\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin
set PATH=%PATH%;D:\esp\env\Espressif\tools\idf-python\3.11.2
set PATH=%PATH%;%IDF_PYTHON_ENV_PATH%\Scripts
set PATH=%PATH%;%SystemRoot%\system32;%SystemRoot%

echo [INFO] IDF_PATH = %IDF_PATH%
echo [INFO] PYTHON   = %PYTHON%

cd /d D:\esp\env\Espressif\frameworks\esp-idf-v5.5.4\tools\unit-test-app

REM ── Bước 1: set-target esp32s3 ────────────────────────
echo.
echo [STEP 1] set-target esp32s3...
%PYTHON% %IDF_PATH%\tools\idf.py ^
    -DEXTRA_COMPONENT_DIRS="%COMP%\ecg_adc;%COMP%\ecg_common" ^
    set-target esp32s3

if errorlevel 1 (
    echo [ERROR] set-target failed!
    exit /b 1
)

REM ── Bước 2: build chỉ ecg_adc tests ─────────────────
echo.
echo [STEP 2] Building ecg_adc tests...
%PYTHON% %IDF_PATH%\tools\idf.py ^
    -DTESTS_ALL=0 ^
    -DTEST_COMPONENTS=ecg_adc ^
    -DEXTRA_COMPONENT_DIRS="%COMP%\ecg_adc;%COMP%\ecg_common" ^
    build

if errorlevel 1 (
    echo [ERROR] Build failed! Xem log bên trên.
    exit /b 1
)

echo.
echo [OK] Build thanh cong!
echo.
echo [STEP 3] Flash + Monitor tren COM5:
echo    %PYTHON% %IDF_PATH%\tools\idf.py -p COM5 flash monitor
