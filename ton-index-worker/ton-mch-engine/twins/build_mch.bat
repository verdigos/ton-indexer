@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
"C:\Users\verdigo\AppData\Local\Programs\CLion\bin\cmake\win\x64\bin\cmake.exe" --build "S:\Dev\ton-indexer-clear\ton-index-worker\cmake-build-debug-visual-studio" --target ton-mch-engine
