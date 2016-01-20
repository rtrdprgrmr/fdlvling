@echo off

cd driver
build
mkdir x64
copy obj%BUILD_ALT_DIR%\amd64\fdlvling.sys x64
cd ..

cd app
build
mkdir x64
copy obj%BUILD_ALT_DIR%\amd64\fdlvlctl.exe x64
cd ..

cd vltlctl
build
mkdir x64
copy obj%BUILD_ALT_DIR%\amd64\vltlctl.exe x64
cd ..
