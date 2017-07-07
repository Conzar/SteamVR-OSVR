/** @file
    @brief OSVR tracked controller

    @date 2015

    @author
    Sensics, Inc.
    <http://sensics.com/osvr>
*/

// Copyright 2015 Sensics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Internal Includes
#include "OSVRTrackedController.h"

#include "osvr_compiler_detection.h"
#include "make_unique.h"
#include "matrix_cast.h"
#include "ValveStrCpy.h"
#include "platform_fixes.h" // strcasecmp
#include "Logging.h"

// OpenVR includes
#include <openvr_driver.h>

// Library/third-party includes
#include <osvr/ClientKit/Display.h>
#include <osvr/Util/EigenInterop.h>
#include <osvr/Client/RenderManagerConfig.h>
#include <util/FixedLengthStringFunctions.h>

// Standard includes
#include <cstring>
#include <ctime>
#include <string>
#include <iostream>
#include <exception>



// TODO:
// Trackpad
// OSVRButton(OSVR_BUTTON_TYPE_DIGITAL, FGamepadKeyNames::MotionController_Left_Thumbstick, "/controller/left/joystick/button"),
// OSVRButton(OSVR_BUTTON_TYPE_DIGITAL, FGamepadKeyNames::MotionController_Left_Shoulder, "/controller/left/bumper"),
// OSVRButton(OSVR_BUTTON_TYPE_DIGITAL, FGamepadKeyNames::SpecialLeft, "/controller/left/middle"),

// TODO:
// use vr::VRServerDriverHost() insteead of vr::IServerDriverHost
// Interface is IVRServerDriverHost
OSVRTrackedController::OSVRTrackedController(osvr::clientkit::ClientContext& context, vr::IVRServerDriverHost* driver_host, int controller_index) : 
    //OSVRTrackedDevice(context, driver_host, vr::TrackedDeviceClass_Controller), controllerIndex_(controller_index)
    //OSVRTrackedDevice(context, driver_host, getDeviceClass()), controllerIndex_(controller_index)
    OSVRTrackedDevice(context, getDeviceClass()), controllerIndex_(controller_index)
    //OSVRTrackedDevice(osvr::clientkit::ClientContext& context, vr::ETrackedDeviceClass device_class, const std::string& name = "OSVR device");
{
    this->driver_host = driver_host;
    controllerName_ = "OSVRController" + std::to_string(controller_index);

    numAxis_ = 0;
    for (int iter_axis = 0; iter_axis < NUM_AXIS; iter_axis++) {
        analogInterface_[iter_axis].parentController = this;
        analogInterface_[iter_axis].axisType = vr::EVRControllerAxisType::k_eControllerAxis_None;
    }
}

OSVRTrackedController::~OSVRTrackedController()
{
    // do nothing
}

vr::EVRInitError OSVRTrackedController::Activate(uint32_t object_id)
{
    OSVRTrackedDevice::Activate(object_id);

    const std::time_t wait_time = 5; // wait up to 5 seconds for init

    freeInterfaces();
    numAxis_ = 0;

    // Ensure context is fully started up
    OSVR_LOG(info) << "Waiting for the context to fully start up...";
    const auto start_time = std::time(nullptr);
    while (!context_.checkStatus()) {
        context_.update();
        if (std::time(nullptr) > start_time + wait_time) {
            OSVR_LOG(warn) << "Context startup timed out!";
            return vr::VRInitError_Driver_Failed;
        }
    }

    // Register callbacks
    std::string trackerPath;
    std::string buttonPath;
    std::string triggerPath;
    std::string joystickPath;
    if (controllerIndex_ == 0) {
        trackerPath = "/me/hands/left";
        buttonPath = "/controller/left/";
        triggerPath = "/controller/left/trigger";
        joystickPath = "/controller/left/joystick";
    } else if (controllerIndex_ == 1) {
        trackerPath = "/me/hands/right";
        buttonPath = "/controller/right/";
        triggerPath = "/controller/right/trigger";
        joystickPath = "/controller/right/joystick";
    } else {
        buttonPath = "/controller" + std::to_string(controllerIndex_) + "/";
        triggerPath = "/controller" + std::to_string(controllerIndex_) + "/trigger";
        joystickPath = "/controller" + std::to_string(controllerIndex_) + "/joystick";
    }

    if (!trackerPath.empty()) {
        trackerInterface_ = context_.getInterface(trackerPath);
        trackerInterface_.registerCallback(&OSVRTrackedController::controllerTrackerCallback, this);
    }

    for (int iter_button = 0; iter_button < NUM_BUTTONS; iter_button++) {
        buttonInterface_[iter_button] = context_.getInterface(buttonPath + std::to_string(iter_button));
        if (buttonInterface_[iter_button].notEmpty()) {
            buttonInterface_[iter_button].registerCallback(&OSVRTrackedController::controllerButtonCallback, this);
        } else {
            buttonInterface_[iter_button].free();
        }
    }

    // TODO: ADD TOUCHPAD PART HERE
    numAxis_ = 0;

    numAxis_ = 1;
    for (int iter_trigger = 0; iter_trigger < NUM_TRIGGER; iter_trigger++) {
        if (numAxis_ >= NUM_AXIS)
            break;

        if (iter_trigger == 0) {
            analogInterface_[numAxis_].analogInterfaceX = context_.getInterface(triggerPath);
        } else {
            analogInterface_[numAxis_].analogInterfaceX = context_.getInterface(triggerPath + std::to_string(iter_trigger));
        }

        if (analogInterface_[numAxis_].analogInterfaceX.notEmpty()) {
            analogInterface_[numAxis_].axisIndex = numAxis_;
            analogInterface_[numAxis_].axisType = vr::EVRControllerAxisType::k_eControllerAxis_Trigger;
            analogInterface_[numAxis_].analogInterfaceX.registerCallback(&OSVRTrackedController::controllerTriggerCallback, &analogInterface_[numAxis_]);
            numAxis_++;
        } else {
            analogInterface_[numAxis_].analogInterfaceX.free();
        }
    }

    numAxis_ = 2;
    for (int iter_joystick = 0; iter_joystick < NUM_JOYSTICKS; iter_joystick++) {
        if (numAxis_ >= NUM_AXIS)
            break;

        if (iter_joystick == 0) {
            analogInterface_[numAxis_].analogInterfaceX = context_.getInterface(joystickPath + "/x");
            analogInterface_[numAxis_].analogInterfaceY = context_.getInterface(joystickPath + "/y");
        } else {
            analogInterface_[numAxis_].analogInterfaceX = context_.getInterface(joystickPath + std::to_string(iter_joystick) + "/x");
            analogInterface_[numAxis_].analogInterfaceY = context_.getInterface(joystickPath + std::to_string(iter_joystick) + "/y");
        }

        bool somethingFound = false;

        if (analogInterface_[numAxis_].analogInterfaceX.notEmpty()) {
            analogInterface_[numAxis_].axisIndex = numAxis_;
            analogInterface_[numAxis_].axisType = vr::EVRControllerAxisType::k_eControllerAxis_Joystick;
            analogInterface_[numAxis_].analogInterfaceX.registerCallback(&OSVRTrackedController::controllerJoystickXCallback, &analogInterface_[numAxis_]);
            somethingFound = true;
        } else {
            analogInterface_[numAxis_].analogInterfaceX.free();
        }

        if (analogInterface_[numAxis_].analogInterfaceY.notEmpty()) {
            analogInterface_[numAxis_].axisIndex = numAxis_;
            analogInterface_[numAxis_].axisType = vr::EVRControllerAxisType::k_eControllerAxis_Joystick;
            analogInterface_[numAxis_].analogInterfaceY.registerCallback(&OSVRTrackedController::controllerJoystickYCallback, &analogInterface_[numAxis_]);
            somethingFound = true;
        } else {
            analogInterface_[numAxis_].analogInterfaceY.free();
        }

        if (somethingFound)
            numAxis_++;
    }

    return vr::VRInitError_None;
}

void OSVRTrackedController::Deactivate()
{
    /// Have to force freeing here
    freeInterfaces();
}

vr::VRControllerState_t OSVRTrackedController::GetControllerState()
{
    // TODO
    vr::VRControllerState_t state;
    state.unPacketNum = 0;
    return state;
}

bool OSVRTrackedController::TriggerHapticPulse(uint32_t axis_id, uint16_t pulse_duration_microseconds)
{
    return false;
}

void OSVRTrackedController::freeInterfaces()
{
    if (trackerInterface_.notEmpty()) {
        trackerInterface_.free();
    }

    for (int iter_axis = 0; iter_axis < NUM_AXIS; iter_axis++) {
        if (analogInterface_[iter_axis].analogInterfaceX.notEmpty())
            analogInterface_[iter_axis].analogInterfaceX.free();
        if (analogInterface_[iter_axis].analogInterfaceY.notEmpty())
            analogInterface_[iter_axis].analogInterfaceY.free();
    }

    for (int iter_button = 0; iter_button < NUM_BUTTONS; iter_button++) {
        if (buttonInterface_[iter_button].notEmpty())
            buttonInterface_[iter_button].free();
    }
}

inline vr::HmdQuaternion_t HmdQuaternion_Init(double w, double x, double y, double z)
{
    vr::HmdQuaternion_t quat;
    quat.w = w;
    quat.x = x;
    quat.y = y;
    quat.z = z;
    return quat;
}

void OSVRTrackedController::controllerTrackerCallback(void* userdata, const OSVR_TimeValue* timestamp, const OSVR_PoseReport* report)
{
    if (!userdata)
        return;

    auto* self = static_cast<OSVRTrackedController*>(userdata);

    vr::DriverPose_t pose = { 0 };
    pose.poseTimeOffset = 0; // close enough

    Eigen::Vector3d::Map(pose.vecWorldFromDriverTranslation) = Eigen::Vector3d::Zero();
    Eigen::Vector3d::Map(pose.vecDriverFromHeadTranslation) = Eigen::Vector3d::Zero();

    map(pose.qWorldFromDriverRotation) = Eigen::Quaterniond::Identity();
    map(pose.qDriverFromHeadRotation) = Eigen::Quaterniond::Identity();
    //pose.qWorldFromDriverRotation = HmdQuaternion_Init(1, 0, 0, 0);
    //pose.qDriverFromHeadRotation = HmdQuaternion_Init(1, 0, 0, 0);

    // Position
    Eigen::Vector3d::Map(pose.vecPosition) = osvr::util::vecMap(report->pose.translation);

    // Position velocity and acceleration are not currently consistently provided
    Eigen::Vector3d::Map(pose.vecVelocity) = Eigen::Vector3d::Zero();
    Eigen::Vector3d::Map(pose.vecAcceleration) = Eigen::Vector3d::Zero();

    // Orientation
    map(pose.qRotation) = osvr::util::fromQuat(report->pose.rotation);

    // Angular velocity and acceleration are not currently consistently provided
    Eigen::Vector3d::Map(pose.vecAngularVelocity) = Eigen::Vector3d::Zero();
    Eigen::Vector3d::Map(pose.vecAngularAcceleration) = Eigen::Vector3d::Zero();

    pose.result = vr::TrackingResult_Running_OK;
    pose.poseIsValid = true;
    //pose.willDriftInYaw = true;
    //pose.shouldApplyHeadModel = true;
    pose.deviceIsConnected = true;

    self->pose_ = pose;

    //self->driver_host->TrackedDevicePoseUpdated(self->objectId_, self->pose_);
    self->driver_host->TrackedDevicePoseUpdated(self->objectId_, self->pose_,sizeof(self->pose_));
                     //TrackedDevicePoseUpdated( uint32_t unWhichDevice, const DriverPose_t & newPose, uint32_t unPoseStructSize ) 
    //vr::IVRServerDriverHost::TrackedDevicePoseUpdated(uint32_t&, vr::DriverPose_t&)'
}

void OSVRTrackedController::controllerButtonCallback(void* userdata, const OSVR_TimeValue* timestamp, const OSVR_ButtonReport* report)
{
    if (!userdata)
        return;

    auto* self = static_cast<OSVRTrackedController*>(userdata);

    vr::EVRButtonId button_id;
    if ((report->sensor >= 0 && report->sensor <= 7) || (report->sensor >= 32 && report->sensor <= 36)) {
        button_id = static_cast<vr::EVRButtonId>(report->sensor);
    } else if (report->sensor >= 8 && report->sensor <= 12) {
        button_id = static_cast<vr::EVRButtonId>(report->sensor + 24);
    } else {
        return;
    }

    if (OSVR_BUTTON_PRESSED == report->state) {
        self->driver_host->TrackedDeviceButtonPressed(self->objectId_, button_id, 0);
    } else {
        self->driver_host->TrackedDeviceButtonUnpressed(self->objectId_, button_id, 0);
    }
}

void OSVRTrackedController::controllerTriggerCallback(void* userdata, const OSVR_TimeValue* timestamp, const OSVR_AnalogReport* report)
{
    if (!userdata)
        return;

    auto* analog_interface = static_cast<AnalogInterface*>(userdata);
    OSVRTrackedController* self = analog_interface->parentController;

    analog_interface->x = report->state;

    vr::VRControllerAxis_t axis_state;
    axis_state.x = static_cast<float>(analog_interface->x);

    self->driver_host->TrackedDeviceAxisUpdated(self->objectId_, analog_interface->axisIndex, axis_state);
}

void OSVRTrackedController::controllerJoystickXCallback(void* userdata, const OSVR_TimeValue* timestamp, const OSVR_AnalogReport* report)
{
    if (!userdata)
        return;

    auto* analog_interface = static_cast<AnalogInterface*>(userdata);
    OSVRTrackedController* self = analog_interface->parentController;

    analog_interface->x = report->state;

    vr::VRControllerAxis_t axis_state;
    axis_state.x = static_cast<float>(analog_interface->x);
    axis_state.y = static_cast<float>(analog_interface->y);

    self->driver_host->TrackedDeviceAxisUpdated(self->objectId_, analog_interface->axisIndex, axis_state);
}

void OSVRTrackedController::controllerJoystickYCallback(void* userdata, const OSVR_TimeValue* timestamp, const OSVR_AnalogReport* report)
{
    if (!userdata)
        return;

    auto* analog_interface = static_cast<AnalogInterface*>(userdata);
    OSVRTrackedController* self = analog_interface->parentController;

    analog_interface->y = report->state;

    vr::VRControllerAxis_t axis_state;
    axis_state.x = static_cast<float>(analog_interface->x);
    axis_state.y = static_cast<float>(analog_interface->y);

    self->driver_host->TrackedDeviceAxisUpdated(self->objectId_, analog_interface->axisIndex, axis_state);
}

const char* OSVRTrackedController::GetId()
{
    /// @todo When available, return the actual unique ID of the HMD
    return controllerName_.c_str();
}

void OSVRTrackedController::configure()
{
    configureProperties();
}

void OSVRTrackedController::configureProperties()
{

    propertyContainer_ = vr::VRProperties()->TrackedDeviceToPropertyContainer(this->objectId_);

    //vr::VRProperties()->SetBoolProperty(propertyContainer_, vr::Prop_WillDriftInYaw_Bool, true);
    //vr::VRProperties()->SetBoolProperty(propertyContainer_, vr::Prop_DeviceIsWireless_Bool, false);
	//ETrackedPropertyError SetBoolProperty( PropertyContainerHandle_t ulContainerHandle, ETrackedDeviceProperty prop, bool bNewValue );
	//ETrackedPropertyError SetFloatProperty( PropertyContainerHandle_t ulContainerHandle, ETrackedDeviceProperty prop, float fNewValue );
	//ETrackedPropertyError SetInt32Property( PropertyContainerHandle_t ulContainerHandle, ETrackedDeviceProperty prop, int32_t nNewValue );
	//ETrackedPropertyError SetUint64Property( PropertyContainerHandle_t ulContainerHandle, ETrackedDeviceProperty prop, uint64_t ulNewValue );
	//ETrackedPropertyError SetStringProperty( PropertyContainerHandle_t ulContainerHandle, ETrackedDeviceProperty prop, const char *pchNewValue );

    vr::VRProperties()->SetInt32Property(propertyContainer_,  vr::Prop_DeviceClass_Int32,       static_cast<int32_t>(deviceClass_));
    vr::VRProperties()->SetInt32Property(propertyContainer_,  vr::Prop_Axis0Type_Int32,         static_cast<int32_t>(analogInterface_[0].axisType));
    vr::VRProperties()->SetInt32Property(propertyContainer_,  vr::Prop_Axis1Type_Int32,         static_cast<int32_t>(analogInterface_[1].axisType));
    vr::VRProperties()->SetInt32Property(propertyContainer_,  vr::Prop_Axis2Type_Int32,         static_cast<int32_t>(analogInterface_[2].axisType));
    vr::VRProperties()->SetInt32Property(propertyContainer_,  vr::Prop_Axis3Type_Int32,         static_cast<int32_t>(analogInterface_[3].axisType));
    vr::VRProperties()->SetInt32Property(propertyContainer_,  vr::Prop_Axis4Type_Int32,         static_cast<int32_t>(analogInterface_[4].axisType));
    vr::VRProperties()->SetUint64Property(propertyContainer_, vr::Prop_SupportedButtons_Uint64, static_cast<int32_t>(NUM_BUTTONS));
    vr::VRProperties()->SetStringProperty(propertyContainer_, vr::Prop_ModelNumber_String,      "OSVR Controller");
    vr::VRProperties()->SetStringProperty(propertyContainer_, vr::Prop_SerialNumber_String,     controllerName_.c_str());
    vr::VRProperties()->SetStringProperty(propertyContainer_, vr::Prop_RenderModelName_String,  "");

    //properties_[vr::Prop_DeviceClass_Int32] = static_cast<int32_t>(deviceClass_);
    //properties_[vr::Prop_Axis0Type_Int32] = static_cast<int32_t>(analogInterface_[0].axisType);
    //properties_[vr::Prop_Axis1Type_Int32] = static_cast<int32_t>(analogInterface_[1].axisType);
    //properties_[vr::Prop_Axis2Type_Int32] = static_cast<int32_t>(analogInterface_[2].axisType);
    //properties_[vr::Prop_Axis3Type_Int32] = static_cast<int32_t>(analogInterface_[3].axisType);
    //properties_[vr::Prop_Axis4Type_Int32] = static_cast<int32_t>(analogInterface_[4].axisType);

    //properties_[vr::Prop_SupportedButtons_Uint64] = static_cast<int32_t>(NUM_BUTTONS);

    //properties_[vr::Prop_ModelNumber_String] = "OSVR Controller";
    //properties_[vr::Prop_SerialNumber_String] = controllerName_.c_str();
    //properties_[vr::Prop_RenderModelName_String] = "";

    // Properties that are unique to TrackedDeviceClass_Controller
    //Prop_AttachedDeviceId_String				= 3000,
    //Prop_SupportedButtons_Uint64				= 3001,
    //Prop_Axis0Type_Int32						= 3002, // Return value is of type EVRControllerAxisType
    //Prop_Axis1Type_Int32						= 3003, // Return value is of type EVRControllerAxisType
    //Prop_Axis2Type_Int32						= 3004, // Return value is of type EVRControllerAxisType
    //Prop_Axis3Type_Int32						= 3005, // Return value is of type EVRControllerAxisType
    //Prop_Axis4Type_Int32						= 3006, // Return value is of type EVRControllerAxisType

}

