#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([clboss], [0.15.1], [ken@bonsai.com])
AC_CONFIG_AUX_DIR([auxdir])
AM_INIT_AUTOMAKE([subdir-objects tar-ustar])
AC_CONFIG_SRCDIR([main.cpp])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIRS([m4])
LT_INIT([disable-shared])

m4_pattern_forbid([^PKG_])

# Enable exception backtrace option
AC_ARG_ENABLE([exception-backtrace],
    [AS_HELP_STRING([--disable-exception-backtrace],
        [Disable exception backtrace in exceptions (enabled by default)])],
    [case "${enable_exception_backtrace}" in
        yes|no) ;;
        *) AC_MSG_ERROR([invalid value for --enable-exception-backtrace: ${enable_exception_backtrace}]) ;;
    esac],
    [enable_exception_backtrace=yes]) # Default to "yes"

AM_CONDITIONAL([ENABLE_EXCEPTION_BACKTRACE], [test "x$enable_exception_backtrace" = "xyes"])
AC_SUBST([ENABLE_EXCEPTION_BACKTRACE], [$enable_exception_backtrace])

# Define a macro in config.h
if test "x$enable_exception_backtrace" = "xyes"; then
    AC_DEFINE([ENABLE_EXCEPTION_BACKTRACE], [1], [Define if exception backtraces are enabled])
else
    AC_DEFINE([ENABLE_EXCEPTION_BACKTRACE], [0], [Define if exception backtraces are enabled])
fi

# Checks for programs.
# If CXXFLAGS is unset, remove the -g
# that AC_PROG_CXX adds to it.
clboss_set_CXXFLAGS=${CXXFLAGS+y}
AC_PROG_CXX
AS_IF([test ${clboss_set_CXXFLAGS}], [
	# AC_PROG_CXX will respect the user-given CXXFLAGS
	:
],[
	# AC_PROG_CXX will default to `-g -O2` if g++,
	# or `-g` otherwise.
	AS_IF([test x"$GXX" = x"yes"], [
		CXXFLAGS="-O2"
	], [
		CXXFLAGS=""
	])
])
AX_CXX_COMPILE_STDCXX([20], , [mandatory])
AC_LANG([C++])

AC_PROG_AWK
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MAKE_SET

AX_VALGRIND_CHECK()
AM_CONDITIONAL([USE_VALGRIND], [test x"$enable_valgrind" = xyes])

# Checks for libraries.
AX_PTHREAD([:],[
	AC_MSG_ERROR([Need pthread.])
])
AX_LIB_EV
PKG_CHECK_MODULES([SQLITE3], [sqlite3 >= 3.0.0])
AX_LIB_CURL(,[:],[
	AC_MSG_ERROR([Need libcurl.])
])

# Check for libunwind
PKG_CHECK_MODULES([LIBUNWIND], [libunwind], [
  AC_DEFINE([HAVE_LIBUNWIND], [1], [Define if libunwind is available])
], [
  AC_MSG_ERROR([libunwind is required but not found])
])

# https://github.com/ZmnSCPxj/clboss/issues/245
case "$host_os" in
  *alpine*)
    LDFLAGS="$LDFLAGS -lexecinfo"
    ;;
esac

AC_SUBST(LDFLAGS)

# Checks for header files.

# Checks for typedefs, structures, and compiler characteristics.
CLBOSS_CXXFLAGS=''
AC_SUBST([CLBOSS_CXXFLAGS])
# Determine if compiling ev.h succeeds with -Wno-noexcept-type
# On FreeBSD ev.h compilation fails without this flag.
AC_MSG_CHECKING([if compiler compiles ev.h with -Wno-noexcept-type])
saved_CXXFLAGS="${CXXFLAGS}"
CXXFLAGS="${CXXFLAGS} ${libev_CFLAGS} -Wno-noexcept-type"
AC_COMPILE_IFELSE([AC_LANG_SOURCE([[
#include<ev.h>
]])], [ # then
    CLBOSS_CXXFLAGS="${CLBOSS_CXXFLAGS} -Wno-noexcept-type"
    AC_MSG_RESULT([yes])
], [ # else
    AC_MSG_RESULT([no])
])
CXXFLAGS="${saved_CXXFLAGS}"

# Determine if the compiler supports __attribute__((format (printf, 2, 3)))
AC_MSG_CHECKING([[if compiler supports __attribute__((format))]])
AC_COMPILE_IFELSE([AC_LANG_SOURCE([[
int fake_printf(void* dummy, char const* fmt, ...)
  __attribute__((format (printf, 2, 3)));
]])], [# then
    AC_DEFINE([HAVE_ATTRIBUTE_FORMAT], [1], [Define if understands gcc __attribute__((format))])
    AC_MSG_RESULT([yes])
], [ # else
    AC_MSG_RESULT([no])
])

# Checks for library functions.
# Check for Posix gmtime_r.
AC_MSG_CHECKING([for gmtime_r])
AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#include<time.h>
]], [[
	time_t timeval = 0;
	struct tm split;
	struct tm* psplit;
	psplit = gmtime_r (&timeval, &split);
]])], [ #then
	AC_DEFINE([HAVE_GMTIME_R], [1],
		  [Define to 1 if you have a gmtime_r function.])
	AC_MSG_RESULT([yes])
], [ #else
	AC_MSG_RESULT([no])
])

# Check for getprogname()
AC_MSG_CHECKING([for getprogname])
AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#include<stdlib.h>
]], [[
	const char* p = getprogname();
	(void)p; // avoid warning about set but unused variable
]])], [ #then
	AC_DEFINE([HAVE_GETPROGNAME], [1],
		  [Define to 1 if you have a getprogname function.])
	AC_MSG_RESULT([yes])
], [ #else
	AC_MSG_RESULT([no])
])

AC_CONFIG_FILES([Makefile
		 external/bitcoin-ripemd160/Makefile
		 external/bitcoin-sha256/Makefile
		])
AC_CONFIG_SUBDIRS([external/basicsecure
		   external/secp256k1
		  ])

AC_OUTPUT
