#!/bin/bash

BUILD_PREFIX="/data/data/com.systemtap.android/stap/"
FLAGS="-DANDROID"
OUTPUT_DIR="android_binaries"
STAPIO_BIN="staprun/stapio"
STAPRUN_BIN="staprun/staprun"
COMPILER_PREFIX_LIST=("arm-linux-androideabi")

#export PATH="/usr/local/CodeSourcery/Sourcery_G++_Lite/bin:$PATH"

function checkCompiler()
{
	for cur_compiler_prefix in ${COMPILER_PREFIX_LIST[@]}
	do
		RET=`${cur_compiler_prefix}-gcc --version 2>&1`
		if [ $? -eq 0 ];
		then
			COMPILER_PREFIX=${cur_compiler_prefix}
			return
		fi
	done;

	echo "No suitable compiler found. Check your PATH" >&2
	exit 1
}

checkCompiler

if [ -e "Makefile" ]; then
	make clean
fi

ac_cv_with_java=no ac_cv_prog_have_jar=no ac_cv_prog_have_javac=no ac_cv_with_java=no ac_cv_file__usr_include_avahi_common=no ac_cv_file__usr_include_avahi_client=no ac_cv_file__usr_include_nspr=no ac_cv_file__usr_include_nspr4=no ac_cv_file__usr_include_nss=no ac_cv_file__usr_include_nss3=no ac_cv_func_malloc_0_nonnull=yes ./configure --prefix=$BUILD_PREFIX --host=$COMPILER_PREFIX CXXFLAGS="$FLAGS" CFLAGS="$FLAGS" --disable-translator --disable-docs --disable-refdocs  --disable-grapher --without-rpm --without-nss

if [ $? -ne 0 ]; then
	echo "Error configuring the workspace"
	exit 1
fi

make

if [ -e $STAPRUN_BIN ] &&  [ -e $STAPIO_RUN ]; then
	if [ ! -d $OUTPUT_DIR ]; then
		mkdir $OUTPUT_DIR
	fi
	cp $STAPRUN_BIN $OUTPUT_DIR
	cp $STAPIO_BIN $OUTPUT_DIR
else
	echo "Some binaries were not created."
	exit 1
fi
