/**
 ******************************************************************************
  Copyright (c) 2022 IOTREADY S.r.l.

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at https://mozilla.org/MPL/2.0/.
 ******************************************************************************
 */

#include "trackle_utils_bt_functions.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <esp_log.h>
#include <network_provisioning/manager.h>

static const char *BT_TAG = "bt_functions";

typedef struct
{
    char name[MAX_BT_POST_NAME_LEN];
    int (*function)(const char *arg);
} BtPost_t;

typedef struct
{
    char name[MAX_BT_GET_NAME_LEN];
    void *(*function)(const char *arg);
    int dataType;
} BtGet_t;

static int actualBtPostsNum = 0;
static BtPost_t btPosts[MAX_BT_POSTS_NUM];

static int actualBtGetsNum = 0;
static BtGet_t btGets[MAX_BT_GETS_NUM];

static bool isNameAlreadyUsed(const char *name)
{
    for (int i = 0; i < actualBtPostsNum; i++)
    {
        if (strcmp(btPosts[i].name, name) == 0)
        {
            return true;
        }
    }
    for (int i = 0; i < actualBtGetsNum; i++)
    {
        if (strcmp(btGets[i].name, name) == 0)
        {
            return true;
        }
    }
    return false;
}

bool Trackle_BtPost_add(const char *name, int (*function)(const char *))
{
    if (actualBtPostsNum < MAX_BT_POSTS_NUM)
    {
        if (isNameAlreadyUsed(name))
            return false;
        if (strnlen(name, MAX_BT_POST_NAME_LEN) + 1 > MAX_BT_POST_NAME_LEN) // +1 because there must be space for null character
            return false;
        strcpy(btPosts[actualBtPostsNum].name, name);
        btPosts[actualBtPostsNum].function = function;
        actualBtPostsNum++;
        return true;
    }
    return false;
}

bool Trackle_BtGet_add(const char *name, void *(*function)(const char *), Data_TypeDef dataType)
{
    if (actualBtGetsNum < MAX_BT_GETS_NUM)
    {
        if (isNameAlreadyUsed(name))
            return false;
        if (strnlen(name, MAX_BT_GET_NAME_LEN) + 1 > MAX_BT_GET_NAME_LEN) // +1 because there must be space for null character
            return false;
        strcpy(btGets[actualBtGetsNum].name, name);
        btGets[actualBtGetsNum].function = function;
        btGets[actualBtGetsNum].dataType = dataType;
        actualBtGetsNum++;
        return true;
    }
    return false;
}

static const char *bt_endpoint_name_for_index(int bt_fun_idx)
{
    if (bt_fun_idx >= 0 && bt_fun_idx < actualBtPostsNum)
    {
        return btPosts[bt_fun_idx].name;
    }
    const int get_idx = bt_fun_idx - MAX_BT_POSTS_NUM;
    if (get_idx >= 0 && get_idx < actualBtGetsNum)
    {
        return btGets[get_idx].name;
    }
    return "?";
}

static esp_err_t btFunctionCallHandler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen, uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
    (void)session_id;

    // If no input buffer passed, use empty string as arg, else add null character at end of passed string.
    char *args = NULL;
    if (inbuf == NULL)
    {
        args = strdup("");
        if (args == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    else
    {
        args = malloc(sizeof(uint8_t) * (inlen + 1));
        if (args == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
        memcpy(args, inbuf, inlen);
        args[inlen] = '\0';
    }

    const int btFunIdx = *((int *)&priv_data); // Interpret pointer as an integer representing the index of the function of interest.
    const char *ep_name = bt_endpoint_name_for_index(btFunIdx);
    char *funResStr = NULL;

    ESP_LOGI(BT_TAG, "BLE endpoint \"%s\" <- args=\"%s\" (inlen=%d)", ep_name, args, (int)inlen);

    if (btFunIdx < MAX_BT_POSTS_NUM)
    {
        int (*function)(const char *) = btPosts[btFunIdx].function;
        const int funRes = function(args);

        // Return string containing function return code, if there is still memory for its string
        funResStr = malloc(sizeof(char) * 20);
        if (funResStr == NULL)
        {
            free(args);
            return ESP_ERR_NO_MEM;
        }
        sprintf(funResStr, "%d", funRes);
        *outbuf = (uint8_t *)funResStr;
        *outlen = strlen(funResStr) + 1;
    }
    else
    {
        void *(*function)(const char *) = btGets[btFunIdx - MAX_BT_POSTS_NUM].function;
        const void *funRes = function(args);
        switch (btGets[btFunIdx - MAX_BT_POSTS_NUM].dataType)
        {
        case VAR_BOOLEAN:
        {
            bool boolRes = *((bool *)funRes);
            funResStr = strdup(boolRes ? "TRUE" : "FALSE");
            break;
        }
        case VAR_CHAR:
        {
            char charRes[2];
            charRes[0] = *((char *)funRes);
            charRes[1] = '\0';
            funResStr = strdup(charRes);
            break;
        }
        case VAR_DOUBLE:
        {
            double doubleRes = *((double *)funRes);
            char doubleResStr[128];
            const int convBytes = snprintf(doubleResStr, 128, "%f", doubleRes);
            if (convBytes < 0 || convBytes >= 128)
            {
                free(args);
                return ESP_ERR_NO_MEM;
            }
            funResStr = strdup(doubleResStr);
            break;
        }
        case VAR_INT:
        {
            int32_t intRes = *((int32_t *)funRes);
            char intResStr[20];
            const int convBytes = snprintf(intResStr, 20, "%" PRIi32, intRes);
            if (convBytes < 0 || convBytes >= 20)
            {
                free(args);
                return ESP_ERR_NO_MEM;
            }
            funResStr = strdup(intResStr);
            break;
        }
        case VAR_JSON:
        case VAR_STRING:
        {
            char *stringRes = (char *)funRes;
            funResStr = strdup(stringRes != NULL ? stringRes : "");
            break;
        }
        case VAR_LONG:
        {
            int64_t longRes = *((int64_t *)funRes);
            char longResStr[40];
            const int convBytes = snprintf(longResStr, 40, "%" PRIi64, longRes);
            if (convBytes < 0 || convBytes >= 40)
            {
                free(args);
                return ESP_ERR_NO_MEM;
            }
            funResStr = strdup(longResStr);
            break;
        }
        default:
            ESP_LOGE(BT_TAG, "BLE endpoint \"%s\": unexpected data type", ep_name);
            free(args);
            return ESP_ERR_NO_MEM;
        }
        if (funResStr == NULL)
        {
            free(args);
            return ESP_ERR_NO_MEM;
        }
        *outbuf = (uint8_t *)funResStr;
        *outlen = strlen(funResStr) + 1;
    }

    ESP_LOGI(BT_TAG, "BLE endpoint \"%s\" -> response=\"%s\" (len=%d, outlen=%d)",
             ep_name, funResStr, (int)strlen(funResStr), (int)*outlen);

    free(args);

    return ESP_OK;
}

esp_err_t btFunctionsEndpointsCreate()
{
    ESP_LOGI(BT_TAG, "btFunctionsEndpointsCreate: %d POST, %d GET", actualBtPostsNum, actualBtGetsNum);
    for (int i = 0; i < actualBtPostsNum; i++)
    {
        const esp_err_t err = network_prov_mgr_endpoint_create(btPosts[i].name);
        if (err != ESP_OK)
        {
            ESP_LOGE(BT_TAG, "endpoint create failed for POST \"%s\": %s", btPosts[i].name, esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(BT_TAG, "endpoint created POST \"%s\"", btPosts[i].name);
    }
    for (int i = 0; i < actualBtGetsNum; i++)
    {
        const esp_err_t err = network_prov_mgr_endpoint_create(btGets[i].name);
        if (err != ESP_OK)
        {
            ESP_LOGE(BT_TAG, "endpoint create failed for GET \"%s\": %s", btGets[i].name, esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(BT_TAG, "endpoint created GET \"%s\"", btGets[i].name);
    }
    return ESP_OK;
}

esp_err_t btFunctionsEndpointsRegister()
{
    for (int i = 0; i < actualBtPostsNum; i++)
    {
        const esp_err_t err = network_prov_mgr_endpoint_register(btPosts[i].name, btFunctionCallHandler, *((void **)&i));
        if (err != ESP_OK)
            return err;

        ESP_LOGI(BT_TAG, "btFunctionsEndpointsRegister: registered POST endpoint \"%s\"", btPosts[i].name);
    }
    for (int i = 0; i < actualBtGetsNum; i++)
    {
        const int iPlusOffset = i + MAX_BT_POSTS_NUM;
        const esp_err_t err = network_prov_mgr_endpoint_register(btGets[i].name, btFunctionCallHandler, *((void **)&iPlusOffset));
        if (err != ESP_OK)
            return err;

        ESP_LOGI(BT_TAG, "btFunctionsEndpointsRegister: registered GET endpoint \"%s\"", btGets[i].name);
    }
    return ESP_OK;
}
