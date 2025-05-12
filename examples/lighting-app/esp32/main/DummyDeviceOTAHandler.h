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

#include <platform/ESP32/DeviceOTAHandler.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include <inttypes.h>

namespace chip {

/**
 * @brief A dummy implementation of DeviceOTAHandler for testing
 * 
 * This implementation logs OTA events and writes blocks to the ESP OTA partition
 */
class DummyDeviceOTAHandler : public DeviceOTAHandler
{
public:
    DummyDeviceOTAHandler(uint32_t deviceId);
    ~DummyDeviceOTAHandler() override = default;

    bool ProcessBlock(ByteSpan & block) override;
    bool NotifyOTAAvailable(const OTADeviceEntry & deviceEntry) override;
    bool OnOTAComplete() override;

private:
    uint32_t mDeviceId;
    uint32_t mBlocksProcessed = 0;
    
    // ESP OTA variables
    esp_ota_handle_t mOTAHandle = 0;
    const esp_partition_t* mOTAPartition = nullptr;
};

} // namespace chip 