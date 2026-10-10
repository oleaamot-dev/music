#!/bin/sh
exec "$APPSTREAM_UTIL" validate --no-net --explain "$top_srcdir/data/org.garagejam.GarageJam.metainfo.xml"
