// Linked into every test executable built with MSVC (see `medium_target` in cmake/Target.cmake).
//
// A failed debug-runtime check - a dead iterator, an out-of-range subscript, an abort - normally
// opens a modal dialog and waits for a click. On a CI runner nobody clicks, so the test hangs until
// the job is killed and prints nothing: that is how a dead iterator in one suite held every
// Windows job for hours. This sends those reports to stderr instead and lets the
// process end, so the failure is a failure with its message in the log.
#ifdef _WIN32
#include <crtdbg.h>
#include <cstdlib>
#include <initializer_list>
#include <windows.h>
namespace {
struct QuietAsserts {
    QuietAsserts() {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
            _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
        }
    }
} quiet_asserts;
} // namespace
#endif
