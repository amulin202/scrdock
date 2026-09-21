@echo off
rem Sync scrdock-mods (and master) with upstream scrcpy -- no PR involved.
rem Usage: scrdock\sync-upstream.bat
setlocal
cd /d D:\Test\scrcpy

echo [1/4] fetching upstream...
git fetch upstream
if errorlevel 1 exit /b 1

echo [2/4] rebasing scrdock-mods onto upstream/master...
git checkout scrdock-mods
if errorlevel 1 exit /b 1
git rebase upstream/master
if errorlevel 1 (
  echo.
  echo *** rebase conflict: fix the files, then:
  echo     git rebase --continue
  echo     git push --force-with-lease origin scrdock-mods
  exit /b 1
)
git push --force-with-lease origin scrdock-mods
if errorlevel 1 exit /b 1

echo [3/4] fast-forwarding master...
git checkout master
if errorlevel 1 exit /b 1
git merge --ff-only upstream/master
if errorlevel 1 exit /b 1
git push origin master
if errorlevel 1 exit /b 1

echo [4/4] back to scrdock-mods.
git checkout scrdock-mods
echo done: fork master + scrdock-mods are up to date.
