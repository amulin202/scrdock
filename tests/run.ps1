$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or !$installation) { throw '未找到 MSVC 编译器' }
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
Push-Location $root
try {
    New-Item -ItemType Directory -Force 'build\tests' | Out-Null
    $command = 'call "{0}" >nul 2>nul && cl /nologo /W4 /WX /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0A00 tests\test_sessions.c qrcodegen.c /Fe:build\tests\test_sessions.exe /Fo:build\tests\ /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib kernel32.lib shell32.lib advapi32.lib comctl32.lib comdlg32.lib' -f $vcvars
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE -ne 0) { throw '回归测试编译失败' }
    & '.\build\tests\test_sessions.exe'
    if ($LASTEXITCODE -ne 0) { throw '回归测试失败' }
    $command = 'call "{0}" >nul 2>nul && cl /nologo /W4 /WX /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0A00 tests\test_lifecycle.c qrcodegen.c /Fe:build\tests\test_lifecycle.exe /Fo:build\tests\ /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib kernel32.lib shell32.lib advapi32.lib comctl32.lib comdlg32.lib' -f $vcvars
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE -ne 0) { throw '窗口生命周期测试编译失败' }
    & '.\build\tests\test_lifecycle.exe'
    if ($LASTEXITCODE -ne 0) { throw '窗口生命周期测试失败' }
    $command = 'call "{0}" >nul 2>nul && cl /nologo /W4 /WX /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0A00 tests\test_pairing.c qrcodegen.c /Fe:build\tests\test_pairing.exe /Fo:build\tests\ /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib kernel32.lib shell32.lib advapi32.lib comctl32.lib comdlg32.lib' -f $vcvars
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE -ne 0) { throw '二维码配对测试编译失败' }
    & '.\build\tests\test_pairing.exe'
    if ($LASTEXITCODE -ne 0) { throw '二维码配对测试失败' }
    if (Test-Path 'build\qr-test-deps\zxingcpp*') {
        python tests/decode_qr.py
        if ($LASTEXITCODE -ne 0) { throw '二维码独立解码失败' }
    }
} finally {
    Pop-Location
}
