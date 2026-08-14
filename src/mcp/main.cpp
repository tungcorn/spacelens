#include "mcp/Protocol.hpp"
#include "mcp/StorageTools.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

int main()
{
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
#endif
    spacelens::mcp::McpServer server;
    spacelens::mcp::registerStorageTools(server);
    return server.runStdio();
}
