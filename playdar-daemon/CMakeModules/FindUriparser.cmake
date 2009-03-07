# Created by Max Howell

FIND_LIBRARY (URIPARSER_LIBRARIES NAMES uriparser)
FIND_PATH (URIPARSER_INCLUDE_DIR NAMES uriparser/Uri.h)

INCLUDE (FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(Uriparser DEFAULT_MSG URIPARSER_LIBRARIES URIPARSER_INCLUDE_DIR)

IF (URIPARSER_FOUND)
   INCLUDE(CheckLibraryExists)
   CHECK_LIBRARY_EXISTS(${URIPARSER_LIBRARIES} uriUnescapeInPlaceA "" URIPARSER_NEED_PREFIX)
ENDIF (URIPARSER_FOUND)

MARK_AS_ADVANCED (URIPARSER_FOUND_INCLUDE_DIR URIPARSER_FOUND_LIBRARIES)
