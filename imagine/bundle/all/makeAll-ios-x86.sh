makefilesToRun='
	src/btstack/ios-x86.mk

	src/libogg/ios-x86.mk
	
	src/libvorbis/ios-x86.mk
	
	src/flac/ios-x86.mk
	
	src/xz/ios-x86.mk
	
	src/libarchive/ios-x86.mk
	
	src/minizip/ios-x86.mk
	
	src/boost/ios-x86.mk
'

source runMakefiles.sh

runMakefiles $@

