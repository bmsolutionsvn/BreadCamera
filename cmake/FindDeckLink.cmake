# Blackmagic Desktop Video SDK (spec §7.2: optional). DECKLINK_SDK_DIR = the SDK root.
set(_dl "$ENV{DECKLINK_SDK_DIR}")
find_file(DECKLINK_IDL DeckLinkAPI.idl PATHS "${_dl}/Win/include" NO_DEFAULT_PATH)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DeckLink DEFAULT_MSG DECKLINK_IDL)