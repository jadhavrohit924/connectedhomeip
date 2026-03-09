/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
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

#include "Device.h"
#include "DeviceCallbacks.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "shell_extension/launch.h"
#include "shell_extension/openthread_cli_register.h"
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>
#include <app/clusters/identify-server/identify-server.h>
#include <app/reporting/reporting.h>
#include <app/util/attribute-storage.h>
#include <app/util/endpoint-config-api.h>
#include <bridged-actions-stub.h>
#include <common/Esp32AppServer.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <lib/core/CHIPError.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/CHIPMemString.h>
#include <lib/support/ZclString.h>
#include <platform/ESP32/ESP32Utils.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <app/InteractionModelEngine.h>
#include <app/server/Server.h>

#if CONFIG_ENABLE_CHIP_SHELL
#include <lib/shell/Engine.h>
#include <lib/shell/commands/Help.h>
#endif // CONFIG_ENABLE_CHIP_SHELL

#if CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER
#include <platform/ESP32/ESP32FactoryDataProvider.h>
#endif // CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER

#if CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER
#include <platform/ESP32/ESP32DeviceInfoProvider.h>
#else
#include <DeviceInfoProviderImpl.h>
#endif // CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER

namespace {
static const uint16_t kSceneTableSize = 16;

#if CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER
chip::DeviceLayer::ESP32FactoryDataProvider sFactoryDataProvider;
#endif // CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER

#if CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER
chip::DeviceLayer::ESP32DeviceInfoProvider gExampleDeviceInfoProvider;
#else
chip::DeviceLayer::DeviceInfoProviderImpl gExampleDeviceInfoProvider;
#endif // CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER

std::unique_ptr<chip::app::Clusters::Actions::ActionsDelegateImpl> sActionsDelegateImpl;
std::unique_ptr<chip::app::Clusters::Actions::ActionsServer> sActionsServer;
} // namespace

extern const char TAG[] = "bridge-app";

using namespace ::chip;
using namespace ::chip::DeviceManager;
using namespace ::chip::Platform;
using namespace ::chip::Credentials;
using namespace ::chip::app::Clusters;

static AppDeviceCallbacks AppCallback;

static const int kNodeLabelSize = 32;
// Current ZCL implementation of Struct uses a max-size array of 254 bytes
static const int kDescriptorAttributeArraySize = 254;

static EndpointId gCurrentEndpointId;
static EndpointId gFirstDynamicEndpointId;
static Device * gDevices[CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT]; // number of dynamic endpoints count

// Dynamic device storage - data versions for bridged devices (devices tracked in gDevices)
static constexpr size_t kMaxBridgedDevices = CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT;
static DataVersion * gBridgedDataVersions[kMaxBridgedDevices];
static size_t gBridgedDeviceCount = 0;

// NVS persistence for bridged devices
static constexpr const char * kBridgeNvsNamespace = "bridge_devs";
static constexpr const char * kEndpointIdsKey     = "ep_id_array";

static uint16_t gBridgedEndpointIds[kMaxBridgedDevices];

struct BridgedDevicePersistentInfo
{
    uint8_t deviceTypeIndex;
    char name[Device::kDeviceNameSize];
    char location[Device::kDeviceLocationSize];
};

static esp_err_t StoreBridgedEndpointIds()
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kBridgeNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(handle, kEndpointIdsKey, gBridgedEndpointIds, sizeof(gBridgedEndpointIds));
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t ReadBridgedEndpointIds()
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kBridgeNvsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        memset(gBridgedEndpointIds, 0xFF, sizeof(gBridgedEndpointIds));
        return err;
    }
    size_t len = sizeof(gBridgedEndpointIds);
    err        = nvs_get_blob(handle, kEndpointIdsKey, gBridgedEndpointIds, &len);
    nvs_close(handle);
    if (err != ESP_OK)
    {
        memset(gBridgedEndpointIds, 0xFF, sizeof(gBridgedEndpointIds));
    }
    return err;
}

static esp_err_t StoreDevicePersistentInfo(uint16_t endpointId, const BridgedDevicePersistentInfo & info)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kBridgeNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }
    char key[16];
    snprintf(key, sizeof(key), "ep_info_%u", endpointId);
    err = nvs_set_blob(handle, key, &info, sizeof(info));
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t ReadDevicePersistentInfo(uint16_t endpointId, BridgedDevicePersistentInfo & info)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kBridgeNvsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        return err;
    }
    char key[16];
    snprintf(key, sizeof(key), "ep_info_%u", endpointId);
    size_t len = sizeof(info);
    err        = nvs_get_blob(handle, key, &info, &len);
    nvs_close(handle);
    return err;
}

static esp_err_t EraseDevicePersistentInfo(uint16_t endpointId)
{
    for (size_t i = 0; i < kMaxBridgedDevices; i++)
    {
        if (gBridgedEndpointIds[i] == endpointId)
        {
            gBridgedEndpointIds[i] = kInvalidEndpointId;
        }
    }
    StoreBridgedEndpointIds();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kBridgeNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }
    char key[16];
    snprintf(key, sizeof(key), "ep_info_%u", endpointId);
    err = nvs_erase_key(handle, key);
    nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void EraseAllBridgePersistence()
{
    nvs_handle_t handle;
    if (nvs_open(kBridgeNvsNamespace, NVS_READWRITE, &handle) == ESP_OK)
    {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    memset(gBridgedEndpointIds, 0xFF, sizeof(gBridgedEndpointIds));
}

// (taken from chip-devices.xml)
#define DEVICE_TYPE_BRIDGED_NODE 0x0013
// (taken from lo-devices.xml)
#define DEVICE_TYPE_LO_ON_OFF_LIGHT 0x0100

// (taken from chip-devices.xml)
#define DEVICE_TYPE_ROOT_NODE 0x0016
// (taken from chip-devices.xml)
#define DEVICE_TYPE_BRIDGE 0x000e

#define DEVICE_TYPE_DIMMABLE_LIGHT 0x0101
#define DEVICE_TYPE_EXTENDED_COLOR_LIGHT 0x010D
#define DEVICE_TYPE_COLOR_TEMPERATURE_LIGHT 0x010C
#define DEVICE_TYPE_FAN 0x002B
#define DEVICE_TYPE_WINDOW_COVERING 0x0202
#define DEVICE_TYPE_THERMOSTAT 0x0301
// Device Version for dynamic endpoints:
#define DEVICE_VERSION_DEFAULT 1

namespace {
/* BRIDGED DEVICE ENDPOINT: contains the following clusters:
   - On/Off
   - Descriptor
   - Bridged Device Basic Information
*/

/* REVISION definitions:
 */

 #define ZCL_DESCRIPTOR_CLUSTER_REVISION (1u)
 #define ZCL_BRIDGED_DEVICE_BASIC_INFORMATION_CLUSTER_REVISION (2u)
 #define ZCL_GROUPS_CLUSTER_REVISION (4u)
 #define ZCL_SCENES_MANAGEMENT_CLUSTER_REVISION (1u)
 #define ZCL_IDENTIFY_CLUSTER_REVISION (6u)
 #define ZCL_FIXED_LABEL_CLUSTER_REVISION (1u)
 #define ZCL_ON_OFF_CLUSTER_REVISION (4u)
 #define ZCL_LEVEL_CONTROL_CLUSTER_REVISION (7u)
 #define ZCL_COLOR_CONTROL_CLUSTER_REVISION (9u)
 #define ZCL_FAN_CONTROL_CLUSTER_REVISION (6u)
 #define ZCL_WINDOW_COVERING_CLUSTER_REVISION (8u)
 #define ZCL_THERMOSTAT_CLUSTER_REVISION (10u)
// Declare On/Off cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(onOffAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::OnOff::Id, BOOLEAN, 1, 0), /* on/off */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();
// TODO: It's not clear whether it would be better to get the command lists from
// the ZAP config on our last fixed endpoint instead.
constexpr CommandId onOffIncomingCommands[] = {
    app::Clusters::OnOff::Commands::Off::Id,
    app::Clusters::OnOff::Commands::On::Id,
    app::Clusters::OnOff::Commands::Toggle::Id,
    app::Clusters::OnOff::Commands::OffWithEffect::Id,
    app::Clusters::OnOff::Commands::OnWithRecallGlobalScene::Id,
    app::Clusters::OnOff::Commands::OnWithTimedOff::Id,
    kInvalidCommandId,
};

// Declare Descriptor cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(descriptorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::DeviceTypeList::Id, ARRAY, kDescriptorAttributeArraySize, 0), /* device list */
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::ServerList::Id, ARRAY, kDescriptorAttributeArraySize, 0), /* server list */
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::ClientList::Id, ARRAY, kDescriptorAttributeArraySize, 0), /* client list */
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::PartsList::Id, ARRAY, kDescriptorAttributeArraySize, 0),  /* parts list */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Bridged Device Basic Information cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(bridgedDeviceBasicAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, CHAR_STRING, kNodeLabelSize, 0), /* NodeLabel */
    DECLARE_DYNAMIC_ATTRIBUTE(BridgedDeviceBasicInformation::Attributes::Reachable::Id, BOOLEAN, 1, 0),              /* Reachable */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Groups cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(groupsAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Groups::Attributes::NameSupport::Id, BITMAP8, 1, 0), /* NameSupport */
DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId groupsIncomingCommands[] = {
    app::Clusters::Groups::Commands::AddGroup::Id,
    app::Clusters::Groups::Commands::RemoveGroup::Id,
    app::Clusters::Groups::Commands::RemoveAllGroups::Id,
    app::Clusters::Groups::Commands::GetGroupMembership::Id,
    app::Clusters::Groups::Commands::ViewGroup::Id,
    kInvalidCommandId,
};

constexpr CommandId groupsOutgoingCommands[] = {
    app::Clusters::Groups::Commands::AddGroupResponse::Id,
    app::Clusters::Groups::Commands::RemoveGroupResponse::Id,
    app::Clusters::Groups::Commands::GetGroupMembershipResponse::Id,
    app::Clusters::Groups::Commands::ViewGroupResponse::Id,
    kInvalidCommandId,
};

// Declare Scenes cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(scenesAttrs)
    DECLARE_DYNAMIC_ATTRIBUTE(ScenesManagement::Attributes::SceneTableSize::Id, INT16U, sizeof(kSceneTableSize), kSceneTableSize), /* SceneTableSize */
    DECLARE_DYNAMIC_ATTRIBUTE(ScenesManagement::Attributes::FabricSceneInfo::Id, ARRAY, CHIP_CONFIG_MAX_FABRICS*sizeof(ScenesManagement::Structs::SceneInfoStruct::Type), CHIP_CONFIG_MAX_FABRICS), /* FabricSceneInfo */
DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId scenesIncomingCommands[] = {
    app::Clusters::ScenesManagement::Commands::AddScene::Id,
    app::Clusters::ScenesManagement::Commands::ViewScene::Id,
    app::Clusters::ScenesManagement::Commands::RemoveScene::Id,
    app::Clusters::ScenesManagement::Commands::RemoveAllScenes::Id,
    app::Clusters::ScenesManagement::Commands::StoreScene::Id,
    app::Clusters::ScenesManagement::Commands::RecallScene::Id,
    app::Clusters::ScenesManagement::Commands::GetSceneMembership::Id,
    kInvalidCommandId,
};

constexpr CommandId scenesOutgoingCommands[] = {
    app::Clusters::ScenesManagement::Commands::AddSceneResponse::Id,
    app::Clusters::ScenesManagement::Commands::ViewSceneResponse::Id,
    app::Clusters::ScenesManagement::Commands::RemoveSceneResponse::Id,
    app::Clusters::ScenesManagement::Commands::RemoveAllScenesResponse::Id,
    app::Clusters::ScenesManagement::Commands::StoreSceneResponse::Id,
    app::Clusters::ScenesManagement::Commands::GetSceneMembershipResponse::Id,
    kInvalidCommandId,
};

// Declare Identify cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(identifyAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Identify::Attributes::IdentifyTime::Id, INT16U, 1, 0), /* IdentifyTime */
    DECLARE_DYNAMIC_ATTRIBUTE(Identify::Attributes::IdentifyType::Id, INT8U, 1, 0), /* IdentifyType */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId identifyIncomingCommands[] = {
    app::Clusters::Identify::Commands::Identify::Id,
    kInvalidCommandId,
};

// Declare Level Control cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(levelControlAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::CurrentLevel::Id, INT8U, 1, 0), /* CurrentLevel */
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::OnLevel::Id, INT8U, 1, 0), /* OnLevel */
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::MinLevel::Id, INT8U, 1, 0), /* MinLevel */
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::MaxLevel::Id, INT8U, 1, 254), /* MaxLevel */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId levelControlIncomingCommands[] = {
    app::Clusters::LevelControl::Commands::MoveToLevel::Id,
    app::Clusters::LevelControl::Commands::Move::Id,
    app::Clusters::LevelControl::Commands::Step::Id,
    app::Clusters::LevelControl::Commands::Stop::Id,
    app::Clusters::LevelControl::Commands::MoveToLevelWithOnOff::Id,
    app::Clusters::LevelControl::Commands::MoveWithOnOff::Id,
    app::Clusters::LevelControl::Commands::StepWithOnOff::Id,
    app::Clusters::LevelControl::Commands::StopWithOnOff::Id,
    kInvalidCommandId,
};

// Declare Color Control cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(colorControlAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorMode::Id, INT8U, 1, 0), /* ColorMode */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::Options::Id, BITMAP8, 1, 0), /* Options */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::NumberOfPrimaries::Id, INT8U, 1, 0), /* NumberOfPrimaries */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::EnhancedColorMode::Id, INT8U, 1, 0), /* EnhancedColorMode */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorCapabilities::Id, BITMAP8, 1, 0), /* ColorCapabilities */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Define you accepted commands for the Color Control cluster
constexpr CommandId colorControlIncomingCommands[] = {
    kInvalidCommandId,
};

// Declare Color Control with Color Temperature cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(colorControlWithColorTemperatureAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorMode::Id, INT8U, 1, 0), /* ColorMode */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::Options::Id, BITMAP8, 1, 0), /* Options */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::NumberOfPrimaries::Id, INT8U, 1, 0), /* NumberOfPrimaries */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::EnhancedColorMode::Id, INT8U, 1, 0), /* EnhancedColorMode */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorCapabilities::Id, BITMAP8, 1, 0), /* ColorCapabilities */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::FeatureMap::Id, BITMAP32, 4, 16), /* FeatureMap */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTemperatureMireds::Id, INT16U, 2, 0), /* ColorTemperatureMireds */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTempPhysicalMinMireds::Id, INT16U, 2, 0), /* ColorTempPhysicalMinMireds */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id, INT16U, 2, 0), /* ColorTempPhysicalMaxMireds */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::CoupleColorTempToLevelMinMireds::Id, INT16U, 2, 0), /* CoupleColorTempToLevelMinMireds */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::StartUpColorTemperatureMireds::Id, INT16U, 2, 0), /* StartUpColorTemperatureMireds */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Define you accepted commands for the Color Control cluster
constexpr CommandId colorControlWithColorTemperatureIncomingCommands[] = {
    app::Clusters::ColorControl::Commands::MoveToColorTemperature::Id,
    app::Clusters::ColorControl::Commands::MoveColorTemperature::Id,
    app::Clusters::ColorControl::Commands::StepColorTemperature::Id,
    app::Clusters::ColorControl::Commands::StopMoveStep::Id,
    kInvalidCommandId,
};


// Declare Fan Control cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(fanControlAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::FanMode::Id, INT8U, 1, 0), /* FanMode */
    DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::FanModeSequence::Id, INT8U, 1, 0), /* FanModeSequence */
    DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::PercentSetting::Id, INT8U, 1, 0), /* PercentSetting */
    DECLARE_DYNAMIC_ATTRIBUTE(FanControl::Attributes::PercentCurrent::Id, INT8U, 1, 0), /* PercentCurrent */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Define you accepted commands for the Fan Control cluster
constexpr CommandId fanControlIncomingCommands[] = {
    kInvalidCommandId,
};

// Declare Window Covering cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(windowCoveringAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::Type::Id, INT8U, 1, 0), /* Type */
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::ConfigStatus::Id, BITMAP8, 1, 0), /* ConfigStatus */
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::EndProductType::Id, INT8U, 1, 0), /* EndProductType */
    DECLARE_DYNAMIC_ATTRIBUTE(WindowCovering::Attributes::Mode::Id, BITMAP8, 1, 0), /* Mode */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Define you accepted commands for the Window Covering cluster
constexpr CommandId windowCoveringIncomingCommands[] = {
    app::Clusters::WindowCovering::Commands::UpOrOpen::Id,
    app::Clusters::WindowCovering::Commands::DownOrClose::Id,
    app::Clusters::WindowCovering::Commands::StopMotion::Id,
    kInvalidCommandId,
};

// Declare Thermostat cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(thermostatAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::LocalTemperature::Id, INT16U, 2, 0), /* LocalTemperature */
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::ControlSequenceOfOperation::Id, INT8U, 1, 0), /* ControlSequenceOfOperation */
    DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::SystemMode::Id, INT8U, 1, 0), /* SystemMode */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Define you accepted commands for the Thermostat cluster
constexpr CommandId thermostatIncomingCommands[] = {
    app::Clusters::Thermostat::Commands::SetpointRaiseLower::Id,
    kInvalidCommandId,
};

// Declare Cluster List for Bridged Light endpoint
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), identifyIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Groups::Id, groupsAttrs, ZAP_CLUSTER_MASK(SERVER), groupsIncomingCommands, groupsOutgoingCommands),
    DECLARE_DYNAMIC_CLUSTER(ScenesManagement::Id, scenesAttrs, ZAP_CLUSTER_MASK(SERVER), scenesIncomingCommands, scenesOutgoingCommands),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

// Declare Bridged Light endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedLightEndpoint, bridgedLightClusters);

// Declare Cluster List for Bridged Dimmable Light endpoint
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedDimmableLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), identifyIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Groups::Id, groupsAttrs, ZAP_CLUSTER_MASK(SERVER), groupsIncomingCommands, groupsOutgoingCommands),
    DECLARE_DYNAMIC_CLUSTER(ScenesManagement::Id, scenesAttrs, ZAP_CLUSTER_MASK(SERVER), scenesIncomingCommands, scenesOutgoingCommands),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelControlAttrs, ZAP_CLUSTER_MASK(SERVER), levelControlIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

// Declare Bridged Dimmable Light endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedDimmableLightEndpoint, bridgedDimmableLightClusters);

// Declare Cluster List for Bridged Extended Color Light endpoint
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedExtendedColorLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), identifyIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Groups::Id, groupsAttrs, ZAP_CLUSTER_MASK(SERVER), groupsIncomingCommands, groupsOutgoingCommands),
    DECLARE_DYNAMIC_CLUSTER(ScenesManagement::Id, scenesAttrs, ZAP_CLUSTER_MASK(SERVER), scenesIncomingCommands, scenesOutgoingCommands),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelControlAttrs, ZAP_CLUSTER_MASK(SERVER), levelControlIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ColorControl::Id, colorControlAttrs, ZAP_CLUSTER_MASK(SERVER), colorControlIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

// Declare Bridged Extended Color Light endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedExtendedColorLightEndpoint, bridgedExtendedColorLightClusters);

// Declare Cluster List for Bridged Color Temperature Light endpoint
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedColorTemperatureLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), identifyIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Groups::Id, groupsAttrs, ZAP_CLUSTER_MASK(SERVER), groupsIncomingCommands, groupsOutgoingCommands),
    DECLARE_DYNAMIC_CLUSTER(ScenesManagement::Id, scenesAttrs, ZAP_CLUSTER_MASK(SERVER), scenesIncomingCommands, scenesOutgoingCommands),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelControlAttrs, ZAP_CLUSTER_MASK(SERVER), levelControlIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ColorControl::Id, colorControlWithColorTemperatureAttrs, ZAP_CLUSTER_MASK(SERVER), colorControlWithColorTemperatureIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

// Declare Bridged Color Temperature Light endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedColorTemperatureLightEndpoint, bridgedColorTemperatureLightClusters);

// Declare Cluster List for Bridged Fan endpoint
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedFanClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), identifyIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Groups::Id, groupsAttrs, ZAP_CLUSTER_MASK(SERVER), groupsIncomingCommands, groupsOutgoingCommands),
    DECLARE_DYNAMIC_CLUSTER(FanControl::Id, fanControlAttrs, ZAP_CLUSTER_MASK(SERVER), fanControlIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

// Declare Bridged Fan endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedFanEndpoint, bridgedFanClusters);

// Declare Cluster List for Bridged Window Covering endpoint
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedWindowCoveringClusters)
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), identifyIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Groups::Id, groupsAttrs, ZAP_CLUSTER_MASK(SERVER), groupsIncomingCommands, groupsOutgoingCommands),
    DECLARE_DYNAMIC_CLUSTER(WindowCovering::Id, windowCoveringAttrs, ZAP_CLUSTER_MASK(SERVER), windowCoveringIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

// Declare Bridged Window Covering endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedWindowCoveringEndpoint, bridgedWindowCoveringClusters);

// Declare Cluster List for Bridged Thermostat endpoint
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedThermostatClusters)
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), identifyIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Groups::Id, groupsAttrs, ZAP_CLUSTER_MASK(SERVER), groupsIncomingCommands, groupsOutgoingCommands),
    DECLARE_DYNAMIC_CLUSTER(Thermostat::Id, thermostatAttrs, ZAP_CLUSTER_MASK(SERVER), thermostatIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

// Declare Bridged Thermostat endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedThermostatEndpoint, bridgedThermostatClusters);

} // namespace

int AddDeviceEndpoint(Device * dev, EmberAfEndpointType * ep, const Span<const EmberAfDeviceType> & deviceTypeList,
                      const Span<DataVersion> & dataVersionStorage, chip::EndpointId parentEndpointId)
{
    uint8_t index = 0;
    while (index < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT)
    {
        if (NULL == gDevices[index])
        {
            gDevices[index] = dev;
            CHIP_ERROR err;
            while (true)
            {
                dev->SetEndpointId(gCurrentEndpointId);
                err =
                    emberAfSetDynamicEndpoint(index, gCurrentEndpointId, ep, dataVersionStorage, deviceTypeList, parentEndpointId);
                if (err == CHIP_NO_ERROR)
                {
                    ChipLogProgress(DeviceLayer, "Added device %s to dynamic endpoint %d (index=%d)", dev->GetName(),
                                    gCurrentEndpointId, index);
                    return index;
                }
                else if (err != CHIP_ERROR_ENDPOINT_EXISTS)
                {
                    gDevices[index] = nullptr;
                    return -1;
                }
                // Handle wrap condition
                if (++gCurrentEndpointId < gFirstDynamicEndpointId)
                {
                    gCurrentEndpointId = gFirstDynamicEndpointId;
                }
            }
        }
        index++;
    }
    ChipLogProgress(DeviceLayer, "Failed to add dynamic endpoint: No endpoints available!");
    return -1;
}

int ResumeDeviceEndpoint(Device * dev, EmberAfEndpointType * ep, const Span<const EmberAfDeviceType> & deviceTypeList,
                         const Span<DataVersion> & dataVersionStorage, chip::EndpointId endpointId,
                         chip::EndpointId parentEndpointId)
{
    uint8_t index = 0;
    while (index < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT)
    {
        if (NULL == gDevices[index])
        {
            gDevices[index] = dev;
            dev->SetEndpointId(endpointId);
            CHIP_ERROR err = emberAfSetDynamicEndpoint(index, endpointId, ep, dataVersionStorage, deviceTypeList, parentEndpointId);
            if (err == CHIP_NO_ERROR)
            {
                ChipLogProgress(DeviceLayer, "Resumed device %s to dynamic endpoint %d (index=%d)", dev->GetName(), endpointId,
                                index);
                return index;
            }
            gDevices[index] = nullptr;
            return -1;
        }
        index++;
    }
    ChipLogProgress(DeviceLayer, "Failed to resume dynamic endpoint: No slots available!");
    return -1;
}

CHIP_ERROR RemoveDeviceEndpoint(Device * dev)
{
    for (uint8_t index = 0; index < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT; index++)
    {
        if (gDevices[index] == dev)
        {
            // Silence complaints about unused ep when progress logging
            // disabled.
            [[maybe_unused]] EndpointId ep = emberAfClearDynamicEndpoint(index);
            gDevices[index]                = NULL;
            ChipLogProgress(DeviceLayer, "Removed device %s from dynamic endpoint %d (index=%d)", dev->GetName(), ep, index);
            return CHIP_NO_ERROR;
        }
    }
    return CHIP_ERROR_INTERNAL;
}

Protocols::InteractionModel::Status HandleReadBridgedDeviceBasicAttribute(Device * dev, chip::AttributeId attributeId,
                                                                          uint8_t * buffer, uint16_t maxReadLength)
{
    using namespace BridgedDeviceBasicInformation::Attributes;
    ChipLogProgress(DeviceLayer, "HandleReadBridgedDeviceBasicAttribute: attrId=%" PRIu32 ", maxReadLength=%u", attributeId,
                    maxReadLength);

    if ((attributeId == Reachable::Id) && (maxReadLength == 1))
    {
        *buffer = dev->IsReachable() ? 1 : 0;
    }
    else if ((attributeId == NodeLabel::Id) && (maxReadLength == 32))
    {
        MutableByteSpan zclNameSpan(buffer, maxReadLength);
        MakeZclCharString(zclNameSpan, dev->GetName());
    }
    else if ((attributeId == ClusterRevision::Id) && (maxReadLength == 2))
    {
        uint16_t rev = ZCL_BRIDGED_DEVICE_BASIC_INFORMATION_CLUSTER_REVISION;
        memcpy(buffer, &rev, sizeof(rev));
    }
    else
    {
        return Protocols::InteractionModel::Status::Failure;
    }

    return Protocols::InteractionModel::Status::Success;
}

Protocols::InteractionModel::Status HandleReadOnOffAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer,
                                                             uint16_t maxReadLength)
{
    ChipLogProgress(DeviceLayer, "HandleReadOnOffAttribute: attrId=%" PRIu32 ", maxReadLength=%u", attributeId, maxReadLength);

    if ((attributeId == OnOff::Attributes::OnOff::Id) && (maxReadLength == 1))
    {
        *buffer = dev->IsOn() ? 1 : 0;
    }
    else if ((attributeId == OnOff::Attributes::ClusterRevision::Id) && (maxReadLength == 2))
    {
        uint16_t rev = ZCL_ON_OFF_CLUSTER_REVISION;
        memcpy(buffer, &rev, sizeof(rev));
    }
    else
    {
        return Protocols::InteractionModel::Status::Failure;
    }

    return Protocols::InteractionModel::Status::Success;
}

Protocols::InteractionModel::Status HandleWriteOnOffAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer)
{
    ChipLogProgress(DeviceLayer, "HandleWriteOnOffAttribute: attrId=%" PRIu32, attributeId);

    VerifyOrReturnError((attributeId == OnOff::Attributes::OnOff::Id) && dev->IsReachable(),
                        Protocols::InteractionModel::Status::Failure);
    dev->SetOnOff(*buffer == 1);
    return Protocols::InteractionModel::Status::Success;
}

Protocols::InteractionModel::Status emberAfExternalAttributeReadCallback(EndpointId endpoint, ClusterId clusterId,
                                                                         const EmberAfAttributeMetadata * attributeMetadata,
                                                                         uint8_t * buffer, uint16_t maxReadLength)
{
    uint16_t endpointIndex = emberAfGetDynamicIndexFromEndpoint(endpoint);

    if ((endpointIndex < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT) && (gDevices[endpointIndex] != NULL))
    {
        Device * dev = gDevices[endpointIndex];

        if (clusterId == BridgedDeviceBasicInformation::Id)
        {
            return HandleReadBridgedDeviceBasicAttribute(dev, attributeMetadata->attributeId, buffer, maxReadLength);
        }
        else if (clusterId == OnOff::Id)
        {
            return HandleReadOnOffAttribute(dev, attributeMetadata->attributeId, buffer, maxReadLength);
        }
    }

    return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status emberAfExternalAttributeWriteCallback(EndpointId endpoint, ClusterId clusterId,
                                                                          const EmberAfAttributeMetadata * attributeMetadata,
                                                                          uint8_t * buffer)
{
    uint16_t endpointIndex = emberAfGetDynamicIndexFromEndpoint(endpoint);

    if (endpointIndex < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT)
    {
        Device * dev = gDevices[endpointIndex];

        if ((dev->IsReachable()) && (clusterId == OnOff::Id))
        {
            return HandleWriteOnOffAttribute(dev, attributeMetadata->attributeId, buffer);
        }
    }

    return Protocols::InteractionModel::Status::Failure;
}

namespace {
void CallReportingCallback(intptr_t closure)
{
    auto path = reinterpret_cast<app::ConcreteAttributePath *>(closure);
    MatterReportingAttributeChangeCallback(*path);
    Platform::Delete(path);
}

void ScheduleReportingCallback(Device * dev, ClusterId cluster, AttributeId attribute)
{
    auto * path = Platform::New<app::ConcreteAttributePath>(dev->GetEndpointId(), cluster, attribute);
    DeviceLayer::PlatformMgr().ScheduleWork(CallReportingCallback, reinterpret_cast<intptr_t>(path));
}
} // anonymous namespace

void HandleDeviceStatusChanged(Device * dev, Device::Changed_t itemChangedMask)
{
    if (itemChangedMask & Device::kChanged_Reachable)
    {
        ScheduleReportingCallback(dev, BridgedDeviceBasicInformation::Id, BridgedDeviceBasicInformation::Attributes::Reachable::Id);
    }

    if (itemChangedMask & Device::kChanged_State)
    {
        ScheduleReportingCallback(dev, OnOff::Id, OnOff::Attributes::OnOff::Id);
    }

    if (itemChangedMask & Device::kChanged_Name)
    {
        ScheduleReportingCallback(dev, BridgedDeviceBasicInformation::Id, BridgedDeviceBasicInformation::Attributes::NodeLabel::Id);
    }
}

const EmberAfDeviceType gRootDeviceTypes[]          = { { DEVICE_TYPE_ROOT_NODE, DEVICE_VERSION_DEFAULT } };
const EmberAfDeviceType gAggregateNodeDeviceTypes[] = { { DEVICE_TYPE_BRIDGE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gBridgedOnOffDeviceTypes[] = { { DEVICE_TYPE_LO_ON_OFF_LIGHT, DEVICE_VERSION_DEFAULT },
                                                       { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gBridgedDimmableDeviceTypes[] = { { DEVICE_TYPE_DIMMABLE_LIGHT, DEVICE_VERSION_DEFAULT },
                                                          { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gBridgedExtColorDeviceTypes[] = { { DEVICE_TYPE_EXTENDED_COLOR_LIGHT, DEVICE_VERSION_DEFAULT },
                                                          { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gBridgedColorTempDeviceTypes[] = { { DEVICE_TYPE_COLOR_TEMPERATURE_LIGHT, DEVICE_VERSION_DEFAULT },
                                                           { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gBridgedFanDeviceTypes[] = { { DEVICE_TYPE_FAN, DEVICE_VERSION_DEFAULT },
                                                     { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gBridgedWindowCoveringDeviceTypes[] = { { DEVICE_TYPE_WINDOW_COVERING, DEVICE_VERSION_DEFAULT },
                                                                { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gBridgedThermostatDeviceTypes[] = { { DEVICE_TYPE_THERMOSTAT, DEVICE_VERSION_DEFAULT },
                                                            { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

struct BridgedDeviceTypeInfo
{
    const char * name;
    const char * defaultPrefix;
    EmberAfEndpointType * endpoint;
    const EmberAfDeviceType * deviceTypes;
    size_t deviceTypesCount;
    size_t clusterCount;
};

static const BridgedDeviceTypeInfo kDeviceTypeTable[] = {
    { "onoff_light", "OnOff Light", &bridgedLightEndpoint, gBridgedOnOffDeviceTypes,
      MATTER_ARRAY_SIZE(gBridgedOnOffDeviceTypes), MATTER_ARRAY_SIZE(bridgedLightClusters) },
    { "dimmable_light", "Dimmable Light", &bridgedDimmableLightEndpoint, gBridgedDimmableDeviceTypes,
      MATTER_ARRAY_SIZE(gBridgedDimmableDeviceTypes), MATTER_ARRAY_SIZE(bridgedDimmableLightClusters) },
    { "extended_color_light", "Extended Color Light", &bridgedExtendedColorLightEndpoint, gBridgedExtColorDeviceTypes,
      MATTER_ARRAY_SIZE(gBridgedExtColorDeviceTypes), MATTER_ARRAY_SIZE(bridgedExtendedColorLightClusters) },
    { "color_temperature_light", "Color Temperature Light", &bridgedColorTemperatureLightEndpoint, gBridgedColorTempDeviceTypes,
      MATTER_ARRAY_SIZE(gBridgedColorTempDeviceTypes), MATTER_ARRAY_SIZE(bridgedColorTemperatureLightClusters) },
    { "fan", "Fan", &bridgedFanEndpoint, gBridgedFanDeviceTypes, MATTER_ARRAY_SIZE(gBridgedFanDeviceTypes),
      MATTER_ARRAY_SIZE(bridgedFanClusters) },
    { "window_covering", "Window Covering", &bridgedWindowCoveringEndpoint, gBridgedWindowCoveringDeviceTypes,
      MATTER_ARRAY_SIZE(gBridgedWindowCoveringDeviceTypes), MATTER_ARRAY_SIZE(bridgedWindowCoveringClusters) },
    { "thermostat", "Thermostat", &bridgedThermostatEndpoint, gBridgedThermostatDeviceTypes,
      MATTER_ARRAY_SIZE(gBridgedThermostatDeviceTypes), MATTER_ARRAY_SIZE(bridgedThermostatClusters) },
};

static const BridgedDeviceTypeInfo * FindDeviceTypeInfo(const char * typeName)
{
    for (const auto & entry : kDeviceTypeTable)
    {
        if (strcasecmp(typeName, entry.name) == 0)
        {
            return &entry;
        }
    }
    return nullptr;
}

static bool FindDeviceTypeIndex(const char * typeName, size_t & outIndex)
{
    for (size_t i = 0; i < MATTER_ARRAY_SIZE(kDeviceTypeTable); i++)
    {
        if (strcasecmp(typeName, kDeviceTypeTable[i].name) == 0)
        {
            outIndex = i;
            return true;
        }
    }
    return false;
}

#if CONFIG_ENABLE_CHIP_SHELL
using chip::Shell::Engine;
using chip::Shell::shell_command_t;

Engine sShellBridgeSubCommands;

// Find device index by endpoint ID
static CHIP_ERROR FindDeviceByEndpoint(EndpointId endpointId, uint16_t & index)
{
    for (size_t i = 0; i < kMaxBridgedDevices; i++)
    {
        if (gDevices[i] != nullptr && gDevices[i]->GetEndpointId() == endpointId)
        {
            index = static_cast<uint16_t>(i);
            return CHIP_NO_ERROR;
        }
    }
    return CHIP_ERROR_NOT_FOUND;
}

static CHIP_ERROR BridgeHelpHandler(int argc, char ** argv)
{
    sShellBridgeSubCommands.ForEachCommand(chip::Shell::PrintCommandHelp, nullptr);
    return CHIP_NO_ERROR;
}

static CHIP_ERROR BridgeAddHandler(int argc, char ** argv)
{
    if (argc < 1 || argc > 3)
    {
        ESP_LOGE(TAG, "Usage: bridge add <type> [name] [location]");
        ESP_LOGE(TAG, "  type: onoff_light, dimmable_light, extended_color_light,");
        ESP_LOGE(TAG, "        color_temperature_light, fan, window_covering, thermostat");
        ESP_LOGE(TAG, "  name: optional device name");
        ESP_LOGE(TAG, "  location: optional location (default: 'Room')");
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    const BridgedDeviceTypeInfo * typeInfo = FindDeviceTypeInfo(argv[0]);
    if (typeInfo == nullptr)
    {
        ESP_LOGE(TAG, "Unknown device type '%s'", argv[0]);
        ESP_LOGE(TAG, "  Supported: onoff_light, dimmable_light, extended_color_light,");
        ESP_LOGE(TAG, "             color_temperature_light, fan, window_covering, thermostat");
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    if (gBridgedDeviceCount >= kMaxBridgedDevices)
    {
        ESP_LOGE(TAG, "Max bridged devices reached (%u). Cannot add more.", kMaxBridgedDevices);
        return CHIP_ERROR_NO_MEMORY;
    }

    char defaultName[32];
    const char * name;
    if (argc >= 2)
    {
        name = argv[1];
    }
    else
    {
        snprintf(defaultName, sizeof(defaultName), "%s %u", typeInfo->defaultPrefix,
                 static_cast<unsigned>(gBridgedDeviceCount + 1));
        name = defaultName;
    }

    const char * location = (argc >= 3) ? argv[2] : "Room";

    Device * newDevice = new (std::nothrow) Device(name, location);
    if (newDevice == nullptr)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for new device");
        return CHIP_ERROR_NO_MEMORY;
    }

    DataVersion * newDataVersions = new (std::nothrow) DataVersion[typeInfo->clusterCount];
    if (newDataVersions == nullptr)
    {
        delete newDevice;
        ESP_LOGE(TAG, "Failed to allocate memory for data versions");
        return CHIP_ERROR_NO_MEMORY;
    }
    memset(newDataVersions, 0, sizeof(DataVersion) * typeInfo->clusterCount);

    newDevice->SetReachable(true);
    newDevice->SetChangeCallback(&HandleDeviceStatusChanged);

    // AddDeviceEndpoint stores newDevice in gDevices[result] internally,
    // so we only need to track the data versions at the same index.
    int result = AddDeviceEndpoint(
        newDevice, typeInfo->endpoint,
        Span<const EmberAfDeviceType>(typeInfo->deviceTypes, typeInfo->deviceTypesCount),
        Span<DataVersion>(newDataVersions, typeInfo->clusterCount), 1);
    if (result < 0)
    {
        delete newDevice;
        delete[] newDataVersions;
        ESP_LOGE(TAG, "Failed to add device endpoint - no available endpoints");
        return CHIP_ERROR_ENDPOINT_POOL_FULL;
    }

    gBridgedDataVersions[result] = newDataVersions;
    gBridgedDeviceCount++;

    // Persist to NVS
    size_t typeIndex;
    if (FindDeviceTypeIndex(argv[0], typeIndex))
    {
        BridgedDevicePersistentInfo persistInfo;
        memset(&persistInfo, 0, sizeof(persistInfo));
        persistInfo.deviceTypeIndex = static_cast<uint8_t>(typeIndex);
        chip::Platform::CopyString(persistInfo.name, name);
        chip::Platform::CopyString(persistInfo.location, location);

        uint16_t epId = newDevice->GetEndpointId();
        StoreDevicePersistentInfo(epId, persistInfo);
        gBridgedEndpointIds[result] = epId;
        StoreBridgedEndpointIds();
        ESP_LOGI(TAG, "Persisted device to NVS (endpoint %u, slot %d)", epId, result);
    }

    ESP_LOGI(TAG, "Added %s '%s' @ %s (endpoint %d) [%u/%u]", typeInfo->name, name, location, newDevice->GetEndpointId(),
             gBridgedDeviceCount, kMaxBridgedDevices);

    return CHIP_NO_ERROR;
}

static CHIP_ERROR BridgeRemoveHandler(int argc, char ** argv)
{
    if (argc != 1)
    {
        ESP_LOGE(TAG, "Usage: bridge remove <endpoint>");
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    char * end;
    chip::EndpointId endpointId = strtoul(argv[0], &end, 10);
    if (end == argv[0] || *end != '\0' || endpointId > 0xFFFF)
    {
        ESP_LOGE(TAG, "Invalid endpoint ID: %s (must be 0-65535)", argv[0]);
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    uint16_t index;
    CHIP_ERROR err = FindDeviceByEndpoint(endpointId, index);
    if (err != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "No device at endpoint %u", endpointId);
        return err;
    }

    const char * name = gDevices[index]->GetName();
    err               = RemoveDeviceEndpoint(gDevices[index]);
    if (err == CHIP_NO_ERROR)
    {
        ESP_LOGI(TAG, "Removed '%s' from endpoint %u", name, endpointId);
        EraseDevicePersistentInfo(endpointId);
        delete gDevices[index];
        delete[] gBridgedDataVersions[index];
        gDevices[index]             = nullptr;
        gBridgedDataVersions[index] = nullptr;
        gBridgedDeviceCount--;
    }
    else
    {
        ESP_LOGE(TAG, "Failed to remove '%s'", name);
        return err;
    }

    return CHIP_NO_ERROR;
}

static CHIP_ERROR BridgeListHandler(int argc, char ** argv)
{
    ESP_LOGI(TAG, "Bridged devices (%u/%u):", gBridgedDeviceCount, kMaxBridgedDevices);

    if (gBridgedDeviceCount == 0)
    {
        ESP_LOGI(TAG, "  (none)");
    }
    else
    {
        for (size_t i = 0; i < kMaxBridgedDevices; i++)
        {
            if (gDevices[i] != nullptr)
            {
                ESP_LOGI(TAG, "  \"%s\" @ %s (endpoint %d)", gDevices[i]->GetName(), gDevices[i]->GetLocation(),
                         gDevices[i]->GetEndpointId());
            }
        }
    }

    return CHIP_NO_ERROR;
}

static CHIP_ERROR BridgeToggleHandler(int argc, char ** argv)
{
    if (argc == 1)
    {
        char * end;
        chip::EndpointId endpointId = strtoul(argv[0], &end, 10);
        if (end == argv[0] || *end != '\0' || endpointId > 0xFFFF)
        {
            ESP_LOGE(TAG, "Invalid endpoint ID: %s (must be 0-65535)", argv[0]);
            return CHIP_ERROR_INVALID_ARGUMENT;
        }

        uint16_t index;
        CHIP_ERROR err = FindDeviceByEndpoint(endpointId, index);
        if (err != CHIP_NO_ERROR)
        {
            ESP_LOGE(TAG, "No device at endpoint %u", endpointId);
            return err;
        }

        if (!emberAfContainsServer(endpointId, OnOff::Id))
        {
            ESP_LOGE(TAG, "'%s' (endpoint %u) does not support OnOff", gDevices[index]->GetName(), endpointId);
            return CHIP_ERROR_INVALID_ARGUMENT;
        }

        gDevices[index]->SetOnOff(!gDevices[index]->IsOn());
        ESP_LOGI(TAG, "Toggled '%s' (endpoint %u): now %s", gDevices[index]->GetName(), endpointId,
                 gDevices[index]->IsOn() ? "ON" : "OFF");
    }
    else
    {
        ESP_LOGI(TAG, "Toggling all OnOff devices:");
        for (size_t i = 0; i < kMaxBridgedDevices; i++)
        {
            if (gDevices[i] != nullptr && emberAfContainsServer(gDevices[i]->GetEndpointId(), OnOff::Id))
            {
                gDevices[i]->SetOnOff(!gDevices[i]->IsOn());
                ESP_LOGI(TAG, "  '%s': now %s", gDevices[i]->GetName(), gDevices[i]->IsOn() ? "ON" : "OFF");
            }
        }
    }

    return CHIP_NO_ERROR;
}

static CHIP_ERROR BridgeMaxHandler(int argc, char ** argv)
{
    ESP_LOGI(TAG, "Bridge endpoint limits:");
    ESP_LOGI(TAG, "  Max bridged devices: %u", kMaxBridgedDevices);
    ESP_LOGI(TAG, "  Current devices: %u", gBridgedDeviceCount);
    ESP_LOGI(TAG, "  Available slots: %u", kMaxBridgedDevices - gBridgedDeviceCount);

    return CHIP_NO_ERROR;
}

static CHIP_ERROR BridgeRemoveAllHandler(int argc, char ** argv)
{
    ESP_LOGI(TAG, "Removing all bridged devices...");
    size_t removedCount = 0;

    for (size_t i = 0; i < kMaxBridgedDevices; i++)
    {
        if (gDevices[i] != nullptr)
        {
            const char * name = gDevices[i]->GetName();
            CHIP_ERROR err    = RemoveDeviceEndpoint(gDevices[i]);
            if (err == CHIP_NO_ERROR)
            {
                ESP_LOGI(TAG, "  Removed '%s'", name);
                delete gDevices[i];
                delete[] gBridgedDataVersions[i];
                gDevices[i]             = nullptr;
                gBridgedDataVersions[i] = nullptr;
                removedCount++;
            }
            else
            {
                ESP_LOGE(TAG, "  Failed to remove '%s'", name);
            }
        }
    }
    gBridgedDeviceCount -= removedCount;
    EraseAllBridgePersistence();

    ESP_LOGI(TAG, "Removed %d devices total, NVS cleared", removedCount);
    return CHIP_NO_ERROR;
}

static CHIP_ERROR BridgeCommandHandler(int argc, char ** argv)
{
    if (argc == 0)
    {
        return BridgeHelpHandler(argc, argv);
    }
    return sShellBridgeSubCommands.ExecCommand(argc, argv);
}

static void RegisterBridgeCommands()
{
    static const shell_command_t sBridgeSubCommands[] = {
        { &BridgeHelpHandler, "help", "Usage: bridge <subcommand>" },
        { &BridgeAddHandler, "add", "Add device: bridge add <type> [name] [location]" },
        { &BridgeRemoveHandler, "remove", "Remove device: bridge remove <endpoint>" },
        { &BridgeRemoveAllHandler, "remove_all", "Remove all bridged devices" },
        { &BridgeMaxHandler, "max", "Show max endpoint limits" },
        { &BridgeListHandler, "list", "List all bridged devices" },
        { &BridgeToggleHandler, "toggle", "Toggle OnOff: bridge toggle [endpoint] (devices with OnOff cluster)" },
    };

    static const shell_command_t sBridgeCommand = { &BridgeCommandHandler, "bridge",
                                                    "Bridge commands. Usage: bridge <subcommand>" };

    sShellBridgeSubCommands.RegisterCommands(sBridgeSubCommands, MATTER_ARRAY_SIZE(sBridgeSubCommands));
    Engine::Root().RegisterCommands(&sBridgeCommand, 1);
}
#endif // CONFIG_ENABLE_CHIP_SHELL

static void ResumeBridgedDevices()
{
    ReadBridgedEndpointIds();

    size_t resumedCount = 0;
    for (size_t i = 0; i < kMaxBridgedDevices; i++)
    {
        uint16_t epId = gBridgedEndpointIds[i];
        if (epId == kInvalidEndpointId)
        {
            continue;
        }

        BridgedDevicePersistentInfo persistInfo;
        if (ReadDevicePersistentInfo(epId, persistInfo) != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to read NVS info for endpoint %u, skipping", epId);
            gBridgedEndpointIds[i] = kInvalidEndpointId;
            continue;
        }

        if (persistInfo.deviceTypeIndex >= MATTER_ARRAY_SIZE(kDeviceTypeTable))
        {
            ESP_LOGW(TAG, "Invalid device type index %u for endpoint %u, skipping", persistInfo.deviceTypeIndex, epId);
            gBridgedEndpointIds[i] = kInvalidEndpointId;
            continue;
        }

        const BridgedDeviceTypeInfo * typeInfo = &kDeviceTypeTable[persistInfo.deviceTypeIndex];

        Device * dev = new (std::nothrow) Device(persistInfo.name, persistInfo.location);
        if (dev == nullptr)
        {
            ESP_LOGE(TAG, "Failed to allocate Device for endpoint %u", epId);
            continue;
        }

        DataVersion * dataVersions = new (std::nothrow) DataVersion[typeInfo->clusterCount];
        if (dataVersions == nullptr)
        {
            delete dev;
            ESP_LOGE(TAG, "Failed to allocate DataVersions for endpoint %u", epId);
            continue;
        }
        memset(dataVersions, 0, sizeof(DataVersion) * typeInfo->clusterCount);

        dev->SetReachable(true);
        dev->SetChangeCallback(&HandleDeviceStatusChanged);

        int result = ResumeDeviceEndpoint(
            dev, typeInfo->endpoint,
            Span<const EmberAfDeviceType>(typeInfo->deviceTypes, typeInfo->deviceTypesCount),
            Span<DataVersion>(dataVersions, typeInfo->clusterCount), static_cast<chip::EndpointId>(epId), 1);
        if (result < 0)
        {
            delete dev;
            delete[] dataVersions;
            ESP_LOGE(TAG, "Failed to resume endpoint %u", epId);
            gBridgedEndpointIds[i] = kInvalidEndpointId;
            continue;
        }

        gBridgedDataVersions[result] = dataVersions;
        gBridgedDeviceCount++;
        resumedCount++;

        // Advance gCurrentEndpointId past any resumed endpoint IDs to avoid collisions
        if (epId >= gCurrentEndpointId)
        {
            gCurrentEndpointId = epId + 1;
            if (gCurrentEndpointId < gFirstDynamicEndpointId)
            {
                gCurrentEndpointId = gFirstDynamicEndpointId;
            }
        }

        ESP_LOGI(TAG, "Resumed %s '%s' @ %s (endpoint %u)", typeInfo->name, persistInfo.name, persistInfo.location, epId);
    }

    if (resumedCount > 0)
    {
        ESP_LOGI(TAG, "Restored %u bridged device(s) from NVS", resumedCount);
    }
}

static void InitServer(intptr_t context)
{
    PrintOnboardingCodes(chip::RendezvousInformationFlags(CONFIG_RENDEZVOUS_MODE));

    Esp32AppServer::Init(); // Init ZCL Data Model and CHIP App Server AND Initialize device attestation config

    // Set starting endpoint id where dynamic endpoints will be assigned, which
    // will be the next consecutive endpoint id after the last fixed endpoint.
    gFirstDynamicEndpointId = static_cast<chip::EndpointId>(
        static_cast<int>(emberAfEndpointFromIndex(static_cast<uint16_t>(emberAfFixedEndpointCount() - 1))) + 1);
    gCurrentEndpointId = gFirstDynamicEndpointId;

    // Disable last fixed endpoint, which is used as a placeholder for all of the
    // supported clusters so that ZAP will generated the requisite code.
    emberAfEndpointEnableDisable(emberAfEndpointFromIndex(static_cast<uint16_t>(emberAfFixedEndpointCount() - 1)), false);

    // A bridge has root node device type on EP0 and aggregate node device type (bridge) at EP1
    emberAfSetDeviceTypeList(0, Span<const EmberAfDeviceType>(gRootDeviceTypes));
    emberAfSetDeviceTypeList(1, Span<const EmberAfDeviceType>(gAggregateNodeDeviceTypes));

    // Restore any bridged devices persisted in NVS from previous boot
    ResumeBridgedDevices();

    ESP_LOGI(TAG, "Bridge ready. Use 'bridge add <type> [name] [location]' to add devices. Max devices: %u", kMaxBridgedDevices);
}

void emberAfActionsClusterInitCallback(EndpointId endpoint)
{
    VerifyOrReturn(endpoint == 1,
                   ChipLogError(Zcl, "Actions cluster delegate is not implemented for endpoint with id %d.", endpoint));
    VerifyOrReturn(emberAfContainsServer(endpoint, app::Clusters::Actions::Id) == true,
                   ChipLogError(Zcl, "Endpoint %d does not support Actions cluster.", endpoint));
    VerifyOrReturn(!sActionsDelegateImpl && !sActionsServer);

    sActionsDelegateImpl = std::make_unique<app::Clusters::Actions::ActionsDelegateImpl>();
    sActionsServer       = std::make_unique<app::Clusters::Actions::ActionsServer>(endpoint, *sActionsDelegateImpl.get());

    sActionsServer->Init();
}

extern "C" void app_main()
{
    // Initialize the ESP NVS layer.
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_flash_init() failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_event_loop_create_default()  failed: %s", esp_err_to_name(err));
        return;
    }

#if CONFIG_ENABLE_CHIP_SHELL
#if CONFIG_OPENTHREAD_CLI
    chip::RegisterOpenThreadCliCommands();
#endif
    chip::LaunchShell();
    RegisterBridgeCommands();
#endif

    CHIP_ERROR chip_err = CHIP_NO_ERROR;

    // bridge will have own database named gDevices.
    // Clear database
    memset(gDevices, 0, sizeof(gDevices));
    memset(gBridgedDataVersions, 0, sizeof(gBridgedDataVersions));
    memset(gBridgedEndpointIds, 0xFF, sizeof(gBridgedEndpointIds));
    gBridgedDeviceCount = 0;

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
    if (DeviceLayer::Internal::ESP32Utils::InitWiFiStack() != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "Failed to initialize the Wi-Fi stack");
        return;
    }
#endif

    DeviceLayer::SetDeviceInfoProvider(&gExampleDeviceInfoProvider);

    CHIPDeviceManager & deviceMgr = CHIPDeviceManager::GetInstance();

    chip_err = deviceMgr.Init(&AppCallback);
    if (chip_err != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "device.Init() failed: %" CHIP_ERROR_FORMAT, chip_err.Format());
        return;
    }

#if CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER
    SetCommissionableDataProvider(&sFactoryDataProvider);
    SetDeviceAttestationCredentialsProvider(&sFactoryDataProvider);
#if CONFIG_ENABLE_ESP32_DEVICE_INSTANCE_INFO_PROVIDER
    SetDeviceInstanceInfoProvider(&sFactoryDataProvider);
#endif
#else
    SetDeviceAttestationCredentialsProvider(Examples::GetExampleDACProvider());
#endif // CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER

    chip::DeviceLayer::PlatformMgr().ScheduleWork(InitServer, reinterpret_cast<intptr_t>(nullptr));
}
