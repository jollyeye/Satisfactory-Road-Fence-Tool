@echo off
rem Road Maker: pull the latest code, rebuild, refresh materials, package into the game.
rem Edit the paths below if your folders differ.
set PROJECT=D:\SFMods\SML\FactoryGame.uproject
set MODDIR=D:\SFMods\SML\Mods\RoadMaker
set GAME=D:\games\Satisfactory
set LOG=D:\SFMods\update-log.txt
set ENGINE=D:\UE-CSS
if exist "D:\UE-CSS\Unreal Engine - CSS\Engine" set ENGINE=D:\UE-CSS\Unreal Engine - CSS
set GIT="%ProgramFiles%\Git\cmd\git.exe"

echo [1/4] Getting the latest Road Maker code...
echo == git pull == > "%LOG%"
%GIT% -C "%MODDIR%" pull >> "%LOG%" 2>&1
if errorlevel 1 goto failed

echo [2/4] Building the editor...
echo == build == >> "%LOG%"
call "%ENGINE%\Engine\Build\BatchFiles\Build.bat" FactoryEditor Win64 Development -Project="%PROJECT%" -WaitMutex >> "%LOG%" 2>&1
if errorlevel 1 goto failed

echo [3/4] Creating and assigning materials...
echo == materials == >> "%LOG%"
"%ENGINE%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "%PROJECT%" -run=pythonscript -script="%MODDIR%\Tools\make_materials.py" -unattended -nop4 -nosplash -stdout >> D:\SFMods\materials-editor-log.txt 2>&1

echo [4/4] Packaging Road Maker into the game...
echo == package == >> "%LOG%"
call "%ENGINE%\Engine\Build\BatchFiles\RunUAT.bat" -ScriptsForProject="%PROJECT%" PackagePlugin -project="%PROJECT%" -clientconfig=Shipping -serverconfig=Shipping -utf8output -build -platform=Win64 -nocompileeditor -installed -Target=FactoryGameEGS -CopyToGameDirectory_Windows="%GAME%" -DLCName=RoadMaker >> "%LOG%" 2>&1
if errorlevel 1 goto failed

echo ALL DONE >> "%LOG%"
echo ALL DONE - start the game and test. Tell Claude.
pause
exit /b 0

:failed
echo FAILED >> "%LOG%"
echo FAILED - tell Claude, it can read D:\SFMods\update-log.txt
pause
exit /b 1
