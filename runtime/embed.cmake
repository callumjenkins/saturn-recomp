# Writes the file INPUT into OUTPUT as `static const unsigned char NAME[]`, for the build to compile in:
#   cmake -DINPUT=font.ttf -DOUTPUT=font.h -DNAME=font -P embed.cmake
file(READ ${INPUT} hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE ${OUTPUT} "#pragma once\nstatic const unsigned char ${NAME}[] = {${bytes}};\n")
