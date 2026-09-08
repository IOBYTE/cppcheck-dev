
# python -m pytest vcxproj_item_macro_path_test.py
#
# Regression coverage for macro ($(...)) expansion in a ClCompile project
# item's Include/Update/Remove path (as opposed to expansion elsewhere, e.g.
# in metadata values or Condition attributes, which is unaffected).
#
# Microsoft documents that the Visual Studio C++ project system does not
# support macros in project item paths in general -- "The IDE doesn't expect
# project item paths to be different for different project configurations" --
# see https://learn.microsoft.com/en-us/cpp/build/reference/vcxproj-file-structure
# The one documented exception is the fixed set of MSBuild "this file"/"this
# project" location properties (MSBuildThisFileDirectory and friends), which
# don't vary by configuration and are exactly what Visual Studio's own Shared
# Items projects rely on -- see test/cli/shared-items-project, covered
# separately by test_shared_items_project() in more-projects_test.py.
#
# This fixture's only <ClCompile> item is
# Include="$(SomeDir)\foo.cpp", where SomeDir is an ordinary user-defined
# property (not one of the invariant location properties above). Expanding
# $(SomeDir) here would resolve to a real file on disk (SomeDir/foo.cpp) and
# get it checked -- but real Visual Studio does not expand it, so the item
# stays unresolved and the project has no valid source files at all.

import os

from testutils import cppcheck

__script_dir = os.path.dirname(os.path.abspath(__file__))


def test_vcxproj_item_macro_path():
    args = [
        '--project=vcxproj_item_macro_path/vcxproj_item_macro_path.vcxproj',
        '--no-cppcheck-build-dir',
        '--dump'
    ]
    ret, stdout, _ = cppcheck(args, cwd=__script_dir)

    # $(SomeDir) must NOT be expanded -- the ClCompile item stays unresolved,
    # so the project ends up with no valid source files at all, exactly like
    # any other project whose only item cppcheck cannot resolve.
    assert ret != 0, stdout
    assert stdout == 'cppcheck: error: no C or C++ source files found.\n', stdout
