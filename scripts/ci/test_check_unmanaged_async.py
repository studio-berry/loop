import unittest

from check_unmanaged_async import scan_source_text


class UnmanagedAsyncAuditTest(unittest.TestCase):
    def test_detects_all_launch_forms(self):
        launches = scan_source_text(
            "LoupeLibGui/example.cpp",
            """
            QtConcurrent::run(work);
            auto thread = QThread::create(work);
            pool->start(new Runnable());
            std::thread worker(work);
            """,
        )
        self.assertEqual(
            [launch.kind for launch in launches],
            ["QtConcurrent::run", "QThread::create", "QThreadPool::start", "std::thread"],
        )

    def test_ignores_non_source_files(self):
        self.assertEqual(scan_source_text("docs/async.md", "QtConcurrent::run(work);"), [])


if __name__ == "__main__":
    unittest.main()
