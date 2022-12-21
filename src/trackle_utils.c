#include <trackle_utils.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <esp_log.h>

EventGroupHandle_t s_wifi_event_group;

void hexToString(unsigned char *in, size_t insz, char *out, size_t outz)
{
    memset(out, 0, outz);
    unsigned char *pin = in;
    const char *hex = "0123456789abcdef";
    char *pout = out;
    for (; pin < in + insz; pout += 2, pin++)
    {
        pout[0] = hex[(*pin >> 4) & 0xF];
        pout[1] = hex[*pin & 0xF];
        if (pout + 2 - out > insz * 2)
        {
            /* Better to truncate output string than overflow buffer */
            /* it would be still better to either return a status */
            /* or ensure the target buffer is large enough and it never happen */
            break;
        }
    }
}

int stringToHex(char *hex_str, unsigned char *byte_array, int byte_array_max)
{
    int hex_str_len = strlen(hex_str);
    int i = 0, j = 0;

    // The output array size is half the hex_str length (rounded up)
    int byte_array_size = (hex_str_len + 1) / 2;

    if (byte_array_size > byte_array_max)
    {
        // Too big for the output array
        return -1;
    }

    if (hex_str_len % 2 == 1)
    {
        // hex_str is an odd length, so assume an implicit "0" prefix
        if (sscanf(&(hex_str[0]), "%1hhx", &(byte_array[0])) != 1)
        {
            return -1;
        }

        i = j = 1;
    }

    for (; i < hex_str_len; i += 2, j++)
    {
        if (sscanf(&(hex_str[i]), "%2hhx", &(byte_array[j])) != 1)
        {
            return -1;
        }
    }

    return byte_array_size;
}

int splitString(char *value, const char *separator, char *results[], size_t max_results)
{
    char *p = strtok(value, separator);
    int i = 0;

    while (p != NULL && i < max_results)
    {
        results[i++] = p;
        p = strtok(NULL, separator);
    }

    return i;
}

bool isValid(char *input, const char *op, int b)
{
    int a = atoi(input);
    ESP_LOGD("TAG", "%d %s %d", a, op, b);

    if (strcmp(op, "<") == 0)
    {
        return a < b;
    }
    else if (strcmp(op, "<=") == 0)
    {
        return a <= b;
    }
    else if (strcmp(op, ">") == 0)
    {
        return a > b;
    }
    else if (strcmp(op, ">=") == 0)
    {
        return a >= b;
    }
    else if (strcmp(op, "=") == 0)
    {
        return a == b;
    }
    return false;
}

/* struct tm to seconds since Unix epoch */
time_t getGmTimestamp()
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    long year;
    time_t result;
#define MONTHSPERYEAR 12 /* months per calendar year */
    static const int cumdays[MONTHSPERYEAR] =
        {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

    year = 1900 + timeinfo.tm_year + timeinfo.tm_mon / MONTHSPERYEAR;
    result = (year - 1970) * 365 + cumdays[timeinfo.tm_mon % MONTHSPERYEAR];
    result += (year - 1968) / 4;
    result -= (year - 1900) / 100;
    result += (year - 1600) / 400;
    if ((year % 4) == 0 && ((year % 100) != 0 || (year % 400) == 0) &&
        (timeinfo.tm_mon % MONTHSPERYEAR) < 2)
        result--;
    result += timeinfo.tm_mday - 1;
    result *= 24;
    result += timeinfo.tm_hour;
    result *= 60;
    result += timeinfo.tm_min;
    result *= 60;
    result += timeinfo.tm_sec;

    return result;
}

int rssiToPercentage(int rssi)
{
    int signalQualityPercent = 0;
    if (rssi <= -100)
    {
        signalQualityPercent = 0;
    }
    else if (rssi >= -50)
    {
        signalQualityPercent = 100;
    }
    else
    {
        signalQualityPercent = 2 * (rssi + 100);
    }
    return signalQualityPercent;
}