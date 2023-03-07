
[wiki](https://github.com/apetsiuk/PrusaSlicer/wiki)

<img align="left" width="240" alt="logo" src="_images/PrusaSlicer_batched3.png"/>

# PrusaSlicer: Agglomerative Tool Clustering Fork (ATC, Interlayer color clustering)

The given fork introduces interlayer color clustering (tool aggregation) features. The algorithm is based on redistributing the order of color print regions based on the allowable batch height and degree of region overlap between adjacent layers.


| **Regular printing** | **After ATC processing**   |
| :---:   | :---: |
| <img width="340" src="_images/int_regular.gif"/> | <img width="340" src="_images/int_atc.gif"/>  |
| <img width="340" src="_images/mario_regular.gif"/> | <img width="340" src="_images/mario_atc.gif"/>  |


**The current fork only works with single-object FFF projects in the PrusaSlicer console version.**

**To switch off the ATC version: change the "allow_layer_batching" variable to "false" (bool allow_layer_batching = false) in PrusaSlicer.cpp and rebuild the source.**


### Usage 

```
cd C:\src\PrusaSlicer\build\src\Release
prusa-slicer-console --export-gcode --output filename.gcode filename.3mf
```

----

### Added features

- :white_check_mark: Interlayer agglomerative tool clustering
- :white_check_mark: Critical height tracking
- :white_check_mark: Adjustable level of intersections between neighboring regions


### Features under development
- :x: Wipe tower generation for new g-code version
- :x: Support clustering along with sliced layers
- :x: Generate updated print statistics
- :x: Integrate control elements to visual GUI
- :x: Visualize processed g-code in GUI window
- :x: Implement slide bar for clustered g-code regions in GUI



### Required slicing parameters
- Disable skirt/brim (Print settings -> Skirt and brim -> Skirt -> Loops = 0)
- Disable wipe tower (Print settings -> Multiple extruders -> Wipe tower -> Enable -> uncheck)
- Enable sequential printing (Print settings -> Output options -> Sequential printing -> Complete individual objects -> check)
- Extruder lift Z retraction (Printer settings -> Extruder X -> Retraction -> Lift Z = 1.6 mm for each extruder)
- Disable support


### Variables

- `bool allow_layer_batching = true; // src/PrusaSlicer.cpp`
- `double safe_height = 0.35; // in mm, src/libslic3r/Print.cpp`
- `double critical_intersection = 0.05; // unscaled units, src/libslic3r/Print.cpp`
- `m_print_config.option<ConfigOptionFloats>("retract_lift")->values[working_extruder_idx] = 4.0; // in mm, Lift Z`



### Test 3MF object

Below is an example of building an optimized printing map for a block consisting of three colors and seven layers (region 0 - yellow, region 1 - pink, region 2 - cyan), 8 layers, 1.60 mm height (constant 0.2 mm layer height).

![intersection_test_4_8_layers_1_60mm](_images/intersection_test_4_8_layers_1_60mm.png)

```
Initial printing map (regulr printing):        Agglomerative tool clustering:
{L0, R0}                                        {L0, R0}
{L1, R0}                                        {L1, R0}
{L2, R0}                                        {L2, R0}
{L2, R1}                                        {L3, R0}
{L3, R0}                                        {L2, R1}
{L3, R1}                                        {L3, R1}
{L3, R2}                                        {L4, R1}
{L4, R0}                                        {L5, R1}
{L4, R1}                                        {L3, R2}
{L4, R2}                                        {L4, R2}
{L5, R0}                                        {L5, R2}
{L5, R1}                                        {L6, R2}
{L5, R2}                                        {L4, R0}
{L6, R0}                                        {L5, R0}
{L6, R1}                                        {L6, R0}
{L6, R2}                                        {L6, R1}
{L7, R2}                                        {L7, R2}
```
----

Multicolor models to test
- [Etta the Parrot](https://cults3d.com/en/3d-model/game/multi-color-parrot-remix-mosaicmanufacturing)
- [Two Color World](https://cults3d.com/en/3d-model/various/multi-color-world-with-stand)
- [Orange Coaster](https://cults3d.com/en/3d-model/home/multi-color-citrus-coaster)
- [Dory](https://cults3d.com/en/3d-model/game/multi-color-baby-dory)
- [Mario Keychain](https://cults3d.com/en/3d-model/game/multi-color-mario-keychain)
- [Eiffel Tower](https://cults3d.com/en/3d-model/architecture/eiffel-tower-color-french-flag)
- [Low Poly Squirtle](https://cults3d.com/en/3d-model/game/low-poly-squirtle-multi-and-dual-extrusion-version)


### Modified files

- src/PrusaSlicer.cpp
- src/libslic3r/Layer.hpp
- src/libslic3r/Print.cpp
- src/libslic3r/Print.hpp
- src/libslic3r/PrintBase.hpp
- src/libslic3r/SLAPrint.cpp
- src/libslic3r/SLAPrint.hpp
- src/libslic3r/GCode.cpp
- src/libslic3r/GCode.hpp


Added private members and methods to Slic3r::Layer and Slic3r::Print objects.


<img src="_images/print_object.png"/>


----

[PrusaSlicer](https://www.prusa3d.com/prusaslicer/) takes 3D models (STL, OBJ, AMF) and converts them into G-code instructions for FFF printers or PNG layers for mSLA 3D printers. It's compatible with any modern printer based on the RepRap toolchain, including all those based on the Marlin, Prusa, Sprinter and Repetier firmware. It also works with Mach3, LinuxCNC and Machinekit controllers.

PrusaSlicer is based on [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community. See the [project homepage](https://www.prusa3d.com/slic3r-prusa-edition/) and the [documentation directory](doc/) for more information.

### Dependencies

- [Boost](https://www.boost.org/)
- [Eigen](https://eigen.tuxfamily.org/dox/GettingStarted.html)
- [Clipper](https://github.com/AngusJohnson/Clipper2)
- [LibIGL](https://libigl.github.io/)
- [OpenGL](https://www.opengl.org/)
- [CGAL](https://www.cgal.org/)




### How to build?

[Linux](doc/How%20to%20build%20-%20Linux%20et%20al.md) | [macOS](doc/How%20to%20build%20-%20Mac%20OS.md) | [Windows](doc/How%20to%20build%20-%20Windows.md)

#### Windows:

***1. Install the tools***

Install Visual Studio Community 2019 from visualstudio.microsoft.com/vs/. Older versions are not supported as PrusaSlicer requires support for C++17. Select all workload options for C++ and make sure to launch Visual Studio after install (to ensure that the full setup completes).

Install git for Windows from gitforwindows.org Download and run the exe accepting all defaults.

***2. Download sources***
Clone the respository. To place it in C:\src\PrusaSlicer, run:
```
c:> mkdir src
c:> cd src
c:\src> git clone https://github.com/prusa3d/PrusaSlicer.git
```

***3. Compile the dependencies***
Dependencies are updated seldomly, thus they are compiled out of the PrusaSlicer source tree. Go to the Windows Start Menu and Click on "Visual Studio 2019" folder, then select the ->"x64 Native Tools Command Prompt" to open a command window and run the following:
```
cd c:\src\PrusaSlicer\deps
mkdir build
cd build
cmake .. -G "Visual Studio 16 2019" -DDESTDIR="c:\src\PrusaSlicer-deps"
msbuild /m ALL_BUILD.vcxproj
```
***4. Generate Visual Studio project***
Go to the Windows Start Menu and Click on "Visual Studio 2019" folder, then select the ->"x64 Native Tools Command Prompt" to open a command window and run the following:
```
cd c:\src\PrusaSlicer\
mkdir build
cd build
cmake .. -G "Visual Studio 16 2019" -DCMAKE_PREFIX_PATH="c:\src\PrusaSlicer-deps\usr\local"
```

***5. Compile PrusaSlicer***
Double-click c:\src\PrusaSlicer\build\PrusaSlicer.sln to open in Visual Studio 2019. Select **PrusaSlicer_app_gui** as your startup project (right-click->Set as Startup Project).

Run Build->Rebuild Solution once to populate all required dependency modules. This is NOT done automatically when you build/run. If you run both Debug and Release variants, you will need to do this once for each.

Debug->Start Debugging or press F5

PrusaSlicer should start. You're up and running!

*note: Thanks to @douggorgen for the original guide, as an answer for an issue.*



### PrusaSlicer license

PrusaSlicer is licensed under the _GNU Affero General Public License, version 3_.
The PrusaSlicer is originally based on Slic3r by Alessandro Ranellucci.

### Use PrusaSlicer from the command line

Please refer to the [Command Line Interface](https://github.com/prusa3d/PrusaSlicer/wiki/Command-Line-Interface) wiki page.
