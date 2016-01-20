@echo off

set WIX=C:\Program Files (x86)\WiX Toolset v3.8
set PATH=%PATH%;%WIX%\bin

rem makecert -r -pe -ss PrivateCertStore -n CN=rtrdprgrmr rtrdprgrmr.cer

cd driver
signtool sign /s PrivateCertStore /n rtrdprgrmr /t http://timestamp.verisign.com/scripts/timestamp.dll fdlvling.sys
makecat fdlvling.cdf 
signtool sign /s PrivateCertStore /n rtrdprgrmr /t http://timestamp.verisign.com/scripts/timestamp.dll fdlvling.cat
cd ..

cd app
signtool sign /s PrivateCertStore /n rtrdprgrmr /t http://timestamp.verisign.com/scripts/timestamp.dll fdlvlctl.exe
cd ..

cd vltlctl
signtool sign /s PrivateCertStore /n rtrdprgrmr /t http://timestamp.verisign.com/scripts/timestamp.dll vltlctl.exe
cd ..

candle -arch x86 -dPKGLOC=japanese fdlvling.wxs -ext WixUIExtension -ext WixDifxAppExtension
light -sw1076 -ext WixUIExtension -ext WixDifxAppExtension -cultures:ja-jp fdlvling.wixobj "%WIX%\bin\difxapp_x86.wixlib" -o fdlvling.msi

signtool sign /s PrivateCertStore /n rtrdprgrmr /t http://timestamp.verisign.com/scripts/timestamp.dll fdlvling.msi

candle -arch x86 -dPKGLOC=english fdlvling.wxs -ext WixUIExtension -ext WixDifxAppExtension
light -sw1076 -ext WixUIExtension -ext WixDifxAppExtension -cultures:en-us fdlvling.wixobj "%WIX%\bin\difxapp_x86.wixlib" -o fdlvling_en.msi

signtool sign /s PrivateCertStore /n rtrdprgrmr /t http://timestamp.verisign.com/scripts/timestamp.dll fdlvling_en.msi
