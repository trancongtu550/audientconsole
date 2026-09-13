#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "vst3/Vst3Controller.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"

#include <cstdint>
#include <string>
#include <vector>

namespace audient::vst3
{

// Native editor host for one Vst3Processor (VST-010 demo path).
//
// The controller lifecycle (resolve/create/initialize/connect/terminate) is
// delegated to Vst3Controller so single-component and separate-controller
// plug-ins behave exactly like the SDK reference host. This class only lives
// OUTSIDE the audio callback and handles the Win32 window + IPlugView attach,
// resize and DPI behavior.
class Vst3Editor
{
public:
    using HWNDPlatform = void*; // Win32 HWND

    Vst3Editor() = default;
    ~Vst3Editor();
    Vst3Editor(const Vst3Editor&) = delete;
    Vst3Editor& operator=(const Vst3Editor&) = delete;

    // Resolves the controller via Vst3Controller, then creates the editor view
    // from it and attaches the view to a Win32 child window. Returns false with
    // a reason on any step. The parent window must exist for the lifetime of
    // the editor.
    bool open(Steinberg::FUnknown* component, Steinberg::IPluginFactory* factory, HWNDPlatform parentWindow,
              Steinberg::FUnknown* hostContext);

    bool isOpen() const;
    void close(); // detaches the view, closes the controller, releases refs

    // Size handling: getPreferredSize returns the plug-in's requested size
    // (points). resizeTo applies it to the view and the hosted window.
    bool getPreferredSize(int& width, int& height) const;
    bool resizeTo(int width, int height);

    // VST-010/012: controller-state persistence through the editor's controller
    // path (IEditController::getState/setState wrapped in the bounded blob).
    // Requires an open editor. UI-thread only.
    bool saveControllerState(std::vector<std::uint8_t>& outBlob);
    bool restoreControllerState(const std::vector<std::uint8_t>& blob);

    const std::string& lastError() const;

private:
    class Vst3EditorFrame;
    friend class Vst3EditorFrame;

    static LRESULT CALLBACK childWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    // Resize the hosted Win32 window (used by IPlugFrame::resizeView).
    bool resizeWindow(int width, int height);
    Steinberg::tresult notifyResized(int width, int height);

    Vst3Controller m_controller;
    Vst3EditorFrame* m_frame = nullptr;
    Steinberg::IPlugView* m_view = nullptr;
    void* m_parentWindow = nullptr;
    void* m_childWindow = nullptr; // owned child hosting the plug-in view
    Steinberg::FUnknown* m_component = nullptr; // borrowed; owned by Vst3Processor
    Steinberg::IPluginFactory* m_factory = nullptr; // borrowed from the loader
    std::string m_lastError;
    bool m_attached = false;
};

} // namespace audient::vst3