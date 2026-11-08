# Custom Blender 5.3 Beta Build

This is an unofficial Windows x64 Blender 5.3.0 Beta source snapshot with the custom features listed below. It is based on Blender revision `7e7c173d45c8684c469aa688ba3f7744e88258f8` and was built locally with Visual Studio 2022.

## Included feature groups

- Geometry Nodes Viewer Edit Mode, selection capture, element selection, transforms, extrusion, merge, duplication, face tools, loop selection, loop cut and slide, and Set Rotation.
- Mesh Edge Loop and Loop Cut and Slide geometry operations, plus Dissolve Elements.
- Compositor IBK, Color Match, Film Grain/Regrain, Relight, Optical Flow, flow utilities, and motion-blur/vector-blur improvements.
- Degree and radian editing for Geometry Nodes angle sockets.
- 3DE4 Degree 4 and equidistant fisheye tracking support.

## Build configuration and scope

- No BEX source or runtime is included.
- Compact Nodes and the experimental disconnected-island Merge by Distance mode are not included.
- CUDA 12.9 Cycles kernels were built. OptiX and oneAPI were unavailable in the build environment and are disabled in this binary.
- The source snapshot excludes Git history and local dependency checkouts. A build requires Blender's dependency libraries and the normal Blender build setup.
- The executable was built and its version/features were checked for registration. Feature workflows and the test suite were not run as part of this release preparation.
