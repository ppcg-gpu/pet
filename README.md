# PET: Polyhedral Extraction Tool

PET with CMake-based build system, can be built for LLVM versions 11...20.


## Building

To build PET with all LLVM versions (11-20):

```bash
docker-compose build
```

This command will build 10 Docker images, one for each LLVM version. Each image will:

- Install the appropriate version of LLVM tools
- Build PET from source
- Run the test suite
- Install PET to `/opt/pet-llvm-XX` (where XX is the LLVM version)

To build PET with specific LLVM versions:

```bash
# Build only LLVM version 15
docker-compose build llvm-15

# Build multiple specific versions
docker-compose build llvm-11 llvm-20
```

After building, you can run a container to inspect the results:

```bash
# Start a shell in the LLVM 15 container
docker-compose run llvm-15 /bin/bash

# Check the installed PET
ls -la /opt/pet-llvm-15
```

If a build fails, you can check the logs:

```bash
docker-compose logs llvm-15
```

To rebuild a specific version after fixing issues:

```bash
docker-compose build --no-cache llvm-15
```

### Build Configuration

By default, PET is built in Release mode. You can control the build configuration by editing the `.env` file:

```
# Build configuration for PET LLVM
# These settings apply to all LLVM versions
# Default configuration (Release build)

# Build type: Release or Debug
CMAKE_BUILD_TYPE=Release
 
# Enable debug hooks: ON or OFF
PET_ENABLE_DEBUG_HOOKS=OFF
 
# Run tests in verbose mode: ON or OFF
CTEST_VERBOSE=OFF

# Debug configuration (uncomment to use)
# CMAKE_BUILD_TYPE=Debug
# PET_ENABLE_DEBUG_HOOKS=ON
# CTEST_VERBOSE=ON
```

### Using a Specific LLVM Version

If you have multiple LLVM versions installed on your system and want to force PET to use a specific version for testing or compatibility reasons, you can use the `PET_FORCE_LLVM_VERSION` option:

```bash
# Force PET to use LLVM 15
cmake -DPET_FORCE_LLVM_VERSION=15 ..

# Force PET to use a specific point release of LLVM
cmake -DPET_FORCE_LLVM_VERSION=11.1.0 ..
```

This is particularly useful when testing PET against different LLVM versions on the same host system without using Docker containers. The build system will search for the exact version specified and fail if it can't be found.


## Docker Building

Ubuntu-based containers is the easiest way to build and test PET with any modern version of LLVM:

- LLVM versions 11 and 12 are installed from the Ubuntu standard repositories
- LLVM versions 13-20 are installed from the LLVM repository (http://apt.llvm.org/jammy/)

When new LLVM versions are released, update the docker-compose.yml file by adding a new service:

```yaml
llvm-21:
  build:
    context: .
    dockerfile: Dockerfile.template
    args:
      LLVM_VERSION: 21
  image: pet:llvm-21
```

The `Dockerfile.template` already handles the logic for installation sources, so no other changes are needed.


## Usage

The main entry points are pet_scop_extract_from_C_source and
pet_transform_C_source.
The first function extracts a scop from the C source file with the given name
and returns it as a pet_scop.  The scop corresponds to the piece
of code delimited by

    #pragma scop

and

    #pragma endscop

The code in between needs to consist only of expression statements,
if statements and for statements.  All access relations and loop initializations
need to be piecewise quasi-affine.  Conditions are allowed to be non-affine,
in which case a separate statement is constructed to evaluate the condition.

The second function (pet_transform_C_source) iterates over all scops.

If the autodetect option has been set, pet will try to automatically
detect a scop and no pragmas are required.  On the other hand, pet
will not produce any warnings in this case as any code that does not
satisfy the requirements is considered to lie outside of the scop.

The layout of pet_scop is documented in include/pet.h.

An example application is given by pet_loopback.c,
which prints out code from the pet_scop without any transformation.


## Development

PET requires LLVM/clang libraries, 3.9 or higher. Unless you have some other reasons for wanting to use the git version, it's best to install the latest release (18.1). The newer versions of LLVM occasionally introduces incompatibilities. Nevertheless, if you encounter any such incompatibilities, please report them so that they can be fixed. However, development versions from before the latest release are not supported.

When developing on a system with multiple LLVM versions installed, you can enforce a specific version using the `PET_FORCE_LLVM_VERSION` CMake option, as described above.


## Citation

If you use pet for your research, you are invited to cite the following paper:

@InProceedings{Verdoolaege2012pet,
    author = {Sven Verdoolaege and Tobias Grosser},
    title = {Polyhedral Extraction Tool},
    booktitle = {Second Int. Workshop on Polyhedral Compilation Techniques
		(IMPACT'12)},
    address = {Paris, France},
    month = jan,
    year = {2012}
}
