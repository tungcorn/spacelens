#include "cli/Args.hpp"
#include "cli/Commands.hpp"
#include "cli/ExitCodes.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <atomic>
#include <iostream>
#include <stop_token>

#ifndef SPACELENS_VERSION_STRING
#define SPACELENS_VERSION_STRING "0.1.0"
#endif

namespace {

std::atomic<bool> g_cancelRequested{false};
std::stop_source* g_stopSource = nullptr;

BOOL WINAPI consoleCtrlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        g_cancelRequested.store(true);
        if (g_stopSource != nullptr) {
            g_stopSource->request_stop();
        }
        return TRUE;
    }
    return FALSE;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    const auto args = spacelens::cli::parseArgs(argc, argv);
    if (!args.error.empty()) {
        std::cerr << "error: " << args.error << "\n";
        std::cerr << "Run 'spacelens help' for usage.\n";
        return spacelens::cli::toInt(spacelens::cli::ExitCode::UsageError);
    }

    if (args.command == spacelens::cli::Command::Help) {
        std::cout << spacelens::cli::helpText();
        return spacelens::cli::toInt(spacelens::cli::ExitCode::Success);
    }
    if (args.command == spacelens::cli::Command::Version) {
        std::cout << "spacelens " << SPACELENS_VERSION_STRING << "\n";
        return spacelens::cli::toInt(spacelens::cli::ExitCode::Success);
    }
    if (args.command == spacelens::cli::Command::Capabilities) {
        return spacelens::cli::toInt(spacelens::cli::runCapabilities(args));
    }

    std::stop_source stopSource;
    g_stopSource = &stopSource;
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    spacelens::cli::ExitCode code = spacelens::cli::ExitCode::InternalError;
    try {
        switch (args.command) {
        case spacelens::cli::Command::Scan:
            code = spacelens::cli::runScan(args, stopSource.get_token());
            break;
        case spacelens::cli::Command::Top:
            code = spacelens::cli::runTop(args, stopSource.get_token());
            break;
        case spacelens::cli::Command::Find:
            code = spacelens::cli::runFind(args, stopSource.get_token());
            break;
        default:
            code = spacelens::cli::ExitCode::InternalError;
            break;
        }
    } catch (const std::exception& ex) {
        std::cerr << "internal error: " << ex.what() << "\n";
        code = spacelens::cli::ExitCode::InternalError;
    } catch (...) {
        std::cerr << "internal error\n";
        code = spacelens::cli::ExitCode::InternalError;
    }

    SetConsoleCtrlHandler(consoleCtrlHandler, FALSE);
    g_stopSource = nullptr;
    return spacelens::cli::toInt(code);
}
