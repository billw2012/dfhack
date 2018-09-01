PROJECT(happyhttp)
ADD_LIBRARY(happyhttp STATIC EXCLUDE_FROM_ALL happyhttp.cpp happyhttp.h)
IDE_FOLDER(happyhttp "Depends")