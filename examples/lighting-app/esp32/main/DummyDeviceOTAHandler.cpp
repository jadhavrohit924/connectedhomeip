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

#include "DummyDeviceOTAHandler.h"
#include "esp_log.h"
#include <inttypes.h>

#define TAG "DummyOTAHandler"

namespace chip {

DummyDeviceOTAHandler::DummyDeviceOTAHandler(uint32_t deviceId) : mDeviceId(deviceId)
{
    ESP_LOGI(TAG, "Created dummy OTA handler for device '%" PRIu32 "'", deviceId);
}

bool DummyDeviceOTAHandler::NotifyOTAAvailable(const OTADeviceEntry & deviceEntry)
{
    ESP_LOGI(TAG, "[%" PRIu32 "] OTA available: offset=%zu, size=%zu",
        mDeviceId, deviceEntry.offset, deviceEntry.size);
    
    // Get the next OTA partition
    mOTAPartition = esp_ota_get_next_update_partition(NULL);
    if (mOTAPartition == NULL)
    {
        ESP_LOGE(TAG, "[%" PRIu32 "] Failed to find OTA partition", mDeviceId);
        return false;
    }
    
    // Begin OTA update
    esp_err_t err = esp_ota_begin(mOTAPartition, OTA_WITH_SEQUENTIAL_WRITES, &mOTAHandle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "[%" PRIu32 "] esp_ota_begin failed: %s", mDeviceId, esp_err_to_name(err));
        return false;
    }
    
    mBlocksProcessed = 0;
    return true;
}

bool DummyDeviceOTAHandler::ProcessBlock(ByteSpan & block)
{
    mBlocksProcessed++;

    // For demonstration, just log the first few bytes of each block
    char hexBuffer[64] = { 0 };
    size_t logSize = block.size() < 16 ? block.size() : 16; // Log at most 16 bytes
    
    for (size_t i = 0; i < logSize; i++)
    {
        sprintf(hexBuffer + (i * 3), "%02x ", block.data()[i]);
    }

    ESP_LOGI(TAG, "[%" PRIu32 "] Processing block #%" PRIu32 " (%zu bytes), first bytes: %s...", 
        mDeviceId, mBlocksProcessed, block.size(), hexBuffer);
    
    // Write the block to the OTA partition
    esp_err_t err = esp_ota_write(mOTAHandle, block.data(), block.size());
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "[%" PRIu32 "] esp_ota_write failed (%s)", mDeviceId, esp_err_to_name(err));
        return false;
    }
    
    ESP_LOGI(TAG, "[%" PRIu32 "] Block #%" PRIu32 " successfully processed", mDeviceId, mBlocksProcessed);
    return true;
}

bool DummyDeviceOTAHandler::OnOTAComplete()
{
    ESP_LOGI(TAG, "[%" PRIu32 "] OTA complete - processed %" PRIu32 " blocks", 
        mDeviceId, mBlocksProcessed);
    
    // Finalize the OTA update
    esp_err_t err = esp_ota_end(mOTAHandle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "[%" PRIu32 "] esp_ota_end failed: %s", mDeviceId, esp_err_to_name(err));
        return false;
    }
    
    // Set the new boot partition
    err = esp_ota_set_boot_partition(mOTAPartition);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "[%" PRIu32 "] esp_ota_set_boot_partition failed: %s", 
                mDeviceId, esp_err_to_name(err));
        return false;
    }
    
    ESP_LOGI(TAG, "[%" PRIu32 "] OTA update successful! Restart to apply", mDeviceId);
    return true;
}

} // namespace chip 