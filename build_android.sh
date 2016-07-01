#!/bin/bash

BUILD_PREFIX="/data/data/com.systemtap.android/stap/"
OUTPUT_DIR="android_binaries"
STAPIO_BIN="staprun/stapio"
STAPRUN_BIN="staprun/staprun"
COMPILER_PREFIX_LIST=("arm-none-linux-gnueabi" "arm-unknown-linux-gnueabi")


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

ac_cv_with_java=no ac_cv_prog_have_jar=no ac_cv_prog_have_javac=no ac_cv_with_java=no ac_cv_file__usr_include_avahi_common=no ac_cv_file__usr_include_avahi_client=no ac_cv_file__usr_include_nspr=no ac_cv_file__usr_include_nspr4=no ac_cv_file__usr_include_nss=no ac_cv_file__usr_include_nss3=no ac_cv_func_malloc_0_nonnull=yes ./configure --prefix=$BUILD_PREFIX --host=$COMPILER_PREFIX --disable-translator --disable-docs --disable-refdocs  --disable-grapher --without-rpm --without-nss --with-android 

if [ $? -ne 0 ]; then
	echo "Error configuring the workspace" >&2
	exit 1
fi

make 

if [ $? -ne 0 ]; then
	echo "Error building stap{run,io}" >&2
	exit 1
fi

if [ -e $STAPRUN_BIN ] &&  [ -e $STAPIO_RUN ]; then
	if [ ! -d $OUTPUT_DIR ]; then
		mkdir $OUTPUT_DIR
	fi
	cp $STAPRUN_BIN $OUTPUT_DIR
	cp $STAPIO_BIN $OUTPUT_DIR
else
	rm ${OUTPUT_DIR}/${STAPRUN_BIN}
	rm ${OUTPUT_DIR}/${STAPIO_BIN}
	echo "Some binaries were not created."
	exit 1
fi
