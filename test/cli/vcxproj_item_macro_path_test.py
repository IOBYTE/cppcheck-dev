
# python -m pytest vcxproj_item_macro_path_test.py
#
# Regression coverage for macro ($(...)) expansion in a ClCompile project
# item's Include/Update/Remove path (as opposed to expansion elsewhere, e.g.
# in metadata values or Condition attributes, which is unaffected).
#
# Microsoft documents that the Visual Studio C++ project system does not
# reliably resolve a macro in a project item path if that macro's value could
# differ by configuration -- "The IDE doesn't expect project item paths to be
# different for different project configurations" -- see
# https://learn.microsoft.com/en-us/cpp/build/reference/vcxproj-file-structure
# That is specifically about macros whose value VARIES by configuration: the
# fixed set of MSBuild "this file"/"this project" location properties
# (MSBuildThisFileDirectory and friends) are always exempt, since they can't
# vary by construction -- see test/cli/shared-items-project, covered
# separately by test_shared_items_project() in more-projects_test.py -- and so
# is any other property THIS project happens to resolve to the same value in
# every one of its configurations, e.g. one a property sheet sets once,
# unconditionally (see vcxproj_item_macro_path_invariant_test.py).
#
# This fixture's only <ClCompile> item is Include="$(SomeDir)\foo.cpp", where
# SomeDir is an ordinary user-defined property that genuinely differs between
# the project's two configurations (SomeDir for Debug|x64, SomeOtherDir for
# Release|x64) -- exactly the case Visual Studio can't reliably resolve, since
# a single Solution Explorer file list can't show two different files
# depending on which configuration happens to be active. Expanding $(SomeDir)
# here would resolve to a real file on disk in either configuration -- but
# real Visual Studio does not expand it, so the item stays unresolved in both
# and the project has no valid source files at all.

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
