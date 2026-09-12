# Post-build proof that the declared mod catalog actually arrived.
#
# Run by snesrecomp_target_mod_catalog as a POST_BUILD step, with:
#   SNES_MOD_DEST   the staged <exe-dir>/mods/preloaded/packages
#   SNES_MOD_IDS    '|'-separated package ids the source tree declared
#   SNES_MOD_TARGET the target, for the message
#
# A staged catalog that is missing a package is the failure this exists to
# catch: it is invisible in a build log, and the first person to notice is a
# player whose Mods page is short one entry.
if(NOT DEFINED SNES_MOD_DEST OR NOT DEFINED SNES_MOD_IDS)
    message(FATAL_ERROR "snes_check_mod_catalog: SNES_MOD_DEST and SNES_MOD_IDS are required")
endif()

string(REPLACE "|" ";" _ids "${SNES_MOD_IDS}")
set(_missing "")
foreach(_id IN LISTS _ids)
    if(NOT IS_DIRECTORY "${SNES_MOD_DEST}/${_id}")
        list(APPEND _missing "${_id}")
    endif()
endforeach()

if(_missing)
    list(JOIN _missing "\n    " _pretty)
    message(FATAL_ERROR
        "${SNES_MOD_TARGET}: these packages were declared but did not reach "
        "${SNES_MOD_DEST}:\n"
        "    ${_pretty}\n"
        "The build would ship a Mods page missing them.")
endif()
