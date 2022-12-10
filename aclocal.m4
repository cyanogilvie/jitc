#
# Include the TEA standard macro set
#

builtin(include,tclconfig/tcl.m4)

#
# Add here whatever m4 macros you want to define for your package
#

builtin(include,ax_gcc_builtin.m4)
builtin(include,ax_cc_for_build.m4)
builtin(include,ax_check_compile_flag.m4)

# We need a modified version of TEA_ADD_SOURCES because some of those files will be
# generated *after* the macro runs, so it can't test for existence:
AC_DEFUN([REURI_ADD_SOURCES], [
    vars="$@"
    for i in $vars; do
	case $i in
	    [\$]*)
		# allow $-var names
		PKG_SOURCES="$PKG_SOURCES $i"
		PKG_OBJECTS="$PKG_OBJECTS $i"
		;;
	    *)
		PKG_SOURCES="$PKG_SOURCES $i"
		# this assumes it is in a VPATH dir
		i=`basename $i`
		# handle user calling this before or after TEA_SETUP_COMPILER
		if test x"${OBJEXT}" != x ; then
		    j="`echo $i | sed -e 's/\.[[^.]]*$//'`.${OBJEXT}"
		else
		    j="`echo $i | sed -e 's/\.[[^.]]*$//'`.\${OBJEXT}"
		fi
		PKG_OBJECTS="$PKG_OBJECTS $j"
		;;
	esac
    done
    AC_SUBST(PKG_SOURCES)
    AC_SUBST(PKG_OBJECTS)
])

AC_DEFUN([DEDUP_STUBS], [
	if test "${STUBS_BUILD}" = "1"; then
		AC_DEFINE(USE_DEDUP_STUBS, 1, [Use Dedup Stubs])
	fi
])

AC_DEFUN([CHECK_TESTMODE], [
	AC_MSG_CHECKING([whether to build in test mode])
	AC_ARG_ENABLE(testmode,
		[  --enable-testmode       Build in test mode (default: on)],
		[enable_testmode=$enableval],
		[enable_testmode="yes"])
	AC_MSG_RESULT($enable_testmode)
	if test "$enable_testmode" = "yes"
	then
		AC_DEFINE(TESTMODE)
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

AC_DEFUN([TIP445], [
	AC_MSG_CHECKING([whether we need to polyfill TIP 445])
	saved_CFLAGS="$CFLAGS"
	CFLAGS="$CFLAGS $TCL_INCLUDE_SPEC"
	AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include <tcl.h>]], [[Tcl_ObjInternalRep ir;]])],[have_tcl_objintrep=yes],[have_tcl_objintrep=no])
	CFLAGS="$saved_CFLAGS"

	if test "$have_tcl_objintrep" = yes; then
		AC_DEFINE(TIP445_SHIM, 0, [Do we need to polyfill TIP 445?])
		AC_MSG_RESULT([no])
	else
		AC_DEFINE(TIP445_SHIM, 1, [Do we need to polyfill TIP 445?])
		AC_MSG_RESULT([yes])
	fi
])


