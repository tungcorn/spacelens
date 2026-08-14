# Portable ZIP staging via `cmake --install --prefix <stage> --component ...`.
# Only runtime executables are installed. Static libraries, tests, PDBs, and
# local AppData state are not.

if(TARGET spacelens)
    install(TARGETS spacelens
        RUNTIME DESTINATION .
        COMPONENT SpaceLensCli
    )
endif()

if(TARGET SpaceLens)
    install(TARGETS SpaceLens
        RUNTIME DESTINATION .
        COMPONENT SpaceLensGui
    )
endif()

# MCP is built with CLI/headless trees but is not installed into the current
# public v0.1.2 zip/npm packages. A later release can add the install rule.

# Fail configure if the agent CLI or MCP adapter ever grows a maintenance link.
if(TARGET spacelens-mcp)
    get_target_property(_mcp_link_libs spacelens-mcp LINK_LIBRARIES)
    if(_mcp_link_libs)
        foreach(_lib IN LISTS _mcp_link_libs)
            if(_lib MATCHES "spacelens_maintenance")
                message(FATAL_ERROR
                    "spacelens-mcp must not link spacelens_maintenance")
            endif()
        endforeach()
    endif()
    get_target_property(_mcp_iface spacelens-mcp INTERFACE_LINK_LIBRARIES)
    if(_mcp_iface)
        foreach(_lib IN LISTS _mcp_iface)
            if(_lib MATCHES "spacelens_maintenance")
                message(FATAL_ERROR
                    "spacelens-mcp INTERFACE_LINK_LIBRARIES must not include "
                    "spacelens_maintenance")
            endif()
        endforeach()
    endif()
endif()

if(TARGET spacelens)
    get_target_property(_spacelens_link_libs spacelens LINK_LIBRARIES)
    if(_spacelens_link_libs)
        foreach(_lib IN LISTS _spacelens_link_libs)
            if(_lib MATCHES "spacelens_maintenance")
                message(FATAL_ERROR
                    "spacelens (CLI) must not link spacelens_maintenance")
            endif()
        endforeach()
    endif()
    get_target_property(_spacelens_iface spacelens INTERFACE_LINK_LIBRARIES)
    if(_spacelens_iface)
        foreach(_lib IN LISTS _spacelens_iface)
            if(_lib MATCHES "spacelens_maintenance")
                message(FATAL_ERROR
                    "spacelens (CLI) INTERFACE_LINK_LIBRARIES must not include "
                    "spacelens_maintenance")
            endif()
        endforeach()
    endif()
endif()
