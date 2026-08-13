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

# Fail configure if the agent CLI ever grows a maintenance link.
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
