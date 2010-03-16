@echo off
mkdir scripts\net_1_1_bootstrap > nul 2> nul
mkdir scripts\net_1_1 > nul 2> nul
mkdir scripts\net_2_0 > nul 2> nul
mkdir scripts\moonlight > nul 2> nul

csc -debug -out:scripts\monowrap.exe scripts\monowrap.cs 
copy scripts\monowrap.exe scripts\net_1_1_bootstrap\csc.exe
copy scripts\monowrap.pdb scripts\net_1_1_bootstrap\csc.pdb

copy scripts\monowrap.exe scripts\net_1_1\csc.exe
copy scripts\monowrap.pdb scripts\net_1_1\csc.pdb

copy scripts\monowrap.exe scripts\net_2_0\csc.exe
copy scripts\monowrap.pdb scripts\net_2_0\csc.pdb

copy scripts\monowrap.exe scripts\moonlight\csc.exe
copy scripts\monowrap.pdb scripts\moonlight\csc.pdb

echo Setup complete, you can now use build the solution

csc -r:System.Xml.Linq -debug -out:scripts\genproj.exe scripts\genproj.cs
cd scripts
genproj.exe