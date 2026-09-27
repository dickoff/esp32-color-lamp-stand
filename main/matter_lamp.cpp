#include "matter_lamp.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_providers.h>
#include <nvs_flash.h>

#include <app/persistence/AttributePersistence.h>
#include <app/persistence/DefaultAttributePersistenceProvider.h>
#include <app/persistence/String.h>
#include <app/server/CommissioningWindowManager.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ESP32/ESP32Config.h>
#include <platform/KvsPersistentStorageDelegate.h>
#include <platform/internal/GenericDeviceInstanceInfoProvider.h>

#include "lamp.h"

using namespace esp_matter;
using namespace chip::app::Clusters;
using chip::DeviceLayer::ChipDeviceEvent;
using chip::DeviceLayer::ConnectivityManager;

static const char *LOG_TAG = "matter-lamp";

// Reopen pairing whenever an unpaired lamp's commissioning window times out.
static constexpr uint16_t kCommissioningWindowSeconds = 15 * 60;

// Stock indication colors reported on the light while stock mode is on, so
// the Home app tile shows what the stand is displaying.
static constexpr uint8_t kStockUpHue = 85;  // 120 degrees
static constexpr uint8_t kStockDownHue = 0;

// Shown by controllers as the accessory's manufacturer and model.
static constexpr char kVendorName[] = "Color Lamp Stand Project";
static constexpr char kProductName[] = "Color Lamp Stand";
// Basic Information NodeLabel until a controller sets its own.
static constexpr char kDefaultNodeLabel[] = "Color Lamp Stand";

// Controllers identify the lamp for this long when they don't stop it sooner.
static constexpr uint32_t kIdentifyMaxMs = 5 * 60 * 1000;
static constexpr uint32_t kIdentifyEffectMs = 2000;

// Replaces the SDK's "TEST_VENDOR"/"TEST_PRODUCT" names; every other value
// (serial number, hardware version, and so on) comes from the generic provider.
class LampDeviceInstanceInfoProvider
    : public chip::DeviceLayer::Internal::GenericDeviceInstanceInfoProvider<
          chip::DeviceLayer::Internal::ESP32Config> {
public:
    LampDeviceInstanceInfoProvider()
        : GenericDeviceInstanceInfoProvider(
              chip::DeviceLayer::ConfigurationManagerImpl::GetDefaultInstance())
    {
    }

    CHIP_ERROR GetVendorName(char *buf, size_t bufSize) override
    {
        return CopyName(kVendorName, buf, bufSize);
    }

    CHIP_ERROR GetProductName(char *buf, size_t bufSize) override
    {
        return CopyName(kProductName, buf, bufSize);
    }

private:
    static CHIP_ERROR CopyName(const char *name, char *buf, size_t bufSize)
    {
        if (strlcpy(buf, name, bufSize) >= bufSize) {
            return CHIP_ERROR_BUFFER_TOO_SMALL;
        }
        return CHIP_NO_ERROR;
    }
};

static LampDeviceInstanceInfoProvider s_device_instance_info_provider;

static uint16_t s_light_endpoint_id;
static uint16_t s_stock_endpoint_id;
static std::atomic<bool> s_sync_scheduled{false};

static uint8_t current_u8(uint32_t cluster_id, uint32_t attribute_id)
{
    esp_matter_attr_val_t val = {};
    attribute::get_val(s_light_endpoint_id, cluster_id, attribute_id, &val);
    return val.val.u8;
}

static uint16_t current_u16(uint32_t cluster_id, uint32_t attribute_id)
{
    esp_matter_attr_val_t val = {};
    attribute::get_val(s_light_endpoint_id, cluster_id, attribute_id, &val);
    return val.val.u16;
}

// Converts a CIE 1931 chromaticity to the lamp's hue and saturation. The
// stand has no calibrated primaries, so sRGB is a reasonable stand-in.
static void xy_to_hue_saturation(uint16_t x_value, uint16_t y_value, uint8_t *hue,
                                 uint8_t *saturation)
{
    float x = x_value / 65536.0f;
    float y = std::max(y_value / 65536.0f, 0.0001f);
    float X = x / y;
    float Z = (1.0f - x - y) / y;
    float r = std::max(0.0f, 3.2406f * X - 1.5372f - 0.4986f * Z);
    float g = std::max(0.0f, -0.9689f * X + 1.8758f + 0.0415f * Z);
    float b = std::max(0.0f, 0.0557f * X - 0.2040f + 1.0570f * Z);

    float maximum = std::max({r, g, b});
    float minimum = std::min({r, g, b});
    if (maximum <= 0.0f) {
        *hue = 0;
        *saturation = 0;
        return;
    }
    float delta = maximum - minimum;
    float degrees = 0.0f;
    if (delta > 0.0f) {
        if (maximum == r) {
            degrees = 60.0f * std::fmod((g - b) / delta, 6.0f);
        } else if (maximum == g) {
            degrees = 60.0f * ((b - r) / delta + 2.0f);
        } else {
            degrees = 60.0f * ((r - g) / delta + 4.0f);
        }
    }
    if (degrees < 0.0f) {
        degrees += 360.0f;
    }
    *hue = (uint8_t)std::lround(degrees * LAMP_HUE_MAX / 360.0f);
    *saturation = (uint8_t)std::lround(delta / maximum * LAMP_SATURATION_MAX);
}

static void apply_color_mode(uint8_t color_mode)
{
    switch ((ColorControl::ColorMode)color_mode) {
        case ColorControl::ColorMode::kCurrentHueAndCurrentSaturation:
            lamp_set_hue_saturation(
                current_u8(ColorControl::Id, ColorControl::Attributes::CurrentHue::Id),
                current_u8(ColorControl::Id, ColorControl::Attributes::CurrentSaturation::Id));
            break;
        case ColorControl::ColorMode::kColorTemperature:
            lamp_set_color_temperature(
                current_u16(ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id));
            break;
        case ColorControl::ColorMode::kCurrentXAndCurrentY: {
            uint8_t hue;
            uint8_t saturation;
            xy_to_hue_saturation(
                current_u16(ColorControl::Id, ColorControl::Attributes::CurrentX::Id),
                current_u16(ColorControl::Id, ColorControl::Attributes::CurrentY::Id), &hue,
                &saturation);
            lamp_set_hue_saturation(hue, saturation);
            break;
        }
        default:
            ESP_LOGW(LOG_TAG, "Ignoring unsupported color mode %u", color_mode);
            break;
    }
}

static void apply_light_attribute(uint32_t cluster_id, uint32_t attribute_id,
                                  const esp_matter_attr_val_t *val)
{
    using namespace ColorControl::Attributes;

    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        lamp_set_on(val->val.b);
    } else if (cluster_id == LevelControl::Id
               && attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
        // CurrentLevel is nullable; null is 0xFF and leaves the level alone.
        if (val->val.u8 <= LAMP_LEVEL_MAX) {
            lamp_set_level(val->val.u8);
        }
    } else if (cluster_id == ColorControl::Id) {
        if (attribute_id == ColorMode::Id) {
            apply_color_mode(val->val.u8);
        } else if (attribute_id == CurrentHue::Id) {
            lamp_set_hue_saturation(val->val.u8,
                                    current_u8(ColorControl::Id, CurrentSaturation::Id));
        } else if (attribute_id == CurrentSaturation::Id) {
            lamp_set_hue_saturation(current_u8(ColorControl::Id, CurrentHue::Id), val->val.u8);
        } else if (attribute_id == ColorTemperatureMireds::Id) {
            lamp_set_color_temperature(val->val.u16);
        } else if (attribute_id == CurrentX::Id || attribute_id == CurrentY::Id) {
            uint16_t x = attribute_id == CurrentX::Id ? val->val.u16
                                                      : current_u16(ColorControl::Id, CurrentX::Id);
            uint16_t y = attribute_id == CurrentY::Id ? val->val.u16
                                                      : current_u16(ColorControl::Id, CurrentY::Id);
            uint8_t hue;
            uint8_t saturation;
            xy_to_hue_saturation(x, y, &hue, &saturation);
            lamp_set_hue_saturation(hue, saturation);
        }
    }
}

// Controllers change the lamp through here. Values the lamp itself reports use
// attribute::report(), which does not call back, so the two directions do not
// feed each other.
static esp_err_t attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                     uint32_t cluster_id, uint32_t attribute_id,
                                     esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != attribute::PRE_UPDATE) {
        return ESP_OK;
    }
    if (endpoint_id == s_light_endpoint_id) {
        apply_light_attribute(cluster_id, attribute_id, val);
    } else if (endpoint_id == s_stock_endpoint_id && cluster_id == OnOff::Id
               && attribute_id == OnOff::Attributes::OnOff::Id) {
        lamp_set_stock_mode(val->val.b);
    }
    return ESP_OK;
}

static esp_err_t identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                   uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(LOG_TAG, "Identify on endpoint %u, type %u, effect %u", endpoint_id, type, effect_id);
    switch (type) {
        case identification::START:
            lamp_identify(kIdentifyMaxMs);
            break;
        case identification::STOP:
            lamp_identify(0);
            break;
        case identification::EFFECT:
            if (effect_id == (uint8_t)Identify::EffectIdentifierEnum::kStopEffect
                || effect_id == (uint8_t)Identify::EffectIdentifierEnum::kFinishEffect) {
                lamp_identify(0);
            } else {
                lamp_identify(kIdentifyEffectMs);
            }
            break;
    }
    return ESP_OK;
}

static void report(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                   esp_matter_attr_val_t val)
{
    attribute::report(endpoint_id, cluster_id, attribute_id, &val);
}

// Runs on the Matter task with the stack lock held.
static void sync_from_lamp(intptr_t unused)
{
    using namespace ColorControl::Attributes;

    s_sync_scheduled = false;
    lamp_state_t state = lamp_get_state();

    report(s_light_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(state.on));
    report(s_light_endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id,
           esp_matter_nullable_uint8(state.level));

    bool temperature = !state.stock_mode && state.color_mode == LAMP_COLOR_MODE_TEMPERATURE;
    uint8_t color_mode = (uint8_t)(temperature
                                       ? ColorControl::ColorMode::kColorTemperature
                                       : ColorControl::ColorMode::kCurrentHueAndCurrentSaturation);
    report(s_light_endpoint_id, ColorControl::Id, ColorMode::Id, esp_matter_enum8(color_mode));
    report(s_light_endpoint_id, ColorControl::Id, EnhancedColorMode::Id,
           esp_matter_enum8(color_mode));
    if (temperature) {
        report(s_light_endpoint_id, ColorControl::Id, ColorTemperatureMireds::Id,
               esp_matter_uint16(state.color_temperature_mireds));
    } else {
        uint8_t hue = state.hue;
        uint8_t saturation = state.saturation;
        if (state.stock_mode) {
            hue = state.stock_indication == LAMP_STOCK_UP ? kStockUpHue : kStockDownHue;
            saturation = state.stock_indication == LAMP_STOCK_UNKNOWN ? 0 : LAMP_SATURATION_MAX;
        }
        report(s_light_endpoint_id, ColorControl::Id, CurrentHue::Id, esp_matter_uint8(hue));
        report(s_light_endpoint_id, ColorControl::Id, CurrentSaturation::Id,
               esp_matter_uint8(saturation));
    }

    report(s_stock_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id,
           esp_matter_bool(state.stock_mode));
}

static void lamp_changed(const lamp_state_t *state, void *context)
{
    // Coalesce bursts (for example, a fade's worth of level updates) into one
    // report pass on the Matter task.
    if (!s_sync_scheduled.exchange(true)
        && chip::DeviceLayer::PlatformMgr().ScheduleWork(sync_from_lamp, 0) != CHIP_NO_ERROR) {
        s_sync_scheduled = false;
        ESP_LOGE(LOG_TAG, "Unable to schedule Matter attribute sync");
    }
}

static void open_commissioning_window_if_unpaired(void)
{
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() != 0) {
        return;
    }
    chip::CommissioningWindowManager &manager =
        chip::Server::GetInstance().GetCommissioningWindowManager();
    if (manager.IsCommissioningWindowOpen()) {
        return;
    }
    CHIP_ERROR err = manager.OpenBasicCommissioningWindow(
        chip::System::Clock::Seconds16(kCommissioningWindowSeconds),
        chip::CommissioningWindowAdvertisement::kDnssdOnly);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(LOG_TAG, "Unable to open commissioning window: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

static void matter_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
        case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
            ESP_LOGI(LOG_TAG, "Commissioning complete");
            break;
        case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
            ESP_LOGW(LOG_TAG, "Commissioning failed: fail-safe timer expired");
            break;
        case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
            ESP_LOGI(LOG_TAG, "Commissioning window opened; add the lamp in the Home app with:");
            PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kOnNetwork));
            break;
        case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
            ESP_LOGI(LOG_TAG, "Commissioning window closed");
            open_commissioning_window_if_unpaired();
            break;
        case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
            ESP_LOGI(LOG_TAG, "Fabric removed");
            open_commissioning_window_if_unpaired();
            break;
        case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
            ESP_LOGI(LOG_TAG, "Fabric committed");
            break;
        default:
            break;
    }
}

static endpoint_t *create_light_endpoint(node_t *node, const lamp_state_t *state)
{
    endpoint::extended_color_light::config_t config;
    config.on_off.on_off = state->on;
    config.on_off_lighting.start_up_on_off = nullptr;
    config.level_control.current_level = state->level;
    config.level_control.on_level = nullptr;
    config.level_control_lighting.start_up_current_level = nullptr;
    config.color_control_color_temperature.color_temperature_mireds =
        state->color_temperature_mireds;
    config.color_control_color_temperature.color_temp_physical_min_mireds =
        LAMP_COLOR_TEMPERATURE_MIN_MIREDS;
    config.color_control_color_temperature.color_temp_physical_max_mireds =
        LAMP_COLOR_TEMPERATURE_MAX_MIREDS;
    config.color_control_color_temperature.couple_color_temp_to_level_min_mireds =
        LAMP_COLOR_TEMPERATURE_MIN_MIREDS;
    config.color_control_color_temperature.start_up_color_temperature_mireds = nullptr;

    endpoint_t *endpoint =
        endpoint::extended_color_light::create(node, &config, ENDPOINT_FLAG_NONE, nullptr);
    if (endpoint == nullptr) {
        return nullptr;
    }

    // Extended color lights require XY and color temperature; add hue and
    // saturation too, which is how the lamp represents color internally.
    cluster_t *color_control = cluster::get(endpoint, ColorControl::Id);
    cluster::color_control::feature::hue_saturation::config_t hue_saturation;
    hue_saturation.current_hue = state->hue;
    hue_saturation.current_saturation = state->saturation;
    if (cluster::color_control::feature::hue_saturation::add(color_control, &hue_saturation)
        != ESP_OK) {
        return nullptr;
    }
    return endpoint;
}

// The Basic Information cluster loads NodeLabel from attribute storage once
// at startup, so seed the default there before Matter starts. A label a
// controller has written, even an empty one, is left alone.
static void seed_default_node_label(void)
{
    if (nvs_flash_init_partition(CONFIG_CHIP_KVS_NAMESPACE_PARTITION_LABEL) != ESP_OK) {
        return;
    }
    chip::KvsPersistentStorageDelegate storage;
    chip::app::DefaultAttributePersistenceProvider provider;
    if (storage.Init(&chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr()) != CHIP_NO_ERROR
        || provider.Init(&storage) != CHIP_NO_ERROR) {
        return;
    }

    chip::app::AttributePersistence persistence(provider);
    chip::app::ConcreteAttributePath path(chip::kRootEndpointId, BasicInformation::Id,
                                          BasicInformation::Attributes::NodeLabel::Id);
    chip::app::Storage::String<32> label;
    if (persistence.LoadString(path, label)) {
        ESP_LOGI(LOG_TAG, "Node label: \"%s\"", label.c_str());
        return;
    }
    label.SetContent(chip::CharSpan::fromCharString(kDefaultNodeLabel));
    CHIP_ERROR err = persistence.StoreString(path, label);
    if (err == CHIP_NO_ERROR) {
        ESP_LOGI(LOG_TAG, "Set default node label \"%s\"", kDefaultNodeLabel);
    } else {
        ESP_LOGW(LOG_TAG, "Unable to set default node label: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

esp_err_t matter_lamp_start(void)
{
    lamp_state_t state = lamp_get_state();

    node::config_t node_config;
    node_t *node = node::create(&node_config, attribute_update_cb, identification_cb);
    if (node == nullptr) {
        ESP_LOGE(LOG_TAG, "Unable to create Matter node");
        return ESP_FAIL;
    }

    endpoint_t *light = create_light_endpoint(node, &state);
    if (light == nullptr) {
        ESP_LOGE(LOG_TAG, "Unable to create light endpoint");
        return ESP_FAIL;
    }
    s_light_endpoint_id = endpoint::get_id(light);

    endpoint::on_off_plug_in_unit::config_t stock_config;
    stock_config.on_off.on_off = state.stock_mode;
    stock_config.on_off_lighting.start_up_on_off = nullptr;
    endpoint_t *stock =
        endpoint::on_off_plug_in_unit::create(node, &stock_config, ENDPOINT_FLAG_NONE, nullptr);
    if (stock == nullptr) {
        ESP_LOGE(LOG_TAG, "Unable to create stock mode endpoint");
        return ESP_FAIL;
    }
    s_stock_endpoint_id = endpoint::get_id(stock);

    seed_default_node_label();
    esp_matter::set_custom_device_instance_info_provider(&s_device_instance_info_provider);
    esp_err_t err = esp_matter::start(matter_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(LOG_TAG, "Unable to start Matter: %s", esp_err_to_name(err));
        return err;
    }

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    // The Wi-Fi manager provisions and reconnects the station; Matter only
    // observes it.
    CHIP_ERROR chip_err = chip::DeviceLayer::ConnectivityMgr().SetWiFiStationMode(
        ConnectivityManager::kWiFiStationMode_ApplicationControlled);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    if (chip_err != CHIP_NO_ERROR) {
        ESP_LOGE(LOG_TAG, "Unable to hand Wi-Fi control to the application: %" CHIP_ERROR_FORMAT,
                 chip_err.Format());
        return ESP_FAIL;
    }

    // The lamp's saved state is authoritative; publish it over whatever Matter
    // restored from its own storage.
    lamp_add_change_callback(lamp_changed, nullptr);
    lamp_changed(nullptr, nullptr);

    ESP_LOGI(LOG_TAG, "Matter started: light endpoint %u, stock mode endpoint %u",
             s_light_endpoint_id, s_stock_endpoint_id);
    return ESP_OK;
}
