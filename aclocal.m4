#
# Include the TEA standard macro set
#

builtin(include,tclconfig/tcl.m4)

#
# Add here whatever m4 macros you want to define for your package
#

builtin(include,teabase/teabase.m4)

AC_DEFUN([DEDUP_STUBS], [
	if test "${STUBS_BUILD}" = "1"; then
		AC_DEFINE(USE_DEDUP_STUBS, 1, [Use Dedup Stubs])
	fi
])

AC_DEFUN([CygPath],[`${CYGPATH} $1`])

AC_DEFUN([TEAX_CONFIG_INCLUDE_LINE], [
	eval "$1=\"-I[]CygPath($2)\""
	AC_SUBST($1)
])

AC_DEFUN([TEAX_CONFIG_LINK_LINE], [
	AS_IF([test ${TCL_LIB_VERSIONS_OK} = nodots], [
		eval "$1=\"-L[]CygPath($2) -l$3${TCL_TRIM_DOTS}\""
	], [
		eval "$1=\"-L[]CygPath($2) -l$3${PACKAGE_VERSION}\""
	])
	AC_SUBST($1)
])


