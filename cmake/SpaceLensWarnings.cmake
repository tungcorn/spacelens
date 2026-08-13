# First-party warning policy. SQLite, Qt headers, and generated moc/uic are
# outside this file's scope.

function(spacelens_apply_first_party_warnings target)
    if(NOT TARGET "${target}")
        return()
    endif()
    if(MSVC)
        target_compile_options("${target}" PRIVATE /W4 /permissive-)
        if(SPACELENS_MSVC_ANALYZE)
            # Analyzer warnings (C6xxx) are reviewed as a quality gate, not
            # mixed with /WX. /analyze:external- keeps system/Qt/SQLite out.
            target_compile_options("${target}" PRIVATE
                /analyze
                /analyze:external-
                /external:anglebrackets
                /external:W0
            )
        elseif(SPACELENS_MSVC_WARNINGS_AS_ERRORS)
            target_compile_options("${target}" PRIVATE /WX)
        endif()
    endif()
endfunction()
