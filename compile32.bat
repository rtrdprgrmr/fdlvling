@echo off

cd driver
build
copy obj%BUILD_ALT_DIR%\i386\fdlvling.sys
cd ..

cd app
build
copy obj%BUILD_ALT_DIR%\i386\fdlvlctl.exe
cd ..

cd vltlctl
build
copy obj%BUILD_ALT_DIR%\i386\vltlctl.exe
cd ..
