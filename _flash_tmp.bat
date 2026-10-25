@echo off
set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.4
set IDF_TOOLS_PATH=C:\Espressif
set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env
call %IDF_PATH%\export.bat >nul 2>&1
cd /d C:\Users\mohut\xiaozhi-wha103
C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe %IDF_PATH%\tools\idf.py -p COM9 flash > flash_log.txt 2>&1
echo DONE=%ERRORLEVEL%
