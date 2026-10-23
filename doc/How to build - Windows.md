# Building the slicer on Windows

## 0. Prerequisites

The following tools need to be installed on your computer:
- Microsoft Visual Studio version 17 2022
- CMake
- Git

Install Git for Windows from [gitforwindows.org](https://gitforwindows.org/).
If you have not used Git before, I advise you to use a GUI such as TortoiseGit.
Download and run the executable, accepting all defaults.


## 1. Download sources

Clone the repository. Use a directory relatively close to the drive root, so the path is not too long. Avoid spaces and non-ASCII characters. To place it in `C:\local\SLICER`, run:

If `C:\local` does not exist, create it first with `mkdir C:\local`, or choose another directory. On Windows, avoid long paths: keep the repository close to the root of the drive and avoid spaces and non-ASCII characters.

```
c:> cd c:\local
c:\local> git clone https://github.com/LynxterAM/SLICER.git
```
## 2.A Manual Build Instructions

### Compile the dependencies.
Dependencies are updated infrequently, so they are compiled outside the Slic3r source tree.
Open a Command Prompt by pressing Win+R, typing `cmd`, and pressing Enter, then run the following:
```
cd c:\local\SLICER\deps
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build .
```
Expect this to take some time. Note that both _Debug_ and _Release_ variants are built.
If you want to compile in a location other than C:\local\Slic3r\deps\build, add this option to the first CMake call: `-DDESTDIR="c:\local\SLICER-deps"`. If you have multiple repositories of the slicer on your computer, they can use the same dependency directory.
You can force only the _Release_ build by passing `-DDEP_DEBUG=OFF` to the first CMake call.

### Generate a Visual Studio project file for the slicer, referencing the precompiled dependencies.
Open a Command Prompt and run the following:
```
cd c:\local\SLICER
mkdir build
cd build
cmake .. -DCMAKE_PREFIX_PATH="c:\local\SLICER\deps\build\destdir\usr\local" -G "Visual Studio 17 2022"
```

Note that `CMAKE_PREFIX_PATH` must be an absolute path. A relative path will not work.
If you set `DESTDIR` yourself, use that location instead of `c:\local\SLICER\deps\build\destdir\usr\local`.

### Compile the slicer. 

Double-click c:\local\SLICER\build\Slic3r.sln to open in Visual Studio.

Run Build->Rebuild Solution once to populate all required dependency modules. This is NOT done automatically when you Build/Run. If you run both Debug and Release variants, you will need to do this once for each.

Debug->Start Debugging or press F5

The slicer should start. You're up and running!

### Troubleshooting

If it complains that it cannot find PSAPI, you can set the value in your CMake cache. Search your computer for `psapi.lib`. For example, mine is located at `C:/Program Files (x86)/Windows Kits/10/Lib/10.0.22621.0/um/x64/psapi.lib`.

