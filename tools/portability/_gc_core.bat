@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d "C:\Users\fahre\.gemini\antigravity\scratch\vulkan_craft\engine"
set GC=external\solutions\geometry-central
set EIG=external\solutions\eigen
set OUT=tools\portability\_gc_gate
set CFLAGS=/nologo /O2 /std:c++20 /EHsc /DNDEBUG /DGC_HAVE_SUITESPARSE=0 /utf-8 /I%GC%\include /I%EIG% /I%GC%\deps\nanort\include /I%GC%\deps\nanoflann\include /I%GC%\deps\happly
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\numerical\eigenproblem_solvers.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\numerical\linear_algebra_utilities.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\numerical\linear_solvers.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\numerical\positive_definite_solvers.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\numerical\qr_solvers.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\numerical\square_solvers.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\numerical\suitesparse_utilities.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\utilities\disjoint_sets.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\utilities\elementary_geometry.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\utilities\knn.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\utilities\quaternion.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\utilities\tri_tri_intersect.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\utilities\unit_vector3.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\utilities\utilities.cpp"
echo === COMPILE DONE ===
