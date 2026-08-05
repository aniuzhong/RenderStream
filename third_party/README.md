# third_party — vendored dependencies

Trimmed source copies of the dependencies used by RenderStream. Each directory is a
faithful subset of the upstream repository (headers + sources + CMake build files;
tests/benchmarks/docs/CI files removed). This is the single source of truth used by
both the offline CMake build (`--preset vendored`) and the native Visual Studio
projects (`.vcxproj`).

## Version table

| Library      | Version / commit | License (see LICENSE.md) |
|--------------|------------------|--------------------------|
| asio         | 1.30.2           | Boost Software License 1.0 |
| nlohmann_json| 3.11.3           | MIT                      |
| fmt          | 11.0.2           | MIT                      |
| llhttp       | 9.2.1            | MIT                      |
| expected-lite| 0.8.0            | Boost Software License 1.0 |
| spdlog       | 1.14.1           | MIT                      |
| restinio     | 392ac5dc (0.7.9+) | Boost Software License 1.0 |
| cpp-httplib  | 0.18.1           | MIT                      |
| Catch2       | 3.7.1            | Boost Software License 1.0 |

Notes:

- **restinio** is pinned to commit `392ac5dc30b70c5643d07462a63b0b16d579daac` (main
  still tracks upstream `origin/master`; this vendored copy is the frozen snapshot).
  `dev/restinio/` is the CMake source subdirectory (`SOURCE_SUBDIR`).
- **llhttp** keeps `libllhttp.pc.in` because its `CMakeLists.txt` references it via
  `configure_file()`. The generated `libllhttp.pc` is a build artifact (recreated on
  configure) and is not committed.
- **expected-lite** keeps `LICENSE.txt` and `README.md` because its `CMakeLists.txt`
  sets both as CPack resource files (checked at configure time).
- **httplib/LICENSE** is retained because of an `install(FILES ...)` rule in its
  `CMakeLists.txt`.
- **fmt** needs `support/cmake/JoinPaths.cmake` (`include(JoinPaths)`), plus
  `README.md` and `ChangeLog.md` — they are part of the library target's source list
  (existence is checked at generate time).
- All license texts are consolidated in `LICENSE.md`; the two per-lib license files
  above are kept only because the build references them.

## Upgrade ritual

1. Bump the `GIT_TAG`/`URL` in the root `CMakeLists.txt` (main branch).
2. Merge main into this branch.
3. Fetch the new version (`cmake -S . -B build-fetch -A x64` or a plain `git clone`),
   trim-copy it over the directory above (keep CMakeLists + referenced files, drop
   tests/benchmarks/docs/CI).
4. Update the version table above and add any new license text to `LICENSE.md`.
5. Verify offline: `cmake --preset vendored` then build.

## License

All third-party license texts are consolidated in [LICENSE.md](LICENSE.md).
