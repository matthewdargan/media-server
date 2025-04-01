#!/bin/sh
set -eu

for arg in "$@"; do eval "$arg=1"; done
if [ -z "${release:-}" ]; then debug=1; fi
if [ -n "${debug:-}" ];   then echo '[debug mode]'; fi
if [ -n "${release:-}" ]; then echo '[release mode]'; fi

compile_common='-I../src/ -g -fdiagnostics-absolute-paths -Wall -Xclang -flto-visibility-public-std'
compile_debug="clang++ -g -O0 -DBUILD_DEBUG=1 ${compile_common}"
compile_release="clang++ -g -O3 -DBUILD_DEBUG=0 ${compile_common}"
out='-o'
link_mooch="$(pkg-config --cflags --libs libxml-2.0 libtorrent-rasterbar) -lcurl"

if [ -n "${debug:-}" ];   then compile="$compile_debug"; fi
if [ -n "${release:-}" ]; then compile="$compile_release"; fi

mkdir -p build

cd build
if [ -n "${mooch:-}" ];    then didbuild=1 && $compile ../src/mooch/main.cpp $link_mooch $out mooch; fi
if [ -n "${moochrss:-}" ]; then didbuild=1 && $compile ../src/moochrss/main.cpp $link_mooch $out moochrss; fi
cd ..

if [ -z "${didbuild:-}" ]
then
  echo '[WARNING] no valid build target specified; must use build target names as arguments to this script, like ./build.sh mooch.'
  exit 1
fi
