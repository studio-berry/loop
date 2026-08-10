#!/usr/bin/env python3
import unittest

from scripts.ci.check_generated_dependency_paths import is_generated_dependency_path


class GeneratedDependencyPathTests(unittest.TestCase):
    def test_rejects_docker_vcpkg_state(self):
        for path in (
            ".docker-vcpkg",
            ".docker-vcpkg-cache/abc.zip",
            ".docker-vcpkg-installed/x64-linux/include/zlib.h",
        ):
            with self.subTest(path=path):
                self.assertTrue(is_generated_dependency_path(path))

    def test_rejects_vcpkg_generated_siblings(self):
        for path in (
            "vcpkg_installed/x64-windows/include/zlib.h",
            "vcpkg/downloads/archive.zip",
            "vcpkg/packages/zlib.zip",
            "vcpkg/buildtrees/zlib/configure.log",
            "vcpkg/installed/x64-windows/share/zlib/zlib-config.cmake",
            "vcpkg/archives/zlib.zip",
        ):
            with self.subTest(path=path):
                self.assertTrue(is_generated_dependency_path(path))

    def test_keeps_vcpkg_source_and_manifests(self):
        for path in (
            "vcpkg/overlays/general/zlib/portfile.cmake",
            "vcpkg.json",
            "vcpkg-configuration.json",
        ):
            with self.subTest(path=path):
                self.assertFalse(is_generated_dependency_path(path))


if __name__ == "__main__":
    unittest.main()
