// This separate directory is necessary for Debian's multiarch with jpeg-turbo,
// because its jpeglib.h cannot perform local inclusion of jconfig.h,
// resulting in it being found within jpeg-quantsmooth and breaking the build.
#include "../libjpegqs.h"
