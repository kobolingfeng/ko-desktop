@echo off
chcp 65001 > nul
setlocal enabledelayedexpansion

:: ============================================================
::  KO Desktop (动态壁纸) 发布脚本
::    · 构建单文件 exe
::    · 上传到 r2:altv/kopanel/
::    · 只更新 update.json 里的 kodesktop_* 字段（不影响 kopanel / widget / beta）
:: ============================================================

:: ── rclone 路径（优先 RCLONE_PATH，再 PATH，最后 winget 安装位置）──
set RCLONE=
if defined RCLONE_PATH (
    set RCLONE=%RCLONE_PATH%
) else (
    where rclone >nul 2>&1
    if !errorlevel! equ 0 (
        for /f "tokens=*" %%p in ('where rclone 2^>nul') do (
            if not defined RCLONE set "RCLONE=%%p"
        )
    ) else (
        for /f "tokens=*" %%p in ('dir /s /b "%LOCALAPPDATA%\Microsoft\WinGet\Packages\Rclone*\rclone.exe" 2^>nul') do (
            if not defined RCLONE set "RCLONE=%%p"
        )
    )
)
if not defined RCLONE (
    echo [×] rclone 未找到，请安装: winget install rclone.rclone
    pause
    exit /b 1
)

:: 切到项目根
cd /d "%~dp0\.."
set PROJECT_ROOT=%CD%

echo ========================================
echo   KO Desktop 动态壁纸 发布脚本
echo ========================================
echo.

:: ========== 1/4 · 读线上现有版本（只读取 kodesktop_* 字段做提示）==========
echo [1/4] 获取线上当前版本...
set OLD_VERSION=无
if not exist "upload" mkdir upload
"!RCLONE!" copy r2:altv/kopanel/update.json upload --quiet 2>nul
if exist "upload\update.json" (
    for /f "delims=" %%v in ('powershell -NoProfile -Command "try { $j = Get-Content 'upload\update.json' -Raw | ConvertFrom-Json; if ($j.kodesktop_version) { $j.kodesktop_version } else { '无' } } catch { '无' }"') do set OLD_VERSION=%%v
)
echo   当前线上版本: !OLD_VERSION!
echo.

:: ========== 2/4 · 交互输入新版本 ==========
echo [2/4] 输入新版本信息
echo.
set /p NEW_VERSION="新版本号 (如 0.1.0): "
set /p NEW_NOTES="更新日志: "

if "%NEW_VERSION%"=="" (
    echo [×] 版本号不能为空
    pause
    exit /b 1
)

echo.
echo ----------------------------------------
echo   即将发布: kodesktop v%NEW_VERSION%
echo   更新日志: %NEW_NOTES%
echo ----------------------------------------
echo.
set /p CONFIRM="确认发布? (y/n): "
if /i not "%CONFIRM%"=="y" (
    echo 已取消
    pause
    exit /b 0
)

:: 同步 package.json 版本号
powershell -NoProfile -Command "$f='package.json'; $c=[System.IO.File]::ReadAllText($f); $c=$c -replace '\"version\":\s*\"[0-9.]+\"', '\"version\": \"%NEW_VERSION%\"'; [System.IO.File]::WriteAllText($f,$c,(New-Object System.Text.UTF8Encoding $false))"

:: ========== 3/4 · 构建单文件 ==========
echo.
echo [3/4] 构建单文件 exe（可以去摸鱼了）...
echo.

:: 先杀进程防 exe 被锁
taskkill /F /IM "kodesktop.exe" >nul 2>&1
taskkill /F /IM "app.exe" >nul 2>&1
timeout /t 1 >nul

call bun run build:single
if errorlevel 1 (
    echo [×] 编译失败
    pause
    exit /b 1
)

set BUILD_EXE=dist\kodesktop.exe
if not exist "%BUILD_EXE%" (
    echo [×] 找不到 %BUILD_EXE%
    pause
    exit /b 1
)

:: 重命名到发布名
set DIST_NAME=kodesktop_%NEW_VERSION%.exe
copy /Y "%BUILD_EXE%" "upload\%DIST_NAME%" > nul

for %%a in ("upload\%DIST_NAME%") do set FILE_SIZE=%%~za
echo   产物: %DIST_NAME% (!FILE_SIZE! 字节)

:: 发布日期
for /f "tokens=*" %%d in ('powershell -NoProfile -Command "Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ'"') do set PUB_DATE=%%d

:: ========== 合并 update.json（只动 kodesktop_* 字段）==========
echo.
echo   合并 update.json (仅 kodesktop_* 字段)...

if exist "upload\update.json" (
    :: 已有 update.json → Add-Member -Force 追加/覆盖 kodesktop_* 字段，其他保留
    powershell -NoProfile -Command ^
      "$j = Get-Content 'upload\update.json' -Raw | ConvertFrom-Json;" ^
      "$j | Add-Member -NotePropertyName 'kodesktop_version'      -NotePropertyValue '%NEW_VERSION%' -Force;" ^
      "$j | Add-Member -NotePropertyName 'kodesktop_notes'        -NotePropertyValue '%NEW_NOTES%'   -Force;" ^
      "$j | Add-Member -NotePropertyName 'kodesktop_pub_date'     -NotePropertyValue '%PUB_DATE%'    -Force;" ^
      "$j | Add-Member -NotePropertyName 'kodesktop_download_url' -NotePropertyValue 'https://cdn.kobo07.cn/kopanel/%DIST_NAME%' -Force;" ^
      "$j | Add-Member -NotePropertyName 'kodesktop_file_size'    -NotePropertyValue %FILE_SIZE% -Force;" ^
      "$text = $j | ConvertTo-Json -Depth 10;" ^
      "[System.IO.File]::WriteAllText('upload\update.json', $text, (New-Object System.Text.UTF8Encoding $false))"
) else (
    :: 线上没 update.json → 创建一个只含 kodesktop_* 的新文件
    powershell -NoProfile -Command ^
      "$j = [ordered]@{ kodesktop_version='%NEW_VERSION%'; kodesktop_notes='%NEW_NOTES%'; kodesktop_pub_date='%PUB_DATE%'; kodesktop_download_url='https://cdn.kobo07.cn/kopanel/%DIST_NAME%'; kodesktop_file_size=%FILE_SIZE% };" ^
      "$text = $j | ConvertTo-Json -Depth 10;" ^
      "[System.IO.File]::WriteAllText('upload\update.json', $text, (New-Object System.Text.UTF8Encoding $false))"
)

:: ========== 4/4 · 上传 ==========
echo.
echo [4/4] 上传到 R2...

:: 先传 exe（让 update.json 指向时已就绪）
:: --s3-no-check-bucket: R2 凭证只有"写入已有 bucket"权限，没有 CreateBucket，
:: rclone 单文件上传时会预检 bucket，失败就 403。跳过预检即可。
echo   ↑ 上传 %DIST_NAME% ...
"!RCLONE!" copy "upload\%DIST_NAME%" r2:altv/kopanel --s3-no-check-bucket --progress
if errorlevel 1 (
    echo [×] exe 上传失败
    pause
    exit /b 1
)

:: 再传 update.json
echo   ↑ 上传 update.json ...
"!RCLONE!" copy upload\update.json r2:altv/kopanel --s3-no-check-bucket --progress
if errorlevel 1 (
    echo [×] update.json 上传失败
    pause
    exit /b 1
)

echo.
echo ========================================
echo   发布完成 · ko-desktop v%NEW_VERSION%
echo ========================================
echo.
echo   下载: https://cdn.kobo07.cn/kopanel/%DIST_NAME%
echo   页面: https://cdn.kobo07.cn/kopanel/index.html
echo   大小: !FILE_SIZE! 字节
echo.
echo   注: 本脚本只更新 update.json 的 kodesktop_* 字段，
echo       kopanel / widget / beta 信息均保持不变。
echo.
pause
