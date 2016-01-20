@echo off

set WIX=C:\Program Files (x86)\WiX Toolset v3.8
set PATH=%PATH%;%WIX%\bin

cd driver
mkdir x64
cd x64
copy ..\fdlvling.cdf
signtool sign /s PrivateCertStore /n rtrdprgrmr /t http://timestamp.verisign.com/scripts/timestamp.dll fdlvling.sys
makecat fdlvling.cdf 
signtool sign /s PrivateCertStore /n rtrdprgrmr /t http://timestamp.verisign.com/scripts/timestamp.dll fdlvling.cat
cd ..
cd ..

cd app
mkdir x64
cd x64
signtool sign /s PrivateCertStore /n rtrdprgrmr /t http://timestamp.verisign.com/scripts/timestamp.dll fdlvlctl.exe
cd ..
cd ..

cd vltlctl
mkdir x64
cd x64
signtool sign /s PrivateCertStore /n rtrdprgrmr /t http://timestamp.verisign.com/scripts/timestamp.dll vltlctl.exe
cd ..
cd ..

candle -arch x64 -dPKGLOC=japanese fdlvling.wxs -ext WixUIExtension -ext WixDifxAppExtension
light -sw1076 -ext WixUIExtension -ext WixDifxAppExtension -cultures:ja-jp fdlvling.wixobj "%WIX%\bin\difxapp_x64.wixlib" -o fdlvling64.msi

signtool sign /s PrivateCertStore /n rtrdprgrmr /t http://timestamp.verisign.com/scripts/timestamp.dll fdlvling64.msi

candle -arch x64 -dPKGLOC=english fdlvling.wxs -ext WixUIExtension -ext WixDifxAppExtension
light -sw1076 -ext WixUIExtension -ext WixDifxAppExtension -cultures:en-us fdlvling.wixobj "%WIX%\bin\difxapp_x64.wixlib" -o fdlvling64_en.msi

signtool sign /s PrivateCertStore /n rtrdprgrmr /t http://timestamp.verisign.com/scripts/timestamp.dll fdlvling64_en.msi
