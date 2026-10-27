# Test Execution Guide

## Quick Start

### Build and Run All Tests
```bash
# From PDFE root directory
cd build
cmake --build . --config Debug
ctest -C Debug -V
```

---

## Running Individual Test Suites

### 1. Form Creation Tests (10 tests)
```bash
./build/Debug/pdfe_tests --gtest_filter=FormCreationTest.*
```

**Expected Output:**
```
[==========] Running 10 tests from 1 test suite.
[----------] Global test environment set-up.
[----------] 10 tests from FormCreationTest
[ RUN      ] FormCreationTest.CreateTextField
[       OK ] FormCreationTest.CreateTextField
[ RUN      ] FormCreationTest.CreateCheckbox
[       OK ] FormCreationTest.CreateCheckbox
[ RUN      ] FormCreationTest.CreateComboBox
[       OK ] FormCreationTest.CreateComboBox
[ RUN      ] FormCreationTest.CreateListBox
[       OK ] FormCreationTest.CreateListBox
[ RUN      ] FormCreationTest.DeleteFormField
[       OK ] FormCreationTest.DeleteFormField
[ RUN      ] FormCreationTest.RestoreFormField
[       OK ] FormCreationTest.RestoreFormField
[ RUN      ] FormCreationTest.CreateFieldCommand_ExecuteUndo
[       OK ] FormCreationTest.CreateFieldCommand_ExecuteUndo
[ RUN      ] FormCreationTest.DeleteFieldCommand_ExecuteUndo
[       OK ] FormCreationTest.DeleteFieldCommand_ExecuteUndo
[ RUN      ] FormCreationTest.FormTool_CreationMode
[       OK ] FormCreationTest.FormTool_CreationMode
[ RUN      ] FormCreationTest.FormTool_FieldCreatedCallback
[       OK ] FormCreationTest.FormTool_FieldCreatedCallback
[----------] 10 tests from FormCreationTest (XX ms total)
[==========] 10 tests from 1 test suite ran. (XX ms total)
[  PASSED  ] 10 tests.
```

---

### 2. Image Tool Wiring Tests (7 tests)
```bash
./build/Debug/pdfe_tests --gtest_filter=ImageToolWiringTest.*
```

**Expected Output:**
```
[==========] Running 7 tests from 1 test suite.
[----------] 7 tests from ImageToolWiringTest
[ RUN      ] ImageToolWiringTest.SetImageTool
[       OK ] ImageToolWiringTest.SetImageTool
[ RUN      ] ImageToolWiringTest.ClearImageTool
[       OK ] ImageToolWiringTest.ClearImageTool
[ RUN      ] ImageToolWiringTest.SetImageToolClearsOther
[       OK ] ImageToolWiringTest.SetImageToolClearsOther
[ RUN      ] ImageToolWiringTest.ImageToolInitiallyNull
[       OK ] ImageToolWiringTest.ImageToolInitiallyNull
[ RUN      ] ImageToolWiringTest.RepaintRequestedSignal
[       OK ] ImageToolWiringTest.RepaintRequestedSignal
[ RUN      ] ImageToolWiringTest.SetImageToolMultipleTimes
[       OK ] ImageToolWiringTest.SetImageToolMultipleTimes
[ RUN      ] ImageToolWiringTest.ImageToolPointerLifetime
[       OK ] ImageToolWiringTest.ImageToolPointerLifetime
[----------] 7 tests from ImageToolWiringTest (XX ms total)
[  PASSED  ] 7 tests.
```

---

### 3. Annotation Font Controls Tests (10 tests)
```bash
./build/Debug/pdfe_tests --gtest_filter=AnnotationFontControlsTest.*
```

**Expected Output:**
```
[==========] Running 10 tests from 1 test suite.
[----------] 10 tests from AnnotationFontControlsTest
[ RUN      ] AnnotationFontControlsTest.FontControlsHiddenByDefault
[       OK ] AnnotationFontControlsTest.FontControlsHiddenByDefault
[ RUN      ] AnnotationFontControlsTest.FontControlsShownForFreeText
[       OK ] AnnotationFontControlsTest.FontControlsShownForFreeText
[ RUN      ] AnnotationFontControlsTest.FontControlsHiddenForText
[       OK ] AnnotationFontControlsTest.FontControlsHiddenForText
[ RUN      ] AnnotationFontControlsTest.FontComboPopulated
[       OK ] AnnotationFontControlsTest.FontComboPopulated
[ RUN      ] AnnotationFontControlsTest.FontSizeRange
[       OK ] AnnotationFontControlsTest.FontSizeRange
[ RUN      ] AnnotationFontControlsTest.SetAnnotationUpdatesFontControls
[       OK ] AnnotationFontControlsTest.SetAnnotationUpdatesFontControls
[ RUN      ] AnnotationFontControlsTest.FontChangeEmitsSignal
[       OK ] AnnotationFontControlsTest.FontChangeEmitsSignal
[ RUN      ] AnnotationFontControlsTest.FontSizeChangeEmitsSignal
[       OK ] AnnotationFontControlsTest.FontSizeChangeEmitsSignal
[ RUN      ] AnnotationFontControlsTest.DefaultAppearancePropertyInEnum
[       OK ] AnnotationFontControlsTest.DefaultAppearancePropertyInEnum
[ RUN      ] AnnotationFontControlsTest.ClearHidesFontControls
[       OK ] AnnotationFontControlsTest.ClearHidesFontControls
[----------] 10 tests from AnnotationFontControlsTest (XX ms total)
[  PASSED  ] 10 tests.
```

---

### 4. Save Operations Tests (10 tests)
```bash
./build/Debug/pdfe_tests --gtest_filter=SaveOperationsTest.*
```

**Expected Output:**
```
[==========] Running 10 tests from 1 test suite.
[----------] 10 tests from SaveOperationsTest
[ RUN      ] SaveOperationsTest.SaveCreatesFile
[       OK ] SaveOperationsTest.SaveCreatesFile
[ RUN      ] SaveOperationsTest.SaveAsCreatesNewFile
[       OK ] SaveOperationsTest.SaveAsCreatesNewFile
[ RUN      ] SaveOperationsTest.SavePreservesPageCount
[       OK ] SaveOperationsTest.SavePreservesPageCount
[ RUN      ] SaveOperationsTest.MarkPageDirty
[       OK ] SaveOperationsTest.MarkPageDirty
[ RUN      ] SaveOperationsTest.SaveClearsDirtyPages
[       OK ] SaveOperationsTest.SaveClearsDirtyPages
[ RUN      ] SaveOperationsTest.SaveAsPreservesPageInfo
[       OK ] SaveOperationsTest.SaveAsPreservesPageInfo
[ RUN      ] SaveOperationsTest.MultipleMarkPageDirty
[       OK ] SaveOperationsTest.MultipleMarkPageDirty
[ RUN      ] SaveOperationsTest.SaveAfterMultipleOperations
[       OK ] SaveOperationsTest.SaveAfterMultipleOperations
[ RUN      ] SaveOperationsTest.SaveToNonExistentDirectory
[       OK ] SaveOperationsTest.SaveToNonExistentDirectory
[----------] 10 tests from SaveOperationsTest (XX ms total)
[  PASSED  ] 10 tests.
```

---

### 5. Updated Form Tool Tests (includes 3 new tests)
```bash
./build/Debug/pdfe_tests --gtest_filter=FormToolTest.*
```

**Look for these NEW tests in output:**
```
[ RUN      ] FormToolTest.SetCreationFieldType
[       OK ] FormToolTest.SetCreationFieldType
[ RUN      ] FormToolTest.ClearCreationFieldType
[       OK ] FormToolTest.ClearCreationFieldType
[ RUN      ] FormToolTest.FieldCreatedCallback
[       OK ] FormToolTest.FieldCreatedCallback
```

---

## Running All New Tests Together

```bash
./build/Debug/pdfe_tests --gtest_filter="FormCreationTest.*:ImageToolWiringTest.*:AnnotationFontControlsTest.*:SaveOperationsTest.*"
```

**Expected Summary:**
```
[==========] 37 tests from 4 test suites ran.
[  PASSED  ] 37 tests.
```

---

## Verbose Output

### Show Test Details
```bash
./build/Debug/pdfe_tests --gtest_filter=FormCreationTest.* --gtest_print_time=1
```

### Show All Assertions
```bash
./build/Debug/pdfe_tests --gtest_filter=FormCreationTest.* --gtest_also_run_disabled_tests
```

---

## Test Coverage

### Generate Coverage Report (if configured)
```bash
# GCC/Clang with gcov
cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="--coverage" ..
cmake --build .
./pdfe_tests
gcov -r *.gcda
lcov --capture --directory . --output-file coverage.info
genhtml coverage.info --output-directory coverage_html
```

### MSVC with Coverage Tools
```powershell
# Visual Studio coverage tools
vstest.console.exe pdfe_tests.exe /EnableCodeCoverage
```

---

## Debugging Failed Tests

### Run Single Test with Debugger
```bash
gdb ./build/Debug/pdfe_tests
(gdb) set args --gtest_filter=FormCreationTest.CreateTextField
(gdb) run
```

### Visual Studio Debugger
```
1. Open PDFE.sln in Visual Studio
2. Set pdfe_tests as startup project
3. Right-click → Properties → Debugging → Command Arguments
4. Add: --gtest_filter=FormCreationTest.CreateTextField
5. Press F5 to debug
```

---

## Common Issues

### Issue 1: Qt Platform Plugin Not Found
**Error:** `qt.qpa.plugin: Could not find the Qt platform plugin "windows"`

**Solution:**
```bash
# Check Qt plugins were deployed
ls build/Debug/platforms/qwindows.dll

# If missing, rebuild
cmake --build build --config Debug --target pdfe_tests
```

### Issue 2: Test Fixtures Not Found
**Error:** `test_single.pdf not available`

**Solution:**
```bash
# Verify fixtures exist
ls tests/fixtures/test_single.pdf

# Check TEST_FIXTURES_DIR is set correctly in CMakeLists.txt
grep TEST_FIXTURES_DIR tests/CMakeLists.txt
```

### Issue 3: MuPDF Context Initialization Fails
**Error:** `MuPdfContext creation failed`

**Solution:**
- Ensure MuPDF DLLs are in PATH or next to executable
- Check vcpkg installation: `vcpkg list | grep mupdf`
- Rebuild with fresh vcpkg cache

---

## CI/CD Integration

### GitHub Actions
```yaml
- name: Build Tests
  run: cmake --build build --config Debug --target pdfe_tests

- name: Run Tests
  run: |
    cd build
    ctest -C Debug --output-on-failure
```

### Test Output Artifacts
```yaml
- name: Upload Test Results
  if: always()
  uses: actions/upload-artifact@v3
  with:
    name: test-results
    path: build/Testing/Temporary/LastTest.log
```

---

## Performance Benchmarks

Expected execution times (Debug build, typical hardware):

| Test Suite                    | Tests | Est. Time |
|-------------------------------|-------|-----------|
| FormCreationTest              | 10    | 500-800ms |
| ImageToolWiringTest           | 7     | 50-100ms  |
| AnnotationFontControlsTest    | 10    | 200-400ms |
| SaveOperationsTest            | 10    | 800-1200ms|
| FormToolTest (3 new tests)    | 3     | 100-200ms |
| **Total New Tests**           | **40**| **~3s**   |

---

## Test Environment

### Required
- Windows 10/11 (or WSL2)
- CMake 3.20+
- MSVC 2019+ or GCC 11+
- Qt 6.5+
- vcpkg with mupdf, qpdf, gtest

### Optional
- Code coverage tools (gcov/lcov)
- Valgrind (Linux only, memory leak detection)
- Qt Creator (integrated test runner)

---

## Additional Resources

- Google Test Documentation: https://google.github.io/googletest/
- Qt Test Framework: https://doc.qt.io/qt-6/qtest-overview.html
- CMake CTest: https://cmake.org/cmake/help/latest/manual/ctest.1.html

---

**Last Updated:** 2026-02-15
**Maintained By:** PDFE Test Team
