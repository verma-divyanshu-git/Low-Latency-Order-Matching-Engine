# Installation and packaging

Configure and build normally, then install to an explicit prefix:

```sh
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix /absolute/install/prefix
```

The install contains public headers, static core and persistence libraries, the `matching_engine_replay` executable, license, and generated CMake package metadata.
External CMake consumers use:

```cmake
find_package(matching_engine CONFIG REQUIRED)
target_link_libraries(application PRIVATE matching_engine::persistence)
```

`matching_engine::persistence` carries its public dependency on `matching_engine::core`.
The installed version file uses same-major compatibility.
Runtime API compatibility is checked separately through `runtime_api_compatible`.

Build a binary TGZ package with:

```sh
cmake --build build/release --target package
```

Build a source TGZ package with the generated `package_source` target.
Build trees, Git metadata, and fuzz crash artifacts are excluded from source packages.

`install_package_test` installs into a temporary prefix, configures a separate consumer project, links exported targets, and compiles against installed headers.
