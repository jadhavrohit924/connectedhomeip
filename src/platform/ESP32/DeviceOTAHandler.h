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

#pragma once

#include <lib/support/Span.h>
#include <lib/core/CHIPError.h>
#include <map>
#include <string>

namespace chip {

/**
 * @brief Structure to track an OTA-capable device entry
 */
struct OTADeviceEntry
{
   uint32_t identifier; // Device identifier
   size_t offset;
   size_t size;
};

/**
 * @brief Interface for device handlers that can receive OTA blocks
 * 
 * Implement this interface to handle OTA operations for specific devices
 */
class DeviceOTAHandler
{
public:
    virtual ~DeviceOTAHandler() = default;

    /**
     * @brief Process an OTA block for the device
     * @param block The block data to process
     * @return true if processing was successful, false otherwise
     */
    virtual bool ProcessBlock(ByteSpan & block) = 0;

    /**
     * @brief Notify the device that an OTA is available
     * @param deviceEntry Information about the OTA
     * @return true if the device accepted the OTA, false otherwise
     */
    virtual bool NotifyOTAAvailable(const OTADeviceEntry & deviceEntry) = 0;

    /**
     * @brief Called when the OTA completes
     * @return true if the completion was acknowledged, false otherwise
     */
    virtual bool OnOTAComplete() = 0;
};

/**
 * @brief Singleton manager for OTA handlers
 * 
 * This class manages registration and access to device OTA handlers
 */
class OTAManager
{
public:
    static OTAManager & GetInstance();

    /**
     * @brief Register a handler for a device
     * @param deviceId The device identifier
     * @param handler Pointer to the handler (ownership remains with the caller)
     * @return CHIP_NO_ERROR if successful, error code otherwise
     */
    CHIP_ERROR RegisterOTAHandler(uint32_t deviceId, DeviceOTAHandler * handler);

    /**
     * @brief Unregister a previously registered handler
     * @param deviceId The device identifier
     * @return CHIP_NO_ERROR if successful, error code otherwise
     */
    CHIP_ERROR UnregisterOTAHandler(uint32_t deviceId);

    /**
     * @brief Get a handler for a device
     * @param deviceId The device identifier
     * @return Pointer to the handler, or nullptr if not found
     */
    DeviceOTAHandler * GetOTAHandler(uint32_t deviceId);

private:
    OTAManager() = default;
    std::map<uint32_t, DeviceOTAHandler *> mHandlers;
};

} // namespace chip 