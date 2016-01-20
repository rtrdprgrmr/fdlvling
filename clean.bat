@echo off

del /Q *.zip
del /Q *.msi
del /Q *.wixobj
del /Q *.wixpdb

cd driver
del /Q *.sys
del /Q *.cat
del /Q *.log
del /Q *.wrn
del /Q *.err
rmdir /Q /S x64
rmdir /Q /S objchk_wxp_x86
rmdir /Q /S objfre_wxp_x86
rmdir /Q /S objfre_wlh_amd64
rmdir /Q /S objfre_win7_amd64
cd ..

cd app
del /Q *.exe
del /Q *.log
del /Q *.wrn
del /Q *.err
rmdir /Q /S x64
rmdir /Q /S objchk_wxp_x86
rmdir /Q /S objfre_wxp_x86
rmdir /Q /S objfre_wlh_amd64
rmdir /Q /S objfre_win7_amd64
cd ..

cd vltlctl
del /Q *.exe
del /Q *.log
del /Q *.wrn
del /Q *.err
rmdir /Q /S x64
rmdir /Q /S objchk_wxp_x86
rmdir /Q /S objfre_wxp_x86
rmdir /Q /S objfre_wlh_amd64
rmdir /Q /S objfre_win7_amd64
cd ..

cd test
cd test1
del /Q *.log
del /Q *.wrn
del /Q *.err
rmdir /Q /S x64
rmdir /Q /S objchk_wxp_x86
rmdir /Q /S objfre_wxp_x86
rmdir /Q /S objfre_wlh_amd64
rmdir /Q /S objfre_win7_amd64
cd ..
cd test2
del /Q *.log
del /Q *.wrn
del /Q *.err
rmdir /Q /S x64
rmdir /Q /S objchk_wxp_x86
rmdir /Q /S objfre_wxp_x86
rmdir /Q /S objfre_wlh_amd64
rmdir /Q /S objfre_win7_amd64
cd ..
cd ..
