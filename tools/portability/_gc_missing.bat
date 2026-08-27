@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d "C:\Users\fahre\.gemini\antigravity\scratch\vulkan_craft\engine"
set GC=external\solutions\geometry-central
set EIG=external\solutions\eigen
set OUT=tools\portability\_gc_gate
set CFLAGS=/nologo /O2 /std:c++20 /EHsc /DNDEBUG /D_USE_MATH_DEFINES /utf-8 /I%GC%\include /I%EIG% /I%GC%\deps\nanort\include /I%GC%\deps\nanoflann\include /I%GC%\deps\happly
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\surface\boundary_first_flattening.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\surface\embed_convex.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\surface\marching_triangles.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\surface\vector_heat_method.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\surface\signed_heat_method.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\surface\polygon_mesh_heat_solver.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\surface\geodesic_centroidal_voronoi_tessellation.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\surface\trace_geodesic.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\surface\normal_coordinates.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\surface\intrinsic_triangulation.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\surface\signpost_intrinsic_triangulation.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\surface\integer_coordinates_intrinsic_triangulation.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\surface\barycentric_vector.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\surface\fast_marching_method.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\surface\flip_geodesics.cpp"
cl %CFLAGS% /Fo%OUT%\ /c "%GC%\src\pointcloud\local_triangulation.cpp"
echo === DONE ===
