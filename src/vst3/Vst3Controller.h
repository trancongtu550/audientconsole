#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include <cstdint>
#include <string>
#include <vector>

namespace audient::vst3
{

// Owns the IEditController of one plug-in instance, off the audio callback.
//
// Mirrors the SDK reference host exactly (public.sdk/source/vst/hosting/
// plugprovider.cpp setupPlugin/terminatePlugin) so both plug-in forms behave:
//
//  - single-component (controller is the SAME object as the component):
//    component->queryInterface(IEditController) succeeds. The host must NOT
//    initialize the controller again (the component was already initialized by
//    Vst3Processor::prepare) and must NOT connect it to itself. close() must
//    NOT terminate the controller; terminate belongs to the component owner
//    (Vst3Processor::teardown).
//
//  - separate controller class: component->getControllerClassId ->
//    factory->createInstance(controllerCid, IEditController) -> initialize with
//    hostContext -> connect component<->controller through IConnectionPoint.
//    close() disconnects, then terminates and releases the controller.
//
// Realtime contract: UI/control-thread only. Never touch this object from the
// ASIO callback.
class Vst3Controller
{
public:
    Vst3Controller() = default;
    ~Vst3Controller();
    Vst3Controller(const Vst3Controller&) = delete;
    Vst3Controller& operator=(const Vst3Controller&) = delete;

    // Resolves the controller for an already-prepared component (the component
    // itself must have been created and initialized by the caller, e.g.
    // Vst3Processor::prepare). factory resolves a separate controller class id;
    // hostContext is the IHostApplication handed to controller initialize.
    bool open(Steinberg::FUnknown* component, Steinberg::IPluginFactory* factory, Steinberg::FUnknown* hostContext);
    void close();

    bool isOpen() const;
    bool isControllerComponent() const; // controller == component object
    Steinberg::Vst::IEditController* controller() const; // borrowed, non-null when open

    // VST-012 controller half: IEditController::getState/setState wrapped in the
    // same bounded, checksummed blob codec as component state.
    bool saveControllerState(std::vector<std::uint8_t>& outBlob);
    bool restoreControllerState(const std::vector<std::uint8_t>& blob);

    const std::string& lastError() const;

private:
    Steinberg::Vst::IComponent* m_componentIf = nullptr;
    Steinberg::Vst::IEditController* m_controller = nullptr;
    Steinberg::Vst::IConnectionPoint* m_componentCP = nullptr;
    Steinberg::Vst::IConnectionPoint* m_controllerCP = nullptr;
    bool m_isControllerComponent = false;
    bool m_connected = false;
    std::string m_lastError;
};

} // namespace audient::vst3