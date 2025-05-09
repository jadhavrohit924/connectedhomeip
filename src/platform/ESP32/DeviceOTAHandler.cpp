/*
 *    Copyright (c) 2024 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "DeviceOTAHandler.h"

namespace chip {

OTAManager & OTAManager::GetInstance()
{
    static OTAManager instance;
    return instance;
}

CHIP_ERROR OTAManager::RegisterOTAHandler(uint32_t deviceId, DeviceOTAHandler * handler)
{
    if (handler == nullptr)
    {
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    if (mHandlers.find(deviceId) != mHandlers.end())
    {
        return CHIP_ERROR_INCORRECT_STATE;
    }

    mHandlers[deviceId] = handler;
    return CHIP_NO_ERROR;
}

CHIP_ERROR OTAManager::UnregisterOTAHandler(uint32_t deviceId)
{
    auto it = mHandlers.find(deviceId);
    if (it == mHandlers.end())
    {
        return CHIP_ERROR_KEY_NOT_FOUND;
    }

    mHandlers.erase(it);
    return CHIP_NO_ERROR;
}

DeviceOTAHandler * OTAManager::GetOTAHandler(uint32_t deviceId)
{
    auto it = mHandlers.find(deviceId);
    if (it == mHandlers.end())
    {
        return nullptr;
    }

    return it->second;
}

} // namespace chip 