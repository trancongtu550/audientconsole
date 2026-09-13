#include "vst3/Vst3Editor.h"

#include <windows.h>

#include <cstring>

namespace audient::vst3
{

namespace
{

using Steinberg::IPlugFrame;
using Steinberg::IPlugView;
using Steinberg::ViewRect;

const wchar_t kChildWindowClass[] = L"AudientVst3EditorChild";

} // namespace

// IPlugFrame implementation forwarded to the owning Vst3Editor. Only
// resizeView is part of the interface; the plug-in calls it to ask the host to
// resize the hosted window, and the host must then call IPlugView::onSize.
class Vst3Editor::Vst3EditorFrame : public IPlugFrame
{
public:
    explicit Vst3EditorFrame(Vst3Editor* owner)
        : m_owner(owner)
    {
    }

    Steinberg::tresult queryInterface(const Steinberg::TUID requestId, void** obj) override
    {
        if (obj == nullptr)
        {
            return Steinberg::kInvalidArgument;
        }
        if (m_owner != nullptr && Steinberg::FUnknownPrivate::iidEqual(requestId, IPlugFrame::iid.toTUID()))
        {
            *obj = this;
            addRef();
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 addRef() override
    {
        return 1; // owned by Vst3Editor
    }

    Steinberg::uint32 release() override
    {
        return 1; // owned by Vst3Editor
    }

    Steinberg::tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override
    {
        (void)view;
        if (m_owner == nullptr || newSize == nullptr)
        {
            return Steinberg::kInvalidArgument;
        }
        const int width = newSize->getWidth();
        const int height = newSize->getHeight();
        if (!m_owner->resizeWindow(width, height))
        {
            return Steinberg::kResultFalse;
        }
        return m_owner->notifyResized(width, height);
    }

private:
    Vst3Editor* m_owner = nullptr;
};

LRESULT CALLBACK Vst3Editor::childWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    Vst3Editor* owner = reinterpret_cast<Vst3Editor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_ERASEBKGND && owner != nullptr)
    {
        return 1; // let the plug-in paint its own background
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

Vst3Editor::~Vst3Editor()
{
    close();
}

bool Vst3Editor::isOpen() const
{
    return m_attached && m_view != nullptr;
}

bool Vst3Editor::open(Steinberg::FUnknown* component, Steinberg::IPluginFactory* factory, HWNDPlatform parentWindow,
                      Steinberg::FUnknown* hostContext)
{
    m_lastError.clear();
    close();

    if (component == nullptr || factory == nullptr || parentWindow == nullptr)
    {
        m_lastError = "invalid component/factory/parent window";
        return false;
    }

    m_component = component;
    m_factory = factory;

    // Resolve the IEditController (same-object or separate class) and connect
    // it exactly like the SDK reference host, without double-initializing or
    // double-terminating a single-component plug-in. Vst3Controller also
    // verifies the component implements IComponent.
    if (!m_controller.open(component, factory, hostContext))
    {
        m_lastError = m_controller.lastError();
        close();
        return false;
    }

    // Create the native editor view.
    Steinberg::Vst::IEditController* controller = m_controller.controller();
    m_view = controller->createView(Steinberg::Vst::ViewType::kEditor);
    if (m_view == nullptr)
    {
        m_lastError = "controller does not provide an editor view";
        close();
        return false;
    }
    if (m_view->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) != Steinberg::kResultOk)
    {
        m_lastError = "editor does not support the HWND platform type";
        close();
        return false;
    }

    m_parentWindow = parentWindow;
    const HWND parent = static_cast<HWND>(parentWindow);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Vst3Editor::childWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kChildWindowClass;
    RegisterClassExW(&wc); // idempotent (ERROR_CLASS_ALREADY_EXISTS is fine)

    ViewRect initialSize{};
    if (m_view->getSize(&initialSize) != Steinberg::kResultOk)
    {
        initialSize.left = 0;
        initialSize.top = 0;
        initialSize.right = 800;
        initialSize.bottom = 600;
    }
    const int width = initialSize.getWidth();
    const int height = initialSize.getHeight();
    if (width <= 0 || height <= 0)
    {
        m_lastError = "editor reported an invalid size";
        close();
        return false;
    }

    HWND child = CreateWindowExW(0, kChildWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
                                 width, height, parent, nullptr, GetModuleHandleW(nullptr), this);
    if (child == nullptr)
    {
        m_lastError = "cannot create editor child window";
        close();
        return false;
    }
    m_childWindow = child;

    // IPlugFrame must outlive the view; the editor owns it.
    m_frame = new Vst3EditorFrame(this);
    if (m_view->setFrame(static_cast<IPlugFrame*>(m_frame)) != Steinberg::kResultOk)
    {
        m_lastError = "editor rejected the frame";
        close();
        return false;
    }
    if (m_view->attached(child, Steinberg::kPlatformTypeHWND) != Steinberg::kResultOk)
    {
        m_lastError = "editor view attach to HWND failed";
        close();
        return false;
    }
    ViewRect sizeRect{};
    sizeRect.left = 0;
    sizeRect.top = 0;
    sizeRect.right = width;
    sizeRect.bottom = height;
    (void)m_view->onSize(&sizeRect);

    // Wrap the host window around the editor's reported size so the plug-in is
    // not letterboxed inside the demo's fixed 860x660 host.
    (void)resizeWindow(width, height);

    m_attached = true;
    ShowWindow(child, SW_SHOW);
    return true;
}

void Vst3Editor::close()
{
    if (m_view != nullptr)
    {
        m_view->setFrame(nullptr);
        if (m_childWindow != nullptr)
        {
            m_view->removed();
        }
        m_view->release();
        m_view = nullptr;
    }
    if (m_childWindow != nullptr)
    {
        DestroyWindow(static_cast<HWND>(m_childWindow));
        m_childWindow = nullptr;
    }
    if (m_frame != nullptr)
    {
        delete m_frame;
        m_frame = nullptr;
    }
    m_controller.close();
    m_component = nullptr;
    m_factory = nullptr;
    m_parentWindow = nullptr;
    m_attached = false;
}

bool Vst3Editor::getPreferredSize(int& width, int& height) const
{
    if (m_view == nullptr)
    {
        return false;
    }
    ViewRect size{};
    if (m_view->getSize(&size) != Steinberg::kResultOk)
    {
        return false;
    }
    width = size.getWidth();
    height = size.getHeight();
    return true;
}

bool Vst3Editor::resizeTo(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return false;
    }
    if (!resizeWindow(width, height))
    {
        return false;
    }
    return notifyResized(width, height) == Steinberg::kResultOk;
}

bool Vst3Editor::resizeWindow(int width, int height)
{
    if (m_childWindow == nullptr || m_parentWindow == nullptr)
    {
        return false;
    }
    const HWND child = static_cast<HWND>(m_childWindow);
    const HWND parent = static_cast<HWND>(m_parentWindow);

    // Wrap the surrounding host window exactly around the plug-in's desired
    // client size so the editor is never letterboxed/stretched by a fixed
    // host window (correct scale at 100/125/150/200% DPI).
    RECT frame{0, 0, width, height};
    AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const int windowWidth = frame.right - frame.left;
    const int windowHeight = frame.bottom - frame.top;
    if (SetWindowPos(parent, nullptr, 0, 0, windowWidth, windowHeight,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) == 0)
    {
        return false;
    }
    // The child fills the whole client area (VST editors expect (0,0) origin).
    return SetWindowPos(child, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE) != 0;
}

Steinberg::tresult Vst3Editor::notifyResized(int width, int height)
{
    if (m_view == nullptr)
    {
        return Steinberg::kResultFalse;
    }
    ViewRect size{};
    size.left = 0;
    size.top = 0;
    size.right = width;
    size.bottom = height;
    return m_view->onSize(&size);
}

const std::string& Vst3Editor::lastError() const
{
    return m_lastError;
}

bool Vst3Editor::saveControllerState(std::vector<std::uint8_t>& outBlob)
{
    if (!m_controller.saveControllerState(outBlob))
    {
        m_lastError = m_controller.lastError();
        return false;
    }
    return true;
}

bool Vst3Editor::restoreControllerState(const std::vector<std::uint8_t>& blob)
{
    if (!m_controller.restoreControllerState(blob))
    {
        m_lastError = m_controller.lastError();
        return false;
    }
    return true;
}

} // namespace audient::vst3

