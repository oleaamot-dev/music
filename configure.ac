dnl configure.ac for GarageJam
dnl Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
dnl SPDX-License-Identifier: GPL-3.0-or-later

AC_PREREQ([2.69])
AC_INIT([GarageJam], [8.0.1],
        [https://www.github.com/oleaamot-dev/music/issues],
        [garagejam], [https://www.garagejam.org/])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([build-aux])

AM_INIT_AUTOMAKE([1.15 gnu subdir-objects dist-xz no-dist-gzip tar-ustar -Wall -Werror -Wno-override])
AM_SILENT_RULES([yes])

AC_PROG_CC
AC_PROG_SED
PKG_PROG_PKG_CONFIG([0.29])

dnl ------------------------------------------------------------------
dnl Dependencies
dnl ------------------------------------------------------------------

GLIB_REQUIRED=2.76
GTK_REQUIRED=4.12
ADW_REQUIRED=1.5
GST_REQUIRED=1.20

PKG_CHECK_MODULES([GARAGEJAM], [
  glib-2.0 >= $GLIB_REQUIRED
  gio-2.0 >= $GLIB_REQUIRED
  gtk4 >= $GTK_REQUIRED
  libadwaita-1 >= $ADW_REQUIRED
  gstreamer-1.0 >= $GST_REQUIRED
])

PKG_CHECK_MODULES([GJCORE], [
  glib-2.0 >= $GLIB_REQUIRED
  gio-2.0 >= $GLIB_REQUIRED
])

AC_SEARCH_LIBS([floor], [m])

dnl gtk-update-icon-cache / update-desktop-database are optional, run at install
AC_PATH_PROG([DESKTOP_FILE_VALIDATE], [desktop-file-validate])
AC_PATH_PROG([APPSTREAM_UTIL], [appstreamcli])
AM_CONDITIONAL([HAVE_DESKTOP_FILE_VALIDATE], [test -n "$DESKTOP_FILE_VALIDATE"])
AM_CONDITIONAL([HAVE_APPSTREAM_UTIL], [test -n "$APPSTREAM_UTIL"])

dnl ------------------------------------------------------------------
dnl Internationalisation (translations are installed from po/ when added)
dnl ------------------------------------------------------------------

GETTEXT_PACKAGE=garagejam
AC_SUBST([GETTEXT_PACKAGE])
AC_DEFINE_UNQUOTED([GETTEXT_PACKAGE], ["$GETTEXT_PACKAGE"], [Gettext domain])

dnl ------------------------------------------------------------------
dnl Compiler warnings
dnl ------------------------------------------------------------------

AC_ARG_ENABLE([werror],
  [AS_HELP_STRING([--enable-werror], [treat compiler warnings as errors])],
  [], [enable_werror=no])

WARN_CFLAGS="-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wformat=2 -Wshadow=local -Wstrict-prototypes"
AS_IF([test "x$enable_werror" = "xyes"], [WARN_CFLAGS="$WARN_CFLAGS -Werror"])
AC_SUBST([WARN_CFLAGS])

AC_CONFIG_FILES([
  Makefile
  data/garagejam.1
])
AC_OUTPUT

AC_MSG_RESULT([
  $PACKAGE_NAME $PACKAGE_VERSION

  prefix:          ${prefix}
  compiler:        ${CC}
  CFLAGS:          ${CFLAGS}
  warnings:        ${WARN_CFLAGS}
])
