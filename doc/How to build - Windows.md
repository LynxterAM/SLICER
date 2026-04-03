# Building the slcier on Windows

## 0. Prerequisities

The following tools need to be installed on your computer:
- Microsoft Visual Studio version 17 2022
- CMake
- git

Install git for Windows from [gitforwindows.org](https://gitforwindows.org/)
If you haven't, I advise to use a gui like tortoisegit.
Download and run the exe accepting all defaults


## 1. Download sources

Clone the respository. Use a directory relatively close to the drive root, so the path is not too long. Avoid spaces and non-ASCII characters. To place it in `C:\local\REPO_NAME`, run:
```
c:> mkdir src
c:> cd src
c:\src> git clone https://github.com/LynxterAM/SLICER.git
```

For now on, we consider that REPO_NAME is "SLICER", but you can rename it if needed.

## 2.A Manual Build Instructions

### Compile the dependencies.
Dependencies are updated seldomly, thus they are compiled out of the Slic3r source tree.
Open the MSVC x64 Native Tools Command Prompt (WIN+R -> cmd) and run the following:
```
cd c:\local\SLICER\deps
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build .
```
Expect this to take some time. Note that both _Debug_ and _Release_ variants are built.
If you want to compile in another place than C:\local\Slic3r\deps\usr\local, add this �ption to the first CMake call: ` -DDESTDIR="c:\local\SLICER-deps"`. If you have multiple repositories of the slicer on your computer, they can use the same dependency directory.
You can force only the _Release_ build by passing `-DDEP_DEBUG=OFF` to the first CMake call.

### Generate Visual Studio project file for the slcier, referencing the precompiled dependencies.
Open the MSVC x64 Native Tools Command Prompt and run the following:
```
cd c:\local\SLICER
mkdir build
cd build
cmake .. -DCMAKE_PREFIX_PATH="c:\local\SLICER\deps\build\destdir\usr\local" -G "Visual Studio 17 2022"
```

Note that `CMAKE_PREFIX_PATH` must be absolute path. A relative path will not work.
If you set yourself the DESTDIR for the deps, write your location instead of `c:\local\SLICER\deps\build\destdir\usr\local`

### Compile the slicer. 

Double-click c:\local\SLICER\build\SLICER.sln to open in Visual Studio.

Run Build->Rebuild Solution once to populate all required dependency modules. This is NOT done automatically when you Build/Run. If you run both Debug and Release variants, you will need to do this once for each.

Debug->Start Debugging or press F5

The sclier should start. You're up and running!

### Troubleshooting

If it complains about not finding PSAPI, you can set yourself the value in your cmakecache. Search your computer for 'psapi.lib'. For exemple, mine is at "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.22621.0/um/x64/psapi.lib".

