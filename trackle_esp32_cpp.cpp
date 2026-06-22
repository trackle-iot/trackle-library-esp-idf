#include <string>
#include "libcoap_protocol.h"

extern std::string string_device_id;

/* Istanza del protocollo definita in trackle.cpp */
namespace trackle { namespace protocol { extern LibcoapProtocol protocol_instance; } }

extern "C" const char *trackleGetDeviceIdAsStr()
{
    return string_device_id.c_str();
}

/**
 * Serializza il messaggio describe (funzioni, variabili, info sistema) in buf.
 * Da chiamare dopo initTrackle() + trackleSetKeys/DeviceId + registrazione funzioni/variabili.
 * @param buf      Buffer di output (null-terminated)
 * @param max_len  Dimensione massima del buffer
 * @param desc_flags  Flags: 1=SYSTEM, 2=APPLICATION, 3=entrambi
 * @return numero di byte scritti, oppure -1 in caso di errore
 */
extern "C" int trackleGetDescribeJson(char *buf, size_t max_len, int desc_flags)
{
    return trackle::protocol::protocol_instance.serialize_describe_json(
        desc_flags, reinterpret_cast<uint8_t *>(buf), max_len);
}