// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/ui/input_reader/input_interpreter.h"

#include <hid/acer12.h>
#include <hid/ambient-light.h>
#include <hid/boot.h>
#include <hid/egalax.h>
#include <hid/eyoyo.h>
#include <hid/ft3x27.h>
#include <hid/hid.h>
#include <hid/paradise.h>
#include <hid/samsung.h>
#include <hid/usages.h>

#include <sys/types.h>
#include <sys/uio.h>
#include <zircon/device/device.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <trace/event.h>

#include <fuchsia/ui/input/cpp/fidl.h>
#include "garnet/bin/ui/input_reader/fdio_hid_decoder.h"
#include "lib/fidl/cpp/clone.h"
#include "lib/fxl/arraysize.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/time/time_point.h"
#include "lib/ui/input/cpp/formatting.h"

namespace {

int64_t InputEventTimestampNow() {
  return fxl::TimePoint::Now().ToEpochDelta().ToNanoseconds();
}

fuchsia::ui::input::InputReport CloneReport(
    const fuchsia::ui::input::InputReportPtr& report) {
  fuchsia::ui::input::InputReport result;
  fidl::Clone(*report, &result);
  return result;
}

// TODO(SCN-473): Extract sensor IDs from HID.
const size_t kParadiseAccLid = 0;
const size_t kParadiseAccBase = 1;
const size_t kAmbientLight = 2;
}  // namespace

namespace mozart {

InputInterpreter::InputInterpreter(
    std::unique_ptr<HidDecoder> hid_decoder,
    fuchsia::ui::input::InputDeviceRegistry* registry)
    : registry_(registry), hid_decoder_(std::move(hid_decoder)) {
  memset(acer12_touch_reports_, 0, 2 * sizeof(acer12_touch_t));
  FXL_DCHECK(hid_decoder_);
}

InputInterpreter::~InputInterpreter() {}

bool InputInterpreter::Initialize() {
  if (!hid_decoder_->Init())
    return false;

  auto protocol = hid_decoder_->protocol();

  if (protocol == HidDecoder::Protocol::Keyboard) {
    FXL_VLOG(2) << "Device " << name() << " has keyboard";
    has_keyboard_ = true;
    keyboard_descriptor_ = fuchsia::ui::input::KeyboardDescriptor::New();
    keyboard_descriptor_->keys.resize(HID_USAGE_KEY_RIGHT_GUI -
                                      HID_USAGE_KEY_A + 1);
    for (size_t index = HID_USAGE_KEY_A; index <= HID_USAGE_KEY_RIGHT_GUI;
         ++index) {
      keyboard_descriptor_->keys.at(index - HID_USAGE_KEY_A) = index;
    }

    keyboard_report_ = fuchsia::ui::input::InputReport::New();
    keyboard_report_->keyboard = fuchsia::ui::input::KeyboardReport::New();
  } else if (protocol == HidDecoder::Protocol::Buttons) {
    FXL_VLOG(2) << "Device " << name() << " has buttons";
    has_buttons_ = true;
    buttons_descriptor_ = fuchsia::ui::input::ButtonsDescriptor::New();
    buttons_descriptor_->buttons |= fuchsia::ui::input::kVolumeUp;
    buttons_descriptor_->buttons |= fuchsia::ui::input::kVolumeDown;
    buttons_descriptor_->buttons |= fuchsia::ui::input::kMicMute;
    buttons_report_ = fuchsia::ui::input::InputReport::New();
    buttons_report_->buttons = fuchsia::ui::input::ButtonsReport::New();
  } else if (protocol == HidDecoder::Protocol::Mouse ||
             protocol == HidDecoder::Protocol::Gamepad) {
    FXL_VLOG(2) << "Device " << name() << " has mouse";
    has_mouse_ = true;
    mouse_device_type_ = (protocol == HidDecoder::Protocol::Mouse)
                             ? MouseDeviceType::BOOT
                             : MouseDeviceType::GAMEPAD;

    mouse_descriptor_ = fuchsia::ui::input::MouseDescriptor::New();
    mouse_descriptor_->rel_x.range.min = INT32_MIN;
    mouse_descriptor_->rel_x.range.max = INT32_MAX;
    mouse_descriptor_->rel_x.resolution = 1;

    mouse_descriptor_->rel_y.range.min = INT32_MIN;
    mouse_descriptor_->rel_y.range.max = INT32_MAX;
    mouse_descriptor_->rel_y.resolution = 1;

    mouse_descriptor_->buttons |= fuchsia::ui::input::kMouseButtonPrimary;
    mouse_descriptor_->buttons |= fuchsia::ui::input::kMouseButtonSecondary;
    mouse_descriptor_->buttons |= fuchsia::ui::input::kMouseButtonTertiary;

    mouse_report_ = fuchsia::ui::input::InputReport::New();
    mouse_report_->mouse = fuchsia::ui::input::MouseReport::New();
  } else if (protocol == HidDecoder::Protocol::Touch) {
    FXL_VLOG(2) << "Device " << name() << " has hid touch";

    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();
    Touchscreen::Descriptor touch_desc;
    hid_decoder_->SetDescriptor(&touch_desc);
    touchscreen_descriptor_->x.range.min = touch_desc.x_min;
    touchscreen_descriptor_->x.range.max = touch_desc.x_max;
    touchscreen_descriptor_->x.resolution = touch_desc.x_resolution;

    touchscreen_descriptor_->y.range.min = touch_desc.y_min;
    touchscreen_descriptor_->y.range.max = touch_desc.y_max;
    touchscreen_descriptor_->y.resolution = touch_desc.x_resolution;

    touchscreen_descriptor_->max_finger_id = touch_desc.max_finger_id;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::HID;
  } else if (protocol == HidDecoder::Protocol::Touchpad) {
    FXL_VLOG(2) << "Device " << name() << " has hid touchpad";

    has_mouse_ = true;
    mouse_descriptor_ = fuchsia::ui::input::MouseDescriptor::New();
    mouse_device_type_ = MouseDeviceType::TOUCH;

    mouse_descriptor_->rel_x.range.min = INT32_MIN;
    mouse_descriptor_->rel_x.range.max = INT32_MAX;
    mouse_descriptor_->rel_x.resolution = 1;

    mouse_descriptor_->rel_y.range.min = INT32_MIN;
    mouse_descriptor_->rel_y.range.max = INT32_MAX;
    mouse_descriptor_->rel_y.resolution = 1;

    mouse_descriptor_->buttons |= fuchsia::ui::input::kMouseButtonPrimary;

    mouse_report_ = fuchsia::ui::input::InputReport::New();
    mouse_report_->mouse = fuchsia::ui::input::MouseReport::New();
  } else if (protocol == HidDecoder::Protocol::Acer12Touch) {
    FXL_VLOG(2) << "Device " << name() << " has stylus";
    has_stylus_ = true;
    stylus_descriptor_ = fuchsia::ui::input::StylusDescriptor::New();

    stylus_descriptor_->x.range.min = 0;
    stylus_descriptor_->x.range.max = ACER12_STYLUS_X_MAX;
    stylus_descriptor_->x.resolution = 1;

    stylus_descriptor_->y.range.min = 0;
    stylus_descriptor_->y.range.max = ACER12_STYLUS_Y_MAX;
    stylus_descriptor_->y.resolution = 1;

    stylus_descriptor_->is_invertible = false;

    stylus_descriptor_->buttons |= fuchsia::ui::input::kStylusBarrel;

    stylus_report_ = fuchsia::ui::input::InputReport::New();
    stylus_report_->stylus = fuchsia::ui::input::StylusReport::New();

    FXL_VLOG(2) << "Device " << name() << " has touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();

    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = ACER12_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;

    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = ACER12_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    // TODO(jpoichet) do not hardcode this
    touchscreen_descriptor_->max_finger_id = 255;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::ACER12;
  } else if (protocol == HidDecoder::Protocol::SamsungTouch) {
    FXL_VLOG(2) << "Device " << name() << " has touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();

    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = SAMSUNG_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;

    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = SAMSUNG_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    // TODO(jpoichet) do not hardcode this
    touchscreen_descriptor_->max_finger_id = 255;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::SAMSUNG;
  } else if (protocol == HidDecoder::Protocol::ParadiseV1Touch) {
    // TODO(cpu): Add support for stylus.
    FXL_VLOG(2) << "Device " << name() << " has touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();

    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = PARADISE_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;

    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = PARADISE_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    // TODO(cpu) do not hardcode |max_finger_id|.
    touchscreen_descriptor_->max_finger_id = 255;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::PARADISEv1;
  } else if (protocol == HidDecoder::Protocol::ParadiseV2Touch) {
    FXL_VLOG(2) << "Device " << name() << " has stylus";
    has_stylus_ = true;
    stylus_descriptor_ = fuchsia::ui::input::StylusDescriptor::New();

    stylus_descriptor_->x.range.min = 0;
    stylus_descriptor_->x.range.max = PARADISE_STYLUS_X_MAX;
    stylus_descriptor_->x.resolution = 1;

    stylus_descriptor_->y.range.min = 0;
    stylus_descriptor_->y.range.max = PARADISE_STYLUS_Y_MAX;
    stylus_descriptor_->y.resolution = 1;

    stylus_descriptor_->is_invertible = false;

    stylus_descriptor_->buttons |= fuchsia::ui::input::kStylusBarrel;

    stylus_report_ = fuchsia::ui::input::InputReport::New();
    stylus_report_->stylus = fuchsia::ui::input::StylusReport::New();

    FXL_VLOG(2) << "Device " << name() << " has touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();

    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = PARADISE_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;

    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = PARADISE_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    // TODO(cpu) do not hardcode |max_finger_id|.
    touchscreen_descriptor_->max_finger_id = 255;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::PARADISEv2;
  } else if (protocol == HidDecoder::Protocol::ParadiseV3Touch) {
    FXL_VLOG(2) << "Device " << name() << " has stylus";
    has_stylus_ = true;
    stylus_descriptor_ = fuchsia::ui::input::StylusDescriptor::New();

    stylus_descriptor_->x.range.min = 0;
    stylus_descriptor_->x.range.max = PARADISE_STYLUS_X_MAX;
    stylus_descriptor_->x.resolution = 1;

    stylus_descriptor_->y.range.min = 0;
    stylus_descriptor_->y.range.max = PARADISE_STYLUS_Y_MAX;
    stylus_descriptor_->y.resolution = 1;

    stylus_descriptor_->is_invertible = false;

    stylus_descriptor_->buttons |= fuchsia::ui::input::kStylusBarrel;

    stylus_report_ = fuchsia::ui::input::InputReport::New();
    stylus_report_->stylus = fuchsia::ui::input::StylusReport::New();

    FXL_VLOG(2) << "Device " << name() << " has touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();

    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = PARADISE_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;

    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = PARADISE_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    // TODO(cpu) do not hardcode |max_finger_id|.
    touchscreen_descriptor_->max_finger_id = 255;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::PARADISEv3;
  } else if (protocol == HidDecoder::Protocol::ParadiseV1TouchPad) {
    FXL_VLOG(2) << "Device " << name() << " has touchpad";
    has_mouse_ = true;
    mouse_device_type_ = MouseDeviceType::PARADISEv1;

    mouse_descriptor_ = fuchsia::ui::input::MouseDescriptor::New();

    mouse_descriptor_->rel_x.range.min = INT32_MIN;
    mouse_descriptor_->rel_x.range.max = INT32_MAX;
    mouse_descriptor_->rel_x.resolution = 1;

    mouse_descriptor_->rel_y.range.min = INT32_MIN;
    mouse_descriptor_->rel_y.range.max = INT32_MAX;
    mouse_descriptor_->rel_y.resolution = 1;

    mouse_descriptor_->buttons |= fuchsia::ui::input::kMouseButtonPrimary;

    mouse_report_ = fuchsia::ui::input::InputReport::New();
    mouse_report_->mouse = fuchsia::ui::input::MouseReport::New();
  } else if (protocol == HidDecoder::Protocol::ParadiseV2TouchPad) {
    FXL_VLOG(2) << "Device " << name() << " has touchpad";
    has_mouse_ = true;
    mouse_device_type_ = MouseDeviceType::PARADISEv2;

    mouse_descriptor_ = fuchsia::ui::input::MouseDescriptor::New();

    mouse_descriptor_->rel_x.range.min = INT32_MIN;
    mouse_descriptor_->rel_x.range.max = INT32_MAX;
    mouse_descriptor_->rel_x.resolution = 1;

    mouse_descriptor_->rel_y.range.min = INT32_MIN;
    mouse_descriptor_->rel_y.range.max = INT32_MAX;
    mouse_descriptor_->rel_y.resolution = 1;

    mouse_descriptor_->buttons |= fuchsia::ui::input::kMouseButtonPrimary;

    mouse_report_ = fuchsia::ui::input::InputReport::New();
    mouse_report_->mouse = fuchsia::ui::input::MouseReport::New();
  } else if (protocol == HidDecoder::Protocol::EgalaxTouch) {
    FXL_VLOG(2) << "Device " << name() << " has touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();

    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = EGALAX_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;

    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = EGALAX_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    touchscreen_descriptor_->max_finger_id = 1;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::EGALAX;
  } else if (protocol == HidDecoder::Protocol::ParadiseSensor) {
    FXL_VLOG(2) << "Device " << name() << " has motion sensors";
    sensor_device_type_ = SensorDeviceType::PARADISE;
    has_sensors_ = true;

    fuchsia::ui::input::SensorDescriptorPtr acc_base =
        fuchsia::ui::input::SensorDescriptor::New();
    acc_base->type = fuchsia::ui::input::SensorType::ACCELEROMETER;
    acc_base->loc = fuchsia::ui::input::SensorLocation::BASE;
    sensor_descriptors_[kParadiseAccBase] = std::move(acc_base);

    fuchsia::ui::input::SensorDescriptorPtr acc_lid =
        fuchsia::ui::input::SensorDescriptor::New();
    acc_lid->type = fuchsia::ui::input::SensorType::ACCELEROMETER;
    acc_lid->loc = fuchsia::ui::input::SensorLocation::LID;
    sensor_descriptors_[kParadiseAccLid] = std::move(acc_lid);

    sensor_report_ = fuchsia::ui::input::InputReport::New();
    sensor_report_->sensor = fuchsia::ui::input::SensorReport::New();
  } else if (protocol == HidDecoder::Protocol::EyoyoTouch) {
    FXL_VLOG(2) << "Device " << name() << " has touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();

    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = EYOYO_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;

    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = EYOYO_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    // TODO(jpoichet) do not hardcode this
    touchscreen_descriptor_->max_finger_id = 255;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::EYOYO;
  } else if (protocol == HidDecoder::Protocol::LightSensor) {
    FXL_VLOG(2) << "Device " << name() << " has an ambient light sensor";
    sensor_device_type_ = SensorDeviceType::AMBIENT_LIGHT;
    has_sensors_ = true;

    fuchsia::ui::input::SensorDescriptorPtr desc =
        fuchsia::ui::input::SensorDescriptor::New();
    desc->type = fuchsia::ui::input::SensorType::LIGHTMETER;
    desc->loc = fuchsia::ui::input::SensorLocation::UNKNOWN;
    sensor_descriptors_[kAmbientLight] = std::move(desc);

    sensor_report_ = fuchsia::ui::input::InputReport::New();
    sensor_report_->sensor = fuchsia::ui::input::SensorReport::New();
  } else if (protocol == HidDecoder::Protocol::EyoyoTouch) {
    FXL_VLOG(2) << "Device " << name() << " has touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();

    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = EYOYO_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;

    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = EYOYO_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    // TODO(jpoichet) do not hardcode this
    touchscreen_descriptor_->max_finger_id = 255;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::EYOYO;
  } else if (protocol == HidDecoder::Protocol::Ft3x27Touch) {
    FXL_VLOG(2) << "Device " << name() << " has a touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();
    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = FT3X27_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;
    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = FT3X27_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    // TODO(SCN-867) Use HID parsing for all touch devices
    // will remove the need for this hardcoding
    touchscreen_descriptor_->max_finger_id = 255;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::FT3X27;
  } else {
    FXL_VLOG(2) << "Device " << name() << " has unsupported HID device";
    return false;
  }

  event_ = hid_decoder_->GetEvent();
  if (!event_)
    return false;

  NotifyRegistry();
  return true;
}

void InputInterpreter::NotifyRegistry() {
  if (has_sensors_) {
    FXL_DCHECK(kMaxSensorCount == sensor_descriptors_.size());
    FXL_DCHECK(kMaxSensorCount == sensor_devices_.size());
    for (size_t i = 0; i < kMaxSensorCount; ++i) {
      if (sensor_descriptors_[i]) {
        fuchsia::ui::input::DeviceDescriptor descriptor;
        zx_status_t status =
            fidl::Clone(sensor_descriptors_[i], &descriptor.sensor);
        FXL_DCHECK(status == ZX_OK)
            << "Sensor descriptor: clone failed (status=" << status << ")";
        registry_->RegisterDevice(std::move(descriptor),
                                  sensor_devices_[i].NewRequest());
      }
    }
    // Sensor devices can't be anything else, so don't bother with other types.
    return;
  }

  fuchsia::ui::input::DeviceDescriptor descriptor;
  if (has_keyboard_) {
    fidl::Clone(keyboard_descriptor_, &descriptor.keyboard);
  }
  if (has_mouse_) {
    fidl::Clone(mouse_descriptor_, &descriptor.mouse);
  }
  if (has_stylus_) {
    fidl::Clone(stylus_descriptor_, &descriptor.stylus);
  }
  if (has_touchscreen_) {
    fidl::Clone(touchscreen_descriptor_, &descriptor.touchscreen);
  }
  registry_->RegisterDevice(std::move(descriptor), input_device_.NewRequest());
}

bool InputInterpreter::Read(bool discard) {
  // If positive |rc| is the number of bytes read. If negative the error
  // while reading.
  int rc = 1;
  auto report = hid_decoder_->use_legacy_mode() ? hid_decoder_->Read(&rc)
                                                : std::vector<uint8_t>(1, 1);

  // TODO(cpu): remove legacy mode, so no raw HidDecoder::Read(int*) is
  // issued from this code.

  if (rc < 1) {
    FXL_LOG(ERROR) << "Failed to read from input: " << rc << " for " << name();
    // TODO(cpu) check whether the device was actually closed or not.
    return false;
  }

  TRACE_DURATION("input", "Read");
  if (has_keyboard_) {
    ParseKeyboardReport(report.data(), rc);
    if (!discard) {
      input_device_->DispatchReport(CloneReport(keyboard_report_));
    }
  }

  if (has_buttons_) {
    if (!ParseButtonsReport())
      return false;

    if (!discard) {
      input_device_->DispatchReport(CloneReport(buttons_report_));
    }
  }

  switch (mouse_device_type_) {
    case MouseDeviceType::BOOT:
      ParseMouseReport(report.data(), rc);
      if (!discard) {
        input_device_->DispatchReport(CloneReport(mouse_report_));
      }
      break;
    case MouseDeviceType::TOUCH:
      Touchscreen::Report touch_report;
      if (!hid_decoder_->Read(&touch_report)) {
        FXL_LOG(ERROR) << " failed reading from touchpad";
        return false;
      }

      if (ParseTouchpadReport(&touch_report)) {
        if (!discard) {
          input_device_->DispatchReport(CloneReport(mouse_report_));
        }
      }
      break;
    case MouseDeviceType::PARADISEv1:
      if (ParseParadiseTouchpadReport<paradise_touchpad_v1_t>(report.data(),
                                                              rc)) {
        if (!discard) {
          input_device_->DispatchReport(CloneReport(mouse_report_));
        }
      }
      break;
    case MouseDeviceType::PARADISEv2:
      if (ParseParadiseTouchpadReport<paradise_touchpad_v2_t>(report.data(),
                                                              rc)) {
        if (!discard) {
          input_device_->DispatchReport(CloneReport(mouse_report_));
        }
      }
      break;
    case MouseDeviceType::GAMEPAD:
      // TODO(cpu): remove this once we have a good way to test gamepad.
      HidDecoder::HidGamepadSimple gamepad;
      if (!hid_decoder_->Read(&gamepad)) {
        FXL_LOG(ERROR) << " failed reading from gamepad ";
        return false;
      }
      ParseGamepadMouseReport(&gamepad);
      if (!discard) {
        input_device_->DispatchReport(CloneReport(mouse_report_));
      }
      break;
    case MouseDeviceType::NONE:
      break;
  }

  switch (touch_device_type_) {
    case TouchDeviceType::HID:
      Touchscreen::Report touch_report;
      if (!hid_decoder_->Read(&touch_report)) {
        FXL_LOG(ERROR) << " failed reading from touchscreen ";
        return false;
      }

      if (ParseTouchscreenReport(&touch_report)) {
        if (!discard) {
          input_device_->DispatchReport(CloneReport(touchscreen_report_));
        }
      }
      break;
    case TouchDeviceType::ACER12:
      if (report[0] == ACER12_RPT_ID_STYLUS) {
        if (ParseAcer12StylusReport(report.data(), rc)) {
          if (!discard) {
            input_device_->DispatchReport(CloneReport(stylus_report_));
          }
        }
      } else if (report[0] == ACER12_RPT_ID_TOUCH) {
        if (ParseAcer12TouchscreenReport(report.data(), rc)) {
          if (!discard) {
            input_device_->DispatchReport(CloneReport(touchscreen_report_));
          }
        }
      }
      break;

    case TouchDeviceType::SAMSUNG:
      if (report[0] == SAMSUNG_RPT_ID_TOUCH) {
        if (ParseSamsungTouchscreenReport(report.data(), rc)) {
          if (!discard) {
            input_device_->DispatchReport(CloneReport(touchscreen_report_));
          }
        }
      }
      break;

    case TouchDeviceType::PARADISEv1:
      if (report[0] == PARADISE_RPT_ID_TOUCH) {
        if (ParseParadiseTouchscreenReport<paradise_touch_t>(report.data(),
                                                             rc)) {
          if (!discard) {
            input_device_->DispatchReport(CloneReport(touchscreen_report_));
          }
        }
      }
      break;
    case TouchDeviceType::PARADISEv2:
      if (report[0] == PARADISE_RPT_ID_TOUCH) {
        if (ParseParadiseTouchscreenReport<paradise_touch_v2_t>(report.data(),
                                                                rc)) {
          if (!discard) {
            input_device_->DispatchReport(CloneReport(touchscreen_report_));
          }
        }
      } else if (report[0] == PARADISE_RPT_ID_STYLUS) {
        if (ParseParadiseStylusReport(report.data(), rc)) {
          if (!discard) {
            input_device_->DispatchReport(CloneReport(stylus_report_));
          }
        }
      }
      break;
    case TouchDeviceType::PARADISEv3:
      if (report[0] == PARADISE_RPT_ID_TOUCH) {
        if (ParseParadiseTouchscreenReport<paradise_touch_t>(report.data(),
                                                             rc)) {
          if (!discard) {
            input_device_->DispatchReport(CloneReport(touchscreen_report_));
          }
        }
      } else if (report[0] == PARADISE_RPT_ID_STYLUS) {
        if (ParseParadiseStylusReport(report.data(), rc)) {
          if (!discard) {
            input_device_->DispatchReport(CloneReport(stylus_report_));
          }
        }
      }
      break;
    case TouchDeviceType::EGALAX:
      if (report[0] == EGALAX_RPT_ID_TOUCH) {
        if (ParseEGalaxTouchscreenReport(report.data(), rc)) {
          if (!discard) {
            input_device_->DispatchReport(CloneReport(touchscreen_report_));
          }
        }
      }
      break;

    case TouchDeviceType::EYOYO:
      if (report[0] == EYOYO_RPT_ID_TOUCH) {
        if (ParseEyoyoTouchscreenReport(report.data(), rc)) {
          if (!discard) {
            input_device_->DispatchReport(CloneReport(touchscreen_report_));
          }
        }
      }
      break;
    case TouchDeviceType::FT3X27:
      if (report[0] == FT3X27_RPT_ID_TOUCH) {
        if (ParseFt3x27TouchscreenReport(report.data(), rc)) {
          if (!discard) {
            input_device_->DispatchReport(CloneReport(touchscreen_report_));
          }
        }
      }
      break;

    default:
      break;
  }

  switch (sensor_device_type_) {
    case SensorDeviceType::PARADISE:
      if (ParseParadiseSensorReport(report.data(), rc)) {
        if (!discard) {
          FXL_DCHECK(sensor_idx_ < kMaxSensorCount);
          FXL_DCHECK(sensor_devices_[sensor_idx_]);
          sensor_devices_[sensor_idx_]->DispatchReport(
              CloneReport(sensor_report_));
        }
      }
      break;
    case SensorDeviceType::AMBIENT_LIGHT:
      if (ParseAmbientLightSensorReport()) {
        if (!discard) {
          FXL_DCHECK(sensor_idx_ < kMaxSensorCount);
          FXL_DCHECK(sensor_devices_[sensor_idx_]);
          sensor_devices_[sensor_idx_]->DispatchReport(
              CloneReport(sensor_report_));
        }
      }
      break;
    default:
      break;
  }

  return true;
}

void InputInterpreter::ParseKeyboardReport(uint8_t* report, size_t len) {
  hid_keys_t key_state;
  uint8_t keycode;
  hid_kbd_parse_report(report, &key_state);
  keyboard_report_->event_time = InputEventTimestampNow();

  auto& pressed_keys = keyboard_report_->keyboard->pressed_keys;
  pressed_keys.resize(0);
  hid_for_every_key(&key_state, keycode) { pressed_keys.push_back(keycode); }
  FXL_VLOG(2) << name() << " parsed: " << *keyboard_report_;
}

void InputInterpreter::ParseMouseReport(uint8_t* r, size_t len) {
  auto report = reinterpret_cast<hid_boot_mouse_report_t*>(r);
  mouse_report_->event_time = InputEventTimestampNow();

  mouse_report_->mouse->rel_x = report->rel_x;
  mouse_report_->mouse->rel_y = report->rel_y;
  mouse_report_->mouse->pressed_buttons = report->buttons;
  FXL_VLOG(2) << name() << " parsed: " << *mouse_report_;
}

void InputInterpreter::ParseGamepadMouseReport(
    // TODO(cpu): remove this once we have a better way to test gamepads.
    const HidDecoder::HidGamepadSimple* gamepad) {
  mouse_report_->event_time = InputEventTimestampNow();

  mouse_report_->mouse->rel_x = gamepad->left_x;
  mouse_report_->mouse->rel_y = gamepad->left_y;
  mouse_report_->mouse->pressed_buttons = gamepad->hat_switch;
}

// This logic converts the multi-finger report from the touchpad into
// a mouse report. It does this by only tracking the first finger that
// is placed down, and converting the absolution finger position into
// relative X and Y movements. All other fingers besides the tracking
// finger are ignored.
bool InputInterpreter::ParseTouchpadReport(Touchscreen::Report* report) {
  mouse_report_->event_time = InputEventTimestampNow();
  mouse_report_->mouse->rel_x = 0;
  mouse_report_->mouse->rel_y = 0;
  mouse_report_->mouse->pressed_buttons = 0;

  // If all fingers are lifted reset our tracking finger.
  if (report->contact_count == 0) {
    has_touch_ = false;
    tracking_finger_was_lifted_ = true;
    return true;
  }

  // If we don't have a tracking finger then set one.
  if (!has_touch_) {
    has_touch_ = true;
    tracking_finger_was_lifted_ = false;
    tracking_finger_id_ = report->contacts[0].id;

    mouse_abs_x_ = report->contacts[0].x;
    mouse_abs_y_ = report->contacts[0].y;
    return true;
  }

  // Find the finger we are tracking.
  Touchscreen::ContactReport* contact = nullptr;
  for (size_t i = 0; i < report->contact_count; i++) {
    if (report->contacts[i].id == tracking_finger_id_) {
      contact = &report->contacts[i];
      break;
    }
  }

  // If our tracking finger isn't pressed return early.
  if (contact == nullptr) {
    tracking_finger_was_lifted_ = true;
    return true;
  }

  // If our tracking finger was lifted then reset the abs values otherwise
  // the pointer will jump rapidly.
  if (tracking_finger_was_lifted_) {
    tracking_finger_was_lifted_ = false;
    mouse_abs_x_ = contact->x;
    mouse_abs_y_ = contact->y;
  }

  // The touch driver returns in units of 10^-5m, but the resolution expected
  // by |mouse_report_| is 10^-3.
  mouse_report_->mouse->rel_x = (contact->x - mouse_abs_x_) / 100;
  mouse_report_->mouse->rel_y = (contact->y - mouse_abs_y_) / 100;

  mouse_report_->mouse->pressed_buttons =
      report->button ? fuchsia::ui::input::kMouseButtonPrimary : 0;

  mouse_abs_x_ = report->contacts[0].x;
  mouse_abs_y_ = report->contacts[0].y;

  return true;
}

bool InputInterpreter::ParseTouchscreenReport(Touchscreen::Report* report) {
  touchscreen_report_->event_time = InputEventTimestampNow();
  touchscreen_report_->touchscreen->touches.resize(report->contact_count);

  for (size_t i = 0; i < report->contact_count; ++i) {
    fuchsia::ui::input::Touch touch;
    touch.finger_id = report->contacts[i].id;
    touch.x = report->contacts[i].x;
    touch.y = report->contacts[i].y;
    // TODO(SCN-1188): Add support for contact ellipse.
    touch.width = 5;
    touch.height = 5;
    touchscreen_report_->touchscreen->touches.at(i) = std::move(touch);
  }

  return true;
}

bool InputInterpreter::ParseAcer12StylusReport(uint8_t* r, size_t len) {
  if (len != sizeof(acer12_stylus_t)) {
    return false;
  }

  auto report = reinterpret_cast<acer12_stylus_t*>(r);
  stylus_report_->event_time = InputEventTimestampNow();

  stylus_report_->stylus->x = report->x;
  stylus_report_->stylus->y = report->y;
  stylus_report_->stylus->pressure = report->pressure;

  stylus_report_->stylus->is_in_contact =
      acer12_stylus_status_inrange(report->status) &&
      (acer12_stylus_status_tswitch(report->status) ||
       acer12_stylus_status_eraser(report->status));

  stylus_report_->stylus->in_range =
      acer12_stylus_status_inrange(report->status);

  if (acer12_stylus_status_invert(report->status) ||
      acer12_stylus_status_eraser(report->status)) {
    stylus_report_->stylus->is_inverted = true;
  }

  if (acer12_stylus_status_barrel(report->status)) {
    stylus_report_->stylus->pressed_buttons |=
        fuchsia::ui::input::kStylusBarrel;
  }
  FXL_VLOG(2) << name() << " parsed: " << *stylus_report_;

  return true;
}

bool InputInterpreter::ParseAcer12TouchscreenReport(uint8_t* r, size_t len) {
  if (len != sizeof(acer12_touch_t)) {
    return false;
  }

  // Acer12 touch reports come in pairs when there are more than 5 fingers
  // First report has the actual number of fingers stored in contact_count,
  // second report will have a contact_count of 0.
  auto report = reinterpret_cast<acer12_touch_t*>(r);
  if (report->contact_count > 0) {
    acer12_touch_reports_[0] = *report;
  } else {
    acer12_touch_reports_[1] = *report;
  }
  touchscreen_report_->event_time = InputEventTimestampNow();

  size_t index = 0;
  touchscreen_report_->touchscreen->touches.resize(index);

  for (uint8_t i = 0; i < 2; i++) {
    // Only 5 touches per report
    for (uint8_t c = 0; c < 5; c++) {
      auto fid = acer12_touch_reports_[i].fingers[c].finger_id;

      if (!acer12_finger_id_tswitch(fid))
        continue;
      fuchsia::ui::input::Touch touch;
      touch.finger_id = acer12_finger_id_contact(fid);
      touch.x = acer12_touch_reports_[i].fingers[c].x;
      touch.y = acer12_touch_reports_[i].fingers[c].y;
      touch.width = acer12_touch_reports_[i].fingers[c].width;
      touch.height = acer12_touch_reports_[i].fingers[c].height;
      touchscreen_report_->touchscreen->touches.resize(index + 1);
      touchscreen_report_->touchscreen->touches.at(index++) = std::move(touch);
    }
  }
  FXL_VLOG(2) << name() << " parsed: " << *touchscreen_report_;
  return true;
}

bool InputInterpreter::ParseSamsungTouchscreenReport(uint8_t* r, size_t len) {
  if (len != sizeof(samsung_touch_t)) {
    return false;
  }

  const auto& report = *(reinterpret_cast<samsung_touch_t*>(r));
  touchscreen_report_->event_time = InputEventTimestampNow();

  size_t index = 0;
  touchscreen_report_->touchscreen->touches.resize(index);

  for (size_t i = 0; i < arraysize(report.fingers); ++i) {
    auto fid = report.fingers[i].finger_id;

    if (!samsung_finger_id_tswitch(fid))
      continue;

    fuchsia::ui::input::Touch touch;
    touch.finger_id = samsung_finger_id_contact(fid);
    touch.x = report.fingers[i].x;
    touch.y = report.fingers[i].y;
    touch.width = report.fingers[i].width;
    touch.height = report.fingers[i].height;
    touchscreen_report_->touchscreen->touches.resize(index + 1);
    touchscreen_report_->touchscreen->touches.at(index++) = std::move(touch);
  }

  return true;
}

template <typename ReportT>
bool InputInterpreter::ParseParadiseTouchscreenReport(uint8_t* r, size_t len) {
  if (len != sizeof(ReportT)) {
    FXL_LOG(INFO) << "paradise wrong size " << len;
    return false;
  }

  const auto& report = *(reinterpret_cast<ReportT*>(r));
  touchscreen_report_->event_time = InputEventTimestampNow();

  size_t index = 0;
  touchscreen_report_->touchscreen->touches.resize(index);

  for (size_t i = 0; i < arraysize(report.fingers); ++i) {
    if (!paradise_finger_flags_tswitch(report.fingers[i].flags))
      continue;

    fuchsia::ui::input::Touch touch;
    touch.finger_id = report.fingers[i].finger_id;
    touch.x = report.fingers[i].x;
    touch.y = report.fingers[i].y;
    touch.width = 5;  // TODO(cpu): Don't hardcode |width| or |height|.
    touch.height = 5;
    touchscreen_report_->touchscreen->touches.resize(index + 1);
    touchscreen_report_->touchscreen->touches.at(index++) = std::move(touch);
  }

  FXL_VLOG(2) << name() << " parsed: " << *touchscreen_report_;
  return true;
}

bool InputInterpreter::ParseEGalaxTouchscreenReport(uint8_t* r, size_t len) {
  if (len != sizeof(egalax_touch_t)) {
    FXL_LOG(INFO) << "egalax wrong size " << len << " expected "
                  << sizeof(egalax_touch_t);
    return false;
  }

  const auto& report = *(reinterpret_cast<egalax_touch_t*>(r));
  touchscreen_report_->event_time = InputEventTimestampNow();
  if (egalax_pressed_flags(report.button_pad)) {
    fuchsia::ui::input::Touch touch;
    touch.finger_id = 0;
    touch.x = report.x;
    touch.y = report.y;
    touch.width = 5;
    touch.height = 5;
    touchscreen_report_->touchscreen->touches.resize(1);
    touchscreen_report_->touchscreen->touches.at(0) = std::move(touch);
  } else {
    // if the button isn't pressed, send an empty report, this will terminate
    // the finger session
    touchscreen_report_->touchscreen->touches.resize(0);
  }

  FXL_VLOG(2) << name() << " parsed: " << *touchscreen_report_;
  return true;
}

template <typename ReportT>
bool InputInterpreter::ParseParadiseTouchpadReport(uint8_t* r, size_t len) {
  if (len != sizeof(ReportT)) {
    FXL_LOG(INFO) << "paradise wrong size " << len;
    return false;
  }

  mouse_report_->event_time = InputEventTimestampNow();

  const auto& report = *(reinterpret_cast<ReportT*>(r));
  if (!report.fingers[0].tip_switch) {
    mouse_report_->mouse->rel_x = 0;
    mouse_report_->mouse->rel_y = 0;
    mouse_report_->mouse->pressed_buttons = 0;

    mouse_abs_x_ = -1;
    return true;
  }

  // Each axis has a resolution of .00078125cm. 5/32 is a relatively arbitrary
  // coefficient that gives decent sensitivity and a nice resolution of .005cm.
  mouse_report_->mouse->rel_x =
      mouse_abs_x_ != -1 ? 5 * (report.fingers[0].x - mouse_abs_x_) / 32 : 0;
  mouse_report_->mouse->rel_y =
      mouse_abs_x_ != -1 ? 5 * (report.fingers[0].y - mouse_abs_y_) / 32 : 0;
  mouse_report_->mouse->pressed_buttons =
      report.button ? fuchsia::ui::input::kMouseButtonPrimary : 0;

  // Don't update the abs position if there was no relative change, so that
  // we don't drop fractional relative deltas.
  if (mouse_report_->mouse->rel_y || mouse_abs_x_ == -1) {
    mouse_abs_y_ = report.fingers[0].y;
  }
  if (mouse_report_->mouse->rel_x || mouse_abs_x_ == -1) {
    mouse_abs_x_ = report.fingers[0].x;
  }

  return true;
}

// Writes out result to sensor_report_ and sensor_idx_.
bool InputInterpreter::ParseParadiseSensorReport(uint8_t* r, size_t len) {
  if (len != sizeof(paradise_sensor_vector_data_t) &&
      len != sizeof(paradise_sensor_scalar_data_t)) {
    FXL_LOG(INFO) << "paradise sensor data: wrong size " << len << ", expected "
                  << sizeof(paradise_sensor_vector_data_t) << " or "
                  << sizeof(paradise_sensor_scalar_data_t);
    return false;
  }

  sensor_report_->event_time = InputEventTimestampNow();
  sensor_idx_ = r[0];  // We know sensor structs start with sensor ID.
  switch (sensor_idx_) {
    case kParadiseAccLid:
    case kParadiseAccBase: {
      const auto& report =
          *(reinterpret_cast<paradise_sensor_vector_data_t*>(r));
      fidl::Array<int16_t, 3> data;
      data[0] = report.vector[0];
      data[1] = report.vector[1];
      data[2] = report.vector[2];
      sensor_report_->sensor->set_vector(std::move(data));
    } break;
    case 2:
    case 3:
    case 4:
      // TODO(SCN-626): Expose other sensors.
      return false;
    default:
      FXL_LOG(ERROR) << "paradise sensor unrecognized: " << sensor_idx_;
      return false;
  }

  FXL_VLOG(3) << name()
              << " parsed (sensor=" << static_cast<uint16_t>(sensor_idx_)
              << "): " << *sensor_report_;
  return true;
}

bool InputInterpreter::ParseParadiseStylusReport(uint8_t* r, size_t len) {
  if (len != sizeof(paradise_stylus_t)) {
    FXL_LOG(INFO) << "paradise wrong stylus report size " << len;
    return false;
  }

  auto report = reinterpret_cast<paradise_stylus_t*>(r);
  stylus_report_->event_time = InputEventTimestampNow();

  stylus_report_->stylus->x = report->x;
  stylus_report_->stylus->y = report->y;
  stylus_report_->stylus->pressure = report->pressure;

  stylus_report_->stylus->is_in_contact =
      paradise_stylus_status_inrange(report->status) &&
      (paradise_stylus_status_tswitch(report->status) ||
       paradise_stylus_status_eraser(report->status));

  stylus_report_->stylus->in_range =
      paradise_stylus_status_inrange(report->status);

  if (paradise_stylus_status_invert(report->status) ||
      paradise_stylus_status_eraser(report->status)) {
    stylus_report_->stylus->is_inverted = true;
  }

  if (paradise_stylus_status_barrel(report->status)) {
    stylus_report_->stylus->pressed_buttons |=
        fuchsia::ui::input::kStylusBarrel;
  }
  FXL_VLOG(2) << name() << " parsed: " << *stylus_report_;

  return true;
}

// Writes out result to sensor_report_ and sensor_idx_.
bool InputInterpreter::ParseAmbientLightSensorReport() {
  HidDecoder::HidAmbientLightSimple data;
  if (!hid_decoder_->Read(&data)) {
    FXL_LOG(ERROR) << " failed reading from ambient light sensor";
    return false;
  }
  sensor_report_->sensor->set_scalar(data.illuminance);
  sensor_report_->event_time = InputEventTimestampNow();
  sensor_idx_ = kAmbientLight;

  FXL_VLOG(2) << name()
              << " parsed (sensor=" << static_cast<uint16_t>(sensor_idx_)
              << "): " << *sensor_report_;
  return true;
}

bool InputInterpreter::ParseButtonsReport() {
  HidDecoder::HidButtons data;
  if (!hid_decoder_->Read(&data)) {
    FXL_LOG(ERROR) << " failed reading from buttons";
    return false;
  }
  buttons_report_->buttons->set_volume(data.volume);
  buttons_report_->buttons->set_mic_mute(data.mic_mute);
  buttons_report_->event_time = InputEventTimestampNow();

  FXL_VLOG(2) << name() << " parsed buttons: " << *buttons_report_
              << " volume: " << static_cast<int32_t>(data.volume)
              << " mic mute: " << (data.mic_mute ? "yes" : "no");
  return true;
}

bool InputInterpreter::ParseEyoyoTouchscreenReport(uint8_t* r, size_t len) {
  if (len != sizeof(eyoyo_touch_t)) {
    return false;
  }

  const auto& report = *(reinterpret_cast<eyoyo_touch_t*>(r));
  touchscreen_report_->event_time = InputEventTimestampNow();

  size_t index = 0;
  touchscreen_report_->touchscreen->touches.resize(index);

  for (size_t i = 0; i < arraysize(report.fingers); ++i) {
    auto fid = report.fingers[i].finger_id;

    if (!eyoyo_finger_id_tswitch(fid))
      continue;

    fuchsia::ui::input::Touch touch;
    touch.finger_id = eyoyo_finger_id_contact(fid);
    touch.x = report.fingers[i].x;
    touch.y = report.fingers[i].y;
    // Panel does not support touch width/height.
    touch.width = 5;
    touch.height = 5;
    touchscreen_report_->touchscreen->touches.resize(index + 1);
    touchscreen_report_->touchscreen->touches.at(index++) = std::move(touch);
  }

  return true;
}

bool InputInterpreter::ParseFt3x27TouchscreenReport(uint8_t* r, size_t len) {
  if (len != sizeof(ft3x27_touch_t)) {
    return false;
  }

  const auto& report = *(reinterpret_cast<ft3x27_touch_t*>(r));
  touchscreen_report_->event_time = InputEventTimestampNow();

  size_t index = 0;
  touchscreen_report_->touchscreen->touches.resize(index);

  for (size_t i = 0; i < arraysize(report.fingers); ++i) {
    auto fid = report.fingers[i].finger_id;

    if (!ft3x27_finger_id_tswitch(fid))
      continue;

    fuchsia::ui::input::Touch touch;
    touch.finger_id = ft3x27_finger_id_contact(fid);
    touch.x = report.fingers[i].x;
    touch.y = report.fingers[i].y;
    touch.width = 5;
    touch.height = 5;
    touchscreen_report_->touchscreen->touches.resize(index + 1);
    touchscreen_report_->touchscreen->touches.at(index++) = std::move(touch);
    FXL_VLOG(2) << name()
                << " parsed (sensor=" << static_cast<uint16_t>(touch.finger_id)
                << ") x=" << touch.x << ", y=" << touch.y;
  }

  return true;
}

}  // namespace mozart
