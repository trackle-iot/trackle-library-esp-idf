#include <string>

extern std::string string_device_id;

extern "C" const char *trackleGetDeviceIdAsStr()
{
    return string_device_id.c_str();
}